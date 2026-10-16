/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Host tests for tinyble against a fake controller. The port records every
 * packet tinyble sends; tests feed events and ACL and compare bytes.
 *
 * Expected ATT replies are derived by hand from Zephyr's att.c rules (see
 * src/tinyble.c) on the Matter table, not recorded from tinyble.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyble/tb_matter.h>
#include <tinyble/tinyble.h>

/* ---- harness --------------------------------------------------------------- */

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

/* For loops: counts failures only. */
#define CHECK_QUIET(cond)                                                             \
	do {                                                                          \
		if (!(cond)) {                                                        \
			g_fail++;                                                     \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
		}                                                                     \
	} while (0)

static size_t unhex(uint8_t *out, const char *hex)
{
	size_t n = 0;
	unsigned int b;

	while (*hex) {
		if (*hex == ' ') {
			hex++;
			continue;
		}
		if (sscanf(hex, "%2x", &b) != 1) {
			abort();
		}
		out[n++] = (uint8_t)b;
		hex += 2;
	}
	return n;
}

static void tohex(char *out, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		snprintf(out + 2 * i, 3, "%02x", b[i]);
	}
	out[2 * n] = 0;
}

/* ---- fake port ------------------------------------------------------------- */

#define LOG_MAX 64

static struct {
	uint8_t type;
	uint16_t len;
	uint8_t b[300];
} g_log[LOG_MAX];
static int g_head, g_tail;
static uint8_t g_buf[300];
static uint8_t g_buf_type;
static uint16_t g_buf_cap;
static int g_no_bufs;
static uint32_t g_now = 1000;
/* The controller completes each ACL packet a test consumes. */
static int g_auto_nocp = 1;
static void nocp(unsigned int n);

uint8_t *tb_port_tx_alloc(uint8_t type, uint16_t len)
{
	if (g_no_bufs || len > sizeof(g_buf)) {
		return NULL;
	}
	g_buf_type = type;
	g_buf_cap = len;
	memset(g_buf, 0xEE, sizeof(g_buf));
	return g_buf;
}

void tb_port_tx_send(uint8_t *buf, uint16_t len)
{
	CHECK(buf == g_buf && len <= g_buf_cap);
	CHECK(g_tail - g_head < LOG_MAX);
	g_log[g_tail % LOG_MAX].type = g_buf_type;
	g_log[g_tail % LOG_MAX].len = len;
	memcpy(g_log[g_tail % LOG_MAX].b, buf, len);
	g_tail++;
}

uint32_t tb_port_ms(void)
{
	return g_now;
}

/* ---- hooks ----------------------------------------------------------------- */

static int g_ready = 1;
static uint8_t g_write_err;
static uint16_t g_write_h, g_write_len;
static uint8_t g_write_data[256];
static int g_writes;
static uint16_t g_ccc_h, g_ccc_v;
static int g_cccs;
static int g_ccc_seen_rsp; /* the Write Response was out before tb_on_ccc */
static int g_indicate_on_ccc;
static int g_ind_err, g_inds;
static uint8_t g_disc_reason;
static int g_discs;
static int g_order; /* tb_on_indicated before tb_on_disconnect */

void tb_on_ready(int err)
{
	g_ready = err;
}

uint8_t tb_on_write(uint16_t handle, const uint8_t *data, uint16_t len)
{
	g_write_h = handle;
	g_write_len = len;
	memcpy(g_write_data, data, len);
	g_writes++;
	return g_write_err;
}

static const uint8_t k_hs[] = {0x65, 0x6C, 0x04};

void tb_on_ccc(uint16_t handle, uint16_t value)
{
	g_ccc_h = handle;
	g_ccc_v = value;
	g_cccs++;
	g_ccc_seen_rsp = g_tail > g_head && g_log[(g_tail - 1) % LOG_MAX].b[8] == 0x13;
	if (g_indicate_on_ccc) {
		CHECK(tb_indicate(handle, k_hs, sizeof(k_hs)) == 0);
	}
}

void tb_on_indicated(int err)
{
	g_ind_err = err;
	g_inds++;
	g_order = 1;
}

void tb_on_disconnect(uint8_t reason)
{
	g_disc_reason = reason;
	g_discs++;
	g_order = g_order == 1 ? 2 : 0;
}

/* ---- helpers --------------------------------------------------------------- */

/* Pop the next sent packet and compare it with @p hex. */
#define EXPECT(type, hex) expect_(type, hex, __LINE__)
static void expect_(uint8_t type, const char *hex, int line)
{
	uint8_t want[300];
	char got[620];
	size_t n = unhex(want, hex);

	if (g_head == g_tail) {
		g_fail++;
		printf("FAIL line %d: nothing sent, want %s\n", line, hex);
		return;
	}
	if (g_log[g_head % LOG_MAX].type != type || g_log[g_head % LOG_MAX].len != n ||
	    memcmp(g_log[g_head % LOG_MAX].b, want, n)) {
		g_fail++;
		tohex(got, g_log[g_head % LOG_MAX].b, g_log[g_head % LOG_MAX].len);
		printf("FAIL line %d: type %u\n  got  %s\n  want %s\n", line,
		       g_log[g_head % LOG_MAX].type, got, hex);
	} else {
		g_pass++;
	}
	g_head++;
	if (g_auto_nocp && type == TB_HCI_ACL) {
		nocp(1);
	}
}

#define EXPECT_NONE() expect_none_(__LINE__)
static void expect_none_(int line)
{
	char got[620];

	if (g_head != g_tail) {
		g_fail++;
		tohex(got, g_log[g_head % LOG_MAX].b, g_log[g_head % LOG_MAX].len);
		printf("FAIL line %d: unexpected %s\n", line, got);
		g_head = g_tail;
	} else {
		g_pass++;
	}
}

static void rx(uint8_t type, const char *hex)
{
	uint8_t b[300];

	tb_hci_rx(type, b, (uint16_t)unhex(b, hex));
}

/* Command Complete for @p op with status 0 and return parameters @p ret. */
static void cc(uint16_t op, const char *ret)
{
	uint8_t b[64];
	size_t n = unhex(b + 6, ret);

	b[0] = 0x0E;
	b[1] = (uint8_t)(4 + n);
	b[2] = 1;
	b[3] = (uint8_t)op;
	b[4] = (uint8_t)(op >> 8);
	b[5] = 0;
	tb_hci_rx(TB_HCI_EVT, b, (uint16_t)(6 + n));
}

/* An L2CAP PDU on @p cid from the peer, handle 1. */
static void l2(uint16_t cid, const char *hex)
{
	uint8_t b[300];
	size_t n = unhex(b + 8, hex);

	b[0] = 0x01;
	b[1] = 0x20; /* handle 1, PB 10 */
	b[2] = (uint8_t)(n + 4);
	b[3] = (uint8_t)((n + 4) >> 8);
	b[4] = (uint8_t)n;
	b[5] = (uint8_t)(n >> 8);
	b[6] = (uint8_t)cid;
	b[7] = (uint8_t)(cid >> 8);
	tb_hci_rx(TB_HCI_ACL, b, (uint16_t)(n + 8));
}

#define ATT_IN(hex)       l2(0x0004, hex)
#define ATT_OUT(hex)      EXPECT(TB_HCI_ACL, att_frame(hex))
#define SIG_OUT(hex)      EXPECT(TB_HCI_ACL, frame(0x0005, hex))

static const char *frame(uint16_t cid, const char *hex)
{
	static char out[700];
	uint8_t b[300];
	size_t n = unhex(b, hex);

	snprintf(out, sizeof(out), "0100%02x%02x%02x%02x%02x%02x", (unsigned)((n + 4) & 0xFF),
		(unsigned)((n + 4) >> 8), (unsigned)(n & 0xFF), (unsigned)(n >> 8),
		(unsigned)(cid & 0xFF), (unsigned)(cid >> 8));
	tohex(out + 16, b, n);
	return out;
}

static const char *att_frame(const char *hex)
{
	return frame(0x0004, hex);
}

static void nocp(unsigned int n)
{
	uint8_t b[] = {0x13, 0x05, 0x01, 0x01, 0x00, (uint8_t)n, 0x00};

	tb_hci_rx(TB_HCI_EVT, b, sizeof(b));
}

static const uint8_t k_addr[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0xC6};
/* Flags 0x06, then Service Data 0xFFF6 with an 8 B commissionable payload. */
static const uint8_t k_ad[] = {0x02, 0x01, 0x06, 0x0B, 0x16, 0xF6, 0xFF, 0x00,
			       0x00, 0x0F, 0xF1, 0xFF, 0x01, 0x80, 0x00};

static void up(unsigned int acl_len, unsigned int credits)
{
	char ret[16];

	g_head = g_tail = 0;
	g_ready = 1;
	tb_init(k_addr);
	EXPECT(TB_HCI_CMD, "010c08 1000000000000020");
	cc(0x0C01, "");
	EXPECT(TB_HCI_CMD, "012008 0100000000000000");
	cc(0x2001, "");
	EXPECT(TB_HCI_CMD, "022000");
	snprintf(ret, sizeof(ret), "%02x%02x%02x", acl_len & 0xFF, acl_len >> 8, credits);
	cc(0x2002, ret);
	EXPECT(TB_HCI_CMD, "052006 1122334455c6");
	cc(0x2005, "");
	EXPECT(TB_HCI_CMD, "06200f a000 f000 00 01 00 000000000000 07 00");
	cc(0x2006, "");
	CHECK(g_ready == 0);
	EXPECT_NONE();
}

static void connect(void)
{
	rx(TB_HCI_EVT, "3e13 01 00 0100 01 01 aabbccddeeff 1800 0000 4800 00");
	EXPECT_NONE();
}

/* Up, advertising, connected, MTU 247, subscribed to C2. */
static void ready_link(void)
{
	up(251, 3);
	connect();
	ATT_IN("02 0502");
	ATT_OUT("03 f700");
	ATT_IN("12 1100 0200");
	ATT_OUT("13");
}

/* ---- tests ----------------------------------------------------------------- */

static void test_init_and_adv(void)
{
	up(251, 3);
	CHECK(tb_mtu() == 23);

	tb_adv(k_ad, sizeof(k_ad));
	EXPECT(TB_HCI_CMD, "082020 0f 020106 0b16f6ff00000ff1ff018000 00000000000000000000000000000000");
	cc(0x2008, "");
	EXPECT(TB_HCI_CMD, "0a2001 01");
	cc(0x200A, "");
	EXPECT_NONE();

	/* The same data again re-sends the data, not the enable. */
	tb_adv(k_ad, sizeof(k_ad));
	EXPECT(TB_HCI_CMD, "082020 0f 020106 0b16f6ff00000ff1ff018000 00000000000000000000000000000000");
	cc(0x2008, "");
	EXPECT_NONE();

	/* Connecting stops legacy advertising in the controller; the link
	 * going down starts it again. */
	connect();
	rx(TB_HCI_EVT, "0504 00 0100 13");
	CHECK(g_discs == 1 && g_disc_reason == 0x13);
	EXPECT(TB_HCI_CMD, "0a2001 01");
	cc(0x200A, "");

	tb_adv(NULL, 0);
	EXPECT(TB_HCI_CMD, "0a2001 00");
	cc(0x200A, "");
	EXPECT_NONE();

	/* No address: public, and no Set Random Address. */
	g_head = g_tail = 0;
	tb_init(NULL);
	EXPECT(TB_HCI_CMD, "010c08 1000000000000020");
	cc(0x0C01, "");
	EXPECT(TB_HCI_CMD, "012008 0100000000000000");
	cc(0x2001, "");
	EXPECT(TB_HCI_CMD, "022000");
	cc(0x2002, "1b0002");
	EXPECT(TB_HCI_CMD, "06200f a000 f000 00 00 00 000000000000 07 00");
	cc(0x2006, "");
	CHECK(g_ready == 0);

	/* A failing init command is reported and stops the sequence. */
	g_head = g_tail = 0;
	tb_init(NULL);
	EXPECT(TB_HCI_CMD, "010c08 1000000000000020");
	rx(TB_HCI_EVT, "0e04 01 010c 12");
	CHECK(g_ready == -0x12);
	tb_adv(k_ad, sizeof(k_ad));
	EXPECT_NONE();

	/* A NOP Command Complete grants the command credit. */
	g_head = g_tail = 0;
	tb_init(NULL);
	EXPECT(TB_HCI_CMD, "010c08 1000000000000020");
	rx(TB_HCI_EVT, "0e04 00 010c 00"); /* done, but no credit */
	EXPECT_NONE();
	rx(TB_HCI_EVT, "0e03 01 0000");
	EXPECT(TB_HCI_CMD, "012008 0100000000000000");
}

static void test_discovery(void)
{
	up(251, 3);
	connect();
	CHECK(tb_mtu() == 23);
	ATT_IN("02 0502"); /* client MTU 517 */
	ATT_OUT("03 f700");
	CHECK(tb_mtu() == 247);

	/* Primary services. */
	ATT_IN("10 0100 ffff 0028");
	ATT_OUT("11 06 0100 0400 0118 0500 0b00 0018 0c00 1100 f6ff");
	ATT_IN("10 1200 ffff 0028");
	ATT_OUT("01 10 1200 0a");

	/* Characteristics: 16-bit declarations, then the 128-bit pair. */
	ATT_IN("08 0100 ffff 0328");
	ATT_OUT("09 07 0200 200300052a 0600 020700002a 0800 020900012a 0a00 020b00042a");
	ATT_IN("08 0b00 ffff 0328");
	ATT_OUT("09 15 0d00 0c0e00 119d9f429c4f9f9559453d26f52eee18"
		"      0f00 201000 129d9f429c4f9f9559453d26f52eee18");
	ATT_IN("08 1000 ffff 0328");
	ATT_OUT("01 08 1000 0a");

	/* Descriptors; a format change ends a reply. */
	ATT_IN("04 1100 1100");
	ATT_OUT("05 01 1100 0229");
	ATT_IN("04 0300 0400");
	ATT_OUT("05 01 0300 052a 0400 0229");
	ATT_IN("04 0e00 1100");
	ATT_OUT("05 02 0e00 119d9f429c4f9f9559453d26f52eee18");
	ATT_IN("04 1200 ffff");
	ATT_OUT("01 04 1200 0a");

#if TB_FIND_BY_TYPE
	/* Find By Type Value: the 0xFFF6 service and its group end. */
	ATT_IN("06 0100 ffff 0028 f6ff");
	ATT_OUT("07 0c00 1100");
	ATT_IN("06 0100 0d00 0028 f6ff"); /* group end bounded by the request */
	ATT_OUT("07 0c00 0d00");
	ATT_IN("06 0100 ffff 0028 3412");
	ATT_OUT("01 06 0100 0a");
	ATT_IN("06 0100 ffff 0328 f6ff");
	ATT_OUT("01 06 0100 0a");
#else
	ATT_IN("06 0100 ffff 0028 f6ff");
	ATT_OUT("01 06 0000 06");
#endif

	/* GAP values. */
	ATT_IN("0a 0700");
	ATT_OUT("0b 74696e7974616c6b");
	ATT_IN("0a 0900");
	ATT_OUT("0b 0000");
	ATT_IN("0a 0b00");
	ATT_OUT("0b 0c001800 0000c800");
	ATT_IN("08 0100 ffff 002a"); /* Device Name by type */
	ATT_OUT("09 0a 0700 74696e7974616c6b");
#if TB_READ_BLOB
	ATT_IN("0c 0700 0400");
	ATT_OUT("0d 74616c6b");
	ATT_IN("0c 0700 0800");
	ATT_OUT("0d");
#else
	ATT_IN("0c 0700 0400");
	ATT_OUT("01 0c 0000 06");
#endif
	EXPECT_NONE();
}

static void test_mtu23_fill(void)
{
	/* Zephyr adds a list item only while more than one item's room is left,
	 * so a reply can stop one item short of full. */
	up(251, 3);
	connect();
	ATT_IN("08 0100 ffff 0328");
	ATT_OUT("09 07 0200 200300052a 0600 020700002a");
	ATT_IN("04 0100 ffff");
	ATT_OUT("05 01 0100 0028 0200 0328 0300 052a 0400 0229 0500 0028");
	ATT_IN("10 0100 ffff 0028");
	ATT_OUT("11 06 0100 0400 0118 0500 0b00 0018 0c00 1100 f6ff");
	/* A 128-bit declaration item is 21 B, which fits once. */
	ATT_IN("08 0c00 ffff 0328");
	ATT_OUT("09 15 0d00 0c0e00 119d9f429c4f9f9559453d26f52eee18");
	/* Reads truncate to MTU - 1. */
	EXPECT_NONE();
}

static void test_errors(void)
{
	up(251, 3);
	connect();
	ATT_IN("0a 1000"); /* C2 value: not readable */
	ATT_OUT("01 0a 1000 02");
	ATT_IN("0a 2000");
	ATT_OUT("01 0a 2000 01");
	ATT_IN("0a 0000");
	ATT_OUT("01 0a 0000 01");
	ATT_IN("0a 07");
	ATT_OUT("01 0a 0000 04");
#if TB_READ_BLOB
	ATT_IN("0c 0700 0900");
	ATT_OUT("01 0c 0700 07");
#endif
	ATT_IN("12 0700 41");
	ATT_OUT("01 12 0700 03");
	ATT_IN("12 2000 41");
	ATT_OUT("01 12 2000 01");
	ATT_IN("12 1100 020000");
	ATT_OUT("01 12 1100 0d");
	ATT_IN("12 1100");
	ATT_OUT("01 12 1100 0d");
	ATT_IN("10 0100 ffff 0328");
	ATT_OUT("01 10 0100 10");
	ATT_IN("10 0100 ffff 00");
	ATT_OUT("01 10 0000 04");
	ATT_IN("16 0e00 0000 41");
	ATT_OUT("01 16 0000 06");
	ATT_IN("0e 0700 0900");
	ATT_OUT("01 0e 0000 06");
	ATT_IN("0b 00"); /* a response the server never asked for */
	ATT_OUT("01 0b 0000 06");
	ATT_IN("02 1600");
	ATT_OUT("01 02 0000 04");
	ATT_IN("04 0500 0100");
	ATT_OUT("01 04 0500 01");
	ATT_IN("04 0000 0500");
	ATT_OUT("01 04 0000 01");
	ATT_IN("08 0100 ffff 119d9f429c4f9f9559453d26f52eee18"); /* C1 value by type */
	ATT_OUT(TB_TYPE128 ? "01 08 0100 02" : "01 08 0100 0a");
	ATT_IN("08 0100 ffff 0429");
	ATT_OUT("01 08 0100 0a");
	/* Commands and stray confirmations get nothing. */
	ATT_IN("60 0100");
	ATT_IN("1e");
	ATT_IN("52 0700 41");
	ATT_IN("");
	EXPECT_NONE();
	CHECK(g_inds == 0);
}

static void test_writes_and_indications(void)
{
	g_writes = 0;
	g_cccs = 0;
	g_inds = 0;
	up(251, 3);
	connect();
	ATT_IN("02 0502");
	ATT_OUT("03 f700");

	/* Not subscribed yet. */
	CHECK(tb_indicate(TB_MATTER_H_C2, k_hs, 3) == TB_EAGAIN);
	CHECK(tb_indicate(0, k_hs, 3) == TB_EAGAIN);
	CHECK(tb_indicate(0x0011, k_hs, 3) == TB_EAGAIN);

	/* Subscribing: the Write Response goes before the hook, so a reply
	 * indicated from the hook comes after it. */
	g_indicate_on_ccc = 1;
	ATT_IN("12 1100 0200");
	g_indicate_on_ccc = 0;
	CHECK(g_cccs == 1 && g_ccc_h == TB_MATTER_H_C2 && g_ccc_v == 2 && g_ccc_seen_rsp);
	ATT_OUT("13");
	ATT_OUT("1d 1000 656c04");
	ATT_IN("0a 1100");
	ATT_OUT("0b 0200");
	CHECK(tb_indicate(TB_MATTER_H_C2, k_hs, 3) == TB_EBUSY);
	ATT_IN("1e");
	CHECK(g_inds == 1 && g_ind_err == 0);
	ATT_IN("1e"); /* a second confirmation is stray */
	CHECK(g_inds == 1);

	/* C1 writes: request and command. */
	ATT_IN("12 0e00 0102030405");
	CHECK(g_writes == 1 && g_write_h == TB_MATTER_H_C1 && g_write_len == 5 &&
	      g_write_data[4] == 5);
	ATT_OUT("13");
	ATT_IN("52 0e00 aabb");
	CHECK(g_writes == 2 && g_write_len == 2);
	EXPECT_NONE();
	g_write_err = 0x13;
	ATT_IN("12 0e00 00");
	ATT_OUT("01 12 0e00 13");
	g_write_err = 0;

	/* A Write Command to the CCC announces at once; 1 B values count. */
	ATT_IN("52 1100 00");
	CHECK(g_cccs == 2 && g_ccc_v == 0);
	CHECK(tb_indicate(TB_MATTER_H_C2, k_hs, 3) == TB_EAGAIN);
	ATT_IN("12 1100 02");
	ATT_OUT("13");
	CHECK(g_cccs == 3 && g_ccc_v == 2);

	/* Length limit is MTU - 3. */
	{
		static uint8_t big[245];

		CHECK(tb_indicate(TB_MATTER_H_C2, big, 245) == TB_EINVAL);
		CHECK(tb_indicate(TB_MATTER_H_C2, big, 244) == 0);
		CHECK(g_log[g_head % LOG_MAX].len == 8 + 3 + 244);
		g_head++;
		nocp(1);
		ATT_IN("1e");
	}

	/* The Service Changed CCC is writable and readable, and independent. */
	ATT_IN("12 0400 0200");
	ATT_OUT("13");
	ATT_IN("0a 0400");
	ATT_OUT("0b 0200");
	ATT_IN("0a 1100");
	ATT_OUT("0b 0200");
	EXPECT_NONE();
}

static void test_credits(void)
{
	/* One controller buffer: owed packets wait for Number Of Completed
	 * Packets, then go in priority order. */
	g_auto_nocp = 0;
	up(251, 1);
	connect();
	ATT_IN("02 0502");
	ATT_OUT("03 f700");
	ATT_IN("0a 0700");
	l2(0x0006, "01 03 00 01 10 07 07");
	l2(0x0005, "14 05 0a00 8000 4000 f700 f700 0100");
	EXPECT_NONE();
	nocp(1);
	ATT_OUT("0b 74696e7974616c6b");
#if TB_SMP_REFUSE
	nocp(1);
	EXPECT(TB_HCI_ACL, frame(0x0006, "05 05"));
#endif
#if TB_SIG_REJECT
	nocp(1);
	SIG_OUT("01 05 0200 0000");
#endif
	EXPECT_NONE();

	/* A request while a reply is still owed is dropped. */
	ATT_IN("0a 0700");
	ATT_IN("0a 0900");
	nocp(1);
	ATT_OUT("0b 74696e7974616c6b");
	nocp(1);
	EXPECT_NONE();

	/* No port buffer: tried again on the next event. */
	g_no_bufs = 1;
	ATT_IN("0a 0900");
	EXPECT_NONE();
	g_no_bufs = 0;
	nocp(0);
	ATT_OUT("0b 0000");

	/* Credits come back on disconnect, not just by NOCP. */
	rx(TB_HCI_EVT, "0504 00 0100 08");
	connect();
	ATT_IN("0a 0900");
	ATT_OUT("0b 0000");
	/* NOCP for another handle does not count. */
	ATT_IN("0a 0900");
	rx(TB_HCI_EVT, "1305 01 0200 0100");
	EXPECT_NONE();
	nocp(5); /* capped at the controller's count */
	ATT_OUT("0b 0000");
	g_auto_nocp = 1;
}

static void test_l2cap_smp(void)
{
	up(251, 3);
	g_now = 5000;
	connect();
	CHECK(tb_poll() == 300);
	g_now = 5299;
	CHECK(tb_poll() == 1);
	EXPECT_NONE();
	g_now = 5300;
	CHECK(tb_poll() == UINT32_MAX);
	SIG_OUT("12 01 0800 0c00 1800 0000 c800");

	l2(0x0005, "13 01 0200 0000"); /* accepted */
	l2(0x0005, "01 07 0200 0000"); /* a reject of something */
	l2(0x0005, "12 02 0800 0c00 1800 0000 c800"); /* a central asking us */
#if TB_SIG_REJECT
	SIG_OUT("01 02 0200 0000");
#endif
	l2(0x0005, "14 00 0a00 8000 4000 f700 f700 0100"); /* identifier 0 */
	l2(0x0005, "14 03 0b00 8000 4000 f700 f700 0100"); /* length wrong */
	l2(0x0005, "14");
	EXPECT_NONE();

	l2(0x0006, "01 03 00 01 10 07 07");
#if TB_SMP_REFUSE
	EXPECT(TB_HCI_ACL, frame(0x0006, "05 05"));
#endif
	l2(0x0006, "05 05");
	l2(0x0006, "");
	l2(0x0007, "00");
	EXPECT_NONE();

	/* The next link's request uses the next identifier. */
	rx(TB_HCI_EVT, "0504 00 0100 13");
	connect();
	g_now += 300;
	(void)tb_poll();
	SIG_OUT("12 02 0800 0c00 1800 0000 c800");
	g_now = 1000;
}

static void test_acl_drops(void)
{
	uint8_t b[31] = {0x01, 0x10, 27, 0};

	up(251, 3);
	connect();
	tb_rx_dropped = 0;
	/* Continuation without a start. */
	rx(TB_HCI_ACL, "0110 0300 0a0700");
	/* ACL length wrong. */
	rx(TB_HCI_ACL, "0120 0800 0300 0400 0a0700");
	/* Another handle. */
	rx(TB_HCI_ACL, "0220 0700 0300 0400 0a0700");
	/* Unknown CID. */
	rx(TB_HCI_ACL, "0120 0500 0100 4000 00");
	rx(TB_HCI_ACL, "01");
	CHECK(tb_rx_dropped == 5);
	/* Longer than the MTU: dropped when it overflows, and what follows has
	 * no start. */
	rx(TB_HCI_ACL, "0120 0400 ff00 0400");
	for (int i = 0; i < 10; i++) {
		tb_hci_rx(TB_HCI_ACL, b, sizeof(b));
	}
	CHECK(tb_rx_dropped == 6);
	tb_hci_rx(TB_HCI_ACL, b, sizeof(b));
	CHECK(tb_rx_dropped == 7);
	/* More than the L2CAP length. */
	rx(TB_HCI_ACL, "0120 0400 0100 0400");
	rx(TB_HCI_ACL, "0110 0200 0a07");
	CHECK(tb_rx_dropped == 8);
	EXPECT_NONE();
	/* Split anywhere, the L2CAP header included. */
	rx(TB_HCI_ACL, "0120 0200 0300");
	rx(TB_HCI_ACL, "0110 0300 0400 0a");
	EXPECT_NONE();
	rx(TB_HCI_ACL, "0110 0200 0700");
	ATT_OUT("0b 74696e7974616c6b");
	/* A new start drops an unfinished PDU. */
	rx(TB_HCI_ACL, "0120 0500 0300 0400 0a");
	rx(TB_HCI_ACL, "0120 0700 0300 0400 0a0700");
	ATT_OUT("0b 74696e7974616c6b");
	/* PB 00 is a start too. */
	rx(TB_HCI_ACL, "0100 0700 0300 0400 0a0700");
	ATT_OUT("0b 74696e7974616c6b");
	CHECK(tb_rx_dropped == 8);
	EXPECT_NONE();
}

static void test_disconnects(void)
{
	/* Mid-indication: the indication completes first, then the link. */
	g_inds = 0;
	g_discs = 0;
	ready_link();
	CHECK(tb_indicate(TB_MATTER_H_C2, k_hs, 3) == 0);
	ATT_OUT("1d 1000 656c04");
	g_order = 0;
	rx(TB_HCI_EVT, "0504 00 0100 08");
	CHECK(g_inds == 1 && g_ind_err == TB_ECONNRESET && g_order == 2);
	CHECK(tb_indicate(TB_MATTER_H_C2, k_hs, 3) == TB_ENOTCONN);
	CHECK(tb_mtu() == 247); /* until the next link */
	connect();
	CHECK(tb_mtu() == 23);
	CHECK(tb_indicate(TB_MATTER_H_C2, k_hs, 3) == TB_EAGAIN); /* CCC per link */

	/* We disconnect. */
	tb_disconnect(0x13);
	EXPECT(TB_HCI_CMD, "060403 0100 13");
	rx(TB_HCI_EVT, "0f04 00 01 0604");
	rx(TB_HCI_EVT, "0504 00 0100 16");
	CHECK(g_disc_reason == 0x16);
	tb_disconnect(0x13); /* no link: nothing */
	EXPECT_NONE();

#if TB_ATT_TIMEOUT
	/* ATT timeout: 30 s without a confirmation. */
	ready_link();
	g_inds = 0;
	CHECK(tb_indicate(TB_MATTER_H_C2, k_hs, 3) == 0);
	ATT_OUT("1d 1000 656c04");
	g_now += 29999;
	CHECK(tb_poll() == 1);
	SIG_OUT("12 01 0800 0c00 1800 0000 c800"); /* came due meanwhile */
	EXPECT_NONE();
	g_now += 1;
	(void)tb_poll();
	CHECK(g_inds == 1 && g_ind_err == TB_ETIMEDOUT);
	EXPECT(TB_HCI_CMD, "060403 0100 13");
#else
	/* No ATT timeout: the indication waits for its confirmation. */
	ready_link();
	g_inds = 0;
	CHECK(tb_indicate(TB_MATTER_H_C2, k_hs, 3) == 0);
	ATT_OUT("1d 1000 656c04");
	g_now += 60000;
	CHECK(tb_poll() == UINT32_MAX);
	SIG_OUT("12 01 0800 0c00 1800 0000 c800"); /* came due meanwhile */
	EXPECT_NONE();
	CHECK(g_inds == 0);
	ATT_IN("1e");
	CHECK(g_inds == 1 && g_ind_err == 0);
#endif
	g_now = 1000;
}

/* Pop ACL packets until one whole PDU is in @p pdu; checks each header. */
static size_t pop_pdu(uint8_t *pdu, unsigned int acl_len)
{
	size_t got = 0, want = 4;

	while (got < want && g_head != g_tail) {
		const uint8_t *p = g_log[g_head % LOG_MAX].b;
		uint16_t len = g_log[g_head % LOG_MAX].len;

		CHECK_QUIET(g_log[g_head % LOG_MAX].type == TB_HCI_ACL);
		CHECK_QUIET(p[0] == 0x01 && p[1] == (got ? 0x10 : 0x00));
		CHECK_QUIET(p[2] + 4 == len && p[3] == 0 && len - 4 <= acl_len);
		memcpy(pdu + got, p + 4, len - 4u);
		got += len - 4u;
		if (got >= 4) {
			want = 4u + (pdu[0] | (pdu[1] << 8));
		}
		g_head++;
	}
	CHECK(got == want);
	return got;
}

static void test_small_controller(void)
{
	static uint8_t big[244];
	uint8_t pdu[260], frag[31];
	size_t n;

	/* 27 B controller buffers, 2 of them: the MTU is still 247. */
	g_auto_nocp = 0;
	g_writes = 0;
	up(27, 2);
	connect();
	ATT_IN("02 0502");
	ATT_OUT("03 f700");
	nocp(1);
	ATT_IN("12 1100 0200");
	ATT_OUT("13");
	nocp(1);
	CHECK(tb_mtu() == 247);

	/* A 251 B indication goes out as 9 packets of 27 B and one of 8, two
	 * at a time; a CCC write meanwhile is answered after the whole PDU,
	 * and announced after its answer. */
	for (size_t i = 0; i < sizeof(big); i++) {
		big[i] = (uint8_t)i;
	}
	CHECK(tb_indicate(TB_MATTER_H_C2, big, sizeof(big)) == 0);
	CHECK(g_tail - g_head == 2);
	g_cccs = 0;
	ATT_IN("12 0400 0200");
	for (int i = 0; i < 4; i++) {
		nocp(2);
	}
	CHECK(g_tail - g_head == 10 && g_cccs == 0);
	n = pop_pdu(pdu, 27);
	CHECK(n == 251 && pdu[0] == 247 && pdu[2] == 4 && pdu[4] == 0x1D && pdu[5] == 0x10 &&
	      !memcmp(pdu + 7, big, sizeof(big)));
	EXPECT_NONE();
	nocp(2);
	CHECK(g_cccs == 1 && g_ccc_h == 0x0003 && g_ccc_seen_rsp);
	ATT_OUT("13");
	nocp(1);
	ATT_IN("1e");

	/* A 185 B Write Request arrives in 27 B pieces. */
	frag[0] = 0x01;
	frag[1] = 0x20;
	frag[3] = 0;
	pdu[0] = 185;
	pdu[1] = 0;
	pdu[2] = 4;
	pdu[3] = 0;
	pdu[4] = 0x12;
	pdu[5] = TB_MATTER_H_C1;
	pdu[6] = 0;
	memcpy(pdu + 7, big, 182);
	for (n = 0; n < 189; n += 27) {
		size_t k = 189 - n < 27 ? 189 - n : 27;

		frag[2] = (uint8_t)k;
		memcpy(frag + 4, pdu + n, k);
		tb_hci_rx(TB_HCI_ACL, frag, (uint16_t)(4 + k));
		frag[1] = 0x10;
	}
	CHECK(g_writes == 1 && g_write_len == 182 && !memcmp(g_write_data, big, 182));
	ATT_OUT("13");
	nocp(1);
	EXPECT_NONE();
	g_auto_nocp = 1;
}

/* Random ACL and events: no crash, no oversized or malformed packet out. */
static void test_random(void)
{
	uint32_t x = 12345;
	uint8_t b[300];

	ready_link();
	for (int i = 0; i < 200000; i++) {
		uint16_t n, j;

		x = x * 1103515245u + 12345u;
		n = (uint16_t)((x >> 8) % 40u);
		for (j = 0; j < n + 8u; j++) {
			x = x * 1103515245u + 12345u;
			b[j] = (uint8_t)(x >> 16);
		}
		if ((x >> 4) % 16u == 0) {
			b[0] = 0x13; /* NOCP-shaped */
			tb_hci_rx(TB_HCI_EVT, b, (uint16_t)(n + 2u));
		} else {
			uint16_t cid = (uint16_t)(4u + (x >> 12) % 3u);

			b[0] = 0x01;
			b[1] = 0x20;
			b[2] = (uint8_t)(n + 4u);
			b[3] = 0;
			b[4] = (uint8_t)n;
			b[5] = 0;
			b[6] = (uint8_t)cid;
			b[7] = 0;
			if (cid == 4u && n) {
				b[8] = (uint8_t)("\x02\x04\x06\x08\x0a\x0c\x10\x12\x52\x1e"[(x >> 20) % 10u]);
			}
			/* Some continuations, some L2CAP lengths spanning packets. */
			if ((x >> 24) % 4u == 0) {
				b[1] = 0x10;
			}
			if ((x >> 26) % 4u == 0) {
				b[4] = (uint8_t)(x >> 3);
			}
			tb_hci_rx(TB_HCI_ACL, b, (uint16_t)(n + 8u));
		}
		if (i % 7 == 0) {
			nocp(1);
		}
		while (g_head != g_tail) {
			const uint8_t *p = g_log[g_head % LOG_MAX].b;
			uint16_t len = g_log[g_head % LOG_MAX].len;

			if (g_log[g_head % LOG_MAX].type == TB_HCI_ACL) {
				CHECK_QUIET(len >= 8 && len <= 8 + 247 && p[2] + 4 == len &&
					    p[4] + 8 == len);
				if (p[6] == 4) {
					CHECK_QUIET(len - 8 <= tb_mtu());
				}
			}
			g_head++;
		}
	}
	CHECK(1);
}

int main(void)
{
	test_init_and_adv();
	test_discovery();
	test_mtu23_fill();
	test_errors();
	test_writes_and_indications();
	test_credits();
	test_l2cap_smp();
	test_acl_drops();
	test_disconnects();
	test_small_controller();
	test_random();
	printf("%s: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
