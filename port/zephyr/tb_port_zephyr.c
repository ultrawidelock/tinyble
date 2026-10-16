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
