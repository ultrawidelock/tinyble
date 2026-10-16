/* SPDX-License-Identifier: Apache-2.0 */

/*
 * tinytalk's door-lock sample on tinyble: the same node, boot order, LED and
 * button as tinytalk's samples/door-lock, with BLE on tinyble
 * (matter_ble_tinyble.c) instead of Zephyr's host.
 *
 * LED0 is on while unlocked. Button 0: a short press opens a commissioning
 * window for WINDOW_S; held for FACTORY_RESET_MS it erases the fabrics,
 * retracts the SRP records and reboots.
 */
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>

#include <psa/crypto.h>

#include <tinyble/tb_tinytalk.h>
#include <tinytalk/matter_commission.h>
#include <tinytalk/matter_fab_store.h>
#include <tinytalk/matter_thread.h>
#include <tinytalk/tm_door_lock.h>

LOG_MODULE_REGISTER(door_lock, LOG_LEVEL_INF);

#define WINDOW_S         900u
#define FACTORY_RESET_MS 5000
#define SRP_RETRACT_MS   2000

static const struct matter_commission_config s_matter_cfg = {
	.vendor_id = CONFIG_TINYTALK_VENDOR_ID,
	.product_id = CONFIG_TINYTALK_PRODUCT_ID,
	.verifier_hex = CONFIG_TINYTALK_SPAKE2P_VERIFIER,
	.salt_hex = CONFIG_TINYTALK_SPAKE2P_SALT,
	.iterations = CONFIG_TINYTALK_SPAKE2P_ITERATIONS,
	.product_name = "tinytalk door lock",
	.hardware_version = 1u,
	.software_version = 1u,
	.endpoints = &tm_door_lock_endpoint,
	.n_endpoints = 1u,
};

static const struct gpio_dt_spec s_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec s_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* The node asks with its lock held, so the work runs on the system queue. */
static void adv_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	matter_ble_advertise(!matter_commission_has_fabric() || matter_commission_window_open());
}
static K_WORK_DEFINE(s_adv_work, adv_work_fn);

void matter_commission_readvertise(void)
{
	k_work_submit(&s_adv_work);
}

void tm_door_lock_actuate(uint8_t lock_state)
{
	(void)gpio_pin_set_dt(&s_led, lock_state == MATTER_DL_LOCK_STATE_UNLOCKED);
	LOG_INF("door %s", lock_state == MATTER_DL_LOCK_STATE_UNLOCKED ? "unlocked" : "locked");
}

/* ---- the button ------------------------------------------------------------ */

static struct gpio_callback s_button_cb;
/* -1 until a press this boot: the release after a factory-reset reboot is not
 * a short press. */
static int64_t s_pressed_at = -1;

/* Clearing the settings store also takes OpenThread's dataset and PSA's keys. */
static void reboot_fn(struct k_work *work)
{
	void *storage;

	ARG_UNUSED(work);
	if (settings_storage_get(&storage) == 0) {
		(void)nvs_clear(storage);
	}
	sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(s_reboot_work, reboot_fn);

/* Retract the SRP records first, or Apple Home dials the stale ones. */
static void factory_reset_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (gpio_pin_get_dt(&s_button) != 1) {
		return;
	}
	LOG_WRN("button held: factory reset");
	(void)gpio_pin_set_dt(&s_led, 1);
	(void)matter_fab_erase();
	matter_thread_advertise_reset();
	(void)k_work_reschedule(&s_reboot_work, K_MSEC(SRP_RETRACT_MS));
}
static K_WORK_DELAYABLE_DEFINE(s_factory_reset_work, factory_reset_fn);

static void open_window_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (matter_commission_open_window(WINDOW_S) == 0) {
		LOG_INF("commissioning window open for %u s", WINDOW_S);
	}
}
static K_WORK_DEFINE(s_open_window_work, open_window_fn);

static void on_button(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	if (gpio_pin_get_dt(&s_button) == 1) {
		s_pressed_at = k_uptime_get();
		(void)k_work_reschedule(&s_factory_reset_work, K_MSEC(FACTORY_RESET_MS));
		return;
	}
	(void)k_work_cancel_delayable(&s_factory_reset_work);
	if (s_pressed_at >= 0 && k_uptime_get() - s_pressed_at < FACTORY_RESET_MS) {
		k_work_submit(&s_open_window_work);
	}
	s_pressed_at = -1;
}

static int button_init(void)
{
	int rc = gpio_is_ready_dt(&s_button) ? 0 : -ENODEV;

	if (rc == 0) {
		rc = gpio_pin_configure_dt(&s_button, GPIO_INPUT);
	}
	if (rc == 0) {
		rc = gpio_pin_interrupt_configure_dt(&s_button, GPIO_INT_EDGE_BOTH);
	}
	if (rc == 0) {
		gpio_init_callback(&s_button_cb, on_button, BIT(s_button.pin));
		rc = gpio_add_callback(s_button.port, &s_button_cb);
	}
	return rc;
}

int main(void)
{
	int rc;

	if (gpio_is_ready_dt(&s_led)) {
		(void)gpio_pin_configure_dt(&s_led, GPIO_OUTPUT_INACTIVE);
	}
	if (button_init() != 0) {
		LOG_WRN("no button: commissioning window and factory reset unavailable");
	}
	if (psa_crypto_init() != PSA_SUCCESS) {
		LOG_ERR("PSA Crypto did not start");
		return 0;
	}
	rc = settings_subsys_init();
	if (rc != 0) {
		LOG_ERR("settings did not start (%d)", rc);
		return 0;
	}
	rc = matter_ble_start();
	if (rc != 0) {
		LOG_ERR("Bluetooth did not start (%d)", rc);
		return 0;
	}
	rc = matter_commission_init(&s_matter_cfg);
	if (rc != 0) {
		LOG_ERR("Matter node did not start (%d)", rc);
		return 0;
	}
	LOG_INF("manual pairing code %s, discriminator 0x%03x", MANUAL_CODE,
		CONFIG_TINYTALK_DISCRIMINATOR);
	matter_commission_readvertise();
	return 0;
}
