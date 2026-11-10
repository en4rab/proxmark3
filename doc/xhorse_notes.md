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
- [XT136](#xt136)
- [Config words (ctl 0x30+n)](#config-words-ctl-0x30n)
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

The **XT136** also uses channel A, with a different handshake and seed constant and no banks.
`lf xhorse info` and `dump` read it; `configure` and `reset` refuse it for now (see [XT136](#xt136)).

Newer families (**XT27B**, **XT57B**) answer a different probe, **XMICRO** ("channel B"). Their
management protocol is not decoded; `lf xhorse info` identifies them and reports them as
unsupported.

# Commands
^[Top](#top)

| Command | Purpose |
|---|---|
| `lf xhorse info` | Identify the chip: channel-B family, or XT27A chip ID, emulated type byte and the first 8 emulated pages, or XT136 pages `0x00`-`0x0F` |
| `lf xhorse dump` | Read the XT27A chip ID, type byte and all 64 pages of bank 0 and of bank 1, one session per bank; on an XT136, pages `0x00`-`0x3F` in one session |
| `lf xhorse configure -t <type> [-u <uid>]` | Set the emulated transponder type, and its UID: `-u` is required for every type that takes one |
| `lf xhorse reset` | Restore the factory-default ID46/HITAG2 personality; also recovers a chip stuck in an EM emulation |
| `lf xhorse read -p <page> [-b <bank>] [-n <count>]` | Read raw channel-A pages: `-b` picks the bank on an XT27A and is ignored on an XT136 |
| `lf xhorse write -p <page> -d <hex> [-b <bank>]` | Write one raw channel-A page and read it back. See "Dangerous pages" first |
| `lf xhorse types` | List the `-t` type tokens |

`info` and `dump` never send the EM unlock burst (it changes the chip), so a chip emulating an EM
type does not answer them; use `reset` or `configure`.

`read` and `write` are the raw page primitives: no recipe, no interpretation, one page at a time.
They identify the family themselves, so an XT136 is read and written with the same commands - it has
no banks, and `-b` is reported as ignored rather than silently dropped. Neither sends the EM burst
or the ID8A probe, so like `info`/`dump` they cannot reach a chip stuck in an EM emulation, and
unlike `configure` they never change anything you did not name. A `write` on an XT27A takes the
VVDI's two-session path (capability read, `ctl`, re-handshake, then the write), which is the only
write path confirmed on hardware; on an XT136 the page is written in the session just opened.
`tools/analysis/xhorse_page_sweep.py` drives both to map a chip's page space.

The chip's replies are weak, and a reply that loses an edge can decode to a wrong value with no
error. So `info` and `dump` read the type byte and each page until two reads in a row agree
(up to four reads). A type byte that never agrees gets a warning; a page that never agrees is marked
with a red `?`.

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
* **The page address is 6 bits, so a bank has exactly 64 pages and no more** - all four `func`
  values are in use, so there is no spare bit and no page number above `0x3F` can be put on the
  wire at all. Measured on hardware with `lf xhorse write`/`read`: all 64 bank-0 pages are real,
  independent, read/write storage - writing a distinctive value to each page in turn moved that
  page and no other, all 64 took the value, and all 64 restored (see "Bank-0 page probe" below).
* **Bank 1 has all 64 pages too**: `0x00`-`0x3C` are plain writable storage (marker took, only that
  page moved, restored clean), while `0x3D`-`0x3F` accept a `WRITE` and silently ignore it. So
  `ctl 0x30+n` is the *only* way to set a config word, not a VVDI quirk.
* **The 16-byte chip ID at bank-1 `0x33`-`0x36` is writable, not read-only.** See "Dangerous pages".
* **An XT136 has 16 pages, not 64.** Reading all 64 addresses returns the `0x00`-`0x0F` block four
  times over, and a write to page *p* lands on `p`, `p+0x10`, `p+0x20` and `p+0x30` at once
  (13/13 pages measured, and writing `0x34` changes `0x04`), so it decodes only `page & 0x0F`.
* **Only banks 0 and 1 are storage.** `sel 2` and `sel 3` are echoed, but every address in them -
  read *and* write - resolves to bank-0 page `0x00`, so they are a degenerate alias, not a third
  and fourth bank. `sel 4` to `sel 0x3F` are not echoed at all. Channel-A storage is therefore
  128 pages / 512 bytes in total.
* The first encrypted command of a session is always `sel bank 1` (`0x41BE`), which the chip echoes:
  a check that both sides agree on the keystream ("4R").
* After a write the chip needs ~12.8 ms to program its EEPROM before the next command.
* A `ctl` ends the session; further commands need a fresh power cycle and handshake.

### Bank-0 page probe (hardware, 2026-11-09)

How the page count above was measured, with `lf xhorse read`/`write` on an XT27A (chip ID
`9EA0F116A3702DB8655FFEC55D397946`):

1. Read all 64 bank-0 pages as a baseline, requiring every page confirmed (two reads agreeing).
2. For each page `n` of `0x00`-`0x3F`: write `A5A5nn nn`, read all 64 pages back, and check that
   page `n` holds the marker and no other page moved; then write the baseline value back and check
   the whole bank matches the baseline again.
3. Result: 64/64 pages `REAL` - the marker took, exactly one page moved, and the restore was clean.
   No page was read-only, and no two addresses aliased.
4. Probe `sel 0`-`sel 0x3F` read-only: `0`, `1`, `2`, `3` echoed, `4`-`0x3F` did not. Banks 2 and 3
   returned the same value at all 64 addresses. Writing bank-0 page `0x00` changed that value, and
   writing bank 2 page `0x3F` changed bank-0 page `0x00`, so both directions alias to page `0x00`.

### Dangerous pages

Neither bank bricked the chip under the sweep below, but three bank-1 ranges need care:

| Page | What it is | Why it is dangerous |
|---|---|---|
| bank 1 `0x33`-`0x36` | the 16-byte unique chip ID | **Writable.** A `WRITE` takes and `info` then reports the altered ID. Nothing else on the chip holds a copy, so overwriting it without having read it first destroys the chip's identity for good. |
| bank 1 `0x3C` | emulated type byte | Writing it changes what the chip emulates. A type that silences channel A (`0x8C`, `0x48`, `0x8A`) needs `reset`/`configure` and its EM burst to recover; a type byte with no recipe may not recover at all. |
| bank 1 `0x3D`-`0x3F` | config words 0/1 and the pad | Immune to `WRITE` (accepted, ignored), so harmless from that path. They *are* writable through `lf xhorse write -c <n>` (`ctl 0x30+n`), which is the dangerous one. Word 0's bit `0x200` and word 1's top nibble were each changed and restored on hardware with no side effect on a 7936 chip, but every other bit is untested and a config word is believed to take part in selecting what the chip emulates. Read the word first and keep the value. |

Bank 0's 64 pages are all plain storage; `lf xhorse reset` rewrites them.

On an **XT136** the equivalent three are pages `0x0D`-`0x0F`, the config words, and they are
write-protected the same way: all three accept a `WRITE` and ignore it (measured, one page at a
time). They take a `ctl 0x30+n` write, though: all three were changed and restored on hardware with
`lf xhorse write -c <n>`, each moving only its own page and leaving emulation working. Word 1's
high byte is the **emulated type byte** (`0x36` ID46, `0x46` 7946; docs/xt136.md SS5a).

`lf xhorse reset` now supports an XT136 and rewrites the config words and pages `0x00`-`0x0C` to
the VVDI's factory image, so a bad write is recoverable - **but only while channel A still answers**.
There is no EM-style recovery burst for the family, so a config value that silenced channel A would
still be unrecoverable. `lf xhorse configure` supports an XT136 for `ID46`/`7936` and `7946` only -
the two types its own app offers - and refuses any other before writing anything.

### Bank-1 page probe (hardware, 2026-11-09)

Same method as the bank-0 probe, but one page per step with the intent written to disk *before*
each write, so a chip that died mid-write would be attributed to the right page, and with the chip
ID re-read after every restore as a tripwire. Ascending order, so the identity and config pages came
last and the plain pages were banked first.

Result over all 64: **61 `REAL`** (`0x00`-`0x3C`), **3 write-ignored** (`0x3D`-`0x3F`), no aliasing,
no page that stopped the chip answering, and both banks byte-identical to their baselines afterwards.
On this chip bank 1 read all zero except `0x21` = `00010000`, `0x29` = `00610000`, the chip ID at
`0x33`-`0x36`, type `0x3C` = `00000036`, `0x3D` = `0000020F` and `0x3E` = `F370CA18`.

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

`lf xhorse reset` (and `configure`) first tries the normal session open twelve times (field off,
power-up, handshake; four timings, see below; `info` and `dump` use only the first three, nine
times). For a chip stuck in an EM type all of them fail, and the firmware falls
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
| ID8E | 52.88 ms | 12.25 ms | ~1535 (the VVDI's position), with the ID48 burst | ID8E (untested) |
| ID48 | 63.86 ms | 12.95 ms | ~1622 (the VVDI's position), with the ID48 burst | ID48 |

A burst at the wrong timing is harmless once the chip is freed: the VVDI keeps bursting after the
chip goes silent. The burst turns the EM emulation off. ID11 and ID12 would answer the handshake
30 ms after a 70 ms field-off, but ID13 goes silent and does not answer until it has been powered
down for longer, which is why the cold-boot is always made. Each attempt takes ~2.3 s whatever the
emulated type; up to three are made.

Normal session opens cycle four timings: the short 20 ms off / 3 ms power-up, the VVDI's 70.05 ms
off / 30.18 ms settle, 177.1 ms off / 41.8 ms on, and that again preceded by the VVDI's TI-style
probe. The second covers a chip left half-recovered
by an earlier failed run (its normal handshakes read 10 bits instead of 0). The third and fourth are for a chip
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

### ID8E uses the same burst

A chip emulating ID8E also broadcasts at power-up (MC64, repeating `8C6318FF…`, from ~950 T0) and
ignores XHORSE (`lf xhorse reset` received the broadcast as 101 edges on every attempt). In the
VVDI's capture (`vvdi-id8E-reset-success.pm3`) each round is: power-up, a burst, the field on 52.88 ms
in total, then **420 ms** off. The first two rounds send burst A (with the extra pulse at +68) 41.7 ms
after power-up, and the chip keeps broadcasting. The third sends burst B, pulse for pulse the ID48
table, **1535 T0** after power-up. After its 420 ms off the chip is silent and answers XHORSE 30.2 ms
after power-up. Our ID48 entry (~1622 T0) did not free it, so ID8E has its own entry at the VVDI's
position, ahead of ID48 so the hardware-confirmed ID48 entry still comes right before the cold-boot.

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

Before that, in every round, the VVDI sends a TI-style command: power-up, 50.04 ms charge, 100
field gaps ~23 T0 wide (gap starts 81 T0 apart, then 52 or 67 T0 in a fixed pattern, identical in
both captures), and field off 31.74 ms after the last gap. The session open's fourth timing replays
the last of these before the 177.1 ms off / 41.8 ms on. This is what opens an ID8A chip (hardware,
2026-10-16: session open on timing 3 after timings 0-2 failed). It also reverts the chip's type
byte to 0x36: `info` run through it read type 0x36 while bank 0 still held the ID8A image. So only
`reset` and `configure` send it; `info` and `dump` report an ID8A chip as not found. The Proxmark sniff of a failed reset
(`pm3-id8A-reset-fail.pm3`) shows the chip is not silent: after our XHORSE and 072F it load-modulates
weakly at the 32 T0 bit rate, starting ~80 T0 later than a normal reply, but it never sends the
5AA5 acknowledgement or answers 7372.

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

# XT136
^[Top](#top)

Decoded from VVDI captures (`docs/xt136.md` in the project folder). Same LFSR, same `op,~op` commands,
but its own handshake:

| | XT27A | XT136 |
|---|---|---|
| after `XHORSE`/`5AA5` | `072F` -> `C8F6B638` | `E20E` -> `604A027E` (same reply from two chips) |
| seed query | `7372` | `E430` |
| seed | `bitrev16(reply) ^ 0xE12E` | `bitrev16(reply) ^ 0x8C07` |
| first encrypted command | `sel bank 1` (4R) | none fixed |
| banks | 2 | none (`sel` never seen) |
| pages | 2 banks x 64 pages, all real (512 B) | **16 real pages** `0x00`-`0x0F` (64 B); `0x10`-`0x3F` alias them, decode is `page & 0x0F` |

An XT136 ignores `072F`, so the firmware tries every XT27A timing first, as the VVDI does, then the
XT136 handshake: field off 130.1 ms, `XHORSE` 220.3 ms after power-up (the VVDI's timing; the XT27A's
short power-up is tried in between), `E20E`, `E430` about 90 T0 after the signature, and 21.3 ms before
the first encrypted command. There is no 4R check; the exact 32-bit signature is what proves the
session, and a wrong seed shows up as reads that never agree.

The VVDI's reset image, pages `0x00`-`0x0C` written with `WRITE`, `0x0D`-`0x0F` with ctl 0x30-0x32:

| Page | Value | Meaning |
|---|---|---|
| `0x00` | `64B55D12` | UID |
| `0x01`/`0x02` | `4D494B52` / `00004F4E` | HITAG2 password `"MIKRON"` |
| `0x03` | `06AA4854` | HITAG2 config |
| `0x04`-`0x07` | `0` | HITAG2 user pages |
| `0x08`-`0x0C` | `00000018` `00282010` `00004141` `28148389` `55555555` | unknown |
| `0x0D`-`0x0F` | `0000000E` `361818CA` `56781234` | config words 0-2; word 1's **high byte is the emulated type byte** (`0x36` ID46, `0x46` 7946), confirmed from VVDI set captures (docs/xt136.md SS5a) |

`configure` and `reset` stop with "not supported" on an XT136 before anything is written, and before
the XT27A EM-recovery bursts, whose effect on an XT136 is unknown.

# Config words (ctl 0x30+n)
^[Top](#top)

On an XT136 this is how the emulated type is set: a VVDI "set" sends `ctl 0x31` with word 1's high
byte replaced by the type byte, captured in both directions (docs/xt136.md SS5a), then writes the
UID and the HITAG2 image in the same session.

`ctl 0x30+n` followed by a 32-bit data frame writes config word `n`; the chip echoes the ctl swapped
(`~op,op`), needs a program wait, and the word reads back with `READ` at bank-1 `0x3D+n` on the XT27A
(`0x0D+n` on the XT136). It does not clear the bank selection. `lf xhorse write -c <n> -d <hex>`
sends it; the program wait is 13 ms on an XT27A and 31 ms on an XT136 (`XH136_T_CTL_PROG_US`), the
XT136's own gap being 2.4x longer than its `WRITE` gap.

**Confirmed on hardware (2026-11-10).** Each write below was made with a changed value, read back
changed, and restored; every time exactly one page moved and the chip kept emulating, so a config
word is ordinary rewritable storage through `ctl`. A same-value rewrite (what the VVDI's ID8A
closing step does) also works and changes nothing.

| Chip | Word | Page | Change made | Effect on emulation |
|---|---|---|---|---|
| XT27A (7936) | 0 | `0x3D` | `0000020F` -> `0000000F` (clears bit `0x200`, the bit the VVDI's 7935 set toggles) | none |
| XT27A (7936) | 1 | `0x3E` | `F370CA18` -> `0370CA18` (the top nibble, the only part that differs between captured chips) | none |
| XT136 | 0 | `0x0D` | `0000000F` -> `0000000E` (the VVDI reset image's value) | none |
| XT136 | 2 | `0x0F` | `56781234` -> `56781235` (low bit) | none |
| XT136 | 1 | `0x0E` | `361818CA` -> `361818CB` (low bit; top byte held at `0x36`) | none |
| XT136 | 1 | `0x0E` | `361818CA` -> `371818CA` (**the high byte**, `0x36` -> `0x37`) | none observable - see below |

In each case the chip ID, the type byte and the HITAG2 UID were unchanged and `lf hitag info` still
read the emulated tag. **All three config words are writable through `ctl` on both families**, and
all six writes above were restored byte-exactly.

Two limits on what "no effect" means here:

* The XT27A was set to **7936** throughout, and it is the 7935 recipe that writes word 1. A field
  can be live for a type that uses it and ignored by one that does not.
* On the XT136, word 1's **high byte is writable** (`0x36` -> `0x37` took, channel A survived, the
  chip kept emulating, restored), but that does **not** show whether the byte is the type field.
  `lf search` says `PCF 7936` either way, and that string comes from bits 4-7 of the emulated *UID*
  (`getHitagTypeStr()`, `client/src/cmdlfhitag.c`), not from the config word; 7936 and 7937 are both
  HITAG2 besides. Proving it needs a high byte from another protocol family, which is the write with
  no way back on a chip with no `reset` recipe.

Unlike the XT27A, neither XT136 word offered an attested alternative value: both read identically on
the sample and in the VVDI reset image (§4.2), so every bit is constant across all observations and
the changes above are minimal single-bit ones rather than known-good states.

The VVDI uses it:

* at the end of a 7935 set: bank-1 `0x00`-`0x0F` cleared, word 1 (`0x3E`) = `F370CA18` (`0370CA18` on
  another chip), word 0 (`0x3D`) rewritten with bit `0x200` set (`409` -> `609`; `0002021F` kept).
  The 7935 recipe does this.
* at the end of an ID8A set: word 0 rewritten with the value just read (the ID8A recipe does this).
* in some older captures, the same unchanged rewrite after 4C/4D/4E sets; newer captures read
  `0x3D` and leave it, so those recipes don't.

# Channel B identify (XT27B / XT57B)
^[Top](#top)

As the VVDI reads an XT27B or XT57B:

```
field on, wait 120.14 ms
reader -> XMICRO (ASCII, 48 bits, 0x584D4943524F)    chip -> 0x5AA5
reader -> 0011 (4 bits)                              chip -> ASCII family, e.g. "XT37" / "XT57", + 1 byte
```

An XT27A in its normal state never answers XMICRO. Set to some types (7938 seen) it answers
XMICRO with something but sends nothing back to the identify query, so `lf xhorse info` only treats
a chip as channel B when the identify reply starts with the ASCII `XT`; anything else goes on to the
XHORSE handshake. The 120 ms wait matters: an XT57B answers an XMICRO sent 3 ms after
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
* Detecting the EM broadcast directly would save the failed normal handshakes before EM recovery: 12 attempts over four timings, about 2.5 s.
* The channel-B (XT27B / XT57B) management protocol.
* How the VVDI reports a chip's identity: the dongle's recognition packet carries `ID_TYPE` (the
  transponder family) and `SPECIFIC_ID` (the exact part). `SPECIFIC_ID`'s base chip is the emulated
  HITAG2 UID's type nibble - confirmed by changing the nibble on an XT136 and watching the app go
  PCF7936A -> PCF7946A (docs/xt136.md SS5a). The "A"-variant suffix comes from somewhere else and is
  decided in the dongle firmware, not the app.
* ~~XT136: where `0x0C` and `0x0D` come from.~~ **Resolved: they are factory values.** Seven unused
  chips all read `0x0C` = `3601C0AA`; `0x0D` is `0000000D` or `0000000F` (a batch marker), and
  nothing the app does writes either. Reset overwrites them with `55555555` / `0000000E`, so **a
  reset is not a return to factory state** - read a chip before resetting it. Still open: what the
  two values mean (docs/xt136.md SS5b).
* XT136: what pages `0x08`-`0x0C` hold, what word 0's low nibble selects, whether pages
  above `0x0F` exist, and whether the XT136 needs the XT27A's EM recovery when set to an EM type.
* Where the top nibble of the 7935 config word 1 (`F370CA18` / `0370CA18`) comes from. It is freely
  writable either way (measured 2026-11-10) and neither value affected a 7936 chip's emulation, so
  it is not a lock or a checksum over the rest of the word; whether it matters to a 7935-configured
  chip is still untested.

# Credits
^[Top](#top)

This would not have been possible without @CastleBravo, @HenryGab and @eltrick on the Proxmark3
Discord, and kivijakola's hitager project on GitHub.
