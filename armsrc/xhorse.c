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
//   * the XT136 handshake (E20E/E430, seed constant 0x8C07, no banks), read-only so far.
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
// A chip emulating a type it goes deaf on - ID48, ID8E, ID8A, an EM type, a blanked 7935 - needs far
// longer off the coil before it listens for XMICRO again.  The Xhorse tool's own recovery drops the
// field for 720.3 ms (90043 T0) and only then gets its ack, which is why 20 ms here and the 228 ms
// between reset rounds never reached an ID48'd XT37 (docs/xt37.md SS5s/SS5t).
#define XH_T_FIELD_OFF_LONG_US 760000
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
// while bank 0 still held the ID8A image), so reset/configure send it and a read only does on
// request (xh_sess_t.probe, `--probe`).
//
// The VVDI sends THREE words in this code, and this is the last of them (docs/xt57b-fullread.md
// SS12.4).  Before it: an 18-symbol word "SLSSSSLLLLSSSSSSSS" at ~0.19 s, which is not ID8A-
// specific - it is the whole of the XT136 reset preamble (docs/xt136.md SS4.2) - and then a
// second 98-symbol word (xh_id8a_probe_a below), twice, each followed by an XMICRO that gets no
// answer.  Only after this word, sent six times, does XHORSE reach the chip - the same first-word /
// last-word shape as the Megamos family.
//
// Nothing answers either word: the envelope after the last gap is flat through the 31.74 ms of
// field-on and the 177.1 ms of field-off, in both captures.  And the VVDI never sends XHORSE
// after word A - decoding every 48-bit frame in vvdi-id8A-reset-success.pm3 gives XMICRO six
// times and XHORSE once, at 3.68 s, after the sixth word B.  So whether word A is a harmless
// read was never tested.  `lf xhorse info --probe a` asked, on an XT27A set to ID8A: word A does
// open a session (timing 3, 4R ok), but bank-1 0x3C then reads 0x00 where word B leaves 0x36 -
// three runs, against a configure that had just verified 0x8A.  So A is not a harmless read, and
// the two words do different things to the chip (Update 32).
//
// The gap width carries nothing (unimodal at 23 T0, same before an S as before an L), so the
// word is 98 bits.  Those bits do not decode: no ASCII under any expansion, no CRC-16 over
// 27,888 framings, and not TI's write encoding (that is a 2 ms bit cell with a varying off time,
// 112 bits; this is 416/536 us with a constant one).  It is a fixed constant - identical across
// eight bursts in two captures - so it is not a DST challenge, which is why replay works.  They
// are not interchangeable wake patterns either: A and B leave the type byte at different values,
// so the 98 bits are a payload the chip reads, not a pattern it merely matches.
//
// Mutating the word (--probe-word, Updates 33-35) shows what guards it.  All 98 single-symbol
// flips of word B were swept against hardware (tools/analysis/probe8a_sweep.py,
// data/probe8a-sweep.jsonl): 97 are refused outright, including every flip inside the long run of
// S, so none of it is padding and nothing is parsed into fields.  The one exception is symbol 5,
// which is accepted and changes the outcome to 0x00.  The check is on the symbol values, not on
// timing: A and B differ in total duration and both work.  And 0x00 is simply "passed the check
// but is not word B" - word A and B-with-symbol-5-flipped both land there - while 0x36 is what
// word B actually writes.
//
// It is not a checksum.  Multi-symbol flips (--flips, data/probe8a-pairs.jsonl, -pos5, -cut and
// -header) refuse six different double flips away from symbol 5, which any equal-weight checksum
// would wave through, yet accept 5+40 and 5+60, which parity says must fail.  Mapping symbol 5
// against every other symbol gives the structure (Update 36):
//
//     accept  iff  symbols 0..14 match word B's, ignoring symbols 5 and 9
//                  and ( symbol 5 == L  or  the whole word is A or B )
//
// So the chip reads thirteen of the 98 symbols: a fixed preamble in 0..14 - no single-symbol
// variant of it is ever accepted - with a one-bit command selector at symbol 5 and symbol 9
// reserved.  L at symbol 5 is a command with no payload: the header with symbols 15..97 all S, and
// the same header with them all L, are both accepted.  S at symbol 5 takes the other 83 symbols as
// a payload and accepts exactly two, word A's tail and word B's tail, with no tolerance anywhere in
// them - which is why replay is the only way in and why xh_id8a_probe must stay byte-exact.  The
// rule reproduces all 151 hardware points, 142 distinct words, with no exceptions.  0x36 is what
// word B's payload writes; 0x00 is what every other accepted word leaves behind.  A rejected word
// leaves the chip untouched and deaf, and every accepted word leaves bank 0 byte-identical - the
// type byte is the only casualty.  tools/analysis/probe8a.py re-derives the three words from a
// trace; probe8a_sweep.py is the mutation harness.
#define XH_T_ID8A_CHARGE_T0  6255
#define XH_T_ID8A_GAP_T0     22       // nominal; our gaps sniff ~1 T0 wider than set
#define XH_T_ID8A_FIRST_T0   81
#define XH_T_ID8A_TAIL_T0    3968
static const char xh_id8a_probe[] =
    "SLLSSSLLSLLLSSLSSSSLLSLLSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSLSLSSSLSLSSL";
// Word A, the one the VVDI sends twice before it.  It opens the chip as B does, but leaves the
// emulated type byte at 0x00 instead of reverting it to 0x36, so it is no safer and 0x00 is not
// even a valid type.  Per Update 34 it carries no payload of its own - it is just a word that
// passes the check without being B.  Sent only when asked for by name (XHORSE_PROBE_A).
static const char xh_id8a_probe_a[] =
    "SLLSSSLLSLLLSSLLLSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSLLSLLSSSLLSSSSLLSSSSLSS";
// A word handed down by the client (XHORSE_PROBE_CUSTOM), for asking the chip what the bits mean.
static char xh_id8a_probe_custom[XHORSE_ID8A_SYMS + 1];

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
// What frees it is one reader burst sent a set time after power-up (xh_em_burst): a short LF
// downlink frame carrying the ASCII word "erase", generated below.  In a sniff of a successful reset the chip broadcasts
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
// different word (xh_burst_id48) at 1622 T0.  We don't know the emulated type, so the entries are
// tried in turn.
#define XH_T_EM_OFF_US            70050    // field off before and after the burst window (VVDI)
#define XH_T_EM_SETTLE_US         30180    // field on this long before XHORSE (VVDI, after the chip is freed)
#define XH_T_EM_COLDBOOT_US     1570940    // VVDI's power-down between the bursts and its handshake (ID13 needs it)
#define XH_EM_TRIES                   3    // attempts: one burst per xh_em_timings[] entry, cold-boot, handshake

// Phase sweep.  A burst only frees the chip if it lands at the right point in the chip's
// broadcast frame, and the frame period differs per type (ID11 RF/32, ID12 RF/40, ID13 RF/64),
// so one fixed offset that suits one chip can miss on another - an ID12 chip that neither the
// VVDI's own offset nor ours would free is what put this here.  After a first pass at the
// nominal offsets (the fast path, and what used to be the only path), later attempts slip the
// burst in 5 T0 steps across a whole 40 T0 ID12 frame.
#define XH_EM_SWEEP_N                 8    // offsets tried on the attempts after the first
#define XH_EM_SWEEP_STEP_US          40    // 5 T0 per step; 8 steps = 320 us = one ID12 frame

// func field (top 2 bits of the op byte)
#define XH_FUNC_CTL   0
#define XH_FUNC_SEL   1
#define XH_FUNC_WRITE 2
#define XH_FUNC_READ  3

// bank-1 (config/identity) addresses
#define XH_PG_TYPE    0x3C            // emulated type byte
#define XH_PG_ID0     0x33            // 16-byte unique chip ID: pages 0x33..0x36
#define XH_PG_CFG0    0x3D            // config word n (written by ctl 0x30+n) reads back at 0x3D+n
#define XH_PG_PAD     0x3F

#define XH_SEED_XT27A 0xE12E          // seed = bitrev16(7372 reply) ^ this

// XT136: same LFSR and op,~op commands, different handshake.  After XHORSE it answers E20E with a
// fixed 32-bit signature and E430 with the 16-bit seed reply; it ignores 072F.  Timings are the
// VVDI's (vvdi-xt136-*.pm3): field off 130.1 ms, XHORSE 220.3 ms after power-up, E430 ~90 T0 after
// the signature, and 21.3 ms before the first encrypted command.
#define XH136_SIG            0x604A027EUL
#define XH136_SEED           0x8C07
#define XH136_T_SIG_SEED_T0  90
#define XH136_T_FIRST_US     21300
#define XH136_T_OFF_US       130100
#define XH136_T_ON_US        220300
// A ctl-with-data (the config-word write) takes far longer to commit on an XT136 than a plain WRITE
// does: the VVDI's reset capture leaves 3846-3848 T0 (30.8 ms) between the data frame and the next
// command, against 1596 T0 (12.8 ms) after a WRITE (docs/xt136.md SS4.3).  On an XT27A the same gap
// is 1347 T0 (10.8 ms), which XH_T_PROG_US already covers.  31 ms gives the XT136 a small margin.
#define XH136_T_CTL_PROG_US  31000
// Config words are written by the same ctl 0x30+n on both families but read back from a different
// page: bank-1 0x3D+n on an XT27A, 0x0D+n in the XT136's flat space (docs/xt136.md SS5).
#define XH136_PG_CFG0        0x0D
#define XH136_TRIES          3        // per timing (VVDI's, then the XT27A's short one)

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

static void xh_ks_init(xh_ks_t *k, uint16_t reply, uint16_t seed_const) {
    k->seed = xh_rev16(reply) ^ seed_const;
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
    uint8_t probe;       // xhorse_probe_t: which ID8A probe word timing 3 may send (0 = none)
    uint8_t family;      // xhorse_family_t of the open session
    uint16_t seed_reply; // the tag's reply to 7372 / E430, which seeds the keystream
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
// t_wait is T0 since the last tag edge; 0 when the caller has already spaced this frame with xh_delay_us().
// Returns the number of reply bits demodulated.
static size_t xh_xfer_clear(xh_sess_t *s, const uint8_t *frame, size_t nbits,
                            uint8_t *rx, size_t rxsz, int t_wait) {
    size_t rxlen = 0;
    memset(rx, 0, rxsz);
    hitag_reader_transfer(frame, nbits, rx, rxsz, &rxlen, t_wait, s->led, XH_MOD, XH_SOF_BITS, XH_SEND_SOF);
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

// sel a bank (0 = emulated-tag EEPROM, 1 = config/identity).  The tag echoes the sel op.
// A missing or wrong echo is re-sent: if the chip did act on the first sel, sending it again
// is harmless and keeps both keystreams in step.  If no echo matches, the selected bank is
// unknown, so the caller must not write.
static int xh_sel(xh_sess_t *s, uint8_t bank) {
    const uint16_t op = xh_op(XH_FUNC_SEL, bank);
    for (int i = 0; i < XH_VERIFY_READS; i++) {
        uint32_t echo = 0;
        if ((xh_cmd_reply(s, op, 16, &echo) == PM3_SUCCESS) && ((uint16_t)echo == op)) {
            s->cur_bank = (int8_t)bank;
            return PM3_SUCCESS;
        }
    }
    if (g_dbglevel >= DBG_ERROR) {
        Dbprintf("xhorse: sel bank %u not echoed", bank);
    }
    s->cur_bank = -1;
    return PM3_EFAILED;
}

static int xh_bank(xh_sess_t *s, uint8_t bank) {
    if (s->cur_bank == (int8_t)bank) {
        return PM3_SUCCESS;
    }
    return xh_sel(s, bank);
}

// READ a page in the current bank; tag answers with 32-bit data.
static int xh_read(xh_sess_t *s, uint8_t page, uint32_t *val) {
    return xh_cmd_reply(s, xh_op(XH_FUNC_READ, page), 32, val);
}

// READ a page until two answered reads in a row agree.  A weak reply can decode to a wrong value
// with nothing to show for it (a type byte of 0x37 once read as 0xC8), so one read is not trusted.
// Returns false, with the last value read, if no two consecutive reads agree.
static bool xh_read_agreed(xh_sess_t *s, uint8_t page, uint32_t *val) {
    uint32_t prev = 0;
    bool prev_ok = (xh_read(s, page, &prev) == PM3_SUCCESS);
    for (int i = 0; i < XH_VERIFY_READS; i++) {
        uint32_t cur = 0;
        bool cur_ok = (xh_read(s, page, &cur) == PM3_SUCCESS);
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
            if (g_dbglevel >= DBG_ERROR) {
                Dbprintf("xhorse: verify failed, bank %d page 0x%02X", s->cur_bank, page);
            }
            return PM3_EFAILED;
        }
    }
    return PM3_SUCCESS;
}

// ctl with a data frame, shaped like a WRITE: command (+echo, which is ~op,op rather than a copy),
// then 32-bit data, then the program wait.  The VVDI ends an ID8A set with ctl 0x30 carrying the
// value it has just read from bank-1 page 0x3D.  The wait is per family: an XT136 needs 30.8 ms
// here, not the WRITE path's 12.8 ms (XH136_T_CTL_PROG_US).
static void xh_ctl_data(xh_sess_t *s, uint8_t page, uint32_t data) {
    xh_cmd_reply(s, xh_op(XH_FUNC_CTL, page), 16, NULL);

    uint32_t ct = data ^ xh_ks_next32(&s->ks);
    uint8_t tx[4] = { (uint8_t)(ct >> 24), (uint8_t)(ct >> 16), (uint8_t)(ct >> 8), (uint8_t)ct };
    hitag_reader_send_frame(tx, 32, s->led, XH_SEND_SOF);
    xh_delay_us((s->family == XHORSE_FAM_XT136) ? XH136_T_CTL_PROG_US : XH_T_PROG_US);
}

// Page config word n reads back from, per family (XH136_PG_CFG0).
static uint8_t xh_cfg_page(const xh_sess_t *s, uint8_t n) {
    uint8_t base = (s->family == XHORSE_FAM_XT136) ? XH136_PG_CFG0 : XH_PG_CFG0;
    return (uint8_t)(base + n);
}

// Write config word n: ctl 0x30+n with data, which reads back at bank-1 0x3D+n on an XT27A and at
// 0x0D+n on an XT136 (doc/xhorse_notes.md).  On an XT27A bank 1 must be selected; unlike a bare ctl
// this does not clear the selection (the VVDI reads straight back).
static int xh_cfg_write(xh_sess_t *s, uint8_t n, uint32_t data, bool verify) {
    xh_ctl_data(s, (uint8_t)(0x30 + n), data);
    if (verify == false) {
        return PM3_SUCCESS;
    }
    for (int i = 0; i < XH_VERIFY_READS; i++) {
        uint32_t rb = 0;
        xh_read(s, xh_cfg_page(s, n), &rb);
        if (rb == data) {
            return PM3_SUCCESS;
        }
    }
    if (g_dbglevel >= DBG_ERROR) {
        Dbprintf("xhorse: verify failed, config word %u", n);
    }
    return PM3_EFAILED;
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

    rxlen = xh_xfer_clear(s, q7372, 16, rx, sizeof(rx), 0);               // -> per-session seed reply
    if (rxlen < 16) {
        if (g_dbglevel >= DBG_DEBUG) {
            Dbprintf("xhorse: no 7372 reply (rxlen=%u)", (unsigned)rxlen);
        }
        return PM3_EFAILED;
    }

    uint16_t reply = (uint16_t)((rx[0] << 8) | rx[1]);
    if (g_dbglevel >= DBG_EXTENDED) {
        Dbprintf("xhorse: 7372 reply=%04x  seed=%04x", reply, xh_rev16(reply) ^ XH_SEED_XT27A);
    }
    s->seed_reply = reply;
    xh_ks_init(&s->ks, reply, XH_SEED_XT27A);
    s->cur_bank = -1;
    s->family = XHORSE_FAM_XT27A;
    return PM3_SUCCESS;
}

// XT136 handshake: XHORSE -> E20E -> 32-bit family signature, E430 -> 16-bit seed reply.  E20E goes
// where the XT27A's 072F does; E430 follows the signature the way the VVDI sends it.  There is no 4R
// check (the XT136 has no fixed first command), so the exact signature is what proves the session.
static int xh_handshake_xt136(xh_sess_t *s) {
    static const uint8_t magic[6] = { 'X', 'H', 'O', 'R', 'S', 'E' };
    static const uint8_t qe20e[2] = { 0xE2, 0x0E };
    static const uint8_t qe430[2] = { 0xE4, 0x30 };

    uint8_t rx[32];
    size_t rxlen;

    xh_send_clear(magic, 48, s->led);
    xh_delay_us(XH_T_XHORSE_072F_US);

    rxlen = xh_xfer_clear(s, qe20e, 16, rx, sizeof(rx), 0);
    uint32_t sig = xh_bits_to_u32(rx, 32);
    if ((rxlen < 32) || (sig != XH136_SIG)) {
        if (g_dbglevel >= DBG_DEBUG) {
            Dbprintf("xhorse: no XT136 signature (rxlen=%u, %08x)", (unsigned)rxlen, sig);
        }
        return PM3_EFAILED;
    }

    rxlen = xh_xfer_clear(s, qe430, 16, rx, sizeof(rx), XH136_T_SIG_SEED_T0);
    if (rxlen < 16) {
        if (g_dbglevel >= DBG_DEBUG) {
            Dbprintf("xhorse: no E430 reply (rxlen=%u)", (unsigned)rxlen);
        }
        return PM3_EFAILED;
    }

    // Keep the reply for the caller to report once the session is over.  It must not be printed
    // here: a Dbprintf between the reply and XH136_T_FIRST_US eats into that 21.3 ms gap and the
    // chip then misses the first command.
    uint16_t reply = (uint16_t)((rx[0] << 8) | rx[1]);
    s->seed_reply = reply;
    if (g_dbglevel >= DBG_EXTENDED) {
        Dbprintf("xhorse: E430 reply=%04x  seed=%04x", reply, xh_rev16(reply) ^ XH136_SEED);
    }
    xh_ks_init(&s->ks, reply, XH136_SEED);
    s->cur_bank = -1;
    s->family = XHORSE_FAM_XT136;
    xh_delay_us(XH136_T_FIRST_US);
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

// Send one ID8A probe word: 100 field gaps about 23 T0 wide, the first two 81 T0 apart and the
// rest 52 T0 (S) or 67 T0 (L), then hold the field on for the 31.74 ms tail.  The caller has
// already powered the chip up and waited out the 50.04 ms charge.
static void xh_send_id8a_probe(const char *word, size_t n) {
    for (size_t i = 0; i < n + 2; i++) {
        uint32_t next;                               // gap start to next gap start (T0)
        if (i == 0) {
            next = XH_T_ID8A_FIRST_T0;
        } else if (i <= n) {
            next = (word[i - 1] == 'L') ? 67 : 52;
        } else {
            next = XH_T_ID8A_TAIL_T0;                // last gap -> field off
        }
        Gpio_SSC_DOUT_High();
        xh_delay_us(XH_T_ID8A_GAP_T0 * 8);
        Gpio_SSC_DOUT_Low();
        xh_delay_us((next - XH_T_ID8A_GAP_T0) * 8);
    }
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
    if (g_dbglevel >= (ok ? DBG_EXTENDED : DBG_DEBUG)) {
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
    const int timings = (s->probe != XHORSE_PROBE_NONE) ? 4 : 3;
    for (int attempt = 0; attempt < timings * XH_TRIES_PER_TIMING; attempt++) {
        int mode = s->em_mode ? 1 : (attempt % timings);
        if (g_dbglevel >= DBG_EXTENDED) {
            Dbprintf("xhorse: session attempt %d, timing %d", attempt, mode);
        }
        if (mode == 3) {
            // The VVDI's whole last round before its first XHORSE to a chip set to ID8A: the TI-style
            // probe, then the long off and XHORSE 41.8 ms after power-up (see XH_T_ID8A_CHARGE_T0).
            const char *word = (s->probe == XHORSE_PROBE_A)      ? xh_id8a_probe_a
                               : (s->probe == XHORSE_PROBE_CUSTOM) ? xh_id8a_probe_custom
                               :                                     xh_id8a_probe;
            const size_t n = XHORSE_ID8A_SYMS;               // 98 S/L intervals -> 100 gaps
            xh_field_reset();
            xh_delay_us(XH_T_ID8A_CHARGE_T0 * 8 - XH_T_POWERUP_US);
            xh_send_id8a_probe(word, n);
            Gpio_SSC_DOUT_High();
            xh_delay_us(XH_T_ID8A_OFF_US);
            Gpio_SSC_DOUT_Low();
            xh_delay_us(XH_T_ID8A_ON_US);
        } else if (mode == 1) {
            // The VVDI's timing: off 70 ms, on, XHORSE 30.18 ms later.  Always used once the chip has
            // been freed from EM emulation this run, and in the timing rotation otherwise: an ID13
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

// Open an XT136 session: alternate the VVDI's power-up (130.1 ms off, XHORSE 220.3 ms on) and the
// XT27A's short one.  Only the VVDI's is confirmed on air.
static int xh_open_session_xt136(xh_sess_t *s) {
    for (int attempt = 0; attempt < 2 * XH136_TRIES; attempt++) {
        if ((attempt % 2) == 0) {
            Gpio_SSC_DOUT_High();
            xh_delay_us(XH136_T_OFF_US);
            Gpio_SSC_DOUT_Low();
            xh_delay_us(XH136_T_ON_US);
        } else {
            xh_field_reset();
        }
        if (xh_handshake_xt136(s) == PM3_SUCCESS) {
            if (g_dbglevel >= DBG_EXTENDED) {
                Dbprintf("xhorse: XT136 session open on attempt %d", attempt);
            }
            return PM3_SUCCESS;
        }
    }
    return PM3_EFAILED;
}

// The emulation-exit burst.
//
// It is not an XHORSE command and not a recorded waveform: it is a plain 125 kHz reader-to-tag
// downlink, the kind the T5577/EM4x05 families use.  Each symbol is a field-ON time that carries
// the symbol's value followed by a field gap that closes the cell, and a start gap opens the
// frame.  Timings below are T0, measured over 120 bursts in 11 VVDI captures of both chips
// (docs/xt57b-fullread.md SS12.2):
//
//   EM word - frees ID11, ID12, ID13          start gap 20
//        bit 0     ON 15 + gap 16   ->  31 T0 cell      (22 of them)
//        bit 1     ON 46 + gap 19   ->  65 T0 cell      (20 of them)
//
// Its 42 bits are a 2-bit preamble 00 followed by a 40-bit value sent least-significant-bit
// first: 0x6572617365, the ASCII word "erase".  That is the whole back door - no key, no session,
// no nonce, and the chip's emulation core listens for it whatever mode it is in.  So the burst is
// generated from the word, not stored as 43 measured edge positions.
//
//   Megamos word - frees ID48, ID8E           start gap 20
//        symbol 0  ON 16 + gap 17   ->  33 T0 cell
//        symbol 1  ON 45 + gap 20   ->  65 T0 cell
//        symbol 2  ON 80 + gap 20   -> 100 T0 cell
//
// That word has three symbol lengths, does not decode to ASCII, and is still unexplained, so its
// 46 symbols are kept verbatim - but as symbols, not as 94 measured edge positions.
#define XH_EM_ERASE       0x6572617365ULL    // ASCII "erase", sent LSB first
#define XH_EM_ERASE_BITS  40
#define XH_EM_WORD_SYMS   (2 + XH_EM_ERASE_BITS)

// The Megamos word, in symbols.  The VVDI sends two different words in a reset round, and only
// the second one frees an XT27A - tested here, on an XT27A put into ID48: the other leaves the
// chip stuck.  The two differ by one gap: this word's 100 T0 cell (symbol '2' at index 1) is sent
// as a 33 and a 65 in the other, i.e. with an extra gap at +68 T0.  In both
// vvdi-id48-reset-success.pm3 and vvdi-id8E-reset-success.pm3 the burst that does not work goes
// out twice first (at 0.33 s and 1.81 s), each followed by XMICRO probes on channel B, and this
// one goes out last (3.30 s), followed by XHORSE.
//
// This one does NOT decode, unlike the 7935 word below (docs/xt57b-fullread.md SS12.4).  Its
// carrier is the EM4x70 reader link - constant gap, cells of one to three ~32 T0 periods - but
// expanded on that rule the 46 symbols give 74 bits, whose command nibble is 1100, none of the
// six EM4x70 commands, and 74 matches no EM4x70 frame length.  Kept verbatim.
static const char xh_id48_word[] = "0200100111101010000111021001112000111000201002";

// Symbol timings, T0: the field-ON time, then the gap that closes the cell.
typedef struct {
    uint8_t start_gap;
    uint8_t on[3];
    uint8_t gap[3];
} xh_dl_t;

static const xh_dl_t xh_dl_em   = { 20, { 15, 46,  0 }, { 16, 19,  0 } };
static const xh_dl_t xh_dl_id48 = { 20, { 16, 45, 80 }, { 17, 20, 20 } };
// The word that frees a chip blanked to 7935, from the VVDI's own reset of one
// (data/traces/xt57b/xhorse-XT57b-reset-from-7935.pm3; docs/xt57b-fullread.md SS12.11).  Three
// symbols again, but on a far slower grid than the other two: cells of 65, 129 and 193 T0, i.e.
// one, two or three units of 512 us, with a ~100 us gap closing each.  58 gaps, 58.3 ms end to
// end - nearly four times the "erase" burst.
//
// Unlike the other two words this one decodes, and as the emulated part's own protocol.  It is a
// PCF7931 reader downlink (armsrc/pcf7931.c AddBitPCF7931): 128 T0 bit cells carrying one gap
// each, at 34 T0 for a '1' and 98 T0 for a '0', so gap to gap gives 64, 128 or 192 - our 65, 129,
// 193.  The 57 symbols are 57 transitions across 58 bits, which is exactly PCF7931's
// "1 password-indication bit + 56-bit password + 1 programming-mode bit" and then nothing: the
// address, data and parity of a write frame are absent and the field simply stays up.  Reading
// the password LSB-first per byte as AddBytePCF7931 sends it gives 10 01 22 44 66 88 AA.  So the
// VVDI is not writing - it presents a magic password to a PCF7935, the part being emulated.
// Polarity and byte framing carry a caveat each; see SS12.11.  The firmware replays the measured
// symbols, so none of that changes what goes on air.
static const xh_dl_t xh_dl_7935 = { 13, { 53, 117, 181 }, { 12, 12, 12 } };
static const char xh_7935_word[] = "011120112011111112011201112011201210121011120112020202020";

typedef struct {
    const char    *name;
    const xh_dl_t *dl;
    const char    *word;    // NULL -> the "erase" word, generated below
    size_t         n;       // symbols
} xh_burst_t;

static const xh_burst_t xh_burst_em   = { "erase",   &xh_dl_em,   NULL,          XH_EM_WORD_SYMS };
static const xh_burst_t xh_burst_id48 = { "megamos", &xh_dl_id48, xh_id48_word,  sizeof(xh_id48_word) - 1 };
static const xh_burst_t xh_burst_7935 = { "7935",    &xh_dl_7935, xh_7935_word,  sizeof(xh_7935_word) - 1 };

// Symbol i of a burst, in the order it goes on air.
static inline uint8_t xh_burst_sym(const xh_burst_t *b, size_t i) {
    if (b->word != NULL) {
        return (uint8_t)(b->word[i] - '0');
    }
    return (i < 2) ? 0 : (uint8_t)((XH_EM_ERASE >> (i - 2)) & 1);   // preamble 00, then LSB first
}

// Burst length in T0: the start gap plus every cell.
static uint32_t xh_burst_len_t0(const xh_burst_t *b) {
    uint32_t t = b->dl->start_gap;
    for (size_t i = 0; i < b->n; i++) {
        const uint8_t s = xh_burst_sym(b, i);
        t += (uint32_t)b->dl->on[s] + b->dl->gap[s];
    }
    return t;
}

// burst length in us (EM 2002 T0 = 16.0 ms, Megamos 2449 T0 = 19.6 ms)
static uint32_t xh_burst_len_us(const xh_burst_t *b) {
    return xh_burst_len_t0(b) * 8;
}

// Emit a burst.  The field is on when this is called and on when it returns.  Edge positions are
// accumulated against one precision-counter reference, so they do not drift pulse to pulse.
// Rebase the reference once the accumulated target passes XH_BURST_REBASE T0, so a burst longer
// than the 16-bit counter's 5461 T0 still has every edge placed against one running clock.  The
// 7935 word is 7290 T0; the other two fit without this ever firing.
#define XH_BURST_REBASE  4000
static inline void xh_burst_wait(uint16_t *ref, uint32_t *t) {
    while (GetPrecisionCounterDelta(*ref) < (uint16_t)(T0 * (*t))) {};
    if (*t > XH_BURST_REBASE) {
        *ref = (uint16_t)(*ref + (uint16_t)(T0 * XH_BURST_REBASE));
        *t -= XH_BURST_REBASE;
    }
}

static void xh_em_burst(const xh_burst_t *b) {
    const xh_dl_t *dl = b->dl;
    uint16_t ref = GetPrecisionCounterRaw();
    uint32_t t = dl->start_gap;

    Gpio_SSC_DOUT_High();                                            // field off: the start gap
    for (size_t i = 0; i < b->n; i++) {
        const uint8_t s = xh_burst_sym(b, i);
        xh_burst_wait(&ref, &t);
        Gpio_SSC_DOUT_Low();                                         // field on: carries the symbol
        t += dl->on[s];
        xh_burst_wait(&ref, &t);
        Gpio_SSC_DOUT_High();                                        // field off: closes the cell
        t += dl->gap[s];
    }
    while (GetPrecisionCounterDelta(ref) < (uint16_t)(T0 * t)) {};
    Gpio_SSC_DOUT_Low();                                             // field back on
}

// The EM burst on its own, for autel.c (armsrc/xhorse.h).
void Xhorse_EmBurst(void) {
    xh_em_burst(&xh_burst_em);
}

uint32_t Xhorse_EmBurstLenUs(void) {
    return xh_burst_len_us(&xh_burst_em);
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
        // first attempt: the nominal offsets only, which is the common case and keeps it quick.
        // after that, slip the burst through a whole frame.  Once the chip is free the extra
        // bursts are harmless - the VVDI keeps bursting after the chip goes silent too.
        const size_t nsweep = (attempt == 0) ? 1 : XH_EM_SWEEP_N;

        for (size_t k = 0; k < nsweep; k++) {
            const uint32_t slip = (uint32_t)k * XH_EM_SWEEP_STEP_US;

            for (size_t t = 0; t < sizeof(xh_em_timings) / sizeof(xh_em_timings[0]); t++) {
                const xh_em_timing_t *tm = &xh_em_timings[t];
                const uint32_t at = tm->burst_at_us + slip;

                Gpio_SSC_DOUT_High();                                // off: the burst is timed from a fresh power-up
                xh_delay_us(XH_T_EM_OFF_US);
                Gpio_SSC_DOUT_Low();                                 // field on: chip boots, broadcasts EM
                xh_delay_us(at);
                xh_em_burst(tm->burst);
                xh_delay_us(tm->on_us - at - xh_burst_len_us(tm->burst));
            }
        }

        Gpio_SSC_DOUT_High();                                        // cold-boot
        xh_delay_us(XH_T_EM_COLDBOOT_US);
        Gpio_SSC_DOUT_Low();
        xh_delay_us(XH_T_EM_SETTLE_US);

        if (xh_handshake(s) == PM3_SUCCESS && xh_4r(s, "EM-recovery", attempt)) {
            s->em_mode = true;                                       // later sessions: 70 ms off / 30.18 ms settle
            return PM3_SUCCESS;
        }
        if (g_dbglevel >= DBG_DEBUG) {
            Dbprintf("xhorse: EM-recovery attempt %d: no session after the bursts and cold-boot", attempt);
        }
    }
    return PM3_EFAILED;
}

// Open a session, trying the normal XHORSE handshake first and falling back to the EM burst
// for a chip stuck emulating a non-HITAG2 type (ID11/EM4100 etc).
// Returns PM3_ENOTIMPL, with the session closed, for an XT136: it has no write recipes yet, and the
// EM bursts are only known to be safe on an XT27A.
static int xh_open_any(xh_sess_t *s) {
    if (xh_open_session(s) == PM3_SUCCESS) {
        return PM3_SUCCESS;
    }
    if (xh_open_session_xt136(s) == PM3_SUCCESS) {
        xh_ctl(s, XH_PG_PAD);
        return PM3_ENOTIMPL;
    }
    if (g_dbglevel >= DBG_INFO) {
        Dbprintf("xhorse: no XHORSE session; sending EM-recovery burst (chip stuck in non-HITAG2 emulation?)");
    }
    return xh_open_session_em(s);
}

// Take the probe choice from a client packet.  A XHORSE_PROBE_CUSTOM word must be exactly 98
// 'S'/'L' symbols; anything else is refused, because a half-copied word is a different command.
static uint8_t xh_probe_from(const lf_xhorse_rdopt_t *opt) {
    if (opt == NULL) {
        return XHORSE_PROBE_NONE;
    }
    if (opt->probe != XHORSE_PROBE_CUSTOM) {
        return (opt->probe > XHORSE_PROBE_CUSTOM) ? XHORSE_PROBE_NONE : opt->probe;
    }
    for (int i = 0; i < XHORSE_ID8A_SYMS; i++) {
        if (opt->word[i] != 'S' && opt->word[i] != 'L') {
            Dbprintf("xhorse: bad custom probe word at symbol %d; not sending a probe", i);
            return XHORSE_PROBE_NONE;
        }
        xh_id8a_probe_custom[i] = opt->word[i];
    }
    xh_id8a_probe_custom[XHORSE_ID8A_SYMS] = 0;
    if (g_dbglevel >= DBG_INFO) {
        Dbprintf("xhorse: custom ID8A probe word: %s", xh_id8a_probe_custom);
    }
    return XHORSE_PROBE_CUSTOM;
}

// probe is a xhorse_probe_t: XHORSE_PROBE_NONE for an ordinary read, or a word to let
// xh_open_session fall through to its fourth timing for a chip set to ID8A.
static int xh_begin(xh_sess_t *s, uint8_t probe, bool ledcontrol) {
    xh_setup(s, ledcontrol);
    s->probe = probe;
    return xh_open_session(s);
}

// Like xh_begin, but also handles a chip stuck in a non-HITAG2 emulation (reset/configure).
static int xh_begin_any(xh_sess_t *s, bool ledcontrol) {
    xh_setup(s, ledcontrol);
    s->probe = XHORSE_PROBE_B;  // the chip is about to be rewritten anyway
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
// xh_bitrev8(), xh_pack_em8() and xh_id13_stream() are shared with the client (include/xhorse.h):
// the XT57B set command builds the same images for its XMICRO sessions.

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

//-----------------------------------------------------------------------------
// Per-type page recipes.  Each returns PM3_SUCCESS, or PM3_EFAILED at the first failed sel or verify.
//-----------------------------------------------------------------------------
// Stop at the first failure: after a missed sel or frame, the chip's bank or keystream may no
// longer match ours, and writing on would put data in the wrong place.
#define XH_WR(pg, val) do { if (xh_write(s, (pg), (val), verify) != PM3_SUCCESS) return PM3_EFAILED; } while (0)
#define XH_BANK(b) do { if (xh_bank(s, (b)) != PM3_SUCCESS) return PM3_EFAILED; } while (0)
#define XH_CFG(n, val) do { if (xh_cfg_write(s, (n), (val), verify) != PM3_SUCCESS) return PM3_EFAILED; } while (0)

static int xh_recipe(xh_sess_t *s, const lf_xhorse_data_t *p) {
    bool verify = p->verify;
    uint32_t uid32 = (p->uidlen >= 4) ? xh_be32(p->uid) : 0;

    // Type byte first (bank 1), as every decoded type except ID8A does (its recipe writes it last).
    if (p->has_type) {
        XH_BANK(1);
        XH_WR(XH_PG_TYPE, p->typeword ? 0x0080008CUL : (uint32_t)p->typebyte);
    }

    switch ((xhorse_recipe_t)p->recipe) {

        case XHORSE_R_HITAG2:       // ID46/7936/7937/7946
            XH_BANK(0);
            XH_WR(0x00, uid32);
            XH_WR(0x01, 0x4D494B52);   // "MIKR"
            XH_WR(0x02, 0x00004F4E);   // "..ON"
            XH_WR(0x03, 0x06AA4854);   // config
            break;

        case XHORSE_R_7935:         // PCF7935: full bank-0 wipe, 0x1F = 0x07
            XH_BANK(0);
            for (uint8_t pg = 0x00; pg <= 0x3F; pg++) {
                XH_WR(pg, (pg == 0x1F) ? 0x00000007UL : 0x00000000UL);
            }
            // Then clear bank-1 0x00..0x0F and set config words 1 and 0, as in both complete VVDI
            // captures (set2-id7935, 013_set_7935); set-id7935 was cut off before this point.
            XH_BANK(1);
            for (uint8_t pg = 0x00; pg <= 0x0F; pg++) {
                XH_WR(pg, 0x00000000UL);
            }
            XH_CFG(1, 0xF370CA18);   // 0x0370CA18 on another chip; where the top nibble comes from is unknown
            {
                uint32_t v3d = 0;
                if (xh_read_agreed(s, XH_PG_CFG0, &v3d) == false) {
                    return PM3_EFAILED;
                }
                XH_CFG(0, v3d | 0x200);   // captures: 0x409 -> 0x609, 0x0002021F kept
            }
            break;

        case XHORSE_R_7938:         // ID47: distinct 5-page image + clear bank-1 0x38..0x3B
            XH_BANK(0);
            XH_WR(0x00, uid32);
            XH_WR(0x01, 0x11112222); // Key 1
            XH_WR(0x02, 0x33334444); // Key 2
            XH_WR(0x03, 0x55556666); // Key 3
            XH_WR(0x04, 0x00FFFFFF); // Config
            XH_BANK(1);
            for (uint8_t pg = 0x38; pg <= 0x3B; pg++) {
                XH_WR(pg, 0x00000000UL);
            }
            break;

        case XHORSE_R_7939:         // ID49: UID at bank-0 0x00, rest in bank 1
            XH_BANK(0);
            XH_WR(0x00, uid32);
            XH_BANK(1);
            XH_WR(0x00, 0xF7F8251D);  // The Xhorse app seem to write a random value here
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
            XH_BANK(0);
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
            XH_BANK(0);
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
            XH_BANK(0);
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
            XH_BANK(0);
            XH_WR(0x02, (uint32_t)((s8[0] << 8) | s8[1]));
            XH_WR(0x01, (uint32_t)((s8[2] << 8) | s8[3]));
            XH_WR(0x04, (uint32_t)((s8[4] << 8) | s8[5]));
            XH_WR(0x03, (uint32_t)((s8[6] << 8) | s8[7]));
            XH_WR(0x00, 0x00000018);
            break;
        }

        case XHORSE_R_TEMPLATE:     // ID4D / 4D80 / ID4E - config-template family
            XH_BANK(0);
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
            XH_BANK(1);
            XH_WR(0x13, 0x00004600);
            break;

        case XHORSE_R_ID8A: {       // full image, type byte last; UID at 0x04/0x05
            // 0x04 = uid[1]<<8 | uid[0] ; 0x05 = uid[3]<<8 | uid[2]  (byte-swapped halves)
            uint32_t p04 = ((uint32_t)p->uid[1] << 8) | p->uid[0];
            uint32_t p05 = ((uint32_t)p->uid[3] << 8) | p->uid[2];
            XH_BANK(0);
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
            XH_BANK(1);
            for (uint8_t pg = 0x00; pg <= 0x1C; pg++) {
                XH_WR(pg, 0x00000000UL);
            }
            XH_WR(XH_PG_TYPE, 0x0000008A);
            // VVDI's closing step (vvdi-id8A-set-success.pm3): re-sel bank 1, read 0x3D
            // (0x0002021F), send it back with ctl 0x30, read 0x3D again.  ctl 0x30 writes config
            // word 0 (xh_cfg_write); why the VVDI rewrites the same value is unknown.
            {
                uint32_t v3d = 0;
                if (xh_sel(s, 1) != PM3_SUCCESS) {
                    return PM3_EFAILED;
                }
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
            XH_BANK(0);
            XH_WR(0x0A, (uint32_t)((p->uid[2] << 8) | p->uid[3]));   // low half
            XH_WR(0x0B, (uint32_t)((p->uid[0] << 8) | p->uid[1]));   // high half
            break;

        default:
            return PM3_EINVARG;
    }
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// Command handlers (called from appmain.c)
//-----------------------------------------------------------------------------
// Probe the XMICRO / "channel B" identify (XT57 / XT37-class super-chips).  The XT27A never
// answers XMICRO as itself, so a family name here means a newer family we do not yet support.  Sequence, as
// the VVDI reads an XT27B / XT57B:
//   field on, wait 120.14 ms
//   reader -> XMICRO (48b)          tag -> 0x5AA5 ACK
//   reader -> 0b0011 (4b identify)  tag -> ASCII family string, e.g. "XT37"
// On success fills type[] with the ASCII the chip reported and returns PM3_SUCCESS.
// CRC-8 of the channel-B frame format: poly 0x07, init 0, no reflection, no final xor, over the
// on-air bytes including the reader command that provoked the reply (docs/xt57-auth.md SS7.1).
static uint8_t xh_crc8(const uint8_t *d, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
        }
    }
    return c;
}

// The identify reply is four family bytes plus that CRC, taken over the 0011 command byte and the
// family (docs/xt57-auth.md SS4): "XT57" -> 0x58, which is why it was long read as the string
// "XT57X"; "XT37" -> 0x26.  Checking it is what makes the family bytes safe to build a select from.
static bool xh_ident_crc_ok(const uint8_t *rx, size_t rxlen) {
    if (rxlen < 40) {
        return false;                        // truncated: no CRC byte to check
    }
    const uint8_t over[5] = { 0x03, rx[0], rx[1], rx[2], rx[3] };
    return xh_crc8(over, sizeof(over)) == rx[4];
}

static int xh_xmicro_identify_off(xh_sess_t *s, uint32_t off_us,
                                  uint8_t *type, size_t typesz, size_t *tlen);

// The normal probe: a short field-off, which is all a chip that is not emulating needs.
static int xh_xmicro_identify(xh_sess_t *s, uint8_t *type, size_t typesz, size_t *tlen) {
    return xh_xmicro_identify_off(s, XH_T_FIELD_OFF_US, type, typesz, tlen);
}

static int xh_xmicro_identify_off(xh_sess_t *s, uint32_t off_us,
                                  uint8_t *type, size_t typesz, size_t *tlen) {
    static const uint8_t xmicro[6] = { 'X', 'M', 'I', 'C', 'R', 'O' };
    static const uint8_t query[1]  = { 0x30 };   // 4 bits, 0b0011 (MSB-aligned)
    uint8_t rx[32];
    size_t rxlen;

    *tlen = 0;

    // Fresh power-up, then give the chip the VVDI's 120 ms before probing (XH_T_XMICRO_POWERUP_US).
    Gpio_SSC_DOUT_High();
    xh_delay_us(off_us);
    Gpio_SSC_DOUT_Low();
    xh_delay_us(XH_T_XMICRO_POWERUP_US);

    // 1) XMICRO probe -> expect the 0x5AA5 ACK (XT27A gives nothing here).
    rxlen = xh_xfer_clear(s, xmicro, 48, rx, sizeof(rx), 0);
    if (g_dbglevel >= DBG_EXTENDED) {
        Dbprintf("xhorse: XMICRO reply %u bits: %02x %02x %02x %02x", (unsigned)rxlen, rx[0], rx[1], rx[2], rx[3]);
    }
    if (rxlen < 16) {
        return PM3_EFAILED;                      // no B-channel response -> not an XT57/XT37-class chip
    }

    // 2) 4-bit identify query -> the chip answers with its ASCII family string.
    xh_delay_us(XH_T_XMICRO_ID_US);
    rxlen = xh_xfer_clear(s, query, 4, rx, sizeof(rx), 0);
    if (g_dbglevel >= DBG_EXTENDED) {
        Dbprintf("xhorse: identify reply %u bits: %02x %02x %02x %02x %02x", (unsigned)rxlen, rx[0], rx[1], rx[2], rx[3], rx[4]);
    }

    // Only a family name ("XT37", "XT57", ...) makes it a B-channel chip.  An XT27A emulating some
    // types (7938 seen) answers XMICRO with something but says nothing to the identify query; it
    // is still an XT27A, so let the caller go on to the channel-A handshake.
    if ((rxlen < 32) || (rx[0] != 'X') || (rx[1] != 'T')) {
        return PM3_EFAILED;
    }
    // A full 40-bit reply carries the CRC: check it rather than trust two ASCII bytes, so a
    // mis-demodulated family cannot pass as a real one.  A 32-bit reply has no CRC to check and is
    // still accepted - the XT57B traces show the 5th byte dropping out on weak coupling.
    if ((rxlen >= 40) && (xh_ident_crc_ok(rx, rxlen) == false)) {
        if (g_dbglevel >= DBG_ERROR) {
            Dbprintf("xhorse: identify CRC bad: %02x %02x %02x %02x %02x (want %02x)",
                     rx[0], rx[1], rx[2], rx[3], rx[4],
                     xh_crc8((uint8_t[5]) { 0x03, rx[0], rx[1], rx[2], rx[3] }, 5));
        }
        return PM3_EFAILED;
    }
    size_t n = rxlen / 8;
    if (n > typesz) n = typesz;
    memcpy(type, rx, n);
    *tlen = n;
    return PM3_SUCCESS;                          // named its family: it is a B-channel chip
}

// Read pages 0..pages-1 of the current bank into out->image, each until two reads agree
// (xh_read_agreed), and mark the pages that did in out->page_ok.
static void xh_read_image(xh_sess_t *s, lf_xhorse_info_t *out, uint8_t pages) {
    out->image_pages = pages;
    for (uint8_t pg = 0; pg < pages; pg++) {
        uint32_t w = 0;
        if (xh_read_agreed(s, pg, &w)) {
            out->page_ok[pg / 8] |= (uint8_t)(1 << (pg % 8));
        }
        out->image[pg * 4 + 0] = (uint8_t)(w >> 24);
        out->image[pg * 4 + 1] = (uint8_t)(w >> 16);
        out->image[pg * 4 + 2] = (uint8_t)(w >> 8);
        out->image[pg * 4 + 3] = (uint8_t)(w);
    }
}

// Free an XT57B stuck in a broadcast emulation mode.
//
// Configured as one of the talk-first types the XT57B broadcasts continuously and stops answering
// XMICRO, so none of the channel-B commands reach it.  The same "erase" downlink word that frees
// an XT27A frees it - the emulation core listens for it whatever family the chip is - but the
// XT57B captures wrap it in a different choreography.  Measured from
// xhorse-XT57b-reset-from-id11.pm3, -id12 and -id13, identical in all three to the millisecond
// (docs/xt57b-fullread.md SS12.1):
//
//   2 rounds of:
//        4 x [ field off 70 ms, on, burst at +109.7 ms, window ends at +156 ms ]
//        field off 568 ms
//        field on, XMICRO probe             <- answers once the chip is free
//        field off 228 ms
//
// NOTE: this frees the chip, it does not rewrite its configuration.  The VVDI follows the burst
// with 11 encrypted page writes (session nonce 6DCD7463, keystream not recovered - SS12.5) which
// are what make its reset permanent.  Until those are decrypted this restores the channel, not
// the factory personality.
#define XT57_EM_OFF_US         70050    // field off between bursts inside a round
#define XT57_EM_AT_US         109700    // burst position in the window, from the field-on edge
#define XT57_EM_WINDOW_US     156000    // field-on window holding one burst
#define XT57_EM_ROUND_OFF_US  568300    // drop at the end of a round, before the probe
#define XT57_EM_PROBE_OFF_US  228100    // drop after a failed probe, before the next round
#define XT57_EM_BURSTS             4    // bursts per round
#define XT57_EM_ROUNDS             2

// Phase sweep, for when the VVDI's own offset does not free this particular chip - which is the
// common case here, not the rare one: an XT57B set to ID11 is not freed by XT57_EM_AT_US at all,
// on any retry.  The burst has to land at the right point in the chip's broadcast frame (ID11 is
// RF/32, so one frame is 32 T0 = 256 us), and the VVDI's fixed offset was measured against the
// chip it was used on, through its antenna.  So after the VVDI's sequence has been tried, walk
// the burst outwards from that offset in 5 T0 steps and probe every few positions.
#define XT57_EM_SWEEP_N           32    // positions, centred on XT57_EM_AT_US: +-16 steps
#define XT57_EM_SWEEP_STEP_US     40    // 5 T0 per step; +-640 us covers five ID11 frames
#define XT57_EM_PROBE_EVERY        8    // probe XMICRO after this many swept bursts

// The 7935 word's own geometry, read off the VVDI's reset of a blanked chip: field off 120.06 ms,
// on, burst at 18.76 ms, field held on to 107.82 ms.  The chip broadcasts from about 26 to 78 ms
// after power-up, so the burst goes out over the broadcast, not before or after it.  One burst at
// the VVDI's offset frees the chip, and it stays free across later power cycles.  Its frame is
// 4096 us, so the sweep steps 128 us: 32 positions walk one whole frame.
#define XT57_7935_OFF_US      120060
#define XT57_7935_AT_US        18760
#define XT57_7935_WINDOW_US   107820
#define XT57_7935_STEP_US        128

// One burst in its own power cycle, so it is timed from a fresh power-up, inside the VVDI's
// 156 ms field-on window (107.8 ms for the 7935 word).
static void xt57_em_burst_at(uint32_t at_us, const xh_burst_t *b, uint32_t on_us, uint32_t off_us) {
    const uint32_t blen = xh_burst_len_us(b);
    Gpio_SSC_DOUT_High();                                        // off: the burst is timed from power-up
    xh_delay_us(off_us);
    Gpio_SSC_DOUT_Low();                                         // on: chip boots, broadcasts
    xh_delay_us(at_us);
    xh_em_burst(b);
    // the tail of the window, if the burst has not already run past its end
    if (on_us > at_us + blen) {
        xh_delay_us(on_us - at_us - blen);
    }
}

// One round: XT57_EM_BURSTS bursts at the same offset, then the long drop that ends the round.
static void xt57_em_round(uint32_t at_us, const xh_burst_t *b, uint32_t on_us, uint32_t off_us) {
    for (int i = 0; i < XT57_EM_BURSTS; i++) {
        xt57_em_burst_at(at_us, b, on_us, off_us);
    }
    Gpio_SSC_DOUT_High();
    xh_delay_us(XT57_EM_ROUND_OFF_US);
    Gpio_SSC_DOUT_Low();
}

// Which words to send, and the geometry each one wants.  XT57_WORD_AUTO tries the EM word and
// then the 7935 word, because the chip cannot be asked what it is emulating while it is stuck.
typedef struct {
    const xh_burst_t *burst;
    uint32_t at_us;
    uint32_t on_us;
    uint32_t off_us;
    int32_t  step_us;
} xt57_word_t;

static const xt57_word_t xt57_words[] = {
    { &xh_burst_em,   XT57_EM_AT_US,   XT57_EM_WINDOW_US,   XT57_EM_OFF_US,   XT57_EM_SWEEP_STEP_US },
    { &xh_burst_id48, XT57_EM_AT_US,   XT57_EM_WINDOW_US,   XT57_EM_OFF_US,   XT57_EM_SWEEP_STEP_US },
    { &xh_burst_7935, XT57_7935_AT_US, XT57_7935_WINDOW_US, XT57_7935_OFF_US, XT57_7935_STEP_US },
};

void Xhorse_Xt57Reset(const lf_xhorse_xt57_reset_t *q, bool ledcontrol) {
    const bool force = (q->force != 0);
    const int nsweep = q->nsweep ? q->nsweep : XT57_EM_SWEEP_N;
    const int probe_every = q->probe_every ? q->probe_every : XT57_EM_PROBE_EVERY;
    // word: 0 = auto (erase, then 7935), 1 = erase, 2 = megamos, 3 = 7935
    const int wfirst = (q->word == 0) ? 0 : (int)q->word - 1;
    const int wlast  = (q->word == 0) ? 2 : (int)q->word - 1;
    xh_sess_t s;
    lf_xhorse_info_t out;
    memset(&out, 0, sizeof(out));

    xh_setup(&s, ledcontrol);

    // If it already answers XMICRO it is not stuck, and there is nothing for the burst to do.
    // `force` sends the bursts anyway, which is how the sequence gets exercised on a healthy chip.
    size_t tlen = 0;
    if ((force == false) && xh_xmicro_identify_off(&s, XH_T_FIELD_OFF_LONG_US, out.bchip_type, sizeof(out.bchip_type), &tlen) == PM3_SUCCESS) {
        out.bchip = 1;
        out.bchip_type_len = (uint8_t)tlen;
        out.found = 1;                                           // already reachable
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_XHORSE_XT57_RESET, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
        return;
    }

    for (int wi = wfirst; wi <= wlast; wi++) {
        if ((q->word == 0) && (wi == 1)) {
            continue;                                            // auto: the EM word, then 7935
        }
        const xt57_word_t *w = &xt57_words[wi];
        const xh_burst_t *word = w->burst;
        const uint32_t at_us = q->at_us ? (uint32_t)q->at_us : w->at_us;
        const uint32_t on_us = q->on_us ? (uint32_t)q->on_us : w->on_us;
        const uint32_t off_us = w->off_us;
        const int32_t step_us = q->step_us ? q->step_us : w->step_us;

        // Pass 1: the VVDI's own sequence, unchanged.
        for (int round = 0; round < XT57_EM_ROUNDS; round++) {
            xt57_em_round(at_us, word, on_us, off_us);

            // xh_xmicro_identify_off power-cycles and waits XH_T_XMICRO_POWERUP_US itself.  The
            // long off is the point of this path: an emulating chip needs ~720 ms down before it
            // listens again (docs/xt37.md SS5t), and the bursts are secondary.
            if (xh_xmicro_identify_off(&s, XH_T_FIELD_OFF_LONG_US, out.bchip_type,
                                       sizeof(out.bchip_type), &tlen) == PM3_SUCCESS) {
                out.bchip = 1;
                out.bchip_type_len = (uint8_t)tlen;
                hitag_cleanup(ledcontrol);
                reply_ng(CMD_LF_XHORSE_XT57_RESET, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
                return;
            }
            if (g_dbglevel >= DBG_DEBUG) {
                Dbprintf("xhorse: XT57B round %d: no XMICRO answer after the bursts", round);
            }

            Gpio_SSC_DOUT_High();
            xh_delay_us(XT57_EM_PROBE_OFF_US);
            Gpio_SSC_DOUT_Low();
        }

        // Pass 2: walk the burst outwards from the VVDI's offset - 0, -1, +1, -2, +2, ... - and probe
        // every XT57_EM_PROBE_EVERY positions.  A burst at the wrong position is harmless once the
        // chip is free, so the probe does not have to follow every one.
        int32_t last_slip = 0;
        for (int k = 0; k < nsweep; k++) {
            const int step = (k & 1) ? -((k + 1) / 2) : (k / 2);
            last_slip = step * step_us;
            {
                const int32_t a = (int32_t)at_us + last_slip;
                xt57_em_burst_at((uint32_t)((a < 0) ? 0 : a), word, on_us, off_us);
            }

            if (((k + 1) % probe_every) != 0) {
                continue;
            }
            Gpio_SSC_DOUT_High();
            xh_delay_us(XT57_EM_ROUND_OFF_US);
            Gpio_SSC_DOUT_Low();

            if (xh_xmicro_identify_off(&s, XH_T_FIELD_OFF_LONG_US, out.bchip_type,
                                       sizeof(out.bchip_type), &tlen) == PM3_SUCCESS) {
                out.bchip = 1;
                out.bchip_type_len = (uint8_t)tlen;
                // the winning burst is somewhere in the block just sent; report the last of them,
                // which bounds the offset to within XT57_EM_PROBE_EVERY steps
                out.em_slip_us = (int16_t)((last_slip > 32767) ? 32767
                                           : (last_slip < -32768) ? -32768 : last_slip);
                hitag_cleanup(ledcontrol);
                reply_ng(CMD_LF_XHORSE_XT57_RESET, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
                return;
            }
            if (g_dbglevel >= DBG_DEBUG) {
                Dbprintf("xhorse: XT57B sweep through %d: no XMICRO answer", k);
            }

            Gpio_SSC_DOUT_High();
            xh_delay_us(XT57_EM_PROBE_OFF_US);
            Gpio_SSC_DOUT_Low();
        }

        if (BUTTON_PRESS() || data_available()) {
            break;
        }
    }   // words, when none was named

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_XHORSE_XT57_RESET, PM3_EFAILED, (uint8_t *)&out, sizeof(out));
}

void Xhorse_Info(const lf_xhorse_rdopt_t *opt, bool ledcontrol) {
    xh_sess_t s;
    lf_xhorse_info_t out;
    memset(&out, 0, sizeof(out));

    xh_setup(&s, ledcontrol);
    s.probe = xh_probe_from(opt);   // NONE unless the caller asked for an ID8A probe word

    // Try the XMICRO / channel-B identify first.  A family name means a newer super-chip family
    // (XT57 / XT37-class) that this command does not yet support - report it and stop.
    size_t tlen = 0;
    // A chip emulating a deaf type answers only after a long power-down, so if the quick probe finds
    // nothing, spend the 760 ms once before concluding there is no channel-B chip (docs/xt37.md SS5t).
    if (xh_xmicro_identify(&s, out.bchip_type, sizeof(out.bchip_type), &tlen) != PM3_SUCCESS) {
        xh_xmicro_identify_off(&s, XH_T_FIELD_OFF_LONG_US, out.bchip_type,
                               sizeof(out.bchip_type), &tlen);
    }
    if (tlen > 0) {
        out.bchip = 1;
        out.bchip_type_len = (uint8_t)tlen;
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_XHORSE_INFO, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
        return;
    }

    // Otherwise fall back to the channel-A handshakes + reads: XT27A first, as the VVDI does, then XT136.
    if (xh_open_session(&s) != PM3_SUCCESS) {
        if (xh_open_session_xt136(&s) == PM3_SUCCESS) {
            out.found = 1;
            out.family = XHORSE_FAM_XT136;
            out.seed_reply = s.seed_reply;
            xh_read_image(&s, &out, XHORSE_XT136_PAGES);     // flat page space, no sel
            xh_ctl(&s, XH_PG_PAD);
        }
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_XHORSE_INFO, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
        return;
    }
    out.found = 1;
    out.family = XHORSE_FAM_XT27A;
    out.seed_reply = s.seed_reply;

    xh_read_chipid(&s, out.chipid);

    uint32_t tb = 0;
    if (xh_bank(&s, 1) == PM3_SUCCESS) {
        out.typebyte_ok = xh_read_agreed(&s, XH_PG_TYPE, &tb) ? 1 : 0;
        // The config words come from the same bank, so read them here rather than in a session of
        // their own: the header `info` and `dump` share reports them.
        uint32_t c0 = 0, c1 = 0;
        bool ok0 = xh_read_agreed(&s, XH_PG_CFG0, &c0);
        bool ok1 = xh_read_agreed(&s, XH_PG_CFG0 + 1, &c1);
        out.cfgword[0] = c0;
        out.cfgword[1] = c1;
        out.cfgword_ok = (uint8_t)((ok0 ? 1 : 0) | (ok1 ? 2 : 0));
    }
    out.typebyte = (uint8_t)(tb & 0xFF);

    // If the sel fails the bank is unknown: leave the pages zero and unconfirmed.
    if (xh_bank(&s, 0) == PM3_SUCCESS) {
        xh_read_image(&s, &out, 8);
    } else {
        out.image_pages = 8;
    }
    xh_ctl(&s, XH_PG_PAD);

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_XHORSE_INFO, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
}

// Dumps one bank (0 = emulated image, 1 = config/identity) per call: both would not fit in one reply.
// An XT136 has no banks: bank 0 returns its pages 0x00..0x0F, and a bank-1 request only reports the
// family.
void Xhorse_Dump(uint8_t bank, const lf_xhorse_rdopt_t *opt, bool ledcontrol) {
    xh_sess_t s;
    lf_xhorse_info_t out;
    memset(&out, 0, sizeof(out));
    out.bank = bank;

    if (xh_begin(&s, xh_probe_from(opt), ledcontrol) != PM3_SUCCESS) {
        if (xh_open_session_xt136(&s) == PM3_SUCCESS) {
            out.found = 1;
            out.family = XHORSE_FAM_XT136;
            if (bank == 0) {
                xh_read_image(&s, &out, XHORSE_XT136_PAGES);
            }
            xh_ctl(&s, XH_PG_PAD);
        }
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_XHORSE_DUMP, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
        return;
    }
    out.found = 1;
    out.family = XHORSE_FAM_XT27A;

    xh_read_chipid(&s, out.chipid);

    uint32_t tb = 0;
    if (xh_bank(&s, 1) == PM3_SUCCESS) {
        out.typebyte_ok = xh_read_agreed(&s, XH_PG_TYPE, &tb) ? 1 : 0;
        // The config words come from the same bank, so read them here rather than in a session of
        // their own: the header `info` and `dump` share reports them.
        uint32_t c0 = 0, c1 = 0;
        bool ok0 = xh_read_agreed(&s, XH_PG_CFG0, &c0);
        bool ok1 = xh_read_agreed(&s, XH_PG_CFG0 + 1, &c1);
        out.cfgword[0] = c0;
        out.cfgword[1] = c1;
        out.cfgword_ok = (uint8_t)((ok0 ? 1 : 0) | (ok1 ? 2 : 0));
    }
    out.typebyte = (uint8_t)(tb & 0xFF);

    // If the sel fails the bank is unknown: leave the pages zero and unconfirmed.
    if (xh_bank(&s, bank) == PM3_SUCCESS) {
        xh_read_image(&s, &out, 64);
    } else {
        out.image_pages = 64;
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
// XT136 factory reset.  The VVDI does it in three sessions (docs/xt136.md SS4.2 and SS5a, decoded
// from vvdi-xt136-reset.pm3): an identify session (ctl 0x3F), one that writes the three config
// words, and one that writes pages 0x00..0x0C.  Every value below is the VVDI's, and a chip reset
// by the app reads back exactly this image.  Unlike the XT27A there is no type-byte/UID choice:
// the image is fixed, with ID46/7936 in config word 1's high byte.
static const uint32_t xh136_reset_cfg[3] = {
    0x0000000EUL,   // word 0 -> page 0x0D
    0x361818CAUL,   // word 1 -> page 0x0E; high byte 0x36 = ID46/7936, the emulated type
    0x56781234UL,   // word 2 -> page 0x0F
};

static const uint32_t xh136_reset_img[13] = {
    0x64B55D12UL,   // 0x00  emulated HITAG2 UID
    0x4D494B52UL,   // 0x01  "MIKR"
    0x00004F4EUL,   // 0x02  "..ON"
    0x06AA4854UL,   // 0x03  HITAG2 config page
    0x00000000UL,   // 0x04  HITAG2 user pages
    0x00000000UL,   // 0x05
    0x00000000UL,   // 0x06
    0x00000000UL,   // 0x07
    0x00000018UL,   // 0x08  emulation parameters (meaning unknown)
    0x00282010UL,   // 0x09
    0x00004141UL,   // 0x0A
    0x28148389UL,   // 0x0B
    0x55555555UL,   // 0x0C
};

// Runs on an open XT136 session; opens the second one itself.  Config words first, as the VVDI
// does - a half-done reset then still leaves a chip whose config says ID46.
static int xh_reset_xt136(xh_sess_t *s, bool verify) {
    for (uint8_t n = 0; n < ARRAYLEN(xh136_reset_cfg); n++) {
        if (xh_cfg_write(s, n, xh136_reset_cfg[n], verify) != PM3_SUCCESS) {
            return PM3_EFAILED;
        }
    }

    // The VVDI starts a fresh session before the image; keep the split rather than carrying one
    // session across both, since that is the only sequence seen to work.
    xh_ctl(s, XH_PG_PAD);
    if (xh_open_session_xt136(s) != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_ERROR) {
            Dbprintf("xhorse: XT136 reset, no session for the image write");
        }
        return PM3_EFAILED;
    }

    for (uint8_t pg = 0; pg < ARRAYLEN(xh136_reset_img); pg++) {
        if (xh_write(s, pg, xh136_reset_img[pg], verify) != PM3_SUCCESS) {
            return PM3_EFAILED;
        }
    }
    return PM3_SUCCESS;
}

// XT136 configure: the VVDI's "set as ID46" / "set as 7946" (docs/xt136.md SS5a).  Those are the
// only two types its own app offers for the family, and both take one recipe - the type byte into
// config word 1's high byte, then the HITAG2 image.
//
// Captured on the wire in both directions (vvdi-xt136-set-7936-to-7946.pm3 and its reverse):
//
//     READ 0x0E -> 361818CA ;  ctl 0x31 <- 461818CA ;  READ 0x0E -> 461818CA
//     WRITE 0x00 <- UID ; READ 0x00 ; WRITE 0x01..0x03 ; READ each
//
// all in one session after the identify.  Only word 1's high byte changes, so the low three bytes
// are preserved by read-modify-write rather than assumed.  The VVDI writes no other page: not
// 0x0C, not 0x0D, not 0x04..0x0B.
static int xh_configure_xt136(xh_sess_t *s, const lf_xhorse_data_t *p) {
    bool verify = (p->verify != 0);

    uint32_t w1 = 0;
    if (xh_read_agreed(s, XH136_PG_CFG0 + 1, &w1) == false) {
        return PM3_EFAILED;
    }
    w1 = (w1 & 0x00FFFFFFUL) | ((uint32_t)p->typebyte << 24);
    if (xh_cfg_write(s, 1, w1, verify) != PM3_SUCCESS) {
        return PM3_EFAILED;
    }

    // The image follows in the same session - unlike a reset, which does split (xh_reset_xt136).
    uint32_t uid = ((uint32_t)p->uid[0] << 24) | ((uint32_t)p->uid[1] << 16) |
                   ((uint32_t)p->uid[2] << 8) | (uint32_t)p->uid[3];
    if (xh_write(s, 0x00, uid, verify) != PM3_SUCCESS) {
        return PM3_EFAILED;
    }
    for (uint8_t pg = 0x01; pg <= 0x03; pg++) {
        if (xh_write(s, pg, xh136_reset_img[pg], verify) != PM3_SUCCESS) {
            return PM3_EFAILED;
        }
    }
    return PM3_SUCCESS;
}

// Types an XT136 can be set to: ID46/7936 and 7946, the only two its app offers.
static bool xh136_type_ok(const lf_xhorse_data_t *p) {
    return (p->recipe == XHORSE_R_HITAG2) && (p->uidlen == 4) &&
           ((p->typebyte == 0x36) || (p->typebyte == 0x46));
}

static void xh_configure_run(const lf_xhorse_data_t *p, uint16_t cmd, bool ledcontrol) {
    xh_sess_t s;
    int open = xh_begin_any(&s, ledcontrol);
    if (open == PM3_ENOTIMPL) {
        // An XT136 answered, and xh_open_any has already closed its identify session with a ctl.
        // Refuse an unsupported type before opening a session, so nothing is disturbed.
        if ((cmd != CMD_LF_XHORSE_RESET) && (xh136_type_ok(p) == false)) {
            hitag_cleanup(ledcontrol);
            reply_ng(cmd, PM3_EINVARG, NULL, 0);
            return;
        }
        int res136 = PM3_EFAILED;
        if (xh_open_session_xt136(&s) == PM3_SUCCESS) {
            res136 = (cmd == CMD_LF_XHORSE_RESET) ? xh_reset_xt136(&s, p->verify)
                     : xh_configure_xt136(&s, p);
            xh_ctl(&s, XH_PG_PAD);
        }
        hitag_cleanup(ledcontrol);
        reply_ng(cmd, res136, NULL, 0);
        return;
    }
    if (open != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_ERROR) {
            Dbprintf("xhorse: no session after all attempts, including EM recovery");
        }
        hitag_cleanup(ledcontrol);
        reply_ng(cmd, PM3_EFAILED, NULL, 0);
        return;
    }

    uint8_t id16[16];
    xh_read_chipid(&s, id16);   // capability read, as the VVDI does before every set
    xh_ctl(&s, XH_PG_PAD);      // ends the capability-read session

    if (xh_begin_writes(&s) != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_ERROR) {
            Dbprintf("xhorse: no write session after the capability read");
        }
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

//-----------------------------------------------------------------------------
// Raw page primitives: `lf xhorse read` / `lf xhorse write`
//-----------------------------------------------------------------------------
// Open a session for either primitive, trying the XT27A handshake then the XT136 one, and record
// which family answered.  Neither primitive sends the EM-recovery burst or the ID8A probe: both
// rewrite the chip, which a raw read or a single-page write must not do.
static int xh_io_open(xh_sess_t *s, lf_xhorse_io_reply_t *out, bool ledcontrol) {
    xh_setup(s, ledcontrol);
    if (xh_open_session(s) != PM3_SUCCESS && xh_open_session_xt136(s) != PM3_SUCCESS) {
        return PM3_EFAILED;
    }
    out->found = 1;
    out->family = s->family;
    return PM3_SUCCESS;
}

// Select a bank, except on an XT136: it has one flat page space and never sees a `sel`.
static int xh_io_bank(xh_sess_t *s, uint8_t bank) {
    if (s->family == XHORSE_FAM_XT136) {
        return PM3_SUCCESS;
    }
    return xh_bank(s, bank);
}

static void xh_io_store(lf_xhorse_io_reply_t *out, uint8_t i, uint32_t w, bool agreed) {
    if (agreed) {
        out->ok[i / 8] |= (uint8_t)(1 << (i % 8));
    }
    out->data[i * 4 + 0] = (uint8_t)(w >> 24);
    out->data[i * 4 + 1] = (uint8_t)(w >> 16);
    out->data[i * 4 + 2] = (uint8_t)(w >> 8);
    out->data[i * 4 + 3] = (uint8_t)(w);
}

// READ `count` pages from `page` of `bank`, each until two reads agree.
void Xhorse_Read(const lf_xhorse_io_t *p, bool ledcontrol) {
    xh_sess_t s;
    lf_xhorse_io_reply_t out;
    memset(&out, 0, sizeof(out));
    out.bank = p->bank;
    out.page = p->page;

    uint8_t count = p->count;
    if (count == 0) {
        count = 1;
    }
    if (count > 64) {
        count = 64;
    }
    if ((uint16_t)p->page + count > 64) {
        count = (uint8_t)(64 - p->page);         // the address field is 6 bits; never wrap
    }
    out.count = count;

    if (xh_io_open(&s, &out, ledcontrol) != PM3_SUCCESS) {
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_XHORSE_READ, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));   // found == 0
        return;
    }

    int res = PM3_SUCCESS;
    if (xh_io_bank(&s, p->bank) != PM3_SUCCESS) {
        res = PM3_EFAILED;                       // sel refused: no bank is selected, so don't read
    } else {
        for (uint8_t i = 0; i < count; i++) {
            uint32_t w = 0;
            bool agreed = xh_read_agreed(&s, (uint8_t)(p->page + i), &w);
            xh_io_store(&out, i, w, agreed);
        }
    }
    xh_ctl(&s, XH_PG_PAD);

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_XHORSE_READ, res, (uint8_t *)&out, sizeof(out));
}

// WRITE one page, then read it back so the caller sees what is there whether or not it took.
// With p->cfg, `page` is a config-word index instead and the write goes via ctl 0x30+n; the page
// reported back is the one that word reads from, which differs per family (xh_cfg_page).
void Xhorse_Write(const lf_xhorse_io_t *p, bool ledcontrol) {
    xh_sess_t s;
    lf_xhorse_io_reply_t out;
    memset(&out, 0, sizeof(out));
    out.bank = p->bank;
    out.page = p->page;

    if (xh_io_open(&s, &out, ledcontrol) != PM3_SUCCESS) {
        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_XHORSE_WRITE, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));  // found == 0
        return;
    }

    // On an XT27A the VVDI never writes in the session it opened: it reads the chip ID, ends the
    // session with a `ctl` and re-handshakes.  That is the only write path confirmed on hardware
    // (xh_configure_run takes it too), so take it here as well.  The XT136 has no such capability
    // read, so it is written in the session just opened.
    if (s.family == XHORSE_FAM_XT27A) {
        uint8_t id16[16];
        xh_read_chipid(&s, id16);
        xh_ctl(&s, XH_PG_PAD);
        if (xh_begin_writes(&s) != PM3_SUCCESS) {
            if (g_dbglevel >= DBG_ERROR) {
                Dbprintf("xhorse: write, no session after the capability read");
            }
            hitag_cleanup(ledcontrol);
            reply_ng(CMD_LF_XHORSE_WRITE, PM3_EFAILED, (uint8_t *)&out, sizeof(out));
            return;
        }
    }

    // A config word lives in bank 1 on an XT27A and the VVDI always has it selected for the ctl
    // write; force it rather than letting a missing -b write with bank 0 selected.
    uint8_t bank = (p->cfg && (s.family == XHORSE_FAM_XT27A)) ? 1 : p->bank;
    out.bank = bank;

    int res = PM3_SUCCESS;
    if (xh_io_bank(&s, bank) != PM3_SUCCESS) {
        res = PM3_EFAILED;
    } else {
        uint8_t pg = p->page;
        if (p->cfg) {
            pg = xh_cfg_page(&s, p->page);
            res = xh_cfg_write(&s, p->page, p->value, p->verify);
        } else {
            res = xh_write(&s, p->page, p->value, p->verify);
        }
        out.verified = (p->verify && (res == PM3_SUCCESS)) ? 1 : 0;
        out.page = pg;
        uint32_t rb = 0;
        bool agreed = xh_read_agreed(&s, pg, &rb);
        xh_io_store(&out, 0, rb, agreed);
        out.count = 1;
    }
    xh_ctl(&s, XH_PG_PAD);

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_XHORSE_WRITE, res, (uint8_t *)&out, sizeof(out));
}

//-----------------------------------------------------------------------------
// XT57B (channel B / XMICRO) nonce collection: `lf xhorse xt57 nonce`.
//
// The XT57B's authentication is not understood yet (doc/xhorse_notes.md).  The 32-bit nonce it
// returns to the fixed 104-bit select is a window of a bit stream that looks the same on every tag,
// and the reader's 32-bit answer and the tag's 64-bit reply depend only on that nonce.  This runs
// the VVDI's session many times while one gap is stepped, to see which timing selects the window
// and to gather enough nonces to rebuild the stream.  The VVDI's gaps (T0 since the last tag edge):
// 0x5AA5 -> 0011 88, "XT57X" -> select 82, nonce -> answer 144, power-up -> XMICRO 120.14 ms.
//-----------------------------------------------------------------------------
#define XT57_DEF_POWERUP_US   XH_T_XMICRO_POWERUP_US
#define XT57_DEF_ACK_ID_T0    88
#define XT57_DEF_ID_SEL_T0    82
#define XT57_DEF_NONCE_ANS_T0 144
#define XT57_MAX_T0           5000     // the precision counter is 16 bit: 5461 T0 at most
// Default gaps for a scripted post-auth frame, from the VVDI's own sessions (doc/xt57-auth.md SS7.3):
// the first frame comes 1404-1409 T0 after the 64-bit reply, a frame that follows a tag response
// 178-192 T0 later, and the frame after a write's 8-bit ack about 1390 T0 later.  A script entry
// may override any of them.
#define XT57_POST_T0_FIRST    1408
#define XT57_POST_T0_RESP      190
#define XT57_POST_T0_ACK      1390

typedef struct {
    uint32_t nonce;
    uint32_t answer;
} xt57_known_t;

static const xt57_known_t xt57_known[] = {
#define X(n, a, h, l) { n, a },
    XHORSE_XT57_KNOWN
#undef X
};

static int xt57_t0(int32_t v) {
    if (v < 0) return 0;
    return (v > XT57_MAX_T0) ? XT57_MAX_T0 : (int)v;
}

static uint32_t xt57_us(int32_t v) {
    return (v < 0) ? 0 : (uint32_t)v;
}

// Replay/compose the scripted frames of a post-auth session.  Returns the number of frames sent.
// A frame at the wrong index is not a valid command and ends the session, so this stops as soon as
// a frame that expects an answer gets none: everything after it would be meaningless anyway.
static uint8_t xt57_post_run(xh_sess_t *s, const lf_xhorse_xt57_t *p, xt57_presp_t *out,
                             uint8_t *nkept) {
    uint8_t rx[32];
    uint16_t len = (p->scriptlen > XHORSE_XT57_SCRIPTBYTES) ? XHORSE_XT57_SCRIPTBYTES : p->scriptlen;
    uint8_t first = p->rfirst ? p->rfirst : 1;
    uint8_t prev_bits = 0;
    uint8_t sent = 0;
    uint8_t kept = 0;
    uint16_t at = 0;

    while (at < len && sent < p->npost) {
        uint8_t b = p->script[at++];
        if (b == 0) {
            break;
        }
        uint16_t gap_t0 = 0;
        if (b & 0x80) {
            if (at + 2 > len) {
                break;
            }
            gap_t0 = (uint16_t)(p->script[at] | (p->script[at + 1] << 8));
            at += 2;
            b &= 0x7F;
        }
        if (b == 0 || b > XHORSE_XT57_POSTBYTES * 8) {
            break;
        }
        uint8_t nb = (uint8_t)((b + 7) / 8);
        if (at + nb > len) {
            break;
        }
        const uint8_t *data = &p->script[at];
        at += nb;

        int gap;
        if (gap_t0) {
            gap = xt57_t0((int32_t)gap_t0);
        } else if (sent == 0) {
            gap = XT57_POST_T0_FIRST;
        } else {
            // a write (48 bits) is acked with 8 bits and the next frame follows it slowly
            gap = (prev_bits == 48) ? XT57_POST_T0_ACK : XT57_POST_T0_RESP;
        }

        size_t rxlen = xh_xfer_clear(s, data, b, rx, sizeof(rx), gap);
        sent++;
        prev_bits = b;
        if (sent >= first && kept < XHORSE_XT57_MAXPOST) {
            out[kept].bits = (uint8_t)((rxlen > 255) ? 255 : rxlen);
            memcpy(out[kept].data, rx, XHORSE_XT57_RESPBYTES);
            kept++;
        }

        if (rxlen == 0) {
            break;
        }
        if (BUTTON_PRESS() || data_available()) {
            break;
        }
    }
    *nkept = kept;
    return sent;
}

// One session, up to and including the reply to the answer.  Whatever got that far is in *r.
static void xt57_session(xh_sess_t *s, const lf_xhorse_xt57_t *p, int32_t val, lf_xhorse_xt57_rec_t *r) {
    static const uint8_t xmicro[6] = { 'X', 'M', 'I', 'C', 'R', 'O' };
    static const uint8_t query[1]  = { 0x30 };   // 4 bits, 0b0011

    uint32_t off_us = p->off_us ? p->off_us : XH_T_FIELD_OFF_US;
    uint32_t pwr_us = p->powerup_us ? p->powerup_us : XT57_DEF_POWERUP_US;
    int t_ack_id = XT57_DEF_ACK_ID_T0;
    int t_id_sel = XT57_DEF_ID_SEL_T0;
    int t_non_ans = XT57_DEF_NONCE_ANS_T0;

    switch (p->sweep) {
        case XT57_SWEEP_POWERUP:    pwr_us = xt57_us(val); break;
        case XT57_SWEEP_ACK_ID:     t_ack_id = xt57_t0(val); break;
        case XT57_SWEEP_ID_SEL:     t_id_sel = xt57_t0(val); break;
        case XT57_SWEEP_NONCE_ANS:  t_non_ans = xt57_t0(val); break;
        case XT57_SWEEP_FIELD_OFF:  off_us = xt57_us(val); break;
        default: break;
    }

    uint8_t rx[32];
    size_t rxlen;

    memset(r, 0, sizeof(*r));

    Gpio_SSC_DOUT_High();            // field off
    xh_delay_us(off_us);
    Gpio_SSC_DOUT_Low();             // field on
    xh_delay_us(pwr_us);

    rxlen = xh_xfer_clear(s, xmicro, 48, rx, sizeof(rx), 0);
    if (rxlen < 16) {
        return;
    }
    r->ack[0] = rx[0];
    r->ack[1] = rx[1];
    // 16 bits is not an ack: a chip emulating a talk-first type modulates the field all the way
    // through the window, and the demodulator reads its carrier as 5555.  Only 5AA5 is the ack.
    if ((rx[0] != 0x5A) || (rx[1] != 0xA5)) {
        if (p->press_on == 0) {
            return;
        }
    } else {
        r->flags |= XT57_F_ACK;
    }

    rxlen = xh_xfer_clear(s, query, 4, rx, sizeof(rx), t_ack_id);
    if (rxlen >= 16) {
        memcpy(r->ident, rx, sizeof(r->ident));
    }
    bool ident_crc = xh_ident_crc_ok(rx, rxlen);
    if (ident_crc) {
        r->flags |= XT57_F_IDCRC;
    }
    if ((rxlen >= 32) && (rx[0] == 'X') && (rx[1] == 'T')) {
        r->flags |= XT57_F_IDENT;
    } else if (p->press_on == 0) {
        return;
    }

    uint16_t selbits = p->selbits ? p->selbits : (XHORSE_XT57_SELLEN * 8);
    if (selbits > XHORSE_XT57_SELLEN * 8) selbits = XHORSE_XT57_SELLEN * 8;

    // A chip ignores a select that does not carry its own family bytes: an XT37 given a "XT57"
    // select acks and identifies and then answers no nonce at all (docs/xt37.md SS3).  So build the
    // select from the family the chip just named, rather than from a constant - but only on the
    // standard 13-byte 0xC0 form, and only when the reply's CRC validated, so a bad demod cannot
    // put rubbish in the frame.  `sel_auto == 0` sends select[] exactly as the client gave it.
    uint8_t sel[XHORSE_XT57_SELLEN];
    memcpy(sel, p->select, sizeof(sel));
    if (p->sel_auto && ident_crc && (selbits == XHORSE_XT57_SELLEN * 8) && (sel[0] == 0xC0)) {
        if (memcmp(&sel[1], rx, 4) != 0) {
            memcpy(&sel[1], rx, 4);
        }
        r->flags |= XT57_F_SELFAM;
    }
    rxlen = xh_xfer_clear(s, sel, selbits, rx, sizeof(rx), t_id_sel);
    r->nonce_bits = (uint8_t)((rxlen > 255) ? 255 : rxlen);
    memcpy(r->nonce, rx, sizeof(r->nonce));
    if (rxlen < 32) {
        return;
    }
    r->flags |= XT57_F_NONCE;

    uint32_t nonce = xh_bits_to_u32(rx, 32);
    uint32_t answer = 0;
    bool send = false;
    if (p->answer_mode == XT57_ANS_FIXED) {
        answer = p->answer;
        send = true;
    } else if (p->answer_mode == XT57_ANS_KNOWN) {
        for (size_t i = 0; i < ARRAYLEN(xt57_known); i++) {
            if (xt57_known[i].nonce == nonce) {
                answer = xt57_known[i].answer;
                send = true;
                break;
            }
        }
    }
    if (send == false) {
        return;
    }

    uint8_t tx[4] = { (uint8_t)(answer >> 24), (uint8_t)(answer >> 16), (uint8_t)(answer >> 8), (uint8_t)answer };
    memcpy(r->answer, tx, sizeof(tx));
    r->flags |= XT57_F_ANSWER;

    rxlen = xh_xfer_clear(s, tx, 32, rx, sizeof(rx), t_non_ans);
    r->reply_bits = (uint8_t)((rxlen > 255) ? 255 : rxlen);
    memcpy(r->reply, rx, sizeof(r->reply));
}

void Xhorse_Xt57Nonce(const lf_xhorse_xt57_t *p, bool ledcontrol) {
    xh_sess_t s;

    // Scripted mode: run sessions until the wanted nonce comes up, then send the script in that
    // session and stop.  The keystream is a function of the nonce, so a script is only meaningful
    // at the nonce it was built for.
    if (p->npost > 0) {
        lf_xhorse_xt57_post_reply_t pout;
        memset(&pout, 0, sizeof(pout));
        xh_setup(&s, ledcontrol);

        uint16_t tries = p->count ? p->count : 1;
        for (uint16_t i = 0; i < tries; i++) {
            int64_t v = (int64_t)p->base + (int64_t)i * p->step;
            if (v > INT32_MAX) v = INT32_MAX;
            if (v < INT32_MIN) v = INT32_MIN;

            lf_xhorse_xt57_rec_t rec;
            xt57_session(&s, p, (int32_t)v, &rec);
            pout.sessions = (uint8_t)((i + 1 > 255) ? 255 : i + 1);

            uint32_t nonce = xh_bits_to_u32(rec.nonce, 32);
            bool want_ok = (p->want == 0) || (nonce == p->want);
            // only a session that actually authenticated can carry the script
            // Normally only an authenticated session can carry the script.  With --noanswer
            // there is no auth at all: run the script straight after the identify, so a
            // plain-mode command set can be probed (doc/xt57-auth.md SS17.5).
            bool carry = (rec.reply_bits >= 64) ||
                         ((p->answer_mode == XT57_ANS_NONE) && (rec.flags & XT57_F_IDENT));
            if (want_ok && carry) {
                pout.rec = rec;
                pout.rfirst = p->rfirst ? p->rfirst : 1;
                pout.nsent = xt57_post_run(&s, p, pout.presp, &pout.npost);
                break;
            }
            pout.rec = rec;
            if (BUTTON_PRESS() || data_available()) {
                break;
            }
        }

        hitag_cleanup(ledcontrol);
        reply_ng(CMD_LF_XHORSE_XT57_NONCE, PM3_SUCCESS, (uint8_t *)&pout, sizeof(pout));
        return;
    }

    lf_xhorse_xt57_reply_t out;
    memset(&out, 0, sizeof(out));

    xh_setup(&s, ledcontrol);

    uint8_t n = (p->count > XHORSE_XT57_MAXSESS) ? XHORSE_XT57_MAXSESS : p->count;
    for (uint8_t i = 0; i < n; i++) {
        // the sweep is a signed 32-bit sum; keep it from overflowing on a silly base/step
        int64_t v = (int64_t)p->base + (int64_t)i * p->step;
        if (v > INT32_MAX) v = INT32_MAX;
        if (v < INT32_MIN) v = INT32_MIN;
        xt57_session(&s, p, (int32_t)v, &out.rec[i]);
        out.count++;
        if (BUTTON_PRESS() || data_available()) {
            break;
        }
    }

    hitag_cleanup(ledcontrol);
    reply_ng(CMD_LF_XHORSE_XT57_NONCE, PM3_SUCCESS, (uint8_t *)&out, sizeof(out));
}
