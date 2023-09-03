/* SPDX-FileCopyrightText: © 2023 Viviane Zwanger, Valentin Obst <legal@eb9f.de>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __PAGE_TABLE_H__
#define __PAGE_TABLE_H__

#include <asm/processor.h>
#include <asm/cpufeatures.h>
#include <asm/cpufeature.h>

#include "pte_mmap.h"

// defined in entry/calling.h which cannot be included
#define PTI_USER_PGTABLE_BIT PAGE_SHIFT
#define PTI_USER_PGTABLE_MASK (1UL << PTI_USER_PGTABLE_BIT)
#define PTI_USER_PCID_BIT X86_CR3_PTI_PCID_USER_BIT
#define PTI_USER_PCID_MASK (1UL << PTI_USER_PCID_BIT)
#define PTI_USER_PGTABLE_AND_PCID_MASK \
    (PTI_USER_PCID_MASK | PTI_USER_PGTABLE_MASK)

/* is_kernel_pgtable - checks if a phys. addr. is suitable for kernel page table
 * @cr3_pa: address to check
 *
 * Returns true iff the address can store a kernel page table
 */
static inline bool is_kernel_pgtable(uint64_t cr3_pa)
{
    if (!boot_cpu_has(X86_FEATURE_PTI))
        return true;
    return (cr3_pa & PTI_USER_PGTABLE_MASK) == 0;
}

/* r_cr3_pa - get phys. address of the current task's (kernel) root page table
 *
 * In theory, the top-level page table should not change while the task is alive.
 * However, the process might exit at any time and this routine 
 * currently does neither probe nor lock the process. It's currently 
 * up to the user to provide sane commands.
 *
 * note: all the pr_debug are not compiled into release builds, which makes it
 *   OK to define it as inline
 *
 * Returns address of current pgd
 */
static inline CR3 r_cr3_pa(void)
{
    CR3 cr3;

    cr3.value = __native_read_cr3();

    pr_debug("Kernel CR3: %llx\n", cr3.value);
    pr_debug("Kernel CR3 (pa): %llx\n", cr3.value & CR3_ADDR_MASK);

    if (cpu_feature_enabled(X86_FEATURE_PCID))
        pr_debug("Kernel PCID: %lx\n", (unsigned long)cr3.pcid);

    if (boot_cpu_has(X86_FEATURE_PTI)) {
        pr_debug("User CR3 (pa): %llx\n",
                 (cr3.value & CR3_ADDR_MASK) | PTI_USER_PGTABLE_MASK);
        if (cpu_feature_enabled(X86_FEATURE_PCID))
            pr_debug("User PCID: %lx\n", cr3.pcid | PTI_USER_PCID_MASK);
    }

    cr3.value &= CR3_ADDR_MASK;

    return cr3;
}

#endif // __PAGE_TABLE_H__
