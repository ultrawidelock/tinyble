/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file tb_zephyr.h
 * @brief tinyble's Zephyr port: raw HCI (CONFIG_BT_HCI_RAW) to NCS's SDC driver.
 *
 * Two ways to run it. A thread of the application's loops on
 * tb_zephyr_wait(), and every tb_* call and hook runs on that thread. Or
 * tb_zephyr_start() runs tinyble on the system work queue, hooks included,
 * and other threads make tb_* calls between tb_zephyr_lock() and
 * tb_zephyr_unlock().
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

/** Run tinyble on the system work queue (CONFIG_POLL), instead of
 *  tb_zephyr_wait(). Call it after tb_init(). */
int tb_zephyr_start(void);

/** Hold tinyble for tb_* calls from outside the system work queue. Hooks run
 *  with it held, so they must not wait on a lock that a thread calling
 *  tb_zephyr_lock() may hold. */
void tb_zephyr_lock(void);
void tb_zephyr_unlock(void);

#ifdef __cplusplus
}
#endif
