//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Xhorse super-chip (XT27A) - shared client/firmware definitions
//
// The XT27A "channel A" management protocol: a HITAG2-BPLM physical layer
// carrying 16-bit op,~op page commands XORed against a 16-bit Fibonacci LFSR
// keystream (seed = bitrev16(reply) ^ 0xE12E).  See doc/xhorse_notes.md for the
// protocol; the per-emulated-type page recipes selected by the "recipe" field
// below are implemented in armsrc/xhorse.c (xh_recipe).
//-----------------------------------------------------------------------------

#ifndef XHORSE_H__
#define XHORSE_H__

#include "common.h"

#define XHORSE_UID_MAXLEN   8

// Per-emulated-type page-write recipe the firmware applies in configure().
// These are the only XT27A types whose on-air write path is fully decoded.
// ID8C (opaque) and the XT27B/XT57-only blob types are
// deliberately absent - the client refuses them before sending a packet.
typedef enum {
    XHORSE_R_HITAG2 = 0,  // ID46/7936/7937/7946: type byte + UID + MIKR/ON/config image
    XHORSE_R_7935,        // PCF7935/SECT: full bank-0 wipe, 0x1F=0x07
    XHORSE_R_7938,        // ID47: distinct 5-page image
    XHORSE_R_7939,        // ID49: UID at bank-0 0x00, bank-1 0x00 = 0xF7F8251D
    XHORSE_R_ID48,        // Megamos: full 16-word bank-0 image
    XHORSE_R_ID4C,        // 8-byte UID + CRC16-KERMIT -> bank-0 0x24..0x26
    XHORSE_R_ID11,        // EM fixed-code, header 0x0010, per-byte bitrev packing
    XHORSE_R_ID12,        // EM fixed-code, header 0x0014
    XHORSE_R_ID13,        // EM4102 + per-byte bitrev, type word 0x0080008C
    XHORSE_R_TEMPLATE,    // ID4D/4D80/ID4E config-template (config word in .aux)
    XHORSE_R_ID8A,        // full-memory image, type byte 0x8A last, UID at 0x04/0x05
    XHORSE_R_ID8E,        // UID at bank-0 0x0B||0x0A
} xhorse_recipe_t;

// Client -> firmware payload for configure (reset sends no payload; it builds its own ID46 image).
typedef struct {
    uint8_t  recipe;      // xhorse_recipe_t
    uint8_t  typebyte;    // on-chip 0x3C low byte
    uint8_t  has_type;    // 1 => write the type byte at 0x3C first (0 for ID8A, whose recipe writes it last)
    uint8_t  typeword;    // 1 => write 32-bit 0x0080008C at 0x3C (ID13)
    uint8_t  uidlen;      // 0 / 4 / 5 / 8
    uint8_t  uid[XHORSE_UID_MAXLEN];
    uint8_t  verify;      // 1 => read every write back and compare
    uint8_t  rfu;         // padding / reserved
    uint16_t aux;         // config word for XHORSE_R_TEMPLATE
} PACKED lf_xhorse_data_t;

// Firmware -> client reply for info/dump.
typedef struct {
    uint8_t  found;           // 1 => an XT27A answered the handshake
    uint8_t  typebyte;        // emulated type byte read from bank-1 0x3C
    uint8_t  typebyte_ok;     // 1 => two consecutive reads of 0x3C agreed
    uint8_t  bank;            // bank image[] was read from (dump: 0 or 1, as requested; info: 0)
    uint8_t  chipid[16];      // 16-byte unique chip ID (bank-1 0x33..0x36)
    uint8_t  image[64 * 4];   // pages 0x00..0x3F of `bank` (dump); bank-0 0x00..0x07 (info)
    uint8_t  image_pages;     // how many bank-0 pages in image[] are valid
    // XMICRO / "channel B" probe (XT57 / XT37-class chips).  The XT27A never answers
    // XMICRO, so a reply here means a newer, not-yet-supported super-chip family.
    uint8_t  bchip;           // 1 => a B-channel (XMICRO) chip answered the identify probe
    uint8_t  bchip_type_len;  // number of ASCII bytes in bchip_type[]
    uint8_t  bchip_type[8];   // ASCII family string the chip reported (e.g. "XT37")
    uint8_t  page_ok[8];      // bit (n % 8) of byte n / 8 set => two consecutive reads of page n agreed
} PACKED lf_xhorse_info_t;

#endif // XHORSE_H__
