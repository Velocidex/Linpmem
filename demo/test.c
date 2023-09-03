/* SPDX-FileCopyrightText: © 2023 Viviane Zwanger
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <linux/types.h>

#include "../userspace_interface/linpmem_shared.h"


// ### Explanation:
//
// This contains demo usage for all ioctls of Linpmem:
// * reading CR3
// * reading from a physical address
//      * qword read
//      * buffer read
// * using the VTOP translation service
//
// All tests are void functions and already inserted in main().
// Recommended: only try one at a time.

// Compiling: gcc -o test test.c
// Usage:
// sudo ./test


// ### Read CR3.
void do_CR3_test(int dev)
{
    LINPMEM_CR3_INFO cr3info = {0};

    cr3info.target_process = 0; // <= pid of process, must be living. 
    cr3info.result_cr3 = 0;

    ioctl(dev, IOCTL_LINPMEM_QUERY_CR3, &cr3info);

    printf("CR3 is: %llx\n", cr3info.result_cr3);

}

// ### Read from a physical address.
// If you, by chance, use qemu/kvm, you can resort to use this 
// hardcoded DSDT address as handy physical test address.
#define QEMU_HARDCODED_DSDT  (0x7FFE0040)

// qword (8 byte) physical read
void do_physread_test_qwordread(int dev)
{
    LINPMEM_DATA_TRANSFER dataTransfer = {0};

    dataTransfer.phys_address = QEMU_HARDCODED_DSDT;  // <= specify what you want here.
    dataTransfer.access_type = PHYS_QWORD_READ;

    // Testing here with the 8 byte read on the DSDT address of QEMU/KVM.
    // (Which gives us "DSDT" + size of DSDT, which actually makes sense).
    // On bare metal, you can also use the VTOP translation service of Linpmem to
    // get a physical address from a virtual address.
    // 
    // Example: a hello world string in your program.
    // Then use the returned physical address to test the physical read...!
    // Note: this is already done in test do_vtop_query_with_proof_read(dev).

    ioctl(dev, IOCTL_LINPMEM_READ_PHYSADDR, &dataTransfer);

    if (dataTransfer.out_value)
    {
        printf("Got: '%llx'\n", dataTransfer.out_value);
    }
    else
    {
        printf("The 8 byte read failed!\n");
    }

}

// Buffer read from physical address.
void do_physread_test_bufferread(int dev)
{
    unsigned char * charptr = NULL;
    LINPMEM_DATA_TRANSFER dataTransfer = {0};
    uint64_t i = 0;

    dataTransfer.phys_address = QEMU_HARDCODED_DSDT;
    dataTransfer.access_type = PHYS_BUFFER_READ;
    dataTransfer.readbuffer = malloc(0x200);
    if (!dataTransfer.readbuffer)
    {
        printf("Malloc didn't not allocate buffer.\n"); // I seriously doubt this ever happens.
        return;
    }
    dataTransfer.readbuffer_size = 0x200;

    // try read 0x200 bytes from DSDT.

    ioctl(dev, IOCTL_LINPMEM_READ_PHYSADDR, &dataTransfer);

    if (dataTransfer.readbuffer_size) // returns either 0x200 or 0.
    {
        printf("Read 0x%llx bytes.\n", dataTransfer.readbuffer_size);
        charptr = (unsigned char *) dataTransfer.readbuffer;

        for (i=0;i<dataTransfer.readbuffer_size;i++)
        {
            printf("%02x ", charptr[i]);
        }
        printf("\n");
    }
    else
    {
        printf("The buffer read has failed!\n");
    }

    if (dataTransfer.readbuffer) free(dataTransfer.readbuffer);

}

void do_vtop_query(int dev)
{
    unsigned char * hello = "Hello World!\n";
    LINPMEM_VTOP_INFO vtop_info = {0};

    vtop_info.virt_address = (uint64_t) hello;

    ioctl(dev, IOCTL_LINPMEM_VTOP_TRANSLATION_SERVICE, &vtop_info);

    if (vtop_info.phys_address)
    {
        printf("My hello buffer is at physical address %llx. PTE address: %llx.\n",
                vtop_info.phys_address,
                vtop_info.ppte);
    }

}

void do_vtop_query_with_proof_read(int dev)
{
    unsigned char * hello = "Hello World!\n";
    LINPMEM_VTOP_INFO vtop_info = {0};
    LINPMEM_DATA_TRANSFER dataTransfer = {0};
    unsigned char * charptr = NULL;
    uint64_t i = 0;

    vtop_info.virt_address = (uint64_t) hello;

    ioctl(dev, IOCTL_LINPMEM_VTOP_TRANSLATION_SERVICE, &vtop_info);

    if (!vtop_info.phys_address)
    {
        printf("vtop failed.\n");
        return;
    }

    // [+] got the physical address.

    dataTransfer.phys_address = vtop_info.phys_address;
    dataTransfer.access_type = PHYS_BUFFER_READ;
    dataTransfer.readbuffer = malloc(0x100);
    if (!dataTransfer.readbuffer)
    {
        printf("Malloc didn't not allocate buffer.\n"); // I seriously doubt this ever happens.
        return;
    }
    dataTransfer.readbuffer_size = 0x100;
    // "Buuuut 'Hello World!\n' is much shorter than 0x100?!"
    // Remember: physical read.
    // It does not matter if we read beyond the hello string.
    // The only thing that stops is a page boundary.

    ioctl(dev, IOCTL_LINPMEM_READ_PHYSADDR, &dataTransfer);

    if (dataTransfer.readbuffer_size) // returns either 0x100 or 0.
    {
        printf("Read 0x%llx bytes.\n", dataTransfer.readbuffer_size);
        charptr = (unsigned char *) dataTransfer.readbuffer;

        for (i=0;i<dataTransfer.readbuffer_size;i++)
        {
            printf("%02x ", charptr[i]);
        }
        printf("\n");
    }
    else
    {
        printf("The buffer read has failed!\n");
    }

    if (dataTransfer.readbuffer) free(dataTransfer.readbuffer);

}


int main()
{
    int dev;

    dev = open("/dev/linpmem", O_WRONLY);

    if (dev == -1)
    {
        printf("Opening '/dev/linpmem' was not possible!\n");
        return -1;
    }

    // Tests that prove/show functionality. 

    do_CR3_test(dev); // try this one first. The least dangerous of all.

    // WARNING: 'do_physread_xx' reads from the hardcoded qemu/kvm DSDT physical address.
    // Change this to a fitting physical address FIRST (if not running on qemu/kvm).

    // do_physread_test_qwordread(dev);

    // do_physread_test_bufferread(dev);

    do_vtop_query(dev); // Returns physical address of hello world string buffer.

    do_vtop_query_with_proof_read(dev); // physical read from the vtop-returned hello world string buffer.

    close(dev);

    return 0;
}
