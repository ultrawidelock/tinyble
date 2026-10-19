/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The Matter commissioning GATT table: GATT and GAP services as Zephyr's
 * door-lock build has them (gatt.c _1_gatt_svc and _2_gap_svc), then the BTP
 * service 0xFFF6 with C1 18EE2EF5-263D-4559-959F-4F9C429F9D11 and C2 ...9D12.
 * With TB_MATTER_DFU (CONFIG_TINYBLE_TINYFOTA), tinyfota's SMP and native DFU
 * services follow (tb_dfu.h).
 */
#include <tinyble/tb_aliro.h>
#include <tinyble/tinyble.h>

/* The device name, as characters (it sits inside one byte array). */
#ifndef TB_MATTER_NAME
#define TB_MATTER_NAME 't', 'i', 'n', 'y', 't', 'a', 'l', 'k'
#endif
#ifndef TB_MATTER_DFU
#define TB_MATTER_DFU 0
#endif
#ifndef TB_ALIRO
#define TB_ALIRO 0
#endif

#define LE16(x) (uint8_t)((x) & 0xFFu), (uint8_t)((x) >> 8)
#define CHR16(props, h, u) props, LE16(h), LE16(u)
#define CHR128(props, h, last) \
	props, LE16(h), (last), 0x9Du, 0x9Fu, 0x42u, 0x9Cu, 0x4Fu, 0x9Fu, 0x95u, 0x59u, \
		0x45u, 0x3Du, 0x26u, 0xF5u, 0x2Eu, 0xEEu, 0x18u
/* tinyfota's native UUIDs, D3B5A1xx-9E23-4B3A-8BE4-6B1EE5F980A3. */
#define NAT128(xx) \
	0xA3u, 0x80u, 0xF9u, 0xE5u, 0x1Eu, 0x6Bu, 0xE4u, 0x8Bu, 0x3Au, 0x4Bu, 0x23u, 0x9Eu, (xx), \
		0xA1u, 0xB5u, 0xD3u

#define PROP_READ     0x02u
#define PROP_WRITE_NR 0x04u
#define PROP_WRITE    0x08u
#define PROP_NOTIFY   0x10u
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
	V_SMP = V_C2_CHR + 19, /* TB_MATTER_DFU from here */
	V_SMP_CHR = V_SMP + 16,
	V_NAT = V_SMP_CHR + 19,
	V_NAT_CHR = V_NAT + 16,
	V_ALIRO = TB_MATTER_DFU ? V_NAT_CHR + 19 : V_SMP, /* TB_ALIRO from here */
	V_ALIRO_RD_CHR = V_ALIRO + 2,
	V_ALIRO_RD = V_ALIRO_RD_CHR + 19,
	V_ALIRO_WR_CHR = V_ALIRO_RD + 7,
	V_END = TB_ALIRO ? V_ALIRO_WR_CHR + 19 : V_ALIRO,
};

_Static_assert(V_END <= 256, "tb_attrs offsets are one byte");

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
#if TB_MATTER_DFU
	/* SMP 8D53DC1D-1DB7-4CD3-868B-8A527460AA84, its characteristic
	 * DA2E7828-FBCE-4E01-AE9E-261174997C48 */
	0x84u, 0xAAu, 0x60u, 0x74u, 0x52u, 0x8Au, 0x8Bu, 0x86u, 0xD3u, 0x4Cu, 0xB7u, 0x1Du, 0x1Du,
	0xDCu, 0x53u, 0x8Du,
	PROP_WRITE_NR | PROP_NOTIFY, LE16(0x0014u), 0x48u, 0x7Cu, 0x99u, 0x74u, 0x11u, 0x26u, 0x9Eu,
	0xAEu, 0x01u, 0x4Eu, 0xCEu, 0xFBu, 0x28u, 0x78u, 0x2Eu, 0xDAu,
	NAT128(0x40u),
	PROP_WRITE | PROP_WRITE_NR | PROP_NOTIFY, LE16(0x0018u), NAT128(0x41u),
#endif
#if TB_ALIRO
	/* The credential reader service 0xFFF2 (tb_aliro.h). Its READ value is
	 * [SPSM be16][versions length][version be16][features length][features]:
	 * SPSM 0x0080, one BLE-UWB protocol version 0x0100, no features. */
	LE16(0xFFF2u),
	PROP_READ, LE16(TB_ALIRO_H_READ), NAT128(0x30u),
	0x00u, 0x80u, 0x02u, 0x01u, 0x00u, 0x01u, 0x00u,
	/* BD4B9502-3F54-11EC-B919-0242AC120005: the version the device selected */
	PROP_WRITE, LE16(TB_ALIRO_H_VER), 0x05u, 0x00u, 0x12u, 0xACu, 0x42u, 0x02u, 0x19u, 0xB9u,
	0xECu, 0x11u, 0x54u, 0x3Fu, 0x02u, 0x95u, 0x4Bu, 0xBDu,
#endif
};

#define SVC(v)      {0x2800u, TB_A_R | 2u, (v)}
#define SVC128(v)   {0x2800u, TB_A_R | 16u, (v)}
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
#if TB_MATTER_DFU
	/* 0x0012 */ SVC128(V_SMP),
	/* 0x0013 */ CHR(V_SMP_CHR, 19u),
	/* 0x0014 */ {0u, TB_A_W, 0},
	/* 0x0015 */ CCC(2u),
	/* 0x0016 */ SVC128(V_NAT),
	/* 0x0017 */ CHR(V_NAT_CHR, 19u),
	/* 0x0018 */ {0u, TB_A_W, 0},
	/* 0x0019 */ CCC(3u),
#endif
#if TB_ALIRO
	/* TB_ALIRO_H_FIRST */ SVC(V_ALIRO),
	CHR(V_ALIRO_RD_CHR, 19u),
	/* TB_ALIRO_H_READ */ {0u, TB_A_R | 7u, V_ALIRO_RD},
	CHR(V_ALIRO_WR_CHR, 19u),
	/* TB_ALIRO_H_VER */ {0u, TB_A_W, 0},
#endif
};

const uint8_t tb_n_attrs = sizeof(tb_attrs) / sizeof(tb_attrs[0]);
