/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file tb_aliro.h
 * @brief The credential reader service 0xFFF2 in the Matter table
 *        (src/tb_matter_gatt.c, TB_ALIRO=1), after the Matter and, with
 *        TB_MATTER_DFU, the DFU services.
 *
 *   TB_ALIRO_H_FIRST  service 0xFFF2
 *   TB_ALIRO_H_READ   value D3B5A130-9E23-4B3A-8BE4-6B1EE5F980A3: Read, the
 *                     reader's SPSM, protocol versions and features
 *   TB_ALIRO_H_VER    value BD4B9502-3F54-11EC-B919-0242AC120005: Write, the
 *                     version the device selected; accepted, not kept
 *
 * The transaction itself runs on the channel of tb_coc.h.
 */
#pragma once
#ifndef TB_MATTER_DFU
#define TB_MATTER_DFU 0
#endif
#define TB_ALIRO_H_FIRST (TB_MATTER_DFU ? 0x001Au : 0x0012u)
#define TB_ALIRO_H_READ  (TB_ALIRO_H_FIRST + 2u)
#define TB_ALIRO_H_VER   (TB_ALIRO_H_FIRST + 4u)
#define TB_ALIRO_H_LAST  TB_ALIRO_H_VER
