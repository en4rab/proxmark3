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

// An XT136 has pages 0x00..0x0F; a read of a higher address aliases back into them.
#define XHORSE_XT136_PAGES  16

// An XT14D addresses pages with a whole byte, not the XT27A's 6-bit field, so a command can name
// 0x00..0xFF.  Measured on hardware (docs/xt14d.md SS7), what is behind those 256 addresses is:
//
//   0x00..0x4F   80 pages of real storage - the whole chip, 80 x 16 bits = 160 bytes
//   0x50..0x7F   nothing.  Reads answer with a CRC-valid 00000000 and writes are ACKed with
//                5AA5 and then discarded, so this region is invisible to a read-only sweep
//   0x80..0xFF   an exact alias of 0x00..0x7F: address bit 7 is ignored on read *and* write
//
// `read` still reaches all 256, because being able to demonstrate the alias and the dead region
// is worth keeping; `dump` reads the 80 real ones.
#define XHORSE_XT14D_PAGES       256   // addressable, alias included
#define XHORSE_XT14D_REAL_PAGES  80    // 0x00..0x4F, the pages actually backed by storage

// How many pages one read/write reply can carry (lf_xhorse_io_reply_t.data / .ok / .ans).
#define XHORSE_IO_MAXPAGES  64

// How many pages one info/dump reply can carry (lf_xhorse_info_t.image / .page_ok).  The XT14D's
// 80 is the largest of the families; an XT27A bank is 64 and an XT136 is 16.
#define XHORSE_INFO_MAXPAGES  XHORSE_XT14D_REAL_PAGES

//-----------------------------------------------------------------------------
// Emulated-image encoders
//-----------------------------------------------------------------------------
// The emulated image is the same on both channels: an XT27A's channel-A bank 0 and an XT57B's
// XMICRO block 0x00 hold the same words in the same pages (docs/xt57b-fullread.md SS12.8).  So
// these live here rather than in armsrc/xhorse.c: the firmware builds channel-A images with
// them, the client builds XMICRO session scripts with them.
static inline uint8_t xh_bitrev8(uint8_t b) {
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

// ID11/ID12 bitrev packing -> image pages 0x00..0x04 (header passed in).
static inline void xh_pack_em8(const uint8_t *uid, uint16_t header, uint16_t pages[5]) {
    uint8_t r[8];
    for (int i = 0; i < 8; i++) {
        r[i] = xh_bitrev8(uid[i]);
    }
    pages[0x00] = header;
    pages[0x02] = (uint16_t)((r[0] << 8) | r[1]);
    pages[0x01] = (uint16_t)((r[2] << 8) | r[3]);
    pages[0x04] = (uint16_t)((r[4] << 8) | r[5]);
    pages[0x03] = (uint16_t)((r[6] << 8) | r[7]);
}

// EM4102-encode 5 (already bit-reversed) bytes into a 64-bit stream, then
// bit-reverse each output byte -> 8 bytes, matching ID13.
static inline void xh_id13_stream(const uint8_t *uid5, uint8_t out8[8]) {
    uint8_t rb[5];
    for (int i = 0; i < 5; i++) {
        rb[i] = xh_bitrev8(uid5[i]);
    }
    uint8_t bits[64];
    int n = 0;
    for (int i = 0; i < 9; i++) {
        bits[n++] = 1;             // 9 header ones
    }
    uint8_t col[4] = {0, 0, 0, 0};
    for (int by = 0; by < 5; by++) {
        for (int half = 0; half < 2; half++) {
            uint8_t nib = half ? (rb[by] & 0x0F) : (rb[by] >> 4);
            uint8_t row[4] = { (uint8_t)((nib >> 3) & 1), (uint8_t)((nib >> 2) & 1),
                               (uint8_t)((nib >> 1) & 1), (uint8_t)(nib & 1)
                             };
            uint8_t rp = 0;
            for (int k = 0; k < 4; k++) {
                bits[n++] = row[k];
                rp ^= row[k];
                col[k] ^= row[k];
            }
            bits[n++] = rp;        // even row parity
        }
    }
    for (int k = 0; k < 4; k++) {
        bits[n++] = col[k];        // even column parity
    }
    bits[n++] = 0;                 // stop bit  -> 64 bits total

    for (int i = 0; i < 8; i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++) {
            v = (uint8_t)((v << 1) | bits[i * 8 + b]);
        }
        out8[i] = xh_bitrev8(v);   // bit-reverse each byte
    }
}

// Channel-A family that answered (lf_xhorse_info_t.family).  XT136: doc/xhorse_notes.md.
// XT14D: docs/xt14d.md - same LFSR and same bitrev16 seed rule as the other two, but it answers
// XHORSE with 4005+CRC-8 instead of 5AA5, takes an 8-bit E7 seed query, and its page I/O is
// byte-oriented 4B/5A frames with a CRC-8 over the plaintext rather than 16-bit op,~op.
typedef enum {
    XHORSE_FAM_NONE = 0,
    XHORSE_FAM_XT27A,
    XHORSE_FAM_XT136,
    XHORSE_FAM_XT14D,
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

// Which ID8A probe word xh_open_session may send as its fourth timing.  The probe is the only
// way to reach a chip set to ID8A, but word B reverts the chip's emulated type byte to 0x36
// (implementation-plan.md Update 29), so a read command sends nothing unless asked to.
typedef enum {
    XHORSE_PROBE_NONE = 0,    // timings 0..2 only - safe, but an ID8A chip will not answer
    XHORSE_PROBE_B,           // the word the VVDI sends last; opens the chip, type byte -> 0x36
    XHORSE_PROBE_A,           // the word it sends first, twice; opens it too, type byte -> 0x00
    XHORSE_PROBE_CUSTOM,      // a word supplied in lf_xhorse_rdopt_t.word - for probing the code
} xhorse_probe_t;

#define XHORSE_ID8A_SYMS  98      // S/L symbols in one ID8A probe word

// Client -> firmware payload for `lf xhorse info` and `lf xhorse dump`.  Both are optional: an
// empty packet means bank 0 with no probe, which is what older clients send.
typedef struct {
    uint8_t  bank;        // dump: 0 = emulated image, 1 = config/identity.  info: unused
    uint8_t  probe;       // xhorse_probe_t
    // XHORSE_PROBE_CUSTOM only: the 98 symbols to send in place of word A or B, as 'S'/'L'.
    // The two known words differ in 20 of the 98, and what the differences mean is open
    // (docs/xt57b-fullread.md SS12.4), so this exists to ask the chip directly.  It is a research
    // control: the chip acts on whatever is sent, and a word it rejects opens no session.
    char     word[XHORSE_ID8A_SYMS];
} PACKED lf_xhorse_rdopt_t;

// Firmware -> client reply for info/dump.
typedef struct {
    uint8_t  found;           // 1 => an XT27A or XT136 answered the handshake (see family)
    uint8_t  typebyte;        // emulated type byte read from bank-1 0x3C (XT27A only)
    uint8_t  typebyte_ok;     // 1 => two consecutive reads of 0x3C agreed
    uint8_t  bank;            // bank image[] was read from (dump: 0 or 1, as requested; info: 0)
    uint8_t  chipid[16];      // 16-byte unique chip ID (bank-1 0x33..0x36; XT27A only)
    uint8_t  image[XHORSE_INFO_MAXPAGES * 4];   // XT27A: pages 0x00..0x3F of `bank` (dump),
                              // bank-0 0x00..0x07 (info); XT136: pages 0x00..0x0F, no banks;
                              // XT14D: pages 0x00..0x4F (dump), 0x00..0x0F (info), no banks
    uint8_t  image_pages;     // how many pages of `bank` are in image[]
    // XMICRO / "channel B" probe (XT57 / XT37-class chips).  An XT27A set to some types (7938)
    // answers XMICRO too, but only a newer, not-yet-supported family names itself ("XT37",
    // "XT57") in reply to the identify query that follows.  That reply is four family bytes plus a
    // CRC-8 over the command byte and the family, not a five-character string (docs/xt57-auth.md SS4).
    uint8_t  bchip;           // 1 => a B-channel (XMICRO) chip answered the identify probe
    uint8_t  bchip_type_len;  // number of ASCII bytes in bchip_type[]
    uint8_t  bchip_type[8];   // the raw identify reply: 4 ASCII family bytes + CRC (e.g. 5854333726)
    uint8_t  page_ok[(XHORSE_INFO_MAXPAGES + 7) / 8];   // bit (n % 8) of byte n / 8 set => two
                              // consecutive reads of page n agreed (XT14D: the read passed its CRC-8)
    uint8_t  family;          // xhorse_family_t
    uint16_t seed_reply;      // the tag's reply to 7372 (XT27A) / E430 (XT136) that seeds the cipher
    int16_t  em_slip_us;      // `xt57 reset`: how far off the VVDI's nominal burst offset the
                              // burst that actually freed the chip was, in us (0 = the nominal one)
    // The config words, read from the bank-1 pages they read back at (XT27A only).  Both `info` and
    // `dump` print them in their shared header, so the firmware reads them in the bank-1 session it
    // already opens for the type byte - two more page reads, no extra session.
    uint32_t cfgword[2];      // bank-1 0x3D, 0x3E
    uint8_t  cfgword_ok;      // bit 0 / bit 1 set => that word had two agreeing reads
} PACKED lf_xhorse_info_t;

// Client -> firmware payload for `lf xhorse read` / `lf xhorse write`: the raw channel-A page
// primitives.  A page address is 6 bits, so 0x00..0x3F is the whole of a bank; the XT136 has no
// banks and `bank` is ignored on one (doc/xhorse_notes.md).
typedef struct {
    uint8_t  bank;        // sel operand, 0..63 (XT27A: 0 = emulated image, 1 = config/identity)
    uint8_t  page;        // first page, 0x00..0x3F
    uint8_t  count;       // read: pages to read, 1..64; write: unused
    uint8_t  verify;      // write: 1 => read the page back and compare before reporting success
    uint8_t  cfg;         // write: 1 => `page` is a config-word index n, written by ctl 0x30+n
    // XT14D read only: how many times this call may close the session and open a fresh one after
    // a page that did not answer.  A page the chip ignores can leave the two keystreams out of
    // step, and it then ignores everything after it, so a read that crosses an address with no
    // storage behind it has to start again to stay meaningful (docs/xt14d.md SS6).  0 = never
    // resync: the read stops at the first such page and the client re-issues from there.
    uint8_t  resync;
    // Try this xhorse_family_t's handshake first (0 = probe all, in the usual order).  Finding an
    // XT14D costs an XT27A probe and an XT136 probe first, about 2.9 s, which a sweep would pay
    // again on every call; the client passes the family its first call reported so the rest go
    // straight to it.  A wrong hint only costs one extra handshake: the full probe still follows.
    uint8_t  family;
    uint8_t  rfu;
    uint32_t value;       // write: the 32-bit value
} PACKED lf_xhorse_io_t;

// Firmware -> client reply for both.  A write returns count = 1 and the page as it reads after the
// write, so the caller always sees what is actually there.
typedef struct {
    uint8_t  found;       // 1 => a chip answered the handshake
    uint8_t  family;      // xhorse_family_t that answered
    uint8_t  bank;        // bank the pages came from; meaningless on an XT136
    uint8_t  page;        // page number of data[0]
    uint8_t  count;       // pages in data[]
    uint8_t  verified;    // write: 1 => a read-back matched the value written
    uint8_t  resyncs;     // XT14D read: sessions reopened mid-read.  `count` short of what was
    //                       asked for means it stopped early; the caller continues from page+count.
    uint8_t  rfu;
    uint8_t  ok[8];       // bit (i % 8) of byte i / 8 set => data[i] is trusted: two agreeing reads
    //                       on an XT27A/XT136, a valid CRC-8 on an XT14D
    // XT14D only: bit set => the chip answered that read at all.  A page it answers with a bad CRC
    // and a page it ignores are different findings when sweeping for which addresses are backed by
    // real storage, and a silent page may also leave the two keystreams out of step, so everything
    // after it in the same session is suspect (docs/xt14d.md SS6).
    uint8_t  ans[8];
    uint8_t  data[64 * 4];
} PACKED lf_xhorse_io_reply_t;

// XT57B (channel B / XMICRO) nonce collection, `lf xhorse xt57 nonce`.  doc/xhorse_notes.md.
// Session: field on, XMICRO -> 0x5AA5, 0011 -> "XT57" + CRC, 104-bit select -> 32-bit nonce, 32-bit answer -> 64-bit reply.
// The answer and reply depend only on the nonce; XHORSE_XT57_KNOWN lists the (nonce, answer, reply) triples seen
// from a VVDI mini, X(nonce, answer, reply_hi, reply_lo).
#define XHORSE_XT57_KNOWN     X(0x2CFE132E, 0xFB33A1F1, 0xE58572D5, 0xE585F912)     X(0x59FC265C, 0x4FBDCA62, 0xB1F0879E, 0x404AFF59)     X(0xB3F84CB9, 0x2E21740A, 0xCC4B51BC, 0x6A69F0B3)     X(0x67F09973, 0x99AAEF6B, 0x55B462CA, 0x132478D0)     X(0xCFE132E7, 0xA9CBEA5B, 0xB72636B1, 0x155D7F00)     X(0x9FC265CF, 0xC9ABF021, 0x983E9981, 0x0367AD7A)     X(0x7F09973E, 0xA0D63F3E, 0x93D4AEEB, 0x592AE381)     X(0xFE132E7C, 0x7CCA106D, 0x6BECC747, 0x19267F38)     X(0xFC265CF9, 0xF24F7DBA, 0x919B81F2, 0x37FC0428)     X(0xF9B735D1, 0x914F41D0, 0xE4F40216, 0xE2070455)     X(0xCDB9AE8C, 0x1BA796D1, 0x79B67285, 0x0078F49D)     X(0x36E6BA31, 0xC64A3095, 0x703C397F, 0xC27ADBF5)     X(0x6DCD7463, 0x519F0319, 0x6B6E92C6, 0x91C91277)

#define XHORSE_XT57_SELLEN   13      // select frame: 0xC0 "XT57" + 8 payload bytes (104 bits)

#define XHORSE_XT57_MAXSESS  16      // sessions per firmware call (the reply must fit one packet)

// Post-authentication frame script.  The traffic after the 64-bit reply is XORed with a keystream
// fixed by (nonce, exchange index) alone - not by time and not by the command byte (doc/xt57-auth.md
// SS7.2, SS9.1).  So a session recorded at a nonce we can steer to can be replayed frame by frame,
// and any frame whose index keystream is known can be rebuilt to carry a chosen plaintext.  A frame
// at the wrong index is not a valid command and kills the session, so the script must keep the
// recorded length and index of every exchange it does not mean to change.
#define XHORSE_XT57_MAXPOST   64     // responses returned per call
#define XHORSE_XT57_POSTBYTES  6     // longest reader frame: a 48-bit write
#define XHORSE_XT57_RESPBYTES  8     // longest tag response kept
#define XHORSE_XT57_SCRIPTBYTES 560  // packed script: a whole VVDI session fits

// The script is a packed byte stream, one record per frame, so that a 180-frame session fits in a
// single 624 byte command:
//
//   [bits]            1..48, MSB-aligned data follows in ceil(bits/8) bytes
//   [0x80 | bits]     the same, but a 16 bit little-endian gap_t0 comes first (0 => default)
//
// A bits byte of 0 ends the script.

typedef struct {
    uint8_t  bits;                          // bits demodulated (0 => the tag said nothing)
    uint8_t  data[XHORSE_XT57_RESPBYTES];
} PACKED xt57_presp_t;

// Which gap `base + i * step` is applied to.
typedef enum {
    XT57_SWEEP_POWERUP = 0,   // field on -> XMICRO, us (default 120140)
    XT57_SWEEP_ACK_ID,        // 0x5AA5 -> 0011, T0 since the last tag edge (default 88)
    XT57_SWEEP_ID_SEL,        // identify reply -> select, T0 (default 82)
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
    uint8_t  selbits;         // bits of select[] to send, 0 => XHORSE_XT57_SELLEN * 8 (104)
    int32_t  base;            // swept value of session 0
    int32_t  step;            // added per session
    uint32_t answer;          // for XT57_ANS_FIXED
    uint32_t off_us;          // field-off time before every session, us (0 = 20000)
    uint32_t powerup_us;      // field-on -> XMICRO, us (0 = the VVDI's 120140).  Overridden by
    //                           the sweep when that is XT57_SWEEP_POWERUP.
    uint8_t  press_on;        // send the select even when the 0011 identify went unanswered -
    //                           a diagnostic for a chip that acks XMICRO and nothing else.
    uint8_t  sel_auto;        // 1 => overwrite select[1..4] with the family bytes the chip just
    //                           named, when the identify CRC validated and select[] is the
    //                           standard 0xC0 + family + 8 tail form.  A chip answers no nonce at
    //                           all to a select carrying someone else's family (docs/xt37.md SS3).
    uint8_t  select[XHORSE_XT57_SELLEN];
    // Post-auth script.  Only run when the session's nonce equals `want` (or want == 0), because a
    // swept delay lands on the wanted nonce only now and then and the keystream is nonce-specific.
    uint8_t  npost;           // frames in script[], 0 => no script (plain nonce logging)
    uint8_t  rfirst;          // 1-based index of the first frame whose response is returned (0 = 1)
    uint16_t scriptlen;       // bytes used in script[]
    uint32_t want;            // required nonce, 0 => any
    uint8_t  script[XHORSE_XT57_SCRIPTBYTES];
} PACKED lf_xhorse_xt57_t;

// `lf xhorse xt57 reset` parameters.  The burst has to land at a particular phase of the chip's
// broadcast frame, and the frame length is a property of the emulated type: 256 us for ID11 at
// RF/32, but 4096 us for a blanked PCF7935 (docs/xt57b-fullread.md SS12.11).  So how far the
// sweep walks has to be settable, or a type with a long frame is never freed.
typedef struct {
    uint8_t  force;           // send the bursts even if the chip already answers XMICRO
    uint8_t  probe_every;     // probe XMICRO every N swept bursts (0 = 8)
    int16_t  nsweep;          // swept positions (0 = 32), centred on `at_us`
    int32_t  at_us;           // centre of the sweep, us from power-up (0 = the VVDI's 109700)
    int32_t  step_us;         // sweep step (0 = 40)
    int32_t  on_us;           // field-on window holding one burst (0 = 156000)
    uint8_t  word;            // 0 = auto: "erase", then the 7935 word.  1 = "erase",
                              // 2 = the Megamos word (ID48/ID8E), 3 = the 7935 word
} PACKED lf_xhorse_xt57_reset_t;

#define XT57_F_ACK     0x01   // 0x5AA5 seen
#define XT57_F_IDENT   0x02   // reply to 0011 started "XT"
#define XT57_F_NONCE   0x04   // 32-bit nonce demodulated
#define XT57_F_ANSWER  0x08   // an answer was sent
#define XT57_F_IDCRC   0x10   // the identify reply's CRC-8 validated (docs/xt57-auth.md SS4)
#define XT57_F_SELFAM  0x20   // the select's family bytes were taken from that reply

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

// Firmware -> client when a post script ran.  Sent under the same command id; the client tells the
// two apart by the reply length, since a scripted call always runs exactly one session.
typedef struct {
    lf_xhorse_xt57_rec_t rec;               // the session the script ran in
    uint8_t  sessions;                      // sessions tried before the wanted nonce came up
    uint8_t  npost;                         // responses in presp[]
    uint8_t  rfirst;                        // 1-based index presp[0] belongs to
    uint8_t  nsent;                         // frames actually sent
    xt57_presp_t presp[XHORSE_XT57_MAXPOST];
} PACKED lf_xhorse_xt57_post_reply_t;

#endif // XHORSE_H__
