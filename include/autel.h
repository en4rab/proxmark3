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
// Autel AT100 programmable transponder - shared client/firmware definitions
//
// The AT100's management interface is a stock HITAG2 air interface (BPLM reader,
// Manchester tag) carrying the stock HITAG2 48-bit stream cipher, authenticated
// with a device key shared by every blank seen so far.  After authentication the
// chip exposes a flat 256-page x 32-bit space; page 0xFE is the emulated-type
// register and the low pages are the emulated transponder's image.
//
// Full protocol, memory map and per-type images: docs/at100.md.
//-----------------------------------------------------------------------------

#ifndef AUTEL_H__
#define AUTEL_H__

#include "common.h"

// Device key shared by every AT100 blank in the captures (docs/at100.md SS6a).
#define AUTEL_DEFAULT_KEY   0x9F63A5E8B14BULL

// The Autel programmer replays this reader nonce in every session, on every tag.
#define AUTEL_READER_NR     0xED355308UL

// Page 0xFE: the emulated-type register.  Auto-returned by the tag straight after
// a successful authentication, so one auth already reports the type.
#define AUTEL_PG_CONFIG     0xFE

// Page 0xFC: low 32 bits of the device key, identical on every blank.  The Autel
// reads it as its "is this a genuine AT100" check.
#define AUTEL_PG_KEYSIG     0xFC
#define AUTEL_KEYSIG        0xA5E8B14BUL

// Factory-default contents of page 0xFE: every unprogrammed blank read so far.
// It is also exactly what the Autel's own "ID4D" menu entry writes.
#define AUTEL_CFG_FACTORY   0x9B290A9BUL

// Pages per reply.  Also the cap on `lf autel read -n` and on the dump page list.
#define AUTEL_MAX_PAGES     64

// Page writes per `configure`.  The largest captured image is ID8A: page 0xFE
// plus pages 0x00..0x27, i.e. 41 writes.
#define AUTEL_MAX_WRITES    48

// Client -> firmware: `lf autel read` / `lf autel write` / `lf autel dump`.
// dump ignores page/count/value/verify and reads the firmware's own page list.
typedef struct {
    uint8_t  page;        // read: first page; write: the page
    uint8_t  count;       // read: pages to read, 1..AUTEL_MAX_PAGES
    uint8_t  verify;      // write: 1 => read the page back and compare
    uint8_t  rfu;
    uint32_t value;       // write: the 32-bit value
    uint64_t key;         // 48-bit device key; 0 => AUTEL_DEFAULT_KEY
    // Session-open timing override, for bring-up on a chip the default schedule
    // cannot reach.  Both 0 => use the built-in schedule.
    uint16_t off_ms;      // field-off time before the wake
    uint16_t on_us;       // field-on settle before the wake
    uint8_t  rfu2[4];
} PACKED lf_autel_io_t;

// Firmware -> client for read / write / dump.  `page[]` is carried per entry
// because a dump reads a non-contiguous list.
typedef struct {
    uint8_t  found;       // 1 => a chip answered the 1010 wake handshake
    uint8_t  auth_ok;     // 1 => the device key reproduced aR
    uint8_t  count;       // entries in page[]/data[]
    uint8_t  verified;    // write: 1 => the read-back matched the value written
    uint32_t uid;         // device UID, returned in clear before authentication
    uint32_t config;      // page 0xFE as the tag reported it after authentication
    uint8_t  page[AUTEL_MAX_PAGES];
    uint8_t  ok[AUTEL_MAX_PAGES / 8];   // bit (i%8) of byte i/8 => data[i] had two agreeing reads
    uint8_t  data[AUTEL_MAX_PAGES * 4];
} PACKED lf_autel_io_reply_t;

// One page write of a configure burst.
typedef struct {
    uint8_t  page;
    uint8_t  rfu[3];
    uint32_t value;
} PACKED autel_write_t;

// Client -> firmware: `lf autel configure` / `lf autel reset`.
//
// Every AT100 type is programmed the same way on air - authenticate, then write
// pages - so the whole per-type recipe is data, built by the client (which owns
// the type catalogue) and replayed verbatim here.  The firmware holds no type
// knowledge at all.
typedef struct {
    uint8_t  count;       // writes in w[], 1..AUTEL_MAX_WRITES
    uint8_t  verify;      // 1 => read every write back and compare
    uint8_t  rfu[6];      // pad: keeps `key` 8-byte aligned
    uint64_t key;         // 48-bit device key; 0 => AUTEL_DEFAULT_KEY
    autel_write_t w[AUTEL_MAX_WRITES];
} PACKED lf_autel_cfg_t;

// Firmware -> client for configure / reset.
typedef struct {
    uint8_t  found;
    uint8_t  auth_ok;
    uint8_t  written;     // writes that were sent before any failure
    uint8_t  failed_page; // page the burst stopped on (valid when written < count)
    uint32_t uid;
    uint32_t config_before; // page 0xFE as read at the start of the session
    uint32_t config_after;  // page 0xFE re-read at the end, or 0 if it was not read
} PACKED lf_autel_cfg_reply_t;

#endif // AUTEL_H__
