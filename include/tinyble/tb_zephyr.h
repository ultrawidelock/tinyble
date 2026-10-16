/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file tb_zephyr.h
 * @brief tinyble's Zephyr port: raw HCI (CONFIG_BT_HCI_RAW) to NCS's SDC driver.
 *
 * Every tb_* call and hook must run on the one thread that calls
 * tb_zephyr_wait().
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Open the controller (bt_enable_raw()). Call tb_init() after it. */
int tb_zephyr_open(void);

/** Block for one packet from the controller or the next tinyble timer, then
 *  hand it to tinyble. Call it in a loop. */
void tb_zephyr_wait(void);

#ifdef __cplusplus
}
#endif
