/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The Matter commissioning GATT table: GATT and GAP services as Zephyr's
 * door-lock build has them (gatt.c _1_gatt_svc and _2_gap_svc), then the BTP
 * service 0xFFF6 with C1 18EE2EF5-263D-4559-959F-4F9C429F9D11 and C2 ...9D12.
 */
#include <tinyble/tinyble.h>

/* The device name, as characters (it sits inside one byte array). */
#ifndef TB_MATTER_NAME
#define TB_MATTER_NAME 't', 'i', 'n', 'y', 't', 'a', 'l', 'k'
#endif

#define LE16(x) (uint8_t)((x) & 0xFFu), (uint8_t)((x) >> 8)
#define CHR16(props, h, u) props, LE16(h), LE16(u)
#define CHR128(props, h, last) \
	props, LE16(h), (last), 0x9Du, 0x9Fu, 0x42u, 0x9Cu, 0x4Fu, 0x9Fu, 0x95u, 0x59u, \
		0x45u, 0x3Du, 0x26u, 0xF5u, 0x2Eu, 0xEEu, 0x18u

#define PROP_READ     0x02u
#define PROP_WRITE_NR 0x04u
#define PROP_WRITE    0x08u
#define PROP_INDICATE 0x20u

#define NAME_LEN sizeof((const char[]){TB_MATTER_NAME})

/* Offsets of each value in tb_vals, in the order below. */
enum {
	V_GATT = 0,
	V_SC_CHR = V_GATT + 2,
	V_GAP = V_SC_CHR + 5,
	V_NAME_CHR = V_GAP + 2,
	V_NAME = V_NAME_CHR + 5,
	V_APP_CHR = V_NAME + NAME_LEN,
	V_APP = V_APP_CHR + 5,
	V_PPCP_CHR = V_APP + 2,
	V_PPCP = V_PPCP_CHR + 5,
	V_BTP = V_PPCP + 8,
	V_C1_CHR = V_BTP + 2,
	V_C2_CHR = V_C1_CHR + 19,
	V_END = V_C2_CHR + 19,
};

const uint8_t tb_vals[V_END] = {
	LE16(0x1801u),
	CHR16(PROP_INDICATE, 0x0003u, 0x2A05u),
	LE16(0x1800u),
	CHR16(PROP_READ, 0x0007u, 0x2A00u),
	TB_MATTER_NAME,
	CHR16(PROP_READ, 0x0009u, 0x2A01u),
	LE16(0x0000u),
	CHR16(PROP_READ, 0x000Bu, 0x2A04u),
	/* 15 ms to 30 ms, latency 0, 2 s, as the sample's PREF Kconfig */
	LE16(12u), LE16(24u), LE16(0u), LE16(200u),
	LE16(0xFFF6u),
	CHR128(PROP_WRITE | PROP_WRITE_NR, 0x000Eu, 0x11u),
	CHR128(PROP_INDICATE, 0x0010u, 0x12u),
};

#define SVC(v)      {0x2800u, TB_A_R | 2u, (v)}
#define CHR(v, len) {0x2803u, TB_A_R | (len), (v)}
#define CCC(slot)   {0x2902u, TB_A_R | TB_A_CCC | (slot), 0}

const struct tb_attr tb_attrs[] = {
	/* 0x0001 */ SVC(V_GATT),
	/* 0x0002 */ CHR(V_SC_CHR, 5u),
	/* 0x0003 */ {0x2A05u, 0u, 0},
	/* 0x0004 */ CCC(0u),
	/* 0x0005 */ SVC(V_GAP),
	/* 0x0006 */ CHR(V_NAME_CHR, 5u),
	/* 0x0007 */ {0x2A00u, TB_A_R | NAME_LEN, V_NAME},
	/* 0x0008 */ CHR(V_APP_CHR, 5u),
	/* 0x0009 */ {0x2A01u, TB_A_R | 2u, V_APP},
	/* 0x000A */ CHR(V_PPCP_CHR, 5u),
	/* 0x000B */ {0x2A04u, TB_A_R | 8u, V_PPCP},
	/* 0x000C */ SVC(V_BTP),
	/* 0x000D */ CHR(V_C1_CHR, 19u),
	/* 0x000E */ {0u, TB_A_W, 0},
	/* 0x000F */ CHR(V_C2_CHR, 19u),
	/* 0x0010 */ {0u, 0u, 0},
	/* 0x0011 */ CCC(1u),
};

const uint8_t tb_n_attrs = sizeof(tb_attrs) / sizeof(tb_attrs[0]);
