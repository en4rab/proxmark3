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
// Autel AT100 programmable transponder - device side.
//
// The AT100's management interface is stock HITAG2: BPLM reader frames, Manchester
// (MC4K) tag replies, and the stock 48-bit HITAG2 stream cipher from
// common/hitag2/hitag2_crypto.c.  What is specific to the chip is the entry
// sequence and the post-auth command format:
//
//   1010                       wake; the tag echoes it verbatim
//   1111100000                 get-UID (5-bit word + its complement); 32-bit UID in clear
//   {nR}{aR}                   standard HITAG2 auth, device key 9F63A5E8B14B, nR fixed
//   <32 bits>                  the tag volunteers page 0xFE (the emulated-type register)
//   {W}{~W} (20 bits)          op:2 | page:8 - 01 = READ, 10 = WRITE
//
// All of it is decoded, verified and timed in docs/at100.md; the timings below are
// the medians measured over the 29 Autel captures in data/traces/at100/.
//
// This file holds no knowledge of emulated types.  Every type is programmed the
// same way on air - write page 0xFE, then write the image pages - so the client
// owns the type catalogue and sends the page list (lf_autel_cfg_t).
//-----------------------------------------------------------------------------

#include "autel.h"
#include "string.h"
#include "proxmark3_arm.h"
#include "cmd.h"             // reply_ng (pulls in pm3_cmd.h: CMD_*/PM3_* codes)
#include "fpga_apis.h"       // FPGA_LF_EDGE_DETECT_*
#include "ticks_apis.h"
#include "gpio_apis.h"    // Gpio_SSC_DOUT_High/Low (field on/off)
#include "dbprint.h"
#include "util.h"
#include "commonutil.h"
#include "hitag_common.h"    // hitag_mod_t, T0, HITAG_T_*
#include "hitag2_crypto.h"   // ht2_hitag2_init / ht2_hitag2_word

//-----------------------------------------------------------------------------
// Physical layer.  Identical to the XT27A's (armsrc/xhorse.c) because both are a
// HITAG2 front end: drive the coil, but detect the weak load-modulated reply with
// slope + sensitive edge detection rather than level slicing.
//-----------------------------------------------------------------------------
#define AT_MOD          MC4K     // tag replies: HITAG2 Manchester, 4 kbit/s
#define AT_SOF_BITS     5        // tag reply SOF '11111' (stripped on receive)
#define AT_SEND_SOF     0        // reader sends no SOF (standard HITAG2)
#define AT_EDGE_MODE    (FPGA_LF_EDGE_DETECT_READER_FIELD | FPGA_LF_EDGE_DETECT_SLOPE | FPGA_LF_EDGE_DETECT_SENSITIVE)
#define AT_FPGA_THR     20       // slope-mode edge-detect threshold

//-----------------------------------------------------------------------------
// Timings, in T0 (8 us) unless named _US.  Medians from data/traces/at100/.
// t_wait values are T0 since the tag's last edge, and the precision counter is
// 16 bit (5461 T0), so anything longer is spun out with at_delay_us() instead.
//-----------------------------------------------------------------------------
#define AT_T_WAKE_T0        0        // first frame of a session: no tag edge to wait from
#define AT_T_UID_T0       725        // tag echoes 1010 -> reader sends get-UID
#define AT_T_AUTH_T0      456        // tag sends UID    -> reader sends {nR}{aR}
#define AT_T_CMD_T0       520        // tag reply        -> next 20-bit command (observed 513)
#define AT_T_DATA_T0      513        // tag echoes a WRITE -> reader sends the 32-bit data frame

// The Autel leaves 76 ms between the post-auth token and its first command.  That is
// almost certainly its own latency rather than a chip requirement, but replaying it
// costs one 76 ms wait per session and removes a variable from the first bring-up.
#define AT_T_FIRSTCMD_US  76000

// Data frame -> next command: 12209 T0 (97.7 ms) in every capture, i.e. the EEPROM
// program cycle.  100 ms gives a small margin.
#define AT_T_PROG_US     100000

// Field off between sessions.  The Autel uses anything from 13 to 185 ms; 20 ms is
// the shortest it ever used and is enough to reset the chip.  It then sends the wake
// about 1 ms after the field returns.
#define AT_T_FIELD_OFF_US 20000
#define AT_T_POWERUP_US    1000
#define AT_T_POWERUP_ALT_US 20000    // second timing tried if the first gets no answer

#define AT_TRIES            2        // session-open attempts per entry of at_timings[]
#define AT_VERIFY_READS     3        // re-reads looking for two that agree

// Opcodes in the 10-bit command word: op:2 | page:8.
#define AT_OP_READ        0x100
#define AT_OP_WRITE       0x200

// Every config word observed ends in 0x9B.  Used as the "this session is really
// open and the key is right" check, the way xhorse.c uses its 4R sel echo.
#define AT_CFG_TAIL       0x9B

// Session-open timing schedule, tried in order.  The first entry is what the Autel
// uses between the sessions of one job.  The later, much longer field-off entries
// are for a chip left emulating a talk-first type (EM4102 and friends): it then
// load-modulates continuously, and only a genuine cold power-down gets it to listen
// for the wake again.  Before its first wake to such a chip the Autel holds the
// field off for seconds (data/traces/at100/autel-at100-set-ID11.pm3 is dead air for
// 3.0 s), then sends the wake ~1 ms after the field returns.
typedef struct {
    uint32_t off_us;
    uint32_t on_us;
} at_timing_t;

static const at_timing_t at_timings[] = {
    {  AT_T_FIELD_OFF_US, AT_T_POWERUP_US     },
    {  AT_T_FIELD_OFF_US, AT_T_POWERUP_ALT_US },
    {  120000,            AT_T_POWERUP_US     },
    {  500000,            AT_T_POWERUP_US     },
    { 1500000,            AT_T_POWERUP_US     },
    { 3000000,            AT_T_POWERUP_US     },
};

//-----------------------------------------------------------------------------
// Session
//-----------------------------------------------------------------------------
typedef struct {
    uint64_t cs;        // HITAG2 cipher state
    bool     led;
    bool     auth_ok;   // the post-auth token looked like a config word
    uint32_t uid;
    uint32_t config;    // page 0xFE, as volunteered right after the auth
} at_sess_t;

// Reverse the low `n` bits of x.  ht2_hitag2_init() wants the key bit-reversed
// over its 48 bits and the UID / IV bit-reversed over 32 (docs/at100.md SS6a).
static uint64_t at_rev(uint64_t x, uint8_t n) {
    uint64_t r = 0;
    for (uint8_t i = 0; i < n; i++) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

// Busy-wait `us` microseconds with the field up.  SpinDelay()/WaitMS() don't work
// here because hitag_setup_fpga() stopped the system ticks, so spin on the precision
// counter (1.5 ticks/us).  It is 16 bit (wraps at ~43 ms), so chunk and pet the WDT.
static void at_delay_us(uint32_t us) {
    while (us) {
        uint32_t chunk = (us > 20000) ? 20000 : us;
        uint16_t ticks = (uint16_t)((chunk * 3) / 2);
        uint16_t ref = GetPrecisionCounterRaw();
        while (GetPrecisionCounterDelta(ref) < ticks) {};
        WDT_HIT();
        us -= chunk;
    }
}

static uint32_t at_bits_to_u32(const uint8_t *rx, uint8_t nbits) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < nbits; i++) {
        v = (v << 1) | ((rx[i / 8] >> (7 - (i % 8))) & 1);
    }
    return v;
}

// Pack `nbits` of `v` (MSB-first) into a transmit buffer.
static void at_u32_to_bits(uint32_t v, uint8_t nbits, uint8_t *tx) {
    memset(tx, 0, (nbits + 7) / 8);
    for (uint8_t i = 0; i < nbits; i++) {
        if ((v >> (nbits - 1 - i)) & 1) {
            tx[i / 8] |= (uint8_t)(1 << (7 - (i % 8)));
        }
    }
}

// Next `nbits` of keystream, MSB-first.  Must be called for EVERY on-air frame in
// both directions, whether or not our demod of it succeeded: the chip advances its
// cipher per frame, so skipping one desynchronises the rest of the session.
static uint32_t at_ks(at_sess_t *s, uint8_t nbits) {
    return ht2_hitag2_word(&s->cs, nbits);
}

static void at_setup(at_sess_t *s, bool ledcontrol) {
    memset(s, 0, sizeof(*s));
    s->led = ledcontrol;
    hitag_setup_fpga(AT_EDGE_MODE, AT_FPGA_THR, ledcontrol);
}

// Power-cycle the chip: coil off (DOUT high = modulation), then on again.
static void at_field_reset(uint32_t off_us, uint32_t powerup_us) {
    Gpio_SSC_DOUT_High();
    at_delay_us(off_us);
    Gpio_SSC_DOUT_Low();
    at_delay_us(powerup_us);
}

// Send a cleartext reader frame and demodulate the tag's reply.  Returns reply bits.
static size_t at_xfer(at_sess_t *s, const uint8_t *tx, size_t txbits, uint8_t *rx, size_t rxsz, int t_wait) {
    size_t rxlen = 0;
    memset(rx, 0, rxsz);
    hitag_reader_transfer(tx, txbits, rx, rxsz, &rxlen, t_wait, s->led, AT_MOD, AT_SOF_BITS, AT_SEND_SOF);
    return rxlen;
}

//-----------------------------------------------------------------------------
// Post-auth commands
//-----------------------------------------------------------------------------
// A command is the 10-bit word followed by its complement, the whole 20 bits
// encrypted.  The complement is the chip's own integrity check, and a tag echo that
// decrypts back to the same word is ours.
static uint32_t at_cmd_word(uint32_t op, uint8_t page) {
    uint32_t w = (op | page) & 0x3FF;
    return (w << 10) | (~w & 0x3FF);
}

// Send one 20-bit command and demodulate a reply of `reply_bits` (20 for a WRITE
// echo, 32 for a READ).  The keystream advances for both frames regardless.
static int at_cmd(at_sess_t *s, uint32_t op, uint8_t page, uint8_t reply_bits, int t_wait, uint32_t *reply) {
    uint32_t pt = at_cmd_word(op, page);
    uint8_t tx[3];
    at_u32_to_bits(pt ^ at_ks(s, 20), 20, tx);

    uint8_t rx[8];
    size_t rxlen = at_xfer(s, tx, 20, rx, sizeof(rx), t_wait);

    uint32_t v = at_bits_to_u32(rx, reply_bits) ^ at_ks(s, reply_bits);
    if (reply) {
        *reply = v;
    }
    return (rxlen >= (size_t)reply_bits) ? PM3_SUCCESS : PM3_EFAILED;
}

// READ one page.
static int at_read(at_sess_t *s, uint8_t page, int t_wait, uint32_t *val) {
    return at_cmd(s, AT_OP_READ, page, 32, t_wait, val);
}

// READ until two consecutive reads agree.  A weak Manchester reply can demodulate to
// a wrong value with nothing to show for it, so one read is never trusted.
// Returns false (with the last value read) if no two agreed.
static bool at_read_agreed(at_sess_t *s, uint8_t page, uint32_t *val) {
    uint32_t prev = 0;
    bool prev_ok = (at_read(s, page, AT_T_CMD_T0, &prev) == PM3_SUCCESS);
    for (int i = 0; i < AT_VERIFY_READS; i++) {
        uint32_t cur = 0;
        bool cur_ok = (at_read(s, page, AT_T_CMD_T0, &cur) == PM3_SUCCESS);
        if (prev_ok && cur_ok && (cur == prev)) {
            *val = cur;
            return true;
        }
        prev = cur;
        prev_ok = cur_ok;
    }
    *val = prev;
    return false;
}

// WRITE one page: command, tag echo of the command, 32-bit data frame, program wait.
// The chip sends no acknowledge after the data; the Autel simply waits out the EEPROM.
static int at_write(at_sess_t *s, uint8_t page, uint32_t data, bool verify) {
    uint32_t want = at_cmd_word(AT_OP_WRITE, page);
    uint32_t echo = 0;
    at_cmd(s, AT_OP_WRITE, page, 20, AT_T_CMD_T0, &echo);
    if (echo != want) {
        // Not fatal on its own - the echo is only our confidence check - but it means
        // either the demod missed it or the session has lost cipher sync.
        if (g_dbglevel >= DBG_ERROR) {
            Dbprintf("autel: page 0x%02X write echo %05x, expected %05x", page, echo, want);
        }
    }

    // Space the data frame off the tag's echo exactly as the Autel does.
    while (GetPrecisionCounter() < T0 * AT_T_DATA_T0) {};

    uint8_t tx[4];
    at_u32_to_bits(data ^ at_ks(s, 32), 32, tx);
    hitag_reader_send_frame(tx, 32, s->led, AT_SEND_SOF);

    at_delay_us(AT_T_PROG_US);

    if (verify == false) {
        return (echo == want) ? PM3_SUCCESS : PM3_EFAILED;
    }
    for (int i = 0; i < AT_VERIFY_READS; i++) {
        uint32_t rb = 0;
        // t_wait 0: at_delay_us() has already spun past any inter-frame gap, and the
        // 16-bit counter has wrapped, so a t_wait here could stall for up to 43 ms.
        at_read(s, page, (i == 0) ? 0 : AT_T_CMD_T0, &rb);
        if (rb == data) {
            return PM3_SUCCESS;
        }
    }
    if (g_dbglevel >= DBG_ERROR) {
        Dbprintf("autel: verify failed, page 0x%02X", page);
    }
    return PM3_EFAILED;
}

//-----------------------------------------------------------------------------
// Session open: wake, get-UID, authenticate.
//-----------------------------------------------------------------------------
static int at_open_once(at_sess_t *s, uint64_t key, uint32_t off_us, uint32_t powerup_us) {
    static const uint8_t wake[1]   = { 0xA0 };          // 4 bits: 1010
    static const uint8_t getuid[2] = { 0xF8, 0x00 };    // 10 bits: 11111 00000

    at_field_reset(off_us, powerup_us);

    uint8_t rx[8];
    if (at_xfer(s, wake, 4, rx, sizeof(rx), AT_T_WAKE_T0) < 4) {
        return PM3_EFAILED;                             // nothing on the coil
    }
    if (at_bits_to_u32(rx, 4) != 0x0A) {
        return PM3_EFAILED;                             // something answered, but not an AT100
    }

    if (at_xfer(s, getuid, 10, rx, sizeof(rx), AT_T_UID_T0) < 32) {
        return PM3_EFAILED;
    }
    s->uid = at_bits_to_u32(rx, 32);

    s->cs = ht2_hitag2_init(at_rev(key, 48), (uint32_t)at_rev(s->uid, 32), (uint32_t)at_rev(AUTEL_READER_NR, 32));
    uint32_t aR = at_ks(s, 32) ^ 0xFFFFFFFFUL;

    uint8_t tx[8];
    at_u32_to_bits(AUTEL_READER_NR, 32, tx);
    at_u32_to_bits(aR, 32, tx + 4);

    size_t rxlen = at_xfer(s, tx, 64, rx, sizeof(rx), AT_T_AUTH_T0);
    s->config = at_bits_to_u32(rx, 32) ^ at_ks(s, 32);
    if (rxlen < 32) {
        return PM3_EFAILED;
    }
    s->auth_ok = ((s->config & 0xFF) == AT_CFG_TAIL);

    // The Autel's own gap before its first command.
    at_delay_us(AT_T_FIRSTCMD_US);
    return PM3_SUCCESS;
}

// Open a session, alternating two power-up timings.  A session cannot be saved and
// restored: a successful open leaves the chip authenticated *now*, and any retry
// power-cycles it, which resets its cipher.  So an open whose post-auth token does
// not look like a config word (usually a mis-demodulated UID, hence the wrong cipher
// init - but possibly the wrong key) is only retried while retries remain; on the
// last one it is returned as-is, and the caller warns about it.
static int at_open_tuned(at_sess_t *s, uint64_t key, uint32_t off_us, uint32_t on_us, int tries) {
    for (int attempt = 0; attempt < tries; attempt++) {
        int res = at_open_once(s, key, off_us, on_us);
        if (g_dbglevel >= DBG_EXTENDED) {
            Dbprintf("autel: off %u us on %u us try %d: res %d uid %08x cfg %08x",
                     off_us, on_us, attempt, res, s->uid, s->config);
        }
        if (res != PM3_SUCCESS) {
            continue;
        }
        if (s->auth_ok || (attempt + 1 == tries)) {
            return PM3_SUCCESS;
        }
    }
    return PM3_EFAILED;
}

static int at_open_ex(at_sess_t *s, uint64_t key, uint32_t off_us, uint32_t on_us) {
    if (off_us || on_us) {
        return at_open_tuned(s, key, off_us ? off_us : AT_T_FIELD_OFF_US,
                             on_us ? on_us : AT_T_POWERUP_US, AT_TRIES);
    }
    for (size_t i = 0; i < ARRAYLEN(at_timings); i++) {
        if (at_open_tuned(s, key, at_timings[i].off_us, at_timings[i].on_us, AT_TRIES) == PM3_SUCCESS) {
            return PM3_SUCCESS;
        }
    }
    return PM3_EFAILED;
}

static int at_open(at_sess_t *s, uint64_t key) {
    return at_open_ex(s, key, 0, 0);
}

//-----------------------------------------------------------------------------
// Reply helpers
//-----------------------------------------------------------------------------
static uint64_t at_key(uint64_t key) {
    return key ? (key & 0xFFFFFFFFFFFFULL) : AUTEL_DEFAULT_KEY;
}

static void at_store(lf_autel_io_reply_t *out, uint8_t i, uint8_t page, uint32_t w, bool agreed) {
    out->page[i] = page;
    if (agreed) {
        out->ok[i / 8] |= (uint8_t)(1 << (i % 8));
    }
    out->data[i * 4 + 0] = (uint8_t)(w >> 24);
    out->data[i * 4 + 1] = (uint8_t)(w >> 16);
    out->data[i * 4 + 2] = (uint8_t)(w >> 8);
    out->data[i * 4 + 3] = (uint8_t)(w);
}

static void at_fill_found(lf_autel_io_reply_t *out, const at_sess_t *s) {
    out->found = 1;
    out->auth_ok = s->auth_ok ? 1 : 0;
    out->uid = s->uid;
    out->config = s->config;
}

//-----------------------------------------------------------------------------
// Entry points
//-----------------------------------------------------------------------------
void Autel_Read(const lf_autel_io_t *p, bool ledcontrol) {
    at_sess_t s;
    lf_autel_io_reply_t out;
    memset(&out, 0, sizeof(out));

    uint8_t count = p->count ? p->count : 1;
    if (count > AUTEL_MAX_PAGES) {
        count = AUTEL_MAX_PAGES;
    }
    if ((uint16_t)p->page + count > 256) {
        count = (uint8_t)(256 - p->page);      // the address field is 8 bits; never wrap
    }

    at_setup(&s, ledcontrol);
    if (at_open_ex(&s, at_key(p->key), (uint32_t)p->off_ms * 1000, p->on_us) != PM3_SUCCESS) {
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_AUTEL_READ, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));   // found == 0
        return;
    }
    at_fill_found(&out, &s);

    for (uint8_t i = 0; i < count; i++) {
        uint32_t w = 0;
        uint8_t pg = (uint8_t)(p->page + i);
        bool agreed = at_read_agreed(&s, pg, &w);
        at_store(&out, i, pg, w, agreed);
    }
    out.count = count;

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_AUTEL_READ, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
}

void Autel_Write(const lf_autel_io_t *p, bool ledcontrol) {
    at_sess_t s;
    lf_autel_io_reply_t out;
    memset(&out, 0, sizeof(out));

    at_setup(&s, ledcontrol);
    if (at_open_ex(&s, at_key(p->key), (uint32_t)p->off_ms * 1000, p->on_us) != PM3_SUCCESS) {
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_AUTEL_WRITE, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));  // found == 0
        return;
    }
    at_fill_found(&out, &s);

    int res = at_write(&s, p->page, p->value, p->verify ? true : false);
    out.verified = (p->verify && (res == PM3_SUCCESS)) ? 1 : 0;

    // Always read the page back, verified or not, so the caller sees what is there.
    uint32_t rb = 0;
    bool agreed = at_read_agreed(&s, p->page, &rb);
    at_store(&out, 0, p->page, rb, agreed);
    out.count = 1;

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_AUTEL_WRITE, res, (uint8_t *)&out, sizeof(out));
}

// Pages the Autel is ever seen to use (docs/at100.md SS8).  Everything else in the
// 256-page space is untouched in every capture; `lf autel read` reaches those.
static const uint8_t at_dump_pages[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x3A, 0x3B, 0x3C, 0x3D,
    0x50,
    0x78, 0x79, 0x7A, 0x7B,
    0xC8,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4,
    0xFC, 0xFE,
};

void Autel_Dump(const lf_autel_io_t *p, bool ledcontrol) {
    at_sess_t s;
    lf_autel_io_reply_t out;
    memset(&out, 0, sizeof(out));

    at_setup(&s, ledcontrol);
    if (at_open_ex(&s, at_key(p->key), (uint32_t)p->off_ms * 1000, p->on_us) != PM3_SUCCESS) {
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_AUTEL_DUMP, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));   // found == 0
        return;
    }
    at_fill_found(&out, &s);

    uint8_t n = (uint8_t)ARRAYLEN(at_dump_pages);
    if (n > AUTEL_MAX_PAGES) {
        n = AUTEL_MAX_PAGES;
    }
    for (uint8_t i = 0; i < n; i++) {
        uint32_t w = 0;
        bool agreed = at_read_agreed(&s, at_dump_pages[i], &w);
        at_store(&out, i, at_dump_pages[i], w, agreed);
    }
    out.count = n;

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_AUTEL_DUMP, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
}

// Replay a page-write burst in one authenticated session, which is how the Autel
// programs every type.  `cmd` is only the reply tag: configure and reset are the
// same operation with a different page list.
void Autel_Configure(const lf_autel_cfg_t *p, uint16_t cmd, bool ledcontrol) {
    at_sess_t s;
    lf_autel_cfg_reply_t out;
    memset(&out, 0, sizeof(out));

    uint8_t count = p->count;
    if (count > AUTEL_MAX_WRITES) {
        count = AUTEL_MAX_WRITES;
    }
    if (count == 0) {
        reply_ng(cmd, PM3_EINVARG, (uint8_t *)&out, sizeof(out));
        return;
    }

    at_setup(&s, ledcontrol);
    if (at_open(&s, at_key(p->key)) != PM3_SUCCESS) {
        hitag_cleanup(ledcontrol);
        reply_ng(cmd, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));                 // found == 0
        return;
    }
    out.found = 1;
    out.auth_ok = s.auth_ok ? 1 : 0;
    out.uid = s.uid;
    out.config_before = s.config;

    int res = PM3_SUCCESS;
    for (uint8_t i = 0; i < count; i++) {
        if (at_write(&s, p->w[i].page, p->w[i].value, p->verify ? true : false) != PM3_SUCCESS) {
            out.failed_page = p->w[i].page;
            res = PM3_EFAILED;
            break;
        }
        out.written = (uint8_t)(i + 1);
        WDT_HIT();
    }

    // Re-read the type register so the caller is told what the chip now is, whether
    // or not the burst completed.
    uint32_t cfg = 0;
    if (at_read_agreed(&s, AUTEL_PG_CONFIG, &cfg)) {
        out.config_after = cfg;
    }

    hitag_cleanup(ledcontrol);
    reply_ng(cmd, res, (uint8_t *)&out, sizeof(out));
}
