/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file tb_dfu.h
 * @brief tinyfota's DFU services in the Matter table (src/tb_matter_gatt.c,
 *        CONFIG_TINYBLE_TINYFOTA), and the hooks the DFU transport provides.
 *
 * They follow the Matter services (tb_matter.h), whose handles stay as they
 * are:
 *
 *   0x0012 SMP service 8D53DC1D-1DB7-4CD3-868B-8A527460AA84
 *   0x0014   value DA2E7828-FBCE-4E01-AE9E-261174997C48: Write Without
 *            Response, Notify; its CCC at 0x0015
 *   0x0016 native DFU service D3B5A140-9E23-4B3A-8BE4-6B1EE5F980A3
 *   0x0018   value D3B5A141-9E23-4B3A-8BE4-6B1EE5F980A3: Write, Write
 *            Without Response, Notify; its CCC at 0x0019
 *
 * tinyble's tinytalk adapter (port/tinytalk/matter_ble_tinyble.c) calls the
 * hooks in tinyble's one context: on Zephyr the system work queue, with
 * tb_zephyr_lock() held.
 *
 * Replies go out with tb_notify() on the value written, at most tb_mtu() - 3
 * bytes, once the peer has turned notifications on. Nothing is queued:
 * tb_notify() returns TB_EBUSY while the last outbound PDU is still going to
 * the controller, and the caller must try again later from the same context
 * (on Zephyr, a hook, or any thread holding tb_zephyr_lock()). A
 * notification sent from tb_dfu_on_write() goes out ahead of that write's
 * Write Response.
 */
#pragma once

#include <tinyble/tinyble.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB_DFU_H_SMP    0x0014u /* SMP value: the client writes, we notify */
#define TB_DFU_H_NATIVE 0x0018u /* native DFU value: the client writes, we notify */

/** A Write Request or Write Command to TB_DFU_H_SMP or TB_DFU_H_NATIVE.
 *  Return 0 or an ATT error code (a command's is dropped); @p data is valid
 *  only during the call. */
uint8_t tb_dfu_on_write(uint16_t handle, const uint8_t *data, uint16_t len);

/** The peer wrote the CCC of value @p handle (bit 0: notifications on). For
 *  a Write Request this fires after its Write Response is handed to the
 *  controller. */
void tb_dfu_on_ccc(uint16_t handle, uint16_t value);

/** The link went down with HCI reason @p reason: every link, used for DFU
 *  or not. The CCCs start off on the next one. */
void tb_dfu_on_disconnect(uint8_t reason);

#ifdef __cplusplus
}
#endif
