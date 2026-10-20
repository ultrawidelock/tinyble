/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * tinyble on Zephyr raw HCI: bt_buf_get_tx() and bt_send() out, an RX fifo
 * in. Packets carry their H4 type in data[0] both ways (hci_raw.c), which is
 * tinyble's TB_HCI_* numbering.
 */
#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci_raw.h>
#include <zephyr/kernel.h>

#include <tinyble/tb_zephyr.h>
#include <tinyble/tinyble.h>

static K_FIFO_DEFINE(rx_fifo);
static K_MUTEX_DEFINE(lock);
static struct net_buf *tx; /* the last tb_port_tx_alloc() buffer */

uint8_t *tb_port_tx_alloc(uint8_t type, uint16_t len)
{
	tx = bt_buf_get_tx(bt_buf_type_from_h4(type, BT_BUF_OUT), K_NO_WAIT, NULL, 0);
	if (tx && net_buf_tailroom(tx) < len) {
		net_buf_unref(tx);
		tx = NULL;
	}
	return tx ? net_buf_tail(tx) : NULL;
}

void tb_port_tx_send(uint8_t *buf, uint16_t len)
{
	ARG_UNUSED(buf);
	net_buf_add(tx, len);
	if (bt_send(tx)) { /* the driver frees it only on success */
		net_buf_unref(tx);
	}
	tx = NULL;
}

uint32_t tb_port_ms(void)
{
	return k_uptime_get_32();
}

int tb_zephyr_open(void)
{
	return bt_enable_raw(&rx_fifo);
}

void tb_zephyr_wait(void)
{
	uint32_t next = tb_poll();
	struct net_buf *buf = k_fifo_get(&rx_fifo, next == UINT32_MAX ? K_FOREVER : K_MSEC(next));

	if (buf) {
		tb_hci_rx(buf->data[0], buf->data + 1, (uint16_t)(buf->len - 1u));
		net_buf_unref(buf);
	}
}

static struct k_work_poll rx_work;
static struct k_poll_event rx_evt =
	K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_FIFO_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
					&rx_fifo, 0);

static int arm(uint32_t next)
{
	return k_work_poll_submit(&rx_work, &rx_evt, 1,
				  next == UINT32_MAX ? K_FOREVER : K_MSEC(next));
}

static void rx_work_fn(struct k_work *work)
{
	struct net_buf *buf;
	uint32_t next;

	ARG_UNUSED(work);
	tb_zephyr_lock();
	while ((buf = k_fifo_get(&rx_fifo, K_NO_WAIT)) != NULL) {
		tb_hci_rx(buf->data[0], buf->data + 1, (uint16_t)(buf->len - 1u));
		net_buf_unref(buf);
	}
	next = tb_poll();
	tb_zephyr_unlock();
	(void)arm(next);
}

int tb_zephyr_start(void)
{
	k_work_poll_init(&rx_work, rx_work_fn);
	return arm(0);
}

void tb_zephyr_lock(void)
{
	(void)k_mutex_lock(&lock, K_FOREVER);
}

void tb_zephyr_unlock(void)
{
	(void)k_mutex_unlock(&lock);
}
