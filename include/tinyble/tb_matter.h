/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file tb_matter.h
 * @brief Handles in the Matter commissioning GATT table (src/tb_matter_gatt.c).
 *
 * The layout is the one Zephyr builds for the door-lock sample today: its
 * 0x1801 and 0x1800 services sort first (_1_gatt_svc, _2_gap_svc), then the
 * 0xFFF6 service registered at runtime. Keeping it means a phone that cached
 * the Zephyr build's handles stays right, and replies compare byte for byte
 * with a trace of that build.
 */
#pragma once

#define TB_MATTER_H_SC_CCC 0x0004u /* Service Changed CCC */
#define TB_MATTER_H_C1     0x000Eu /* BTP RX: the commissioner writes */
#define TB_MATTER_H_C2     0x0010u /* BTP TX: we indicate */
#define TB_MATTER_H_C2_CCC 0x0011u
