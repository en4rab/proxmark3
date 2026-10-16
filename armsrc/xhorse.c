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
// Xhorse super-chip (XT27A) firmware - "channel A" management protocol.
//
// Physical layer is HITAG2 BPLM, so this file reuses the non-static helpers in
// hitag_common.c (setup/cleanup, send-frame, receive-frame, transfer) for all
// modulation and timing.  What is NOT HITAG2 and lives only here:
//
//   * the keystream cipher - seed = bitrev16(reply) ^ 0xE12E, a 16-bit Fibonacci
//     LFSR (taps 0xA011) whose first 16 output bits ARE the seed; every on-air
//     frame (both directions) is XORed with the running keystream, which never
//     resets within a session.
//   * the session state machine - XHORSE/072F/7372 handshake, the 4R self-check
//     (first encrypted frame decrypts to sel-page-1 = 0x41BE), and the op,~op
//     page protocol with sel/bank switching.
//   * the per-emulated-type page recipes (configure).
//   * recovery of a chip stuck in a talk-first EM emulation (xh_open_session_em).
//
// Protocol, timings and how the EM recovery was found: doc/xhorse_notes.md.
//
// This would not have been possible without @CastleBravo @HenryGab and @eltrick
// on the Proxmark3 Discord and kivijakola for their hitager project on github
//-----------------------------------------------------------------------------

#include "xhorse.h"
#include "string.h"
#include "proxmark3_arm.h"
#include "cmd.h"             // reply_ng (pulls in pm3_cmd.h: CMD_*/PM3_* codes)
#include "BigBuf.h"
#include "fpga_loader.h"
#include "fpga_apis.h"       // FPGA_LF_EDGE_DETECT_SLOPE
#include "ticks_apis.h"
#include "dbprint.h"
#include "util.h"
#include "commonutil.h"
#include "hitag_common.h"   // pulls in hitag.h (hitag_mod_t, timing constants)

//-----------------------------------------------------------------------------
// Physical-layer parameters (HITAG2 defaults, confirmed on hardware)
//-----------------------------------------------------------------------------
#define XH_MOD        MC4K             // tag replies: HITAG2 Manchester, 4 kbit/s
#define XH_SOF_BITS   5                // tag reply SOF '11111' (stripped on receive)
#define XH_SEND_SOF   0               // reader sends NO SOF (standard HITAG2)
#define XH_TWAIT      HITAG_T_WAIT_SC  // carrier periods to wait before a reader frame

// Weak-reply reception.  The XT27A's load-modulation answer is weak; HITAG2's own
// code found level slicing "measured not to" resolve it and slope detection is "what
// made the receive work" (armsrc/hitag2.c).  So we drive the coil (READER_FIELD) but
// detect tag edges by SLOPE + SENSITIVE, at the slope threshold (20, not the level
// default 127).
#define XH_EDGE_MODE  (FPGA_LF_EDGE_DETECT_READER_FIELD | FPGA_LF_EDGE_DETECT_SLOPE | FPGA_LF_EDGE_DETECT_SENSITIVE)
#define XH_FPGA_THR   20              // slope-mode edge-detect threshold (HITAG2_SLOPE_THRESHOLD)

// Handshake inter-frame gaps.  XHORSE --~8 ms--> 072F --~11.78 ms--> 7372.  The chip
// needs these gaps to compute its replies; sending them back-to-back gets no answer.
#define XH_T_XHORSE_072F_US  8000     // the VVDI's gap measures 8.1 ms
#define XH_T_072F_7372_US    11780    // official tool's fixed 072F->7372 spacing

// Reliability: the tag's load-modulation reply is weak, so a handshake read can come
// back short/empty (rxlen 0 or 15).  The chip opens its XHORSE-listening window right
// after power-up, so we field-reset (coil off then on) before each attempt to give it
// a fresh power-up, and retry until we get a full 16-bit 7372 reply.
#define XH_TRIES_PER_TIMING  3        // session-open attempts per timing (xh_open_session)
#define XH_T_FIELD_OFF_US    20000    // coil off long enough to reset the chip (VVDI uses 20-60 ms)
#define XH_T_POWERUP_US      3000     // let the chip power up again before XHORSE
// A chip set to ID8A ignored every XHORSE we sent 3 ms, 30 ms or 198 ms after power-up, even
// after a 1.57 s cold-boot.  Before its first XHORSE to such a chip the VVDI keeps the field off
// 177.1 ms (22135/22136 T0), then sends XHORSE 41.8 ms (5226/5225 T0) after power-up
// (vvdi-id8A-reset-success.pm3 and -success-2.pm3).
#define XH_T_ID8A_OFF_US     177100
#define XH_T_ID8A_ON_US      41800
// In every round before that, and in the round just before that XHORSE, the VVDI first sends a
// TI-style command: power-up, 50.04 ms (6255 T0) charge, 100 field gaps ~23 T0 wide, then field off
// 31.74 ms (3968 T0) after the last gap.  Gap starts are 81 T0 apart for the first two, then 52 T0
// (S) or 67 T0 (L) as in xh_id8a_probe (identical in both captures).  Replaying it is what lets
// us reach an ID8A chip, but it also reverts the chip's type byte to 0x36 (info read 0x36 after it
// while bank 0 still held the ID8A image), so only reset/configure send it (xh_sess_t.allow_probe).
#define XH_T_ID8A_CHARGE_T0  6255
#define XH_T_ID8A_GAP_T0     22       // nominal; our gaps sniff ~1 T0 wider than set
#define XH_T_ID8A_FIRST_T0   81
#define XH_T_ID8A_TAIL_T0    3968
static const char xh_id8a_probe[] =
    "SLLSSSLLSLLLSSLSSSSLLSLLSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSLSLSSSLSLSSL";
#define XH_T_XMICRO_ID_US    1000     // gap after the XMICRO ACK before the 4-bit identify query
// XMICRO must not be sent straight after power-up: the VVDI waits 120.14 ms (15018 T0, identical in
// its XT27B and XT57B read captures, with nothing else on air in between).  The XT57B happens to
// answer an XMICRO sent 3 ms after power-up; the XT27B only answers after the 120 ms wait.
#define XH_T_XMICRO_POWERUP_US 120140

// EEPROM program cycle: after a WRITE's 32-bit data frame the chip needs ~1597 T0
// (~12.8 ms, measured in a VVDI reset capture) to commit the page before it can be read
// back or receive the next command.  Reading immediately returns the old/garbage
// page and fails verify.  13 ms gives a small margin.
#define XH_T_PROG_US         13000
// A verify read-back can miss on a weak reply; re-read a few times before failing.
// Each READ advances the keystream identically on chip and reader, so retrying is safe.
#define XH_VERIFY_READS      3

// Recovery for a chip stuck emulating a non-HITAG2 type (e.g. ID11 = EM4100, ID48 = Megamos).  Such a chip
// continuously broadcasts its fixed code and ignores the XHORSE config channel.
//
// What frees it is one reader burst sent a set time after power-up (xh_em_burst:
// 43 field-off pulses spaced 1 or 2 bit-times of the ID11 chip's RF/32 broadcast).  In a sniff of a successful reset the chip broadcasts
// EM right up to the first burst and is silent from power-up in every window after it, across
// field-offs of 70 ms, 570 ms and 1.57 s, i.e. the burst turns the emulation off and it stays off
// across power cycles.  The VVDI wraps the burst in a ~10 s sequence (listen window, XMICRO probes,
// "1111111" frames, 11 more bursts, long rests, a 1.57 s cold-boot).  For ID11/ID12 none of that
// changed anything observable; ID13 goes silent after the burst but only answers after the cold-boot.
// We keep the VVDI's structure (bursts, then one cold-boot, then the handshake) but send only one
// burst per timing.  See doc/xhorse_notes.md.  Values are the VVDI's, in us.
//
// The burst only works if it starts at the right point in the chip's EM frame, which is locked to
// power-up.  Measured from the field-on edge, freeing bursts for ID11 and ID12 start ~44 "units" into
// the frame, a unit being 32 T0 for ID11 (RF/32) and 40 T0 for ID12 (RF/40): ID11 1409-1423, ID12 1741
// T0.  For ID11 bursts at ~1427 T0 and later fail - the VVDI's own first try often does - so the
// timings aim at the window's centre, commanding 3.5 T0 early because our burst reaches the air that
// much after the commanded time.  ID13 (RF/64) is freed in its first frame after power-up (VVDI at
// 1398 T0, ours at ~1752) but not by the same position in a later frame.  ID48 (Megamos) needs a
// different burst (xh_burst_id48) at 1622 T0.  We don't know the emulated type, so the entries are
// tried in turn.
#define XH_T_EM_OFF_US            70050    // field off before and after the burst window (VVDI)
#define XH_T_EM_SETTLE_US         30180    // field on this long before XHORSE (VVDI, after the chip is freed)
#define XH_T_EM_COLDBOOT_US     1570940    // VVDI's power-down between the bursts and its handshake (ID13 needs it)
#define XH_EM_TRIES                   3    // attempts: one burst per xh_em_timings[] entry, cold-boot, handshake

// func field (top 2 bits of the op byte)
#define XH_FUNC_CTL   0
#define XH_FUNC_SEL   1
#define XH_FUNC_WRITE 2
#define XH_FUNC_READ  3

// bank-1 (config/identity) addresses
#define XH_PG_TYPE    0x3C            // emulated type byte
#define XH_PG_ID0     0x33            // 16-byte unique chip ID: pages 0x33..0x36
#define XH_PG_PAD     0x3F

//-----------------------------------------------------------------------------
// Keystream engine (incremental; frames are always 16/32 bits so pos stays a
// multiple of 16 and no frame straddles the seed boundary).
//-----------------------------------------------------------------------------
typedef struct {
    uint16_t seed;
    uint16_t win;    // current 16-bit LFSR window, MSB = oldest
    uint32_t pos;    // keystream bits consumed so far
} xh_ks_t;

static inline uint8_t xh_parity16(uint16_t x) {
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return x & 1;
}

static uint16_t xh_rev16(uint16_t x) {
    uint16_t r = 0;
    for (int i = 0; i < 16; i++) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

static void xh_ks_init(xh_ks_t *k, uint16_t reply) {
    k->seed = xh_rev16(reply) ^ 0xE12E;
    k->win = 0;
    k->pos = 0;
}

// Next 16 keystream bits, MSB-first.  The first call returns the seed itself.
static uint16_t xh_ks_next16(xh_ks_t *k) {
    uint16_t out;
    if (k->pos == 0) {
        out = k->seed;
        k->win = k->seed;
    } else {
        out = 0;
        for (int i = 0; i < 16; i++) {
            uint16_t nb = xh_parity16(k->win & 0xA011);
            out = (uint16_t)((out << 1) | nb);
            k->win = (uint16_t)((k->win << 1) | nb);
        }
    }
    k->pos += 16;
    return out;
}

static uint32_t xh_ks_next32(xh_ks_t *k) {
    uint32_t hi = xh_ks_next16(k);
    uint32_t lo = xh_ks_next16(k);
    return (hi << 16) | lo;
}

//-----------------------------------------------------------------------------
// Session
//-----------------------------------------------------------------------------
typedef struct {
    xh_ks_t ks;
    bool    led;
    int8_t  cur_bank;   // last sel'd bank, or -1
    bool    em_mode;    // freed from EM emulation this run: later sessions reopen with the EM off/settle timing
    bool    allow_probe; // may send the ID8A probe, which reverts the type byte (reset/configure only)
} xh_sess_t;

static uint32_t xh_bits_to_u32(const uint8_t *rx, int nbits) {
    uint32_t v = 0;
    int nbytes = nbits / 8;
    for (int i = 0; i < nbytes; i++) {
        v = (v << 8) | rx[i];
    }
    return v;
}

static uint16_t xh_op(uint8_t func, uint8_t page) {
    uint8_t op = (uint8_t)((func << 6) | (page & 0x3F));
    return (uint16_t)((op << 8) | ((uint8_t)~op));
}

// Busy-wait for `us` microseconds while the field stays energised.  SpinDelay()/
// WaitMS() don't work here because hitag_setup_fpga() stopped the system ticks, so
// we spin on the precision counter (TC0 @ MCK/32 = 1.5 ticks/us; T0 = 12 ticks =
// 8 us).  It's a 16-bit counter (wraps ~43 ms), so chunk long waits and pet the WDT.
static void xh_delay_us(uint32_t us) {
    while (us) {
        uint32_t chunk = (us > 20000) ? 20000 : us;   // 20 ms -> 30000 ticks, safely < 65536
        uint16_t ticks = (uint16_t)((chunk * 3) / 2);  // 1.5 ticks per us
        uint16_t ref = GetPrecisionCounterRaw();
        while (GetPrecisionCounterDelta(ref) < ticks) {};
        WDT_HIT();
        us -= chunk;
    }
}

// Send a cleartext reader frame (handshake literals only - no keystream).
static void xh_send_clear(const uint8_t *frame, size_t nbits, bool led) {
    hitag_reader_send_frame(frame, nbits, led, XH_SEND_SOF);
}

// Send a cleartext reader frame and receive the tag reply (no keystream).
// t_wait == 0 because the caller has already spaced this frame with xh_delay_us().
// Returns the number of reply bits demodulated.
static size_t xh_xfer_clear(xh_sess_t *s, const uint8_t *frame, size_t nbits,
                            uint8_t *rx, size_t rxsz) {
    size_t rxlen = 0;
    memset(rx, 0, rxsz);
    hitag_reader_transfer(frame, nbits, rx, rxsz, &rxlen, 0, s->led, XH_MOD, XH_SOF_BITS, XH_SEND_SOF);
    return rxlen;
}

// Send an encrypted 16-bit command and receive+decrypt a reply of reply_bits.
// Keystream is advanced for BOTH the command and the reply, deterministically:
// the real reader advances its cipher for every on-air frame in each direction,
// so we must too even if our demod of the reply is imperfect.
static int xh_cmd_reply(xh_sess_t *s, uint16_t pt_cmd, int reply_bits, uint32_t *reply_out) {
    uint16_t ct = pt_cmd ^ xh_ks_next16(&s->ks);
    uint8_t tx[2] = { (uint8_t)(ct >> 8), (uint8_t)(ct & 0xFF) };

    uint8_t rx[32];
    size_t rxlen = 0;
    memset(rx, 0, sizeof(rx));
    hitag_reader_transfer(tx, 16, rx, sizeof(rx), &rxlen, XH_TWAIT, s->led, XH_MOD, XH_SOF_BITS, XH_SEND_SOF);

    uint32_t ksr = (reply_bits == 32) ? xh_ks_next32(&s->ks) : xh_ks_next16(&s->ks);
    uint32_t pt = xh_bits_to_u32(rx, reply_bits) ^ ksr;
    if (reply_out) {
        *reply_out = pt;
    }
    return (rxlen >= (size_t)reply_bits) ? PM3_SUCCESS : PM3_EFAILED;
}

// sel a bank (0 = emulated-tag EEPROM, 1 = config/identity).  The tag echoes it.
static int xh_sel(xh_sess_t *s, uint8_t bank) {
    int res = xh_cmd_reply(s, xh_op(XH_FUNC_SEL, bank), 16, NULL);
    s->cur_bank = (int8_t)bank;
    return res;
}

static void xh_bank(xh_sess_t *s, uint8_t bank) {
    if (s->cur_bank != (int8_t)bank) {
        xh_sel(s, bank);
    }
}

// READ a page in the current bank; tag answers with 32-bit data.
static int xh_read(xh_sess_t *s, uint8_t page, uint32_t *val) {
    return xh_cmd_reply(s, xh_op(XH_FUNC_READ, page), 32, val);
}

// WRITE a page in the current bank: command (+echo), then the 32-bit data frame
// (reader -> tag, no reply), then an optional read-back verify.
static int xh_write(xh_sess_t *s, uint8_t page, uint32_t data, bool verify) {
    xh_cmd_reply(s, xh_op(XH_FUNC_WRITE, page), 16, NULL);

    uint32_t ct = data ^ xh_ks_next32(&s->ks);
    uint8_t tx[4] = { (uint8_t)(ct >> 24), (uint8_t)(ct >> 16), (uint8_t)(ct >> 8), (uint8_t)ct };
    hitag_reader_send_frame(tx, 32, s->led, XH_SEND_SOF);

    // Wait out the EEPROM program cycle before reading back or issuing the next
    // command (the chip is busy committing the page - see XH_T_PROG_US).
    xh_delay_us(XH_T_PROG_US);

    if (verify) {
        bool ok = false;
        for (int i = 0; i < XH_VERIFY_READS && !ok; i++) {
            uint32_t rb = 0;
            xh_read(s, page, &rb);
            ok = (rb == data);
        }
        if (!ok) {
            return PM3_EFAILED;
        }
    }
    return PM3_SUCCESS;
}

// ctl with a data frame, shaped like a WRITE: command (+echo, which is ~op,op rather than a copy),
// then 32-bit data, then the program wait.  The VVDI ends an ID8A set with ctl 0x30 carrying the
// value it has just read from bank-1 page 0x3D.
static void xh_ctl_data(xh_sess_t *s, uint8_t page, uint32_t data) {
    xh_cmd_reply(s, xh_op(XH_FUNC_CTL, page), 16, NULL);

    uint32_t ct = data ^ xh_ks_next32(&s->ks);
    uint8_t tx[4] = { (uint8_t)(ct >> 24), (uint8_t)(ct >> 16), (uint8_t)(ct >> 8), (uint8_t)ct };
    hitag_reader_send_frame(tx, 32, s->led, XH_SEND_SOF);
    xh_delay_us(XH_T_PROG_US);
}

// ctl frame (no tag reply): advances the keystream by the 16-bit command only.
// In every capture a ctl is followed by a fresh sel, i.e. ctl clears the chip's
// bank selection - so force the next bank access to re-sel.
static void xh_ctl(xh_sess_t *s, uint8_t page) {
    uint16_t ct = xh_op(XH_FUNC_CTL, page) ^ xh_ks_next16(&s->ks);
    uint8_t tx[2] = { (uint8_t)(ct >> 8), (uint8_t)(ct & 0xFF) };
    xh_send_clear(tx, 16, s->led);   // send-only; same wire shape, no receive
    s->cur_bank = -1;
}

//-----------------------------------------------------------------------------
// Handshake: XHORSE -> 072F -> 7372 -> 16-bit tag reply, which seeds the cipher.
// Returns PM3_SUCCESS and a seeded session, or an error if no reply was demodulated.
//-----------------------------------------------------------------------------
static int xh_handshake(xh_sess_t *s) {
    static const uint8_t magic[6] = { 'X', 'H', 'O', 'R', 'S', 'E' };
    static const uint8_t q072f[2] = { 0x07, 0x2F };
    static const uint8_t q7372[2] = { 0x73, 0x72 };

    uint8_t rx[32];
    size_t rxlen;

    // Cleartext, as the VVDI sends it:
    //   XHORSE (48 bits)  - send only (the chip acknowledges; we don't need to read it)
    //   8 ms
    //   072F (16 bits)    - send only
    //   11.78 ms
    //   7372 (16 bits)    - send and receive the chip's 16-bit reply
    // The chip needs the gaps to compute its replies.  The 7372 reply is different every
    // session (e.g. 0x54CF) and seeds the cipher.
    xh_send_clear(magic, 48, s->led);
    xh_delay_us(XH_T_XHORSE_072F_US);
    xh_send_clear(q072f, 16, s->led);
    xh_delay_us(XH_T_072F_7372_US);

    rxlen = xh_xfer_clear(s, q7372, 16, rx, sizeof(rx));                  // -> per-session seed reply
    if (rxlen < 16) {
        if (g_dbglevel >= DBG_ERROR) {
            Dbprintf("xhorse: no 7372 reply (rxlen=%u)", (unsigned)rxlen);
        }
        return PM3_EFAILED;
    }

    uint16_t reply = (uint16_t)((rx[0] << 8) | rx[1]);
    if (g_dbglevel >= DBG_EXTENDED) {
        Dbprintf("xhorse: 7372 reply=%04x  seed=%04x", reply, xh_rev16(reply) ^ 0xE12E);
    }
    xh_ks_init(&s->ks, reply);
    s->cur_bank = -1;
    return PM3_SUCCESS;
}

static void xh_setup(xh_sess_t *s, bool ledcontrol) {
    memset(s, 0, sizeof(*s));
    s->led = ledcontrol;
    s->cur_bank = -1;
    // READER_FIELD energises the coil; SLOPE|SENSITIVE detect the weak tag reply the
    // way the HITAG2 reader/sim do (see XH_EDGE_MODE).  The bits are independent: the
    // field is driven by READER_FIELD regardless of the detector algorithm.
    hitag_setup_fpga(XH_EDGE_MODE, XH_FPGA_THR, ledcontrol);
}

// Power-cycle the chip: hold the coil off (DOUT high = modulation), then on again.
// Ticks are stopped after setup so we spin on the precision counter via xh_delay_us.
static void xh_field_reset(void) {
    Gpio_SSC_DOUT_High();            // modulation on -> field OFF
    xh_delay_us(XH_T_FIELD_OFF_US);
    Gpio_SSC_DOUT_Low();             // field ON again
    xh_delay_us(XH_T_POWERUP_US);
}

// The 4R opener: the first encrypted command of a session is sel bank 1 (0x41BE), and the chip
// echoes it.  Only a real XT27A with the right keystream returns an echo that decrypts to exactly
// 0x41BE, so this is the check that a session is genuinely open.  It matters for chips stuck in
// an EM emulation: their continuous broadcast can be demodulated as a "7372 reply", giving a
// handshake that appears to succeed with a garbage seed.  Returns true if the echo matched.
static bool xh_4r(xh_sess_t *s, const char *what, int attempt) {
    const uint16_t sel1 = xh_op(XH_FUNC_SEL, 1);
    uint32_t echo = 0;
    int res = xh_cmd_reply(s, sel1, 16, &echo);
    s->cur_bank = 1;
    bool ok = (res == PM3_SUCCESS) && ((uint16_t)echo == sel1);
    if (g_dbglevel >= (ok ? DBG_EXTENDED : DBG_ERROR)) {
        Dbprintf("xhorse: %s attempt %d  4R echo=%04x %s", what, attempt, (uint16_t)echo,
                 ok ? "(sel ok)" : "(bad - not a real session, retrying)");
    }
    return ok;
}

// Open one channel-A session on an already-set-up front end: power-cycle, handshake, 4R.
// The chip's reply is weak and position-sensitive, so this retries, re-powering the chip
// each time.  A `ctl` ends a session (the chip stops responding), so a caller that sends
// more commands after a `ctl` must open a new session here first, as the VVDI does between
// its capability read and its writes.
static int xh_open_session(xh_sess_t *s) {
    const int timings = s->allow_probe ? 4 : 3;
    for (int attempt = 0; attempt < timings * XH_TRIES_PER_TIMING; attempt++) {
        int mode = s->em_mode ? 1 : (attempt % timings);
        if (g_dbglevel >= DBG_EXTENDED) {
            Dbprintf("xhorse: session attempt %d, timing %d", attempt, mode);
        }
        if (mode == 3) {
            // The VVDI's whole last round before its first XHORSE to a chip set to ID8A: the TI-style
            // probe, then the long off and XHORSE 41.8 ms after power-up (see XH_T_ID8A_CHARGE_T0).
            xh_field_reset();
            xh_delay_us(XH_T_ID8A_CHARGE_T0 * 8 - XH_T_POWERUP_US);
            const size_t n = sizeof(xh_id8a_probe) - 1;     // 98 S/L intervals -> 100 gaps
            for (size_t i = 0; i < n + 2; i++) {
                uint32_t next;                               // gap start to next gap start (T0)
                if (i == 0) {
                    next = XH_T_ID8A_FIRST_T0;
                } else if (i <= n) {
                    next = (xh_id8a_probe[i - 1] == 'L') ? 67 : 52;
                } else {
                    next = XH_T_ID8A_TAIL_T0;                // last gap -> field off
                }
                Gpio_SSC_DOUT_High();
                xh_delay_us(XH_T_ID8A_GAP_T0 * 8);
                Gpio_SSC_DOUT_Low();
                xh_delay_us((next - XH_T_ID8A_GAP_T0) * 8);
            }
            Gpio_SSC_DOUT_High();
            xh_delay_us(XH_T_ID8A_OFF_US);
            Gpio_SSC_DOUT_Low();
            xh_delay_us(XH_T_ID8A_ON_US);
        } else if (mode == 1) {
            // The VVDI's timing: off 70 ms, on, XHORSE 30.18 ms later.  Always used once the chip has
            // been freed from EM emulation this run, and on every third attempt otherwise: an ID13
            // chip left half-recovered by an earlier run answers a 3 ms power-up only rarely
            // (rxlen=10) but does answer this one.
            Gpio_SSC_DOUT_High();
            xh_delay_us(XH_T_EM_OFF_US);
            Gpio_SSC_DOUT_Low();
            xh_delay_us(XH_T_EM_SETTLE_US);
        } else if (mode == 2) {
            // The VVDI's timing for a chip set to ID8A (see XH_T_ID8A_OFF_US).
            Gpio_SSC_DOUT_High();
            xh_delay_us(XH_T_ID8A_OFF_US);
            Gpio_SSC_DOUT_Low();
            xh_delay_us(XH_T_ID8A_ON_US);
        } else {
            xh_field_reset();
        }

        if (xh_handshake(s) != PM3_SUCCESS) {
            continue;                // no/partial 7372 reply - re-power and retry
        }
        if (xh_4r(s, "session", attempt) == false) {
            continue;                // handshake was not a real session - re-power and retry
        }
        if (g_dbglevel >= DBG_EXTENDED) {
            Dbprintf("xhorse: session open on attempt %d (timing %d)", attempt, mode);
        }
        return PM3_SUCCESS;
    }
    return PM3_EFAILED;
}

// The VVDI's EM unlock burst, sent at a set time after power-up (xh_em_timings).  43 field-off pulses; start
// offsets are T0 from the start of the first pulse, averaged over 22 occurrences in two VVDI
// reset captures (they agree to within 0.75 T0); widths are how long the field is held off, i.e.
// the measured width minus the 2 T0 the sniffer adds (our 8 T0 BPLM gap measures 10, as does the
// VVDI's).  What it encodes is unknown, so it is replayed verbatim.  The VVDI sends the same burst
// to an ID12 chip (RF/40 broadcast) and it frees that too.  See doc/xhorse_notes.md.
static const uint16_t xh_emb_start[43] = {
    0, 36, 68, 129, 163, 224, 258, 290, 351, 415, 449, 511, 576, 610, 641, 702, 767, 832, 865, 927,
    961, 992, 1024, 1054, 1115, 1180, 1214, 1245, 1306, 1340, 1372, 1433, 1497, 1562, 1596, 1658,
    1692, 1753, 1787, 1818, 1880, 1944, 1978
};
static const uint8_t xh_emb_width[43] = {
    20, 16, 15, 19, 16, 19, 15, 15, 19, 19, 15, 19, 19, 16, 15, 19, 19, 19, 16, 19, 16, 15, 15, 15,
    19, 19, 16, 15, 19, 16, 15, 19, 19, 19, 16, 19, 16, 19, 16, 15, 19, 19, 16
};

// The VVDI's ID48 (Megamos) unlock burst: a different 47-pulse sequence on the same ~32 T0 grid,
// sent 1622 T0 after power-up.  Measured the same way as the EM burst, averaged over the three
// occurrences in one VVDI capture (they agree to within 2 T0).  The VVDI first sends twice a 48-pulse
// variant with an extra pulse at +69 T0, each followed by XMICRO probes (channel B), which did not
// free an XT27A; this 47-pulse one, followed by XHORSE, did.  See doc/xhorse_notes.md.
static const uint16_t xh_id48b_start[47] = {
    0, 35, 131, 167, 200, 262, 298, 332, 394, 459, 525, 590, 626, 689, 725, 787, 823, 857, 890, 923,
    985, 1050, 1116, 1152, 1247, 1313, 1348, 1382, 1444, 1510, 1575, 1673, 1709, 1742, 1776, 1838,
    1903, 1969, 2004, 2038, 2071, 2165, 2202, 2264, 2300, 2333, 2428
};
static const uint8_t xh_id48b_width[47] = {
    19, 17, 19, 16, 16, 20, 17, 16, 19, 19, 20, 20, 17, 20, 16, 20, 17, 16, 16, 16, 19, 20, 19, 16,
    19, 19, 16, 16, 19, 20, 19, 19, 16, 16, 15, 20, 19, 20, 17, 16, 16, 20, 16, 19, 16, 16, 19
};

typedef struct {
    const uint16_t *start;      // T0 from the start of the first pulse
    const uint8_t  *width;      // T0 the field is held off
    size_t          n;
} xh_burst_t;

static const xh_burst_t xh_burst_em   = { xh_emb_start,   xh_emb_width,   sizeof(xh_emb_start) / sizeof(xh_emb_start[0]) };
static const xh_burst_t xh_burst_id48 = { xh_id48b_start, xh_id48b_width, sizeof(xh_id48b_start) / sizeof(xh_id48b_start[0]) };

// burst length in us: start of the last pulse + its width (EM 1994 T0 = 15.95 ms, ID48 2447 T0 = 19.6 ms)
static uint32_t xh_burst_len_us(const xh_burst_t *b) {
    return ((uint32_t)b->start[b->n - 1] + b->width[b->n - 1]) * 8;
}

// Emit a burst with the field on.  One precision-counter reference covers it (2447 T0 = 29364
// ticks < 65536), so pulse positions don't accumulate per-pulse error.
static void xh_em_burst(const xh_burst_t *b) {
    const uint16_t ref = GetPrecisionCounterRaw();
    for (size_t i = 0; i < b->n; i++) {
        while (GetPrecisionCounterDelta(ref) < (uint16_t)(T0 * b->start[i])) {};
        Gpio_SSC_DOUT_High();                                        // field off
        while (GetPrecisionCounterDelta(ref) < (uint16_t)(T0 * (b->start[i] + b->width[i]))) {};
        Gpio_SSC_DOUT_Low();                                         // field on
    }
}

// Burst windows (see XH_T_EM_OFF_US): which burst, in a field-on window of on_us, commanded
// burst_at_us after power-up.
typedef struct {
    const char       *name;
    const xh_burst_t *burst;
    uint32_t          on_us;
    uint32_t          burst_at_us;
} xh_em_timing_t;

static const xh_em_timing_t xh_em_timings[] = {
    // 13692.5 T0 commanded -> ~13696 on air = 6 ID11 frames + 1408 T0 (unit 44.0).  Frees ID11.  It
    // does NOT free a fresh ID13 even though it is the same position 3 ID13 frames later: ID13 seems
    // to listen only in its first frame after power-up (the VVDI frees it at 11.18 ms).
    { "ID11 (32 T0 units)",      &xh_burst_em,   155940, 109540 },
    // 1748.75 T0 commanded -> ~1752 on air = unit 43.8 of ID12's 40 T0 units.  Frees ID12 (13.93 ms,
    // the VVDI's value, freed it at 1741-1745), and also frees ID13 in its first frame - ID13 then
    // only answers after the cold-boot.
    { "ID12/ID13 (first frame)", &xh_burst_em,   60300,  13990 },
    // ID8E broadcasts too (MC64, from ~950 T0 after power-up) and is freed by the same burst B as
    // ID48, but earlier: the VVDI's freeing round puts it at 1535 T0 on air (1531.5 commanded),
    // keeps the field on 52.88 ms in total, then powers down 420 ms (vvdi-id8E-reset-success.pm3).
    // Its two earlier rounds send burst A at 41.7 ms and don't free it.  Our ID48 timing (~1622 on
    // air) did not free it.
    { "ID8E",                    &xh_burst_id48, 52880,  12252 },
    // 1618.5 T0 commanded -> ~1622 on air, the VVDI's position in all three of its tries; it keeps
    // the field on 63.86 ms in total.  Its own power-down before the handshake is 420 ms; the
    // cold-boot here is longer.
    { "ID48 (Megamos)",          &xh_burst_id48, 63860,  12948 },
};

// Pull a chip stuck broadcasting a non-HITAG2 emulation (ID11/ID12/ID13/ID48/ID8E) back onto the config
// channel, structured as the VVDI does it: bursts first, no handshake in between, then one cold-boot
// and one handshake.  Each attempt sends one burst per entry of xh_em_timings[] (each in its own
// power cycle: off 70 ms, on, burst at burst_at_us, rest of the window), so whichever type the chip
// emulates gets its timing; a burst at the wrong timing is harmless once the chip is freed (the
// VVDI keeps bursting after the chip goes silent).  Then field off for the VVDI's 1.57 s cold-boot,
// on, XHORSE handshake 30.18 ms later.  ID13 only answers after the cold-boot; ID11/ID12 answer
// either way.  ~2.3 s per attempt.
static int xh_open_session_em(xh_sess_t *s) {
    for (int attempt = 0; attempt < XH_EM_TRIES; attempt++) {
        for (size_t t = 0; t < sizeof(xh_em_timings) / sizeof(xh_em_timings[0]); t++) {
            const xh_em_timing_t *tm = &xh_em_timings[t];

            Gpio_SSC_DOUT_High();                                    // off: the burst is timed from a fresh power-up
            xh_delay_us(XH_T_EM_OFF_US);
            Gpio_SSC_DOUT_Low();                                     // field on: chip boots, broadcasts EM
            xh_delay_us(tm->burst_at_us);
            xh_em_burst(tm->burst);
            xh_delay_us(tm->on_us - tm->burst_at_us - xh_burst_len_us(tm->burst));
        }

        Gpio_SSC_DOUT_High();                                        // cold-boot
        xh_delay_us(XH_T_EM_COLDBOOT_US);
        Gpio_SSC_DOUT_Low();
        xh_delay_us(XH_T_EM_SETTLE_US);

        if (xh_handshake(s) == PM3_SUCCESS && xh_4r(s, "EM-recovery", attempt)) {
            s->em_mode = true;                                       // later sessions: 70 ms off / 30.18 ms settle
            return PM3_SUCCESS;
        }
        if (g_dbglevel >= DBG_ERROR) {
            Dbprintf("xhorse: EM-recovery attempt %d: no session after the bursts and cold-boot", attempt);
        }
    }
    return PM3_EFAILED;
}

// Open a session, trying the normal XHORSE handshake first and falling back to the EM burst
// for a chip stuck emulating a non-HITAG2 type (ID11/EM4100 etc).
static int xh_open_any(xh_sess_t *s) {
    if (xh_open_session(s) == PM3_SUCCESS) {
        return PM3_SUCCESS;
    }
    if (g_dbglevel >= DBG_ERROR) {
        Dbprintf("xhorse: no XHORSE session; sending EM-recovery burst (chip stuck in non-HITAG2 emulation?)");
    }
    return xh_open_session_em(s);
}

static int xh_begin(xh_sess_t *s, bool ledcontrol) {
    xh_setup(s, ledcontrol);
    return xh_open_session(s);
}

// Like xh_begin, but also handles a chip stuck in a non-HITAG2 emulation (reset/configure).
static int xh_begin_any(xh_sess_t *s, bool ledcontrol) {
    xh_setup(s, ledcontrol);
    s->allow_probe = true;      // the chip is about to be rewritten anyway
    return xh_open_any(s);
}

// Read the 16-byte unique chip ID (bank-1 pages 0x33..0x36) into id16.
static void xh_read_chipid(xh_sess_t *s, uint8_t id16[16]) {
    xh_bank(s, 1);
    for (int i = 0; i < 4; i++) {
        uint32_t w = 0;
        xh_read(s, (uint8_t)(XH_PG_ID0 + i), &w);
        id16[i * 4 + 0] = (uint8_t)(w >> 24);
        id16[i * 4 + 1] = (uint8_t)(w >> 16);
        id16[i * 4 + 2] = (uint8_t)(w >> 8);
        id16[i * 4 + 3] = (uint8_t)(w);
    }
}

//-----------------------------------------------------------------------------
// Encoders for the per-type page images
//-----------------------------------------------------------------------------
static uint8_t xh_bitrev8(uint8_t b) {
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

// CRC-16/KERMIT over data[] (poly 0x1021, init 0, refin/refout, xorout 0).
static uint16_t xh_crc16_kermit(const uint8_t *data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(xh_bitrev8(data[i]) << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    uint16_t r = 0;   // refout: bit-reverse the 16-bit result
    for (int i = 0; i < 16; i++) {
        r = (uint16_t)((r << 1) | (crc & 1));
        crc >>= 1;
    }
    return r;
}

static uint32_t xh_be32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

// ID11/ID12 bitrev packing -> bank-0 pages 0x00..0x04 (header passed in).
static void xh_pack_em8(const uint8_t *uid, uint16_t header, uint16_t pages[5]) {
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
static void xh_id13_stream(const uint8_t *uid5, uint8_t out8[8]) {
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

//-----------------------------------------------------------------------------
// Per-type page recipes.  Each returns PM3_SUCCESS or the first verify failure.
//-----------------------------------------------------------------------------
#define XH_WR(pg, val) do { if (xh_write(s, (pg), (val), verify) != PM3_SUCCESS) ok = PM3_EFAILED; } while (0)

static int xh_recipe(xh_sess_t *s, const lf_xhorse_data_t *p) {
    bool verify = p->verify;
    int ok = PM3_SUCCESS;
    uint32_t uid32 = (p->uidlen >= 4) ? xh_be32(p->uid) : 0;

    // Type byte first (bank 1), as every decoded type except ID8A does (its recipe writes it last).
    if (p->has_type) {
        xh_bank(s, 1);
        XH_WR(XH_PG_TYPE, p->typeword ? 0x0080008CUL : (uint32_t)p->typebyte);
    }

    switch ((xhorse_recipe_t)p->recipe) {

        case XHORSE_R_HITAG2:       // ID46/7936/7937/7946
            xh_bank(s, 0);
            XH_WR(0x00, uid32);
            XH_WR(0x01, 0x4D494B52);   // "MIKR"
            XH_WR(0x02, 0x00004F4E);   // "..ON"
            XH_WR(0x03, 0x06AA4854);   // config
            break;

        case XHORSE_R_7935:         // PCF7935: full bank-0 wipe, 0x1F = 0x07
            xh_bank(s, 0);
            for (uint8_t pg = 0x00; pg <= 0x3F; pg++) {
                XH_WR(pg, (pg == 0x1F) ? 0x00000007UL : 0x00000000UL);
            }
            break;

        case XHORSE_R_7938:         // ID47: distinct 5-page image + clear bank-1 0x38..0x3B
            xh_bank(s, 0);
            XH_WR(0x00, uid32);
            XH_WR(0x01, 0x11112222);
            XH_WR(0x02, 0x33334444);
            XH_WR(0x03, 0x55556666);
            XH_WR(0x04, 0x00FFFFFF);
            xh_bank(s, 1);
            for (uint8_t pg = 0x38; pg <= 0x3B; pg++) {
                XH_WR(pg, 0x00000000UL);
            }
            break;

        case XHORSE_R_7939:         // ID49: config-bank-only
            xh_bank(s, 1);
            XH_WR(0x00, 0xF7F8251D);
            for (uint8_t pg = 0x01; pg <= 0x07; pg++) {
                XH_WR(pg, 0x00000000UL);
            }
            for (uint8_t pg = 0x38; pg <= 0x3B; pg++) {
                XH_WR(pg, 0x00000000UL);
            }
            break;

        case XHORSE_R_ID48: {       // Megamos: 16-word bank-0 image
            // The UID is held 16 bits per page, low half first: UID 78B8D707 is
            // 0x02 = 0x0000D707, 0x03 = 0x000078B8.
            const uint32_t img[4] = { 0x00005678, 0x0000A1BF, uid32 & 0xFFFF, uid32 >> 16 };
            xh_bank(s, 0);
            for (uint8_t pg = 0x00; pg <= 0x03; pg++) {
                XH_WR(pg, img[pg]);
            }
            for (uint8_t pg = 0x04; pg <= 0x0F; pg++) {
                XH_WR(pg, 0x0000AAAA);
            }
            break;
        }

        case XHORSE_R_ID4C: {       // 8-byte UID + CRC16-KERMIT
            uint8_t rev[8];
            for (int i = 0; i < 8; i++) {
                rev[i] = p->uid[7 - i];
            }
            uint16_t crc = xh_crc16_kermit(rev, 8);
            // 12-byte stream [crc_hi crc_lo uid(8) 00 00], reversed -> T[12]
            uint8_t st[12] = { (uint8_t)(crc >> 8), (uint8_t)crc,
                               p->uid[0], p->uid[1], p->uid[2], p->uid[3],
                               p->uid[4], p->uid[5], p->uid[6], p->uid[7], 0, 0
                             };
            uint8_t t[12];
            for (int i = 0; i < 12; i++) {
                t[i] = st[11 - i];
            }
            xh_bank(s, 0);
            XH_WR(0x26, xh_be32(&t[0]));
            XH_WR(0x25, xh_be32(&t[4]));
            XH_WR(0x24, xh_be32(&t[8]));
            break;
        }

        case XHORSE_R_ID11:         // header 0x0010
        case XHORSE_R_ID12: {       // header 0x0014
            uint16_t pages[5];
            uint16_t hdr = (p->recipe == XHORSE_R_ID11) ? 0x0010 : 0x0014;
            xh_pack_em8(p->uid, hdr, pages);
            xh_bank(s, 0);
            XH_WR(0x02, pages[0x02]);
            XH_WR(0x01, pages[0x01]);
            XH_WR(0x04, pages[0x04]);
            XH_WR(0x03, pages[0x03]);
            XH_WR(0x00, pages[0x00]);
            break;
        }

        case XHORSE_R_ID13: {       // EM4102 + bitrev; type word already written
            uint8_t s8[8];
            xh_id13_stream(p->uid, s8);
            xh_bank(s, 0);
            XH_WR(0x02, (uint32_t)((s8[0] << 8) | s8[1]));
            XH_WR(0x01, (uint32_t)((s8[2] << 8) | s8[3]));
            XH_WR(0x04, (uint32_t)((s8[4] << 8) | s8[5]));
            XH_WR(0x03, (uint32_t)((s8[6] << 8) | s8[7]));
            XH_WR(0x00, 0x00000018);
            break;
        }

        case XHORSE_R_TEMPLATE:     // ID4D / 4D80 / ID4E - config-template family
            xh_bank(s, 0);
            for (uint8_t pg = 0x00; pg <= 0x1E; pg++) {   // 0x1F deliberately skipped
                XH_WR(pg, 0x00000000UL);
            }
            XH_WR(0x20, 0x00000003);
            XH_WR(0x21, 0x00000000);
            XH_WR(0x22, (uint32_t)p->aux);                // config word
            XH_WR(0x23, 0x0000036C);
            for (uint8_t pg = 0x24; pg <= 0x28; pg++) {
                XH_WR(pg, 0x00000000UL);
            }
            xh_bank(s, 1);
            XH_WR(0x13, 0x00004600);
            break;

        case XHORSE_R_ID8A: {       // full image, type byte last; UID at 0x04/0x05
            // 0x04 = uid[1]<<8 | uid[0] ; 0x05 = uid[3]<<8 | uid[2]  (byte-swapped halves)
            uint32_t p04 = ((uint32_t)p->uid[1] << 8) | p->uid[0];
            uint32_t p05 = ((uint32_t)p->uid[3] << 8) | p->uid[2];
            xh_bank(s, 0);
            XH_WR(0x00, 0x8A0AD459);
            XH_WR(0x01, 0x75020AE0);
            XH_WR(0x02, 0xA03448A3);
            XH_WR(0x03, 0xA77F0000);
            XH_WR(0x04, p04);
            XH_WR(0x05, p05);
            XH_WR(0x06, 0x00000000);
            XH_WR(0x07, 0x7771654D);   // "wqeM"
            XH_WR(0x08, 0xFFFFFFFF);
            XH_WR(0x09, 0x1F010E06);
            XH_WR(0x0A, 0x393448A3);
            for (uint8_t pg = 0x0B; pg <= 0x0F; pg++) {
                XH_WR(pg, 0x00000000UL);
            }
            XH_WR(0x10, 0x0005E770);
            for (uint8_t pg = 0x20; pg <= 0x3F; pg++) {
                XH_WR(pg, 0x00000000UL);
            }
            xh_bank(s, 1);
            for (uint8_t pg = 0x00; pg <= 0x1C; pg++) {
                XH_WR(pg, 0x00000000UL);
            }
            XH_WR(XH_PG_TYPE, 0x0000008A);
            // VVDI's closing step (vvdi-id8A-set-success.pm3): re-sel bank 1, read 0x3D
            // (0x0002021F), send it back with ctl 0x30, read 0x3D again.  Meaning unknown.
            {
                uint32_t v3d = 0;
                xh_sel(s, 1);
                xh_read(s, 0x3D, &v3d);
                xh_ctl_data(s, 0x30, v3d);
                xh_read(s, 0x3D, &v3d);
            }
            break;
        }

        case XHORSE_R_ID8E:         // type byte 0x8E + UID store at 0x0A/0x0B
            // NOTE: the secondary emulation block at bank-0 0x00..0x04 is UID-high-half
            // dependent and not yet reversed.  We write only the
            // authoritative UID store; a reader may need that block for full emulation.
            xh_bank(s, 0);
            XH_WR(0x0A, (uint32_t)((p->uid[2] << 8) | p->uid[3]));   // low half
            XH_WR(0x0B, (uint32_t)((p->uid[0] << 8) | p->uid[1]));   // high half
            break;

        default:
            ok = PM3_EINVARG;
            break;
    }
    return ok;
}

//-----------------------------------------------------------------------------
// Command handlers (called from appmain.c)
//-----------------------------------------------------------------------------
// Probe the XMICRO / "channel B" identify (XT57 / XT37-class super-chips).  The XT27A never
// answers XMICRO, so any reply here means a newer family we do not yet support.  Sequence, as
// the VVDI reads an XT27B / XT57B:
//   field on, wait 120.14 ms
//   reader -> XMICRO (48b)          tag -> 0x5AA5 ACK
//   reader -> 0b0011 (4b identify)  tag -> ASCII family string, e.g. "XT37"
// On success fills type[] with the ASCII the chip reported and returns PM3_SUCCESS.
static int xh_xmicro_identify(xh_sess_t *s, uint8_t *type, size_t typesz, size_t *tlen) {
    static const uint8_t xmicro[6] = { 'X', 'M', 'I', 'C', 'R', 'O' };
    static const uint8_t query[1]  = { 0x30 };   // 4 bits, 0b0011 (MSB-aligned)
    uint8_t rx[32];
    size_t rxlen;

    *tlen = 0;

    // Fresh power-up, then give the chip the VVDI's 120 ms before probing (XH_T_XMICRO_POWERUP_US).
    Gpio_SSC_DOUT_High();
    xh_delay_us(XH_T_FIELD_OFF_US);
    Gpio_SSC_DOUT_Low();
    xh_delay_us(XH_T_XMICRO_POWERUP_US);

    // 1) XMICRO probe -> expect the 0x5AA5 ACK (XT27A gives nothing here).
    rxlen = xh_xfer_clear(s, xmicro, 48, rx, sizeof(rx));
    if (rxlen < 16) {
        return PM3_EFAILED;                      // no B-channel response -> not an XT57/XT37-class chip
    }

    // 2) 4-bit identify query -> the chip answers with its ASCII family string.
    xh_delay_us(XH_T_XMICRO_ID_US);
    rxlen = xh_xfer_clear(s, query, 4, rx, sizeof(rx));
    if (rxlen >= 8) {
        size_t n = rxlen / 8;
        if (n > typesz) n = typesz;
        memcpy(type, rx, n);
        *tlen = n;
    }
    return PM3_SUCCESS;                          // answered XMICRO: it is a B-channel chip
}

void Xhorse_Info(bool ledcontrol) {
    xh_sess_t s;
    lf_xhorse_info_t out;
    memset(&out, 0, sizeof(out));

    xh_setup(&s, ledcontrol);

    // Try the XMICRO / channel-B identify first.  A reply means a newer super-chip family
    // (XT57 / XT37-class) that this command does not yet support - report it and stop.
    size_t tlen = 0;
    if (xh_xmicro_identify(&s, out.bchip_type, sizeof(out.bchip_type), &tlen) == PM3_SUCCESS) {
        out.bchip = 1;
        out.bchip_type_len = (uint8_t)tlen;
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_XHORSE_INFO, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
        return;
    }

    // Otherwise fall back to the XT27A channel-A handshake + reads.
    if (xh_open_session(&s) != PM3_SUCCESS) {
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_XHORSE_INFO, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
        return;
    }
    out.found = 1;

    xh_read_chipid(&s, out.chipid);

    uint32_t tb = 0;
    xh_bank(&s, 1);
    xh_read(&s, XH_PG_TYPE, &tb);
    out.typebyte = (uint8_t)(tb & 0xFF);

    xh_bank(&s, 0);
    out.image_pages = 8;
    for (uint8_t pg = 0; pg < out.image_pages; pg++) {
        uint32_t w = 0;
        xh_read(&s, pg, &w);
        out.image[pg * 4 + 0] = (uint8_t)(w >> 24);
        out.image[pg * 4 + 1] = (uint8_t)(w >> 16);
        out.image[pg * 4 + 2] = (uint8_t)(w >> 8);
        out.image[pg * 4 + 3] = (uint8_t)(w);
    }
    xh_ctl(&s, XH_PG_PAD);

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_XHORSE_INFO, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
}

void Xhorse_Dump(bool ledcontrol) {
    xh_sess_t s;
    lf_xhorse_info_t out;
    memset(&out, 0, sizeof(out));

    if (xh_begin(&s, ledcontrol) != PM3_SUCCESS) {
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_XHORSE_DUMP, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
        return;
    }
    out.found = 1;

    xh_read_chipid(&s, out.chipid);

    uint32_t tb = 0;
    xh_bank(&s, 1);
    xh_read(&s, XH_PG_TYPE, &tb);
    out.typebyte = (uint8_t)(tb & 0xFF);

    xh_bank(&s, 0);
    out.image_pages = 64;
    for (uint16_t pg = 0; pg < out.image_pages; pg++) {
        uint32_t w = 0;
        xh_read(&s, (uint8_t)pg, &w);
        out.image[pg * 4 + 0] = (uint8_t)(w >> 24);
        out.image[pg * 4 + 1] = (uint8_t)(w >> 16);
        out.image[pg * 4 + 2] = (uint8_t)(w >> 8);
        out.image[pg * 4 + 3] = (uint8_t)(w);
    }
    xh_ctl(&s, XH_PG_PAD);

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_XHORSE_DUMP, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
}

// Start the write phase.  A `ctl` ended the capability-read session, so the chip no
// longer responds on it; the VVDI re-powers and re-handshakes before writing.  Open a
// fresh session, READ 0x3F (status) as the VVDI does, and clear the cached bank so the
// recipe's first write issues an explicit sel.
//
// This deliberately uses xh_open_session, not xh_open_any: the first session already
// opened, so the chip is not stuck in an EM emulation and a failure here is a coupling
// problem the EM burst cannot fix.  If the chip was recovered, em_mode makes
// xh_open_session reopen with the EM timing.
static int xh_begin_writes(xh_sess_t *s) {
    if (xh_open_session(s) != PM3_SUCCESS) {
        return PM3_EFAILED;
    }
    uint32_t st = 0;
    xh_read(s, XH_PG_PAD, &st);
    s->cur_bank = -1;
    return PM3_SUCCESS;
}

// Shared by configure and reset: capability-read session (chip ID), then a write session
// that applies the recipe.  Recovers a chip stuck in an EM emulation if needed.
static void xh_configure_run(const lf_xhorse_data_t *p, uint16_t cmd, bool ledcontrol) {
    xh_sess_t s;
    if (xh_begin_any(&s, ledcontrol) != PM3_SUCCESS) {
        hitag_cleanup(ledcontrol);
        reply_ng(cmd, PM3_EFAILED, NULL, 0);
        return;
    }

    uint8_t id16[16];
    xh_read_chipid(&s, id16);   // capability read, as the VVDI does before every set
    xh_ctl(&s, XH_PG_PAD);      // ends the capability-read session

    if (xh_begin_writes(&s) != PM3_SUCCESS) {
        hitag_cleanup(ledcontrol);
        reply_ng(cmd, PM3_EFAILED, NULL, 0);
        return;
    }

    int res = xh_recipe(&s, p);
    xh_ctl(&s, XH_PG_PAD);

    hitag_cleanup(ledcontrol);
    reply_ng(cmd, res, NULL, 0);
}

void Xhorse_Configure(const lf_xhorse_data_t *packet, bool ledcontrol) {
    xh_configure_run(packet, CMD_LF_XHORSE_CONFIGURE, ledcontrol);
}

// Restore the factory-default ID46/HITAG2 personality: type 0x36, UID 0x1D421D12, the
// HITAG2 default password "MIKRON" and config page.  The chip is not blanked.
void Xhorse_Reset(bool ledcontrol) {
    lf_xhorse_data_t p;
    memset(&p, 0, sizeof(p));
    p.recipe = XHORSE_R_HITAG2;
    p.typebyte = 0x36;
    p.has_type = 1;
    p.uidlen = 4;
    p.uid[0] = 0x1D;
    p.uid[1] = 0x42;
    p.uid[2] = 0x1D;
    p.uid[3] = 0x12;
    p.verify = 1;

    xh_configure_run(&p, CMD_LF_XHORSE_RESET, ledcontrol);
}
