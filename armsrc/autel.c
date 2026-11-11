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
#include "fpga_loader.h"     // FpgaWriteConfWord / FpgaSendCommand / FpgaSetupSsc
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
// t_wait is counted from the tag's last captured edge, which the device's own trace
// shows landing ~79 T0 before the frame-end that the trace timestamps.  Commanding
// the Autel's gaps verbatim therefore put our frames ~78 T0 early on air (646 vs 725,
// 377 vs 455).  These are the Autel's gaps plus that offset, so the on-air spacing
// matches; check with `lf autel read ... ; trace list -t ht2`.
#define AT_T_EDGE_OFFSET   79
#define AT_T_UID_T0       (725 + AT_T_EDGE_OFFSET)   // tag echoes 1010 -> reader sends get-UID
#define AT_T_AUTH_T0      (455 + AT_T_EDGE_OFFSET)   // tag sends UID    -> reader sends {nR}{aR}
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
// Field-off between the probe session and the authenticated one (the Autel: 116 ms).
#define AT_T_SESSION_OFF_US 120000

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
// Session-open variants (lf_autel_io_t.mode, and the per-entry mode below).
#define AT_M_ONESESSION  1   // skip the Autel's probe session; authenticate in the first
#define AT_M_REWAKE      2   // re-send the wake just before the auth
#define AT_M_SOFTRESET   4   // DOUT-only field reset instead of taking the FPGA mode off
#define AT_M_FORCEUID    8   // skip get-UID, authenticate with a caller-supplied UID
#define AT_M_EMBURST    16   // send the EM-freeing burst + cold boot first

typedef struct {
    uint32_t off_us;
    uint32_t on_us;
    uint8_t  mode;
} at_timing_t;

static const at_timing_t at_timings[] = {
    // A real power-down, one session.  This is the only form measured to reach every
    // emulated type, and it is what the Autel's own captures show.  A chip emulating a
    // continuously-transmitting type (ID4C/TIRIS is the worst; EM4102 next) only opens its
    // management window after it genuinely loses power, and driving DOUT high does not take
    // it away - measured, see docs/at100.md SS13.  In ID4C mode this form authenticates
    // 5 times out of 5 where the DOUT-only entries below manage 4 out of 33.
    { 120000,            AT_T_POWERUP_US,     AT_M_ONESESSION },
    // Then the quicker DOUT-only resets: 20 ms instead of 120, and enough for any chip
    // emulating a type that stays quiet until spoken to.
    { AT_T_FIELD_OFF_US, AT_T_POWERUP_US,     AT_M_ONESESSION | AT_M_SOFTRESET },
    { AT_T_FIELD_OFF_US, AT_T_POWERUP_ALT_US, AT_M_ONESESSION | AT_M_SOFTRESET },
    // Last, the Autel's probe-session-then-auth-session pair over longer power-downs.
    // Kept because it reaches a chip the forms above cannot, but it is not the first
    // choice: against a talk-first chip the probe session leaves it less reachable, not
    // more (ID4C: stage 0 on every attempt where the one-session form above succeeded).
    { AT_T_FIELD_OFF_US, AT_T_POWERUP_US,     0 },
    { 120000,            AT_T_POWERUP_US,     0 },
    { 1500000,           AT_T_POWERUP_US,     0 },
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

// The AT100 needs a wider reader gap than the stock HITAG_T_LOW of 8.  Measured on
// the bench against a chip emulating EM4102 (which is the demanding case - it is
// load-modulating continuously while it listens): at 8 the chip echoed the 4-bit
// wake but reached a full authentication 0 times in 12; at 10, 12, 14 and 16 it
// authenticated 6 times out of 6, at every width.  That is the signature of missed
// gaps, whose effect compounds with frame length - which is why a 4-bit wake got
// through, a 10-bit command usually did not, and a 64-bit auth never did.
// 12 sits in the middle of the working range and matches the Autel programmer's own
// gaps, which measure 9..15 T0 (median 11..12) in the captures.
#define AT_T_LOW 12

// Widen (or restore) the reader's field-off pulse.  The Autel programmer's gaps
// measure 9..15 T0 in the captures against the stock HITAG_T_LOW of 8, and a tag
// that misses gaps fails frames in proportion to their length - which is what a
// chip that echoes a 4-bit wake, usually drops a 10-bit command and never answers
// a 64-bit one looks like.  Bring-up knob: 0 restores the default.
static void at_set_tlow(uint8_t tlow) {
    g_hitag_t_low = tlow ? tlow : AT_T_LOW;
}

// Every autel entry point must come through here, so a widened pulse can never
// leak into the next `lf hitag` command.
static void at_teardown(bool ledcontrol) {
    g_hitag_t_low = HITAG_T_LOW;
    hitag_cleanup(ledcontrol);
}

// Power-cycle the chip, for real.
//
// Driving DOUT high is how a BPLM gap is made, and it is what xhorse.c calls a field
// reset - but it only stops the modulation drive.  Measured against an AT100 left
// emulating EM4102: after three seconds of DOUT-high the chip had *not* rebooted
// (it was still in whatever mode the previous frame put it in), so a chip that is
// broadcasting never cold-boots and never re-opens its management window.
//
// Taking the FPGA's major mode off removes the carrier itself, which does power the
// chip down.  Coming back is the tail of hitag_setup_fpga() - divisor, conf word,
// threshold, ADC mux, SSC - and the edge capture has to be restarted around it:
// FpgaWriteConfWord()/FpgaSendCommand() bit-bang the same SSC lines that
// StartLoEdgeCapture() claims via PIO_BSR, so routing set up before them is undone
// (see the comment in hitag_common.c).
static void at_field_reset_ex(uint32_t off_us, uint32_t powerup_us, bool real) {
    if (real == false) {
        // The DOUT-only form: how xhorse.c does it, and how this file did until the
        // AT100 turned out not to reboot from it.  Kept for A/B on the bench.
        Gpio_SSC_DOUT_High();
        at_delay_us(off_us);
        Gpio_SSC_DOUT_Low();
        at_delay_us(powerup_us);
        return;
    }
    StopLoEdgeCapture();
    Gpio_SSC_DOUT_High();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);

    at_delay_us(off_us);

    FpgaSendCommand(FPGA_CMD_SET_DIVISOR, LF_DIVISOR_125);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_EDGE_DETECT | AT_EDGE_MODE);
    if (AT_FPGA_THR != 127) {
        FpgaSendCommand(FPGA_CMD_SET_EDGE_DETECT_THRESHOLD, AT_FPGA_THR);
    }
    SetAdcMuxFor(ADC_MUXSEL_LOPKD);
    FpgaSetupSsc(FPGA_MAJOR_MODE_LF_READER);
    StartLoEdgeCapture();
    gpio_fpga_mod_only_setup();

    Gpio_SSC_DOUT_Low();            // modulation off -> field ON
    at_delay_us(powerup_us);
}

static bool at_real_reset = true;   // bring-up knob (mode)

static void at_field_reset(uint32_t off_us, uint32_t powerup_us) {
    at_field_reset_ex(off_us, powerup_us, at_real_reset);
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
static const uint8_t at_wake[1]   = { 0xA0 };        // 4 bits: 1010
static const uint8_t at_getuid[2] = { 0xF8, 0x00 };  // 10 bits: 11111 00000 ({W}{~W}, W = 0b11111)

// Wake the chip and read its UID.  Returns PM3_SUCCESS with *uid set.
// Stage reached, for the bring-up log: 0 = no wake reply, 1 = wake reply was not
// the 1010 echo, 2 = wake ok but no UID, 3 = UID read.
static int at_wake_uid(at_sess_t *s, uint32_t *uid, int uid_t0) {
    uint8_t rx[8];
    if (at_xfer(s, at_wake, 4, rx, sizeof(rx), AT_T_WAKE_T0) < 4) {
        return 0;                                       // nothing on the coil, or buried in noise
    }
    if (at_bits_to_u32(rx, 4) != 0x0A) {
        return 1;                                       // answered, but not with the AT100's echo
    }
    if (at_xfer(s, at_getuid, 10, rx, sizeof(rx), uid_t0) < 32) {
        return 2;
    }
    *uid = at_bits_to_u32(rx, 32);
    return 3;
}


//-----------------------------------------------------------------------------
// EM-freeing burst (ported from armsrc/xhorse.c, doc/xhorse_notes.md)
//
// A chip left emulating a talk-first type broadcasts continuously.  The Xhorse VVDI
// frees an XT27A with a timed sequence of field gaps landing at a fixed position in
// the chip's EM frame, then a 1.57 s cold boot, then its handshake - and the Autel
// captures have exactly that shape off-screen: all three traces that begin against an
// EM-type AT100 open with 2.6-3.0 s of dead air preceded by the tail of something the
// sniff did not catch.  Whether the AT100 answers the same burst is an open question;
// this is here to find out.
//-----------------------------------------------------------------------------
#define AT_T_EM_OFF_US        70050     // field off before and after each burst window
#define AT_T_EM_COLDBOOT_US 1570940     // power-down between the bursts and the handshake
#define AT_T_EM_SETTLE_US     30180     // field on this long before the wake

static const uint16_t at_emb_start[43] = {
    0, 36, 68, 129, 163, 224, 258, 290, 351, 415, 449, 511, 576, 610, 641, 702, 767, 832, 865, 927,
    961, 992, 1024, 1054, 1115, 1180, 1214, 1245, 1306, 1340, 1372, 1433, 1497, 1562, 1596, 1658,
    1692, 1753, 1787, 1818, 1880, 1944, 1978
};
static const uint8_t at_emb_width[43] = {
    20, 16, 15, 19, 16, 19, 15, 15, 19, 19, 15, 19, 19, 16, 15, 19, 19, 19, 16, 19, 16, 15, 15, 15,
    19, 19, 16, 15, 19, 16, 15, 19, 19, 19, 16, 19, 16, 19, 16, 15, 19, 19, 16
};

// Emit the burst with the field on.  One counter reference covers it (1994 T0 = 23928
// ticks < 65536), so pulse positions do not accumulate per-pulse error.
static void at_em_burst(void) {
    const uint16_t ref = GetPrecisionCounterRaw();
    for (size_t i = 0; i < ARRAYLEN(at_emb_start); i++) {
        while (GetPrecisionCounterDelta(ref) < (uint16_t)(T0 * at_emb_start[i])) {};
        Gpio_SSC_DOUT_High();
        while (GetPrecisionCounterDelta(ref) < (uint16_t)(T0 * (at_emb_start[i] + at_emb_width[i]))) {};
        Gpio_SSC_DOUT_Low();
    }
}

// Burst windows: field-on window length, and when in it the burst is commanded.
static const uint32_t at_em_on_us[2]    = { 155940, 60300 };
static const uint32_t at_em_at_us[2]    = { 109540, 13990 };   // ID11 frame 6; ID12/ID13 first frame

// One round of the VVDI's shape: a burst per timing, each in its own power cycle, then
// a single long cold boot.  Leaves the field on, settled, ready for the wake.
static void at_em_free(void) {
    const uint32_t burst_len_us = ((uint32_t)at_emb_start[ARRAYLEN(at_emb_start) - 1]
                                   + at_emb_width[ARRAYLEN(at_emb_width) - 1]) * 8;
    for (size_t t = 0; t < ARRAYLEN(at_em_on_us); t++) {
        Gpio_SSC_DOUT_High();
        at_delay_us(AT_T_EM_OFF_US);
        Gpio_SSC_DOUT_Low();
        at_delay_us(at_em_at_us[t]);
        at_em_burst();
        at_delay_us(at_em_on_us[t] - at_em_at_us[t] - burst_len_us);
    }
    Gpio_SSC_DOUT_High();
    at_delay_us(AT_T_EM_COLDBOOT_US);
    Gpio_SSC_DOUT_Low();
    at_delay_us(AT_T_EM_SETTLE_US);
}

static uint32_t forced_uid = 0;   // bring-up knob (mode bit 3)

// Furthest point any open attempt reached, and the UID if one was read.  Kept
// across the whole at_open_ex() schedule so a failed open still reports what it
// saw: "echoed the wake, then ignored the auth" and "nothing on the coil" are
// very different problems and must not print the same message.
static uint8_t at_best_stage = AUTEL_STAGE_NONE;
static uint32_t at_best_uid = 0;

static int at_open_once(at_sess_t *s, uint64_t key, uint32_t off_us, uint32_t powerup_us, int uid_t0, int auth_t0, uint8_t mode) {
    uint8_t rx[8];

    // The Autel never authenticates in the session it first reaches the chip in.  In
    // all 29 captures it opens a UID-only session, drops the field, and authenticates
    // in the next one - so do the same.  It matters: against a chip left emulating
    // EM4102 the one-session form reads the UID correctly and then gets no answer at
    // all to {nR}{aR}.  The first session is also what silences a talk-first chip,
    // which is why its own UID read is allowed to fail.
    // mode: bit 0 clear => the Autel's two sessions (probe, field reset, auth session);
    //       bit 0 set   => one session.  bit 1 set => re-send the wake just before the
    //       auth.  bit 2 set => use the DOUT-only field reset instead of a real one.
    at_real_reset = ((mode & AT_M_SOFTRESET) == 0);

    if (mode & AT_M_EMBURST) {
        at_em_free();                    // bursts + cold boot, field left on and settled
    }

    int st0 = -1;
    uint32_t probe = 0;
    if ((mode & AT_M_EMBURST) != 0) {
        // the burst sequence already left the chip powered and settled
    } else if ((mode & AT_M_ONESESSION) == 0) {
        at_field_reset(off_us, powerup_us);
        st0 = at_wake_uid(s, &probe, uid_t0);
        at_field_reset(AT_T_SESSION_OFF_US, powerup_us);
    } else {
        at_field_reset(off_us, powerup_us);
    }

    int st1;
    if (mode & AT_M_FORCEUID) {
        // Wake only, then authenticate with a UID the caller already knows: tests
        // whether the get-UID frame is what leaves the chip unable to authenticate.
        uint8_t wrx[8];
        st1 = (at_xfer(s, at_wake, 4, wrx, sizeof(wrx), AT_T_WAKE_T0) >= 4
               && at_bits_to_u32(wrx, 4) == 0x0A) ? 3 : 0;
        s->uid = forced_uid;
    } else {
        st1 = at_wake_uid(s, &s->uid, uid_t0);
    }
    if (st1 > at_best_stage) {
        at_best_stage = (uint8_t)st1;        // at_wake_uid()'s 0..3 are AUTEL_STAGE_NONE..UID
    }
    if (st1 == 3) {
        at_best_uid = s->uid;
    }

    if (st1 != 3) {
        // Logged at DBG_DEBUG, not DBG_EXTENDED: at extended the hitag frame dumps
        // flood the USB log and these lines get dropped.
        if (g_dbglevel >= DBG_DEBUG) {
            Dbprintf("autel: m%d off %u on %u | s0 %d %08x | s1 stage %d uid %08x | no auth",
                     mode, off_us, powerup_us, st0, probe, st1, s->uid);
        }
        return PM3_EFAILED;
    }

    s->cs = ht2_hitag2_init(at_rev(key, 48), (uint32_t)at_rev(s->uid, 32), (uint32_t)at_rev(AUTEL_READER_NR, 32));
    uint32_t aR = at_ks(s, 32) ^ 0xFFFFFFFFUL;

    if (mode & AT_M_REWAKE) {
        uint8_t wrx[8];
        at_xfer(s, at_wake, 4, wrx, sizeof(wrx), AT_T_WAKE_T0);
    }

    uint8_t tx[8];
    at_u32_to_bits(AUTEL_READER_NR, 32, tx);
    at_u32_to_bits(aR, 32, tx + 4);

    size_t rxlen = at_xfer(s, tx, 64, rx, sizeof(rx), auth_t0);
    s->config = at_bits_to_u32(rx, 32) ^ at_ks(s, 32);
    if (g_dbglevel >= DBG_DEBUG) {
        Dbprintf("autel: m%d off %u on %u | s0 %d %08x | s1 uid %08x aR %08x | auth rx %d cfg %08x",
                 mode, off_us, powerup_us, st0, probe, s->uid, aR, (int)rxlen, s->config);
    }
    if (rxlen < 32) {
        return PM3_EFAILED;
    }
    at_best_stage = AUTEL_STAGE_AUTH;
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
static int at_open_tuned(at_sess_t *s, uint64_t key, uint32_t off_us, uint32_t on_us, int tries, int uid_t0, int auth_t0, uint8_t mode) {
    for (int attempt = 0; attempt < tries; attempt++) {
        int res = at_open_once(s, key, off_us, on_us, uid_t0, auth_t0, mode);
        if (res != PM3_SUCCESS) {
            continue;
        }
        if (s->auth_ok || (attempt + 1 == tries)) {
            return PM3_SUCCESS;
        }
    }
    return PM3_EFAILED;
}

static int at_open_ex(at_sess_t *s, uint64_t key, uint32_t off_us, uint32_t on_us, int uid_t0, int auth_t0, uint8_t mode) {
    at_best_stage = AUTEL_STAGE_NONE;
    at_best_uid = 0;
    if (uid_t0 <= 0) {
        uid_t0 = AT_T_UID_T0;
    }
    if (auth_t0 <= 0) {
        auth_t0 = AT_T_AUTH_T0;
    }
    if (off_us || on_us || mode) {
        return at_open_tuned(s, key, off_us ? off_us : AT_T_FIELD_OFF_US,
                             on_us ? on_us : AT_T_POWERUP_US, AT_TRIES, uid_t0, auth_t0, mode);
    }
    for (size_t i = 0; i < ARRAYLEN(at_timings); i++) {
        if (at_open_tuned(s, key, at_timings[i].off_us, at_timings[i].on_us, AT_TRIES,
                          uid_t0, auth_t0, at_timings[i].mode) == PM3_SUCCESS) {
            return PM3_SUCCESS;
        }
    }
    return PM3_EFAILED;
}

static int at_open(at_sess_t *s, uint64_t key) {
    return at_open_ex(s, key, 0, 0, 0, 0, 0);
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
    out->stage = AUTEL_STAGE_AUTH;
    out->auth_ok = s->auth_ok ? 1 : 0;
    out->uid = s->uid;
    out->config = s->config;
}

// A session open that never completed.  found is still set whenever anything at all
// answered, so the caller can tell a silent antenna from a chip that talks up to a
// point and then stops.
static void at_fill_partial(lf_autel_io_reply_t *out) {
    out->stage = at_best_stage;
    out->found = (at_best_stage > AUTEL_STAGE_NONE) ? 1 : 0;
    out->uid = at_best_uid;
}

//-----------------------------------------------------------------------------
// Entry points
//-----------------------------------------------------------------------------
void Autel_Read(const lf_autel_io_t *p, bool ledcontrol) {
    at_sess_t s;
    forced_uid = p->value;   // only consulted when mode bit 3 is set
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
    at_set_tlow(p->tlow);
    if (at_open_ex(&s, at_key(p->key), (uint32_t)p->off_ms * 1000, p->on_us, p->uid_t0, p->auth_t0, p->mode) != PM3_SUCCESS) {
        at_fill_partial(&out);
        at_teardown(ledcontrol);
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

    at_teardown(ledcontrol);
    reply_ng(CMD_LF_AUTEL_READ, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
}

void Autel_Write(const lf_autel_io_t *p, bool ledcontrol) {
    at_sess_t s;
    lf_autel_io_reply_t out;
    memset(&out, 0, sizeof(out));

    at_setup(&s, ledcontrol);
    at_set_tlow(p->tlow);
    if (at_open_ex(&s, at_key(p->key), (uint32_t)p->off_ms * 1000, p->on_us, p->uid_t0, p->auth_t0, p->mode) != PM3_SUCCESS) {
        at_fill_partial(&out);
        at_teardown(ledcontrol);
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

    at_teardown(ledcontrol);
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
    at_set_tlow(p->tlow);
    if (at_open_ex(&s, at_key(p->key), (uint32_t)p->off_ms * 1000, p->on_us, p->uid_t0, p->auth_t0, p->mode) != PM3_SUCCESS) {
        at_fill_partial(&out);
        at_teardown(ledcontrol);
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

    at_teardown(ledcontrol);
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
    at_set_tlow(0);
    if (at_open(&s, at_key(p->key)) != PM3_SUCCESS) {
        out.stage = at_best_stage;
        out.found = (at_best_stage > AUTEL_STAGE_NONE) ? 1 : 0;
        out.uid = at_best_uid;
        at_teardown(ledcontrol);
        reply_ng(cmd, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
        return;
    }
    out.found = 1;
    out.stage = AUTEL_STAGE_AUTH;
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

    at_teardown(ledcontrol);
    reply_ng(cmd, res, (uint8_t *)&out, sizeof(out));
}
