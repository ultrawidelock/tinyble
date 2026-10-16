/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file tinyble.h
 * @brief A minimal BLE peripheral host: one connection, a const GATT table,
 *        indications and notifications, no pairing. Portable C11; speaks HCI through three port
 *        functions.
 *
 * Size is the design goal, so the rules are strict:
 *
 * - One L2CAP PDU each way at a time, up to the 247 B ATT MTU: inbound ones are
 *   reassembled from ACL packets, outbound ones split into the controller's
 *   ACL length, so any data length works, 27 B included.
 * - Nothing is queued. Owed packets are a bitmask, each built when the one
 *   before it has gone, and sent while the controller has free ACL buffers.
 * - No threads and no locks: every tb_* call and every hook runs in one
 *   context. The port serializes.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional behaviour, Zephyr's, off by default: none of it appeared in an
 * Apple Home commissioning (RESEARCH.md). Define one to 1 to restore it. */
#ifndef TB_FIND_BY_TYPE
#define TB_FIND_BY_TYPE 0 /* Find By Type Value; off: Request Not Supported */
#endif
#ifndef TB_READ_BLOB
#define TB_READ_BLOB 0 /* Read Blob; off: Request Not Supported */
#endif
#ifndef TB_TYPE128
#define TB_TYPE128 0 /* Read By Type on a 128-bit type; off: Attribute Not Found */
#endif
#ifndef TB_SIG_REJECT
#define TB_SIG_REJECT 0 /* Command Reject for unknown signaling; off: ignored */
#endif
#ifndef TB_SMP_REFUSE
#define TB_SMP_REFUSE 0 /* Pairing Failed to any SMP PDU; off: counted as dropped */
#endif
#ifndef TB_ATT_TIMEOUT
#define TB_ATT_TIMEOUT 0 /* 30 s indication timeout; off: wait for the link */
#endif

/* Error returns. The values are Zephyr's errno numbers, so a Zephyr adapter
 * can pass them through. */
#define TB_EIO        (-5)
#define TB_EAGAIN     (-11)
#define TB_EBUSY      (-16)
#define TB_EINVAL     (-22)
#define TB_ECONNRESET (-104)
#define TB_ETIMEDOUT  (-116)
#define TB_ENOTCONN   (-128)

/* HCI packet types, H4 numbering. */
#define TB_HCI_CMD 0x01u
#define TB_HCI_ACL 0x02u
#define TB_HCI_EVT 0x04u

/* ---- GATT table ------------------------------------------------------------
 *
 * Handle n is entry n - 1, 4 B each. An entry's value is the len bytes at
 * tb_vals + off. A characteristic value whose type is 0 has the 128-bit UUID
 * carried in the declaration just before it (bytes 3 to 18). A CCC must
 * directly follow the value it configures; its len is its CCC slot (0 to 3).
 */
#define TB_A_LEN 0x1Fu /* fl: the value length, at most 31 B */
#define TB_A_R   0x20u /* fl: readable */
#define TB_A_W   0x40u /* fl: writes go to tb_on_write() */
#define TB_A_CCC 0x80u /* fl: client characteristic configuration, held per link */

struct tb_attr {
	uint16_t type;
	uint8_t fl;  /* TB_A_* flags | length */
	uint8_t off; /* into tb_vals */
};

/* The table and its values, supplied at link time (src/tb_matter_gatt.c is
 * the Matter one). */
extern const struct tb_attr tb_attrs[];
extern const uint8_t tb_n_attrs;
extern const uint8_t tb_vals[];

/* ---- API ------------------------------------------------------------------- */

/**
 * Start the controller: event masks, buffer size, address, advertising
 * parameters. tb_on_ready() reports the result.
 * @param addr static random address, least significant byte first, or NULL
 *        to advertise with the controller's public address. Copied.
 */
void tb_init(const uint8_t *addr);

/** One HCI event (TB_HCI_EVT) or ACL packet (TB_HCI_ACL) from the controller. */
void tb_hci_rx(uint8_t type, const uint8_t *pkt, uint16_t len);

/**
 * Run timers: the connection parameter request and, with TB_ATT_TIMEOUT, the
 * 30 s ATT timeout.
 * @return milliseconds until it needs calling again, UINT32_MAX for never.
 * Calling it early or often is harmless.
 */
uint32_t tb_poll(void);

/**
 * Advertise connectable, undirected, every 100 ms to 150 ms, whenever no link
 * is up. @p ad (at most 31 B) is borrowed until the next tb_adv() call.
 * A zero @p len stops advertising.
 */
void tb_adv(const uint8_t *ad, uint8_t len);

/**
 * Indicate @p len bytes on characteristic value @p handle. @p data is borrowed
 * until tb_on_indicated() fires, which happens exactly once after a zero
 * return.
 * @return 0, TB_ENOTCONN, TB_EAGAIN if the peer has not enabled indications,
 *         TB_EBUSY while one is outstanding, TB_EINVAL if it exceeds MTU - 3.
 */
int tb_indicate(uint16_t handle, const uint8_t *data, uint16_t len);

/**
 * Notify @p len bytes on characteristic value @p handle. No confirmation, so
 * no hook: @p data is copied before the call returns.
 * @return 0, TB_ENOTCONN, TB_EAGAIN if the peer has not enabled notifications,
 *         TB_EBUSY while the last outbound PDU is still going to the
 *         controller, TB_EINVAL if it exceeds MTU - 3.
 */
int tb_notify(uint16_t handle, const uint8_t *data, uint16_t len);

/** Current ATT MTU: 23 until the peer exchanges it. */
uint16_t tb_mtu(void);

/** Drop the link with HCI reason @p reason (0x13: remote user terminated). */
void tb_disconnect(uint8_t reason);

/** Inbound ACL packets dropped: not ours, out of order, too long, or malformed. */
extern uint8_t tb_rx_dropped;

/* ---- port: provided by the platform ---------------------------------------- */

/** A buffer for one outbound HCI packet of @p type, at least @p len bytes, the
 *  H4 type byte excluded; NULL if none is free (tinyble retries later). */
uint8_t *tb_port_tx_alloc(uint8_t type, uint16_t len);
/** Hand the packet built in the last tb_port_tx_alloc() buffer to the controller. */
void tb_port_tx_send(uint8_t *buf, uint16_t len);
/** A free-running millisecond clock. */
uint32_t tb_port_ms(void);

/* ---- hooks: provided by the application ------------------------------------ */

/** Controller ready (0), or an init command failed (negated HCI status). */
void tb_on_ready(int err);
/** A link came up (LE Connection Complete; the enhanced one stays masked).
 *  @p peer is the peer's address type (0 public, 1 random), then its address,
 *  least significant byte first; valid only during the call. */
void tb_on_connect(const uint8_t peer[7]);
/** A write to a TB_A_W attribute, request or command. Return 0 or an ATT
 *  error code; @p data is valid only during the call. */
uint8_t tb_on_write(uint16_t handle, const uint8_t *data, uint16_t len);
/** The peer wrote the CCC of value @p handle. For a Write Request this fires
 *  after the Write Response is handed to the controller, so an indication
 *  sent from here cannot overtake it. */
void tb_on_ccc(uint16_t handle, uint16_t value);
/** The outstanding indication was confirmed (0), timed out (TB_ATT_TIMEOUT),
 *  or the link fell. */
void tb_on_indicated(int err);
/** The link went down with HCI reason @p reason. */
void tb_on_disconnect(uint8_t reason);

#ifdef __cplusplus
}
#endif
