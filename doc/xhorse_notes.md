# Notes on Xhorse super-chips (`lf xhorse`)
<a id="top"></a>

# Table of Contents
- [Notes on Xhorse super-chips (`lf xhorse`)](#notes-on-xhorse-super-chips-lf-xhorse)
- [Table of Contents](#table-of-contents)
- [Overview](#overview)
- [Commands](#commands)
- [Timing conventions](#timing-conventions)
- [Channel A protocol (XT27A)](#channel-a-protocol-xt27a)
  - [Handshake](#handshake)
  - [Cipher](#cipher)
  - [Commands, banks and pages](#commands-banks-and-pages)
  - [Worked example](#worked-example)
- [Reset and recovering a chip stuck in an EM emulation](#reset-and-recovering-a-chip-stuck-in-an-em-emulation)
  - [The problem](#the-problem)
  - [The sequence](#the-sequence)
  - [The unlock burst](#the-unlock-burst)
  - [Evidence](#evidence)
  - [How it was found](#how-it-was-found)
- [Channel B identify (XT27B / XT57B)](#channel-b-identify-xt27b--xt57b)
- [Open questions](#open-questions)
- [Credits](#credits)

# Overview
^[Top](#top)

Xhorse "super chips" are 125 kHz transponders that can be configured to emulate many key
transponders (HITAG2/ID46, PCF7935, Megamos ID48, TI, EM4100/ID11, ...). They are normally set up
with Xhorse's own programmers (e.g. the VVDI Mini).

`lf xhorse` supports the **XT27A**, configured over a management protocol referred to here as
**channel A**: it rides on the HITAG2 physical layer, and after a short cleartext handshake every
frame is XOR-encrypted with a 16-bit LFSR keystream.

Newer families (**XT27B**, **XT57B**) answer a different probe, **XMICRO** ("channel B"). Their
management protocol is not decoded; `lf xhorse info` identifies them and reports them as
unsupported.

# Commands
^[Top](#top)

| Command | Purpose |
|---|---|
| `lf xhorse info` | Identify the chip: channel-B family, or XT27A chip ID, emulated type byte and the first 8 emulated pages |
| `lf xhorse dump` | Read the XT27A chip ID, type byte and all 64 bank-0 pages |
| `lf xhorse configure -t <type> [-u <uid>]` | Set the emulated transponder type (and UID) |
| `lf xhorse reset` | Restore the factory-default ID46/HITAG2 personality; also recovers a chip stuck in an EM emulation |
| `lf xhorse types` | List the `-t` type tokens |

`info` and `dump` never send the EM unlock burst (it changes the chip), so a chip emulating an EM
type does not answer them; use `reset` or `configure`.

# Timing conventions
^[Top](#top)

* Carrier 125 kHz, so **1 T0 = 8 us** (one carrier cycle).
* Reader to tag: HITAG2 BPLM. Every bit starts with an 8 T0 field-off gap; a `0` lasts 20 T0 and a
  `1` 28 T0, gap to gap; a final gap ends the frame. The reader sends no start-of-frame.
* Tag to reader: Manchester, 4 kbit/s, after a 5-bit `11111` start-of-frame.

# Channel A protocol (XT27A)
^[Top](#top)

## Handshake

Three cleartext frames:

| Frame | Bits | Value | Gap before next |
|---|---|---|---|
| `XHORSE` (ASCII) | 48 | `0x58484F525345` | 8 ms |
| `072F` | 16 | `0x072F` | 11.78 ms |
| `7372` | 16 | `0x7372` | the chip replies |

The chip needs the gaps to compute its replies. Its 16-bit answer to `7372` is different every
session (e.g. `0x54CF`) and seeds the cipher.

## Cipher

* `seed = bitrev16(reply) XOR 0xE12E`
* The keystream is a 16-bit Fibonacci LFSR with taps `0xA011`, output MSB first. Its **first 16
  output bits are the seed itself**; it then runs continuously for the rest of the session.
* Every later frame, **in both directions**, is XORed with the next keystream bits: 16 for a
  command or a 16-bit reply, 32 for a data word.

## Commands, banks and pages

Commands are 16 bits, `op, ~op`, with `op = (func << 6) | page`:

| func | Meaning | Chip reply |
|---|---|---|
| 0 | `ctl` - ends the session | none |
| 1 | `sel` - select bank (page field = bank) | echo, 16 bits |
| 2 | `write` page | echo, 16 bits; then the reader sends a 32-bit data word |
| 3 | `read` page | 32-bit data |

* Bank 0 is the emulated transponder's memory; bank 1 holds configuration and identity
  (unique chip ID at pages `0x33`-`0x36`, emulated type byte at `0x3C`).
* The first encrypted command of a session is always `sel bank 1` (`0x41BE`), which the chip echoes:
  a check that both sides agree on the keystream ("4R").
* After a write the chip needs ~12.8 ms to program its EEPROM before the next command.
* A `ctl` ends the session; further commands need a fresh power cycle and handshake.

## Worked example

From a real session (`hw dbg 4`):

```
7372 reply        0x54CF
bitrev16          0xF32A
seed              0xF32A ^ 0xE12E = 0x1204
sel bank 1        0x41BE ^ 0x1204 = 0x53BA     (sent)
chip echo         0x115D ^ 0x50E3 = 0x41BE     (2nd keystream word 0x50E3)
read page 0x33    0xF30C ^ next keystream = 0x0469   (sent)
```

# Reset and recovering a chip stuck in an EM emulation
^[Top](#top)

## The problem

An XT27A emulating a reader-talks-first type such as HITAG2 powers up and listens, so the handshake
works. An EM4100-family type such as ID11 is tag-talks-first: a real EM4100 is read-only and loops
its 64-bit code from power-up. An XT27A set to ID11 does the same, never hears the handshake, and
looks dead to anything that only knows channel A. Measured from the frame repetition period:

| Emulated type | EM frame period | Bit time | Rate |
|---|---|---|---|
| ID11 | 16.38 ms | 32 T0 | RF/32 |
| ID12 | 20.48 ms | 40 T0 | RF/40 |

RF/32 Manchester is the same coding and rate as a genuine XT27A reply, and RF/40 is close to it,
so a broadcast could be demodulated as a `7372` reply and "complete" a handshake with a garbage
seed, which would stop the EM fallback from running. To guard against that, the firmware only
accepts a session once the chip's echo of the first encrypted command (`sel bank 1`) decrypts to
exactly `0x41BE`; otherwise the attempt counts as failed.

## The sequence

`lf xhorse reset` (and `configure`) first tries the normal session open nine times (field off,
power-up, handshake; three timings, see below). For a chip stuck in an EM type all nine fail, and the firmware falls
back to the unlock:

```
for each burst timing in the table (see below):
    OFF 70.05 ms
    ON  |<-- burst_at -->|<- burst (EM 15.95 ms, ID48 19.6 ms) ->|<- rest of window ->|
OFF 1570.94 ms                                   (cold-boot)
ON  |<- 30.18 ms ->| XHORSE ..8 ms.. 072F ..11.78 ms.. 7372 -> reply
```

This is the VVDI's structure. In all eight VVDI EM reset captures (ID11, ID12, ID13) it sends its
bursts with no handshake in between, then powers down for 1.57 s once, then handshakes. It sends 12
bursts; the firmware sends one per timing.

The burst only works if it starts at the right point in the chip's own EM frame, which starts at
power-up. Measured from the field-on edge:

| Type | Frame | Freeing bursts (T0 after power-up) | Failing bursts |
|---|---|---|---|
| ID11 (RF/32) | 2048 T0 | 1409, 1410, 1417 (VVDI), 1423 (Proxmark), and the same + 6 frames | 1427-1450 + 6 frames (VVDI first tries) |
| ID12 (RF/40) | 2560 T0 | 1741 = 43.5 x 40 (VVDI), ~1752 (Proxmark) | ~911 + 5 frames (Proxmark, ID11 timing) |
| ID13 (RF/64) | 4096 T0 | 1398 (VVDI), ~1752 (Proxmark) | ~1408 and ~1423 + 3 frames (Proxmark) |

For ID11 and ID12 the freeing bursts start about **44 units** into the frame, a unit being 32 T0
for ID11 and 40 T0 for ID12, and the same position in a later frame also works for ID11. The ID11
window is narrow: from ~1425 T0 bursts stop working reliably, which is why the VVDI's own first try
often fails. ID13 is different: it is freed by a burst in its **first frame** after power-up (at
1398 or ~1752 T0), but not by the same position three frames later. The firmware aims at the centre
of each window, commanding 3.5 T0 early because its burst reaches the air that much after the
commanded time, and sends a burst at every entry because it doesn't know the emulated type:

| Entry | Window | `burst_at` | Lands at (T0 after power-up) | Frees |
|---|---|---|---|---|
| ID11 | 155.94 ms | 109.54 ms | 6 ID11 frames + 1408 (unit 44.0) | ID11 |
| ID12/ID13 | 60.30 ms | 13.99 ms | ~1752 (unit 43.8 for ID12; first ID13 frame) | ID12, ID13 |
| ID48 | 63.86 ms | 12.95 ms | ~1622 (the VVDI's position), with the ID48 burst | ID48 |

A burst at the wrong timing is harmless once the chip is freed: the VVDI keeps bursting after the
chip goes silent. The burst turns the EM emulation off. ID11 and ID12 would answer the handshake
30 ms after a 70 ms field-off, but ID13 goes silent and does not answer until it has been powered
down for longer, which is why the cold-boot is always made. Each attempt takes ~2.2 s whatever the
emulated type; up to three are made.

Normal session opens cycle three timings: the short 20 ms off / 3 ms power-up, the VVDI's 70.05 ms
off / 30.18 ms settle, and 177.1 ms off / 41.8 ms on. The second covers a chip left half-recovered
by an earlier failed run (its normal handshakes read 10 bits instead of 0). The third is for a chip
set to ID8A (see [Types that need no unlock](#types-that-need-no-unlock)). Later sessions in the same run always use the 70.05 ms off /
30.18 ms settle. Reset then:

1. reads the chip ID (bank 1, pages `0x33`-`0x36`) and ends the session with `ctl 0x3F`;
2. opens a new session, reads status page `0x3F`, and writes, each with a 13 ms program wait and a
   read-back check:

| Bank | Page | Value | Meaning |
|---|---|---|---|
| 1 | `0x3C` | `0x00000036` | emulated type = ID46 (HITAG2) |
| 0 | `0x00` | `0x1D421D12` | default UID |
| 0 | `0x01` | `0x4D494B52` | `"MIKR"` (HITAG2 default password, part 1) |
| 0 | `0x02` | `0x00004F4E` | `"..ON"` (part 2) |
| 0 | `0x03` | `0x06AA4854` | HITAG2 configuration page |

All recovery timings are the VVDI Mini's, measured from its captures.

## The unlock burst

43 field-off pulses, 15.95 ms from the start of the first to the end of the last. Offsets are T0
from the start of the first pulse; widths are how long the field is held off.

| # | start | width | # | start | width | # | start | width | # | start | width |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | 20 | 12 | 511 | 19 | 23 | 1024 | 15 | 34 | 1562 | 19 |
| 2 | 36 | 16 | 13 | 576 | 19 | 24 | 1054 | 15 | 35 | 1596 | 16 |
| 3 | 68 | 15 | 14 | 610 | 16 | 25 | 1115 | 19 | 36 | 1658 | 19 |
| 4 | 129 | 19 | 15 | 641 | 15 | 26 | 1180 | 19 | 37 | 1692 | 16 |
| 5 | 163 | 16 | 16 | 702 | 19 | 27 | 1214 | 16 | 38 | 1753 | 19 |
| 6 | 224 | 19 | 17 | 767 | 19 | 28 | 1245 | 15 | 39 | 1787 | 16 |
| 7 | 258 | 15 | 18 | 832 | 19 | 29 | 1306 | 19 | 40 | 1818 | 15 |
| 8 | 290 | 15 | 19 | 865 | 16 | 30 | 1340 | 16 | 41 | 1880 | 19 |
| 9 | 351 | 19 | 20 | 927 | 19 | 31 | 1372 | 15 | 42 | 1944 | 19 |
| 10 | 415 | 19 | 21 | 961 | 16 | 32 | 1433 | 19 | 43 | 1978 | 16 |
| 11 | 449 | 15 | 22 | 992 | 15 | 33 | 1497 | 19 | | | |

The gaps are 15-20 T0 (120-160 us), spaced roughly 32 T0 (S) or 64 T0 (L) apart, i.e. one or two
bit-times of the ID11 chip's RF/32 broadcast:

```
S S L S L S S L L S L L S S L L L S L S S S S L L S S L S S L L L S L S L S S L L S
```

* Positions are averaged over 22 occurrences in two VVDI reset captures; they agree to within
  0.75 T0, so it is a fixed waveform.
* Widths are corrected for the sniffer: an 8 T0 BPLM gap measures 10 T0 in a trace (both the
  Proxmark's and the VVDI's), so 2 T0 was subtracted from each measured width.
* It is the reader's signal: it clips to full scale like the reader's BPLM frames, whereas the
  chip's EM signal does not, and it is absent from a Proxmark capture of the same chip taken before
  the burst was implemented.
* The VVDI sends the identical burst (to within 1-2 T0) to a chip emulating ID12, whose broadcast
  is RF/40, and it frees that chip too. The pulses themselves are not tuned to the chip's bit rate;
  only the time the burst starts is (see [The sequence](#the-sequence)).
* What it encodes is unknown. The spacings are close to, but not exactly, multiples of 32 T0
  (30-36 and 61-65). The firmware replays it verbatim (`xh_em_burst` in `armsrc/xhorse.c`).

## The ID48 unlock burst

A chip emulating ID48 (Megamos) is not freed by the EM burst. In the VVDI's capture
(`vvdi-id48-reset-success.pm3`) the chip broadcasts a 160 T0 periodic pattern from ~213 T0 after
power-up, and the VVDI sends a **different burst** 1622 T0 after power-up, on the same ~32 T0 grid
with the same two pulse widths. Its sequence is short (~4.4 s, with no 1.57 s cold-boot):

| Step | VVDI |
|---|---|
| 1 | off 120 ms, on 63.9 ms: burst **A** at 1622 T0 |
| 2 | off 720 ms, XMICRO probe (120 ms after power-up), then 3 x (off 30 ms, XMICRO): no answer |
| 3 | off 130 ms, on 63.9 ms: burst **A** again |
| 4 | as step 2: no answer |
| 5 | off 140 ms, on 63.9 ms: burst **B** at 1621 T0 |
| 6 | off 420 ms, on, XHORSE 30.08 ms later: `5AA5`, handshake, then the normal reset writes |

Burst A has 48 pulses; burst B is burst A with its third pulse (at +69 T0) left out, matching to
within 1-3 T0 elsewhere. After each A the chip is still broadcasting at the next power-up; after B
it is silent and answers XHORSE. A is followed by XMICRO (channel B) probes and B by XHORSE
(channel A), so A is probably the variant for the XMICRO-family chips; that is a guess. The firmware
sends only B, as a third entry in the timing table, before the usual cold-boot (1.57 s rather than
the VVDI's 420 ms here).

Burst B, 47 field-off pulses, 19.6 ms (2447 T0); offsets in T0 from the start of the first pulse,
widths as held off (measured minus 2 T0), averaged over the three occurrences (agreeing within 2 T0):

```
start: 0 35 131 167 200 262 298 332 394 459 525 590 626 689 725 787 823 857 890 923
       985 1050 1116 1152 1247 1313 1348 1382 1444 1510 1575 1673 1709 1742 1776 1838
       1903 1969 2004 2038 2071 2165 2202 2264 2300 2333 2428
width: 19 17 19 16 16 20 17 16 19 19 20 20 17 20 16 20 17 16 16 16 19 20 19 16
       19 19 16 16 19 20 19 19 16 16 15 20 19 20 17 16 16 20 16 19 16 16 19
```

Gaps in units of ~32 T0 (burst A has `11` where B has `3` at the start):

```
A: 1 1 2 1 1 2 1 1 2 2 2 2 1 2 1 2 1 1 1 1 2 2 2 1 3 2 1 1 2 2 2 3 1 1 1 2 2 2 1 1 1 3 1 2 1 1 3
B:  3  1 1 2 1 1 2 2 2 2 1 2 1 2 1 1 1 1 2 2 2 1 3 2 1 1 2 2 2 3 1 1 1 2 2 2 1 1 1 3 1 2 1 1 3
```

Gaps of 3 units appear here but never in the EM burst, so the two are different messages; what
either encodes is unknown.

### Types that need no unlock

A chip emulating ID4C (TI fixed-code) is reader-talks-first, like ID46: in the VVDI's capture
(`vvdi-id4C-reset-success.pm3`) it is silent at every power-up, the VVDI sends no burst, and the
chip answers an ordinary XHORSE handshake (off 130 ms, on, XHORSE 30.18 ms later). `lf xhorse reset`
resets it through the normal session open, without EM recovery.

A chip set to ID8A is also silent and needs no burst (`vvdi-id8A-reset-success.pm3`; the reset writes are
the usual ones), but it ignored every XHORSE we sent straight after power-up (3, 30 or 198 ms on). Before its first
XHORSE the VVDI keeps the field off 177.1 ms (a 20.6 ms "off" followed by a one-sample blip, not a
real power-up: the sniff stays flat until the real power-up transient), and sends XHORSE 41.8 ms after
power-up, in both VVDI captures (`vvdi-id8A-reset-success.pm3`, `-success-2.pm3`). The session
open's third timing copies this. The first capture also has one lone 56 us field gap 11.6 ms after
power-up; the second does not, so it is not needed.

## Evidence

A sniff of the first successful Proxmark reset, which still replayed the VVDI's entire ~10 s
sequence (a listen window, twelve burst windows interleaved with XMICRO probes, short `1111111`
frames, long rests and a 1.57 s cold-boot), checked window by window for the chip's EM signal:

| Window | Chip |
|---|---|
| 8 normal handshake attempts | broadcasting EM throughout |
| first burst window, before the burst | broadcasting, right up to the burst |
| first burst window, after the burst | silent |
| every later window (70 ms / 570 ms / 1.57 s offs between) | silent from power-up |
| after the cold-boot | answers the handshake; reset completes |

The first burst alone turned the emulation off, and it stayed off across every later power cycle.
The firmware was cut down to just that burst plus the handshake, and still resets a stuck ID11 chip
first time.

## How it was found

1. With the chip set to ID11 every handshake got EM noise instead of a reply.
2. Captures of a VVDI Mini resetting the chip showed ~10 s of regular field on/off cycling before
   its handshake. Copying those on/off timings exactly did not work.
3. Suspected causes that turned out wrong: the Proxmark's stronger field (moving and tilting the
   chip made no difference), coil ring-down shortening the off periods (measured: it doesn't), and
   the VVDI's no-tag behaviour (a different branch of its state machine that probes for TI 134 kHz
   transponders).
4. Going through the VVDI captures one field-on window at a time showed a 16 ms burst of field gaps
   inside each 156 ms window. It had been missed for two reasons: the trace decoder ended a reader
   frame after a 40 T0 gap, so it discarded pulses 32-64 T0 apart; and with a chip present, its EM
   broadcast overlapped the burst.
5. Replaying the full VVDI sequence with the bursts reset the chip; a sniff of that success showed
   only the first burst mattered.

# Channel B identify (XT27B / XT57B)
^[Top](#top)

As the VVDI reads an XT27B or XT57B:

```
field on, wait 120.14 ms
reader -> XMICRO (ASCII, 48 bits, 0x584D4943524F)    chip -> 0x5AA5
reader -> 0011 (4 bits)                              chip -> ASCII family, e.g. "XT37" / "XT57", + 1 byte
```

The XT27A never answers XMICRO. The 120 ms wait matters: an XT57B answers an XMICRO sent 3 ms after
power-up, but an XT27B only answers once it has had the VVDI's 120 ms (confirmed on hardware). An
XT27B reports itself as `XT37`. The byte after the family name varies; it may be a seed for
channel-B encryption, as the `7372` reply is for channel A. The channel-B management protocol that
follows is not decoded.

# Open questions
^[Top](#top)

* What the unlock burst encodes, and whether it is a generic "leave emulation" command.
* Whether other talk-first emulations need their own timing. The same burst frees ID11, ID12 and
  ID13, and the Proxmark reset is confirmed on all three.
* What decides the window. ID11 and ID12 fit "~44 units into a frame, in any frame"; ID13 is freed at
  two different points of its first frame (1398 and ~1752 T0) but not at the ID11 position three
  frames later. One VVDI ID11 burst at 1512 T0 also worked. Detecting the chip's bit rate from its
  broadcast might allow computing the position instead of trying a table.
* What the half-recovered ID13 state is (normal handshakes read 10 bits instead of 0 after a failed
  first run), and why ID13 needs a long power-down before it answers when ID11 and ID12 don't.
* How much slack the copied 70 ms gap and 30.18 ms settle have.
* Detecting the EM broadcast directly would save the ~0.8 s of failed normal handshakes.
* The channel-B (XT27B / XT57B) management protocol.

# Credits
^[Top](#top)

This would not have been possible without @CastleBravo, @HenryGab and @eltrick on the Proxmark3
Discord, and kivijakola's hitager project on GitHub.
