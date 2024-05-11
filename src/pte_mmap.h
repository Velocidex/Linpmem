// Copyright 2018 Velocidex Innovations <mike@velocidex.com>
// Copyright 2014 - 2017 Google Inc.
// Copyright 2012 Google Inc. All Rights Reserved.
// Author: Viviane Zwanger, Valentin Obst
// derived from Rekall/WinPmem by Mike Cohen and Johannes Stüttgen.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _PTE_MMAP_H_
#define _PTE_MMAP_H_

#include <linux/types.h>
#include <asm/tlbflush.h>
#include <asm/special_insns.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE (4096)
#endif

// not used / not needed.
#ifndef LARGE_PAGE_SIZE
#define LARGE_PAGE_SIZE (2097152)
#endif

#ifndef PAGE_MASK
#define PAGE_MASK (~(PAGE_SIZE - 1))
#endif

/* Protects PTE of rogue page. Only modify the value after acquiring this
 * mutex. Only read from the rogue page while holding this mutex.
 */
extern struct mutex g_rogue_page_mutex;
extern char g_rogue_page[];

#pragma pack(push, 1)
typedef union {
    uint64_t value;
    void *pointer;
    struct {
        uint64_t offset : 12;
        uint64_t pt_index : 9;
        uint64_t pd_index : 9;
        uint64_t pdpt_index : 9;
        uint64_t pml4_index : 9;
        uint64_t reserved : 16;
    };
} VIRT_ADDR, *PVIRT_ADDR;

typedef union {
    uint64_t value;
    struct {
        // CR4.PCIDE is set
        struct {
            uint64_t pcid : 12;
        };
        // non-PAE, non-PCID
        struct {
            uint64_t ignored_1 : 3;
            uint64_t write_through : 1;
            uint64_t cache_disable : 1;
            uint64_t ignored_2 : 7;
        };
        uint64_t pml4_p : 40;
        uint64_t reserved : 12;
    };
} CR3, *PCR3;

typedef union {
    uint64_t value;
    struct {
        uint64_t present : 1;
        uint64_t rw : 1;
        uint64_t user : 1;
        uint64_t write_through : 1;
        uint64_t cache_disable : 1;
        uint64_t accessed : 1;
        uint64_t ignored_1 : 1;
        uint64_t reserved_1 : 1;
        uint64_t ignored_2 : 4;
        uint64_t pdpt_p : 40;
        uint64_t ignored_3 : 11;
        uint64_t xd : 1;
    };
} PML4E, *PPML4E;

typedef union {
    uint64_t value;
    struct {
        uint64_t present : 1;
        uint64_t rw : 1;
        uint64_t user : 1;
        uint64_t write_through : 1;
        uint64_t cache_disable : 1;
        uint64_t accessed : 1;
        uint64_t dirty : 1;
        uint64_t large_page : 1;
        uint64_t ignored_2 : 4;
        uint64_t pd_p : 40;
        uint64_t ignored_3 : 11;
        uint64_t xd : 1;
    };
} PDPTE, *PPDPTE;

typedef union {
    uint64_t value;
    struct {
        uint64_t present : 1;
        uint64_t rw : 1;
        uint64_t user : 1;
        uint64_t write_through : 1;
        uint64_t cache_disable : 1;
        uint64_t accessed : 1;
        uint64_t dirty : 1;
        uint64_t large_page : 1;
        uint64_t ignored_2 : 4;
        uint64_t pt_p : 40;
        uint64_t ignored_3 : 11;
        uint64_t xd : 1;
    };
} PDE, *PPDE;

typedef union {
    uint64_t value;
    struct {
        uint64_t present : 1;
        uint64_t rw : 1;
        uint64_t user : 1;
        uint64_t write_through : 1;
        uint64_t cache_disable : 1;
        uint64_t accessed : 1;
        uint64_t dirty : 1;
        uint64_t large_page : 1; // PAT/PS
        uint64_t global : 1;
        uint64_t ignored_1 : 3;
        uint64_t page_frame : 40;
        uint64_t ignored_3 : 11;
        uint64_t xd : 1;
    };
} PTE, *PPTE;
#pragma pack(pop)

/* Operating system independent error checking. */
typedef enum {
    PTE_SUCCESS = 0,
    PTE_ERROR,
    PTE_ERROR_HUGE_PAGE,
    PTE_ERROR_RO_PTE
} PTE_STATUS;

typedef struct {
    bool pte_method_is_ready_to_use;
    VIRT_ADDR rogue_va;
    volatile PPTE rogue_pte;
    PTE original_pte;
} PTE_METHOD_DATA, *PPTE_METHOD_DATA;

/* Parse a 64 bit page table entry and print it. */
static void inline dprint_pte_contents(volatile PPTE ppte)
{
    pr_debug(
        "Page information: %#016llx\n"
        "\tpresent:      %llx\n"
        "\trw:           %llx\n"
        "\tuser:         %llx\n"
        "\twrite_through:%llx\n"
        "\tcache_disable:%llx\n"
        "\taccessed:     %llx\n"
        "\tdirty:        %llx\n"
        "\tpat/ps:       %llx\n"
        "\tglobal:       %llx\n"
        "\txd:           %llx\n"
        "\tpfn: %010llx",
        (long long unsigned int)ppte, (long long unsigned int)ppte->present,
        (long long unsigned int)ppte->rw, (long long unsigned int)ppte->user,
        (long long unsigned int)ppte->write_through,
        (long long unsigned int)ppte->cache_disable,
        (long long unsigned int)ppte->accessed,
        (long long unsigned int)ppte->dirty,
        (long long unsigned int)ppte->large_page,
        (long long unsigned int)ppte->global, (long long unsigned int)ppte->xd,
        (long long unsigned int)ppte->page_frame);
}

/* tlb_flush - flush a single TLB entry
 * @addr: virtual address for which to clear the PTE entry
 *
 * INVLPG is unfortunately not sufficient if PTI is on, see comment of
 * flush_tlb_one_kernel. In short, other PCIDs might still have a stale TLB
 * entry after this operation. Therefore, always flush the TLB before using the
 * rogue page.
 * INVLPG is an architecturally serializing instruction, thus, no barriers or
 * fences are needed. Furthermore, using the "memory" clobber effectively
 * forms a read/write memory barrier for the compiler. Thus, no further need to
 * prevent compiler reordering.
 */
static inline void tlb_flush(uint64_t addr)
{
    asm volatile("invlpg (%0)" ::"r"(addr) : "memory");
}


// Winpmem (x64 platform) uses cli/sti.

static inline void pmem_x64cli(void)
{
    asm volatile("cli" ::: "memory");
}

static inline void pmem_x64sti(void)
{
    asm volatile("sti" ::: "memory");
}

PTE_STATUS pte_remap_rogue_page_locked(PPTE_METHOD_DATA pte_data, PTE new_pte);

PTE_STATUS virt_find_pte(VIRT_ADDR vaddr, volatile PPTE *pPTE,
                         uint64_t foreign_CR3);

int setup_pte_method(PPTE_METHOD_DATA pPtedata);

void restore_pte_method(PPTE_METHOD_DATA pPtedata);

#endif
