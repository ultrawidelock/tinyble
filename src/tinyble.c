/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * tinyble core: HCI, the fixed L2CAP channels, and an ATT server over the
 * const table in tb_attrs[].
 *
 * ATT replies follow Zephyr's subsys/bluetooth/host/att.c (NCS v3.3.0) rule for
 * rule: how full a list reply gets, which handle an error names, which opcodes
 * earn an error. A trace of the Zephyr build is then a known-answer test for
 * this file. Where tinyble differs it says so.
 */
#include <string.h>

#include <tinyble/tinyble.h>
#if TB_COC
#include <tinyble/tb_coc.h>
#endif

#ifndef TB_ADV_MIN
#define TB_ADV_MIN 0x00A0u /* 100 ms, in 0.625 ms */
#endif
#ifndef TB_ADV_MAX
#define TB_ADV_MAX 0x00F0u /* 150 ms */
#endif
/* The L2CAP connection parameter request, sent TB_CPUR_MS after connecting:
 * the door-lock sample's PREF and CONN_PARAM_UPDATE_TIMEOUT Kconfig. Zephyr
 * asks through HCI LE Connection Update when both sides support the LL
 * procedure and falls back to L2CAP; tinyble always uses L2CAP, which every
 * central must accept. */
#ifndef TB_CPUR_MS
#define TB_CPUR_MS 300u
#endif
#define CONN_MIN 12u  /* 15 ms, in 1.25 ms */
#define CONN_MAX 24u  /* 30 ms */
#define CONN_LAT 0u
#define CONN_TO  200u /* 2 s, in 10 ms */

#define NO_CONN        0xFFFFu
#define MTU_MIN        23u
#define MTU_MAX        247u /* 251 B L2CAP PDU */
#define ATT_TIMEOUT_MS 30000u

#define CID_ATT 0x0004u
#define CID_SIG 0x0005u
#define CID_SMP 0x0006u

/* ATT error codes. */
#define E_INVALID_HANDLE 0x01u
#define E_READ_NOT_PERM  0x02u
#define E_WRITE_NOT_PERM 0x03u
#define E_INVALID_PDU    0x04u
#define E_NOT_SUPPORTED  0x06u
#define E_INVALID_OFFSET 0x07u
#define E_NOT_FOUND      0x0Au
#define E_INVALID_LEN    0x0Du
#define E_UNSUPP_GROUP   0x10u

/* HCI commands owed, sent lowest bit first, one at a time. Bits 0 to 4 are
 * the init sequence. */
enum { C_EVT_MASK, C_LE_EVT_MASK, C_LE_BUF, C_RAND_ADDR, C_ADV_PARAMS, C_ADV_DATA,
#if TB_SCAN_RSP
       C_SCAN_RSP, /* ahead of the enable */
#endif
       C_ADV_EN, C_DISC,
#if TB_RSSI
       C_RSSI,
#endif
};
/* ACL PDUs owed, sent lowest bit first while the controller has buffers. The
 * channel's four come last: its data behind everything else. */
enum { P_ATT, P_SMP, P_REJ, P_CPUR, P_IND, P_COC_RSP, P_COC_CRED, P_COC_DISC, P_COC_DATA };
#if TB_COC
typedef uint16_t pdus_t;
#else
typedef uint8_t pdus_t;
#endif

#define BIT(n) (1u << (n))

uint8_t tb_rx_dropped;

/* One L2CAP PDU each way, header included: the inbound one being reassembled,
 * the outbound one being split into the controller's ACL length. */
static uint8_t rx_pdu[4u + MTU_MAX], tx_pdu[4u + MTU_MAX];

#if TB_SERVICE_CHANGED
/* tb_service_changed()'s range, little endian; survives tb_init(). */
static uint8_t sc_range[4];
#endif

/* Bytes first, then halfwords, then words: Thumb's short loads reach offsets
 * 0 to 31, 62 and 124 B respectively. */
static struct {
	uint8_t op, err, ulen; /* the ATT reply owed: its request, parsed */
	pdus_t pdus;           /* P_* bits; P_IND lives in ind */
	uint8_t ncmd;          /* the controller takes a command */
	uint8_t credits, credits_max;
	uint8_t state;         /* 0 starting, 1 up, 2 failed */
	uint8_t inside;        /* depth of tb_* calls; the outermost one pumps */
	uint8_t adv_on;        /* what the controller is doing */
	uint8_t adv_len;
#if TB_SCAN_RSP
	uint8_t sd_len;
#endif
	uint8_t ident;         /* last signaling identifier we used */
#if TB_SIG_REJECT
	uint8_t rej_ident;
#endif
	uint8_t ind;           /* indication: 1 owed, 2 awaiting confirmation */
#if TB_SERVICE_CHANGED
	uint8_t sc;            /* the Service Changed one, likewise */
#endif
	uint8_t disc_reason;
#if TB_COC
	uint8_t coc_ident;     /* the connection request's, for its response */
	uint8_t coc_dident;    /* the disconnection request's */
	uint8_t coc_used;      /* K-frames taken since the last credits went back */
	uint8_t coc_sent;      /* tb_on_coc_sent() owed once the last K-frame is out */
#endif
	uint16_t conn;
	uint16_t cmds;         /* C_* bits */
	uint16_t mtu;
	uint16_t acl;          /* controller's ACL length, at most one PDU */
	uint16_t rx_len;       /* bytes of rx_pdu so far */
	uint16_t tx_len, tx_off; /* tx_pdu's length and how much has gone */
	uint16_t h;            /* start handle, or the handle */
	uint16_t end;          /* end handle, or the Read Blob offset */
	uint16_t ccc_h;        /* value handle whose CCC this reply's write changed */
	uint16_t ccc_v;
	uint16_t ind_h, ind_len;
#if TB_SERVICE_CHANGED
	uint16_t sc_h;         /* Service Changed value while the peer has it on */
#endif
	const uint8_t *adv;
#if TB_SCAN_RSP
	const uint8_t *sd;
#endif
	const uint8_t *ind_data;
	uint32_t cpur_at;
#if TB_ATT_TIMEOUT
	uint32_t ind_at;
#endif
#if TB_COC
	uint16_t coc_cid;      /* the peer's CID; 0 while no channel */
	uint16_t coc_mtu, coc_mps, coc_tx_cr, coc_rx_cr; /* the peer's, and the credits */
	uint16_t coc_result;   /* the connection response's */
	uint16_t coc_tx_len, coc_tx_off, coc_rx_len, coc_rx_sdu;
	uint16_t coc_disc[2];  /* the disconnection request's two CIDs, echoed */
	const uint8_t *coc_tx;
#endif
	uint8_t u[16];         /* type UUID, or the Find By Type Value value */
	uint8_t addr[7];       /* [6]: an address was given */
	uint8_t ccc[4][2];     /* per CCC slot, little endian */
} s;

#if TB_COC
static uint8_t coc_rx_buf[TB_COC_MTU];
#endif

/* On a little-endian CPU the compiler turns these memcpy calls into one
 * halfword access each; the byte form costs 72 B more on Cortex-M4. */
static uint16_t get16(const uint8_t *p)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	uint16_t v;

	memcpy(&v, p, 2);
	return v;
#else
	return (uint16_t)(p[0] | (p[1] << 8));
#endif
}

static void put16(uint8_t *p, uint16_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	memcpy(p, &v, 2);
#else
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
#endif
}

/* GCC and Clang builtin: RBIT and CLZ on Cortex-M4, 28 B less than a loop. */
static uint8_t lowest(uint16_t bits)
{
	return (uint8_t)__builtin_ctz(bits);
}

static void pump(void);
#if TB_COC
static void coc_close(void);
#endif

static void enter(void)
{
	s.inside++;
}

/* The outermost call sends what is owed. Hooks run inside it, so an
 * indication asked for from a hook joins the same pass instead of nesting. */
static void leave(void)
{
	if (s.inside == 1u) {
		pump();
	}
	s.inside--;
}

/* ---- HCI commands --------------------------------------------------------- */

static const uint16_t k_op[] = {0x0C01u, 0x2001u, 0x2002u, 0x2005u,
				 0x2006u, 0x2008u,
#if TB_SCAN_RSP
				 0x2009u,
#endif
				 0x200Au, 0x0406u,
#if TB_RSSI
				 0x1405u,
#endif
};
static const uint8_t k_plen[] = {8, 8, 0, 6, 15, 32,
#if TB_SCAN_RSP
				 32,
#endif
				 1, 3,
#if TB_RSSI
				 2,
#endif
};

static void cmd(void)
{
	uint8_t *b, *p;
	unsigned int i, on = 0;

	for (;;) {
		if (!s.ncmd || !s.cmds || s.state == 2u) {
			return;
		}
		i = lowest(s.cmds);
		on = s.adv_len && s.conn == NO_CONN;
		if ((i == C_ADV_EN && on == s.adv_on) || (i == C_DISC && s.conn == NO_CONN)
#if TB_RSSI
		    || (i == C_RSSI && s.conn == NO_CONN)
#endif
		) {
			s.cmds &= (uint16_t)~BIT(i);
			continue;
		}
		break;
	}
	b = tb_port_tx_alloc(TB_HCI_CMD, 3u + 32u);
	if (!b) {
		return;
	}
	put16(b, k_op[i]);
	b[2] = k_plen[i];
	p = b + 3;
	memset(p, 0, k_plen[i]);
	switch (i) {
	case C_EVT_MASK: /* Disconnection Complete, LE Meta */
		p[0] = 0x10;
		p[7] = 0x20;
		break;
	case C_LE_EVT_MASK: /* LE Connection Complete */
		p[0] = 0x01;
		break;
	case C_RAND_ADDR:
		memcpy(p, s.addr, 6);
		break;
	case C_ADV_PARAMS: /* ADV_IND, all channels, no filter */
		put16(p, TB_ADV_MIN);
		put16(p + 2, TB_ADV_MAX);
		p[5] = s.addr[6]; /* own address: random if given */
		p[13] = 0x07;
		break;
	case C_ADV_DATA:
		p[0] = s.adv_len;
		memcpy(p + 1, s.adv, s.adv_len);
		break;
#if TB_SCAN_RSP
	case C_SCAN_RSP:
		p[0] = s.sd_len;
		memcpy(p + 1, s.sd, s.sd_len);
		break;
#endif
	case C_ADV_EN:
		p[0] = (uint8_t)on;
		s.adv_on = (uint8_t)on;
		break;
	case C_DISC:
		put16(p, s.conn);
		p[2] = s.disc_reason;
		break;
#if TB_RSSI
	case C_RSSI:
		put16(p, s.conn);
		break;
#endif
	default: /* C_LE_BUF: no parameters */
		break;
	}
	s.cmds &= (uint16_t)~BIT(i);
	s.ncmd = 0;
	tb_port_tx_send(b, (uint16_t)(3u + k_plen[i]));
}

static void cmd_complete(uint16_t op, uint8_t status, const uint8_t *ret, uint8_t rlen)
{
	uint16_t m;

	if (s.state == 1u) {
		if (op == 0x200Au && status) {
			s.adv_on = 0;
		}
#if TB_RSSI
		if (op == 0x1405u && rlen >= 3u && get16(ret) == s.conn) {
			tb_on_rssi(status ? TB_EIO : 0, (int8_t)ret[2]);
		}
#endif
		return;
	}
	if (op == 0x2002u && !status && rlen >= 3u) {
		m = get16(ret);
		s.acl = m > 4u + MTU_MAX ? 4u + MTU_MAX : m;
		s.credits = s.credits_max = ret[2];
		if (m < MTU_MIN + 4u || !ret[2]) {
			status = 0xFFu;
		}
	}
	if (status) {
		s.state = 2;
		s.cmds = 0;
		tb_on_ready(-(int)status);
	} else if (op == 0x2006u) {
		s.state = 1;
		tb_on_ready(0);
	}
}

/* ---- link ----------------------------------------------------------------- */

static void link_up(uint16_t handle)
{
	s.conn = handle;
	s.adv_on = 0; /* legacy advertising stops when it connects */
	s.mtu = MTU_MIN;
	memset(s.ccc, 0, sizeof(s.ccc));
	s.credits = s.credits_max;
	s.cpur_at = tb_port_ms() + TB_CPUR_MS;
	s.pdus = BIT(P_CPUR);
}

static void link_down(uint8_t reason)
{
	s.conn = NO_CONN;
	s.pdus = 0;
	s.ccc_h = 0;
	s.rx_len = 0;
	s.tx_len = s.tx_off = 0;
	/* The controller frees a dead link's buffers without telling us. */
	s.credits = s.credits_max;
	s.cmds = (uint16_t)((s.cmds & ~BIT(C_DISC)) | BIT(C_ADV_EN));
#if TB_SERVICE_CHANGED
	s.sc = 0;
	s.sc_h = 0;
#endif
	if (s.ind) {
		s.ind = 0;
		tb_on_indicated(TB_ECONNRESET);
	}
#if TB_COC
	coc_close();
#endif
	tb_on_disconnect(reason);
}

static void evt(const uint8_t *p, uint16_t n)
{
	uint16_t i;

	if (n < 2u || n < 2u + p[1]) {
		return;
	}
	switch (p[0]) {
	case 0x0E: /* Command Complete: ncmd, opcode, status, return parameters */
		if (p[1] >= 3u) { /* a NOP (opcode 0) carries no status */
			s.ncmd = p[2];
		}
		if (p[1] >= 4u) {
			cmd_complete(get16(p + 3), p[5], p + 6, (uint8_t)(p[1] - 4u));
		}
		break;
	case 0x0F: /* Command Status: status, ncmd, opcode */
		if (p[1] >= 4u) {
			s.ncmd = p[3];
		}
		break;
	case 0x05: /* Disconnection Complete: status, handle, reason */
		if (p[1] >= 4u && !p[2] && get16(p + 3) == s.conn) {
			link_down(p[5]);
		}
		break;
	case 0x13: /* Number Of Completed Packets: count, then handle and packets */
		for (i = 0; i < p[2] && 7u + 4u * i <= n; i++) {
			if (get16(p + 3 + 4 * i) == s.conn) {
				s.credits = (uint8_t)(s.credits + get16(p + 5 + 4 * i));
			}
		}
		if (s.credits > s.credits_max) {
			s.credits = s.credits_max;
		}
		break;
	case 0x3E: /* LE Connection Complete: subevent, status, handle, role,
		    * peer address type and address, ... */
		if (p[1] >= 19u && p[2] == 0x01u && !p[3] && s.conn == NO_CONN) {
			link_up(get16(p + 4) & 0x0FFFu);
			tb_on_connect(p + 7);
		}
		break;
	default:
		break;
	}
}

/* ---- ATT ------------------------------------------------------------------ */

static const struct tb_attr *attr(uint16_t h)
{
	return &tb_attrs[h - 1u];
}

static int is_svc(uint16_t h)
{
	return (attr(h)->type & 0xFFFEu) == 0x2800u; /* primary or secondary */
}

/* The last handle of the service at @p h, no further than @p last. */
static uint16_t group_end(uint16_t h, uint16_t last)
{
	while (h < last && !is_svc(h + 1u)) {
		h++;
	}
	return h;
}

static uint8_t value(uint16_t h, const uint8_t **v)
{
	const struct tb_attr *a = attr(h);

	if (a->fl & TB_A_CCC) {
		*v = s.ccc[a->fl & TB_A_LEN];
		return 2;
	}
	*v = tb_vals + a->off;
	return a->fl & TB_A_LEN;
}

/* A 128-bit type is carried by the declaration before the value. */
static const uint8_t *uuid128(uint16_t h)
{
	return tb_vals + attr(h - 1u)->off + 3;
}

#if TB_SERVICE_CHANGED
/* Owe the Service Changed indication while a range is set and the peer has
 * its indications on; one in flight finishes first. */
static void sc_arm(void)
{
	if (s.sc != 2u) {
		s.sc = (uint8_t)(s.sc_h && (sc_range[0] | sc_range[1]) ? 1u : 0u);
	}
}
#endif

/* An indication is owed and none awaits its confirmation. */
static int ind_owed(void)
{
#if TB_SERVICE_CHANGED
	return (s.ind == 1u || s.sc == 1u) && s.ind != 2u && s.sc != 2u;
#else
	return s.ind == 1u;
#endif
}

#if TB_ATT_TIMEOUT
/* One awaits its confirmation. */
static int ind_out(void)
{
#if TB_SERVICE_CHANGED
	return s.ind == 2u || s.sc == 2u;
#else
	return s.ind == 2u;
#endif
}
#endif

static uint8_t write_attr(uint16_t h, const uint8_t *v, uint16_t n, int req)
{
	const struct tb_attr *a;
	uint16_t val;

	if (h - 1u >= tb_n_attrs) { /* 0 wraps */
		return E_INVALID_HANDLE;
	}
	a = attr(h);
	if (a->fl & TB_A_CCC) {
		if (!n || n > 2u) {
			return E_INVALID_LEN;
		}
		val = n == 1u ? v[0] : get16(v);
		s.ccc[a->fl & TB_A_LEN][0] = (uint8_t)(val & 3u);
#if TB_SERVICE_CHANGED
		if (attr((uint16_t)(h - 1u))->type == 0x2A05u) {
			s.sc_h = (val & 2u) ? (uint16_t)(h - 1u) : 0u;
			sc_arm();
		}
#endif
		if (req) {
			s.ccc_h = (uint16_t)(h - 1u);
			s.ccc_v = val;
		} else {
			tb_on_ccc((uint16_t)(h - 1u), val);
		}
		return 0;
	}
	if (a->fl & TB_A_W) {
		return tb_on_write(h, v, n);
	}
	return E_WRITE_NOT_PERM;
}

/* Zephyr's range_is_valid(): handle 0 is named 0, a reversed range its start. */
static void range(const uint8_t *p)
{
	s.h = get16(p + 1);
	s.end = get16(p + 3);
	if (!s.h || !s.end) {
		s.err = E_INVALID_HANDLE;
		s.h = 0;
	} else if (s.h > s.end) {
		s.err = E_INVALID_HANDLE;
	}
}

static void att(const uint8_t *p, uint16_t n)
{
	uint8_t op;
	uint16_t m;

	if (!n) {
		return;
	}
	op = p[0];
	if (op == 0x52u) { /* Write Command */
		if (n >= 3u) {
			(void)write_attr(get16(p + 1), p + 3, (uint16_t)(n - 3u), 0);
		}
		return;
	}
	if (op == 0x1Eu) { /* Handle Value Confirmation */
#if TB_SERVICE_CHANGED
		if (s.sc == 2u) {
			s.sc = 0;
		} else
#endif
		if (s.ind == 2u) {
			s.ind = 0;
			tb_on_indicated(0);
		}
		return;
	}
	/* Other commands are dropped. A request while a reply is still owed
	 * breaks ATT's one-at-a-time rule; it is dropped too. */
	if ((op & 0x40u) || (s.pdus & BIT(P_ATT))) {
		return;
	}
	s.op = op;
	s.err = 0;
	s.h = 0;
	s.end = 0;
	switch (op) {
	case 0x02: /* Exchange MTU */
		if (n < 3u || get16(p + 1) < MTU_MIN) {
			goto invalid;
		}
		m = get16(p + 1);
		s.mtu = m < MTU_MAX ? m : MTU_MAX;
		break;
	case 0x04: /* Find Information */
		if (n < 5u) {
			goto invalid;
		}
		range(p);
		break;
#if TB_FIND_BY_TYPE
	case 0x06: /* Find By Type Value: primary services only, as Zephyr */
		if (n < 7u) {
			goto invalid;
		}
		range(p);
		s.ulen = (uint8_t)(n - 7u <= 16u ? n - 7u : 0u);
		memcpy(s.u, p + 7, s.ulen);
		if (!s.err && get16(p + 5) != 0x2800u) {
			s.err = E_NOT_FOUND;
		}
		break;
#endif
	case 0x08: /* Read By Type */
	case 0x10: /* Read By Group Type */
		if (n != 7u && n != 21u) {
			goto invalid;
		}
		range(p);
		s.ulen = (uint8_t)(n - 5u);
		memcpy(s.u, p + 5, s.ulen);
		if (op == 0x10u && !s.err && (s.ulen != 2u || (get16(s.u) & 0xFFFEu) != 0x2800u)) {
			s.err = E_UNSUPP_GROUP;
		}
		break;
	case 0x0A: /* Read */
#if TB_READ_BLOB
	case 0x0C: /* Read Blob */
#endif
		if (n < (op == 0x0Au ? 3u : 5u)) {
			goto invalid;
		}
		s.h = get16(p + 1);
		if (op == 0x0Cu) {
			s.end = get16(p + 3);
		}
		break;
	case 0x12: /* Write Request */
		if (n < 3u) {
			goto invalid;
		}
		s.h = get16(p + 1);
		s.err = write_attr(s.h, p + 3, (uint16_t)(n - 3u), 1);
		break;
	default:
		/* Prepare and Execute Write, Read Multiple, and anything unknown
		 * that is not a command. Zephyr answers responses and
		 * notifications this way too. */
		s.err = E_NOT_SUPPORTED;
		break;
	}
	s.pdus |= BIT(P_ATT);
	return;
invalid:
	s.err = E_INVALID_PDU;
	s.h = 0;
	s.pdus |= BIT(P_ATT);
}

/*
 * Build the owed reply at @p q; returns its length.
 *
 * The four list replies share one item rule. An item is the handle, a group
 * end for Find By Type Value and Read By Group Type, then a value (the UUID
 * for Find Information). Every item is the first one's size, and Zephyr's
 * fill limits hold: Find Information and Read By Type add an item only while
 * more than its size is left, the grouped two while at least its size is.
 */
static unsigned int att_reply(uint8_t *q)
{
	const uint8_t *v;
	uint8_t t16[2];
	unsigned int h, last, pos, vl, hdr, il = 0, any = 0, m = s.mtu, op = s.op;
	unsigned int fbtv = TB_FIND_BY_TYPE && op == 0x06u;
	unsigned int grp = fbtv || op == 0x10u;

	if (s.err) {
		goto err;
	}
	switch (op) {
	case 0x02:
		q[0] = 0x03;
		put16(q + 1, MTU_MAX);
		return 3;
	case 0x12:
		q[0] = 0x13;
		return 1;
	case 0x0A:
#if TB_READ_BLOB
	case 0x0C:
#endif
		if (s.h - 1u >= tb_n_attrs) {
			s.err = E_INVALID_HANDLE;
			goto err;
		}
		if (!(attr(s.h)->fl & TB_A_R)) {
			s.err = E_READ_NOT_PERM;
			goto err;
		}
		vl = value(s.h, &v);
#if TB_READ_BLOB
		if (s.end > vl) {
			s.err = E_INVALID_OFFSET;
			goto err;
		}
		vl -= s.end;
		v += s.end;
#endif
		if (vl > m - 1u) {
			vl = m - 1u;
		}
		q[0] = (uint8_t)(op + 1u);
		memcpy(q + 1, v, vl);
		return vl + 1u;
	default:
		break;
	}

	last = s.end < tb_n_attrs ? s.end : tb_n_attrs;
	hdr = grp ? 4u : 2u;
	pos = fbtv ? 1u : 2u;
	for (h = s.h; h <= last; h++) {
		const struct tb_attr *a = attr((uint16_t)h);

		vl = 0;
		v = q; /* Find By Type Value copies nothing */
		if (op == 0x04u) {
			vl = a->type ? 2u : 16u;
			put16(t16, a->type);
			v = a->type ? t16 : uuid128((uint16_t)h);
		} else if (fbtv) { /* a primary service with this UUID */
			if (a->type != 0x2800u || (a->fl & TB_A_LEN) != s.ulen ||
			    memcmp(tb_vals + a->off, s.u, s.ulen)) {
				continue;
			}
		} else if (s.ulen == 2u ? (!a->type || a->type != get16(s.u))
					: (!TB_TYPE128 || a->type ||
					   memcmp(uuid128((uint16_t)h), s.u, 16))) {
			continue;
		} else if (!(a->fl & TB_A_R)) { /* services always read */
			if (!any) {
				s.err = E_READ_NOT_PERM; /* named by the start handle */
				goto err;
			}
			break;
		} else {
			vl = value((uint16_t)h, &v);
		}
		if (any && pos + il + !grp > m) {
			break;
		}
		if (vl > m - pos - hdr) {
			vl = m - pos - hdr;
		}
		if (any && vl + hdr != il) {
			break;
		}
		il = vl + hdr;
		put16(q + pos, (uint16_t)h);
		if (grp) {
			put16(q + pos + 2, group_end((uint16_t)h, (uint16_t)last));
		}
		memcpy(q + pos + hdr, v, vl);
		pos += il;
		any = 1;
	}
	if (!any) {
		s.err = E_NOT_FOUND;
		goto err;
	}
	q[0] = (uint8_t)(op + 1u);
	if (!fbtv) { /* Find By Type Value has no format or length byte */
		q[1] = (uint8_t)(op == 0x04u ? (il == 4u ? 1u : 2u) : il);
	}
	return pos;
err:
	q[0] = 0x01;
	q[1] = (uint8_t)op;
	put16(q + 2, s.h);
	q[4] = s.err;
	return 5;
}

#if TB_COC
/* ---- LE credit based flow control (Core Vol 3 Part A 3.4, 4.22 to 4.24) --
 *
 * One connection-oriented channel on SPSM TB_COC_SPSM, local CID TB_COC_CID,
 * SDUs of up to TB_COC_MTU each way. K-frames are our ACL size inbound (the
 * MPS we announce) and the peer's MPS outbound; the first of an SDU carries
 * its length. Every K-frame costs the sender one credit: the peer gets
 * TB_COC_CREDITS to start and them back, in batches, as SDUs are consumed. */
static void coc_close(void)
{
	if (s.coc_cid) {
		s.coc_cid = 0;
		s.pdus &= (pdus_t)~(BIT(P_COC_CRED) | BIT(P_COC_DATA));
		s.coc_rx_len = 0;
		s.coc_tx_len = s.coc_tx_off = 0;
		s.coc_sent = 0;
		tb_on_coc_close();
	}
}

/* A signaling command the channel takes: connection request, credits, a
 * disconnection request or response. 1 when it was one of those. */
static int coc_sig(const uint8_t *p, uint16_t n)
{
	uint16_t len = get16(p + 2);
	uint32_t c;

	if (len != n - 4u || !p[1]) { /* identifier 0 is not a request */
		return 0;
	}
	switch (p[0]) {
	case 0x14: /* LE Credit Based Connection Request: SPSM, SCID, MTU, MPS, credits */
		if (len != 10u) {
			return 0;
		}
		s.coc_ident = p[1];
		s.coc_result = 0;
		if (get16(p + 4) != TB_COC_SPSM) {
			s.coc_result = 0x0002; /* SPSM not supported */
		} else if (s.coc_cid || get16(p + 6) < 0x0040u) {
			s.coc_result = 0x0004; /* no resources available */
		} else if (get16(p + 8) < MTU_MIN || get16(p + 10) < MTU_MIN) {
			s.coc_result = 0x000B; /* unacceptable parameters */
		} else {
			s.coc_cid = get16(p + 6);
			s.coc_mtu = get16(p + 8);
			s.coc_mps = get16(p + 10);
			s.coc_tx_cr = get16(p + 12);
			s.coc_rx_cr = TB_COC_CREDITS;
			s.coc_used = 0;
			s.coc_rx_len = 0;
		}
		s.pdus |= BIT(P_COC_RSP);
		return 1;
	case 0x16: /* LE Flow Control Credit: CID, credits */
		if (len == 4u && s.coc_cid && get16(p + 4) == s.coc_cid) {
			c = (uint32_t)s.coc_tx_cr + get16(p + 6);
			s.coc_tx_cr = c > 0xFFFFu ? 0xFFFFu : (uint16_t)c;
		}
		return 1;
	case 0x06: /* Disconnection Request: DCID (ours), SCID (theirs) */
		if (len != 4u || get16(p + 4) != TB_COC_CID || !s.coc_cid ||
		    get16(p + 6) != s.coc_cid) {
			return 0; /* not our channel: rejected like any other command */
		}
		s.coc_dident = p[1];
		s.coc_disc[0] = get16(p + 4);
		s.coc_disc[1] = get16(p + 6);
		coc_close();
		s.pdus |= BIT(P_COC_DISC);
		return 1;
	case 0x07: /* Disconnection Response: we never ask, so never expected */
		return 1;
	default:
		return 0;
	}
}

/* A K-frame on our CID. */
static void coc_rx(const uint8_t *p, uint16_t n)
{
	if (!s.coc_rx_cr) { /* the peer spent credits it was not given */
		tb_disconnect(0x13);
		return;
	}
	s.coc_rx_cr--;
	s.coc_used++;
	if (!s.coc_rx_len) {
		if (n < 2u) {
			goto bad;
		}
		s.coc_rx_sdu = get16(p);
		p += 2;
		n = (uint16_t)(n - 2u);
		if (!s.coc_rx_sdu || s.coc_rx_sdu > TB_COC_MTU) {
			goto bad;
		}
	}
	if (s.coc_rx_len + n > s.coc_rx_sdu) {
		goto bad;
	}
	memcpy(coc_rx_buf + s.coc_rx_len, p, n);
	s.coc_rx_len = (uint16_t)(s.coc_rx_len + n);
	if (s.coc_rx_len < s.coc_rx_sdu) {
		return;
	}
	s.coc_rx_len = 0;
	/* Credits go back ahead of whatever the hook sends in reply. */
	if (s.coc_used >= TB_COC_CREDITS / 2u) {
		s.pdus |= BIT(P_COC_CRED);
	}
	tb_on_coc_data(coc_rx_buf, s.coc_rx_sdu);
	return;
bad:
	s.coc_rx_len = 0;
	tb_rx_dropped++;
}
#endif /* TB_COC */

/* ---- L2CAP ---------------------------------------------------------------- */

/* A PDU starts with PB 00 or 10 and continues with PB 01; a new start drops
 * an unfinished one. */
static void acl(const uint8_t *p, uint16_t n)
{
	uint16_t cid;

	if (n < 4u || (get16(p) & 0x0FFFu) != s.conn || get16(p + 2) != n - 4u) {
		goto drop;
	}
	if (!(p[1] & 0x10u)) {
		s.rx_len = 0;
	} else if (!s.rx_len) {
		goto drop;
	}
	n = (uint16_t)(n - 4u);
	if (s.rx_len + n > sizeof(rx_pdu)) {
		s.rx_len = 0;
		goto drop;
	}
	memcpy(rx_pdu + s.rx_len, p + 4, n);
	s.rx_len = (uint16_t)(s.rx_len + n);
	if (s.rx_len < 4u || s.rx_len < 4u + get16(rx_pdu)) {
		return;
	}
	n = get16(rx_pdu);
	if (s.rx_len != 4u + n) {
		s.rx_len = 0;
		goto drop;
	}
	s.rx_len = 0;
	cid = get16(rx_pdu + 2);
	p = rx_pdu + 4;
	if (cid == CID_ATT) {
		att(p, n);
	} else if (cid == CID_SIG) {
		/* Code, identifier, length. Zephyr ignores a Connection Parameter
		 * Update Response and a Command Reject (no dynamic channels) and
		 * rejects every other code as not understood, the LE credit based
		 * request included. */
#if TB_COC
		if (n >= 4u && coc_sig(p, n)) {
			return;
		}
#endif
#if TB_SIG_REJECT
		if (n >= 4u && p[1] && get16(p + 2) == n - 4u && p[0] != 0x13u && p[0] != 0x01u) {
			s.rej_ident = p[1];
			s.pdus |= BIT(P_REJ);
		}
#endif
#if TB_SMP_REFUSE
	} else if (cid == CID_SMP) {
		/* No pairing: answer anything but Pairing Failed with Pairing
		 * Failed, Pairing Not Supported (Core Vol 3 Part H 3.3). */
		if (n && p[0] != 0x05u) {
			s.pdus |= BIT(P_SMP);
		}
#endif
#if TB_COC
	} else if (cid == TB_COC_CID && s.coc_cid) {
		coc_rx(p, n);
#endif
	} else {
		goto drop;
	}
	return;
drop:
	tb_rx_dropped++;
}

static void pump(void)
{
	uint8_t *b, *q = tx_pdu + 4, k;
	uint16_t n, cid;
	pdus_t owed;

	cmd();
	while (s.credits && s.conn != NO_CONN) {
		if (s.tx_off == s.tx_len) {
			owed = (pdus_t)(s.pdus | (ind_owed() ? BIT(P_IND) : 0u));
			if ((owed & BIT(P_CPUR)) && (int32_t)(tb_port_ms() - s.cpur_at) < 0) {
				owed &= (pdus_t)~BIT(P_CPUR);
			}
#if TB_COC
			if (!s.coc_tx_cr) { /* data waits for the peer's credits */
				owed &= (pdus_t)~BIT(P_COC_DATA);
			}
#endif
			if (!owed) {
				return;
			}
			k = lowest(owed);
			cid = CID_ATT;
			switch (k) {
			case P_ATT:
				n = (uint16_t)att_reply(q);
				break;
#if TB_SMP_REFUSE
			case P_SMP:
				cid = CID_SMP;
				q[0] = 0x05;
				q[1] = 0x05;
				n = 2;
				break;
#endif
#if TB_SIG_REJECT
			case P_REJ: /* Command Reject, command not understood */
				cid = CID_SIG;
				memset(q, 0, 6);
				q[0] = 0x01;
				q[1] = s.rej_ident;
				q[2] = 2;
				n = 6;
				break;
#endif
			case P_CPUR:
				cid = CID_SIG;
				if (!++s.ident) {
					s.ident = 1;
				}
				q[0] = 0x12;
				q[1] = s.ident;
				put16(q + 2, 8);
				put16(q + 4, CONN_MIN);
				put16(q + 6, CONN_MAX);
				put16(q + 8, CONN_LAT);
				put16(q + 10, CONN_TO);
				n = 12;
				break;
#if TB_COC
			case P_COC_RSP: /* LE Credit Based Connection Response */
				cid = CID_SIG;
				q[0] = 0x15;
				q[1] = s.coc_ident;
				put16(q + 2, 10);
				put16(q + 4, s.coc_result ? 0u : TB_COC_CID);
				put16(q + 6, s.coc_result ? 0u : TB_COC_MTU);
				put16(q + 8, s.coc_result ? 0u : MTU_MAX);
				put16(q + 10, s.coc_result ? 0u : TB_COC_CREDITS);
				put16(q + 12, s.coc_result);
				n = 14;
				if (!s.coc_result) {
					tb_on_coc_open(); /* anything it sends follows this */
				}
				break;
			case P_COC_CRED: /* LE Flow Control Credit */
				cid = CID_SIG;
				if (!++s.ident) {
					s.ident = 1;
				}
				q[0] = 0x16;
				q[1] = s.ident;
				put16(q + 2, 4);
				put16(q + 4, TB_COC_CID);
				put16(q + 6, s.coc_used);
				s.coc_rx_cr = (uint16_t)(s.coc_rx_cr + s.coc_used);
				s.coc_used = 0;
				n = 8;
				break;
			case P_COC_DISC: /* Disconnection Response */
				cid = CID_SIG;
				q[0] = 0x07;
				q[1] = s.coc_dident;
				put16(q + 2, 4);
				put16(q + 4, s.coc_disc[0]);
				put16(q + 6, s.coc_disc[1]);
				n = 8;
				break;
			case P_COC_DATA: { /* one K-frame */
				uint16_t room = s.coc_mps < MTU_MAX ? s.coc_mps : MTU_MAX, m;

				cid = s.coc_cid;
				n = 0;
				if (!s.coc_tx_off) {
					put16(q, s.coc_tx_len);
					n = 2;
					room = (uint16_t)(room - 2u);
				}
				m = (uint16_t)(s.coc_tx_len - s.coc_tx_off);
				if (m > room) {
					m = room;
				}
				memcpy(q + n, s.coc_tx + s.coc_tx_off, m);
				n = (uint16_t)(n + m);
				s.coc_tx_off = (uint16_t)(s.coc_tx_off + m);
				s.coc_tx_cr--;
				if (s.coc_tx_off == s.coc_tx_len) {
					s.coc_sent = 1;
				}
				break;
			}
#endif
			default: /* P_IND: Service Changed first */
				q[0] = 0x1D;
#if TB_SERVICE_CHANGED
				if (s.sc == 1u) {
					put16(q + 1, s.sc_h);
					memcpy(q + 3, sc_range, 4);
					n = 7;
					s.sc = 2;
				} else
#endif
				{
					put16(q + 1, s.ind_h);
					memcpy(q + 3, s.ind_data, s.ind_len);
					n = (uint16_t)(3u + s.ind_len);
					s.ind = 2;
				}
#if TB_ATT_TIMEOUT
				s.ind_at = tb_port_ms() + ATT_TIMEOUT_MS;
#endif
				break;
			}
			put16(tx_pdu, n);
			put16(tx_pdu + 2, cid);
			s.tx_len = (uint16_t)(n + 4u);
			s.tx_off = 0;
			s.pdus &= (pdus_t)~BIT(k);
#if TB_COC
			if (k == P_COC_DATA && !s.coc_sent) {
				s.pdus |= BIT(P_COC_DATA); /* more K-frames to go */
			}
#endif
		}
		b = tb_port_tx_alloc(TB_HCI_ACL, (uint16_t)(4u + s.acl));
		if (!b) {
			return;
		}
		n = (uint16_t)(s.tx_len - s.tx_off);
		if (n > s.acl) {
			n = s.acl;
		}
		/* PB 00 first, not automatically flushable; PB 01 continuing. */
		put16(b, (uint16_t)(s.conn | (s.tx_off ? 0x1000u : 0u)));
		put16(b + 2, n);
		memcpy(b + 4, tx_pdu + s.tx_off, n);
		s.tx_off = (uint16_t)(s.tx_off + n);
		s.credits--;
		tb_port_tx_send(b, (uint16_t)(n + 4u));
		/* Once the Write Response has gone; it fits one packet. */
		if (s.ccc_h && !(s.pdus & BIT(P_ATT))) {
			n = s.ccc_h;
			s.ccc_h = 0;
			tb_on_ccc(n, s.ccc_v);
		}
#if TB_COC
		if (s.coc_sent && s.tx_off == s.tx_len) {
			s.coc_sent = 0;
			tb_on_coc_sent();
		}
#endif
	}
}

/* ---- API ------------------------------------------------------------------ */

void tb_init(const uint8_t *addr)
{
	memset(&s, 0, sizeof(s));
	tb_rx_dropped = 0;
	s.conn = NO_CONN;
	s.ncmd = 1;
	s.mtu = MTU_MIN;
	s.cmds = BIT(C_EVT_MASK) | BIT(C_LE_EVT_MASK) | BIT(C_LE_BUF) | BIT(C_ADV_PARAMS);
	if (addr) {
		memcpy(s.addr, addr, 6);
		s.addr[6] = 1;
		s.cmds |= BIT(C_RAND_ADDR);
	}
	enter();
	leave();
}

void tb_hci_rx(uint8_t type, const uint8_t *pkt, uint16_t len)
{
	enter();
	if (type == TB_HCI_EVT) {
		evt(pkt, len);
	} else if (type == TB_HCI_ACL) {
		acl(pkt, len);
	}
	leave();
}

uint32_t tb_poll(void)
{
	uint32_t now, d, next = UINT32_MAX;

	enter();
	now = tb_port_ms();
#if TB_ATT_TIMEOUT
	if (ind_out()) {
		d = s.ind_at - now;
		if ((int32_t)d <= 0) {
			/* No further ATT on this bearer (Core Vol 3 Part F 3.3.3):
			 * drop the link. */
#if TB_SERVICE_CHANGED
			s.sc = 0;
#endif
			if (s.ind) { /* in flight, or owed behind Service Changed */
				s.ind = 0;
				tb_on_indicated(TB_ETIMEDOUT);
			}
			tb_disconnect(0x13);
		} else {
			next = d;
		}
	}
#endif
	if (s.pdus & BIT(P_CPUR)) {
		d = s.cpur_at - now;
		if ((int32_t)d > 0 && d < next) {
			next = d;
		}
	}
	leave();
	return next;
}

void tb_adv(const uint8_t *ad, uint8_t len)
{
	enter();
	s.adv = ad;
	s.adv_len = len > 31u ? 31u : len;
	s.cmds |= (uint16_t)(BIT(C_ADV_EN) | (len ? BIT(C_ADV_DATA) : 0u));
	leave();
}

#if TB_SCAN_RSP
void tb_scan_rsp(const uint8_t *sd, uint8_t len)
{
	enter();
	s.sd = sd;
	s.sd_len = len > 31u ? 31u : len;
	s.cmds |= BIT(C_SCAN_RSP);
	leave();
}
#endif

#if TB_SERVICE_CHANGED
void tb_service_changed(uint16_t start, uint16_t end)
{
	enter();
	put16(sc_range, start);
	put16(sc_range + 2, end);
	sc_arm();
	leave();
}
#endif

/* Linked, and the CCC after value @p handle has @p bit (1 notify, 2 indicate). */
static int subscribed(uint16_t handle, unsigned int bit)
{
	const struct tb_attr *c;

	if (s.conn == NO_CONN) {
		return TB_ENOTCONN;
	}
	if (handle - 1u >= tb_n_attrs - 1u) {
		return TB_EAGAIN;
	}
	c = attr((uint16_t)(handle + 1u)); /* the CCC after the value */
	if (!(c->fl & TB_A_CCC) || !(s.ccc[c->fl & TB_A_LEN][0] & bit)) {
		return TB_EAGAIN;
	}
	return 0;
}

int tb_indicate(uint16_t handle, const uint8_t *data, uint16_t len)
{
	int rc = subscribed(handle, 2u);

	if (rc) {
		return rc;
	}
	if (s.ind) {
		return TB_EBUSY;
	}
	if (len > s.mtu - 3u) {
		return TB_EINVAL;
	}
	enter();
	s.ind = 1;
	s.ind_h = handle;
	s.ind_data = data;
	s.ind_len = len;
	leave();
	return 0;
}

/* Built straight into the outbound PDU, ahead of anything owed: nothing to
 * keep, and it goes out as the controller frees buffers, like the rest. */
int tb_notify(uint16_t handle, const uint8_t *data, uint16_t len)
{
	int rc = subscribed(handle, 1u);

	if (rc) {
		return rc;
	}
	if (s.tx_off != s.tx_len) {
		return TB_EBUSY;
	}
	if (len > s.mtu - 3u) {
		return TB_EINVAL;
	}
	enter();
	put16(tx_pdu, (uint16_t)(3u + len));
	put16(tx_pdu + 2, CID_ATT);
	tx_pdu[4] = 0x1B;
	put16(tx_pdu + 5, handle);
	memcpy(tx_pdu + 7, data, len);
	s.tx_len = (uint16_t)(7u + len);
	s.tx_off = 0;
	leave();
	return 0;
}

uint16_t tb_mtu(void)
{
	return s.mtu;
}

#if TB_COC
int tb_coc_up(void)
{
	return s.coc_cid != 0u;
}

int tb_coc_send(const uint8_t *sdu, uint16_t len)
{
	if (!s.coc_cid) {
		return TB_ENOTCONN;
	}
	if ((s.pdus & BIT(P_COC_DATA)) || s.coc_sent) {
		return TB_EBUSY;
	}
	if (!len || len > s.coc_mtu || len > TB_COC_MTU) {
		return TB_EINVAL;
	}
	enter();
	s.coc_tx = sdu;
	s.coc_tx_len = len;
	s.coc_tx_off = 0;
	s.pdus |= BIT(P_COC_DATA);
	leave();
	return 0;
}
#endif

#if TB_RSSI
void tb_rssi_request(void)
{
	enter();
	if (s.conn != NO_CONN) {
		s.cmds |= BIT(C_RSSI);
	}
	leave();
}
#endif

const uint8_t *tb_addr(void)
{
	return s.addr[6] ? s.addr : NULL;
}

void tb_disconnect(uint8_t reason)
{
	enter();
	if (s.conn != NO_CONN) {
		s.disc_reason = reason;
		s.cmds |= BIT(C_DISC);
	}
	leave();
}
