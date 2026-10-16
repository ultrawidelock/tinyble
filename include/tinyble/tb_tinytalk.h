/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file tb_tinytalk.h
 * @brief tinytalk's BLE commissioning transport (matter_ble.h) on tinyble,
 *        and what the application needs beyond it.
 *
 * Build tinytalk with CONFIG_TINYTALK_BLE=n and this with
 * CONFIG_TINYBLE_TINYTALK=y; port/tinytalk/matter_ble_tinyble.c provides
 * matter_ble.h.
 */
#pragma once

#include <stdbool.h>

#include <tinytalk/matter_ble.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Open the controller and run tinyble on the system work queue. Call it
 *  before matter_commission_init(). */
int matter_ble_start(void);

/** Advertise the commissionable payload, with the current discriminator, or
 *  stop. tinyble readvertises by itself after a link drops; a disconnect also
 *  calls matter_commission_readvertise(). */
void matter_ble_advertise(bool on);

#ifdef __cplusplus
}
#endif
