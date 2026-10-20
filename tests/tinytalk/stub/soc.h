/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */
/* Host stub: the FICR, owned by the test. */
#pragma once

#include <stdint.h>

struct nrf_ficr {
	uint32_t DEVICEADDR[2];
};

extern struct nrf_ficr g_ficr;
#define NRF_FICR (&g_ficr)
