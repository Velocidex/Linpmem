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

#include "precompiler.h"
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <asm/io.h>
#include <linux/mm.h>
#include <linux/mutex.h>

#include "linpmem.h"
#include "page_table.h"
#include "pte_mmap.h"

DEFINE_MUTEX(g_rogue_page_mutex);

char g_rogue_page[PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE))) = { "SacrificePhysicalPage=1;" };

// Edit the page tables to relink a virtual address to a specific physical page.
//
// Argument 1: a PTE data struct, filled with information about the rogue page to be used.
// Argument 2: the physical address to re-map to.
//
// Returns:
//  PTE_SUCCESS (with g_rogue_page_lock)
//  PTE_ERROR (without g_rogue_page_lock)
//
PTE_STATUS pte_remap_rogue_page_locked(PPTE_METHOD_DATA pte_data, PTE new_pte)
{
    if (!pte_data || !pte_data->rogue_va.pointer)
        return PTE_ERROR;

    pr_debug("Remapping va %llx to %llx\n",
             (long long unsigned int)pte_data->rogue_va.pointer,
             __pfn_to_phys(new_pte.page_frame));

    mutex_lock(&g_rogue_page_mutex);

    // It is *critical* there is no interruption while doing PTE remapping.
    // Alternatively we could allow interruption and being re-scheduled in the plain middle 
    // of messing with the PTEs, but then we need to make sure we get the same CPU core (with its private cache) when being re-scheduled. 
    // On Linux, it seems a more viable option to simply use cli/sti, which works well. 
    // The cli region is kept very small, it covers the PTE remap action and the flush command.
    
    // cli
    pmem_x64cli();
    
    // Change the pte to point to the new offset.
    WRITE_ONCE((*pte_data->rogue_pte).value, new_pte.value);

    // Flush the old pte from the tlbs (maybe incomplete, see comment)
    tlb_flush((uint64_t)pte_data->rogue_va.pointer);

    // sti
    pmem_x64sti();

    return PTE_SUCCESS;
}

// Traverses the page tables to find the pte for a given virtual address.
//
// Args:
//  _In_ VIRT_ADDR vaddr: The virtual address to resolve the pte for.
//  _Out_  PPTE * pPTE: A pointer to receive the PTE virtual address.
//                      ... if found.
//  _In_Optional   uint64_t foreign_CR3. Another CR3 (not yours) can be used instead. Hopefully you know that it's valid!
//
// Returns:
//  PTE_SUCCESS or PTE_ERROR
//
// Remarks: Supports only virtual addresses that are *not* using huge pages. It will return PTE_ERROR upon finding use of a huge page.
//          Large pages are supported.
//
//
PTE_STATUS virt_find_pte(VIRT_ADDR vaddr, volatile PPTE *pppte,
                         uint64_t foreign_cr3_pa)
{
    CR3 cr3;
    PPML4E pml4;
    PPML4E pml4e;
    PPDPTE pdpt;
    PPDPTE pdpte;
    PPDE pd;
    PPDE pde;
    PPTE pt;
    PPTE final_ppte = 0;
    PTE_STATUS status = PTE_ERROR;
    uint64_t cur_pa;

    if (!vaddr.pointer || !pppte)
        goto error;

    // Initialize _Out_ variable with zero. This guarantees there is no arbitrary value on it if we later take the error path.
    *pppte = 0;

    pr_debug("Resolving PTE for address: %llx.\n", vaddr.value);
    pr_debug(
        "Printing ambiguous names: WinDbg terminus(first)/normal terminus(second).\n");

    // Get CR3 to get to the PML4
    if (foreign_cr3_pa == 0) {
        cr3 = r_cr3_pa();
    } else if (pfn_valid(__phys_to_pfn(foreign_cr3_pa))) {
        cr3.value = foreign_cr3_pa;
    } else {
        pr_notice_ratelimited(
            "A custom CR3 was specified for vtop, but it is clearly wrong and invalid. Caller: please check your code.\n");
        goto error;
    }

    pr_debug("CR3 pa is %llx.\n", cr3.value);

    // I don't know how this could fail, but...
    if (!cr3.value)
        goto error;

    // Resolve the PML4
    cur_pa = cr3.value;
    pml4 = phys_to_virt(cur_pa);

    pr_debug("Kernel PX/PML4 base is at %llx physical, and %llx virtual.\n",
             (long long unsigned int)cr3.value, (long long unsigned int)pml4);

    if (!pml4)
        goto error;

    // Resolve the PDPT
    pml4e = pml4 + vaddr.pml4_index;

    if (!pml4e->present) {
        pr_notice_ratelimited("Address %llx has no valid mapping in PML4\n",
                              vaddr.value);
        dprint_pte_contents((PPTE)pml4e);
        goto error;
    }

    pr_debug("PXE/PML4[%llx] (at %llx): %llx\n",
             (long long unsigned int)vaddr.pml4_index,
             (long long unsigned int)pml4e,
             (long long unsigned int)pml4e->value);

    cur_pa = PFN_PHYS(pml4e->pdpt_p);
    pdpt = phys_to_virt(cur_pa);

    pr_debug("Points to PP/PDPT base: %llx.\n", (long long unsigned int)pdpt);

    if (!pdpt)
        goto error;

    // Resolve the PDT
    pdpte = pdpt + vaddr.pdpt_index;

    if (!pdpte->present) {
        pr_notice_ratelimited("Address %llx has no valid mapping in PDPT\n",
                              vaddr.value);
        dprint_pte_contents((PPTE)pdpte);
        goto error;
    }

    if (pdpte->large_page) {
        pr_notice_ratelimited("Address %llx belongs to a 1GB huge page\n",
                              vaddr.value);
        dprint_pte_contents((PPTE)pdpte);
        goto error;
    }

    pr_debug("PPE/PDPT[%llx] (at %llx): %llx.\n",
             (long long unsigned int)vaddr.pdpt_index,
             (long long unsigned int)pdpte, pdpte->value);

    cur_pa = PFN_PHYS(pdpte->pd_p);
    pd = phys_to_virt(cur_pa);

    pr_debug("Points to PD base: %llx.\n", (long long unsigned int)pd);

    if (!pd)
        goto error;

    // Resolve the PT
    pde = pd + vaddr.pd_index;

    if (!pde->present) {
        pr_notice_ratelimited("Address %llx has no valid mapping in PD\n",
                              vaddr.value);
        dprint_pte_contents((PPTE)pde);
        goto error;
    }

    if (pde->large_page) {
        // this is basically like a PTE, just like one tier level above. Though not 100%.
        final_ppte = (PPTE)pde;
        *pppte = final_ppte;

        pr_debug("Final 'PTE' --large page PDE-- (at %llx) : %llx.\n",
                 (long long unsigned int)final_ppte,
                 (long long unsigned int)final_ppte->value);

        return PTE_SUCCESS;
    }

    pr_debug("PDE/PD[%llx] (at %llx): %llx.\n",
             (long long unsigned int)vaddr.pd_index,
             (long long unsigned int)pde, (long long unsigned int)pde->value);

    cur_pa = PFN_PHYS(pde->pt_p);
    pt = phys_to_virt(cur_pa);

    pr_debug("Points to PT base: %llx.\n", (long long unsigned int)pt);

    if (!pt)
        goto error;

    // Get the PTE and Page Frame
    final_ppte = pt + vaddr.pt_index;

    if (!final_ppte)
        goto error;

    if (!final_ppte->present) {
        pr_notice_ratelimited("Address %llx has no valid mapping in PT\n",
                              vaddr.value);
        dprint_pte_contents(final_ppte);
        goto error;
    }

    pr_debug("final PTE [%llx] (at %llx): %llx.\n",
             (long long unsigned int)vaddr.pt_index,
             (long long unsigned int)final_ppte,
             (long long unsigned int)final_ppte->value);

    *pppte = final_ppte;

    // Everything went well, set PTE_SUCCESS
    status = PTE_SUCCESS;

error:
    return status;
}

int setup_pte_method(PPTE_METHOD_DATA pte_data)
{
    PTE_STATUS pte_status;

    pte_data->pte_method_is_ready_to_use = false;

    if (!PAGE_ALIGNED(g_rogue_page)) {
        pr_warn(
            "Setup of PTE method failed: rogue map is not pagesize aligned. This is a programming error!\n");
        return -1;
    }
    pte_data->rogue_va.pointer = g_rogue_page;

    // We only need one PTE for the rogue page, and just remap the PFN.
    // A part of the driver's body is sacrificed for this.
    // However, during rest of the life time, this part of the driver 
    // must be considered "missing", basically to be treated as a black hole.
    pte_status = virt_find_pte(pte_data->rogue_va, &pte_data->rogue_pte, 0);
    if (pte_status != PTE_SUCCESS) {
        pr_warn(
            "Setup of PTE method failed: virt_find_pte failed. This method will not be available!\n");
        return -1;
    }

    // Backup original rogue page (full pte)
    pte_data->original_pte.value = pte_data->rogue_pte->value;

    if (!pte_data->original_pte.page_frame) // <= shouldn't we put pfn_valid here instead?
    // not going to fail until there is some voodoo VSM magic going on. But there are a few anomalous systems.
    {
        pr_warn(
            "Setup of PTE method failed: no rogue page pfn?!?. This method will not be available!\n");
        return -1;
    }

    pte_data->pte_method_is_ready_to_use = true;

    return 0;
}

void restore_pte_method(PPTE_METHOD_DATA pte_data)
{
    PTE_STATUS pte_status;

    // If pte method IS ALREADY false, then do nothing
    // (This might for example happen in DriverEntry in the error path.)
    if (!pte_data->pte_method_is_ready_to_use)
        return;

    // If there is null stored don't even try to restore. null is wrong.
    if (!pte_data->original_pte.page_frame)
    {
        pr_crit(
            "Restoring the sacrificed section failed horribly. The backup value was null! Please reboot soon.\n");
        return;
    }

    pte_status = pte_remap_rogue_page_locked(pte_data, pte_data->original_pte);
    if (pte_status != PTE_SUCCESS) {
        pr_crit("PTE remapping error in restore function.\n");
        return;
    }

    if (g_rogue_page[0] == 'S')
        pr_info("Sacrifice section successfully restored: %s.\n", g_rogue_page);
    else
        pr_crit("Uh-oh, restoring failed. Consider rebooting. (Right now.)\n");

    mutex_unlock(&g_rogue_page_mutex);

    return;
}
