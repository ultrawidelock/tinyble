/* SPDX-License-Identifier: Apache-2.0 */

/*
 * tinytalk's matter_ble.h on tinyble: C1 writes go into tinytalk's BTP
 * reassembler (matter_btp.h), its fragments come out as C2 indications. The
 * rules follow tinytalk's ports/zephyr/matter_ble_zephyr.c, whose comments
 * say where each was found.
 *
 * tinyble runs on the system work queue. tinytalk may call matter_ble_send()
 * from other threads, so it takes the port lock; the three tinytalk callbacks
 * run from cb_work without it, because they take tinytalk's lock, which a
 * thread waiting on ours in matter_ble_send() holds.
 */
#include <errno.h>
#include <string.h>

#include <soc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <tinyble/tb_matter.h>
#include <tinyble/tb_tinytalk.h>
#include <tinyble/tb_zephyr.h>
#include <tinyble/tinyble.h>
#include <tinytalk/matter_btp.h>

LOG_MODULE_REGISTER(tb_tinytalk, CONFIG_TINYBLE_TINYTALK_LOG_LEVEL);

BUILD_ASSERT(MATTER_BLE_API == 1, "port/tinytalk follows matter_ble.h version 1");

#define RX_BUF 1024u /* largest inbound message; tinytalk's default */
#define WINDOW 4u    /* BTP receive window we grant; tinytalk's default */

#define ATT_VALUE_NOT_ALLOWED 0x13u
#define HCI_REMOTE_USER_TERM  0x13u

static struct matter_btp_rx s_rx;
static uint8_t s_rx_buf[RX_BUF];
static struct matter_btp_tx s_tx;
static uint8_t s_frag[MATTER_BTP_MAX_FRAGMENT]; /* the indication in flight */
static uint8_t s_hs_len; /* handshake response held in s_frag until C2 is on */
static bool s_claimed, s_handshaked, s_tx_active, s_busy, s_tx_notifies, s_ack_pending;
static bool s_linked, s_started;
static uint8_t s_tx_seq, s_ack_seq, s_unacked;
static uint16_t s_frag_size, s_discriminator;

/* Flags 0x06, then Service Data 0xFFF6: opcode 0, discriminator and version
 * 0 (bytes 8 and 9, set when advertising), vendor, product, no additional data
 * (CHIPBleServiceData.h). */
static uint8_t s_ad[] = {0x02, 0x01, 0x06, 0x0B, 0x16, 0xF6, 0xFF, 0x00, 0x00, 0x00,
			 CONFIG_TINYTALK_VENDOR_ID & 0xFFu, CONFIG_TINYTALK_VENDOR_ID >> 8,
			 CONFIG_TINYTALK_PRODUCT_ID & 0xFFu, CONFIG_TINYTALK_PRODUCT_ID >> 8, 0x00};

/* tinytalk callbacks owed, run by cb_work. */
#define CB_TX   BIT(0)
#define CB_LINK BIT(1)
#define CB_MSG  BIT(3)
static uint8_t s_cb;
static int s_tx_status;
static size_t s_msg_len;
static matter_ble_msg_cb s_msg_cb;
static matter_ble_link_cb s_link_cb;
static matter_ble_tx_cb s_tx_cb;

static void cb_work_fn(struct k_work *work)
{
	uint8_t cb;
	int status;

	ARG_UNUSED(work);
	tb_zephyr_lock();
	cb = s_cb;
	s_cb = 0;
	status = s_tx_status;
	tb_zephyr_unlock();
	if ((cb & CB_TX) && s_tx_cb) {
		s_tx_cb(status);
	}
	if ((cb & CB_LINK) && s_link_cb) {
		s_link_cb();
	}
	if (cb & CB_MSG) {
		/* Both run on this queue, so no fragment lands mid-handler. */
		if (s_msg_cb) {
			s_msg_cb(s_rx_buf, s_msg_len);
		}
		matter_btp_rx_reset(&s_rx);
	}
}
static K_WORK_DEFINE(s_cb_work, cb_work_fn);

static void owe(uint8_t cb)
{
	s_cb |= cb;
	k_work_submit(&s_cb_work);
}

/* The message borrowed by matter_ble_send() goes back exactly once. */
static void tx_done(int status)
{
	s_tx_active = false;
	if (s_tx_notifies) {
		s_tx_notifies = false;
		s_tx_status = status;
		owe(CB_TX);
	}
}

static void reset_link(void)
{
	tx_done(-ECONNRESET);
	matter_btp_rx_init(&s_rx, s_rx_buf, sizeof(s_rx_buf), 0u);
	s_handshaked = false;
	s_busy = false;
	s_ack_pending = false;
	s_tx_seq = 0u;
	s_unacked = 0u;
	s_hs_len = 0u;
	owe(CB_LINK);
}

static int indicate(size_t n)
{
	int rc = tb_indicate(TB_MATTER_H_C2, s_frag, (uint16_t)n);

	LOG_DBG("C2 indicate %u B, flags 0x%02x: %d", (unsigned int)n, s_frag[0], rc);
	s_busy = rc == 0;
	return rc;
}

/*
 * What we owe the peer, as a standalone ack, when no fragment of ours will
 * carry it. CHIP acks every fragment it receives, its standalone acks
 * included, within 2.5 s and drops the link when one of its own goes 15 s
 * unacked (BLEEndPoint.cpp: BTP_ACK_SEND_TIMEOUT_MS, BTP_ACK_TIMEOUT_MS).
 * Held while the handshake response still occupies s_frag.
 */
static void send_ack(void)
{
	size_t n = 0;

	if (!s_ack_pending || s_tx_active || s_busy || s_hs_len ||
	    matter_btp_standalone_ack(s_ack_seq, s_tx_seq, s_frag, sizeof(s_frag), &n) != MATTER_OK) {
		return;
	}
	LOG_DBG("BTP ack %u, seq %u", s_ack_seq, s_tx_seq);
	if (indicate(n) == 0) {
		s_tx_seq++;
		s_ack_pending = false;
		s_unacked = 0u;
	}
}

/* The response waits for the subscription: an iPhone writes C1 first. */
static void send_hs(void)
{
	if (s_hs_len && indicate(s_hs_len) == 0) {
		s_hs_len = 0u;
	}
}

static int pump_tx(void)
{
	size_t n = 0;
	int rc;

	if (!s_tx_active || s_busy) {
		return 0;
	}
	rc = matter_btp_tx_next(&s_tx, s_ack_pending ? &s_ack_seq : NULL, s_frag, sizeof(s_frag),
				&n);
	if (rc == MATTER_END) {
		tx_done(0);
		return 0;
	}
	rc = rc == MATTER_OK ? indicate(n) : -EIO;
	if (rc) {
		tx_done(rc);
		return rc;
	}
	/* Sequence and piggybacked ack count only once the fragment is taken. */
	LOG_DBG("BTP seq %u%s, ack %d", s_tx_seq, s_tx.off == s_tx.len ? " end" : "",
		s_ack_pending ? s_ack_seq : -1);
	s_tx_seq = s_tx.next_seq;
	s_ack_pending = false;
	s_unacked = 0u;
	return 0;
}

/* ---- tinyble hooks -------------------------------------------------------- */

void tb_on_ready(int err)
{
	if (err) {
		LOG_ERR("controller init failed (%d)", err);
	}
}

/* C1 is the table's only writable value. */
uint8_t tb_on_write(uint16_t handle, const uint8_t *data, uint16_t len)
{
	struct matter_btp_handshake_req req;
	struct matter_btp_handshake_resp resp;
	size_t n = 0;
	int rc;

	ARG_UNUSED(handle);
	if (!s_claimed) {
		s_claimed = true;
		reset_link();
	}
	if (!s_handshaked) {
		if (matter_btp_req_decode(data, len, &req) != MATTER_OK ||
		    matter_btp_accept(&req, tb_mtu(), WINDOW, &resp) != MATTER_OK ||
		    matter_btp_resp_encode(&resp, s_frag, sizeof(s_frag), &n) != MATTER_OK) {
			return ATT_VALUE_NOT_ALLOWED;
		}
		/* As peripheral the response is sequence 0: our first fragment is
		 * 1, the peer's is 0 (BLEEndPoint.cpp:512-514). */
		matter_btp_rx_init(&s_rx, s_rx_buf, sizeof(s_rx_buf), 0u);
		s_tx_seq = 1u;
		s_frag_size = resp.fragment_size;
		s_handshaked = true;
		s_hs_len = (uint8_t)n;
		LOG_INF("BTP up: fragment %u, ATT MTU %u", resp.fragment_size, tb_mtu());
		send_hs();
		return 0;
	}
	rc = matter_btp_rx_fragment(&s_rx, data, len);
	LOG_DBG("C1 %u B, flags 0x%02x: %d, seq %u, ack %d", len, len ? data[0] : 0u, rc,
		s_rx.last_seq, s_rx.got_ack ? s_rx.ack : -1);
	if (rc != MATTER_OK && rc != MATTER_END) {
		/* BTP cannot recover a gap. */
		LOG_ERR("BTP fragment refused (%d)", rc);
		tb_disconnect(HCI_REMOTE_USER_TERM);
		return ATT_VALUE_NOT_ALLOWED;
	}
	s_ack_pending = true;
	s_ack_seq = s_rx.last_seq;
	if (s_unacked < UINT8_MAX) {
		s_unacked++;
	}
	if (rc == MATTER_END) {
		s_msg_len = s_rx.len;
		owe(CB_MSG);
	}
	/* Ack at half the window, a whole message or a standalone ack, or the
	 * peer stalls or times out. A fragment going out carries the ack
	 * instead; tb_on_indicated() sends any still owed. */
	if (rc == MATTER_END || s_unacked >= WINDOW / 2u ||
	    !(data[0] & (MATTER_BTP_FLAG_START | MATTER_BTP_FLAG_CONTINUE | MATTER_BTP_FLAG_END))) {
		send_ack();
	}
	return 0;
}

void tb_on_ccc(uint16_t handle, uint16_t value)
{
	if (handle == TB_MATTER_H_C2 && (value & 2u)) {
		send_hs();
	}
}

void tb_on_indicated(int err)
{
	LOG_DBG("C2 confirmed: %d", err);
	s_busy = false;
	if (err) {
		tx_done(-EIO);
		return;
	}
	(void)pump_tx();
	send_ack();
}

/* Every link, not only the one that writes C1: a peer that connects and never
 * writes shows up nowhere else. The address as bt_addr_le_to_str() prints it. */
void tb_on_connect(const uint8_t peer[7])
{
	LOG_INF("BLE connected: %02X:%02X:%02X:%02X:%02X:%02X (%s)", peer[6], peer[5], peer[4],
		peer[3], peer[2], peer[1], (peer[0] & 1u) ? "random" : "public");
	s_linked = true;
}

void tb_on_disconnect(uint8_t reason)
{
	LOG_INF("BLE disconnected (0x%02x)", reason);
	s_linked = false;
	s_busy = false;
	if (s_claimed) {
		s_claimed = false;
		reset_link();
	}
	/* tinyble advertises again as last asked: matter_ble.h's "repeats the
	 * last request". Asking tinytalk again here judged the advert while a
	 * fail-safe fabric still existed, stopped it, and nothing restarted it
	 * when the fail-safe expired. */
}

/* ---- matter_ble.h ----------------------------------------------------------- */

int matter_ble_send(const uint8_t *msg, size_t len)
{
	int rc;

	if (msg == NULL || len == 0u) {
		return -EINVAL;
	}
	tb_zephyr_lock();
	if (!s_handshaked) {
		rc = -ENOTCONN;
	} else if (s_tx_active) {
		rc = -EBUSY;
	} else if (matter_btp_tx_init(&s_tx, msg, len, s_frag_size, s_tx_seq) != MATTER_OK) {
		rc = -EINVAL;
	} else {
		s_tx_active = true;
		rc = pump_tx();
		/* A refusal is returned here and never also called back. */
		s_tx_notifies = rc == 0;
	}
	tb_zephyr_unlock();
	return rc;
}

void matter_ble_set_link_handler(matter_ble_link_cb cb)
{
	s_link_cb = cb;
}

void matter_ble_set_tx_handler(matter_ble_tx_cb cb)
{
	s_tx_cb = cb;
}

void matter_ble_set_msg_handler(matter_ble_msg_cb cb)
{
	s_msg_cb = cb;
}

void matter_ble_set_discriminator(uint16_t discriminator)
{
	s_discriminator = discriminator;
}

uint16_t matter_ble_discriminator(void)
{
	return s_discriminator ? s_discriminator : (uint16_t)CONFIG_TINYTALK_DISCRIMINATOR;
}

/* The service stays in the table; off with a link up says so (tb_tinytalk.h). */
int matter_ble_advertise(bool on)
{
	uint16_t d = matter_ble_discriminator();
	int rc;

	tb_zephyr_lock();
	s_ad[8] = (uint8_t)d;
	s_ad[9] = (uint8_t)((d >> 8) & 0x0Fu);
	tb_adv(s_ad, on ? sizeof(s_ad) : 0u);
	rc = !on && s_linked ? -EBUSY : 0;
	tb_zephyr_unlock();
	return rc;
}

int matter_ble_start(void)
{
	/* The FICR device address, as a static random address. */
	uint32_t lo = NRF_FICR->DEVICEADDR[0], hi = NRF_FICR->DEVICEADDR[1];
	uint8_t addr[6] = {(uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)(lo >> 16),
			   (uint8_t)(lo >> 24), (uint8_t)hi, (uint8_t)((hi >> 8) | 0xC0u)};
	int rc;

	if (s_started) {
		return 0;
	}
	rc = tb_zephyr_open();
	if (rc == 0) {
		tb_init(addr);
		rc = tb_zephyr_start();
	}
	s_started = rc == 0;
	return rc;
}
