/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * M2 bench app: tinyble with the Matter commissioning table, advertising as a
 * commissionable node. A write to C1 is indicated back on C2, so a phone app
 * can check discovery, writes, subscription and indications end to end.
 */
#include <string.h>

#include <soc.h>
#include <zephyr/kernel.h>

#include <tinyble/tb_matter.h>
#include <tinyble/tb_zephyr.h>
#include <tinyble/tinyble.h>

/* Flags 0x06, then Service Data 0xFFF6: opcode 0, discriminator 0xF00 with
 * version 0, vendor 0xFFF1, product 0x8001, no additional data. */
static const uint8_t k_ad[] = {0x02, 0x01, 0x06, 0x0B, 0x16, 0xF6, 0xFF, 0x00,
			       0x00, 0x0F, 0xF1, 0xFF, 0x01, 0x80, 0x00};

static uint8_t echo[244];

void tb_on_ready(int err)
{
	ARG_UNUSED(err);
}

uint8_t tb_on_write(uint16_t handle, const uint8_t *data, uint16_t len)
{
	if (handle == TB_MATTER_H_C1 && len <= sizeof(echo)) {
		memcpy(echo, data, len);
		(void)tb_indicate(TB_MATTER_H_C2, echo, len);
	}
	return 0;
}

void tb_on_ccc(uint16_t handle, uint16_t value)
{
	ARG_UNUSED(handle);
	ARG_UNUSED(value);
}

void tb_on_indicated(int err)
{
	ARG_UNUSED(err);
}

void tb_on_connect(const uint8_t peer[7])
{
	ARG_UNUSED(peer);
}

void tb_on_disconnect(uint8_t reason)
{
	ARG_UNUSED(reason);
}

int main(void)
{
	/* The FICR device address, as a static random address. */
	uint32_t lo = NRF_FICR->DEVICEADDR[0], hi = NRF_FICR->DEVICEADDR[1];
	uint8_t addr[6] = {(uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)(lo >> 16),
			   (uint8_t)(lo >> 24), (uint8_t)hi, (uint8_t)((hi >> 8) | 0xC0u)};

	if (tb_zephyr_open()) {
		return 0;
	}
	tb_init(addr);
	tb_adv(k_ad, sizeof(k_ad));
	for (;;) {
		tb_zephyr_wait();
	}
}
