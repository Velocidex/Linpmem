/* SPDX-FileCopyrightText: © 2023 Viviane Zwanger, Valentin Obst <legal@eb9f.de>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LINPMEM_H_
#define _LINPMEM_H_

#include "pte_mmap.h"
#include "../userspace_interface/linpmem_shared.h"

/* Our Device Extension Structure.
 * pte_data	Our management data for the rogue page.
 * 		Contains volatile PPTE of rogue_pte.
 *		READ ONLY after init method!
 *		*pte_data.rogue_pte and *pte_data.rogue_va are protected by
 *		g_rogue_page_lock. Do not read/write eiter without holding it.
 */
typedef struct {
    PTE_METHOD_DATA pte_data;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

extern DEVICE_EXTENSION g_device_extension;

#endif
