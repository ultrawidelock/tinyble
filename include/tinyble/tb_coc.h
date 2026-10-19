/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file tb_coc.h
 * @brief One L2CAP connection-oriented channel with LE credit based flow
 *        control (Core Vol 3 Part A 3.4), built with TB_COC=1.
 *
 * The peer opens it on SPSM TB_COC_SPSM; tinyble accepts one at a time, with
 * local CID TB_COC_CID, an MTU of TB_COC_MTU each way and an MPS of its ACL
 * size. SDUs arrive whole through tb_on_coc_data() and go out whole through
 * tb_coc_send(), one at a time: the buffer must stay valid until
 * tb_on_coc_sent(). The hooks run in tinyble's one context, like the others
 * in tinyble.h.
 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#ifndef TB_COC
#define TB_COC 0
#endif
#ifndef TB_COC_SPSM
#define TB_COC_SPSM 0x0080u /* the credential reader's dynamic PSM */
#endif
#ifndef TB_COC_MTU
#define TB_COC_MTU 512u /* the largest SDU each way */
#endif
#ifndef TB_COC_CREDITS
#define TB_COC_CREDITS 8u /* K-frames the peer may send before we hand credits back */
#endif
#define TB_COC_CID 0x0040u

/** 1 while the channel is open. */
int tb_coc_up(void);
/** Queue one SDU. 0, TB_ENOTCONN without a channel, TB_EBUSY while the last
 *  one is still going out, TB_EINVAL when it is empty or beyond the MTU. */
int tb_coc_send(const uint8_t *sdu, uint16_t len);

/* Hooks. */
void tb_on_coc_open(void);
void tb_on_coc_close(void); /* the peer closed it, or the link went down */
void tb_on_coc_data(const uint8_t *sdu, uint16_t len);
void tb_on_coc_sent(void); /* the queued SDU has gone to the controller */

#ifdef __cplusplus
}
#endif
