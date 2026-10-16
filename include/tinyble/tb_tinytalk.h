/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file tb_tinytalk.h
 * @brief tinytalk's BLE commissioning transport (matter_ble.h) on tinyble,
 *        and where it differs from what matter_ble.h describes.
 *
 * Build tinytalk with CONFIG_TINYTALK_BLE=n and this with
 * CONFIG_TINYBLE_TINYTALK=y; port/tinytalk/matter_ble_tinyble.c provides
 * matter_ble.h.
 */
#pragma once

#include <tinytalk/matter_ble.h>

/* - matter_ble_start() opens the controller and runs tinyble on the system
 *   work queue.
 * - The 0xFFF6 service is in tinyble's const table, so it is never withdrawn.
 *   matter_ble_advertise(false) stops the advert and returns -EBUSY while a
 *   link is up, as the Zephyr port does, 0 otherwise.
 * - tinyble readvertises by itself after a link drops; a disconnect also
 *   calls matter_commission_readvertise().
 */
