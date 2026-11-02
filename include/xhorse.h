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

// Channel-A family that answered (lf_xhorse_info_t.family).  XT136: doc/xhorse_notes.md.
typedef enum {
    XHORSE_FAM_NONE = 0,
    XHORSE_FAM_XT27A,
    XHORSE_FAM_XT136,
} xhorse_family_t;

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
    uint8_t  found;           // 1 => an XT27A or XT136 answered the handshake (see family)
    uint8_t  typebyte;        // emulated type byte read from bank-1 0x3C (XT27A only)
    uint8_t  typebyte_ok;     // 1 => two consecutive reads of 0x3C agreed
    uint8_t  bank;            // bank image[] was read from (dump: 0 or 1, as requested; info: 0)
    uint8_t  chipid[16];      // 16-byte unique chip ID (bank-1 0x33..0x36; XT27A only)
    uint8_t  image[64 * 4];   // XT27A: pages 0x00..0x3F of `bank` (dump), bank-0 0x00..0x07 (info);
                              // XT136: pages 0x00..0x3F (dump), 0x00..0x0F (info), no banks
    uint8_t  image_pages;     // how many pages of `bank` are in image[]
    // XMICRO / "channel B" probe (XT57 / XT37-class chips).  An XT27A set to some types (7938)
    // answers XMICRO too, but only a newer, not-yet-supported family names itself ("XT37",
    // "XT57") in reply to the identify query that follows.
    uint8_t  bchip;           // 1 => a B-channel (XMICRO) chip answered the identify probe
    uint8_t  bchip_type_len;  // number of ASCII bytes in bchip_type[]
    uint8_t  bchip_type[8];   // ASCII family string the chip reported (e.g. "XT37")
    uint8_t  page_ok[8];      // bit (n % 8) of byte n / 8 set => two consecutive reads of page n agreed
    uint8_t  family;          // xhorse_family_t
} PACKED lf_xhorse_info_t;

// XT57B (channel B / XMICRO) nonce collection, `lf xhorse xt57 nonce`.  doc/xhorse_notes.md.
// Session: field on, XMICRO -> 0x5AA5, 0011 -> "XT57X", 104-bit select -> 32-bit nonce, 32-bit answer -> 64-bit reply.
// The answer and reply depend only on the nonce; XHORSE_XT57_KNOWN lists the (nonce, answer, reply) triples seen
// from a VVDI mini, X(nonce, answer, reply_hi, reply_lo).
#define XHORSE_XT57_KNOWN     X(0x2CFE132E, 0xFB33A1F1, 0xE58572D5, 0xE585F912)     X(0x59FC265C, 0x4FBDCA62, 0xB1F0879E, 0x404AFF59)     X(0xB3F84CB9, 0x2E21740A, 0xCC4B51BC, 0x6A69F0B3)     X(0x67F09973, 0x99AAEF6B, 0x55B462CA, 0x132478D0)     X(0xCFE132E7, 0xA9CBEA5B, 0xB72636B1, 0x155D7F00)     X(0x9FC265CF, 0xC9ABF021, 0x983E9981, 0x0367AD7A)     X(0x7F09973E, 0xA0D63F3E, 0x93D4AEEB, 0x592AE381)     X(0xFE132E7C, 0x7CCA106D, 0x6BECC747, 0x19267F38)     X(0xFC265CF9, 0xF24F7DBA, 0x919B81F2, 0x37FC0428)     X(0xF9B735D1, 0x914F41D0, 0xE4F40216, 0xE2070455)     X(0xCDB9AE8C, 0x1BA796D1, 0x79B67285, 0x0078F49D)

#define XHORSE_XT57_SELLEN   13      // select frame: 0xC0 "XT57" + 8 payload bytes (104 bits)
#define XHORSE_XT57_MAXSESS  16      // sessions per firmware call (the reply must fit one packet)

// Which gap `base + i * step` is applied to.
typedef enum {
    XT57_SWEEP_POWERUP = 0,   // field on -> XMICRO, us (default 120140)
    XT57_SWEEP_ACK_ID,        // 0x5AA5 -> 0011, T0 since the last tag edge (default 88)
    XT57_SWEEP_ID_SEL,        // "XT57X" -> select, T0 (default 82)
    XT57_SWEEP_NONCE_ANS,     // nonce -> answer, T0 (default 144)
    XT57_SWEEP_FIELD_OFF,     // field-off time before the session, us (default 20000)
    XT57_SWEEP_NONE,          // nothing: repeat the default session
} xt57_sweep_t;

typedef enum {
    XT57_ANS_KNOWN = 0,       // answer only if the nonce is in XHORSE_XT57_KNOWN
    XT57_ANS_FIXED,           // always send `answer`
    XT57_ANS_NONE,            // never answer: record the nonce only
} xt57_answer_t;

typedef struct {
    uint8_t  sweep;           // xt57_sweep_t
    uint8_t  answer_mode;     // xt57_answer_t
    uint8_t  count;           // sessions this call, 1..XHORSE_XT57_MAXSESS
    uint8_t  rfu;
    int32_t  base;            // swept value of session 0
    int32_t  step;            // added per session
    uint32_t answer;          // for XT57_ANS_FIXED
    uint32_t off_us;          // field-off time before every session, us (0 = 20000)
    uint8_t  select[XHORSE_XT57_SELLEN];
} PACKED lf_xhorse_xt57_t;

#define XT57_F_ACK     0x01   // 0x5AA5 seen
#define XT57_F_IDENT   0x02   // reply to 0011 started "XT"
#define XT57_F_NONCE   0x04   // 32-bit nonce demodulated
#define XT57_F_ANSWER  0x08   // an answer was sent

typedef struct {
    uint8_t  flags;           // XT57_F_*
    uint8_t  ident[5];        // reply to 0011
    uint8_t  nonce_bits;      // bits demodulated for the nonce (32 expected)
    uint8_t  nonce[4];
    uint8_t  reply_bits;      // bits demodulated for the reply (64 expected)
    uint8_t  reply[8];
    uint8_t  answer[4];       // answer sent (valid with XT57_F_ANSWER)
    uint8_t  ack[2];          // reply to XMICRO
} PACKED lf_xhorse_xt57_rec_t;

typedef struct {
    uint8_t  count;
    lf_xhorse_xt57_rec_t rec[XHORSE_XT57_MAXSESS];
} PACKED lf_xhorse_xt57_reply_t;

#endif // XHORSE_H__
