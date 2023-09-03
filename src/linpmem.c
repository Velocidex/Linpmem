/* SPDX-FileCopyrightText: © 2023 Viviane Zwanger, Valentin Obst <legal@eb9f.de>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "precompiler.h"
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/moduleparam.h>
#include <linux/pid.h>
#include <linux/pfn.h>
#include <linux/gfp.h>
#include <linux/cdev.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/align.h>
#include <linux/string.h>
#include <asm/io.h>

#include "pte_mmap.h"
#include "page_table.h"
#include "linpmem.h"

unsigned int major = 42;

DEVICE_EXTENSION g_device_extension = { 0 };

static int pmem_open(struct inode *device_file, struct file *instance)
{
    pr_debug("open\n");

    return 0;
}

static int pmem_close(struct inode *device_file, struct file *instance)
{
    pr_debug("close\n");

    return 0;
}

/* pte_mmap_read - read up to count bytes from `phys_addr` using rogue PTE
 * @pte_data: management data
 * @phys_addr: physical address to read from
 * @buf: the buffer to read data into (user-space pointer in buffer read mode)
 * @count: requested amount of bytes to read, size of buf
 * @access_mode: how to access the memory
 *
 * note: reads can not cross page boundaries
 * note: non-buffer-mode accesses must be properly aligned
 *
 * Returns number of bytes read into `buf`
 */
static uint64_t pte_mmap_read(PPTE_METHOD_DATA pte_data, uint64_t phys_addr,
                              void *buf, uint64_t count,
                              PHYS_ACCESS_MODE access_mode)
{
    PTE_STATUS pte_status;
    uint64_t page_offset;
    uint64_t to_read;
    uint64_t pfn;
    PTE new_pte;
    uint64_t bytes_read = 0;

    if (!pte_data) {
        pr_err("BUG: pte_data == NULL");
        return 0;
    }
    new_pte = pte_data->original_pte;

    page_offset = offset_in_page(phys_addr);
    to_read = min(PAGE_SIZE - page_offset, count);

    pfn = __phys_to_pfn(phys_addr);
    if (!pfn_valid(pfn)) {
        pr_notice_ratelimited("invalid pfn");
        return 0;
    }

    new_pte.page_frame = pfn;
    pte_status = pte_remap_rogue_page_locked(pte_data, new_pte);
    if (pte_status != PTE_SUCCESS) {
        return 0;
    }

    switch (access_mode) {
    case PHYS_BYTE_READ:
        *((uint8_t *)buf) = ((uint8_t *)(((uint64_t)pte_data->rogue_va.value) +
                                         page_offset))[0];
        break;
    case PHYS_WORD_READ:
        if (!IS_ALIGNED(page_offset, __alignof__(uint16_t)))
            goto out_unlock;
        *((uint16_t *)buf) = ((
            uint16_t *)(((uint64_t)pte_data->rogue_va.value) + page_offset))[0];
        break;
    case PHYS_DWORD_READ:
        if (!IS_ALIGNED(page_offset, __alignof__(uint32_t)))
            goto out_unlock;
        *((uint32_t *)buf) = ((
            uint32_t *)(((uint64_t)pte_data->rogue_va.value) + page_offset))[0];

        break;
    case PHYS_QWORD_READ:
        if (!IS_ALIGNED(page_offset, __alignof__(uint64_t)))
            goto out_unlock;
        *((uint64_t *)buf) = ((
            uint64_t *)(((uint64_t)pte_data->rogue_va.value) + page_offset))[0];

        break;
    case PHYS_BUFFER_READ:
        pr_debug(
            "%s: copying %llu bytes from rogue page to user address %llx\n",
            __func__, to_read, (uint64_t)buf);
        // we don't want any size checks inserted here, just in case
        if (_copy_to_user(
                buf,
                (void *)(((uint64_t)pte_data->rogue_va.value) + page_offset),
                to_read)) {
            pr_notice_ratelimited("%s: copying rogue page to user failed\n",
                                  __func__);
            goto out_unlock;
        }
        break;
    }

    bytes_read = to_read;

out_unlock:
    mutex_unlock(&g_rogue_page_mutex);

    return bytes_read;
}

/* r_cr3_pa_pid - get the physical address of the top-level page tables of task
 * @upid: user space pid
 *
 * Currently, Linpmem will NOT try to probe and lock the process in question.
 * From security perspective, the process might exit at any time. 
 * Make sure that the process still exists while asking for its CR3! 
 *
 * Returns physical address of pgd
 */
static CR3 r_cr3_pa_pid(pid_t upid)
{
    CR3 cr3_pa;
    struct pid *pid;
    struct task_struct *task;
    struct mm_struct *mm;

    cr3_pa.value = 0;

    pid = find_get_pid(upid);
    if (!pid)
        goto out;

    task = get_pid_task(pid, PIDTYPE_PID);
    if (!task)
        goto out_pid;

    mm = get_task_mm(task);
    if (!mm)
        goto out_task;

    cr3_pa.value = (uint64_t)virt_to_phys((void *)mm->pgd);

    pr_debug("Task with upid %d has pdg@0x%llx\n", upid, cr3_pa.value);
    if (!is_kernel_pgtable(cr3_pa.value)) {
        pr_notice("PGD stored in mm is not kernel\n");
        cr3_pa.value &= ~PTI_USER_PGTABLE_MASK;
    }

    mmput(mm);
out_task:
    put_task_struct(task);
out_pid:
    put_pid(pid);
out:
    return cr3_pa;
}

static long do_ioctl_query_cr3(PLINPMEM_CR3_INFO __user userbuffer)
{
    long ret = 0;
    LINPMEM_CR3_INFO cr3_info;
    CR3 cr3_pa;

    if (copy_from_user(&cr3_info, userbuffer, sizeof(LINPMEM_CR3_INFO))) {
        pr_notice_ratelimited("IOCTL: copying LINPMEM_CR3_INFO from user!\n");
        ret = -EFAULT;
        goto out;
    }

    if (cr3_info.target_process) {
        cr3_pa = r_cr3_pa_pid((pid_t)cr3_info.target_process);
        if (!cr3_pa.value) {
            ret = -ESRCH;
            goto out;
        }
    } else {
        cr3_pa = r_cr3_pa();
    }

    // CR3 can return as zero in very rare and anomalous circumstances.
    if (!pfn_valid(__phys_to_pfn(cr3_pa.value))) {
        pr_err(
            "User requested cr3 read is invalid! This should NOT happen. You can't use Linpmem for physical reading on this OS.\n");
        ret = -EIO;
        goto out;
    }

    cr3_info.result_cr3 = cr3_pa.value;

    if (copy_to_user(userbuffer, &cr3_info, sizeof(LINPMEM_CR3_INFO))) {
        pr_notice_ratelimited("IOCTL: copying LINPMEM_CR3_INFO to user!\n");
        ret = -EFAULT;
        goto out;
    }

out:
    return ret;
}

static long do_ioctl_vtop(PLINPMEM_VTOP_INFO __user userbuffer)
{
    LINPMEM_VTOP_INFO vtop_info;
    VIRT_ADDR in_va = { 0 };
    uint64_t page_offset = 0;
    volatile PPTE ppte;
    PTE_STATUS pte_status;
    long ret = 0;

    if (copy_from_user(&vtop_info, userbuffer, sizeof(LINPMEM_VTOP_INFO))) {
        pr_notice_ratelimited("%s: copy-in LINPMEM_VTOP_INFO from user!\n",
                              __func__);
        ret = -EFAULT;
        goto out;
    }

    pr_debug("%s: translation wanted for: VA %llx, associated CR3: %llx.\n",
             __func__, vtop_info.virt_address, vtop_info.associated_cr3);

    if (!vtop_info.virt_address) {
        pr_notice_ratelimited("%s: no virtual address specified for vtop.\n",
                              __func__);
        ret = -EINVAL;
        goto out;
    }

    in_va.value = vtop_info.virt_address;
    page_offset = in_va.offset;
    in_va.value -= page_offset;

    pte_status = virt_find_pte(in_va, &ppte, vtop_info.associated_cr3);
    if (pte_status != PTE_SUCCESS) {
        pr_info_ratelimited(
            "%s: No translation possible: no present page for %llx. Sorry.\n",
            __func__, in_va.value);
        vtop_info.phys_address = 0;
        vtop_info.ppte = NULL;
        ret = -EIO;
        goto out_usercopy;
    }

    if (ppte->present) {
        if (!ppte->large_page) {
            // Normal calculation. 4096 page size.
            vtop_info.phys_address = (PFN_PHYS(ppte->page_frame)) + page_offset;
        } else {
            // Large page calculation. 2MiB size.
            vtop_info.phys_address =
                (PFN_PHYS(ppte->page_frame + in_va.pt_index)) + page_offset;
        }
        vtop_info.ppte = (void *)ppte;
    } else {
        pr_info_ratelimited(
            "%s: No translation possible: Present bit not set in PTE.\n",
            __func__);
        vtop_info.phys_address = 0;
        vtop_info.ppte = NULL;
        goto out_usercopy;
    }

    pr_debug(
        "%s: vtop translation success. Physical address: %llx. PTE address: %llx\n",
        __func__, (long long unsigned int)vtop_info.phys_address,
        (long long unsigned int)vtop_info.ppte);

    dprint_pte_contents(ppte);

out_usercopy:
    if (copy_to_user((PLINPMEM_VTOP_INFO)userbuffer, &vtop_info,
                     sizeof(LINPMEM_VTOP_INFO))) {
        pr_notice_ratelimited("%s: copying LINPMEM_VTOP_INFO back to user!\n",
                              __func__);
        ret = -EFAULT;
        goto out;
    }

out:
    return ret;
}

static long do_ioctl_read(PLINPMEM_DATA_TRANSFER __user userbuffer)
{
    LINPMEM_DATA_TRANSFER data_transfer;
    uint64_t tmp = 0;
    void *buf = &tmp;
    PHYS_ACCESS_MODE access_mode = 0;
    uint64_t count;
    long ret = 0;
    uint64_t bytes_read = 0;

    if (copy_from_user(&data_transfer, userbuffer,
                       sizeof(LINPMEM_DATA_TRANSFER))) {
        pr_notice_ratelimited("%s: copying LINPMEM_DATA_TRANSFER from user!\n",
                              __func__);
        ret = -EFAULT;
        goto out;
    }

    switch (data_transfer.access_type) {
    case PHYS_BYTE_READ:
        count = 1;
        access_mode = PHYS_BYTE_READ;
        break;
    case PHYS_WORD_READ:
        count = 2;
        access_mode = PHYS_WORD_READ;
        break;
    case PHYS_DWORD_READ:
        count = 4;
        access_mode = PHYS_DWORD_READ;
        break;
    case PHYS_QWORD_READ:
        count = 8;
        access_mode = PHYS_QWORD_READ;
        break;
    case PHYS_BUFFER_READ:
        count = data_transfer.readbuffer_size;

        if (count == 0 || count > PAGE_SIZE) {
            pr_notice_ratelimited(
                "%s: BUFFER_READ: invalid read size specified\n", __func__);
            ret = -EINVAL;
            goto out;
        }

        if (!data_transfer.readbuffer) {
            pr_notice_ratelimited(
                "%s: BUFFER_READ: provided usermode buffer is null\n",
                __func__);
            ret = -EINVAL;
            goto out;
        }

        access_mode = PHYS_BUFFER_READ;

        buf = data_transfer.readbuffer;

        break;
    default:
        pr_notice_ratelimited("%s: unknown access type %08x set!\n", __func__,
                              data_transfer.access_type);
        ret = -EINVAL;
        goto out;
    } // end of switch (data_transfer.access_type)

    pr_debug("%s: Reading up to %llu bytes from %llx.\n", __func__, count,
             (long long unsigned int)data_transfer.phys_address);

    bytes_read = pte_mmap_read(&g_device_extension.pte_data,
                               data_transfer.phys_address, buf, count,
                               access_mode);

    pr_debug("%s: Read %llu bytes from %llx.\n", __func__, bytes_read,
             (long long unsigned int)data_transfer.phys_address);

    data_transfer.out_value = bytes_read == count ? tmp : 0;

    if (access_mode != PHYS_BUFFER_READ && bytes_read != count) {
        ret = -EIO;
    }

    if (access_mode == PHYS_BUFFER_READ) {
        if (bytes_read <= count) {
            data_transfer.readbuffer_size = bytes_read;
        } else {
            data_transfer.readbuffer_size = 0;
            ret = -EIO;
        }
    }

    if (copy_to_user(userbuffer, &data_transfer,
                     sizeof(LINPMEM_DATA_TRANSFER))) {
        pr_notice_ratelimited(
            "%s: copying LINPMEM_DATA_TRANSFER back to user!\n", __func__);
        ret = -EFAULT;
        goto out;
    }

out:
    return ret;
}

static long int pmem_ioctl(struct file *file, unsigned int ioctl,
                           unsigned long userbuffer)
{
    long ret = 0;

    switch (ioctl) {
    case IOCTL_LINPMEM_READ_PHYSADDR:
        ret = do_ioctl_read((PLINPMEM_DATA_TRANSFER)userbuffer);
        break;
    case IOCTL_LINPMEM_VTOP_TRANSLATION_SERVICE:
        ret = do_ioctl_vtop((PLINPMEM_VTOP_INFO)userbuffer);
        break;
    case IOCTL_LINPMEM_QUERY_CR3:
        ret = do_ioctl_query_cr3((PLINPMEM_CR3_INFO)userbuffer);
        break;
    default:
        pr_err_ratelimited("%s: unknown IOCTL %08x\n", __func__, ioctl);
        ret = -ENOSYS;
    }

    return ret;
}

const static struct file_operations pmem_fops = { .owner = THIS_MODULE,
                                                  .open = pmem_open,
                                                  .release = pmem_close,
                                                  .unlocked_ioctl =
                                                      pmem_ioctl };

/* init_check_compatibility - checks necessary conditions for driver loading
 *
 * This function function checks that some necessary conditions for loading
 * the driver are satisfied. Decides whether to bail out, to adapt strategies,
 * or that some features will not be available.
 *
 * Returns 0 if it is ok to continue loading the driver, or -1 if we must bail
 * out.
 */
static int init_check_compatibility(void)
{
    int ret = 0;

    if (boot_cpu_has(X86_FEATURE_SEV)) {
        pr_debug("SEV: active. BAIL OUT\n");
        ret = -1;
        goto out;
    } else {
        pr_debug("SEV: not active. OK\n");
    }

    if (boot_cpu_has(X86_FEATURE_SME)) {
        pr_debug("SME: active. BAIL OUT\n");
        ret = -1;
        goto out;
    } else {
        pr_debug("SME: not active. OK\n");
    }

    // pgtable_l5_enabled() =- cpu_feature_enabled(X86_FEATURE_LA57)
    if (cpu_feature_enabled(X86_FEATURE_LA57)) {
        pr_debug("5-level paging: active. BAIL OUT\n");
        ret = -1;
        goto out;
    } else {
        pr_debug("5-level paging: not active. OK\n");
    }

out:
    return ret;
}

static int __init pmem_init(void)
{
    int ret = 0;

    pr_info("init start\n");

    ret = init_check_compatibility();
    if (ret) {
        pr_err("check_compatibility->%d\n", ret);
        return ret;
    }

    ret = register_chrdev(major, KBUILD_MODNAME, &pmem_fops);
    if (ret) {
        pr_err("register_chrdev->%d\n", ret);
        return ret;
    } else {
        pr_info("registered chrdev with major %d\n", major);
    }

    ret = setup_pte_method(&g_device_extension.pte_data);
    if (ret) {
        pr_emerg("rogue page setup failed terribly - pls reboot\n");
        goto out_chrdev;
    }

    pr_info("startup successfull\n");

    return 0;

out_chrdev:
    unregister_chrdev(major, KBUILD_MODNAME);

    return ret;
}

static void __exit pmem_exit(void)
{
    // Undo the sacrifice.
    if (g_device_extension.pte_data.pte_method_is_ready_to_use) {
        restore_pte_method(&g_device_extension.pte_data);
    }

    // Everything should be as it should be, except if we lost control.
    // If there is a programming error, or a tiny little thing which we did not guard against,
    // this might lead to a full loss of control over the rogue page.
    // We won't know until we go looking.
    // So, peek first character carefully. Expected: 'S'.
    if (g_rogue_page[0] != 'S') {
        pr_emerg("The rogue page is out of control. Reboot. now.\n");
        goto out;
    }

    // Turns out fine.
    pr_debug("Identifier string on sacrifice page: %s, %llx\n", g_rogue_page,
             (long long unsigned int)&g_rogue_page);

    pr_info("Goodbye, Kernel\n");

out:
    unregister_chrdev(major, KBUILD_MODNAME);
}

module_init(pmem_init);
module_exit(pmem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Viviane Zwanger/Valentin Obst");
MODULE_DESCRIPTION("Updated Pmem driver for Linux");
MODULE_INFO(version, LINPMEM_DRIVER_VERSION);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdate-time"
MODULE_INFO(compilation_timestamp, __TIMESTAMP__);
#pragma GCC diagnostic pop

module_param(major, uint, 00);
MODULE_PARM_DESC(major,
                 "The major number that the driver will use (default is 42)");
