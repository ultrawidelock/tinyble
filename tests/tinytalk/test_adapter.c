/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Host test for port/tinytalk/matter_ble_tinyble.c. A scripted phone runs
 * BTP against it through tinyble and a fake controller; the phone's side
 * uses tinytalk's own BTP codec. Zephyr is stubbed (stub/), and the system
 * work queue is run by hand.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <soc.h>
#include <zephyr/kernel.h>

#include <tinyble/tb_matter.h>
#include <tinyble/tb_tinytalk.h>
#include <tinyble/tb_zephyr.h>
#include <tinyble/tinyble.h>
#include <tinytalk/matter_btp.h>
#include <tinytalk/matter_commission.h>

static int g_fail, g_pass;

#define CHECK(cond)                                                                   \
	do {                                                                          \
		if (cond) {                                                           \
			g_pass++;                                                     \
		} else {                                                              \
			g_fail++;                                                     \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
		}                                                                     \
	} while (0)

/* ---- fake controller and Zephyr ---------------------------------------------- */

#define LOG_MAX 64

static struct {
	uint8_t type;
	uint16_t len;
	uint8_t b[300];
} g_log[LOG_MAX];
static int g_head, g_tail;
static uint8_t g_buf[300];
static uint8_t g_buf_type;

uint8_t *tb_port_tx_alloc(uint8_t type, uint16_t len)
{
	if (len > sizeof(g_buf)) {
		return NULL;
	}
	g_buf_type = type;
	return g_buf;
}

void tb_port_tx_send(uint8_t *buf, uint16_t len)
{
	CHECK(g_tail - g_head < LOG_MAX);
	g_log[g_tail % LOG_MAX].type = g_buf_type;
	g_log[g_tail % LOG_MAX].len = len;
	memcpy(g_log[g_tail % LOG_MAX].b, buf, len);
	g_tail++;
}

uint32_t tb_port_ms(void)
{
	return 1000u;
}

struct nrf_ficr g_ficr = {{0x44332211u, 0x00006655u}};
static int g_locked;

int tb_zephyr_open(void)
{
	return 0;
}

int tb_zephyr_start(void)
{
	return 0;
}

void tb_zephyr_lock(void)
{
	g_locked++;
}

void tb_zephyr_unlock(void)
{
	g_locked--;
}

static struct k_work *g_work[8];
static int g_nwork;

int k_work_submit(struct k_work *work)
{
	for (int i = 0; i < g_nwork; i++) {
		if (g_work[i] == work) {
			return 0;
		}
	}
	g_work[g_nwork++] = work;
	return 1;
}

static int run_work(void)
{
	int n = 0;

	while (g_nwork) {
		struct k_work *w = g_work[0];

		memmove(g_work, g_work + 1, (size_t)--g_nwork * sizeof(g_work[0]));
		w->handler(w);
		n++;
	}
	return n;
}

/* ---- tinytalk's side ------------------------------------------------------------- */

static uint8_t g_msg[1024];
static size_t g_msg_len;
static int g_msgs, g_links, g_readv, g_txs, g_tx_status, g_seq;
static int g_link_at, g_tx_at; /* call order: g_seq at the last call */
static size_t g_reply_len; /* sent back from the message handler */
static uint8_t g_reply[600];

static void on_msg(uint8_t *msg, size_t len)
{
	CHECK(g_locked == 0);
	memcpy(g_msg, msg, len);
	g_msg_len = len;
	g_msgs++;
	if (g_reply_len) {
		CHECK(matter_ble_send(g_reply, g_reply_len) == 0);
	}
}

static void on_link(void)
{
	CHECK(g_locked == 0);
	g_links++;
	g_link_at = ++g_seq;
}

static void on_tx(int status)
{
	CHECK(g_locked == 0);
	g_txs++;
	g_tx_status = status;
	g_tx_at = ++g_seq;
}

void matter_commission_readvertise(void)
{
	g_readv++;
}

/* ---- HCI helpers ------------------------------------------------------------- */

static uint8_t g_adv[31];
static uint8_t g_adv_len;

static void cc(uint16_t op, const uint8_t *ret, uint8_t n)
{
	uint8_t b[16] = {0x0E, (uint8_t)(4u + n), 1, (uint8_t)op, (uint8_t)(op >> 8), 0};

	memcpy(b + 6, ret, n);
	tb_hci_rx(TB_HCI_EVT, b, (uint16_t)(6u + n));
}

/* Answer every command at the head of the log; LE Read Buffer Size reports
 * 251 B and 3 buffers. Returns how many. */
static int answer_cmds(void)
{
	static const uint8_t k_buf[] = {251, 0, 3};
	int n = 0;

	while (g_head != g_tail && g_log[g_head % LOG_MAX].type == TB_HCI_CMD) {
		uint8_t *b = g_log[g_head++ % LOG_MAX].b;
		uint16_t op = (uint16_t)(b[0] | b[1] << 8);

		if (op == 0x2008) {
			g_adv_len = b[3];
			memcpy(g_adv, b + 4, sizeof(g_adv));
		}
		if (op == 0x0406) {
			continue; /* Disconnect: no Command Complete */
		}
		cc(op, k_buf, op == 0x2002 ? 3 : 0);
		n++;
	}
	return n;
}

static void evt(const char *hex)
{
	uint8_t b[64];
	unsigned int v;
	uint16_t n = 0;

	for (; *hex; hex += 2) {
		if (sscanf(hex, "%2x", &v) != 1) {
			abort();
		}
		b[n++] = (uint8_t)v;
	}
	tb_hci_rx(TB_HCI_EVT, b, n);
}

static void connect(void)
{
	evt("3e1301000100010" "1aabbccddeeff18000000480000");
}

static void att_in(const uint8_t *pdu, uint16_t n)
{
	uint8_t b[300] = {0x01, 0x20, (uint8_t)(n + 4u), (uint8_t)((n + 4u) >> 8),
			  (uint8_t)n, (uint8_t)(n >> 8), 0x04, 0x00};

	memcpy(b + 8, pdu, n);
	tb_hci_rx(TB_HCI_ACL, b, (uint16_t)(n + 8u));
}

/* Pop the next ACL packet's ATT PDU into @p out and complete the packet.
 * Returns its length, or 0 if the head is not one. */
static uint16_t att_out(uint8_t *out)
{
	static const uint8_t k_nocp[] = {0x13, 0x05, 0x01, 0x01, 0x00, 0x01, 0x00};
	uint16_t n;

	answer_cmds();
	if (g_head == g_tail || g_log[g_head % LOG_MAX].type != TB_HCI_ACL) {
		return 0;
	}
	n = (uint16_t)(g_log[g_head % LOG_MAX].len - 8u);
	CHECK(g_log[g_head % LOG_MAX].b[6] == 0x04);
	memcpy(out, g_log[g_head % LOG_MAX].b + 8, n);
	g_head++;
	tb_hci_rx(TB_HCI_EVT, k_nocp, sizeof(k_nocp));
	return n;
}

static void write_c1(const uint8_t *v, uint16_t n)
{
	uint8_t pdu[260] = {0x12, (uint8_t)TB_MATTER_H_C1, 0x00};

	memcpy(pdu + 3, v, n);
	att_in(pdu, (uint16_t)(n + 3u));
}

/* ---- the phone ------------------------------------------------------------------- */

#define MTU 185u /* what the traced iPhone asked for */

static struct matter_btp_rx g_prx;
static uint8_t g_prx_buf[1024];
static uint8_t g_pseq;      /* the phone's next transmit sequence */
static int g_rsp, g_ind;    /* Write Responses and indications seen */
static uint8_t g_rsp_err;   /* the last Error Response's code */
static uint8_t g_hs[8];     /* the handshake response */
static int g_hs_len;
static int g_got;           /* messages the phone reassembled */
static int g_hold;          /* do not confirm indications */

/* Take everything sent: count responses, feed indications to the phone and
 * confirm them. Then run the work queue, and again until nothing moves. */
static void settle(void)
{
	static const uint8_t k_cfm[] = {0x1E};
	uint8_t pdu[260];
	uint16_t n;

	do {
		while ((n = att_out(pdu)) != 0) {
			if (pdu[0] == 0x13) {
				g_rsp++;
			} else if (pdu[0] == 0x01) {
				g_rsp_err = pdu[4];
			} else if (pdu[0] == 0x1D) {
				CHECK(pdu[1] == TB_MATTER_H_C2 && pdu[2] == 0);
				CHECK(n - 3u <= MTU - 3u);
				g_ind++;
				if (g_hs_len < 0) {
					g_hs_len = n - 3;
					memcpy(g_hs, pdu + 3, (size_t)g_hs_len);
				} else {
					int rc = matter_btp_rx_fragment(&g_prx, pdu + 3, n - 3u);

					CHECK(rc == MATTER_OK || rc == MATTER_END);
					if (rc == MATTER_END) {
						g_got++;
						CHECK(g_prx.len == g_reply_len &&
						      !memcmp(g_prx_buf, g_reply, g_reply_len));
						matter_btp_rx_reset(&g_prx);
					}
				}
				if (!g_hold) {
					att_in(k_cfm, 1);
				}
			}
		}
	} while (run_work());
}

static void handshake_write(void)
{
	struct matter_btp_handshake_req req = {.versions = {4}, .mtu = MTU, .window_size = 4};
	uint8_t b[MATTER_BTP_REQ_LEN];
	size_t n = 0;

	CHECK(matter_btp_req_encode(&req, b, sizeof(b), &n) == MATTER_OK);
	g_hs_len = -1;
	write_c1(b, (uint16_t)n);
}

static void subscribe(void)
{
	static const uint8_t k_ccc[] = {0x12, (uint8_t)TB_MATTER_H_C2_CCC, 0x00, 0x02, 0x00};

	att_in(k_ccc, sizeof(k_ccc));
}

/* Send @p len bytes as BTP fragments of the negotiated size. */
static void phone_send(const uint8_t *msg, size_t len)
{
	struct matter_btp_tx tx;
	uint8_t frag[MATTER_BTP_MAX_FRAGMENT];
	size_t n = 0;

	CHECK(matter_btp_tx_init(&tx, msg, len, MTU - 3u, g_pseq) == MATTER_OK);
	while (matter_btp_tx_next(&tx, NULL, frag, sizeof(frag), &n) == MATTER_OK) {
		write_c1(frag, (uint16_t)n);
		settle();
	}
	g_pseq = tx.next_seq;
}

/* Up, advertising, connected at MTU 185, nothing else. */
static void start(void)
{
	static const uint8_t k_mtu[] = {0x02, MTU, 0x00};
	uint8_t pdu[8];

	g_head = g_tail = 0;
	g_msgs = g_links = g_readv = g_txs = g_seq = g_rsp = g_ind = g_got = g_hold = 0;
	g_rsp_err = 0;
	g_reply_len = 0;
	g_pseq = 0;
	matter_btp_rx_init(&g_prx, g_prx_buf, sizeof(g_prx_buf), 1u);
	matter_ble_set_msg_handler(on_msg);
	matter_ble_set_link_handler(on_link);
	matter_ble_set_tx_handler(on_tx);
	CHECK(matter_ble_start() == 0);
	CHECK(answer_cmds() == 6);
	matter_ble_advertise(true);
	CHECK(answer_cmds() == 2);
	connect();
	att_in(k_mtu, sizeof(k_mtu));
	CHECK(att_out(pdu) == 3 && pdu[0] == 0x03);
	CHECK(tb_mtu() == MTU);
	CHECK(g_locked == 0);
}

/* ---- tests ------------------------------------------------------------------------ */

/* The advert: Flags, then 0xFFF6 Service Data with the discriminator. */
static void test_advert(void)
{
	static const uint8_t k_want[] = {0x02, 0x01, 0x06, 0x0B, 0x16, 0xF6, 0xFF, 0x00,
					 0x00, 0x0F, 0xF1, 0xFF, 0x01, 0x80, 0x00};

	start();
	CHECK(g_adv_len == sizeof(k_want) && !memcmp(g_adv, k_want, sizeof(k_want)));
	matter_ble_set_discriminator(0x123);
	CHECK(matter_ble_discriminator() == 0x123);
	matter_ble_advertise(true);
	answer_cmds();
	CHECK(g_adv[8] == 0x23 && g_adv[9] == 0x01);
	matter_ble_set_discriminator(0);
	matter_ble_advertise(false);
	answer_cmds();
	evt("0504000100" "13");
	settle();
}

/* The traced iPhone's order: handshake write, then subscribe, then PASE. */
static void test_write_then_subscribe(void)
{
	struct matter_btp_handshake_resp resp;
	uint8_t msg[400];

	start();
	CHECK(matter_ble_send(msg, 1) == -ENOTCONN);
	handshake_write();
	settle();
	CHECK(g_rsp == 1 && g_ind == 0); /* nothing to indicate on yet */
	CHECK(g_links == 1);             /* the link was claimed */
	subscribe();
	settle();
	CHECK(g_rsp == 2 && g_ind == 1);
	CHECK(matter_btp_resp_decode(g_hs, (size_t)g_hs_len, &resp) == MATTER_OK);
	CHECK(resp.version == 4 && resp.fragment_size == MTU - 3u && resp.window_size == 4);

	/* 400 B in three fragments, acked after two and at the end; the reply
	 * goes out as 300 B in two. */
	for (size_t i = 0; i < sizeof(msg); i++) {
		msg[i] = (uint8_t)i;
	}
	for (size_t i = 0; i < 300u; i++) {
		g_reply[i] = (uint8_t)(i * 7u);
	}
	g_reply_len = 300;
	phone_send(msg, sizeof(msg));
	CHECK(g_msgs == 1 && g_msg_len == sizeof(msg) && !memcmp(g_msg, msg, sizeof(msg)));
	CHECK(g_rsp == 5);
	CHECK(g_ind == 1 + 2 + 2); /* handshake, two standalone acks, two fragments */
	CHECK(g_got == 1);
	CHECK(g_txs == 1 && g_tx_status == 0);
	CHECK(matter_ble_send(g_reply, 0) == -EINVAL);

	/* The link drops: tinytalk hears it, and is asked to readvertise. */
	evt("0504000100" "13");
	settle();
	CHECK(g_links == 2 && g_readv == 1);
	CHECK(matter_ble_send(msg, 1) == -ENOTCONN);
}

/* Subscribed first: the response follows the handshake's Write Response. */
static void test_subscribe_then_write(void)
{
	uint8_t pdu[8];

	start();
	subscribe();
	settle();
	CHECK(g_rsp == 1 && g_ind == 0 && g_links == 0);
	handshake_write();
	CHECK(att_out(pdu) == 1 && pdu[0] == 0x13); /* the response first */
	settle();
	CHECK(g_ind == 1 && g_hs_len == 6);
	evt("0504000100" "13");
	settle();
}

/* A send in flight when the link drops completes once, before the link
 * callback, and a second send meanwhile is refused. */
static void test_drop_mid_send(void)
{
	start();
	handshake_write();
	subscribe();
	settle();
	for (size_t i = 0; i < 500u; i++) {
		g_reply[i] = (uint8_t)i;
	}
	g_hold = 1;
	CHECK(matter_ble_send(g_reply, 500) == 0);
	settle();
	CHECK(g_ind == 2 && g_txs == 0);
	CHECK(matter_ble_send(g_reply, 10) == -EBUSY);
	evt("0504000100" "08");
	settle();
	CHECK(g_txs == 1 && g_tx_status != 0);
	CHECK(g_links == 2 && g_tx_at < g_link_at);
}

/* A fragment out of sequence: Value Not Allowed, and the link is dropped. */
static void test_desync(void)
{
	static const uint8_t k_bad[] = {0x05, 0x07, 0x02, 0x00, 0xAA, 0xBB};
	uint8_t *b;

	start();
	handshake_write();
	subscribe();
	settle();
	write_c1(k_bad, sizeof(k_bad));
	CHECK(g_head != g_tail);
	b = g_log[g_head % LOG_MAX].b;
	CHECK(g_log[g_head % LOG_MAX].type == TB_HCI_CMD && b[0] == 0x06 && b[1] == 0x04 &&
	      b[5] == 0x13);
	g_head++;
	settle();
	CHECK(g_rsp_err == 0x13);
	CHECK(g_msgs == 0);
	evt("0504000100" "16");
	settle();
}

int main(void)
{
	test_advert();
	test_write_then_subscribe();
	test_subscribe_then_write();
	test_drop_mid_send();
	test_desync();
	printf("%s %d\n", g_fail ? "FAIL" : "PASS", g_pass);
	return g_fail != 0;
}
