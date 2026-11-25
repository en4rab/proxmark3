 Change Log
All notable changes to this project will be documented in this file.
This project uses the changelog in accordance with [keepchangelog](http://keepachangelog.com/). Please use this to write notable changes, which is not the same as git commit log...

## [unreleased][unreleased]

### Added
 - **A chip blanked to 7935 can be freed again** (docs/xt57b-fullread.md SS12.11). A sniff of the
   VVDI resetting one (`data/traces/xt57b/xhorse-XT57b-reset-from-7935.pm3`) shows it is the same
   kind of back door the EM types have, with a **third word**: 58 gaps over 58.3 ms, cells of one,
   two or three 512 us units (65/129/193 T0), sent 18.76 ms after power-up, over the chip's own
   broadcast rather than before or after it. The word is
   `011120112011111112011201112011201210121011120112020202020`. One burst frees the chip and it
   stays free across later power cycles. `tools/analysis/burst7935.py` re-derives it from the
   trace and decodes it.
   `lf xhorse xt57 reset` now sends the "erase" word and then this one when `--word` is not given,
   because a stuck chip cannot be asked what it is emulating; `--word 7935` sends only this one.
   Verified end to end twice on an XT57B: wipe, deaf, `reset`, `restore`, `lf hitag info` shows
   the UID again - 17 s on the default path.
 - **`lf xhorse info` / `lf xhorse dump` take `--probe <a|b>`, and the VVDI's two ID8A probe words
   turn out to do different things** (docs/implementation-plan.md Updates 31-32,
   docs/xt57b-fullread.md SS12.4). A chip set to ID8A answers no handshake at any power-up delay,
   so a read reports it as not found; the only thing that reaches it is the VVDI's TI-style probe.
   `--probe b` sends the word the firmware already used, from a read instead of only from
   reset/configure, trading the chip's type byte for a dump. `--probe a` sends the VVDI's *other*
   word - the one it sends twice first - and then puts `XHORSE` to the chip, which the VVDI never
   does: after word A it only ever tries `XMICRO`, which an ID8A chip ignores in any state.
   Hardware, on an XT27A set to ID8A: word A **does** open a session (timing 3, 4R echo `41be`),
   but bank-1 0x3C then reads **0x00** where word B leaves **0x36** - three runs each against a
   `configure` that had just verified 0x8A. So word A is not a non-destructive read, and the two
   words are not interchangeable wake patterns: they carry different payloads. `--probe b` stays
   the one to use (0x36 is a valid type, 0x00 is not), and reads send no probe by default.
 - **`--probe-word` / `--probe-type`, and the ID8A probe word turns out to be parity-checked**
   (docs/implementation-plan.md Updates 33-34, docs/xt57b-fullread.md SS12.4). Aligning words A and
   B against their known outcomes gives one candidate - bits 17..24 read 0x36 in B and 0x00 in A,
   the two type bytes exactly - but nothing in the traces can confirm it, so `lf xhorse info|dump`
   gained `--probe-word <98 S/L>` to send an arbitrary word and `--probe-type <hex>` to rewrite
   just those eight symbols of word B. `--probe-type 36` rebuilds word B to the symbol and opens
   the chip, as the control. The candidate is **falsified**: rewriting bits 17..24 makes the chip
   ignore the word rather than write the byte. `tools/analysis/probe8a_sweep.py` then swept all 98
   single-symbol flips against hardware (`data/probe8a-sweep.jsonl`): **97 are refused**, including
   every flip inside the long run of S - so none of the word is padding - and the one exception is
   symbol 5, which is accepted and leaves the type byte at 0x00. One rule covers every point:
   the check is on the symbol values rather than timing - A and B differ in total duration and both
   work - and 0x00 is just "passed the check but is not word B" (word A and B-with-symbol-5-flipped
   both land there) while 0x36 is what word B writes. `--flips` then takes arbitrary flip sets, and 52 more
   points (`data/probe8a-pairs.jsonl`, `-pos5`, `-cut`, `-header`) first rule out a checksum - six
   different double flips away from symbol 5 are refused, which any equal-weight checksum would
   wave through, while 5+40 and 5+60 are accepted - and then give the structure: **the chip reads
   thirteen of the 98 symbols.** Symbols 0..14 are a fixed preamble, ignoring symbols 5 and 9
   inside it; symbol 5 is a one-bit command selector. With `L` there the header alone is acted on
   and symbols 15..97 are ignored outright (the header with the tail all `S`, and with it all `L`,
   are both accepted); with `S` there the 83 remaining symbols are a payload and exactly two are
   accepted, word A's tail and word B's tail, with no tolerance anywhere in them - which is why
   replay is the only way in. The rule reproduces all 151 hardware points over 142 distinct words:
   21 of them open the chip and leave 0x00, and exactly one leaves 0x36, which is word B. A
   rejected word leaves the chip untouched and deaf, and every accepted word leaves bank 0
   byte-identical - the emulated type byte is the only casualty.
 - **The ID8A probe analysed: three words in one code, none of which decode**
   (docs/xt57b-fullread.md SS12.4, `tools/analysis/probe8a.py`). The TI-style command the VVDI
   must send before it can open a session with a chip set to ID8A is a binary pulse-distance code
   - constant ~184 us gap, intervals of 416 us (S) or 536 us (L) - and the gap width carries
   nothing, so `xh_id8a_probe` is 98 bits. Its envelope is TI half-duplex: the 50.04 ms charge is
   exactly the 50 ms Proxmark's own `WriteTItag` uses, and the long field-off afterwards is when
   an HDX tag answers. New: the VVDI sends **three** words in this code, not one. An 18-symbol
   word opens both captures and is also the entire XT136 reset preamble (`xt136.md` SS4.2); then a
   second 98-symbol word goes out twice, each time followed by an XMICRO that gets no answer; then
   the stored word six times, after which XHORSE reaches the chip - the same first-word /
   last-word shape as the Megamos family. The second word is now written down for the first time
   (Update 29 named it only "probe variant A"). None of the three decodes: no ASCII under any
   expansion of S/L into 0-3 bits, no CRC-16 across 27,888 framings per word, not TI's write
   encoding, and no all-zero address field. All are fixed constants, identical across every
   capture, so they are not DST challenges - which is why replaying them works.
 - **The 7935 back-door word decoded: it is a PCF7931 password frame** (docs/xt57b-fullread.md
   SS12.11). Unlike the other two words it is not an opaque constant. The PCF7931 reader downlink
   is pulse-position - a 128 T0 bit cell with one gap in it, at 34 T0 for a `1` and 98 T0 for a
   `0` (`armsrc/pcf7931.c`) - so gap to gap gives 64, 128 or 192, which is the measured 65, 129,
   193. The model predicts that the two transition symbols must strictly alternate, and across 29
   of them they do, without exception. That makes the 57 symbols 58 bits, which is exactly
   PCF7931's `1 password-indication bit + 56-bit password + 1 programming-mode bit` - and then the
   frame stops, with none of the address, data or parity bits a write would carry. So the VVDI is
   not writing anything; it presents a magic password, `10 01 22 44 66 88 AA` read LSB-first per
   byte, to a PCF7935 - the part the chip has been told to be. Polarity and byte framing each
   carry a caveat, documented in SS12.11; the firmware replays the measured symbols, so nothing
   about what goes on air changes.
 - **Why the Megamos word does not decode** (docs/xt57b-fullread.md SS12.4), re-measured from the
   trace rather than from the stored constant. Its gaps are constant and the on-time carries the
   symbol: cells of 33, 63 and 95 T0, i.e. one to three periods of ~32 T0, which is the EM4x70
   reader link. Expanded on that rule the 46 symbols give 74 bits, and there it fails - the
   command nibble reads `1100`, which is none of the six EM4x70 commands, and 74 matches no
   EM4x70 frame length (6, 36, 70, 97). The pulse-position reading that works for the 7935 word is
   ruled out outright: 18 repeats among 28 transition symbols. Also noted there: the shorter
   bursts in the same captures (49 gaps, 8.9 ms) are not a third word but the ASCII probe
   `XMICRO`, and the 33-gap ones are session frames.
 - **181 exchanges of XMICRO keystream at nonce `CDB9AE8C`**, up from 23
   (docs/xt57b-fullread.md SS12.9). The long session in `vvdi-xt157b-set-id33.pm3` turned out to
   run at `CDB9AE8C` itself, not at the nonce the docs named, and its first 23 exchanges match the
   table already in hand - so its later indices were reachable by the method that gave the first
   ones. The recorded wire bytes fix `k0(i) & 0xF8` at every index because the plaintext opcode
   family follows from the frame length; sending `(k0 & 0xF8) ^ x` is then always a read, and
   sweeping `x` over 0..7 solves for the low three bits of `k0(i)` and for `K(i)` against the
   eight known words of the block the session starts in. 181 of 184 indices solved, none
   ambiguous, all 23 previously known reproduced exactly. Table in
   `data/traces/xt57b/keystream/cdb9ae8c.txt`.
 - `lf xhorse xt57 set -t 7935` - the ID33 / ID40..ID44 personality, which decrypting that session
   shows is **not an image write but a wipe**: `7F` page 4 `<- 00000035`, then all ten addresses of
   the emulated store filled (`5A5A0000`, `5A5A5A5A`, `00000000`, with `5A5A0707` at one slot).
   182 exchanges do not fit one call - 560 script bytes and 64 responses per reply - so the
   builder emits chunk marks and the runner splits the plan into five sessions at the same
   power-up delay. 84 of 84 writes read back and verified on an XT57B.
   **It needs `--wipe`**, because it overwrites the factory tables at addresses `00`, `01`, `03`,
   `04`, `08` and `09` and nothing puts them back - not `lf xhorse xt57 restore`, and not the
   VVDI's own reset, whose write session is seven configuration writes rather than an image. The
   chip also goes deaf to XMICRO afterwards, but that part is recoverable (SS12.11); reading the
   ten addresses first, 90 read-only exchanges in one session, is the only way to keep them.
 - **The whole emulated store is channel A's two banks.** Addresses `00`-`07` of the XMICRO
   session are bank 0's pages `0x00`-`0x3F` and `08`-`09` are bank 1's `0x00`-`0x0F`, matching
   `xh_recipe()`'s `XHORSE_R_7935` case page for page including the odd one at `0x1F`. SS12.8 had
   shown this for the image block; it holds for the whole store.
 - `lf xhorse xt57 reset` takes `--at`, `--step`, `-n`, `--probe`, `--on` and `--word`, so the
   burst can be walked over a frame as long as the emulated type makes it - the default +-640 us
   suits ID11's 256 us frame, and the 7935 word steps 128 us so 32 positions walk one whole
   4096 us frame.
 - `lf xhorse xt57 nonce` takes `--powerup` (set the field-on delay while another gap is swept)
   and `--presson` (send the select even when the identify went unanswered).
 - `lf xhorse xt57 set` - **set which transponder an XT57B emulates**, the channel-B counterpart of
   `lf xhorse configure` (docs/xt57b-fullread.md SS12.8). Takes `-t <type>` and `-u <uid>`, builds
   the VVDI's own set session from a recovered keystream rather than replaying it, and reads every
   write back. Types: `ID46`/`7936`/`7937`/`7946` (4-byte UID, 21 exchanges), `ID11`/`ID12`
   (8-byte, 23) and `ID13` (5-byte, 23). Anything else is refused with its reason - `7935` wipes
   64 pages, `ID48` needs 16, the Texas family writes bank 1 - none of which fit the 23 exchanges
   of keystream recovered at the time. (`7935` was added once the keystream reached 181; see the
   entry above.)
   The decrypted set captures show the session is **the reset's two halves in the other order**:
   the eleven-exchange `7F` config block, identical but for the type byte, and the block `00`
   image, one write and one read-back per page. A broadcast type gets the image first, which is
   what the VVDI does. The ID11 session never selects a block before its first write, so a session
   starts on block `00`.
   **The image block is the channel-A bank 0.** The ID11 half writes pages `2, 1, 4, 3, 0` - the
   same pages in the same order as `xh_recipe()`'s ID11 case, derived from XT27A captures that
   know nothing about XMICRO - and the ID46 half is that function's HITAG2 case. So `xh_bitrev8()`,
   `xh_pack_em8()` and `xh_id13_stream()` move from `armsrc/xhorse.c` to `include/xhorse.h` and
   both channels build their images with the same code.
   Measured on an XT57B, every write read back and compared: `-t 7946 -u DE269A2F` 8/8, then
   `lf search` -> `PCF 7946` / `DE269A2F`; `-t ID13 -u 20471BB01B` 9/9, then `lf em 410x reader`
   -> `04E2D80DD8`; `-t ID11` and `-t ID12` 9/9 each, both broadcasting stable UID-derived
   patterns that differ between the types. `lf xhorse xt57 reset` + `restore` recovers from each.
 - **ID13's `--uid` is not the ID a reader sees.** `-t ID13 -u 20471BB01B` writes the four words
   `docs/xt27a-datasheet.md` SS11.4 records from the VVDI's own captures for that UID, and on air
   it reads as EM410x `04E2D80DD8` - the per-byte bit-reverse. Feeding `04E2D80DD8` in gives
   `20471BB01B` on air. The encoding bit-reverses each byte going in, so the VVDI's user-ID field
   and the EM410x ID are bit-reverses of each other. Both `set` and `configure` keep the VVDI's
   convention so they agree; the help text now says so. This is the air side of SS11.4, which had
   only ever been checked against the on-chip words.
 - **Retraction: writing a broadcast type byte does not kill the session that writes it.** The
   previous entry said the chip "stops answering in the middle of the session that wrote it".
   A built set session reads back all four writes that follow `7F` page 4 `<- 01F0008C`, so the
   chip answers XMICRO to the end; it is only from the **next power-up** that it talks first and
   goes quiet. The replay that suggested otherwise was failing for some other reason. What stands
   unchanged: the type byte is the whole arming mechanism, and the burst rewrites it.
 - **Nonces are not equally reachable, and `restore` was pinned to a bad one.** The nonce a
   power-up delay lands on drifts by a bit either way, so some sit on a plateau and some are hit
   only now and then: stepping 2 us across 119500-119738, `CDB9AE8C` holds for about 10 us while
   `6DCD7463` is interleaved with both its neighbours and turns up in roughly one sample in forty.
   A session that misses its nonce silently does nothing, and `restore` - pinned to `6DCD7463`
   because that is the nonce the VVDI's reset was recorded at - failed one run in three. Since a
   built session is not tied to the nonce it was recorded at, `set` and `restore` now try each
   recovered nonce in turn and run at the first one they land on, and both search with a 2 us step
   instead of 8. `restore` keeps its behaviour otherwise and is now `set -t 7936` internally.

 - **What arms an XT57B emulation is the type byte, and nothing else** (docs/xt57b-fullread.md
   SS12.7), from decrypting the VVDI's "set transponder" sessions. `vvdi-xt157b-set-id11-*.pm3`
   runs under nonce `CDB9AE8C`, whose keystream SS4 already gives, so it decrypts directly: it
   writes the emulated image to block `00` first, then `7F` page 4 `<- 01F0008C`, page 5
   `<- 00000049`, page 6 `<- 0000C900` then `00000000` - the same `7F` writes the reset makes,
   differing only in the type byte (`8C` = ID11, `36` = 7936/ID46).
   Measured by replaying that session onto a chip: writing `7F` page 4 with a broadcast code
   **arms the emulation** - the VVDI writes the image first and the type word last, and the reset
   can safely write its non-broadcast `0x36` first. (This entry originally said the chip stops
   answering in the middle of the session that wrote it; see the retraction above - it answers to
   the end and goes quiet from the next power-up.) And the `erase` burst **rewrites that
   byte**: a chip armed with `01F0008C` reads back `01F00046` after `lf xhorse xt57 reset`, with
   the emulated image untouched. So `erase` clears the type selection, not the memory.
   **Retracts** the previous entry's conclusion that the enable was not among the pages the reset
   writes. That test wrote back values read off the chip *after* the burst had neutralised them:
   `01F00046` is not ID13's configuration, it is what the burst leaves behind - `0x46` is **7946**,
   which does not talk first, while ID11, ID12 and ID13 all share `0x8C`.
 - `0x33`, a fifth opcode, and a **negative result** on it. It is the only command that appears in
   a set-to-ID11 session and not in a set-to-ID46 one (index 23, after the `7F` terminator), which
   made it the obvious arming candidate; replaying the set session without it arms the chip just
   the same, so it is not. Found by extending the `CDB9AE8C` keystream past index 20 - 21 `k0 3C`
   / `K FEB31762`, 22 `82`/`60694C6A`, 23 `2C`/`91130817` - giving plaintexts `06`, `7F`, `33`.
 - **Full cycle tested end to end**: a chip set to ID11 by replaying the VVDI's set session stops
   answering XMICRO; `lf xhorse xt57 reset` frees it (via the sweep, -160 us off the VVDI's
   offset); `lf xhorse xt57 restore` puts back `01F00036` and the factory image; `lf search` then
   reports `PCF 7936` / `Hitag 2` with the UID written.

 - `lf xhorse xt57 restore` - **rewrite an XT57B's factory ID46 personality**, the half of the
   VVDI's reset that `lf xhorse xt57 reset` does not do. This is the first command in the project
   that writes to a chip.
   The frames are **generated, not replayed**: each is built from its plaintext opcode and value,
   XORed with the recovered `k0(i)`/`K(i)` for nonce `6DCD7463`, with the CRC-8 taken over the
   resulting ciphertext (SS5). That is what lets `--uid` choose the emulated UID while every other
   write keeps the VVDI's value; with no `--uid` one is generated, as the VVDI does.
   The keystream is nonce-specific, so the session must land on `6DCD7463`, and the power-up delay
   that produces it differs per chip (119322 us on tag C, 119536-119544 on tag I). The command
   searches for the nonce first, then runs the script at the delay that found it; `--base` and
   `--span` narrow the search. Each write carries the VVDI's own read-back, with an explicit
   2000 T0 gap for the ~16 ms programming time, and the command decrypts and compares it, so it
   reports what is on the chip rather than what it sent.
   **Tested end to end** on tag I, configured to ID13 and freed: `7F` page 4 `01F00046` ->
   `01F00036` (type byte 46 -> 36, ID13 -> ID46), page 5 `0000004B` -> `00000049`, block `00`
   pages 0-3 -> UID + `MIKRON` + HITAG2 config. All 8 read-backs matched, a second session under a
   *different* nonce and keystream (`CDB9AE8C`) confirmed the same values independently, and the
   chip then answered `lf hitag info` as a HITAG2 with the UID written - with a pinned UID
   (`37786F1F`) and with a generated one (`7CD3F292`).
   It runs backwards too - see the next entry, which **retracts** an earlier claim here that it
   did not.

 - **All three sessions of the XT57B reset are decrypted**, in all three traces
   (docs/xt57b-fullread.md SS12.6). The reset makes three sessions, not one: a one-frame probe, a
   session that reads the chip's identity, and the session that writes.
   Session 2 runs under a second nonce, **`36E6BA31`** (answer `C64A3095`, reply
   `703C397FC27ADBF5`, stream offset 38), whose keystream is recovered for indices 1-20 by the
   same method. It selects address `7E` and reads pages 4-7, then selects `21` and reads page 7 -
   i.e. the VVDI reads the three identity words before it writes anything. The select echo bytes
   decrypt to `7E` and `21`, matching the command bytes, which is an independent check that did
   not go into the solve.
   The identity words read there, `59A41534 4146EDE2 99A9011D`, are **tag I's**, so the reset
   captures are tag I being recovered - the chip SS11 recorded as configured and then reset.
 - **A fourth opcode, `7F`** (docs/xt57b-fullread.md SS5, SS12.6). It ends every one of the three
   sessions, under two different nonces and with three different wire bytes (`4C`, `68`, `A0`),
   takes no operand and is never acknowledged. Finding it needed a different method, since with no
   reply and no opcode family the trace pins nothing: fill the earlier indices with real reads and
   try all 32 values of `k0(i) & 0xF8` at the index in question - exactly one makes it a valid
   read and draws a reply.
 - **The emulated UID is generated fresh for each reset.** The three traces write `37786F1F`,
   `A5F9191A` and `FAF72412` to block `00` page 0 while everything else they write is identical,
   so it is neither a fixed factory default nor the chip's own identity (tag I's dump has
   `A50B2310` there). Noted in SS12.5, which previously implied a fixed default.
   The id12 trace puts the two nonces on the opposite sessions from id11 and id13, so its writes
   decrypt under the *other* keystream to the same plaintext - the strongest available check that
   the recovery is right.

 - **The XT57B reset's writes are decrypted** (docs/xt57b-fullread.md SS12.5), closing the last
   open item on the reset. There are **8** of them, not the 11 an earlier draft counted, under
   session nonce `6DCD7463`.
   The keystream fell out without any guessing: the trace itself pins `k0(i) & 0xF8` at every
   index, because a read's plaintext opcode is `00000AAA`, a write's `10000AAA` and a select's
   `11011SSS`. A byte carrying those five bits therefore decrypts to *some* valid page read at
   that index whatever its low three bits are, so sweeping the low three bits across eight
   sessions gives eight replies per index; matching them against pages 0-7 read under `CDB9AE8C`
   (the known keystream) solves the low three bits of `k0(i)` and all 32 bits of `K(i)`. All 20
   indices resolved uniquely. Read-only throughout - every frame sent was an 8-bit read.
   Checks: all 20 frames decrypt to correctly formed opcodes of their own family, and all 8
   write/read-back pairs name the same page.
   What the reset writes: `7F` page 4 <- `01F00036` (low byte `36` = the ID46/7936 type byte),
   `7F` page 5 <- `00000049` (the constant SS11.3 found in every used chip and could not explain -
   so it is written by the reset, not a serial), `7F` page 6 <- `0000C900` then `00000000`, and
   block `00` pages 0-3 <- `37786F1F 4D494B52 00004F4E 06AA4854`. The last three are ASCII
   `MIKRON` plus the HITAG2 config word and are byte-identical to what an XT27A carries after
   `lf xhorse reset`, so the factory ID46 image is shared across the two families.
   This turns SS11's before/after inference into measurement.
   Tools: `data/traces/xt57b/nonce-captures/ks6dcd7463-{step1,step2,solve,decrypt}.py`.
 - `lf xhorse xt57 nonce` - nonce `6DCD7463` added to the known table (answer `519F0319`, reply
   `6B6E92C691C91277`) and `xt57_stream` extended by 3 bits to 71. It is the reset session's
   nonce and sits at stream offset 39, immediately after `CDB9AE8C`; the client now reports
   "reply matches" for it. Reachable on the bench at powerup 119544 us with `--off 45000`.

### Fixed
 - The back-door burst emitter accumulated every edge against one 16-bit precision-counter
   reference, which is fine for the 16 ms "erase" word and the 19.6 ms Megamos one but overflows
   on the 58.3 ms 7935 word (7290 T0 = 87,480 ticks). It now rebases the reference every 4000 T0,
   which is exact modular arithmetic and leaves the two short words' timing unchanged.
 - `docs/xt57b-fullread.md` SS12.10 concluded that a chip blanked to 7935 could not be brought
   back, and that the burst's phase was not what was missing. The second half was right and the
   first was wrong for the same reason: the word was. SS12.11 has the trace and the recovery, and
   SS12.10 is kept as the record of what a wrong word costs.
 - `lf xhorse xt57 nonce` reported an XMICRO ack for **any** 16 demodulated bits. A chip emulating
   a talk-first type modulates all the way through the window and reads back as `5555`, which
   looked like a healthy handshake at delays where nothing was listening. The flag now means the
   `5AA5` ack and the table prints what came back instead.
 - `docs/xt57b-fullread.md` SS12.8 said the long `set-id33` session was "183 exchanges under nonce
   `1FF1A845`, for which nothing is recovered". `1FF1A845` is an answer, not a nonce; the session
   is 184 reader frames at `CDB9AE8C`; and 23 exchanges of its keystream were already in hand.
 - `docs/xt57b-fullread.md` SS5 - **the write frame's CRC-8 is over the ciphertext, not the
   plaintext**. Poly `0x07`, init 0, computed over the five encrypted bytes, so it verifies on the
   wire without the key: all eight writes in SS12.5 check that way and none checks the other way.
   SS5 had described it as a CRC over the plaintext, which was inference from a frame this project
   had never sent. Also noted there: the 40-bit keystream block covers the opcode and four data
   bytes exactly, with the CRC outside it, so a write is the one command that uses the whole block.
 - `docs/xt57b-fullread.md` SS4 - **retracted: there is no 20-exchange limit.** An earlier entry
   here claimed the session supplies exactly 20 keystream blocks, because the trace's 21st frame
   drew no reply and a 21-frame probe stopped at 20. Both observations have a different cause:
   frame 21 is the `7F` command (SS12.6), which never draws a reply, so the probe byte built for
   it was not a read either. The right byte gets an answer at index 21 at once, and index 21's
   keystream is now in SS12.5's table. SS4's table stopping at 20 is how far it was measured, not
   a limit of the chip.
 - `lf xhorse xt57 reset` - **free an XT57B stuck emulating a broadcast transponder type**. Such a
   chip transmits continuously and answers nothing on XMICRO, so none of the other xt57 commands
   reach it; there was no recovery for it at all before. Sends the VVDI's exit-emulation
   choreography, measured from `xhorse-XT57b-reset-from-id11/-id12/-id13.pm3` and identical in all
   three (docs/xt57b-fullread.md SS12.1): two rounds of four downlink bursts, each in its own
   power cycle (field off 70 ms, on, burst at +109.7 ms, window ends at +156 ms), a 568 ms drop
   and an XMICRO probe after each round, 228 ms between rounds. The burst is the same generated
   `"erase"` word the XT27A recovery sends - one emitter, shared - because the emulation core
   listens for it whatever family the chip is.
   The VVDI's own offset is tried first; if that fails the burst is walked outwards from it in
   5 T0 steps (+-640 us, five ID11 frames), probing every 8 positions. **That is the normal path,
   not the fallback**: an XT57B set to ID11 is not freed by the VVDI's offset at all, on any
   retry - the same phase problem the XT27A's ID12 showed. The reply carries how far off the
   nominal offset the freeing burst was, so the constant can be retuned.
   `--force` sends the bursts even when the chip already answers, which is how the sequence is
   exercised on a healthy chip.
   **Scope**: this restores the channel, not the factory personality. The VVDI follows the burst
   with 11 encrypted page writes under session nonce `6DCD7463`, whose keystream is not recovered
   (SS12.5), and those are what make its reset permanent.
   **Tested end to end** on one XT57B put into each of ID11, ID12 and ID13 with a VVDI. Each time:
   confirmed stuck (no XMICRO), freed, answering as `XT57X` and reachable by `lf xhorse info`
   afterwards, and idempotent on a re-run ("not stuck").

   | from | freed in | by |
   |------|----------|----|
   | ID11 | 8.4 s | the sweep - the VVDI's offset failed 3/3 |
   | ID12 | 4.3 s | the VVDI's offset, second round |
   | ID13 | 2.4 s | the VVDI's offset, first round |

   Note this is the **opposite** of the XT27A, where ID11 works on the nominal offset and ID12
   needs the sweep. So the offset is not a property of the emulated type: the same type lands on
   one chip and misses on another. It depends on the individual chip and the reader, which is why
   both commands sweep rather than carrying a per-type constant.

 - `lf xhorse xt57 nonce --post/--want` - send scripted reader frames after an authenticated XT57B
   session and report each response with its CRC-8, so a recorded session can be replayed or rebuilt
   frame by frame (@claude)
 - `lf xhorse xt57 nonce --post` now carries a packed script instead of a 24 frame array, so a whole
   VVDI session (183 frames) fits in one command; `--rfirst <n>` picks which 64 responses are
   reported, and a long session is read out in a few passes (@claude)
 - `lf xhorse xt57 nonce --selbits <n>` - send a select frame shorter than 13 bytes, so the
   authenticate position can be probed with another command set's frames; a `--noanswer`
   session may now also carry a `--post` script, for probing a mode that has no auth (@claude)
 - `lf xhorse info` - prints the raw channel-B identify reply, not just the four ASCII family bytes;
   a chip whose model word has been altered shows it in the fifth byte (@claude)

### Fixed
 - `lf xhorse reset` / `lf autel` - **the emulation-exit burst is now generated, not replayed**.
   It was two tables of 43 and 47 measured pulse edges per chip family, copied into both
   `armsrc/xhorse.c` and `armsrc/autel.c`, with a comment saying what it encoded was unknown.
   It is a plain 125 kHz reader-to-tag downlink (docs/xt57b-fullread.md SS12.2): a start gap,
   then one cell per symbol, each a field-ON time carrying the symbol followed by a gap. The EM
   word that frees ID11/ID12/ID13 is 42 bits - a 2-bit preamble and a 40-bit value sent
   least-significant-bit first, `0x6572617365`, the ASCII word **"erase"** - so it is now emitted
   from that constant rather than from 86 numbers. The Megamos word (ID48/ID8E) is still
   unexplained, so it is kept verbatim, but as 46 symbols instead of 94 edge positions. autel.c's
   duplicate copy is gone; it calls `Xhorse_EmBurst()` (new, `armsrc/xhorse.h`).
   Tested on an XT27A: configure to ID11, ID12, ID13, ID48 and ID8E in turn and recover each one.
   All five round-trip.
 - `lf xhorse reset` - **phase sweep when the first pass fails**. The burst only frees the chip
   if it lands at the right point in the chip's broadcast frame, and the frame period differs per
   type (ID11 RF/32, ID12 RF/40, ID13 RF/64), so one fixed offset can simply miss: an XT27A put
   into ID12 here was not recovered by the nominal offsets - not by the new code and **not by the
   old table-based code either**, so this is a pre-existing gap, not a regression. The first
   attempt still uses the nominal offsets only, so the common case is as quick as before; later
   attempts slip the burst in 5 T0 steps across a whole 40 T0 frame. That recovers the ID12 chip,
   and ID11/ID12/ID13/ID48/ID8E now all round-trip repeatably.
 - `docs/xt57b-fullread.md` SS12 - **how the VVDI frees an XT57B stuck in a broadcast emulation
   mode**, from `xhorse-XT57b-reset-from-id11/-id12/-id13.pm3`. It is not a command on `XMICRO`
   and needs no key, session or nonce: it is a fixed burst of 43 field dips, sent at +109.7 ms
   into a 156 ms field-on window, 8 times in two rounds of four (226 ms apart inside a round,
   1187 ms between rounds) with 70 ms power-cycles between (the fourth drop of a round 570 ms),
   after which `XMICRO` is answered normally. **The burst decodes.** Split into gaps and field-on
   times rather than measured edge to edge, it is an ordinary 125 kHz reader-to-tag downlink on
   the T5577/ATA5577 physical layer - 43 gaps of 16-22 T0 and 42 on-times of either 14-17 or
   43-46 T0, which are Proxmark's own `T55xx_Timing` defaults (write gap 17, bit 0 = 15,
   bit 1 = 47). That makes it 42 bits: a 2-bit preamble `00` and a 40-bit payload sent
   least-significant-bit first, `0x6572617365` - the ASCII word **`erase`**. The competing
   EM4x05 forward-link reading (a `1` is *no* gap) is ruled out: it would make the burst 63 bits
   with no two adjacent `1`s. Identical in **120 bursts over 11 traces**, the three XT57B
   captures and eight year-older XT27A ones, settling an open question from
   `implementation-plan.md` Update 14 (ID13 shares the ID11/ID12 burst). Activity drops 43 -> 4
   across the first burst on ID11 and 40 -> 5 on ID13; ID12 is not silenced by the first burst,
   the known phase sensitivity. With no tag on the pad no downlink burst is sent at all. New
   tool: `tools/analysis/burstdecode.py`; also `tools/analysis/emburst.py`,
   `tools/analysis/emsilence.py`.
   Corrects two things in the first draft of SS12: the burst is sent **eight** times, not four,
   and the ID48/ID8E word is **not** 47 binary intervals - its on-times take three values (~16,
   ~45, ~80 T0, cells of 33/65/100), so it is not a two-symbol frame. A reset round sends two
   such words, differing by one gap, and only the second frees the chip:
   `00100100111101010000111021001112000111000201002` goes out twice and does not,
   `0200100111101010000111021001112000111000201002` goes out last and does - confirmed on an
   XT27A. Neither decodes to ASCII under any expansion of the three symbols into bit strings of
   length 0-3, either polarity, either bit order, any preamble up to 12 - still open.
   Also still open: the 11 writes that make the reset permanent are under session nonce
   `6DCD7463` and undecrypted, but the trace carries the VVDI's answer `519F0319` for it, so they
   are recoverable.
 - `docs/xt57b-fullread.md` SS11 - **a ninth chip (I), and a correction**. Tag I first read as an
   **EM4x70** with `lf hitag info` silent and every XMICRO session returning *no ack*: it had been
   configured to emulate another transponder family, and answered normally after a reset. So the
   XT57B's car-facing personality spans families, not just HITAG2/ID46, and the tool-facing
   channel is unreachable while it is set up as one of the others - a chip that does not answer
   `XMICRO` has not necessarily failed. Its identity words survived the round trip intact.
   **Correction:** SS11.3 called `49` at `7F` page 5 a collision between A and E and concluded the
   field could not be a serial. That compared a used chip's overwritten word with a fresh chip's.
   Both used chips (A and I) read exactly `00000049`, so that is a constant written by
   configuration; across the seven chips still holding the factory value, all seven bytes are
   **distinct**. The field is now undetermined - consistent with a per-chip value and with a batch
   code - rather than ruled out. The conclusion on the ten identity bytes is unchanged: distinct on
   all nine, no structure, not a readable serial.
 - `docs/xt57b-fullread.md` - **page existence is now proven, not assumed: 280 of 288 exist**.
   The forward/backward pass cannot decide the interior of an all-zero block, where a latched zero
   and a stored zero are the same byte; the earlier note's "on the other 34 blocks every page is
   real" overstated what it had measured. 203 pages were settled by the two passes across eight
   chips, leaving 77. Those were settled by priming the latch - read a known non-zero page, select
   the block, read the page under test first: the primer coming back means the page does not
   exist. All 77 are real, with four controls (two known-real, two known-absent) correct in the
   same run. The absent set is exactly `7E` pages 0-3 and `7F` pages 0-3, so the chip holds 1120
   bytes of real storage in 1152 bytes of address space. New: `exists.py`, `pageprobe.py`,
   `pageprobe-C.json`.
 - `docs/xt57b-fullread.md` SS11 - **eight chips read and compared; there is no readable serial
   number**. Seven factory-fresh XT57Bs plus the one programmed chip, each read forwards and
   backwards. Among the seven fresh chips **9 of 280 existing pages differ**: the emulated HITAG2
   UID at `00` page 0, three identity words at `7E` pages 4-6, and one byte at `7F` page 5 (the
   last four mirrored at `21` pages 2-4 and `20` page 1). Everything else - every factory table,
   the `2600CC57...` block at `04`, the fixed table at `20` - is byte-identical. The ten identity
   bytes are distinct on all eight and show no structure: no shared prefix, no ordering, no
   low-entropy field, bit balance where eight random samples would sit. `7E` page 6 ends `011D` on
   all eight, so it is 10 bytes plus a type field. The `7F` byte **collides** (A and E both `49`),
   so it is not a serial either, and it is not a checksum over the identity or the UID under sum,
   XOR or CRC-8 in either byte order. Per-chip reach parameters recorded too: the power-up base
   for nonce `CDB9AE8C`, and that tag H needs a 100 ms field-off where 45 ms suffices for the rest.
   Tag C was re-read directly rather than converted from its pre-SS18.10 dump; the two agree on
   all 288 pages, an independent check on SS18.10's `page = slot ^ 4`. Artefacts:
   `full<A-H>.json`, `full<A-H>-rev.json`, `cmp.py`, `corpus.txt`.
 - `data/traces/xt57b/store-dump/fullread.py` - **a page that does not exist returns the previous
   read's value**, not zero; a select does not clear the latch. Only blocks `7E` and `7F` have
   absent pages (0-3), so the common area is two half blocks of 16 bytes. Found when tag B showed
   its UID four times where tag C had shown zeros - both artefacts of the preceding read. The
   script now takes `--rev` (read pages 7-0; a page disagreeing with the forward pass does not
   exist), `--find` (sweep the power-up delay for the wanted nonce), `--base`, `--off` and `--uid`.
   `docs/xt57b-fullread.md` and SS18.9 corrected.
 - `docs/xt57b-fullread.md` (new) - **a complete, standalone read-out of the chip**: the session
   shape and handshake, the per-exchange 40-bit keystream and how it covers each frame type, the
   three Hitag Pro commands with their plaintext encodings, the 8-bit `segment:block` address space
   with its ignored alias bit, the full 36-block dump with all eight pages of each, an annotated
   20-exchange transcript decrypted frame by frame, and what is still unknown. Written for a reader
   with no prior knowledge of the XT57B. Read-only: 8-bit page reads and 16-bit selects only.
 - `docs/xt57-auth.md` SS18.10 - **`OPC = 0x04`, not `0x00`**. SS13.3 and SS17.4 left the low three
   bits of the command-keystream byte an explicit free choice and took `0`; the full read measures
   them three independent ways - the decrypted echo is the address exactly, the factory counter
   tables ascend in page order, and the VVDI's HITAG2 image lands at page *n* for HITAG2 page *n*.
   Two of the three measure the bits through the read's page field and one through the select's
   segment field, and they agree. Every `k0` is XOR 4 (`k0(12) = 7F`, `k0(2) = A4`), the index-12
   select mask is `7F1E`, and `byte0 & 7` is now the segment number directly. The common area of
   SS18.9 relabels to segment 3, addresses `7E`/`7F`, which is the form the VVDI sends; its select
   of the main store decrypts to `D800`, the block the transponder image is written to and the
   power-on default. No measured value moves - `K`, the data and the address map are unaffected.
 - `docs/xt57-auth.md` SS18.9 - **the common area at `FE` and `FF` dumped**. Segment 7 blocks
   `1E`/`1F`, canonical selects `A400`/`A401`, read live on tag C: three used words each and five
   zero, eight slots being the whole block since the read opcode `00000AAA` has a three-bit page
   field. It is SS16.4's identity group - what SS15.3's labelling called `0:00` and `0:01` - so the
   ten per-chip bytes with the constant `011D` tail live in the common area, not in a segment. That
   also explains SS16.4's aliases: old `2:1E`/`2:1F` are the two blocks of segment 1, and a read
   there runs past them into the common area, wrapping the page field. Weak support for SS18.7's
   common-area reading, not proof: selecting `FE` or `FF` returns four pages and then nothing,
   where an ordinary block's read runs on, so the pair is at the end of the address space - but it
   still answers to segment bits 3/7, not the `0` that `D8FF` encodes. Artefacts:
   `data/traces/xt57b/store-dump/common.py` and `common.txt`.
 - `docs/xt57-auth.md` SS18 - **the select keystream byte 1 is measured, so the select decrypts**.
   The 40-bit block of SS7.2 is one contiguous stream, so a 16-bit select takes `k0` over byte 0,
   `K >> 24` over byte 1 and `(K >> 16) & FF` over the tag's echo. `K` was unknown at the two
   select positions, so both were read instead - index 1 accepts a read (SS9.1's failed attempt
   there sent index 2's opcode), and index 12's eight read bytes must lie in `78`-`7F` - each
   anchored on the eight words of the no-select default group, seven 32-bit checks apiece:
   `K(1) = B0CD03E4`, `K(12) = 1E2BB45E`, the latter matching a VVDI capture in SS7.2. Confirmed by
   sending one plaintext select (`DD01`) at indices 2, 3, 6, 7 and 12: five different ciphertexts,
   all opening the same address, all five echoes decrypting to the same byte. Canonical select
   plaintext = on-air `^ 7B1E`; segment = `(byte0 & 3) ^ 3`, block = `byte1 ^ 1E`. The VVDI's three
   selects are segment 7 block 1E, segment 5 block 01, segment 4 block 00. Also: the tag's echo is
   `[g(segment)][block & F]` in plaintext; `k0(12)` is `7B` and `k0(1)` `7A`, correcting SS17.4's
   `78`/`79`, which had chosen the low three bits rather than measured them; a select is accepted
   at a read index, so SS17.8's "one upper-five-bit pattern per index" is the wrong reading of
   SS9.1 step 3. It does **not** settle SS17.11: the awkward group is the two blocks at the top of
   the block space (`FE FF`), which look like Hitag Pro's common area but answer under the wrong
   segment bits (`selks.py`, `selks.txt`, read-only) (@claude)
 - `docs/xt57-auth.md` SS17.10/SS17.11 - **the XT57B decodes 36 of its 128 addresses, not 128**.
   All 256 selects opened at index 1 and read at indices 2-5 (`shallow.py`, `tagC-shallow.json`,
   read-only): an address returning the live page 0 at all four is not decoded. 92 of 128 are
   not there at all, so SS15.4's "103 all-zero" was 92 undecoded plus 11 decoded-but-empty.
   Measured on tag C alone, the map predicts the other two chips exactly - every address holding
   data on any of the three falls inside it, 25/25 on tag A and 10/10 on B and C. Real storage is
   1152 bytes; 4096 is the address space. The map (one group of 32 blocks, two of 2 in different
   bit-4 halves, one empty) is expressible in Hitag Pro's segment configuration only as 8
   segments of 16 blocks, since `blockCount` is a nibble - so SS17.3's 4x32 reading is the less
   likely one (@claude)
 - `docs/xt57-auth.md` SS17.8/SS17.9 - Hitag Pro's 24-bit `WriteByte` (`0xA0 | page | bytenum<<3`)
   probed on tag C, `bytenum` 0-3 at exchange indices 2 and 3: every frame drew no response and
   ended the session, which is only SS9.1's validity rule refusing a non-read opcode at a read
   index. A real test needs a write index, and it cannot be made a no-op - the write opcode
   addresses a different space whose current contents cannot be read first (SS15.5) - so it needs
   a tag chosen to be spent. Correction: the probe address was picked as all-zero from the dump
   but is a shallow-index **fallback** - selected at index 1 it returns the live page 0 at indices
   2, 3, 4 and 5 - so the intended no-op guarantee did not hold; nothing was written only because
   every frame was refused - confirmed by a full re-dump of all 128 addresses afterwards, byte-
   identical to the original at 256/256 selects
   (`data/traces/xt57b/store-dump/tagC-after-writebyte-probe.json`). This also gives a test that separates an
   unimplemented address from an empty one, so SS15.4's "103 all-zero" is an upper bound on empty
   storage, not a count of it (@claude)
 - `docs/xt57-auth.md` SS17.6/SS17.7 - the Hitag Pro **plain-mode authenticate is refused** by the
   XT57B, so the segment configuration cannot be read from the reader side. Probed read-only on
   tag C: all 256 first-byte values at 104 bits (only `C0`-`C7` answer), every frame length from
   8 to 104 bits with `FF` + IDE (nothing at any length), and the known-good `C0` frame at every
   length (103 bits refused, 104 accepted - the chip counts bits). `C0`-`C7` are not modes: all
   eight authenticate on the same recorded answer, the post-auth keystream is byte-identical
   under all eight, and the only difference is `reply(Cx) = reply(C0) XOR (x in all 8 bytes)` -
   three bits that pass through the authentication function linearly. Steering a tag back onto a
   known nonce needs `--off 45000`; at the default 20000 us the orbit holds no known nonce
   (@claude)
 - `docs/xt57-auth.md` SS17 - **the XT57B's memory format is Hitag Pro**, and with it the last
   unknown constant of the command keystream. Matched against the four Hitag XMA variants
   implemented by the Hitager project (`reference/hitager/`): `Hitag2 + EE`, `Hitag2 Extended` and
   `Hitag AES` are excluded (5-bit command plus inverse, at most 2048 bytes). `HitagPro.cs` matches
   every frame the XT57B sends - the 4-bit `0011` IDE read with its 32-bit truncation, the 16-bit
   `0xD800 | segment << 8 | block` select, the 8-bit `0x00 | page` read, the 48-bit
   `0x80 | page` + 32-bit data + CRC-8 (poly 0x07) write, the 32-bit page, and 8 segments x 16
   blocks x 8 pages x 32 bits = 4096 bytes, exactly the measured store size. `SuperChip.cs` (VVDI
   Super Chip / XT27A) is the same code, so Xhorse already ships Hitag Pro. Index 6 is the one
   index driven both as a select and as a read, and the keystream there is command-independent, so
   the measured `C8`-`CF` select window decrypts to Hitag Pro's `1101 1AAA` only if the top five
   bits of `OPC` are zero: **`OPC = 0x00`, `k0(2) = A0`**, and SS13.3's `d(i)` table becomes the
   absolute command keystream rather than a relative one (@claude)
 - `docs/xt57-auth.md` SS16.4 - a third XT57B dumped. Its UID was read first and used as a
   prediction: address `3:1E` slot 4 decrypts to `95298F14`, matching `lf hitag info` exactly, on a
   second chip nothing was fitted to. Tags B and C are byte-identical at 123 of 128 addresses, so
   the entire per-chip content of a fresh XT57B is five words: three identity words at `0:00`
   (aliased at `2:1F`), one 16-bit field at `0:01` (aliased at `2:1E`), and the ID46 page 0. The
   identity words carry a constant `011D` tail and are independent of the emulated UID (@claude)
 - `docs/xt57-auth.md` SS16 - a second XT57B dumped and diffed against the first. The decryption is
   verified on a chip it was not derived from: address `3:1E` slot 4 decrypts to `11D89614` and
   `lf hitag info` on that tag reports exactly that UID, while `"MIKR"`/`"..ON"`/config stay
   byte-identical across both tags. 102 of 128 addresses identical (all zero), 26 differing; tag A
   is programmed (VW part number, `CAFECAFE`, record templates), tag B near-blank but carrying
   ascending index tables. The key is global - same 64-bit reply from both tags - but the nonce
   timing is per-tag (base 119296 vs 119568), so SS13's recipe needs re-steering per chip (@claude)
 - `docs/xt57-auth.md` SS15 - **the XT57B store is read out in full**: 128 addresses, 1024 words,
   4096 bytes, at nonce `CDB9AE8C`, read-only (16-bit selects and 8-bit reads, nothing written).
   The unknown opcode byte cancels out of the addressing (`on-air byte = A0 ^ d(i) ^ s`), so the
   five unmeasurable bits of SS13.3 are not needed. The last unknown keystream constant is measured,
   `D = 4D494B1B`, confirmed by a known word read at a deep index and by address `3:1E` decrypting
   to the stored ID46 image - page 0, `"MIKR"`, `"..ON"`, config - in ASCII. The whole session
   keystream is now absolute. Dump and scripts in `data/traces/xt57b/store-dump/` (@claude)
 - `docs/xt57-auth.md` SS12.3 - the select decode was wrong. Byte 1 bits 3 and 4 are address bits,
   not ignored bits, so the store is 128 addresses and not the 8-per-position that was swept; and
   the three select positions address one space through an XOR offset on the select value, not the
   disjoint sets SS13.5 claimed. Verified by predicting index-1 selects for groups found at index 12
   (@claude)
 - `docs/xt57-auth.md` SS14 - the generator hunt, redone against the corrected keystream: not a linear
   recurrence in any of six bit orderings, not states of the public nonce register, not an affine
   filter of it at any offset to 20,000 clocks, not a quadratic one, and `k0` is not a byte of the
   data keystream. The exhaustive HITAG2 filter-generator state recovery over the anchored 160 bits
   has **completed with a null in both bit orders**: of all 2^48 states, none produces that
   keystream MSB-first or LSB-first, so the channel is not HITAG2's cipher clocked as a contiguous
   40-bits-per-exchange stream however the bits are ordered. 2 h 45 min and 2 h 43 min on 32
   threads, guided DFS, ~2^43 node visits each, validated against a planted state. What survives:
   a per-exchange block function of (key, nonce, index), now the leading hypothesis, and a
   HITAG2-like core with Xhorse's own taps or filter constants (@claude)
 - `docs/xt57-auth.md` SS13 - the XT57B **command** keystream is recovered for a whole session, up to
   one unknown byte: the write opcode is the read opcode XOR `0x80` in plaintext (seven independent
   confirmations), the plaintext command byte does not depend on the exchange index, and two measured
   cross-window links join every index into one chain. The five remaining bits are the plaintext read
   opcode, which the protocol never transmits in clear (@claude)
 - `docs/xt57-auth.md` SS13.4 - 13 of the 17 data-keystream values were wrong, inherited from the
   "HITAG2 pages 0-7 in order" assumption SS9.1 step 6 refuted. The `A5AE`/`A5AF` window is corrected
   absolutely (anchored on the live emulated page 0); the other two windows are corrected relative to
   each other and left with one unknown 32-bit constant, since the VVDI traces show the word index 7
   reads is the same on two different tags and so cannot be the live page 0. Replays were unaffected
   (a write and its read-back share the error) (@claude)
 - `docs/xt57-auth.md` SS12 - the memory map was about three times too large: every exchange between
   two selects returns the *same* eight words, so an ID46 session reaches 24 words (96 bytes), not 72
   addresses. `T1`/`T2`/`T3` of SS7.6 are one set of words, not three (@claude)
 - `docs/xt57-auth.md` - the post-auth keystream at exchange indices 13-20 was off by a constant
   `0x000C0000`; corrected, and the missing index-5 word (`6CC0FE4B`) recovered (@claude)
 - `docs/xt57-auth.md` SS9.1 - the XT57B *command* keystream depends on the 104-bit select payload,
   not on the nonce alone: a chip whose family string has changed answers none of the recorded
   command bytes. The data keystream is unaffected and stays a function of (nonce, index) (@claude)
 - `docs/xt57-auth.md` SS9.1 step 9 - the family/model identifier is not address `13` of the `A5AF`
   window. It is the write-mode window's index-2 location, which reads `00000000` on an undamaged
   chip; a non-zero value there is what makes an XT57B report `XT5F`. Writing zero back restores
   `"XT57X"` (@claude)
- Changed `lf xhorse xt57 nonce` - the 104-bit select now carries the family bytes the chip itself reports to the `0011` identify, instead of the constant `C0 "XT57" 123456789ABCDEF0`. A chip answers no nonce at all to a select carrying another family's bytes, so an XT37 previously needed `--select C058543337123456789ABCDEF0` spelled out and now needs no arguments (verified on one: flags `AICFN`, nonce `D2BFF843`). Only the standard 13-byte `0xC0` form is rewritten, only when the identify CRC validated, and an explicit `--select` is still sent verbatim - the new `F` flag says which happened. `xt57 set`/`restore` keep the constant on purpose: they replay a keystream recorded under an `XT57` select. No regression on an XT57B: it identifies with a valid CRC, and a 14-session sweep steered onto four recorded nonces (`6DCD7463`, `36E6BA31`, `CDB9AE8C`, `F9B735D1`) authenticated at all four with every reply byte-for-byte the recorded one, 0 mismatches (@claude)
- Changed the channel-B identify check - it tested `rx[0] == 'X' && rx[1] == 'T'` and took the rest on trust. A 40-bit reply's last byte is the CRC-8 of SS7.1 over the `0011` command byte and the four family bytes, so `xh_ident_crc_ok()` now validates it and the probe rejects a reply whose CRC is wrong rather than accepting any two bytes that read `"XT"`; a 32-bit reply is still accepted, since it carries no CRC byte (the XT57B read4 traces show the 5th byte dropping out on weak coupling). `lf xhorse info` prints the reply as family + CRC with an ok/BAD verdict, and `lf xhorse xt57 nonce` reports it as the `C` flag, with `identify CRC bad` replacing the old `no XT57X` note (@claude)
- Changed `lf xhorse xt57 restore` on an XT37 to the order the tool itself uses. Session 2 of `xhorse-XT37b-reset-from-ID48.pm3` runs at nonce `C1CBFC8C`, which the XT37 tables already cover, so the VVDI's reset decrypts frame for frame: select `s3 b1F`, read page 7, **write the config word `01F00036` first**, then select `s0 b00` and write the image (`E7EC931E` - a fresh random UID - then `4D494B52`, `00004F4E`, `06AA4854`). That is the reverse of a set, and both are right: a set must finish the image before the config word because that word makes the chip emulate (`xt57b-fullread.md` SS12.7), while a reset puts the chip on a reachable personality first so a session that dies halfway still leaves it answering. **(correction)** the claim that `restore` "had it backwards, image-first" was wrong - `xt57_build_steps()` already orders by type (image first only for a broadcast type, config first otherwise), so it was right here; what appends the config last whatever the type is the decoded-image path added for the XT37. `restore` now uses `xt57_build_steps_reset()` on an XT37 anyway, for a different reason: `reset-transponder-xt57b.pm3` session 3 maps one-to-one onto the native step list and shows the XT57 writing config pages 4, **5 (`0x49`) and 6 (`0xC900`, then `0`)** where an XT37's reset writes page 4 alone - 21 exchanges against 14 (`docs/xt37.md` SS5v). Verified: all 5 writes in the tool's order, chip reading back UID `ED642E16` type `36`. See `docs/xt37.md` SS5u (@claude)
- Fixed the channel-B recovery field-off - **(correction)** SS5s blamed `xt57 reset`'s failure on ID48 having no broadcast frame for the burst to synchronise against. Wrong: a sniff of the Xhorse tool recovering the same chip (`xhorse-XT37b-reset-from-ID48.pm3`) shows it simply drops the field for **720.3 ms** and then gets its `5AA5` ack - no burst, no word, no phase. Every field-off here was far shorter: 20 ms in the identify probe (`XH_T_FIELD_OFF_US`), 228 ms between reset rounds (`XT57_EM_PROBE_OFF_US`), 200 ms in the probes SS5s tried. So an emulating chip was never given long enough to stop emulating and listen. Added `XH_T_FIELD_OFF_LONG_US` (760 ms), used by the reset path, and `lf xhorse info` now spends it once as a second attempt before reporting no channel-B chip. Verified: a 750 ms field-off reaches the chip normally, and `info` still reads a non-emulating chip unchanged. See `docs/xt37.md` SS5t (@claude)
- `docs/xt37.md` SS5s + `lf xhorse xt57 set` - **setting an XT37 to ID48 makes it deaf to channel B and nothing in this client brings it back.** The write itself is correct - 17 of 17 pages verified, config word `3FF00048` read back, and the chip emulates a Megamos properly (`lf em 4x70 info` answers with tag ID `78 B9 04 19`, built from the image's own pages) - but no XMICRO ack appears anywhere from 2 ms to 360 ms of power-up afterwards. `lf xhorse info`'s own hint had said so for the XT27A ("an EM type (ID11/12/13), ID48, ID8E or ID8A never answers it") and it holds for the XT37. `xt57 reset` was tried seven times - default, `erase`, `7935`, `megamos`, and a 64-position burst sweep - and `lf xhorse reset` too; all failed, and the chip needed the Xhorse programmer. The reset works by landing a burst in a *broadcast* frame, and ID48 is reader-talk-first, so there is nothing to synchronise against. `set` now refuses type bytes `0x8C`, `0x35`, `0x48`, `0x8E` and `0x8A` without `--wipe`, where before only the broadcasting ones were gated (@claude)
- Changed `lf xhorse xt57 set` - it now covers the XT37 as well, writing a whole image and committing the config word in one operation, which is what the tool itself does and in its order (image first, config last, `xt57b-fullread.md` SS12.7). All 17 decoded images (`docs/xt37.md` SS5p) are available through it, chunked over several sessions via the `'X'` markers `xt57_run_session` already understood. Three things this fixed: the **full config word** is written rather than the type byte patched into whatever the chip held - the upper half is not a constant (`01F0`, `3FF0` for ID48/MQB, `7FF0` for ID7935) and ID7935 also writes page 5 (`00000240`); on an XT37 the **decoded capture is preferred over a built recipe**, because the built ID46 recipe writes the XT57's config pages 5 (`0x49`) and 6 (`0xC900`) which an XT37's own sets never touch; and the broadcast warning is raised only for the types that actually broadcast (type byte `0x8C` or `0x35`) instead of every image. `setimage` is removed as redundant and `settype` is relabelled as the advanced config-word-only path. Verified on an XT37: `set -t ID46 -u ED642E16` wrote the 4-page image and `01F00036` across 2 sessions, all 5 writes verified, chip reading back UID `ED642E16` type `36` (@claude)
- Added `lf xhorse xt57 settype` - writes the emulated type byte of an XT37/XT57 (config block page 4, `docs/xt37.md` SS5o) as select/write/read-back, reporting success only if the chip returns what went in. It preserves the config word's upper half, which varies by type and is not understood (SS5p), replacing only the low byte; it refuses a type whose emulated image it cannot also write, since `0x36`/`0x37`/`0x46` share one image while any other type set byte-only would leave the chip emulating a type whose image is absent - a one-way door per `xt57b-fullread.md` SS12.7 (`--force` overrides); and it prints the command that restores the previous type. Verified `36 -> 37 -> 36` on an XT37 with the image and UID unchanged throughout. A first attempt reported "no read-back came back" having written nothing - correct, but caused by the in-call nonce sweep being too narrow (16 sessions) for the phase drift between runs; it is 30 sessions over ~60 us now, and the same widening went into the read path. Full image writing is still not implemented (@claude)
- Added `lf xhorse xt57 read` - reads channel-B pages of an **XT37 or XT57**. It identifies the family from the chip's own identify reply, picks that family's keystream table, finds a power-up delay that reaches one of its nonces, then reads a block as a select plus eight triggers and decrypts each page. `--arg` is the select operand (segment << 5 | block, so `0x7F` is the config block), `--at` skips the search, and with no argument it walks every block the family is known to use. The reads are *built* from keystream, not replayed, so they reach addresses no capture contains. Verified against the hand-derived map: `s0 b00` returns the live image `ED642E16 4D494B52 00004F4E 06AA4854`, `s0 b12` returns `00000305`, `s3 b1F` the type word - all byte-identical (@claude)
- Changed `lf xhorse info` - an XT37/XT57 is no longer reported as "not currently supported". It now prints the identify reply with its CRC checked, then opens a session to read the **emulated type byte** from the config block (`s3 b1F` page 4) and names it from `mem-layout.md`'s table, every entry of which was confirmed against a decoded set session (`docs/xt37.md` SS5p). The whole config word is printed alongside, since its upper half varies by type (`01F0`/`3FF0`/`7FF0`) and is not understood. If no recorded nonce comes up it says so instead of failing (@claude)
- Added XT37 keystream tables and made `xt57_ks_t` family-aware via a `sel_auto` flag: an XT57 table replays a select recorded as `C0 "XT57" ...` verbatim, while an XT37 table has the firmware fill the family bytes from the chip's identify reply - which is what lets one table serve any chip of the family, since `K` depends on (nonce, index) alone and `k0` additionally on that select payload, leaving only the per-tag power-up delay to search for. The delay is **computed, not searched**, the way the hitager tool does it: one no-answer session reads the nonce the chip is producing, the client steps the LFSR for the clock distance, and a target `d` clocks forward sits at `delay - d * 8 us` and one `d` behind at `delay + d * 8 us` (`docs/xt57-auth.md` SS10.3) - **both** directions, since searching only forward makes a target a few clocks behind look like most of a 715M-state orbit away, which is how an XT57 presents against these tables. The field-off time is carried per family too, and that turned out to be the whole story behind the earlier flakiness: at the XT57's 45 ms an **XT37 pins its nonce** and stops tracking the power-up delay (SS4a), so steering was impossible and sweeps succeeded only by luck. With 200 ms for XT37, `lf xhorse info` went from 76 s to **4.8 s** and a full eight-block read from blank rows to **13 s with every block present** (@claude)
- `docs/xt37.md` SS5q - **all 39 XT37 sessions now decode**: the last one, `set-ID8C` session 3 carrying ID8C's type commit, had lost its nonce frame to a broken demodulation (two fragments `0BF`/`1F0` that an earlier pass mistook for the nonce and answer fields). Re-reading the handshake found the answer `B728EDDE` and reply `1913A186AA4460CE` intact, and since an answer authenticates at exactly one nonce the chip can identify it: every nonce is a window on the shared LFSR stream and the XMICRO window bounds the power-up to ~100-160 ms, which is 7599 positions, of which 21 fit either fragment - and exactly one, `0BF76AA0` at 140100 us, returned the captured reply while the other 20 stayed silent. It sits 4 clocks from `BF76AA08`, the nonce ID8A's commit session used, so the tool's commit sessions land at the same delay. (Only the 12-bit fragment was real; the 9-bit one is debris, and requiring both had put the sole match at a 1.47 s power-up.) Decoded, the session selects `s3 b1F`, writes page 4 = **`01F0008C`** and reads it back identically - ID8C writing the same type byte `0x8C` as ID11/ID12/ID13, as `mem-layout.md` says for both, with the common `01F0` wrapper. Read-only throughout (@claude)
- `docs/xt37.md` SS5p - **every XT37 set session decoded**: 38 of the 39 XMICRO sessions in `data/traces/xt37b/`, keystream recovered for 12 steerable nonces by the SS5n read-only probe. The one failure is `set-ID8C` session 3, whose nonce *and* answer both demodulated corrupt, leaving nothing to identify the nonce from (three other mis-decoded nonces were recovered from their answers). 354 of 358 write/read-back pairs agree. The **type byte** is written at `s3 b1F` page 4 for all 19 sets, and every value matches `mem-layout.md`'s type-byte table where it has an entry - including ID11/ID12/ID13 all writing `0x8C`, ID7938 `0x47` and ID4E `0x64` - nineteen independent agreements with a table derived from the app decompile. **(correction)** SS5o called `01F000` a fixed wrapper: ID48 and MQB write `3FF0` and ID7935 writes `7FF0`, so only the low byte is the type. Sets come in three shapes - the ID46 family writing four pages of `s0 b00`, small-image types a few `0000xxxx` pages, and large-image types writing `b00` onward then zeroing a run of blocks, which is what decides whose image survives. This **proves SS5m**: `set-MQB` writes `s0 b0C/b0D/b10/b12` byte-for-byte as the live chip still reads them, so SS5m's "probably a shifted remnant" hedge for `b10`/`b12` is settled, and `set-ID8E` writes `b01` pages 2-3 exactly as the chip reads them now, finishing the one block SS5l could not place. Read-only throughout (@claude)
- `docs/xt37.md` SS5o - the XT37 **type-byte write**, decoded: sessions 1 and 3 of `set-ID8A-H` give the rest of the set-type flow. Session 3 selects **segment 3 block 1F** and writes **page 4 = `01F0008A`** - `0x8A` being ID8A - and the chip now reads `01F00036` there after the reset, so the type is committed as `01F000<type>`, a fixed wrapper plus the type byte, in the same page `xt57_build_session()` writes the XT57B's type to. Session 1 is the genuineness check: it reads the identity block `s3 b1E` pages 4-7 and returns `43C642CC 7352653A A9BE6214` - **byte-identical to what the live chip returns at those pages today**, a capture decoded with keystream measured hours later from a different nonce, which independently validates the keystream method, the block map (SS5l) and the addressing model (SS5k) at once. Full flow: check `s3 b1E`, write the image, then commit the type - the order `xt57b-fullread.md` SS12.7 describes, since once the type byte is in the next power-up has the chip emulating. The session-3 nonce needed power-up `140068 us`, agreed by four independent stream anchors (@claude)
- `docs/xt37.md` SS5n - **the ID8A set session decoded in full**: 167 exchanges at nonce `D4BD0543` (79 writes, 79 read-backs, 10 selects, terminator), with keystream for all 166 indices recovered from 8 read-only probe passes rather than the ~1350 sessions per-index probing would need. The shortcut: every plaintext in a set session is `0x80|page`, `page` or `0xD8|seg`, which vary only in the low 3 bits, so each recorded frame pins `k0`'s high 5 bits for free; any byte sharing them is a valid read, so a read-only session traverses all indices; and with no select the window stays `s0 b00`, whose zero pages make four of eight probes return `K(i)` while the UID-returning probe fixes `k0`'s low bits. The session writes the ID8A image to `s0 b00` pages 0-7 (`8A0AD459 75020AE0 A03448A3 A77F0000 00004DAD 000059AD 00000000 7771654D` - the last being `"wqeM"`, the marker `armsrc/xhorse.c:1227` writes, confirming the decode independently), then `s0 b01` pages 0-2 and `s0 b02` page 0, then zeroes blocks `04`-`0B`, skipping `03`. **This identifies `s0 b01` as per-type config written by every set** - the block SS5m could not place - and explains why the MQB image survived at `0C`/`0D`: an ID8A set never touches them. 56 of the 57 checkable write/read-back pairs agree; the one that does not is flagged. Read-only: no write frame was sent to the chip (@claude)
- `docs/xt37.md` SS5m - **(correction)** XT37 blocks `0C`/`0D` are not a serial number, as SS5l guessed from their printable bytes - they are **28 contiguous bytes of `chip8BData`**, the 174-byte plaintext constant the Xhorse app appends for the MQB type, byte-exact at blob offsets 68-95 with a linear page mapping (2 bytes per page, 16 per block). `set-type.md` had already noted the ASCII `D6A0091GH3A7559695G` sits mid-blob. `data/traces/xt37b/xhorse-XT37b-set-MQB.pm3` is in the capture set, so these are left-over image data from that set: a reset restores the ID46 personality in `s0 b00` and the type byte in `s3 b1F` but does not wipe the image area, so the other blocks are emulated-image scratch space rather than identity. `s0 b10`'s `21011100` and `b12`'s `0305` also occur in the blob but at offsets that do not fit that grid (135 and 170 vs a predicted 132 and 164), so `b10` is probably a shifted remnant and `b12` is a guess; `s0 b01`'s `26B1 D809` is in no app blob and stays unidentified (@claude)
- `docs/xt37.md` SS5l - the XT37's complete block map, and **(correction)** SS5i's sweep was incomplete: it read only pages `{0,2,6}` of each block, its can't-miss argument having depended on the XOR model SS5k refuted. A complementary pass over 4 segments x 32 blocks found one more block and the important one - **`s3 b1F`, the config block**, whose page 4 `01F00036` ends in type byte `0x36` = ID7936/ID46, the factory personality this chip was last reset to, in the same page `xt57_build_session()` writes the XT57B's type to (its select `0x7F` *is* `s3 b1F`). `s3 b1E` is the identity block the tool's identify read targets, holding the only four full 32-bit words on the chip and unchanged across every set and reset in the captures. Final: **29 words = 116 bytes in 8 blocks** of 128 addressable, coverage 128/128 x 8 pages with the one dropped session re-run and empty. `s0 b0D`'s six pages are all printable - `"GH3A7559695G"` - which looks like a serial, flagged as shape-only; `s0 b01 b0C b10 b12` remain unidentified (@claude)
- `docs/xt37.md` SS5k - **(correction)** the XT37's layout is plain NXP Hitag Pro, and two earlier claims in this file were wrong. `command-reference.md` SSD/SSG documents the hitager drill channel - read page `i08<pp>`, 0-7 within the selected block; write page `0x80 | page`; write one byte `0xA0 | page | (bytenum & 3) << 3`; 32 blocks of 8 pages x 4 bytes - and the chip matches it exactly. So `0xA0` is the **write-single-byte opcode**, not a read: SS5f's `0xA2` was refused because an 8-bit frame carries neither its byte nor its CRC, and SS18.1's formula describes a write. And the per-index/per-window XOR masks of SS5f/SS5h **do not exist** - they were an artifact of SS5d assuming the ID12 session wrote pages `0,1,2,3,4` in order, which shifts `k0(i)` by exactly that XOR; the tool actually wrote pages `2,1,4,3`. Corrected masks `k0 = FE 74 74 D9 6B 17 AA 1B` read page 0 as the UID `ED642E16` at **all eight indices**, so there is no index-dependent addressing: a read is `(segment, block)` from the select and `page 0..7` from the 3-bit trigger. Segment labels in SS5i were off by 2 for the same reason. Re-derived, all 448 reads agree with zero conflicts, and the emulated HITAG2 image is pages 0-3 of segment 0 block 0 - the block a session opens in - in canonical UID/key-low/key-high/config order (@claude)
- `docs/xt37.md` SS5j - the XT37's whole reachable address space read out in plaintext: all six remaining populated windows swept at all eight triggers in all four groups, 48 sessions and 336 reads, every one consistent with `word = trigger ^ c_group ^ c_window` and every word identical from all four groups and both indices of each pair. **27 words = 108 bytes** across 7 windows: `0x40` is the live HITAG2 image, `0x3E` holds four full 32-bit words, and `0x41 0x4C 0x4D 0x50 0x52` hold 2, 8, 6, 2 and 1 words, all `0000xxxx`. **(correction)** SS5h's "a window holds four words" generalised from `0x3E` alone - windows hold 1, 2, 4, 6 or 8, at word indices `0..N-1`. Two limits stated: `c_window` is observable only where a window is partly populated (unique at `0x52`, ambiguous elsewhere, and at `0x4C` - all eight words live - every value fits), so word numbering inside a window is known only up to that symmetry; and all of it is at one nonce, indices 1-8. Window `0x4D`'s words read as the ASCII pairs `GH 3A 75 59 69 5G`, flagged as a hypothesis from byte values alone. Read-only throughout (@claude)
- `docs/xt37.md` SS5h/SS5i - the XT37's window addressing, mapped. Reads are **XOR-addressed**: `word = trigger ^ c_group ^ c_window`, where the index pair and the window each contribute a 3-bit mask (`c_window` = 0 at block `0x40`, 6 at `0x3E`), a window holds four words at indices `0..3` and the other four addresses read zero. Window `0x3E` was swept at all eight triggers in all four groups: four words (`7352653A A9BE6214 25FD4F90 43C642CC`), both indices of every pair returning the same one - a free check on all eight `K(i)`. All 256 blocks were then probed with a detector that cannot miss (one select, trigger 0 at every index, so the words read are `{c, 2^c, 6^c}` and for all eight possible `c` at least one falls in `0..3`); five sessions whose nonce never came up were re-run and read zero, so the sweep is complete at 256/256. **Only seven distinct windows hold data** - `0x3E 0x40 0x41 0x4C 0x4D 0x50 0x52` - each answering at two addresses because **bit 7 of the block is ignored**, making the select's block effectively 7 bits. `0x40` is the window a session opens in: it returns the live HITAG2 image. An earlier four-windows-per-session sweep that reported everything empty was a false negative - it probed `0x3E` at a word that reads zero (@claude)
- `docs/xt37.md` SS5f/SS5g - the XT37's read-trigger encoding, measured against known plaintext. A read address is `(window, exchange-index-pair, trigger & 7)`: the trigger is **three bits and nothing else** - `0x08`, `0x0A`, `0x10`, `0x12`, `0x20`, `0x80` and `0xA2` all got no answer and killed the session at the first frame, so **(correction)** SS18.1's `0xA0 | page | bytenum << 3` does not describe this command and `xt57_build_session()`'s `arg & 7` does. Exchange indices pair up `(1,2) (3,4) (5,6) (7,8)`, which is SS12.2 measured rather than inferred: the same trigger returns different pages at different indices but agrees within a pair. Of the eight words a group exposes, four are the live HITAG2 image and four read zero - words `0-3` for groups `(3,4)`/`(7,8)` mapping trigger to page directly, words `4-7` for `(5,6)`, and groups `(1,2)`/`(5,6)` mapping it as `(trigger & 3) ^ 2`, which is a swap of the low address bits rather than an offset. A built 16-bit select is also accepted (`16:2420` for block `0x1E`, echo `0654`, CRC ok), so windows are reachable. Requiring the four non-zero reads to be the four image pages over-determines each index and pins `K(1..8)` at nonce `B52F4150` exactly, with the zero read as an unfitted fifth constraint and the high halves matching SS5d's write-session derivation. Image verified unchanged afterwards; no write frame was sent (@claude)
- `docs/xt37.md` SS5d - **the XT37's keystream is recovered and the chip reads out in plaintext.** The command keystream came from the write session at nonce `B52F4150`, whose plaintexts are fixed by `xt57_build_session()` (`0x80 | slot` for a write, `slot & 7` for a read): `k0(1..10)` built into read frames that appear in no capture, and the chip answered all ten with valid CRCs - a wrong mask is not a valid command and kills the session, so 10/10 is the test passing. The data keystream needed a correction the XT57B work did not: decrypting under the capture's all-zero user data gave `ED6448E6` where the live emulated UID is `ED642E16` - right in the high half, wrong in the low - because the tool generates a random UID on every set, so an ID12 image's UID pages are not the user data. Their high half is always `0x0000`, so the top 16 bits of `K(i)` are sound; anchoring on the live UID (readable with `lf search`, SS11's own trick) pins the rest, and the two derivations agree exactly at indices 1 and 2 (`3E58`, `1D80`). What comes out is real: `4D494B52` ("MIKR"), `00004F4E`, `06AA4854` - the same config word as the XT57B dump - zero pages, and the live UID. Still missing for a whole-chip read: the trigger-to-page encoding, keystream past index 10, and entering other windows (SS5e). All read-only: no write frame has been sent to the chip (@claude)
- `docs/xt37.md` SS5b - **an XT37 authenticates and answers scripted reads.** 22 captures of an Xhorse tool configuring one (`data/traces/xt37b/`, @en4rab) yield 12 distinct `(nonce, answer, reply)` triples, consistent wherever a nonce recurs; the tool's own select is `C0 "XT37" 123456789ABCDEF0`, the frame the new auto-build produces. Steering to `BFFF6ABF` (2160 clocks forward of `6DCD7463`, so power-up 103548 us) and sending its captured answer returned the captured 64-bit reply byte for byte, and the tool's own post-auth frames replayed with 5/5 CRCs valid and every response identical to the capture - one of them fuller than the capture, whose decode had truncated a 40-bit reply to 18. Not yet a full read: the only recorded read sessions are the tool's six-frame identify read, reaching further needs the family-specific command keystream, and the responses stay ciphertext until a data keystream is recovered for one of these nonces - which the known-plaintext writes in the `set-*` captures should give offline. All read-only: no write frame was sent and no `--post` script contains one (@claude)
- `docs/xt37.md` SS4a/SS4b/SS5a - an XT37 cannot be read with what this repo holds, now measured rather than argued. Two new facts made the test possible: its nonce LFSR is the *same* as the XT57's (same taps, same orbit - any XT37 nonce sits 84-91 clocks ahead of `6DCD7463`, and a sweep at `delay + d*8 us` lands on the recorded nonces exactly as predicted), and its nonce state is retained far longer than the XT57B's ~33 ms - at `--off 45000` the phase is pinned for a whole run regardless of the power-up, identify-to-select or field-off sweeps, while `--off 200000` restores 8 us = one clock steering. Steered onto `6DCD7463`, `CDB9AE8C` and `F9B735D1` and given their recorded answers, the chip returned nothing at all, where an XT57B returns the recorded 64-bit reply; a control sweep of the nonce-to-answer gap from 64 to 304 T0 got no reply at any timing, so it is a refusal and not a timing miss. The nonce stream is family-independent, the answer function is not - reading an XT37 needs one `(nonce, answer)` pair captured against one. All read-only: no write frame and no `--post` script was issued (@claude)
- `docs/xt37.md` - first contact with an XT37, read-only (no write frame sent). Its channel B is the XT57B's: `XMICRO` acks `5AA5`, `0011` identifies, the 104-bit select returns a 32-bit nonce, and the nonces are windows on one bit stream (`8D2BFF84 << 1 = 1A57FF08`; `442D1A57`'s low half is `1A57FF08`'s high half). What does not carry over is the read-out: a select carrying `XT57` gets an ack and an identify but **no nonce at all**, while the same select with `XT37` gets one in every session, so a chip ignores a foreign family's select and no recorded `(nonce, answer)` pair can authenticate it - reading an XT37 needs one sniffed tool session against the chip, as the XT5F repair did. Bench notes: `--off 20000` loses the XMICRO ack after the first session on this chip (45000 works), and it does answer at the XT57 VVDI base 120140. Car-facing side answers HITAG2, UID `D621FE19` (@claude)
- `docs/xt57-auth.md` SS4 - **(correction)** the `0011` identify reply is not the five-character string `"XT57X"`. The trailing byte is the CRC-8 of SS7.1 (poly `0x07`, init 0) over the command byte and the four family bytes, which for this family collides with ASCII `'X'`: `CRC8(03 58 54 35 37) = 58` and, on the XT37, `CRC8(03 58 54 33 37) = 26`, both as observed. Nothing downstream depended on the string reading - the reader echoes only the four family bytes into the select and the probe tests just the leading `"XT"` (@claude)
- `docs/at100.md` SS6c - **(correction)** the device key was said to have been recovered with `tools/hitag2crack/crack5` "from a single `(UID, nR, aR)` triple", which describes no run that can be performed: `ht2crack5` takes two `{nR}{aR}` pairs for one UID and uses the second as a filter (`crack5/ht2crack5.c:821`), while the Autel's fixed reader IV means a tag only ever yields one. Duplicate the pair and the filter is a no-op: it prints the first candidate it reaches and `exit(0)`s (line 832) - for `836BC7C7` that is `58C0916AE748`, which reproduces that tag's `aR` exactly and fails on every other blank. Rebuilt to print all candidates, the one-pair run enumerates 61 831 keys, `9F63A5E8B14B` among them. Two filters each cut that to exactly one, both measured: the `{W}{~W}` self-check on a session carrying 18 commands (so one capture of one tag does suffice, given >=2 commands), and a second blank's `(UID, aR)`, the key being shared. Added `tools/analysis/at100_keyfilter.py`, which runs both. The key itself arrived in this project already known, so SS6c is a verified reconstruction, not a record of the original run (@claude)
- `docs/at100.md` SS9c - the AT100 config word is mostly decoration. Page `0xFE` stores whatever is written to it verbatim, so the fields had to be tested against the emulation rather than a read-back: with the chip configured to ID13 (whose payload is a literal EM4102 frame, so `lf em 410x reader` is a direct readout of the emulation engine) `b0` and `b3` turn out not to be read at all - mangled singly and together, the tag still answered `0456B307C8` - and of `b2` only bit 0 is, confirmed by two independent one-bit flips (`04`->`05` and `06`->`07` both silence it, while `04`->`06` and `04`->`FC` do nothing). The engine follows `b1`: `b1 = 02` with everything else left at ID13's values stopped the EM output, while `b1 = 01` with `b2 >> 2 = 02` kept it. So the four-block `(b3, g)` partition is not an engine selector and `g` was never a 2-bit field (@claude)
- Added `lf autel write` warning on pages `0xFB`-`0xFD` - they are live, not an identity stamp, which write tests settled: `0xFB` *is* the UID (write it, power-cycle, and the chip announces the new one in the wake handshake, authenticating with it consistently), and `0xFD:0xFC` is the live 48-bit device key, not a genuineness signature (change one byte of either half and the old key gets the UID then a refused `{nR}{aR}` while the new key opens a full session). Each was tested one page at a time with one byte changed and restored; the chip swept back byte-identical. So an AT100 can be re-keyed and re-UID'd by anyone who can authenticate, which - the key being shared and readable - is anyone. The warning exists because every AT100 command authenticates first, so a key written and then forgotten locks the chip permanently. See `docs/at100.md` SS8b (@claude)
- Changed `lf autel dump` - prints the whole readable space, all 255 pages `0x00`-`0xFE`, in two columns (`0x00`-`0x7F` beside `0x80`-`0xFE`) so it is 128 rows rather than 255, with a footer counting blank versus used pages. It was printing only the 59 pages the Autel programmer touches, which read as a window onto the chip rather than the whole of it. One reply carries at most `AUTEL_MAX_PAGES`, so a full dump is four READ chunks merged in the client; a chunk that fails names its range instead of printing zeros. `--used` keeps the old short view and still takes a single session (@claude)
- Changed `lf autel dump` - now also reads pages `0xFB` and `0xFD`, two factory pages the Autel programmer never touches: `0xFB` is the UID and `0xFD` the device key's high 16 bits, so `0xFD:0xFC` is the whole 48-bit `9F63A5E8B14B` in plain sight. Found by sweeping all 256 addresses, which also settled what the dump's curated page list had left open: every address `0x00`-`0xFE` answers a READ, the 196 the Autel never uses all read `FFFFFFFF`, and `0xFF` alone is silent - four READs on it return `Edges count: 0`, and the word a dump prints for it is cipher keystream, reproducible per session position, which is why it is always flagged `?`. So `0xFE` is the last page of memory, not the end of the dump. A write test on all 196 blank pages - each given a distinct value, so one sweep checks existence and aliasing at once - shows every one is real read/write EEPROM, independently addressed with no aliasing, no leakage into the 59 data pages and settable both ways; 392 writes, all verified, and the chip swept back byte-identical to its baseline. So an AT100 has 196 free pages = 784 bytes the Autel never touches. See `docs/at100.md` SS8a (@claude)
- Added `lf autel` - Autel AT100 programmable transponder support: `list`, `dump`, `read`, `write`, `configure`, `reset`. 18 emulated types, each image replayed byte-for-byte from a decrypted Autel capture. Reads and reconfigures a chip in any emulated type, including the talk-first EM4102 types (ID11/ID12/ID13), which need a wider reader gap than the Hitag default. See `docs/at100.md` (@en4rab)
- Fixed `lf autel` auth-stage hint - a tag that returns its UID and then ignores `{nR}{aR}` was blamed on a talk-first emulated type, but a wrong `-k` key reaches exactly the same stage; the hint now names both (@claude)
- Changed `lf autel` session open - a real power-down is tried first instead of the DOUT-only field gap, which a continuously-transmitting chip does not reboot from; a chip emulating ID4C now reads 8/8 where it managed 4/33 (@en4rab)
- Added `g_hitag_t_low` - the reader's field-off pulse width is now settable at runtime instead of fixed at `HITAG_T_LOW`; `lf autel` uses 12 T0, matching the Autel programmer's own gaps, and restores the default on teardown (@en4rab)
- Added `lf xhorse` - Xhorse XT27A super-chip support: `info`, `dump`, `configure`, `reset`, `types`; `reset`/`configure` also recover a chip stuck emulating an EM type; `info` identifies XT27B/XT57B (channel B) chips as unsupported. See `doc/xhorse_notes.md` (@CHANGEME)
- Fixed `hf search` - the OpenPGP AID probe sent 8 bytes for a 6-byte AID, reading past the string, so no OpenPGP card was found (@matrix)
- Fixed `hf 15 raw -r` - the device reply was left unread, so every later command printed the answer of the one before (@matrix)
- Fixed `hf 15 raw` - data too long for one packet is refused at once instead of timing out after 2 s (@matrix)
- Changed PM5 battery - charges to 4.2 V, the full charge of the fitted 3.7 V cell, and the fuel gauge uses its 4.2 V profile instead of the 4.35 V default, so it can detect a full charge and learn the capacity; re-applied at boot after a gauge reset (@iceman1001)
- Added PM5 FPGA usercode - shown in `hw status`, `hw version` and the connect banner, `fpga/common/gowin_usercode.py` prints the value of a bitstream file (@nieldk)
- Added `client/requirements.txt` - the Python packages the scripts in `client/` import, `bleak` for the BLE roles of `pm5_ble_bridge.py` and the two maintenance scrapers' deps, none of which were listed anywhere before (@iceman1001)
- Fixed `hf 15 info` - AFI, memory size and IC reference are read after the fields the tag really sent (a tag without DSFID or AFI showed wrong values), and a 256-block tag no longer shows 0 blocks (@matrix)
- Fixed `hf 15 info` - the DSFID and password protection lock bits of NXP tags were read from the wrong bits (@matrix)
- Fixed `hf 15 info` - on ST25TV512C/02KC the signature overflowed a stack buffer, a tag error leaked the read request, an early return left the field on, and a missing signature ended the command with an error (@matrix)
- Fixed `hf 15 info` - no answer to EAS ALARM, as sent by the SLIX tested with the EAS mode disabled, is reported as EAS not active instead of ending with an error (@matrix)
- Fixed `hf 15` - a tag error was reported with the flags byte instead of the error code (@matrix)
- Fixed `hf 15 info` - a tag error answered with code 0F or 10, e.g. by a SLIX to `-o`, printed nothing (@matrix)
- Fixed `hf 15 info` - the NXP and ST requests after the system info ignored `-2`: the ST signature was always read in 1 out of 256, the NXP requests always sent in 1 out of 4 (@matrix)
- Fixed `hf 15 info` - a UID given with `-u` was sent in the wrong byte order, so no tag answered, and a `-u` of the wrong length is refused instead of scanning for any tag (@matrix)
- Fixed `hf 15 dump` - wrong block size on tags without DSFID or AFI, buffer overflow on large memories, short block answers copied as full blocks (@matrix)
- Added `configure_environment.sh` / `make configure` - first-run check of a fresh clone: host and ARM toolchains, required and optional client libraries, the `PLATFORM` / `PLATFORM_EXTRAS` in `Makefile.platform`, Python venv and requirements, and device access rights; reports only, with `--install` to fix what it can (@iceman1001)
- Changed `hw status` - PM5 Battery, BWM and Power sections are down to 7 lines with colour coded charge, health and temperature; charger profile, chip details and idle / sleep counters moved to debug level (@iceman1001)
- Changed `hw bwm` - `autooff`, `charge`, `powersave`, `wifi`, `wifipower` and `ble pairing` take dash options (`--on`/`--off`, `--status`/`--stop`, `-m <mode>`) instead of bare words, so they show in the help; their output is aligned and colour codes the state, `autooff` and `ble status` are shorter and flag a default passkey (@iceman1001)
- Changed `hf mf hardnested` - PartialSumProperty() walks the 16 four bit suffixes as a prefix tree, and the even and odd halves of the all-bitflips update run side by side, about 1 second less per run (@iceman1001)
- Fixed `--ncpu` - it refused the machine's own core count, so all cores could never be requested explicitly (@iceman1001)
- Changed `hf mf hardnested` - the partial sum state tables are built in one pass with whole words instead of 67 million single bit writes, cutting about 2 seconds off every run (@iceman1001)
- Fixed `hf mf hardnested` - it searched every configured default path for each of the ~2000 bitflip state tables, which took 30+ seconds when those paths are on a network or Windows mount; the table directory is now resolved once (@iceman1001)
- Fixed `tools/external_sam_read.py iclass` - the faked answer to the e-purse update is the new value with its halves swapped, as a card answers (@matrix)
- Changed `hf mf sen` - default key prioritisation indexed the candidate list instead of rescanning it for each of the ~85k dictionary keys (@iceman1001)
- Changed `hf mf sen` - the default-key dictionary check no longer mallocs a cipher state per key and gives up on the first wrong keystream byte, ~6x faster for that phase (@iceman1001)
- Changed `hf mf nested` and `hf mf staticnested` - key recovery now uses both nonces in one `lfsr_recovery32_dual` pass, ~4 ms instead of ~390 ms per key (@iceman1001)
- Changed `lfsr_recovery32` - bit sliced first pass, branch free table extension and a cheaper bucket intersect, ~165 ms instead of ~270 ms per call and less memory, which speeds up `hf mf sen`, mfkey32/64 and the nested tools (@iceman1001)
- Fixed the ISO15693 reader - a tag answer was lost when a noise step came right before its SOF, e.g. `hf 15 dump` ended with `Dump incomplete` (@matrix)
- Fixed the ISO15693 tag answer decoders - an answer longer than the buffer wrote one byte past it (@matrix)
- Fixed `hf mf ndefread` - on sectors 32-39 of a 4K card it overflowed a stack buffer and decoded 3 of their 15 data blocks (@matrix)
- Fixed `hf mf ndefread` - it told the user to retry with `-k` but read the MAD only with the MAD key, a `-k` or `--aid` of the wrong length was ignored (@matrix)
- Changed `tools/external_sam_read.py` - added `mfc` for a HID MIFARE Classic credential, as `hf mf sam`, and `desfire` for a HID DESFire one, as `hf mfdes sam` (@matrix)
- Fixed `hf mf mad` - with `-f` and `--aid` it decoded card holder info instead of printing the AID's sectors, `--override` was refused and a `-k` or `--aid` of the wrong length ignored (@matrix)
- Fixed `hf mf mad` - on sectors 32-39 of a 4K card it overflowed a stack buffer, printed only their first blocks (numbered wrong from sector 33) and decoded 3 of their 15 data blocks with `--dch` (@matrix)
- Fixed `hf 15 sniff` and `hf iclass sniff` - the sniffer could not keep up with the samples and lost most tag answers, it now runs from RAM (@matrix)
- Fixed `lf t55xx detect` - finds ASK and FSK configurations that have the sequence terminator set, and reports whether it is set (@mfcarroll)
- Fixed `CLIParamStrToBuf()` - a string option that filled its buffer had its terminator written one byte past it and is now refused as too long (@matrix)
- Fixed `hf legic clone` - the client read `--mcc` as a string into a 1-byte buffer (refused, or a stack overflow), printed the wrong message for a missing `--mcc` or `-o`, and ignored `--mcc` and `-o` with `--write` (@matrix)
- Fixed `hf legic migrate` - it refused every run without `--dcf`, and took a `--dcf` longer than two bytes after a parse error (@matrix)
- Fixed `hf legic wrbl` - it took a `--fill` longer than one byte after a parse error (@matrix)
- Fixed `pm3_save_dump` - it returned success for a dump file it could not write, and `saveFileEx` printed "Saved" after a failed write (@matrix)
- Fixed `hf gallagher test` - the known vector and MES tests failed, it printed no overall result and `make check` did not run it (@matrix)
- Fixed `hf mf sen` - Enter or a timeout no longer leaves the field on, Enter is reported and also stops a single-key check (@matrix)
- Fixed `hf mf sen` - `--parity-mask` accepts its values, `--max-online-candidates` keeps the keys found (@matrix)
- Fixed `hf mf sen` and `hf mf isen` - a failed nonce file save is no longer reported as success (@matrix)
- Fixed `hf mf sen` - the dictionary fallback no longer says no key authenticated when one did (@matrix)
- Added AIDs `A0000003820013000101` and `A0000003820017000101` (HID configuration cards) to aidlist.json (@iceman1001)
- Changed `hf ntag424 getfs` - without `--fileno` it lists the settings of every file on the card; NTAG 424 has no GetFileIDs, so file numbers 0-31 are probed and a number the card never answers makes the list incomplete (PM3_EPARTIAL) (@iceman1001)
- Fixed `hf mfu chk` - Enter or a timeout during a chunk stops it (@matrix)
- Fixed `hf mfu chk` - no "-nan" speed when no chunk ran, the progress line counts the keys tested (@matrix)
- Fixed `hf 15 dump` - a dump that stops early no longer prints and saves the unread blocks as zeros; it shows the blocks it read, says how many of the total, saves nothing and returns PM3_EPARTIAL (@iceman1001)
- Fixed `hf mf brute` - Enter or a chunk timeout stops it, no division by zero on a stage under 1 s (@matrix)
- Fixed `hf mf brute` - `--emu` checks the trailer read, `--dump` keeps the keys when the card is gone and reports a failed save (@matrix)
- Fixed `hf mf fchk` - Enter or a timeout during a chunk stops it, `--emu` checks the trailer read (@matrix)
- Fixed `hf mf fchk` - `--dump` keeps the keys when the card is gone and reports a failed save, `--blk` no longer ignores `--emu` and `--dump` (@matrix)
- Fixed `hf mfp chk` - a failed exchange no longer abandons every remaining sector with a success exit code (@iceman1001)
- Fixed `hf mf autopwn` - reports failed emulator memory transfers, fills and file saves, keeps the trailers' access bits (@matrix)
- Fixed `hf mf autopwn` - a failed attack keeps the keys found so far, the dump is saved without the card, Mini dump size (@matrix)
- Fixed `hf mf autopwn` - Enter stops the dictionary, `-k` buffer overflow, memory leaks (@matrix)
- Fixed PM5 HF reader losing frames in both directions - the USB start of frame interrupt (1 kHz, unused) preempted the 14443-A reader loops long enough to overrun the single word FPGA SPI link  (@iceman1001)
- Changed PM5 bundled BWM firmware to V5 - per-host forwarding, 1.5 Mbaud link, UART FIFO overflow fix (@iceman1001)
- Changed PM5 BWM link - UART to the BWM at 1.5 Mbaud once the module supports it (BWM fw V5+): ~140 KB/s over WiFi, ~100 KB/s over BLE, was ~90 / ~80 KB/s (@iceman1001)
- Changed PM5 BWM link - replies go back to the BLE or WiFi host that sent the command (per-host forwarding, needs BWM fw V5+); etc etc (@iceman1001)
- Fixed `lf idteck reader` - reads wherever the first frame starts in the sample (@mfcarroll)
- Fixed `hf mfp`, `hf mfdes` native command set - the ISO 14443-4 block number no longer goes out of step with the card when a frame is lost (@iceman1001)
- Fixed `hf mf brute` - options read at their own positions (`--1k` ran a Mini), `--emu` reports keys and dumps not transferred to emulator memory (@matrix)
- Fixed `hf mf darkside` - with more candidates than fit one check, every candidate is tested and none is read past the list (@matrix)
- Fixed `hf mf nested` - tests every key candidate, follows cards whose nonce distance varies, single sector retries (@matrix)
- Fixed `hf mf chk`, `hf mf staticnested` and other key checks - a card that stops answering no longer locks the device (@matrix)
- Changed `hf mf staticnested` and `hf mf autopwn` - say "Card stopped answering" when the card is lost (@matrix)
- Fixed `hf mf rdbl`, `hf mf fchk`, `hf 14a info` and other MIFARE reader commands - the emulator memory is kept instead of being reset to zeros (@matrix)
- Added ISO/IEC 14443-4 error recovery to the reader - a lost or damaged answer is asked for again with R(NAK) instead of failing the exchange, `hw status` reports how often that was needed (@iceman1001)
- Fixed `hf mfdes chk` - an exchange the card never answered is no longer counted as a wrong key or as a missing application, such candidates are retried and what stays uncovered is reported instead of hiding behind "No keys found" (@iceman1001)
- Fixed `hf mfdes detect` - an exchange the card never answered is no longer counted as a wrong key (@iceman1001)
- Update Amiibo database (@tpglitch)
- Changed `lf pcf7931 write` - write is verified via the tag's read after write, retried up to 3 times, failures are reported (@GiacoBot)
- Changed `lf pcf7931 reader` - prints blocks 0-7 in order and decodes PAC, password, write protection and selection; handles partial selections, new `--rfb` param (@GiacoBot)
- Fixed `hf mf fchk` - `--emu` reports keys not transferred to emulator memory and a partial card dump into it instead of always succeeding (@matrix)
- Fixed `hf mf chk` - `--emu` reports keys not transferred to emulator memory instead of always saying they were, with no key found it says "No keys found" instead of reporting a transfer (@matrix)
- Fixed `hf mf staticnested` - `--emu` reports keys not transferred to emulator memory instead of always saying they were (@matrix)
- Fixed `hf mf eclr` - waits for the device and reports a missing emulator memory instead of always succeeding (@matrix)
- Fixed `hf mf nested` - `--emu` reports keys not transferred to emulator memory instead of always saying they were, a failed single sector recovery no longer returns success (@matrix)
- Changed PM5 bundled BWM firmware to V4 - TCP send buffer 11.5 KB, pm3 over WiFi up to ~90 KB/s (@iceman1001)
- Fixed `hf mf eload`, `hf mfu eload` - a failed emulator memory write, for example with the device memory full, is reported instead of `Done!`, fast mode no longer stays on after the upload (@matrix)
- Fixed `hf_mf_tnp3_sim.lua`, `hf_mfu_amiibo_sim.lua` - a failed emulator memory write is reported, tnp3 sends the flags byte and its blocks no longer land one byte off (@matrix)
- Fixed PM5 BWM link - BWM queries (e.g. `hw status`) no longer drop forward acks and client data over WiFi/BLE: no lost output lines, WiFi up to ~90 KB/s instead of ~20 KB/s (@iceman1001)
- Changed PM5 bundled BWM firmware to V3 - WiFi + BLE RAM fix (issue #10), OTA at full clock (@iceman1001)
- Changed PM5 bundled BWM firmware to V2 - power save, ESP-IDF 6.1 support, stability fixes (@iceman1001)
- Changed `hf mfu info` - UL-C fingerprint prints PRNG, Vendor and Chip IC lines, weak PRNG and counterfeit / magic chips in green (@iceman1001)
- Added `hf mfu info`, `hf mfu dump -v`, `hf mfu view -v`, `hf mfu eview -v` - UL-C key page lock check from the config pages, flags partial key overwrite / relayed auth attacks (BREAKMEIFYOUCAN, eprint 2026/100) (@iceman1001)
- Fixed `lf pcf7931 reader` - every read ran all 12 captures, no reply without a tag, field left on, block lost after a 16T0 PMC (@GiacoBot)
- Fixed `lf pcf7931 config` - out of range delay and offsets are refused instead of silently truncated (@GiacoBot)
- Fixed `lf pcf7931 write` - device froze on the 16-bit timer wrap, client always timed out, write right after a read failed; default init delay is now 17450 us (@GiacoBot)
- Fixed `lf nexwatch clone` - `-m` defaults to 1 as its help says, values the frame can't carry and conflicting options are refused, `--psk2` writes block 4 (@matrix)
- Fixed `lf nexwatch demod` - the raw id is the id field the tag sends instead of 0x40c00080 for every tag (@matrix)
- Fixed `lf nexwatch demod` - a magic number of 0xFF is found (@matrix)
- Fixed `lf nexwatch sim` - `--magic` is applied, `-m` defaults to 1 as its help says, values the frame can't carry and conflicting options are refused (@matrix)
- Updated `/Getting_Started_With_PM5.md` (@innocentbystanderproxmark)
- Changed `hf mfu pwdgen` - reads the tag by default (`-r` removed), `--uid` is offline, new `--mfg` for the Philips toothbrush algo offline (@iceman1001)
- Changed PM5 FPGA image - `client/resources/fpga_pm5.bin` is now built from the repo sources (`make -C fpga fpga_pm5_release`) and replaces the vendor built `PM5_FPGA_fix_loedge_bug.bin` (@iceman1001)
- Changed `hf mf info`, `hf mfu info`, `hf mfdes info`, `hf felica info`, `hf 14b info`, `hf calypso info` and `hf 14a info` - identify transit cards by keys, AIDs, system codes or data, show card number / balance  (@iceman1001)
- Fixed PM5 `hf mf sim` and other 14a sims - the RSSI ADC was reset and recalibrated before every listen, missing fast reader frames such as the auth nR/aR (@iceman1001)
- Added `hf mf sam` and `hf mfdes sam` - read PACS from MIFARE Classic SE and DESFire EV1/EV2 through a HID SAM, as Seader does (@iceman1001)
- Added `--dg` / `--save` to the HID SAM commands and `wiegand downgrade` - Seader style downgrades to iCLASS legacy / SR dumps, MIFARE Classic dumps and HID Prox clone commands (@iceman1001)
- Changed `hf iclass sam`, `hf seos sam` - show the CSN / UID and the plaintext SIO seen during the read (@iceman1001)
- Added transit card parsers in `client/src/parsers/`, run by `hf mf`, `hf mfu`, `hf mfdes`, `hf felica`, `hf 14b` and `hf calypso` dump/view/info (Clipper, Opal, Suica, Octopus, Navigo, Troika, CharlieCard, Bip!, SmartRider, ...) (@iceman1001)
- Added `hf 15 implant` - read temperature from NTAG 5 link sensor implants (VivoKey Thermo, Temptress) with TMP112/TMP117/TMP119 (@iceman1001)
- Added `hf goodisplay load` - write images to Goodisplay NFC e-paper tags GDEY0154D67, GDEY0213B74, GDEY029T94 and GDEY037T03, protocol from the Flipper Zero NFC Eink app (@iceman1001)
- Changed `hf 14a info` - hints at `hf waveshare` / `hf goodisplay` for e-paper tags (@iceman1001)
- Fixed `hf waveshare load` - 2.7" height is 264, and last pixels of non byte-aligned rows (2.13") were misplaced (@iceman1001)
- Preserve jobserver in Makefile builds to fix ProxSpace build speeds (@tpglitch)
- Added `hf gp info`, `hf gp crs` and `hf gp aids` - read GlobalPlatform card info, list CRS registered applets and find applet AIDs, all without keys (@iceman1001)
- Added GlobalPlatform ISD, CRS, ARA-M, telecom, UnionPay and digital key AIDs to `aidlist.json` (@iceman1001)
- Changed `trace list -t 14a` - APDUs inside an ISO14443-4 session are named, error status words explained and SW1 SW2 highlighted (@iceman1001)
- Fixed `lf em 4x50 rdbl/info/dump`, `lf search` - RM was sent into the tag data instead of the second listen window on strong, AC-coupled signals, so every command failed; reads now retry until a reply deadline 1 s before the 5 s client timeout (@abou28)
- Fixed PM5 ssp_clk counter - uninitialised TMR2 input filter could drop edges, skewing ISO14443-A timing and `hf mf nested` (@adrian-stepien)
- Changed `lf cotag sim` - add and default to passive mode, improve field drop and activation sequence detection (@towelbyte)
- Fixed `lf noralsy clone` and `sim` - CN above 9999999 and years outside 1961-2060 are refused instead of cut (@matrix)
- Fixed `trace list` - Topaz commands end at the frames their code takes, ISO14443-A frames in Topaz traces and ISO15693 frames in iCLASS traces get their own CRC, five byte ISO14443-A answers and FeliCa frames get a real CRC check  (@matrix)
- Fixed `trace list` annotations - none reads past its frame, SEOS answers are no longer shown as reader commands, contactless S-blocks, 14b slots, Xerox WUP2 and the Hitag µ field offsets are corrected (@matrix)
- Fixed `trace list -t ht2` and `-c` - a frame over 30 bytes no longer overwrites the Hitag 2 state or stops the client, a frame longer than the rows shown no longer overflows the row buffer (@matrix)
- Fixed `trace extract` - no write past the UID buffer or read past the trace, DESFire AES challenges are extracted again (@matrix)
- Fixed `lf indala clone`, `sim` and `brute` with `--fc`/`--cn` - parity and checksum bits as on genuine cards (@matrix)
- Fixed `lf hitag wrbl`, `hts wrbl` and `htu wrbl` - a page out of range or data shorter than 4 bytes is refused (@matrix)
- Fixed `lf hitag eview -1/-s/-m` and `htu rdbl -c` - no read past the label table or the reply (@matrix)
- Fixed `lf hitag` and `lf search` Paxton id - no garbage before the form factor (@matrix)
- Fixed `lf hitag hts rdbl`, `hts -m` and `ta -h` - page 32 under LCK1, mode above 3 refused, help shown (@matrix)
- Fixed `lf noralsy demod` - one demod error outside the frame, like a cut first bit, no longer loses the read (@matrix)
- Fixed `lf noralsy demod` - a card with the 0xBB0314FF first block no longer gets the unknown bits warning (@matrix)
- Added `lf destron sim` - simulates the FDX-A frame that `lf destron clone` writes (@matrix)
- Fixed `lf destron clone` - a uid that is not 5 bytes of 00-7F is refused instead of written as another id or with bad parity (@matrix)
- Fixed `lf fdxb` decoding - ISO 11784:2024 and ISO 24631-6 fields and names, and many more fixes. See commit message (@matrix)
- Fixed `lf fdxb clone` and `sim` - a country above 1023 or a national code above 274877906943 is refused instead of cut, `sim` sends the data block flag for `--extended 000000` (@matrix)
- Added `lf fdxb clone` and `sim` `--biothermo` and `--no-datablock` - a Bio-Thermo temperature, or any `--extended` trailer, sent with the data block flag clear as Bio-Thermo chips do (@matrix)
- Changed `lf fdxb reader` - each read tries 134 kHz, then 125 kHz for emulators tuned there (@matrix)
- Fixed payload for `CMD_HF_MIFARE_EML_MEMSET` in `hf_mfu_amiibo_sim.lua` (@tpglitch)
- Fixed `hf thinfilm sim` - a second simulation in the same client no longer ends at once with `Done!` (@matrix)
- Added `hf mf supercard -u` - UID change on super card generation 2 (@mishamyte)
- Changed `hf mf supercard` - a refused `-r`/`-u` now exits non-zero instead of reporting success (@mishamyte)
- Fixed `wiegand` formats - HPP32, H10320 and S12906b encode as laid out, HGen37 decode checks the right parity bit, the limits of nine formats match their fields, `wiegand list -h` shows its own usage (@matrix)
- Fixed `hf secc sim` - the ATQA codes the UID size, so readers that check it select a 7 or 10 byte UID, and a zero UID prints as taken from emulator memory (@matrix)
- Fixed `lf keri clone` and `sim` - values the frame can't carry are refused instead of cut, `sim` prints the id and its raw, the help example and the clone hint name `--id` and `lf keri reader` (@matrix)
- Fixed `lf paradox clone` and `sim` - FC above 255 and CN above 65535 are refused instead of cut, the clone hint names `lf paradox reader` (@matrix)
- Fixed NDEF TLV decoding - a NULL TLV has no length, Lock and Memory Control sizes of 0 mean 256 and their page fields are powers of two (@matrix)
- Fixed flashing with redirected output - the progress animation is drawn only when stdout is a terminal (@matrix)
- Fixed `proxmark3 --flash`, `--dumpmem` and `--reboot-to-bootloader` exit status - a failure exits 1, a failed flash no longer ends with `All done` (@matrix)
- Fixed `data rawdemod` - `--max`, `--samples` and the `--p2` clock no longer wrap above 255, `--fs` refuses values above 255 (@matrix)
- Fixed `mfulc_des_brute` build on macOS without `openssl@3.6` - OpenSSL is found with `brew --prefix openssl` as in `mfd_aes_brute` (@matrix)
- Fixed CI - ProxSpace installs its packages again, WSL builds in its own file system, Ubuntu and CodeQL skip the slow Azure apt mirror (@matrix)
- Fixed `recovery.bin` on the AT91 platforms - the bootrom is padded to 0x102000 again, so the OS image is at its load address (@matrix)
- Added `hf mfu info` weak-PRNG backstop - detects counterfeit ULC via RndB collision (@Ryan4n6)
- Updated `/Getting_Started_With_PM5.md` (@innocentbystanderproxmark)
- Fixed `hf seos pacs`/`write` - 2K3DES GET/UPDATE DATA no longer fails silently with exit 236; print APDU exchange errors, check DES-CMAC, decode SELECT `85` at its real lengthm ()
- Fixed client build with `-march=x86-64-v3` (CentOS/RHEL 10 default) - false `-Wstringop-overflow` in the EMV AID list table (@r3wretrhy)
- Changed `hf seos info` - now prints UID, ATQA, SAK, ATS, MIFARE Classic emulation, which SEOS AIDs answer and the known AIDs found (@antoniovazquezblanco)
- Fixed `hf seos managekeys -p` - key table columns stay aligned for slots 10 and up (@antoniovazquezblanco)
- Fixed `aidlist.json` - uppercased the SEOS, TeslaDAP and LA Tap AIDs (@antoniovazquezblanco)
- Changed `hf seos` - a failed MAC check with an empty privacy key slot now says so (@antoniovazquezblanco)
- Fixed `hf 14a info -s` - only hints `emv reader` when an EMV application is found, not for any AID (@antoniovazquezblanco)
- Fixed `aidlist.json` - tagged 25 more payment AIDs (UnionPay, MIR, eftpos, girocard, US common debit, ...) as type `EMV` (@antoniovazquezblanco)
- Improved PM5 nonce-acquisition throughput for mfc by replacing the fixed 68-pair cap with a 2900 ms target budget (@nip-was-here)
- Fixed PM5 tick-count reads across RTC second rollovers to prevent false receive timeouts
- Fixed iso14443a - reject incomplete encrypted nonce frames instead of delaying nested authentication on PM5 (@nip-was-here)
- Fixed PM5 tick-count reads across RTC second rollovers to prevent false receive timeouts (@nip-was-here)
- Added `lf cotag sim` - simulate COTAG tag (@towelbyte)
- Added standalone mode `GDB_GATEWAY` (PM5): GDB Remote Serial Protocol server on the USB CDC link, flash and SRAM access, no probe needed (@nieldk)
- Fixed weak fc/2 PSK tags (Indala, NexWatch, IDTECK, ...) misreading or failing: when the wave tracker has errors, the PSK demod falls back to `pm3_psk_demod` (@mfcarroll)
- Fixed `lf t55xx detect -p` - the password is no longer sent to a tag whose PWD bit could not be confirmed (@mfcarroll)
- Fixed `hf mf hardnested`/`hf mf autopwn` on PM5 with hardened MIFARE Classic EV1 (#3595, #3664)  (@ByteOrderMarc)
- Added PM5 FPGA open source (@xianglin1998)
- Fixed `lf search`, `lf t55xx detect`, `data modulation` and the TI/Visa2000/ZX8211 demods leaking the saved graph and demod buffers on every call (@r3wretrhy)
- Updated `/Getting_Started_With_PM5.md` (@innocentbystanderproxmark)
- Fixed corrected `sim014.bin` references to `sim024.bin` as `sim014.bin` does not exist in this repo currently, thus removing some newbie confusion. (@innocentbystanderproxmark)
- Added `A1856618` (an EM4305 anti-clone card password) to the `t55xx_default_pwds` dictionary used by `lf em 4x05 chk` (@nemanjan00)
- Added `hw bwm ble` - the BWM's BLE settings: `on`/`off` (persisted radio switch), `pairing` (require a 6-digit passkey, LE Secure Connections; the module ships open), `forget` bonded devices, `txpower`, `status` (@Msprg)
- Changed `hw bwm autooff` - `--idle <sec>` adds an opt-in power-off after that long idle on battery (no command, button press or BLE/WiFi client) (@Msprg)
- Added `hw bwm wifipower` - turn the BWM WiFi fully off, or set its modem power-save type (none/min/max), persisted on the module (@Msprg)
- Added `hw bwm powersave` - show/set the BWM (ESP32) power-save switch: DFS, light sleep and slow advertising after 30 s, or the stock always-on behaviour, persisted on the module (@Msprg)
- Added `hw powersave` - PM5 low-power idle, on by default: between commands the core drops to 48 MHz with the PLL off, the FPGA clock stopped and the CPU halted (WFI); USB now runs crystal-less off HICK' (@Msprg)
- Added `CMD_PM5_BWM_GET_BATTERY` on PM5 - compact structured battery/charger telemetry (SoC, voltage, current, remaining/full capacity, temp, health, charger fault/status) (@TomHarkness)
- Fixed `lf t55xx dump -p` - reuses the password already confirmed by `lf t55xx detect`  (@iceman1001)
- Fixed `lf t55xx p1detect`, `lf search` - T5577 clones with a non-Atmel manufacturer byte now recognised (@iceman1001)
- Added Flipper Zero link on PM5's Type-C extended port (handshake + NG frames over SPI) - tracked in #3667 (@TomHarkness)
- Fixed PM5 CEP hang - missing `StartTicks()` before `I2C_init()` in `cep_attach_poll()`/`cep_init()` could freeze the main loop, e.g. during `hf search` (@TomHarkness)
- Fixed PM5 CEP reply corruption - `receive_ng()` zeroed the payload buffer between two time-critical SPI reads, losing bytes on CEP's FIFO-less receiver (@TomHarkness)
- Added `CMD_CEP_STATUS` on PM5 - a compact snapshot (CEP handshake active state, battery SoC, firmware version string) for CEP clients  (@TomHarkness)
- Fixed `hf mf autopwn` on FM11RF08S - static encrypted nonce is now detected via the backdoor key (@iceman1001)
- Added `hf mfu view -v` - prints the UL-C 3DES key or UL-AES key stored in the dump file (@iceman1001)
- Added `hf mf sen --ns` - skip writing the key and dump files (@iceman1001)
- Fixed `hf mf autopwn --ns` on FM11RF08S - the flag is now honoured by the static encrypted nonce recovery instead of being dropped (@iceman1001)
- Added `hf mf sen --suffix` - generated files can be named `hf-mf-<uid>-<dump|key|nonces>-<suffix>` (@iceman1001)
- Fixed `hf mf autopwn --suffix` on FM11RF08S - the suffix is now forwarded to the static encrypted nonce recovery instead of being dropped (@iceman1001)
- Fixed `hf mf sen` and `hf mf autopwn` on FM11RF08S - the recovered card dump is now saved as JSON as well as BIN (@iceman1001)
- Fixed `hf mfdes chk` - a key given with `--key` is now tried every round instead of being overwritten by the dictionary and pattern fills (@iceman1001)
- Fixed `hf mfdes chk` - when key settings are unreadable, key numbers are found with GetKeyVersion instead of the file access rights, which are gated by the same bit (@iceman1001)
- Fixed `hf mfdes chk` - the PICC master key is now checked, and a card that refuses GetApplicationIDs no longer aborts the whole run (@iceman1001)
- Fixed `hf mfdes chk` and `hf mfdes detect` - AuthenticateISO (0x1A) is read on its own instead of only when Authenticate (0x0A) answered, so an application holding 3TDEA keys is no longer skipped (@iceman1001)
- Changed `hf mfdes detect` - a key type the application does not hold is dropped on the first answer instead of after the error counter fills (@iceman1001)
- Changed `hf mfdes view` - now names known applications from the `aid_desfire` dictionary (@iceman1001)
- Added new entries from Metrodroid project into our AID json (@iceman1001)
- Changed `hf iclass view` - SIO Insights now surfaces unhandled TLV tags (e.g. the [5]/[9] context tags seen on SR credentials) instead of silently dropping them (@youngh82)
- Changed `hf mfdes view` - decodes the card issuer header of NORTIC travel cards, the Norwegian national public transport card (@iceman1001)
- Added `hf mf view` - decodes RKF travel cards, the Nordic public transport card (Resekortsföreningen i Norden) on MIFARE Classic (@iceman1001)
- Fixed `hf 14a info`  now correctly handles NAK when receiving RATS (@iceman1001)
- Fixed `hf mfu restore` to support Ultralight AES (@iceman1001)
- Fixed `hf mfu info` to print UL-AES config pages again (@iceman1001)
- Changed `hf search` - checks and provides hints about SEOS tags (@antoniovazquezblanco)


## [Frosty Lemon.4.23346][2026-09-18]
- Fixed `hf mfdes sim` - the ATS now advertises 106 kbit/s only, PPS to a higher rate is refused, a selected card ignores REQA/WUPA, and ISO-DEP block recovery now follows the card sequence (@mistial-dev)
- Added `hf mfdes sim` - answers ISO 7816 SELECT by DF name or file id with real status words, so a reader that opens with a SELECT reaches the card (@mistial-dev)
- Fixed `hf mfdes sim` - a session opened with the 0x0A legacy authentication now uses the legacy secure messaging: 4 byte DES MAC, CRC16, no MAC on plain answers (@mistial-dev)
- Fixed `hf mfdes sim` - the 0x0A legacy authentication now enciphers the reader token and RndA' from a zero IV as a card does, so the session key and final frame match genuine silicon (@mistial-dev)
- Added `hf mfdes etest` - drives the DESFire simulation over USB without RF, with injectable RndB / random UID (@mistial-dev)
- Fixed USB receive stalling on a zero-length OUT packet and missing a command already buffered behind another (@cindersocket)
- Fixed dictionary loading to skip keys longer than the requested key size instead of truncating them (@iceman1001)
- Changed `hf mfdes chk` to check every key number the application declares, not only the ones a file's access rights name (@iceman1001)
- Changed `hf mfdes chk` to report when no keys were found and to stop spending a card round trip on an exhausted dictionary (@iceman1001)
- Changed `hf mfu cchk` and `hf mfu aeschk` into one `hf mfu chk` which detects UL-C vs UL-AES (@iceman1001)
- Added `hf 14b view` - MyKey / COGES keys on SRIX4K now decode (@iceman1001)
- Added `hf 14b view --selftest` - runs the MyKey parser self tests (@iceman1001)
- Removed `hf 14b valid` - the SRIX4K scrambler stub it wrapped is finished and lives in the MyKey parser (@iceman1001)
- Fixed build too large for 256kb images by guarding WITH_DESFIRE_SIM (@iceman1001)
- Fixed `pm3 flash` - the progress bar no longer keeps SIGINT after it finishes, CTRL-C works again afterwards (@iceman1001)
- Fixed `CTRL-C` / `CTRL-Z` - the client owns its signals instead of letting readline re-raise them, CTRL-C quits cleanly saving the history and CTRL-Z suspends reliably (@iceman1001)
- Fixed `CTRL-C` / `CTRL-Z` - readline re-raised caught signals to itself, which in a threaded client looped instead of quitting or suspending (@iceman1001)
- Fixed `pm3 flash` - the progress bar no longer keeps SIGINT after it finishes, CTRL-C works again afterwards (@iceman1001)
- Changed `pm3 history` - written after every command, so it survives a kill instead of only a clean exit (@iceman1001)
- Changed `memset` - the device's own implementation now fills a word at a time, the same treatment `memcpy` got.  (@iceman1001)
- Changed `receive_ng` - the command payload is zeroed when a packet actually arrives rather than on every idle pass of the main loop.  Thanks @Msprg (@iceman1001)
- Changed `hf mfdes chk` - runs with no arguments now, falling back to the bundled dictionary the way `hf mfdes detect` already does (@iceman1001)
- Changed `hf mfdes chk` - tidier output(@iceman1001)
- Fixed `hf mfdes sim` - a chained write is answered instead of being refused with 91 1C on its second frame. (@iceman1001)
- Fixed `hf mfdes sim` - an error status now ends the authenticated session, as M134034 7.3.4 requires of a card (@iceman1001)
- Fixed `hf mfdes sim` - answers are sized from the frame size the reader asks for in its RATS instead of a fixed 96 bytes (@iceman1001)
- Fixed `hf mfdes sim` - the length of a write is taken from the 3 byte length the command carries rather than guessed from how full a frame looked (@iceman1001)
- Fixed `hf mfdes detect` - the same cipher is no longer sent to the card twice, cutting a PICC level scan with the bundled dictionary from 99 authentications to 63 (@iceman1001)
- Changed `hf mfdes detect` - a single key number is printed as one value instead of a range of one, so a PICC level scan reads `key num 0x00` (@iceman1001)
- Fixed `hf mfdes sim` - an enciphered read longer than one frame is chained instead of refused, the init vector carrying from frame to frame (@iceman1001)
- Fixed `lf read` - a realtime read no longer ends early when the host falls behind, and the device no longer goes silent until it is replugged (@iceman1001)
- Fixed `lf read` - a realtime read ending on a full USB packet now delivers its last 64 bytes, an odd packet count was silently one packet short (@iceman1001)
- Fixed `lf read` - a transfer that stops short says so, instead of being presented as a complete read (@iceman1001)
- Changed `lf read` / `lf sniff` - the sample count is capped to the graph buffer size, the surplus was streamed and then thrown away (@iceman1001)
- Added `hf mfdes sim` - SetConfiguration is answered, so a reader can disable FormatPICC, switch the card to a random id, or give it a user defined ATS (@iceman1001)
- Added `hf mfdes sim` - ReadSignature is answered for a card image of a generation that has one, and refused the way a genuine EV1 refuses it (@iceman1001)
- Added `hf mfdes sim` - GetKeyVersion is answered, so a reader can read the version of any key without knowing the key itself (@iceman1001)
- Added `hf mfdes sim` - ChangeKeySettings is answered, enciphered and master key only, and clearing the configuration changeable bit is one way as it is on a card (@iceman1001)
- Added `hf mfdes sim` - ChangeKey is answered, both the plain and the XORed key frame, so a reader can rekey the simulated card and the new keys are in what `hf mfdes esave` writes out (@iceman1001)
- Added `hf mfdes sim` - ChangeFileSettings is answered, plain when the change access right is free and enciphered under the key it names otherwise (@iceman1001)
- Added `hf mfdes esave` - `--keep` saves applications and files a reader deleted, which are otherwise left out so the dump is the card as a reader sees it (@iceman1001)
- Added `hf mfdes sim` - CreateFile in all five EV1 flavours and DeleteFile are answered, so a reader can lay out an application's file system on the simulated card (@iceman1001)
- Added `hf mfdes sim` - CreateApplication and DeleteApplication are answered, so a reader can add and remove applications on the simulated card (@iceman1001)
- Added `hf mfdes sim` - FormatPICC is answered, releasing every application and file and handing the memory back, with the PICC master key and its settings kept (@iceman1001)
- Fixed `hf mfdes esave` - the ATS was saved as zeros, a `sizeof()` of the 256 byte destination cast to uint8_t wrapped to 0 and the copy was skipped while its length was still reported (@iceman1001)
- Added `hf mfdes sim` - DESFire EV1 simulation now answers authentication, secure messaging, file reads, writes and transactions from the card image in emulator memory (@iceman1001)
- Fixed `hf mfdes sim` - a 2TDEA key whose two halves match is handled as a single DES key, session key generation included, so the session key matches what a genuine PICC derives (@iceman1001)
- Fixed `hf mfdes sim` - the CMAC of a chained answer is taken over the whole transfer instead of being restarted on every 0xAF continuation (@iceman1001)
- Fixed `PLATFORM_EXTRAS=BWM` - a command started over the Proxmark5 wireless module can be aborted again. (@tweathers-sec)
- Fixed `lf search` - a leftover debug print no longer reports `DEBUG: detectindala | <n>` on every failed Indala demodulation (@tweathers-sec)
- Changed `memcpy` - the device's own implementation now copies a word at a time when source and destination allow it (@iceman1001)
- Changed NG replies - the outgoing frame is offset so its payload lands word aligned, taking USB transfer speed from 630 to 789 kB/s on RDV4  (@iceman1001)
- Fixed `AT91F_USB_SendStall` - waiting for the host to collect the STALL is now bounded (@iceman1001)
- Fixed `GET_LINE_CODING` - the CDC line coding struct is now packed to the 7 bytes the spec defines (@iceman1001)
- Fixed USB control transfers - a descriptor whose length is an exact multiple of the 8 byte control endpoint now ends with a zero length packet (@iceman1001)
- Fixed USB WCID descriptors - the device advertised Microsoft OS descriptor support in string 0xEE but stalled the vendor request that follows (@iceman1001)
- Added `hf mfdes sim` - answers GetDFNames and GetISOFileIDs, so a reader sees the ISO file ids and DF names of the simulated card (@iceman1001)
- Fixed `hf 14a` - an FPGA bitstream download no longer destroys the emulator memory that `eload` just filled (@iceman1001)
- Changed `hf mfdes sim` - now a self contained DESFire simulation, answering both native and ISO 7816 wrapped commands (@iceman1001)
- Fixed `hf mfdes` - GetDFNames is no longer sent inside an authenticated session, which permanently disabled DESFire EV1 cards (@iceman1001)
- Added `hf mfdes` - the five DESFire status codes that mean the card damaged or disabled itself are now named instead of reported as `-20` (@iceman1001)
- Fixed `hf mfdes` - a command error now drops the client's secure channel, since the PICC has already ended the authentication (@iceman1001)
- Fixed `hf mfdes` - file settings that could not be read are no longer reported and dumped as a zero byte standard data file (@iceman1001)
- Fixed `hf mfdes pc` - the proximity check commands now check the card's status byte instead of treating an error as success (@iceman1001)
- Changed `hf mfdes sim` - now runs through the shared ISO14443-A simulation loop as tag type 3 (@iceman1001)
- Added `hf mfdes eload/esave/eview` - load a DESFire card dump into emulator memory (@iceman1001)
 - Fixed `PLATFORM=PM3ICOPYX` - builds again, it has no FPGA power switch and the AT91 GPIO layer used the pin unconditionally (@iceman1001)
- Fixed `fpga_compress` - PM3ULTIMATE builds again, its bitstreams are an exact multiple of the interleave size (@iceman1001)
- Fixed `hf legic eload` - an upload after a command that left a different FPGA bitstream loaded now writes the whole image (@iceman1001)
- Fixed `hf legic sim` - no longer wipes the emulator memory that `hf legic eload` just filled (@iceman1001)
- Changed emulator memory - raised from 4096 to 8192 bytes (@iceman1001)
- Fixed `hf 14a sim` - a dynamic response now gets a modulation buffer sized to fit it, instead of one 68 bytes too small (or 1788 too large for ST25TA) (@iceman1001)
- Fixed `BigBuf_malloc` - takes a uint32_t, so a request of 64KB or more fails instead of wrapping to zero (@iceman1001)
- Fixed `BigBuf_max_traceLen` - returns a uint32_t, so LF sampling on a device with more than 64KB of BigBuf is not handed a truncated buffer (@iceman1001)
- Fixed PM5 BigBuf allocation
- Changed `hf mf view` - now show which VIGIK fields are RSA signed (@iceman1001)
- Changed `hw status` - now reports EMULATOR memory (@iceman1001)
- Changed `hf mf view` - Urmet Captiv cards now decode their number field and data regions (@iceman1001)
- Changed `hf mf view --selftest` - now also checks the Hexact payload decoder, both ways (@iceman1001)
- Changed `hf mf view` - Hexact sector 9,11 payload is now decoded and cross checked against sector 0,15 and UID (@iceman1001)
- Added `hf mfdes dump` - now walks the whole PICC and saves a `hf-mfdes-<UID>-dump.json` card image, picking up `hf-mfdes-<UID>-keys.json` on its own (@iceman1001)
- Added `hf mfdes view` - print a DESFire card dump file, no device needed (@iceman1001)
- Fixed `sprint_hex` - a buffer twice the old size, so printing a long block no longer runs off the end of the static buffer (@iceman1001)
- Added `vigik_recover_pk.py` - recover public keys from RSA signatures (@iceman1001)
- Fixed `hf mf autopwn` - a sector that cannot be cracked no longer discards the keys already recovered (@iceman1001)
- Fixed `hf mf nested` - a NAK from the tag now ends the nonce collection instead of retrying until the client times out (@iceman1001)
- Fixed `hf mfdes` - select / authenticate failures are now reported once, naming the AID and the failing step (@iceman1001)
- Fixed `-f <fn>` - a filename given with its own extension is now used as-is on both load and save (@iceman1001)
- Fixed `hf mf view` - VIGIK signature verification now recovers the ISO 9796-1 message and checks it against the card (@iceman1001)
- Added `hf mf view --selftest` - recovers and checks the signature of a known good expired VIGIK card (@iceman1001)
- Changed `hf mf view` / `hf mf dump --ns` - identify VIGIK family systems (@iceman1001)
- Added `parsehid.c` - HID PACS decoding for MIFARE Classic dumps
- Added `parsevigik.c` - VIGIK PACS decoding for MIFARE Classic dumps (@iceman1001)
- Changed `hf mf view` - now decode a HID PACS credential (@iceman1001)
- Changed `hf mf dump --ns` - now decode a HID PACS credential (@iceman1001)
- Fixed `fpga_compress -d` - a single bitstream archive was unpacked 2384 bytes short, the trailing partial interleave chunk was dropped (@iceman1001)
- Fixed `fpga_compress` - a build with only one FPGA bitstream (`SKIP_LF`, `SKIP_FELICA`, `SKIP_ISO15693`) packed it as a single 1 MB block the ARM could not decompress (@iceman1001)
- Fixed `FpgaDownloadAndGo` - a failed FPGA decompress leaked the ring buffer, leaving BigBuf 16 kB short for the rest of the session (@iceman1001)
- Added `hf mf dump --sk` - save extracted keys to binary file (@iceman1001)
- Changed `hf mf dump --ns` - now runs the same analysis as `hf mf view`(@iceman1001)
- Changed `tools/pm3_online_tests.sh desfire_value` - the plain/mac assertion pairs sent byte identical APDUs (@iceman1001)
- Fixed `smart raw --t1` - a card runs the protocol its ATR names until a PPS changes it (@iceman1001)
- Fixed `tools/pm3_online_tests.sh smartcard` - the T=0 checks used the non existent `-0` flag (@iceman1001)
- Fixed `hf mfdes dump` - the ISO file id probe ends the PICC session on applications without ISO file ids (@iceman1001)
- Changed `hf mfdes lsfiles` / `hf mfdes dump` - no longer warn about a missing ISO file id list (@iceman1001)
- Fixed `hf mfdes read` / `hf mfdes write` - a file whose access right is free (0x0E) is served in plain by the PICC (@iceman1001)
- Fixed `hf mfdes value` - credit/debit/limitedcredit used a fixed comm mode, now look on card (@iceman1001)
- Fixed `hf mfdes value -o get` / `hf mfdes dump` - a value file read in the wrong comm mode printed a silently wrong value instead of failing (@iceman1001)
- Fixed `lf idteck demod` - without `--raw` it decoded a zeroed stack buffer instead of the graphbuffer, always reporting card id 0 (@mfcarroll)
- Fixed `lf idteck demod` - a raw frame without the Idteck preamble printed `No genuine IDTECK found` and then announced a tag anyway (@mfcarroll)
- Changed `lf idteck demod` - no longer prints an HID H10301 credential built from the Idteck card id (@mfcarroll)
- Added `trace clear` - clears the tracebuffer (@iceman1001)
- Changed `trace save -1` - now downloads from device by default (@iceman1001)
- Added `hf 14a antifuzz --coll` - the UID collides from the 9th bit (@iceman1001)
- Changed `trace list -t 14a` - decodes SEL 0x99..0x9F, shows how many UID bits a bit oriented ANTICOLL claims (@iceman1001)
- Fixed `hf iclass encode` / `hf iclass tagsim` / `hf mf encodehid` / `lf hid sim` / `lf hid clone` / `lf hid brute` - `--cn` was read with a 32 bit getter (@iceman1001)
- Changed `hf mf eload` - the device now zeroes the emulator memory before the upload (@iceman1001)
- Fixed `hf mf sim` / `hf 14a sim` - the anticollision responses were prepared in BigBuf before `iso14443a_setup()` (@iceman1001)
- Changed `CAPABILITIES_VERSION` to 10 - `hf mf eload` sends a flags byte the device did not have before, so client and firmware must be flashed as a pair (@iceman1001)
- Changed `mem spiffs upload` - the file is held open across packets instead of being reopened by name for each one (@iceman1001)
- Added `hf thinfilm sniff` - passively records the frames a Thinfilm / NFC Barcode tag beams at a reader (@iceman1001)
- Fixed `hf thinfilm sim` - it now holds the genuine tag's measured 65536 carrier period frame rate instead of a fixed delay (@iceman1001)
- Changed `mem spiffs upload`/`mem spiffs dump` - both now print inline progress, and a refused write is reported with the byte it stopped at instead of claiming success (@iceman1001)
- Changed real-time LF sample reads `lf read`, `lf sniff`, `lf cotag reader`(@iceman1001)
- Changed the `INPLACE` spinner prefix (@iceman1001)
- Fixed `mem spiffs dump` - a file of 64K or more wrapped the uint16_t `BigBuf_calloc()` takes, the download now streams one frame at a time (@iceman1001)
- Fixed SPIFFS flash layer - a failed erase was reported to SPIFFS as success, letting it write into an un-erased sector where NOR AND-semantics silently mangled file data (@iceman1001)
- Fixed `hf thinfilm sim` - a 32 sample field read between every frame cost 3.8ms and nearly doubled the frame repeat period (@iceman1001)
- Added `hf thinfilm sim` - it now traces, so `trace list -t thinfilm` works on the sim side (@iceman1001)
- Fixed `hf texkom reader -v` - now no ovewflow in the general decoder (@iceman1001)
- Changed `trace list` - the Hitag U and Hitag S annotators now build their text with the `strlen()` offset (@iceman1001)
- Fixed `hf felica` - the service code footer wrote its closing bar past the end of a 128 byte buffer (@iceman1001)
- Fixed `trace list` - a Hitag U LOGIN or WRITE SINGLE BLOCK frame could write up to 17 bytes past the end of the 60 byte annotation buffer (@iceman1001)
- Fixed `hf mf autopwn` - it now re-authenticates and retries the block with the other key (@iceman1001)
- Fixed `hf mf hardnested` - the device declared `num_nonces * 4` bytes but packs 9 bytes per nonce pair. Thanks @TheArchitect0880 (@iceman1001)
- Changed `hf mfdes detect` - with no `-n` it now sweeps every key number the application declares instead of only key 0 (@iceman1001)
- Fixed `hf mfdes detect` - the card error counter was shared by every key type, so a bad run during the DES pass could abort the AES pass (@iceman1001)
- Fixed `hf mfdes detect` - with no `-f` it drew candidates from the MIFARE Plus 16 byte key list, it now defaults to the bundled `mfdes_default_keys` dictionary (@iceman1001)
- Fixed `hf mfdes detect` - LRP was only found when the key settings happened to be unreadable (@iceman1001)
- Added `hf mfdes chk --schann` - now detect the secure channel EV/EV2/LRP mode; detected per application and can be overridden (@iceman1001)
- Fixed `hf mfdes chk` - found keys were tracked per key number instead of per application, so a key number recovered on one AID was skipped later (@iceman1001)
- Fixed `data modulation` - the 160 sample antenna settle trim was never undone, so the graph buffer stayed short for everything after it (@mfcarroll)
- Changed `tests/lf_t55xx_writetest.lua` - reads each block three times and names how a bad read differs (`rol1`, `ror5`, `inverted`, `shr1`) instead of only pass/fail (@mfcarroll)
- Fixed `lf t55xx detect` - PSK1 now detects at every bit rate and subcarrier. (@iceman1001)
- Fixed `lf t55xx detect` - FSK was skipped entirely when the field-clock pair measured as neither legal pair, losing FSK1 at RF/32 and RF/40 and every variant at RF/16 (@iceman1001)
- Fixed `lf t55xx detect` - the block 0 rotation was picked by scan order; the measured broadcast period now settles it (@mfcarroll)
- Fixed `lf t55xx detect` - the psk antenna-settle trim left the graph 160 samples short, so the graph-sample anchor was discarded on most block reads (@mfcarroll)
- Fixed `lf psk demod` - lead-in samples were accepted as a phase shift, complementing the rest of the word (@mfcarroll)
- Fixed `lf fsk demod` - a leading run too short to be a bit was forced to one, fabricating a bit and shifting the word (@mfcarroll)
- Fixed `lf nrz demod` - the samples before the first level change were counted as bits, rotating the word whenever that edge moved (@mfcarroll)
- Fixed `lf t55xx dump/read` - blocks were extracted at a bit offset cached from the last `detect`  The offset is now anchored in graph samples (@iceman1001)
- Fixed `lf t55xx write --verify` - a successful write could be reported as a validation failure (@iceman1001)
- Fixed `lf t55xx detect` - the automatic detect after a block 0 write reported a password that was never supplied (@iceman1001)
- Fixed `hf emrtd info` - now correctly pretty print algos (@iceman1001)
- Fixed `trace list -t topaz` - now parses topaz reader frames up to 16 bytes (@iceman1001)
- Fixed `hw bwm` - doc and comment references still named the flat `hw bwmsetcap`/`bwmcharge`/`bwmautooff` commands from before the subgroup move (@xilni)
- Added TA1=96 support via pwm to sim module `sim024.bin` - v4.66 (@antiklesys)
- Added `hf mfu ndefformat` - NDEF format Ultralight/NTAG tags, restores the NXP factory Capability Container for the detected type (@0x6r1an0y)
- Changed `lf hitag` - refactoring hitag2 operations and now they work much better (@iceman1001)
- Fixed `lf hitag list` - now prints in same style like the rest (@iceman1001)
- Changed pathing - client now supports `~/` tilde sign in paths (@iceman1001)
- Added `hf_legic_rdv4` - Standalone mode for managing 7 persistent legic dumps (@digitalhoax)
- Fixed `lf t55xx detect` - dropped the psk3 candidates (@mfcarroll)
- Fixed `lf t55xx detect` - a psk3 tag read back as psk2 with a block 0 one bit out (@mfcarroll)
- Added doc/md/PM5_Start_Here/Getting_Started_With_PM5.md now exists as a starting point for those who have their PM5 (@innocentbystanderproxmark)
- Fixed `tools/ePassport` - the personal number read from the MRZ optional-data field showed a filler character where a space belonged (@pkilar)
- Fixed `tools/ePassport` - the detail tabs named the files their data came from as fixed text, so a document without EF_DG11 was told otherwise, and the personal-number caption wrapped (@pkilar)
- Added `tools/ePassport` - EF_DG13 is decoded, and a Polish document's PESEL is shown as the personal number when it carries no EF_DG11 (@pkilar)
- Fixed `tools/ePassport` - a disabled button drew Kivy's banded disabled texture, which left its label unreadable (@pkilar)
- Fixed `hf 14b` - a card asking for a waiting time extension of 4 or more left the timeout at zero, so any ISO14443-B card needing time to answer looked like it had stopped responding (@pkilar)
- Fixed `hf mfdes chk` - now handles `-k` user supplied keys properly (@iceman1001)
- Fixed `BigBuf_Clear_keep_EM()` - now also clear the trace length set (@iceman1001)
- Fixed `BTADDON` - rx ring reported empty when filled to exactly its size, silently dropping 1kB of received data; now read straight from the PDC banks (@iceman1001)
- Changed `hf 15|iclass` - poll for an abort once per 1024 sample bytes helps BTADDON builds stop blowing the DMA circular buffer (@iceman1001)
- Fixed `tools/ePassport` - a read that dumped nothing is now reported as a failure with the client log kept beside the dump, and EF_CardAccess/DG14 protocols are decoded and correctly named (@pkilar)
- Fixed `tools/ePassport` - FILES and LOG panes no longer go blank on long content, and picture files now show the picture (@pkilar)
- Fixed `hf xerox view` - now rejects a dump file too small to contain the info blocks instead of reading past it (@munzzyy)
- Changed NG frame layout to align better with 64bytes frames. From 512 -> 624bytes (@iceman1001)
- Changed `hf plot` - converted to NG frame (@iceman1001)
- Fixed `hf mfu cchk` - now 3-pass key check all keys sent to device  (@iceman1001)
- Changed iso15693 on device side to share one response buffer across the reader helpers (@iceman1001)
- Changed version data on device side to not be inlined, freeing 1.5kB of stack in the main loop (@iceman1001)
- Changed `lf pcf7931` - now use bigbuf_calloc and much smaller array (@iceman1001)
- Changed `hf 15 sniff` - now uses bigbuf_calloc and lessen ram footprint on device (@iceman1001)
- Added individual json dump formats for ht2, ht1, htS, htU. No more .bin files either for hitag (@iceman1001)
- Change NG: iso14a_raw_cmd_t, convert hf 14a raw, dual-mode reader handler (@iceman1001)
- Support for flipper ISO15693.nfc dump files (@iceman100)
- Change NG: convert CMD_LF_EM4X50_ESET to em4x50_eset_t  (@iceman1001)
- Change NG: convert CMD_LF_T55XX_SET_CONFIG to t55xx_setconfig_t (@iceman1001)
- Added a new ISo15693 v5 dump file format to handle 0x100 number of pages and 8 byte pages better (@iceman1001)
- Fixed ISO15693 v4 file handling and converting to v5 (@iceman1001)
- Fixed clock  `SpinDelayUsPrecision` to not wrap. Now more 43ms issues (@iceman1001)
- Changed `hf felica raw` -  now can stream per-window envelope for better measurments (@iceman1001)
- Updated `fpga_pm3_felica.bit` - FeliCa signal handling got refactored. (@iceman1001)
- Changed client tab completion - vocabulary is now built at runtime from the live command tree and uses the same availability rules as `help` (@Msprg)
- Fixed standalone modes - the one second button hold trigger was a no-op on all non-PM5 platforms, leaving `hw standalone` over USB as the only entry point (@ShawInnes)
- Added `sim022.bin` - enabled burst mode transfers, which allows us to do TA1=96 in speeds (@iceman1001)
- Fixed `hf felica liteauth` - empty long option names for `-c` and `-k` made argtable read past the string (found by ASAN on `--fulltext`) (@Msprg)
- Added `sim020.bin` - v4.60 of sim module firmware, better T=0 handling and clock etu handling (@iceman1001)
- Fixed `hf seos sam` - now have a invalid pacs guard (@iceman1001)
- Changed i2c comms to auto-negotiation and use ATR-keyed rate cache for speedier smart card comms (@iceman1001)
- Changed `emv search` - now has better table output (@iceman1001)
- Changed client side polling. Should make client more spiffy (@iceman1001)
- Fixed `trace list -t 7816` - now handles contact vs contactless annotation better (@iceman1001)
- Added `smart pps` - ISO 7816-3 protocol and parameter selection exchange (@iceman1001)
- Added `sim015.bin` - sim modul firmware now supports T=0 and T=1 (@iceman1001)
- Added tools script `tools/ePassport/ePassport` - kivy python3 script that dumps a passport with BAC/CAN and a nice viewer (@iceman1001)
- Added tools script `external_sam_read.py` enabling PM3 3 Easy and Proxmark 5 to use an external SIM/SAM reader for `hf iclass sam` and `hf seos sam` commands (@antiklesys)
- Added `nfc encode` and `hf mfu ndefwrite` - build NDEF URI/Text/AAR records and write them to Ultralight tags (@0x6r1an0y)
- Fixed `hf emrtd info` - the picture viewer now scales images to the window instead of pinning them at native size (@iceman1001)
- Added `hf emrtd` - PACE-CAM (Chip Authentication Mapping) session support. The anti-clone proof itself is not yet verified (@iceman1001)
- Added `hf emrtd test` - offline regression tests for the PACE / secure messaging primitives (@iceman1001)
- Added `hf emrtd info/dump --can/--pace/--bac` - PACE-ECDH-GM support with MRZ or CAN passwords, AES-CMAC-128/192/256 and 3DES-CBC-CBC secure messaging (@iceman1001)
- Changed `hf emrtd info` - now shows EF_DG2 portraits for JPREG2000 via new dependecy lib (@iceman1001)
- Changed `hf emrtd info`- now shows portrait, signature and other biometrics at the same time (@iceman1001)
- Fixed `pm3_grabbed_output_get` - heap overflow (@jonyen)
- Fixed `hf iclass sam` - removed the old implementation (@iceman1001)
- Fixed (PM5) failing to re-enter the bootloader (@nemanjan00)
- Fixed `hw fpga config` (PM5) - now powers on the FPGA 24MHz clock (@nemanjan00)
- Fixed `hf mfdes chk` - now correctly check all provided keys instead of only checking a small portion (@corollary-de).
- Fixed `hf iclass unhash` - now omitting valid hash0 pre-images (@trichimtrich)
- Fixed `hf iclass unhash` - returning no pre-images at all for some keys, caused by `check()` not being invertible in a single reading (@trichimtrich)
- Added `--rgb` option to `hf tune` / `lf tune` (PM5) - mirrors the antenna tuning level on the antenna RGB LED (@nemanjan00)
- Fixed `hw version` / `hw status` (PM5) - now showing correct information for fpga images (@nemanjan00)
- Fixed `hw status` [Model] section reporting "PM3 GENERIC" firmware on Proxmark5 (PM5/AT32); it now reports `PM5` (@nemanjan00)
- Added `hw version` / `hw status` - now reports Proxmark5 (PM5/AT32) correct MCU (AT32F437) (@nemanjan00)
- Added CMake support for armsrc and bootrom (@xianglin1998)
- Added HAL support for ARM firmware (@xianglin1998)
- Fixed `hf mf dump` - preserving readable sector trailer Key B data (@oSPANNERo)
- Changed `magic_cards_notes.md` - documented the USCUID-UL helper scripts (`hf_mfu_uscuid` / `hf_mf_uscuid_prog`) (@c-barron)
- Fixed `hf_mf_uscuid_prog.lua` - corrected the script name shown in its usage text (@c-barron)
- Added `hf felica seacauth1` command (@kormax)
- Added `lf trovan` commands to support Trovan Animal ID (@iceman1001)
- Added support for emulating ST25TA tag (IKEA Rothult) with custom NDEF response (@sanduuz)
- Added `hf mf gdmgetblk/gdmgethidblk/gdmsethidblk/gdmsetuid/gdmwipe/gdmsetsig` (@0x6r1an0y)
- Changed `hf mf gdmparsecfg/gdmsetblk` (@0x6r1an0y)
- Renamed `hf mf gdmcfg` (@0x6r1an0y)
- Added `hf mfu aesgetuid` for retrieving the real UID when random ID is enabled, using the UIDRetrKey. (@lolbird123)
- Fixed `hf iclass view` reading past the end of dump files smaller than a picopass header (@munzzyy)
- Fixed `wiegand decode` returning facility code 16777339 instead of the encoded value for the IR56 (@munzzyy)
- Fixed `lf em 4x05 dump` writing byte-swapped (corrupted) block data to the saved dump file (@munzzyy)
- Added `hf 14b dump` support for Standard ISO14443-B tags (@thisiscamk)
- Added `hf 14b ctrdbl` and `hf 14b ctdump` commands for interacting with ASK CTS tags (@kormax)
- Fixed `hf legic migrate` failing to parse the optional DCF argument as hex (@IdanHo)
- Add support for parsing Finnish Helsinki Regional Transport (HRT) travel cards (@sanduuz)
- Added standalone mode `HF_DOEGOX_COMMIT` - DESFire suspended commit without relay (@doegox)
- Changed `hf 14b info`- now support Innovatron protocol detection (@kormax)
- Improved `lf cotag reader` and `lf cotag demod` - Reimplementation and enhancement of proxmark3 COTAG support (@towelbyte)
- Added `hf felica sim` command (@kormax)
- Added `mad read`, `mad write`, `mad verify`, `mad decode`, `mad encode` commands with typed struct MAD API (@AlxCzl)
- Added `hf mfdes getversion` command (@kormax)
- Fixed iCLASS emulator writes clearing previously loaded emulator memory after an FPGA reload (@cindersocket)
- Added DES transport mode support for iCLASS credential encode/decode helpers. (@cindersocket)
- Changed `hf iclass view` - now detect iCLASS SE SIO blocks from AIA values that point past block 6. (@cindersocket)
- Changed `doc/magic_cards_notes.md` - updated the ID82xx / Hitag µ clone section with current Proxmark3 support, chip variations, default passwords and detection notes (@mishamyte)
- Added two Mifare Classic keys into extensive dictionary which are hardcoded into Mifare Plus SE (@team-orangeBlue)
- Fixed `hf mfp rdbl` when using "read multiple" blocks by decrypting the entire buffer instead of one block only (@team-orangeBlue)
- Improved `hf mfp dump` execution speed by removing crypto+card restarts, approx. 40% faster (@team-orangeBlue)
- Added support for non-first authentication in Mifare Plus (@team-orangeBlue)
- Added CUDA version of tools/mfulc_des_brute (@C2Pwn)
- Added `hf mfu desbrute` command: native client support for ULC key recovery (@C2Pwn)
- Added `hf mf sen` command: native client support for FM11RF08S SEN recovery (@C2Pwn)
- Fixed an issue where the COM port was sometimes not being selected on Windows (@boss-chifra)
- Added Makefile `PLATFORM_FILE` variable to specify another Makefile.platform file (@doegox)
- Added `hf calypso probecmdcompat` command (@kormax)
- Added secure channel operations to `hf iclass sam` and fixed/improved I2C bus operations (@antiklesys)
- Added `hf calypso list` command (@kormax)
- Added `hf mfd verifycert` command (@kormax)
- Added `hf calypso dump` command (@kormax)
- Added `pm3trace_edit.py`  script for editing of pm3 trace files (@iceman1001)
- Added `hf calypso info` command (@kormax)
- Added ARM compilation support for picolibc, fixing compilation on Debian Forky (@doegox)
- Added `hf mdfes leaf` command (@pcunning)
- Added `hf mfdes vdesign` command (@kormax)
- Added `hf iclass liberate` - simple command to remove changes to MKF or Icopy-X iclass tags (@iceman1001)
- Fixed `mfpReadSector` function to decrypt received data, fixing commands like `hf mfp dump` (@team-orangeBlue) 
- Added `hf mfdes intauth` command (@kormax)
- Improved `hf iclass legbrute` throughput: added a 64-wide bitsliced MAC1 sweep that tests 64 key candidates in parallel per cipher tick. Thanks @chick3nman (@antiklesys)
- Improved `hf iclass legbrute` throughput further: added wider SIMD backends (128-lane NEON, 256-lane AVX2, 512-lane AVX-512F) Thanks @chick3nman (@antiklesys)
- Added `hf mfdes pc` command (@kormax)
- Fixed `hf mfu ndefread` NDEF data area size handling (@mjaksn)
- Fixed `hw qc_pm5` command to ensure the RGB LED is off at the conclusion of the test (@barefootdeveloper)
- Improved`hw qc_pm5` command to support specifying a timeout (@barefootdeveloper)

## [BREAKMEIFYOUCAN!.4.21611][2026-04-14]
- Fixed `hf mf wrbl` and `hf mfp wrbl` the ACL RO checks on 16-block sectors correct  (@team-orangeBlue)
- Changed `hf mfp wrbl` command to check for Sector Trailer errors that could potentially lock sectors out (@team-orangeBlue)
- Changed `hf felica info` and `hf felica rqsyscode` system code name annotation (@kormax)
- Added `lf relay` command where it relays between two pm3 devices over internet. Thanks to Moerno for the code! (@iceman1001)
- Changed `hf mf acl` command to have more recognized generic configurations (@team-orangeBlue)
- Added `hf mfp acl` command (@team-orangeBlue)
- Modified `data_hex_crc.lua` script, it now takes a `-s` parameter and the output is now in alternative row colors. (@iceman1001)
- Added `hf mfdes brutedamslot` command (@kormax)
- Added `hf mfdes getdelegateappinfo` command (@kormax)
- Added `hf secc info` command to retrieve the Card Recognition Template(@antiklesys)
- Added `hf mfdes createdelegateapp` command (@kormax)
- Optimized `hf iclass legbrute` throughput: replaced recursive `suc()`/`output()` cipher functions with iterative loops, added 256-entry LUT for the `select()` function eliminating redundant bit arithmetic and halving key lookups per state step, switched successor state to in-place pointer update removing per-call struct copies, added `doMAC_brute()` with byte-wise LSB-first processing and direct output bit packing eliminating all bitstream struct overhead and output reversal calls per key candidate, and replaced per-iteration 64-bit modulo progress check with a countdown counter (@antiklesys)
- Improved `hf iclass legbrute` fixed multithreaded key-range partitioning so threads cover non-overlapping slices of the 40-bit keyspace, added ETA display, keyboard abort with resume hint, `_Atomic` correctness for shared state, `pthread_create` error handling, and thread count capped at available CPUs (@antiklesys)
- Changed `hf secc sim` it nows supports wildcard to payloads (@antiklesys)
- Added `hf secc` to build a base for simulating basic function of iclass SE config cards (@antiklesys)
- Changed `hf iclass view` , improved SIO parsing for based on @Iceman1001's "Dismantling the SEOS Protocol" talk (@antiklesys)
- Changed `hf iclass tagsim` added live fc/cn update to refreshing the csn with each update (@antiklesys)
- Added `--live` option to `hf iclass lookup` command to perform a live recovery of the reader's key by simulating a tag and running the lookup command against both standard and elite dictionaries (@antiklesys)
- Added `hf iclass tagsim` command to quickly simulate an iclass card based on facility code and card number(@antiklesys)
- Added `-f` parameter to `hf iclass sam` command to use the sam to parse a card dump (@antiklesys)
- Fixed WTX response behavior in `ExchangeRaw14A()` (@team-orangeBlue)
- Added `-t` / `--timeout` option for `hf 15 sim` (@recursivenomad)
- Added `--aid` parameter to `hf seos` commands (@kormax)
- Added `hf iclass blacktears` command to perform an automated tearoff of block 1 to set non-secure page mode(@antiklesys)
- Changed wiegand encoding to use shared helpers and have unified parameters (--raw, --bin, --new, --wiegand, etc.) (@cindersocket)
- Added `hf gst read` command (@kormax)
- Added `hf gst info` command (@kormax)
- Added `hf 14b tearoff` - interactive ST25TB/SRx monotonic counter tear-off attack (@xNovyz)
- Fixed missing `WDT_HIT()` in `Get14443bAnswerFromTag()` DMA polling loop causing hardware watchdog reboot on SSC clock stall (@xNovyz)
- Added `hf vas info` command (@kormax)
- Changed `wiegand encode` / `wiegand decode` - added support for encoding and decoding the new 96-bit ASN.1 encoded format, `--bin` encoding, verbose PACS encoding output, and explicit rejection of raw/binary decodes above 96 bits (@cindersocket)
- Added Mifare Classic support to `hf gallagher` command (@pingu2211)
- Added `hf felica discnodes` command (@kormax)
- Added `hf mfp dump` command (@apply-science)
- Added `hf felica seacinfo` command (@kormax)
- Added `hf mfdes bruteisofid` and `hf mfdes selectisofid` commands (@kormax)
- Added DESFire AID values related to LEAF (@kormax)
- Added `dict`, `ascii`, `mad` presets for `hf mfdes bruteaid` (@kormax)
- Added tag loss detection & recovery into `hf mfdes bruteaid` (@kormax)
- Added hardening for all host binaries. Exact level of hardening depends on the OS (@doegox)
- Added `hf aliro read` command (@kormax)
- Added `hf aliro info` command (@kormax)
- Added Qt6 support for the client (@doegox)

## [Permafrost.4.21128][2026-02-25]
- Added standalone mode `HF_DOEGOX_AUTH0`: UL-C / UL-AES unlocker (@doegox)
- Fixed `lf t55xx wakeup` options parsing (@suut)
- Added `--` arg separator to client to pass following args to scripts (@doegox)
- Added `hf mfu cchk/aeschk` and options to `hf mf cauth/aesauth` (@doegox)
- Added support to inject card auth replies in `hf mfu sim` (@doegox)
- Added ntag22x_suncmac_recovery and mfulaes_mask_recovery scripts (@doegox)
- Added mfulc_counterfeit_recovery script and mfulc_des_brute tool (@np0 & @doegox)
- Fixed ISO14443-4 type B NDEF workflow + parsing (@team-orangeBlue)
- Added Snapmaker U1 filament spool KDF in `hf mf keygen` (@Foxushka)
- Replaced `hf mf bambukeys` with `hf mf keygen` with multiple KDFs support (@Foxushka)
- Fixed `hf seos adf/pacs` handling of cards with different diversifier lengths and different ADF OIDs (@nvx)
- Added `data qrcode` - to generate QR codes from inside the pm3 client (@iceman1001)
- Fixed unicode on mingw/proxspace (@nvx)
- Fixed `hf felica raw` - wrong length calculationes. Thanks @dxl for the solutions! (@iceman1001)
- Added basic QR code generation support. Thanks @mistial-dev for the idea! (@iceman1001)
- Added identification of NDEF/Open print tag record (@iceman1001)
- Added support for Bruce dump files [.rfid] (@iceman1001)
- Added script `read_t-union.py` (@klks)
- Added better ECP configuration aliases (@kormax)
- Added `hf 14b config` (@kormax);
- Added polling loop annotation support to `hf 14b config` (@kormax);
- Updated id48lib (adds unit tests, better build management) (@henrygab)
- Added two trace files for Ultralight AES (@iceman1001)
- Added support for Ultralight AES secure messaing in `hf 14a raw` (@iceman1001)
- Added support for Ultralight AES secure messaging in `hf mfu info/rdbl/wrbl/dump/ndefread/wipe/setkey` (@iceman1001)
- Added support for Ultralight AES auth in `hf mfu info/rdbl/wrbl/dump/ndefread/wipe/setkey` (@doegox)
- Added Ultralight AES basic DataProtKey emulation support (@doegox)
- Changed `mem info` and how the signature handling is done (@iceman1001)
- Added `client/resources/pm3_generic_private_key.pem` in order to self-sign a modded device (@iceman1001)
- Fixed `hf mfdes value --op clear` commands for clearing more than 0x80000000 values and getfilesettings mac mode (@merlokk)
- Added ATR fingerprinting to `hf 14a/14b info` (@doegox)
- Added `Verkada 40-bit` format (@aaronmaxlevy)
- Added `hf seos write` command (@aaronjamt)
- Added `hf seos sim` command (@aaronjamt)
- Fixed `hf mf staticnested` faild to find a KeyB (@xianglin1998)
- Fixed errors(jansson) for MSYS2+UCRT compiler (@xianglin1998)
- Added Kaba evolo AIDs (@xtruan)

## [Phrack.4.20728][2025-09-11]
- Added `unofficial desfire bible` document (@mistial-dev)
- Fixed desfire value file operations (@mistial-dev)
- Added script `pm3_online_tests.sh` (@mistial-dev)
- Changed `lf t55xx restore` - now skips writing block0 if its all zeros (@iceman1001)
- Added `HID Simplex Grinnell 36-bit` - Improved Simplex decoder (@datafx, @henrygab)
- Changed `lf search` - also test for chipset even if there was just signal noice (@iceman1001)
- Added detection of PCF 7961 , thanks progman (@iceman1001)
- Changed `reveng -g` - now correctly take 1024 hexstring as input (@iceman1001)
- Added `--override` parameter to NDEF read for overriding MAD CRC check (@iceman1001)
- Added `hf saflok` commands (@stiebeljoshua)
- Added `ntag_clean.lua` script for easier NTAG memory wipe (@trigat)
- Changed from Bigbuf malloc to Bigbuf calloc calls on device side (@iceman1001)
- Added `lf t55xx view` - now viewing of T55XX dump files is possible (@iceman1001)
- Fixed `lf indala cone` - now writing the right bits when using `--fc` and `--cn`
- Changed readline hack logic for async dbg msg to be ready for readline 8.3 (@doegox)
- Changed documentation to avoid conflicts with ModemManager on Linux, is recommended to masking the service (@grugnoymeme)
- Changed `data crypto` - now also handles AES-256 (@iceman1001)
- Changed `hf mfdes info` - add recognition of Swissbit iShield Key Mifare (@ah01)
- Changed `hf mf info` - add detection for unknown backdoor keys and for some backdoor variants (@doegox)
- Changed `mqtt` commnands - now honors preference settings (@iceman1001)
- Changed `prefs` - now handles MQTT settings too (@iceman1001)
- Fixed `mqtt` segfault and gdb warning under windows (proper thread stopping and socket handling). (@virtyvoid)
- Added `mqtt` - the pm3 client can now send and receive MQTT messages or json files. (@iceman1001)
- Changed `hf iclass wrbl` - replay behavior to use privilege escalation if the macs field is not passed empty(@antiklesys)
- Changed `hf iclass restore` - it now supports privilege escalation to restore card content using replay (@antiklesys)
- Fixed `hf 15 dump` - now reads sysinfo response correct (@iceman1001)
- Changed `make clean` - it now removes all __pycache__ folders (@iceman1001)
- Fixed `hf 15 readmulti` - fix block calculations (@iceman1001)
- Changed `mem load` - now handles UL-C and UL-AES dictionary files (@iceman1001)
- Changed `hf mfu sim` - now support UL-C simulation (@iceman1001)
- Added `!` - run system commands from inside the client. Potentially dangerous if running client as SUDO, SU, ROOT (@iceman1001)
- Implemented `hf felica scsvcode` - now dumps all service and area codes. (@zinongli)
- Added `hf felica liteauth` - now support FeliCa Lite-S authentication(@q0jt)
- Added `he felica dump` - partial support for dumping all blocks from unauth readable services (@zinongli)
- Changed `hf 14b calypso` - now don't break the file id loop when one file can't be selected or read. Add new file ids to iterate through (@zinongli)
- Fixed `script run hf_mfu_amiibo_restore.lua` - broken reference to a dependency. (@CorySolovewicz)
- Changed `/client/update_amiibo_tools_lua.py` - updated the output to make things more clear. (@CorySolovewicz)
- Changed `/client/lualibs/amiibo_tools.lua` - updated to the most recent data. (@CorySolovewicz)
- Changed `/client/pyscripts/mf_backdoor_dump.py` - fixed output checks to handle whitespace variations. fixed output of card dump. (@robo-w)


## [Daddy Iceman.4.20469][2025-06-16]
- Fixed edge case in fm11rf08s key recovery tools (@doegox)
- Removed `--par` from `lf em 4x70` commands.
- Changed `hf 14a info` - refactored code to be able to detect card technology across the client easier (@iceman1001)
- Changed `hf mf info` - now informs better if a different card technology is detected (@iceman1001)
- Changed `hf mf autopwn` - now exits if desfire is detected and limit attacks if mifare plus is detected (@iceman1001)
- Changed `hf mfp chk` - improved key handling and output (@iceman1001)
- Fix `hf mf dump` - added a check for keyfile to contain enough keys for card (@iceman1001)
- Fix `hf mf eview` - now viewing 2k, 4k cards doesn't get wrong background color (@iceman1001)
- Changed `hf mf info` - skip checking if it detects a MIFARE Ultralight family card (@iceman1001)
- Changed `hf mf rdsc` - it now addeds the used key to the output in the sector trailer (@iceman1001)
- Added the `PM3ULTIMATE` platform in the build / docs. *untested* (@iceman1001)
- Added fpga compilation for PM3ULTIMATE device (@n-hutton)
- Updated the ATR list (@iceman1001)
- Fixed fpga binary images to use fixed seed 2 (@n-hutton)
- Added `hf iclass sam --info` - option that returns sam specific details (@antiklesys)
- Changed `hf iclass sim -t 7` - implemented simulation that glitches key block responses (@antiklesys)
- Changed `hf iclass sim -t 6` - implemented simulation that glitches sio block (@antiklesys)
- Changed `hf iclass legbrute` - implemented multithreading support (@antiklesys)
- Changed `hf iclass legrec` - added a --sl option for further speed increase by tweaking the communication delays (@antiklesys)
- Changed `hf iclass legrec` - added a --fast option for further speed increase and automated AA2 block selection (@antiklesys)
- Changed `hf iclass legrec` - additional code optimizations gaining a ~147% speed increase (@antiklesys)
- Changed `hf iclass tear` - readability improvements for erase phase (@antiklesys)
- Changed `hf iclass legrec` - code optimizations gaining a ~8% speed increase (@antiklesys)
- Modified `hf iclass tear` - now has a device side implementation also. (@antiklesys) (@iceman1001)
- Changed `hf iclass info` - now uses CSN values based checks (@antiklesys)
- Changed `hf iclass dump` - now uses default AA1 key when called without a key or key index (@iceman1001)
- Renamed `hf iclass trbl` to `hf iclass tear` (@iceman1001)
- Changed `hw tearoff` - the device side message is now debug log controlled (@iceman1001)
- Changed `pm3.sh` - Serial ports enumeration on Proxspace3.xx / MINGW environments,  now using powershell.exe since wmic is deprecated (@iceman1001)
- Fixed `hf iclass trbl` - to correctly use the credit key when passed and show partial tearoff results (@antiklesys)
- Fixed `hf iclass legbrute` was not correctly parsing the index value
- Fixed `hf mf ekeyprn` - failed to download emulator memory due to wrong size calculation (@iceman1001)
- Fixed `hf mf fchk --mem` to actually use flash dict (@doegox)
- Fixed `make install` on OSX thanks DaveItsLong (@doegox)
- Added new standalone mode `HF_ST25_TEAROFF` to store/restore ST25TB tags with tearoff for counters (@seclabz)
- Added `hf_mfu_ultra.lua` script enables restoring dump to ULTRA/UL-5 tags and clearing previously written ULTRA tags (@mak-42)
- Fixed `hf mfu sim` to make persistent the counter increases in the emulator memory (@sup3rgiu)
- Fixed `hf mf mad` to correctly display MAD version 2 card publisher sector (@BIOS9)
- Fixed `lf hitag dump` and related commands stability when tag is configured in public mode/TTF mode (@rfidgeek1337)

## [Blue Ice.4.20142][2025-03-25]
- Added `des_talk.py` script for easier MIFARE DESFire handling (@trigat)
- Fixed `hf 14b info` - wrong endianess when looking for lock bits etc (@gentilkiwi)
- Changed `hf mf autopwn` - tries to detect static encrypted nonces and also user cancel during chk keys (@iceman1001)
- Changed `hf mf autopwn` - added option to use SPI flash dictionary (@jmichelp)
- Changed `trace list -t seos` - now annotate ISO7816 (@iceman1001)
- Updated aid and mad json files (@iceman1001)
- Changed `hf 14a apdu` - now can be interrupted and dynamically adds time (@iceman1001)
- Changed `trace list -t` - shortend the hitag types (@iceman1001)
- Added Be-Tech identification (@iceman1001)
- Added `lf em 410x clone --htu` clone EM410x ID to Hitag µ/8265 (@douniwan5788)
- Added `lf hitag htu` support for Hitag µ/8265 (@douniwan5788)
- Added `hf mfu aesauth` based on existing UL AES support (@doegox)
- Changed `hf mfu sim` deny OTP changes with all zeros (@iceman1001)
- Added missing file in CMakeLists.txt (@iceman1001)
- Changed `lf em 4x70` internals on ARM side; Enabling improved debugging and reliability (@henrygab)
- Improved `pcf7931` generic readability of the code. Unified datatypes and added documentation/explainations (@tinooo)
- Improved `lf pcf7931` read code - fixed some checks for more stability (@tinooo)
- Changed `trace list -t seos` - improved annotation (@iceman1001)
- Added `make commands` to regenerate commands documentation files and autocompletion data independently of `make style` (@doegox)
- Added texecom identification,  thanks @en4rab ! (@iceman1001)
- Added `hf 15 slixprotectpage` command
- Fixed `hf mf gload` - missing parameter (@iceman1001)
- Changed `hf mf gload` - now handles 1k ev1 sized dumps (@iceman1001)
- Changed wiegand format unpack functions to clear struct later (@iceman1001)
- Changed `wiegand decode` - now accepts new padding format (@iceman1001)
- Changed `mem spiffs tree` - ID is now shown in decimal (@iceman1001)
- Added sample wiegand format 56bit (@iceman1001)
- Changed Wiegand formats to include number of bits (@iceman1001)
- Fixed compilation warning in hitagS (@iceman1001)
- Added new wiegand format H800002 (@jmichelp)
- Changed `Makefile.platform.sample` file - now have clear instructions for generating images for other proxmark3 hardware (@iceman1001)
- Changed `doc/magic_cards_notes.md` - now contains documentation for iKey LLC's MF4 tag (@team-orangeBlue)
- Changed `hf mf cload` - now accepts MFC Ev1 sized dumps (@iceman1001)
- Changed `hf mfu info` - now properly identify ULEv1 AES 50pF (@iceman1001)
- Changed `hf mf info` - now differentiates between full USCUID and cut down ZUID chips (@nvx)
- Changed `lf hitag chk` - added key counter, client side abort and minor delay (@iceman1001)
- Added `hf seos sam` - Added support for HID SAM SEOS communications (@jkramarz)
- Changed the extended area accessible by spiffs into last page of FLASH (@piotrva)
- Changed flash-stored key dictionaries (Mifare, iClass, T55XX) and T55XX configurations to SPIFFS files (@piotrva)
- Changed `lf em 410x sim` to use default gap value of 0 and extended help (@piotrva)
- Changed `hf 14a info` - now identifies MIAFRE Duox (@iceman1001)
- Added `hf iclass trbl` to perform tear-off attacks on iClass (@antiklesys)
- Added support for connection to host device in all Docker envs (@doegox)
- Changed `hf 15 info` to show all type matches and check ST25TVxC signature (@doegox)
- Added initial support for ST25TN and its signature verification (@doegox)
- Changed originality checks handling to refactor code and pk data (@doegox)
- Changed `uniq.yaml` workflow to be case-insensitive (@iceman1001)
- Fixed `mem load --mfc` not erasing all SPI flash blocks after extending to 4095 keys (@piotrva)
- Changed extended area for Mifare keys in SPI flash to hold 4095 keys (@piotrva)
- Fixed DESFire D40 secure channel crypto (@nvx)
- Fixed `hf mfp info` fix signature check on 4b UID cards (@doegox)
- Changed `hf_mf_ultimatecard` - it now automatically set maximum read/write block when using predefined types (@piotrva)
- Changed SPI flash detection to calculate the size instead of table lookup (@ANTodorov)
- Changed `spi_flash_decode.py` script with more ICs (@ANTodorov)
- Fixed `hf/lf tune` segfault when called from script (@doegox)
- Changed `hf_mf_ultimatecard` - added option to set and get maximum read/write block number (@piotrva)
- Added JEDEC information for SPI flash W25Q64JV (@ANTodorov)
- Changed `hf iclass configcard` - added special iclass legacy config cards (@antiklesys)
- Changed `hf iclass legrec` - added simulation function (@antiklesys)
- Added keys from Momentum firmware projects. (@onovy)
- Added Dutch Statistics Agency default key (@eagle00789)
- Fixed Wiegand decode with hex input dropping the first bit (@emilyastranova)
- Changed `hf mf autopwn` - now allows for custom suffix (@zxkmm)

## [Orca.4.19552][2024-11-22]
- Fixed `hf_legic.lua` - removed bit32 commands from the script (@diorch1968)
- Fixed `mem spiffs tree` - now show correct symlink name (@ANTodorov)
- Fixed `mem spiffs wipe` - reported file/link names is now correct  (@ANTodorov)
- Updated atrs list (@iceman1001)
- Added support for a new KDF (@iceman1001)
- Added Inner range aid and mad entries (@iceman1001)
- Changed `mem spiffs` - Use all available space in SPI flash (@ANTodorov)
- Fixed `hf mf sim` - wrong size check in MifareSim (@iceman1001)
- Fixed `hf mf sim` not to respond to authentication attempts for sectors out of bound for selected Mifare type (@piotrva)
- Added option to build against non-default python3 with CMake as well (@doegox)
- Added option to build against non-default python3 with Makefile (@ANTodorov)
- Changed `hf 14a info` `hf mf info` - now detects FM1216-137 CPU cards (@iceman1001)
- Changed `hf iclass configcard` - expanding the list of available options and functionalities (@antiklesys)
- Fixed `intertic.py` - missing comma in array (@iceman1001)
- Changed `hf iclass legrec` - improved algorithm leveraging reduced entropy from hash0 constraints (@antiklesys)
- Fixed `hf iclass configcard` when generating elite or keyroll elite configcards for Rev.C legacy readers (@antiklesys)
- Changed `hf mf c*` - now accepts a --gdm flag to write using uscuid/gdm 20/23 alt magic wakeup (@nvx)
- Changed `pm3_console()` - Python/Lua/C: replace `passthru` by `capture` and `quiet` (@doegox)
- Fixed `hf iclass list` - annotation crc handled better (@iceman1001)
- Fixed `hf_mf_uscuid_prog.lua` - bad divisions and code style fixes (@iceman1001)
- Changed `hf iclass info` - now checks for cards silicon version (@antiklesys)
- Changed `hf iclass legrec` - updated script implementation to ensure functionality (@antiklesys)
- Added recovered iclass custom key to dictionary (@antiklesys)
- Added support for all Hitag S response protocol mode (@douniwan5788)
- Fixed `hf_young` - flags declaration was missing a semicolon (@jakkpotts)
- Changed `hf mf sim` - add option to allow key b to be used even if readable (@doegox)
- Changed `data num` - outputed binary strings are now properly zero padded (@iceman1001)
- Changed `hf iclass info` - now tries default keys and decode if legacy (@iceman1001)
- Changed `hf iclass chk` - now loads dictionary file by default (@iceman1001)
- Added Makefile variable `DONT_BUILD_NATIVE` in mfd_aes_brute Makefile to easify downstream package (@Cryolitia)
- Auto detect whether compile option `march=native` is supported for mfd_aes_brute Makefile
- Changed `hf mf sim` - support data-first and nested reader attacks (@doegox)
- Fixed `lf search` and `lf em 4x50 rdbl -b <blk>` does not coredump reading EM4450 tag (@ANTodorov)
- Fixed flashing - client doesnt fail every other flash attempt (@iceman1001)
- Changed `pref show` - add option to dump as JSON (@doegox)
- Changed `mf_backdoor_dump.py`- use faster ecfill/eview (@doegox)
- Changed `hf mf ecfill` - wait for execution and return status (@doegox)
- Changed `hf 14a reader` - added option to wait for a card (@doegox)
- Changed `hf mf ecfill` - added support for quick dump via backdoor auth (@doegox)
- Fixed `hf mf restore` - really skip strict ACLs unless --force (@doegox)
- Added `hf 14b setuid` - set uid on magic 14b tag (@iceman1001)
- Changed `hf 14b info` - now detect Tiananxin (@iceman1001)
- Fixed `lf em 410x brute` - better filehandling and memory handling (@iceman1001)
- Changed split PacketResponseNG status into status and reason (@douniwan5788)
- Added `spi_flash_decode.py` - helper script to decode JEDEC data (@ANTodorov)
- Changed `hw status` - now show SPI flash JEDEC Manufacturer ID and Device ID in output (@ANTodorov)
- Changed `hf iclass configcards` to support generating config cards using a different key than the default k0 as the card's key (@antiklesys)
- Added maur keys (@iceman1001)
- Fixed `hf mfu pwdgen` for the 7 byte UID (@ANTodorov)
- Added `hf iclass unhash` command to reverse an iclass diversified key to hash0 pre-images (@antiklesys)
- Changed `hf 14a raw` - now supports crypto  (@doegox)
- Changed `hw version` command to print LUA and Python versions (@jmichelp)
- Updated LUA to v5.4.7 which adds utf-8 support (@jmichelp)
- Moved `lf hitag sim --hts` -> `lf hitag hts sim` (@douniwan5788)
- Removed `lf hitag read/write --hts` (@douniwan5788)
- Changed `lf search` - it now tries to read and decode paxton id (@iceman1001)
- Changed `lf search` - to identify hitag2/s/82xx in chipset detection to preserve their EM4100 or other outputs (@iceman1001)
- Added `lf hitag hts reader` - to act as a HitagS / 82xx reader (@iceman1001)
- Changed `lf hitag hts write` -> `lf hitag hts wdbl` to fit rest of client command names (@iceman1001)
- Changed `lf hitag hts read` -> `lf hitag hts rdbl` to fit rest of client command names (@iceman1001)
- Changed `hf mf info` - Better handling when printing ATS (@iceman1001)
- Changed to also try the MFC_B key when extracting memory (@iceman1001)
- Fixed `make -j check`  Thanks @elboulangero  (@iceman1001)
- Added support for 8268/8310 (@douniwan5788)
- Changed scripting string params to accept 1024 chars, Thanks @evildaemond! (@iceman1001)
- Added detection for FM11NT021 (@iceman1001)
- Added detection of a magic NTAG 215 (@iceman1001)
- Fixed hardnested on AVX512F #2410 (@xianglin1998)
- Added `hf 14a aidsim` - simulates a PICC and allows you to respond to specific AIDs and getData responses (@evildaemond)
- Fixed arguments for `SimulateIso14443aTag` and `SimulateIso14443aInit` in `hf_young.c`, `hf_aveful.c`, `hf_msdsal.c`, `hf_cardhopper.c`, `hf_reblay.c`, `hf_tcprst.c` and `hf_craftbyte.c` (@archi)
- Added `mf_backdoor_dump.py` script that dumps FM11RF08S and similar (Mifare Classic 1k) tag data that can be directly read by known backdoor keys. (@Aptimex)
- Added keys for Metro Q transit cards in Huston, TX. (@Anarchothulhu)
- Added keys from MifareClassicTool and Flipper projects. (@onovy)

## [Backdoor.4.18994][2024-09-10]
- Changed flashing messages to be less scary (@iceman1001)
- Fixed docker containers and their documentation (@doegox)
- Fixed `hf ict` - buffer overflow (@doegox)
- Fixed `hf felica raw -s` - dont check crc for select tag response,  thanks @RebornedBrian! (@iceman1001)
- Added a multi-threaded of ht2crack2search (@iceman1001)
- Fixed ISO14443a bounds-checking because @doegex found cards not following ISO14443a when fuzzed (@iceman1001)
- Added `mfkey32nested`: recovering partial nested authentication with known nT (@doegox)
- Added support for dumping FM11RF08S data at once (@doegox)
- Added support for collecting all FM11RF08S nT/{nT}/par_err at once (@doegox)
- Fixed `hf mfu wrbl` - compatibility write only writes 4 bytes. Now handled correctly (@iceman1001)
- Changed `hf mfu info` - better magic tag detection (@iceman1001)
- Added ELECTRA pattern decoding in `lf search` (@CiRIP)
- Firmware size optimization, skipping unused FPGA bitstreams (@douniwan5788)
- Added pretty Hitag S config parsing (@CiRIP)
- Moved Hitag S operations into separate submenu: `lf hitag s` (@CiRIP)
- Added MFC keys for Sofia public transport cards (@user890104)
- Added `lf em 410x clone --hs` clone EM410x ID to Hitag S/8211 (@douniwan5788)
- Fixed Hitag S read/write in plain mode (@douniwan5788)
- Fixed fm11rf08s script for non-4B UID (FM11RF08S-7B) (@Foxushka)
- Fixed missing require of ansicolors in `lf_hid_bulkclone_v2.lua` script (@whiteneon)
- Added `lf_t55xx_reset.lua` - a script to aid in quickly resetting t55xx chips (@whiteneon)
- Added more fingerprinting in `hf mf info` (@doegox)
- Added --issue and (--emu)lator support to `hf iclass encode` command (@micsen)
- Added custom CTF Wiegand format from Defcon32 with comments (@micsen)
- Added native output grabbing for Python and Lua: less hacky than `output_grabber.py`, should work on ProxSpace as well (@doegox)
- Changed `hf mf chk/fchk`: added option `--no-default` to skip loading the usual ~61 hardcoded keys (@doegox)
- Fixed `hf mf wipe` to detect properly write errors (@doegox)
- Fixed `hf mf fchk` which was leaving the RF field on when interrupted by keyboard (@doegox)
- Changed file saving: do not prepend with default path if given path is an absolute path (@doegox)
- Added few tools to crack static encrypted nonce tags such as FM11RF08S (@doegox)
- Added `hf mf isen` for analyzing some static encrypted nonce tags (@doegox)
- Changed `hf mf rdbl/wrbl/rdsc/nested` to support extended authentication commands (@doegox)
- Changed `hf mf fchk` to allow cracking a single key and show progress info (@doegox)
- Changed `trace list -t mf` to add support for extended authentication commands (@doegox)
- Fixed `output_grabber.py` to allow several calls in a script, make it robust to emoji and make it available to scripts (@doegox)
- Changed `validate_prng_nonce` for a much faster version (@doegox)
- Changed standalone mode HF_MATTYRUN - support more card sizes, user dictionaries, improved emulation (@michaelroland)
- Changed `hf iclass dump --ns` - now supports the nosave flag (@iceman1001)
- Fixed write check in hitag2crack2 buildtables (@mwalker33)
- Fixed breaking of client when trying to load a non-supported .picopass file (@iceman100)  Thanks to Jump for suggested fixes!
- Changed `mf_nonce_brute` tool to handle the odd case of multiple key candidates (@iceman1001)
- Fixed a bad memory erase (@iceman1001)
- Fixed BT serial comms (@iceman1001)
- Changed `intertic.py` - updated and code clean up (@gentilkiwi)
- Added `pm3_tears_for_fears.py` - a ISO14443b tear off script by Pierre Granier
- Added new t55xx password (002BCFCF) sniffed from cheap cloner (@davidbeauchamp)
- Fixed 'hf 14b sim' - now works (@michi-jung)
- Added VB6 Rng for iclass elite keys `hf iclass lookup` and `hf iclass chk` functions by porting @bettse work in the Flipper Zero Picopass App (@antiklesys)
- Added MFC Key for swimming pool cards in Reykjavík Iceland (@dandri)
- Added key for Orkan keyfobs (@dandri)
- Added key for Atlantsolía keyfobs (@dandri)
- Added `hf iclass legbrute` this function allows to bruteforce 40/64 k1 bits of an iclass card to recover the raw key used(@antiklesys).
- Added `hf iclass legrec` this function allows to recover 24/64 k1 bits of an iclass card (@antiklesys).
- Added script for quick WSL-2 environment initialization every reboot (@dhuuthang).
## [Aurora.4.18589][2024-05-28]
- Fixed the pm3 regressiontests for Hitag2Crack (@iceman1001)
- Changed `mem spiffs tree` - adapted to bigbuff and show if empty (@iceman1001)
- Changed `lf hitag info` - now tries to identify different key fob emulators (@iceman1001)
- Added `lf hitag reader` - act as a Hitag2 reader (@iceman1001)
- Fixed `lf hitag crack2` - now works. (@iceman1001)
- Fixed wrong use of free() in desfire crypto on arm src,  thanks @jlitewski! (@iceman1001)
- Added `lf em 4x70 calc` - calculate `frn`/`grn` for a given `key` + `rnd`
- Fixed `hf 15 dump` memory leaks (@jlitewski)
- Changed `hf search` - topaz is detect before ISO14443a and commented out WIP ICT code path (@iceman1001)
- Fixed `hf search` - where felica reader now doesnt timeout and give wrong response (@iceman1001)
- Fixed overflow in deps/jansson library (@iceman1001)
- Added `lf hitag crack2` -  WIP.  Trying to add the second attack vector against Hitag2 (@iceman1001)
- Changed `hf 14b reader --plot` - made the anticollision signal trace download optional (@iceman1001)
- Added `lf_hitag_crypto.trace` - trace file of a complete read out of a Hitag2 in crypto mode (@iceman1001)
- Fix `lf cmdread` - uninitialised memory usage (@iceman1001)
- Changed `hf st info` - now tries to check signature if available (@iceman1001)
- Added `hf 14b mobib` - try to read out all data from a MOBIB card (@iceman1001)
- Added `hf 14b calyso` - try to read out all data from a Calypso card (@iceman1001)
- Changed the graphbuffer handling implementation (@HACKhalo2)
- Changed `lf hitag dump --nrar` - now supports attack 1 from "gone in 360 seconds" paper. Thanks @kevsecurity! (@iceman1001)
- Added `lf hitag selftest` - converted from RFIDLers selftest (@iceman1001)
- Added `lf hitag chk` - dictionary attack against card (@iceman1001)
- Added `lf hitag lookup` - verify collected challenges aginst dictionary (@iceman1001)
- Updated windows workflow to use latest setup-wsl script (@iceman1001)
- Added a micro second clock in the client (@iceman1001)
- Fix `hf mfdes read` - buffer overflow when reading large files (@iceman1001)
- Changed `hf 15 csetuid` - now supports gen2 command. Thanks @whywilson! (@iceman1001)
- Changed `hf mfp info` - to identify Ev2 (@iceman1001)
- Updated Graph Markers implementation to include temporary markers and marker labels (@HACKhalo2)
- Updated to SWIG 4.2.1 (@iceman1001)
- Removed `data bin2hex` - replaced by `data num`  (@iceman1001)
- Removed `data hex2bin` - replaced by `data num`  (@iceman1001)
- Fixed `lf em 410x clone --electra` - now writes properly (@iceman1001)
- Changed waiting for device loops. More stable experience (@iceman1001)
- Changed client now handles CTRL-C and saves history if using READLINE (@iceman1001)
- Changed `hf mf *` - printing of keys if MFC EV1 added extra explaination (@iceman1001)
- Changed `hf mf ginfo` - now supports decoding of user supplied configuration block (@iceman1001)
- Changed `data load` - now shows loaded number as comma printed. (@iceman1001)
- Updated `/tools/hitag2crack/common/OpenCL-Headers/CL` with latest from KhronosGroup github page (@iceman1001)
- Fixed `lf hitag list` - improved HITAG2 protocol annotation (@iceman1001)
- Added AIDs `002000` and `FF30FF` from Metrolinx Presto Card (@RunTheBot)

## [Zenith.4.18340][2024-03-20]
 - Changed `hf mf info` - some detections (@iceman1001)
 - Changed `hf mf autopwn` - minor fix (@iceman1001)
 - Fixed `lf em 4x70 recover` - no more compiler warning (@iceman1001)
 - Added `hf mfu wipe` - filles card memory with zeros (@iceman1001)
 - Added `lf em 4x70 autorecover` - writable ID48 tags now have automatic key recovery (@henrygab)
 - Set max write blocks to 251 in hf_mf_ultimatecard before writing version/signature in NTAG 21X (@antgamdia)
 - Changed HF_UNISNIFF standalone mode, output and also sniffs iclass too (@iceman1001)
 - Changed `hw standalone` - added `-b` param to control HF_UNISNIFF from client (@iceman1001)
 - Changed `mem spiffs dump` - now if `-t` and `-d` it will also save to file (@iceman1001)
 - Fixed `hf mf sim` - key type identification (@iceman1001)
 - Added `lf em 4x70 recover` - recovery the second half of an id48 key (@henrygab)
 - Changed `hf emrtd info` - Added EG_DG7 viewing (@iceman1001)
 - Changed `hf mf dump` - it now also prints the dumped memory (@franscesco-scar)
 - Changed NDEF parsing to show mime images (@iceman1001)
 - Fixed `hf mf ndefformat` - now correctly handles MADv2 when formatting (@iceman1001)
 - Fixed `hf 14a info` - magic detection works again and better hint detection (@iceman1001)
 - Changed `hf 14b wrbl` - now uses SRIX 4k as default tag (@Sonic803)
 - Changed `hf 14b reader` - now shows data plot (@Sonic803)
 - Added `hf 14b restore` - new command to restore dump files to a SR512/4k card (@Sonic803)
 - Changed led show / leds detection for PM3 Easy devices (@francesco-scar)

## [DEFCON is Cancelled.4.18218][2024-02-18]
 - Fixed `hf mf autopwn` - now correctly attempts static nested attack (@Sonic803 and @francesco-scar)
 - Changed `hf fudan dump --ns` - now supports nosave flag (@iceman1001)
 - Added `lf em 410x --electra` - adds two extra blocks. Thanks jareckib (@iceman1001)
 - Changed `hf mfu info` - now identifies UL-AES correct (@iceman1001)
 - Changed `hw detectreader` - Detect both LF and HF at the same time (@wh201906)
 - Changed `hf mfu info` - should not try pwd against a UL-AES (@iceman1001)
 - Fixed `hf mfu info` - tag type identification now properly handles 64bits (@iceman1001)
 - Changed `hf st info` - reworked the output (@iceman1001)
 - Rename `smart relay` to `smart pcsc` and add support for contact interface (@gm3197)
 - Updated ATR list (@iceman1001)
 - Changed `hf mfu keygen -b` - now takes a block number (@iceman1001)
 - Fixed `hf mfu info` - now tries pwdgens better (@iceman1001)
 - Changed `hf mf info` - now also checks sector 1 (@iceman1001)
 - Changed `lf em 410x` - now allows for card number zero.  thanks @dangerousthings !  (@iceman1001)
 - Changed `liblua` - now bugfixes applied and current version is v.5.2.4 (@iceman1001)
 - Changed `lf em 4x50 dump --ns` - now supports the nosave flag (@iceman1001)
 - Added `lf em 4x50 view` - view on Em4x50 dump files (@iceman1001)
 - Changed the magic card detection. It now acts as flags (@iceman1001)
 - Added `hf ict` - skeleton commands in preperation (@iceman1001)
 - Added `lf hitag eview` - now supports viewing of emulator memory (@iceman1001)
 - Added `lf hitag view` - now supports viewing of dump files (@iceman1001)
 - Changed `lf hitag dump --ns` - now support nosave flag (@iceman1001)
 - Added `hf xerox rdbl` - read block of a Fuji/Xerox tag (@iceman1001)
 - Added a xerox trace file,  thanks @jeroenSteen,  (@iceman1001)
 - Fixed `lf em 4x05 view` - now loads JSON files properl (@iceman1001)
 - Added `hf mf gdmparsecfg` command to decode the GDM/USCUID config block (@nvx)
 - Changed `hf mf gdmcfg` show the parsed config on read (@nvx)
 - Changed `hf mf gdmcfg/gdmsetcfg` commands to support Gen1a and GDM Alt magic wakeups (@nvx)
 - Fixed `hf 15 writedsfid` - long wait after write and dont force -o flag (@iceman1001)
 - Changed `hf 14a sniff -i` - now supports the interactive flag (@iceman1001)
 - Added `HF_15SIM` standalone mode that dump then simulate iso15 tags (@lnv42)
 - Changed `iso15 simulation`, reworked, added support for lot of features (@lnv42)
 - Changed `hf 15 sniff` quality while low signal (@lnv42)
 - Fixed `hf sniff` broken since 17ab86c52 (@nvx)
 - Added `--dumpmem` to proxmark3 client for memory dumping to file (@martian01010)
 - Changed `hw readmem` to allow larger reads, write to file and better hex viewer (@martian01010)
 - Added `CMD_READ_MEM_DOWNLOAD` and `CMD_READ_MEM_DOWNLOADED` to osimage and bootloader (@martian01010)
 - Changed `hf mfu info` - now correctly identifies NTAG I2C tags (@icemann1001)
 - Changed `hf 14b dump/view` - now supports `-z` flag for dense output (@iceman1001)
 - Changed `hf xerox dump/view` - now supports `-z` flag for dense output (@iceman1001)
 - Changed `hf mfu dump/view/eview` - now supports `-z` flag for dense output (@iceman1001)
 - Added `data crypto` to encrypt and decrypt data in the proxmark client using built-in methods (@team-orangeBlue)
 - Changed `hf tune` and `lf tune` - Added an option to show statistical data after tuning (@wh201906)
 - Changed `lf idteck demod --raw` - now supports raw hex string to decode (@iceman1001)
 - Changed `lf em 410x demod --bin` - now supports a raw binary string to demodulate. (@iceman1001)
 - Changed `lf em 4x05 info` - use `-v` verbose flag to see protection bits (@iceman1001)
 - Changed `lf em 4x05 info` - output now shows detailed bits (@iceman1001)
 - Added `lf em 4x05 view` - now supports viewing of dump files (@iceman1001)
 - Changed `hf 15 eview/view` - now supports `-z` flag for dense output (@iceman1001)
 - Changed `hf mf cload` - now supports specifying tag size (@augustozanellato)
 - Added `hf 14b raw --pico` - now supports picopass anticollision over ISO14443-B (@iceman1001)
 - Changed `hf 14b *` - worked apdu and comms. Improved output. Uses NG packets (@iceman1001)
 - Fixed `data manrawdecode` - now copy to demodbuf even if em4100 decode fails (@iceman1001)
 - Changed `nfc decode` - now supports Android Managed Provision NDEF message decoding (@iceman1001)
 - Changed `hf_cardhopper` - Allow button presses to break, handle non-zero CID from reader by relaying RATS and improving PPS and WTX handling, more reliably cook ATS, ignore client packets on serial line (@nvx)
 - Fixed `data diff` - client no longer crashes when using short widths on long filenames (@iceman1001)
 - Added `hf 15 wipe` - fills card memory with zeros (@iceman1001)
 - Changed `hf xerox info` - now prints some part info (@iceman1001)
 - Added `hf xerox view` - view dump files of fuji/xerox tags (@iceman1001)
 - Changed `hf 15 findafi` - improved the params (@iceman1001)
 - Changed `hf 15 rdbl/rdmulti/dump` - should handle 4 vs 8 bytes block sizes in cards (@iceman1001)
 - Changed `hf 15 *` - all 15 commands now uses NG packets (@iceman1001)
 - Changed `hf 15 raw` - now supports `-k` keep field on and `-s` select (@iceman1001)
 - Changed `prefs set client.debug` - now also toggles the client side APDU logging (@iceman1001)
 - Changed `hf 14b sriwrbl` - now supports `--force` to override block checks. Thanks @gentilkiwi for the idea! (@iceman1001)
 - Changed `hf 14b sriwrbl` - now tries to verify the write (@iceman1001)
 - Changed `hf 14b apdu` - now supports tearoff (@iceman1001)
 - Fixed `hf 14b raw` - fixed a potential write out of bounds. (@iceman1001)
 - Fixed `hf 14b` commands got some serious overhaul and love. Better return values and packet handling (@iceman1001)
 - Changed `hf waveshare` - image is automatically scaled and cropped to match panel size (@socram8888)
 - Changed `hf waveshare` - image loading and processing is now done using [GDlib](https://github.com/libgd/libgd) (@socram8888)
 - Added trace of sniffed SEOS traffic between reader and card (@iceman1001)
 - Change `hf seos list` - print crc and annotate better (@iceman1001)
 - Added troubleshooting entry - ARM architecture error (@francis2054)
 - Fixed `lf pyramid sim` - wrong parameter handling (@iceman1001)
 - Fixed bootloader - Ignore jitters when pressing the button (@wh201906)

## [Steamboat Willie.4.17768][2024-01-03]
 - Changed `mem spiffs dump -t` - now supports downloading direct into trace buffer (@hazardousvoltage)
 - Changed `hf search` - added hints for all HF protocols we search for (@iceman1001)
 - Changed `hf search` - added hint for iClass (@bettse)
 - Changed `hf 14a apdu` - It now uses the FWI and SGFI values from the ATS to determine an appropriate timeout (@nvx)
 - Added a thread to check when device comes online again. It will connect and update prompt (@iceman1001)
 - Changed CLI offline prompt - replaces the old prompt when offline is detected (@iceman100)
 - Changed `hf mf info` - it now uses found keys to try identify Gen2 cards (@iceman1001)
 - Changed `hf 14a info` - disabled debug logging while performing the magic tests (@iceman1001)
 - Changed `hf 14a info` - magic detection to better detect different GDM/USCUID configurations and improved Gen2/CUID detection when default keys are used (@nvx)
 - Changed `hf_cardhopper` - standalone mode to allow running over the internal Proxmark3 USB-CDC serial port (@nvx)
 - Fixed CLI prompt - Update connection type prompt after running `hw connect` (@wh201906)
 - Changed `uart_receive()` - Check if TCP connection is lost (@wh201906)
 - Change `data detectclock` - now tries all clocks if called w/o any params (@iceman1001)
 - Changed `lf search -1u` - improved the autocorrelation detection for unknown signals (@iceman1001)
 - Fixed `hf emrtd dump` stack smashing on device side (@iceman1001)
 - Changed `dbprint` on device side to use max 200 chars strings. (@iceman1001)
 - Fixed bootloader to correctly clear bss segment on start. Fixes USB serial number sometimes not working in the bootloader (@nvx)
 - Changed `notes on downgrade attacks` - reworked the original text follow repo style (@iceman1001)
 - Added `hf mf info` command and static encrypted nonce detection (@merlokk)
 - Added Saflok KDF - generate MFC keys (@h1kari)
 - Changed `lf fdx demod` - now raw bytes shows all data (@iceman1001)
 - Changed `data num` - now can print reversed and inverse (@iceman1001)
 - Fixed `hf mf sim -ix` never returning console (@datatags)
 - Added standalone mode `hf_unisniff` combining 14a/14b/15 sniffing with extra flash save options (@hazardousvoltage)
 - Added encryption and other SL3 functions for Mifare Plus - more to come (@team-orangeBlue)
 - Fixed the corrupted data in real-time sampling (@wh201906)
 - Added a slider in the plot window for navigation (@wh201906)
 - Fixed client build bug with Python 3.12 (@wh201906)
 - Fixed `ExchangeAPDUSC()` in `cmdsmartcard.c` to prevent client crash (@wh201906)
 - Added real-time LF sampling support (@wh201906)
 - Changed `hf 14a info` - now reads and prints QL88 sigantures (@iceman1001)
 - Fixed `hf iclass dump` truncating AA2 blocks and improve reliability (@nvx)
 - Added some info about UMC in "doc/magic_cards_notes.md" (@temskiy)
 - Added `hw bootloader` to reboot to the bootloader mode (@wh201906)

## [Faraday.4.17511][2023-11-13]
 - Fixed Python support of `experimental_client_with_swig` (@doegox)
 - Use proxmark3 as a generic smartcard reader with other software with `smart relay` (@gm3197)
 - Added `tools\mfkeys\staticnested` - program to recover static nested keys (@iceman1001)
 - Added `pm3_gen_dictionary.py` - python script to extract and save all keys from MFC dump files. (@iceman1001)
 - Changed `hf mfu info` - now detect MIFARE Ultralight AES (@iceman1001)
 - Changed `hf mf autopwn` - now supports multiple user supplied keys (@iceman1001)
 - Added `hf mf gchpwd` command for change Gen4 GTU card access password (@merlokk)
 - Added `--ms` option in `hw status` to specify the timeout of connection speed test (@wh201906)
 - Added `hf mf ginfo` command for get info about Gen4 GTU configuration (@merlokk)
 - Added support for loading Flipper PICOPASS dump files (@iceman1001)
 - Fixed unknown chip identification (@jmichelp)
 - Fixed `nfc decode` - now properly handles MFU dump files (@iceman1001)
 - Added support for loading Flipper MCT/MFU dump files (@iceman1001)
 - Changed `data bmap` - now default `-m` is 8 (@iceman1001)
 - Added support for NTAG424 cards. (@dankar)
 - Additional fixes to configcard code for keyroll mode based on nfc-iclass output (@Antiklesys)
 - Changed lf sampling - improved the performance (@yah01)
 - Added `bind` option for network connections to specify the outbound address and port (@wh201906)
 - Changed `lf em 4x05 dump` - now supports the `--ns` nosave parameter (@iceman1001)
 - Fixed some wrong synchronization waits in usb_write() to increase the communication speed (@wh201906)
 - Added new command `data bmap` - breaks down a hexvalue to a binary template (@iceman1001)
 - Changed aid_desfire.json - added entreis from the Metrodroid project (@iceman1001)
 - Changed mad.json - added entries from the Metrodroid project (@iceman1001)
 - Changed `hf iclass dump` - now allow no save of dumped data (@iceman1001)
 - Changed `hf iclass calcnewkey` - Added calculations for old key elite and new key non elite (@Antiklesys)
 - Changed the CLI prompt to show tcp/udp if used (@iceman1001)
 - Changed `hw ping` - now shows transfer time (@doegox)
 - Added `hf mf encodehid` - writes HID legacy credential to a empty MFC (@iceman1001)
 - Added `hf iclass sam` - Added support for HID SAM Picopass communications (@iceman1001)
 - Add support for quoted arguments in the CLI, allowing spaces in them which
   are removed automatically (@jmichelp)
 - Added UDP support on Windows (@wh201906)
 - Added client communication timeout to preferences (@iceman1001)
 - Added IPv6 support (@wh201906)
 - Fixed `lf hid clone --bin` - now correctly handles sentinel bits (@iceman1001)
 - Experimental UDP support in linux (@iceman1001, @wh201906)
 - Changed CI scripts to speed up the builds (@wh201906)
 - Changed the timeout of local TCP connections (@wh201906)
 - Finalized implementation of configcard generation for keyroll when cardhelper is not present (@Antiklesys)
 - Added documentation for compiling on iOS (@The-SamminAter)
 - Fixed `hf iclass wrbl` - pagemap bit map for secured is now handled better (@iceman1001)
 - Changed `hf iclass view/decrypt` to detect SIO lengths better and show if legacy credentials are encrypted (@nvx)
 - Changed the json file formats for mfc, 14b, 15, legic, cryptorf, ndef (@iceman1001)
 - Deprecated the EML file format when saving dump files. (@iceman1001)
 - Added `sim014.bin` - new sim module firmware v4.42 with improved ISO7816 Protocol T0 support (@gentilkiwi)
 - Added datasheet for sim module (@iceman1001)
 - Changed `smart raw --timeout` - allows for a custom timeout (@iceman1001)
 - Changed `lf t55 detectp1` - now also accepts 0xE039 Silicon Craft Tech as valid card (@iceman1001)
 - Fixed `utils.lua` library function "convertdectohex" wasn't working (@iceman1001)
 - Added `hf iclass creditepurse` command to allow crediting the epurse debit value (@nvx)
 - Modified `hf iclass configcard` to only support online mode (@Antiklesys)
 - Modified `hf iclass configcard` command to generate config cards without a cardhelper module by porting the contents of blocks 6 & 7 from nfc-iclass (@Antiklesys)
 - Fixed `hf iclass info` command showing incorrectly in offline mode (@Antiklesys)
 - The "doc/magic_cards_notes.md" file has been rebuilt, filled up, and so on. (@team-orangeBlue)

## [Raccoon.4.17140][2023-09-09]
 - Changed text and adjust pm3_test case for mf_aes_brute (@doegox)
 - Fix CPPChecker warnings (@doegox)
 - Fix TubmleWeed docker setup (@doegox)
 - Added default keys (@ernestask) (@craftbyte)
 - Fixed MFU authentication to send PACK correctly (@shallax)
 - Fixed list output when line has 16 bytes (@piru)
 - Changed AIDlist w new entries (@kormax)
 - Swapped to OE1 for Shallow modulation on RDV4. Thanks to @gentilkiwi for testing (@d18c7db)
 - Changed iClass SIO and Legacy credential detection to be more reliable (@nvx)
 - Add hf_cardhopper standalone mode for long-distance relay attacks (@startrekdude)
 - Added `hf iclass esetblk` - set iClass emulator memory block data (@nvx)
 - Added cryptorf regressiontests (@iceman1001)
 - Fixed `cryptorf/sma_multi` - local state used in multithread (@iceman1001)
 - Changed `fpga_compress` - better deallocation of memory and closing of file handles (@iceman1001)
 - Changed `hf search` - less swaps of fpga images on device side (@iceman1001)
 - Changed `mkversion.sh` - now regenerates version_pm3.c (and consequently the binaries) only when needed (@doegox)
 - Added `data atr` - a command to lookup ATR (@iceman1001)
 - Fixed bug in ATR lookup fct, thanks @DidierA  (@iceman1001)
 - Updated ATR list (@iceman1001)
 - Changed `mem load -m` - now correctly erase all allocated flash memory (@iceman1001)
 - Fixed emulator quick dump to handle MFC Ev1 extra sectors (@iceman100)
 - Removed some empty dump files (@iceman1001)
 - Added a fct to choose fpga mode (@iceman1001)
 - Changed `hf mf eload/gsave` - fast uploading to emulator memory (@iceman1001)
 - Added empty dump files with keys for easy simulation (@iceman1001)
 - Added `hf 15 view` - view ISO15693 dump files (@iceman1001)
 - Fixed `hf iclass config` - now loops correct in keyroll generation (@iceman1001)
 - Added `hf iclass sam` - skeleton command (@iceman1001)
 - Changed `lf cotag demo` - a new decoder (@iceman1001)
 - Changed `hf legic view/eview/info` - now in verbose mode will print raw hex dump (@iceman1001)
 - Added new test for cotag demod using data commands in pm3_test.sh (@iceman1001)
 - Added new sample trace file for cotag w fc/272. Thanks s1acky! (@iceman1001)
 - Fixed `hf legic eload` - now it doesn't crash client (@doegox)
 - Changed `lf hitag *` - rework client side (@doegox)
 - Changed data commands to handle ask/nrz clocks above 256 (@iceman1001)
 - Added `data envelope` - almost acts like data askedgedetect (@iceman1001)
 - Added `data cthreshold` - acts like an inverted dirtythreshold command. Remove center values (@iceman1001)
 - Added `hf mfp list` - interprets MIFARE Plus commands in traces (@DidierA)
 - Changed `hf legic sim` - loop and return codes on deviceside updated to DEFINES (@iceman1001)
 - Changed `hf legic einfo` - now accepts the three different cardsizes as params (@iceman1001)
 - Fixed `lf cotag reader -1` - now doesn't fail (@iceman1001)
 - Added support for LZ4 compressed hardnested tables (@doegox)
 - Changed `emv reader -v` - now tries to print found transactions logs (@iceman1001)
 - Added ISO4217 currency lookup (@iceman1001)
 - Fixed bad free in loadfilebinarykey fct.  Thanks to @gentilkiwi
 - Changed `emv reader -v` - now can decode track1/2 data if found (@iceman1001)
 - Added `emv reader` - act as a EMV reader (@iceman1001)
 - Added support for Apple Wallet NFC Passes with the Value Added Services protocol implementation (@gm3197)
 - Fixed compiling liblua on iOS (@The-SamminAter)
 - Changed `hf_mf_luxeo_dump.lua` - now have list of keys to iterate (@iceman1001)
 - Fixed the timeout of TCP connections (@wh201906)
 - Added `hw timeout` - make the connection timeout configurable (@wh201906)

## [Seven.4.16717][2023-06-25]
 - Change `hf 14a info` - now identifes QL88 tags (@iceman1001)
 - Added support for compiling on iOS (@The-SamminAter)
 - Fixed viewing MFC dump - border char is now white (@iceman1001)
 - Changed `data diff` - to print filenames in header if it fits (@iceman1001)
 - Changed viewing MFC dump files - it now colors ACL + GPB bytes (@iceman1001)
 - Added `hf mf supercard --furui` - now supports key recovery from Furui detection card. Thanks foxushka! (@iceman1001)
 - Added `hf topaz dump --ns` - now supports nosave param (@iceman1001)
 - Changed `hf topaz rdbl` - unified output (@iceman1001)
 - Fixed `hf topaz wrbl` - now supports tear off and write_nonerase command (@iceman1001)
 - Fixed `hf mf` commands (@iceman1001)
 - Fixed `hf mfp` commands (@iceman1001)
 - Added more default keys (@iceman1001)  Thanks anon!
 - Fixed `pm3-flash-all` shell script now correctly identify the if running on outdated bootloader (@iceman1001)
 - Fixed `hf 15693/iclass sniff` trace timings (@nvx)
 - Fixed LegicCash segment handling in `hf_legic.lua` script (@jmichelp)
 - Fixed `trace list` - now handles marking of crc bytes w color a bit better (@iceman1001)
 - Changed `hf mfu pwdgen -r` - now generates pwd/pack for Philips Sonicare, thanks @ckuenzi, @atc1441 (@iceman1001)
 - Changed `hf mfu info` - now detects Philips Sonicare devices (@iceman1001)
 - Fixed truncated FPGA upload due to incorrect integer size variable (@d18c7db)
 - Changed `usart btfactory` - handles the new BT board with version "BT SPP V3.0" (@iceman1001)
 - Changed `hf mf eview --sk` - now can extract keys and save to file (@iceman1001)
 - Changed `hf mf view --sk` - now can extract keys and save to file (@iceman1001)
 - Changed `hf mf sim` - reduce 6ms threshold to 4ms for reset to idle #1974 (@net147)
 - Rebuilt the Spartan-2 `fpga_*.bit` files to include the `hi_iso14443a.v` update (@d18c7db)
 - Added minor orphaned change from `hi_iso14443a.v` in `fpga-xc3s100e` to `hi_iso14443a.v` in `fpga-xc2s30` (@d18c7db)
 - Added python3 script to convert amiibo nfc Flipper Zero files to eml files to be used with Proxmark3 (@OscarAkaElvis)
 - Changed `hf mf restore` - Auth both key A and key B with default password (@wh201906)
 - Changed `nfc decode -f` - now can detect and convert MFC dumpfiles to NDEF byte arrays (@iceman1001)
 - Changed `nfc decode` - now handles EXTERNAL RECORDS better (@iceman1001)
 - Fixed `nfc decode` - now handles NDEF Signature version1 records better (@iceman1001)
 - Added new standalone mode `LF_MULTIHID` - HID26 (H1031) multi simulator (@flamebarke)
 - Changed `hf 14b dump --ns` - now supports `no save` of card memory (@iceman1001)
 - Changed `hf mfu dump --ns` - now supports `no save` of card memory (@iceman1001)
 - Changed the PM3 client to honor the preferences dump/trace paths. experimental support (@iceman1001)
 - Added the possibility to load `.MCT` dump files (@iceman1001)
 - Changed `lf t55xx dump --ns` - now supports `no save` of memory (@iceman1001)
 - Fixed the USB enumeration process (@wh201906)
 - Fixed `hf mf rdsc` - now correctly gets size in bytes when sector is larger than 32 (@iceman1001)
 - Changed `hf mf supercard` - Support editing UID and recovery of keys from second generation card (@AloneLiberty)
 - Added iClass credit key to default iClass key table and reorganized key order (@GuruSteve)
 - Changed `hf mf value` - ability to use transfer on different block (@AloneLiberty)
 - Changed `hf mf dump --ns` - dump command now supports `no save` of MFC card memory (@iceman1001)
 - Added `hf mf gdmsetcfg` - Supprt Gen4 GDM write configuration block (@iceman1001)
 - Added `hf mf gdmcfg` - Support Gen4 GDM read configuration block (@iceman1001)
 - Changed magic note to include a section about GDM tags (@iceman1001)
 - Added `hf mf gdmsetblk` - Support Gen4 GDM write block (@iceman1001)
 - Changed `hf 14a info` - detect Gen GDM magic tags (@iceman1001)
 - Changed CLI max string argument length limit from 512 to 4096 (@iceman1001)
 - Fixed `data asn1` - now handles bad input better (@iceman1001)
 - Added new public key for signature MIFARE Plus Troika (@iceman100)
 - Fixed the client build on Android (@wh201906)
 - Added TCP connection support on Windows (@wh201906)
 - Added `data num` - easy convert between dec/hex/bin (@iceman1001)
 - Fixed `hf mfdes info` - now handles incorrect tag answers better (@didiera)
 - Fixed `hf mfdes` generic help text is now correct (@didiera)
 - Fixed `pm3` script to correctly identify WSL enabled distros (@henrygab)
 - Changed device enumeration with "unique USB serial numbers when built with `FLASH` -- **_UPDATES BOOTROM ALSO_**" (@henrygab)
 - Changed the readline package to v8.2 in the CMAKE files for the client (@iceman1001)
 - Fixed `pm3` script for passing arguments (@doegox)
 - Fixed python paths to include current directory (@jmichelp)
 - Fixed infinite loops in spindelayus (@lnv42)
 - Changed ICECLASS standalone to support a read/sim mode (@natesales)
 - Changed `hf iclass encode` - added verbose flag (@natesales)
 - Changed `hf waveshare` - now identify 1.54 nfc epaper correct (@ah01)
 - Fixed `Makefile` regression that broke `make install` (@henrygab)
 - Fixed `lf em 4x70 brute` - now works as expected (@adite)
 - Fixed the lf sampling when bits_per_sample is less than 8 (@wh201906)
 - Added `lf em 4x70 brute` command (@adite)
 - Added documentation for usage of Proxmark3 under WSL2 (@henrygab)
 - Fixed device permissions via updated `udev` rules (@henrygab)
 - Added `--back` option to `clear` command to clear the scrollback buffer (@wh201906)
 - Changed `hf iclass decrypt` - mark credentials as decrypted in the dump (@natesales)
 - Changed `hf iclass view` - show credentials on a decrypted dump (@natesales)
 - Changed `hf mfu info` - NTAG213TT tamper info (mjaksn)
 - Added commands for configuring NTAG213TT tamper featue (@mjaksn)
 - Added Mifare Classic EV1 signature write support to gen4 magic tag lua script (@augustozanellato)
 - Added XOR key extraction and flag to Guardall G-Prox II (@GuruSteve)
 - Changed verbiage on `hf iclass info` KeyAccess area to be congruent with AA1 and AA2 areas (@GuruSteve)
 - Added `hf legic info` command for other sources (@0xdeb)
 - Added `hf legic einfo` - views emulator menory (@0xdeb)
 - Changed `hf legic view` - now also print the decoded info of the dump file (@0xdeb)
 - Changed `script run hf_mf_ultimatecard.lua -u` to support 10bytes UID (@alejandro12120)
 - Updated documentation for installation on macOS with MacPorts (@linuxgemini)
 - Added possible Paxton id to hitag2 tag info output
 - Changed `hf mf sim` - reduce 50ms threshold to 6ms for reset to idle #1974 (@net147)
 - Update `amiibo_tools.lua` with new identifiers and create a python script `update_amiibo_tools_lua.py` to automate the process in the future. (@CorySolovewicz)
 - Added `lf paradox sim --fc --cn` - Simulates Paradox fob from facility code and card number (jerji)


## [Nitride.4.16191][2023-01-29]
 - Changed `build_all_firmwares.sh` to fit GENERIC 256kb firmware images (@doegox)
 - Fixed some coverity fixes (@iceman1001)
 - Fixed `make accessrights` on Fedora (@mooey5775)
 - Fixed `hf mfu info` - can now identify the 50 pF version of NTAG 210u(micro) (@mjacksn)
 - Added `hf 15` sub-commands for controlling EAS, AFI, privacy mode, and the setting of passwords on SLIX tags (@mjacksn)
 - Added new magic gen4 cards command in docs (@McEloff)
 - Added `hf tesla info` - intital information command to read TESLA cards (@iceman1001)
 - Changed `hf emrtd info` - looking for lower case .bin extensions (@iceman1001)
 - Changed `hf emrtd dump` - looking for lower case .bin extensions (@iceman1001)
 - Changed `lf paradox clone` - it now accepts FC/CN (@mwalker33)
 - Added standalone mode for simulatin Nedap ID (@anon)
 - Changed `hf mfu info` - now also does a simple OTP fingerprinting (@iceman1001)
 - Changed `hf mf wrbl` - now checks for strict readonly ACL's in the data to write (@iceman1001)
 - Changed `hf mf view` - verbose printing if strict readonly ACL's exists in dump file (@iceman1001)
 - Added command `piv authsign` to get a buffer signed by the selected key (@jmichelp)
 - Added command `piv scan` which tries to read all known containers on PIV (@jmichelp)
 - Added support for PIV commands, over wired and contactless interfaces (@jmichelp)
 - Added `--shallow` option to `hf iclass` reader commands to do shallow (ASK) reader modulation instead of OOK (@nvx)
 - Change and improved NXP SLI/SLIX series tag identification (@nvx)
 - Fixed buffer overflow in "lf em 4x05 sniff" (@HeinrichsH)
 - Fixed potential NULL array printing (@jmichelp)
 - Added PIV aid to resource file (@jmichelp)
 - Fixed failing compilation on Proxspace environment due to how python is initialized (@jmichelp)
 - Fixed length check in sim module communications (@jmichelp)
 - Changed timings in i2c.c when communicating with sim module (@iceman1001)
 - Moved to non-deprecated API to initialize Python interpreter (@jmichelp)
 - Changed `sc upgrade` updated firmware v4.13 (RDV40) - frame buffer is now 384 bytes (@sentiprox)
 - Fixed contact interface / smartcard APDU chaining logic and allow 256 bytes ADPU payload. Need SIM firmware 4.13 to work (@jmichelp)
 - Fixed `lf hitag dump` - Should now work as described in the command help (@natmchugh)
 - Fixed SPI flash overflow when loading dictionnaries into flash. Breaking change: added 1 more sector for Mifare - dictionnaries should be loaded again (@jmichelp)
 - Added `hf mf gload, gsave, ggetblk, gsetblk` for Gen4 GTU in mifare classic mode (@DidierA)
 - Fixed `trace list -r` (relative times) not working unless `-u` (microseconds) was specified, and made `--frame` respect `-u` and `-r` options (@nvx)
 - Added detection of magic Gen4 GTU (@DidierA)
 - Added luascript `hf_i2c_plus_2k_utils` - Script for dumping/modifying user memory of sectors 0 and 1 (@flamebarke)
 - Added `hf mfu esave` - saves emulator memory to mfu dump file (@DidierA)
 - Added luascript `hf_mfu_ntag` - Script for configuring NTAG216 configuration pages (@flamebarke)
 - Changed `hf mf hardnested` - a detection for static encrypted nonces (@iceman1001)
 - Added requirements.txt file to tools folder. Minimum to run pm3_tests.sh (@iceman1001)
 - Changed `hf mf hardnested` - now can detect and use MFC EV1 signature sector key (@iceman1001)
 - Changed `hf mf autopwn` - now can detect and use MFC EV1 signature sector key (@iceman1001)
 - Fixed `pm3` shell script now automatically detects WSL2 with USBIPD serial ports (@iceman1001)
 - Fixed `trace list -c` - annotation of CRC bytes now is colored or squared if no ansi colors is supported (@iceman1001)
 - Fixed `trace list -t mf` - now also finds UID if anticollision is partial captured, to be used for mfkey (@iceman1001)

## [Radium.4.15864][2022-10-29]
 - Changed `lf indala sim` - now accepts fc / cn (@iceman1001)
 - Added `lf indala brute`- brute forcing of 64b Indala ID (@iceman1001)
 - Added `hf 14a ndefwrite` - write raw NDEF records to TYPE4A tags (@iceman1001)
 - Changed ndef output to be more dense. Honors verbose now (@iceman1001)
 - Fixed `hf mf ndefwrite` - now skips not ndef formatted sectors (@iceman1001)
 - Fixed `hf mf ndefread` - now skips not ndef formatted sectors correctly (@iceman1001)
 - Fixed `lf hitag` - keep inside the arrays for key/password/nrar (@iceman1001)
 - Added `hf mf ndefwrite` - write raw NDEF records to MIFARE Classic tag (@iceman1001)
 - Changed `hf mf cwipe` - swapped the block0 data to genuine manufacture ones (@iceman1001)
 - Added `hf mf ndefformat` - format a MIFARE Classic tag as NFC tag with Data Exchange Format (NDEF) (@iceman1001)
 - Changed `hf 14b dump, view` to get correct chip type in case of SRT512 and friends (@DidierA)
 - Added `hf 15 eview` and `hf 15 esave` - Retrieve emulator image for ISO15693 simulation (@markus-oehme-pg40)
 - Changed `hf 15 sim` - now supports reader writes (@markus-oehme-pg40)
 - Added `hf 15 eload` - specify memory image for ISO15693 simulation (@markus-oehme-pg40)
 - Added `hf 15 sim --blocksize` - configure block size for simulation (@markus-oehme-pg40)
 - Fixed buffer overflow in mfu ndef decode (@mwalker33)
 - Changed spiffs write/append to send in 8192 chunks to ensure its eraised (@mwalker33)
 - Fixed spiffs dump to ensure to fails correctly if no big_buff was allocated (@mwalker33)
 - Change Client Makefile to respect global flags (@blshkv)
 - Change Makefile, honors global CC values (@blshkv)
 - Fixed bad memory handling in MifareSim device side (@iceman1001)
 - Added json topaz file format (@iceman1001)
 - Added `hf topaz rdbl,  wrbl, view` commands (@iceman1001)
 - Added more details to the annotations of `hf mfdes list` output (@nvx)
 - Changed `hf iclass view` and related to suppress consecutive blocks with repeated contents with a `-z` flag, or `prefs set output --dense` (@nvx)
 - Changed `hf iclass list` to display matched keys on the CHECK command rather than the card response, and made it check for elite keys too (@nvx)
 - Fixed `hf iclass info` and `hf iclass view` key access info looking at the wrong card config bit (@nvx)
 - Added `hf gallagher decode` command and fix Gallagher diversification for card master key (@nvx)
 - Changed `hf texkom reader` - now supports mmbit-002 (kibi-002, kb5004xk1) russian tag (@merlokk)
 - Added `hf sniff --smode` skip/group adc data to consume less memory. Now it can sniff very long signals (@merlokk)
 - Added `hf fudan` skeleton commands (@iceman1001)
 - Added `--reboot-to-bootloader` arg to pm3
 - Changed `hf 14b raw` - now supports selecting Fuji/Xerox tag (@horror)
 - Added `hf xerox dump` - dump a Fuji/Xerox tag (@horror)
 - Added `hf xerox info` - read Fuji/Xerox tag (@horror)
 - Fixed `hf 14a reader --ecp` to work consistently (@kormax)
 - Change `trace list -t 14a` to annotate ECP frames of all valid V1 and V2 formats (@kormax)
 - Added 122 new keys from Flipper Zero community to `mfc_default_keys.dic` (@UberGuidoZ)
 - Added showing password for the read command in the `lf t55xx sniff` command (@merlokk)
 - Added reading texcom tk13 and tk17 tags with `hf texkom read` command (@merlokk @iceman1001)
 - Added simulating texcom tk13 and tk17 tags with `hf texkom sim` command (@merlokk)
 - Changed `hf mf staticnested` - significant speedups by using two encrypted nonces (@xianglin1998)
 - Change use of `sprintf` in code to `snprintf` to fix compilation on macOS (@Doridian)
 - Change `hf mf mad` - it now takes a dump file for offline MAD decoding (@iceman1001)
 - Fixed Standalone mode  hf_iceclass - wrong fpga image set (@iceman1001)
 - Fixed `hf iclass sim` - wrong fpga image set (@iceman1001)  Thanks to NVX!
 - Fixed `lf viking reader` - search in inverted bitsteam as well (#doegox)
 - Added more default keys (@CONIGUERO)
 - Change `hf 14a info` - it now detects FUDAN (@iceman1001)  Thanks to @secit-pl
 - Fixed support to clone Pyramic, Paradox and Awid on EM4x05 (@doegox)
 - Added support to clone HID, Viking and EM410x on EM4x05 (@doegox)
 - Fixed `lf config --reset` - averaging is set to 1 rather than 0 (@wh201906)
 - Added standalone mode for sniffing 14b (@jacopo-j)
 - Fixed `hf 14a apdu` - now don't skip first P2 iteration (@iceman1001)
 - Added `hf ntag424` - skeleton with SDM (@iceman1001)
 - Updated hf_14a_i2crevive.lua - fixed broken apdus (@Equipter)
 - Use 128 bit slices whenever ARM NEON is available to speed up hardnested (@Doridian)
 - Fixed compilation of mfd_aes_brute on Apple M1 based Macs (@Doridian)
 - Changed calculation of companion ARM firmware hash to be uniform accross platforms (@Doridian)
 - Changed `hf mf *` - verbose flag now also decode and prints found value blocks (@iceman1001)
 - Changed `hf mf wrbl` - added more helptext and new param --force to point out block0 writes (@iceman1001)
 - Changed `hf 15 raw` - it now uses NG frame for response (@iceman1001)
 - Added `hf_14a_i2crevive` Script, revives soft bricked i2c (@equipter)
 - Changed `hf mf view` - now prints all keys and all value blocks under the ´verbose´ param (@iceman1001)
 - Added a new OSX install guide (@maxieds)
 - Added new public key (@anon)
 - Changed start screen to be less intimidating (@iceman1001)
 - Added detection of a possible mismatch between client and Proxmark3 image (@doegox)
 - Changed `hf 14a info` - added a ATR historical compact TLV decoder (@iceman1001)
 - Added `hf mf value` - decode a value block (@iceman1001)
 - Changed `hf mf nested` - removed option `--single` redundant with usage of `--tblk` (@doegox)
 - Fixed `hf mf chk` - single block mode (@doegox)
 - Fixed `hf mf fchk/chk` - internal logic to load keys (@doegox)
 - Changed `hf mf *` - printKeyTable: now display sector trailer info too (@doegox)
 - Changed `hf mf chk` - option `--blk` into `--tblk` (as for nested) (@doegox)
 - Added new tool `mfd_multi_brute` - MIFARE DESfire / UL-C key recovery (@iceman1001)
 - Fixed `hf emrtd info` - segfault on some platforms (@doegox)
 - Fixed `hf emrtd info` - when offline (@doegox)
 - Fixed `commands.json` - generation (@doegox)
 - Added new standalone mode `hf_legicsim` (@uhei)
 - Changed `hf legic *` - now uses NG instead (@iceman1001)
 - Added `hf legic view` - view contents of LEGIC Prime dump files (@iceman1001)
 - Changed `hf mfu restore` - now takes bin/json as dump files (@iceman1001)
 - Added `hf mfu view` - view contents of MFU dump files (@iceman1001)
 - Changed `hf_mf_uidbruteforce` - added support for S70, enhance UID length management (@cactuschibre)
 - Fixed build issues that may happen from building `mfd_aes_brute` (@linuxgemini)
 - Added silicon data parsing logic for NXP chips in `hf mfu info` (@linuxgemini)
 - Added luascript `hf_mf_em_util.lua` - Script for emulator configuration (@nisgola)
 - Fixes `hf mf restore` - now takes bin/eml/json as dump files (@iceman1001)
 - Fixes `script run some_python_script` segfault on armhf architecture (@doegox)
 - Added `trace extract` - extract authentication parts from trace (@iceman1001)
 - Added luascript `hf_mf_ultimatecard.lua` - Script for Ultimate Magic Card (GEN4) (@startrk1995)
 - Added new tool `brute_key` - MIFARE DESFire Telenot access AES recovery (@x41sec)
 - Fixed `hf mfu dump -k` - insert PWD in dump (@doegox)
 - Changed `hf mfu pwdgen` - now generate xiaomi air purifier pwd/pack (@doegox)
 - Fixed `hf 14a sim` - sneaky detection of user supplied UID might be empty (@iceman1001)
 - Added support for PCSC's proprietary HID 37bit format P10004 (@bthedorff)
 - Added `nfc decode` - now NDEF vCard messages with a PHOTO in base64 format is shown (@iceman1001)
 - Changed - AID limitations when using Gallagher key diversification (@DarkMatterMatt)
 - Added new standalone mode `lf_em4100rsww` (@zabszk)
 - Fixed `hf 15 slixdisable` wrong pass id (@r1ddl3rz)
 - Added `script run hf_mf_hid_sim.lua` (@micsen)
 - Added flashmem support in `HF_14BSNIFF` standalone mode (@wh201906)
 - Changed `HF_14ASNIFF` standalone mode - now supports Proxmark3 without flashmem (@wh201906)

## [Frostbit.4.14831][2022-01-11]
 - Changed Wiegand format lookup - now case-insensitive (@iceman1001)
 - Added new standalone mode `hf_15SNIFF` - Same as `hf_14ASNIFF` standalone mode for RDV4 - flashmem (@startrk1995)
 - Added `hf gallagher` commands for read/writing DESFire cards (@DarkMatterMatt)
 - Added support for MIFARE DESFire application creation without authentication (@joswr1ght)
 - Changed drastically Hitag S ARM code to remove state machines and ease way to build new commands (@doegox)
 - Fixed Hitag S crypto mode with key or NrAr, fixed `lf hitag cc`, fixed pwd dump in hitagS dump with LKP (@doegox)
 - Changed `trace list -h` - textual change (@iceman1001)
 - Fixed `hf iclass config` - not get stuck when trying to make a keyroll config card (@iceman1001)
 - Changed textual output for iclass (@iceman1001)
 - Changed `hf iclass reader` to use NG (@iceman1001)
 - Changed various notes to have ToC and top links (@iceman1001)
 - Added option `lf gprox demod --raw` - now supports decoding of raw bytes (@iceman1001)
 - Added option `hf mf gview --pwd` - now supports user supplied password (@iceman1001)
 - Added option `--force` to `hf mfu rdb/wrbl` to force operation even if address is out of range (@doegox)
 - Added documentation for detailed usage of the Ultimate Magic Card (@doegox)
 - Changed HitagS trace record and parsing to deal with partial bytes and to check CRC8 (@doegox)
 - Added support for KS X 6924 (South Korea's T-money card) (@toucan12)
 - Fixed `hf 15 dump` - now correctly dumps 256 blocks w/o crashing the client (@iceman1001)
 - Changed `hf 14a sim -t 3` - anticollision for DESFire simulation now uses different RATS (@mosci)
 - Fixed `hf mfdes` - works with dynamic apdu and encode/decode buffers (@merlokk)
 - Added luascript `hf_ntag_bruteforce.lua` - ntag password bruteforce with the option to do NFC Tools MD5 versions of passwords (@keldnorman)
 - Added option `--crc-ht` to `lf cmdread` to compute and add CRC-8/HITAG (@doegox)
 - Added option `-k` to `lf cmdread` to keep field on (@doegox)
 - Added `hf mf acl` - decode and print MIFARE access rights (@iceman1001)
 - Added for readline, <tab> autocomplete work inside pm3 client (@iceman1001)
 - Fixed `hf iclass dump` - now uses the right key when suppling credit key (@iceman1001)
 - Changed crc16_legic to be supported in lua (@iceman1001)
 - Changed `trace list -t 14a` - now annotate magsafe wupa (@iceman1001)
 - Added magsafe polling (thanks to @tcprst !) (@iceman1001)
 - Changed `trace list -t 14a` - now annotate ECP (@iceman1001)
 - Added enhanced contactless polling (thanks to @relays user at gitlab) (@iceman1001)
 - Added iCS key (@FlUxIuS)
 - Changed `trace list -t 14a` - now annotate ULC when known key is used in auth (@doegox)
 - Fixed `hf mfu cauth` - option -k collision, used both for key and keep (@doegox)
 - Changed Dockerfiles, instructions, tests and CI to cover Python script within client (@doegox, @gator96100)
 - Changed `hw version` - now works offline too and provides client info (@doegox)
 - Added trace of VISA Apple transactions (thanks @a66at)
 - Added trace of reading a MIFARE UL-C with 3DES authentication (thanks @rad1game)
 - Added ATR fingerprinting (@iceman1001, @doegox)
 - Fixed DesFIRE decoding when not using wrapped cmds (@colinoflynn)
 - Added script to build all firmware variants (@doegox, @sbrun)
 - Fixed `hw tune` voltage values when no antenna is connected (@mwalker33)
 - Fixed `hf mf dump/auto/csave` MFC JSON dumps - save real ATQA/SAK (@doegox)
 - Fixed dynamic-stack-buffer-overflow in ndefDecodePayload (@AxisRay)
 - Added option to load raw NDEF to `nfc decode` command (@doegox)
 - Added option to save raw NDEF to all `hf * ndefread` commands (@doegox)
 - Changed `hf 14a info` - different FUDAN clone detection (@iceman1001)
 - Added `CROSS_CC` and similar args for maintainers, see Maintainers.md (@doegox)
 - Fixed crack5opencl - fix deadlock in wu_queue_destroy() + minor changes on threads.c (@matrix)
 - Fixed 14a incorrect BCC when multiple tags (@AxisRay)
 - Added additional support for macOS users utilizing MacPorts (@linuxgemini)
 - Fixed Windows USB prioritization (@gator96100)
 - Fixed `hf mfdes` - fix buffer overflow (@AxisRay)
 - Changed `hf mfdes` - detect LRP mode and info updates (@merlokk)
 - Fixed `hf mfdes` - increase response buffer length (@merlokk)
 - Fixed `SimulateTagLowFrequencyEx` ignoring the `ledcontrol` argument (@zabszk)
 - Added ledcontrol argument to LF operations (@zabszk)
 - Added new standalone mode `dankarmulti` -  select and run multiple standalone modes (@dankar)

## [crimson.4.14434][2021-09-18]
 - Fixed `hf mf staticnested` - flashmem / non loop now works (@horrordash)
 - Fixed `hf 15 restore` write command and `trace list -t 15` write command (@doegox)
 - Changed default standalone mode to LF_SAMYRUN (@iceman1001)
 - Added MIFARE Plus SL3 mode traces (@iceman1001)
 - Fixed PAC/Stanley checksum calculation (@rknoll)
 - Added option --mem to `hf mf nested`, from icopyx source (@doegox)
 - Added fpga-xc3s100e and icopyx source code specificities to this repo (@doegox)
 - Changed `hf mfdes` - Transactions. commit, abort, commit reader id. (@merlokk)
 - Changed `hf mfdes` - LRP secure channel for Desfire Light. (@merlokk)
 - Changed `hf mfdes` - commands refactoring. now fully works: des,2tdea,3tdea,aes keys with d40,ev1,ev2 channels and native/nativeiso/iso command sets(@merlokk)
 - Changed `lf em 410x *` - now better text (@clach04)
 - Fixed `hf mfu dump` - now sets keytype correct #1426 (@axisray)
 - Added new standalone mode `hf_mfcsim` which can load dump from flash and simulate Mifare Classic 1K card (@axisray)
 - Added support to demodulate Electra tags and column parity check for EM410x (@doegox)
 - Fixed demod plot for various demodulations (@doegox)
 - Fixed `lf t55xx detect/rdbl/dump` - to override if user set `lf config` and use default values during operation (@iceman1001)
 - Added `hf iclass encode --wiegand/--fc/--cn` - direct fmt/fc/cn support (@bettse)
 - Fixed `lf t55xx detect` - to unset lf config decimation if value is not one (@iceman1001)
 - Fixed `hf 15 sample` - data collection works again (@iceman1001)
 - Changed `data plot -h` - removed line (@doegox)
 - Fixed `lf_tharexde` - double define of global variable (@axisray)
 - Added new standalone mode `lf_hidfcbrute` - bruteforce facility code (@ss23)
 - Changed `hf felica info` - got more ic chip definitions (@ah01)
 - Changed `lf pyramid demod` - relaxed the size check (@iceman1001)
 - Changed `hf emrtd info` to view DG5 picture (@iceman1001)
 - Added `-4041x` option to lf indala clone for the Indala 4041X format (@chunkystew)
 - Added a new client preference,  delay of execution,  a delay in ms before a cmd is sent. Good for implants (@iceman1001)
 - Fixed `lf t55xx brute` - now correctly prints last key if it was correct (@scott4290)
 - Added support python scripts (@salmg)
 - Added new standalone mode `hf_reblay` -  relay 14a over bt (@salmg)
 - Added one new key from gun cloner found in wild (@scott4290)
 - Fixed `hf_msdsal` standalone in simulation flow (@salmg)
 - Added a picture viewer in QT. To be used with `hf emrtd info` (@iceman1001)
 - Fixed - move des functions to libcrypto (@merlokk)
 - Added `CLIGetOptionList` to cliparser that makes it easier to implement text options in the cli (@merlokk)
 - Added experimental support for macOS users utilizing MacPorts instead of Homebrew (@linuxgemini)
 - Added `pm3_online_check.py` - a script to verify and initialize a Proxmark3 RDV4 device (@iceman1001)

## [midsummer.4.13441][2021-06-25]
 - Fixed `hf iclass` - a crash when AA1 limit was larger than AA2 (@pcunning)
 - Added bruteforce function for the magic byte in `cmdlfnexwatch.c` and ability to clone with psk2 modulation (@Guilhem7, @MaximeBosca)
 - Changed `hw setmux` - improve user feedback for special case (@iceman1001)
 - Changed 'filename' - unified file name param across client (@iceman1001)
 - Fixed `lf em 4x05 brute/chk` - fixed input params crash (@iceman1001)
 - Fixed `lf hitag reader --23` - now accepts 6bytes key (@iceman1001)
 - Fixed coverity scans findigs (@merlokk, @iceman1001)
 - Fixed `hf iclass config` - now fallback to default config card configuration (@iceman1001)
 - Changed `nfc parser` - now also identify xvcard types (@iceman1001)
 - Added `hf mf gview` - view contents of a magic Gen3 GTU (@iceman1001)
 - Added Standalone mode for nexwatch ID credentials (@Guilhem7, @MaximeBosca)
 - Fixed `lf em 4x50/4x70 *`  reverted a misunderstanding in byte order macros (@iceman1001)
 - Added more keys (@equipter)
 - Changed `hf nfc ndefread` - ndef parser now handles more types (@iceman1001)
 - Fixed `hf desfire` changekey, GetUID, 3DES session key tweak. (@mwalker33)
 - Fixed `hf fido` commands now works correctly (@merlokk)
 - Moved / renamed  `client/resource/fido2_defparams.json` ->  `client/resource/hf_fido2_defparams.json` (@merlokk)
 - Added `hf cipurse` commands to work with cipurse transport cards (@merlokk)
 - Added `--gap` option to lf em 410x sim for more control over sim data (@mwalker33)
 - Changed `hf fido` - refactored load/save json objects (@iceman1001)
 - Moved / renamed  `fido2.json` ->  `client/resource/fido2_defparams.json` (@iceman1001)
 - Added openocd shikra support based on @ninjastyle82 patch to deprecated iceman fork (@iceman1001)
 - Changed `hf lto info` - now decode more of LTO CM info (@iceman1001)
 - Added `HF_TMUDFORD` standalone mode that reads and emulates ISO15693 UID (@tmudford)
 - Changed `hf mf wipe` - now supports enforcing sector 0 / block 0 writes (@iceman1001)
 - Added `data asn1` - a command to decode ASN1 byte arrays  (@iceman1001)
 - Added `hf 15 disableprivacy` - from @g3gg0 fork *WIP* (@iceman1001)
 - Added `lf_ident_json.lua` - script to identify t55xx json dump files (@iceman1001)
 - Fixed `hf iclass chk` -  multithread concurrency issues solved (@iceman1001)
 - Fixed hf_iceclass standalone - correct null terminator filename (@metalauricle)
 - Changed `trace list -t mfdes - added annotations for EV2, EV3 (@iceman1001)`
 - Changed `hf iclass lookup` - fixed swapped args (@iceman1001)
 - Changed `hf iclass decrypt` - added the possibility to decode the data as block6 if you have a cardhelper (@iceman1001)
 - Changed `weigand decode` - removed -p param since we wanna see all encodings anyway (@iceman1001)
 - Changed `wiegand decode` - when bin string is supplied, use that exact length instead (@iceman1001)
 - Added a bunch more wiegand formats (@iceman1001)
 - Added `lf idteck clone` - now supports creating using raw values (@iceman1001)
 - Changed calypso scripts to work with NG (@iceman1001)
 - Changed HF 14b - fixed timings on device side (@iceman1001)
 - Changed `hf 14b raw` - now uses NG (@iceman1001)
 - Changed `hf 14a/14b/15 list etc alias commands now unified helptext (@doegox)
 - Changed `trace list` - now colors whole reader line (@iceman1001)
 - Changed `lf search` - add option `-c` to continue searching after first hit (@doegox)
 - Fixed DESFire mis-annotation (@VortixDev)
 - Changed `lf pac demod` - now also search for inverted bitstreams (@iceman1001)
 - Changed `hf 14b reader` - now supports continuous mode (@iceman1001)
 - Fixed `hf search` - now doesn't false identify ISO15693 (@iceman1001)
 - Changed emv commands now works with tokenized cards (@merlokk)
 - Changed `hf 15 restore` - now also support EML/JSON (@iceman1001)
 - Changed - all commands now use cliparser (@iceman1001)
 - Changed `lf t55xx restore` - now also support JSON (@iceman1001)
 - Changed `hf mf csetuid` - adapted to accept 7byte uids ~untested~  (@iceman1001)
 - Added `hf mf view/eload/cload` - now accepts bin/eml/json  (@iceman1001)
 - Added `hf mf eload/cload' - now accepts bin/eml/json  (@iceman1001)
 - Fixed RESTORE mis-annotation (@VortixDev)
 - Changed `smart raw` - now uses NG (@iceman1001)
 - Added `hf iclass configcard` - now can download / generate config card dumps with a cardhelper [WIP] (@iceman1001)
 - Fixed swapped DESELECT and WTX annotations (@VortixDev)
 - Fixed `hf 15 findafi` - cliparser bug, and loop is user interruptible (@iceman1001)
 - Added `hf mf wipe` - takes a keyfile and wipes a MFC to default values (@iceman1001)
 - Changed `pm3 shell` - now prioritize USB connections in WSL1 (@gator96100)
 - Added `hf mf restore -w` - now supports usage of specified keyfile to authenticate with instead of default FFFFFF key (@iceman1001)
 - Added `mem info --pem`- now supports loading of private key in PEM format (@iceman1001)
 - Changed `hf mfp auth` - more verbose message (@vortixdev)
 - Fixed `hf mfdes rd` - AES chained mode fixes (@anon)
 - Added `weigand decode -b` - now supports binary wiegand string as input (@iceman1001)
 - Updated `README.md` - new link to a GUI project,  and textual update (@iceman1001)
 - Changed `hf search` - reenabled FeliCa search (@iceman1001)
 - Changed `auto -a` - now allows for keep on searching, useful when detecting unknown multi tech cards (@iceman1001)
 - Added more aid (@anon)
 - Changed `hw version` - now also looks for FPGA chip id match (@iceman1001)
 - Added WSL checks for outdated bootloader (@gator96100)
 - Changed many, many more commands uses cliparser (@iceman1001)
 - Changed many many cppchecker fixes (@iceman1001)
 - Changed some coverity scan fixes (@iceman1001)
 - Added `hw break` - to break device side loops from client (@iceman1001)
 - Changed `hf mf sim` - removed field detection on device side (@iceman1001)
 - Changed `mf_nonce_brute` - to recover key for nested authentications from traces (@iceman1001)
 - Changed `trace list -t mf` - now prints needed data for mf_nonce_brute (@iceman1001)
 - Fixed `trace list -t mf` crc overwrote first hex char (@iceman1001)
 - Fixed `trace list -t mf` to print correct parity "!" (@doegox)
 - Fixed `lf em 4x05 unlock` not to break when tear off happens (@doegox)
 - Fixed `mem spiffs view` to not print when file doesn't exist on flash mem (@iceman1001)
 - Changed `hf mf rdsc, egetsc, cgetsc` to have unified output (@iceman1001)
 - Changed `hf mf rdsc` output to be more clear (@iceman1001)
 - Changed `mem dump -c` now supports optional column breaks of output (@iceman1001)
 - Changed unified client menu's to use curly brackets to indicate a sub category (@iceman1001)
 - Added `mem spiffs view` to view external flash memory (@iceman1001)
 - Fixed 'lf t55xx config, chk, bruteforce' some logic to got mixed up in cliparser conversion (@mwalker33)
 - Moved countones / bitcount fcts to utils.c (@doegox)
 - Changed tunings in `hf mfu opttear` (@doegox)
 - Updated the text in T5577_Guide.md to match new output (@iceman1001)
 - Added a new t55xx configblock found in the wild (@iceman1001)
 - Added `mem info -d` to support manual input of data (@iceman1001)
 - Fixed 'lf t55xx detect' some logic to got mixed up in cliparser conversion (@mwalker33)
 - Added `HF_CRAFTBYTE` standalone mode that reads and emulates 14a UID (@craftbyte)
 - Added cryptoRF trace (@iceman1001)
 - Changed default tcp port to 18888  (@iceman1001)
 - Added more default keys to MFC dictionary (@iceman1001)
 - Added one more icode slix2 signature to recover_pk.py (@iceman1001)
 - Changed C15001 format to use OEM = 900 by default (@iceman1001)
 - Added a script to dump originality signatures from MFU EV1s and NTAGs (@aveao)
 - Fixed `hf emrtd` segfaults (@aveao)
 - Fixed `lf em 4150` timings on device side (@tharexde)
 - Changed dependencies fixes in build scripts (@doegox, @iceman1001, @gator96100)
 - Changed `recover_pk.py` to support colors (@iceman1001)
 - Changed flashing feedback text to include needed steps (@iceman1001)
 - Changed `lf_em4100_bulk.lua` in order to handle Proxspace odd 32b limit (@iceman1001)
 - Added `script run lf_em4100_bulk.lua` a EM4100 bulk enrolling script (@iceman1001)
 - Changed amiitool now linked, compiles in cmake,  and outlined code usage in cmdhfmfu.c (@iceman1001)
 - Changed `hf mfu info` - to have a fingerprint fct (@iceman1001)
 - Added parsing of EF_CardAccess to display PACE algorithm, version and parameter in `hf emrtd info` (@aveao)
 - Changed numerous commands more uses cliparser (@tcprst, @iceman1001)
 - Added more originality public keys (@anon)
 - Added `hf 14a info` - now also verify MFC Ev1 signatures (@iceman1001)
 - Added `LF_THAREXDE` standalone mode which simulates and reads EM4x50 cards (@tharexde)
 - Added `hf jooki` commands (@iceman1001)
 - Changed `lf hid clone` - also accepts binary wiegand (@iceman1001)
 - Changed `wiegand encode` - format param is now optional, w/o it will try encode all formats (@iceman1001)
 - Fixed cppchecker warnings (@iceman1001)
 - Added `trace list -t mf` - now can use external dictionary keys file (@McEloff)
 - Fixed `lf gallagher read` - now correctly decodes card data
 - Added support to `lf gallagher clone` and `lf gallagher sim` for writing specific card region, facility, card & issue numbers (@DarkMatterMatt)
 - Added support for older vid/pid detection (@Gator96100)
 - Added `hf mfdes bruteaid` - proper bruteforce of DESFire AID when no reading of files is possible (@craftbyte)
 - Added support for bidirectional communication for `lf em 4x50 sim` (@tharexde)
 - Changed `PLATFORM=PM3OTHER` to `PLATFORM=PM3GENERIC` (@iceman1001)
 - Added `tools/hitag2crack/crack5opencl`, an optimized version of `crack5gpu` (@matrix)
 - Fixed Makefile to account for changes when running on Apple Silicon (@tcprst)
 - Added support for debugging ARM with JTAG & VSCode (@Gator96100)
 - Added MFUL "Gen1b" support to `hf_mfu_setuid.lua` (@iceman1001)
 - Added possibility to get bargraph in `lf tune` and `hf tune` (@iceman1001, @doegox)
 - Added `hf emrtd` ePassport dumping and parsing (@aveao)
 - Added `aidsearch` to `hf 14b info` (@iceman1001)
 - Added `ICE_STATE_DUMP_SIM` - standalone mode for dumping/simming one iClass tag (@iconicsec)
 - Added  `lf em 4x50 eview` - show uploaded EM4x50 data in emul memory (@tharexde)
 - Fixed `data rawdemod` parsing for psk2 and user defined clock (@cyberpunk-re)
 - Added `hf iclass encode` - encode a wiegand binary to a encrypted credential (@iceman1001)
 - Changed `recoverpk.py` - now tests more ECDSA curves (@doegox)
 - Added `hf 14a apdufuzz`- a naive apdu cla/ins/p1p2/len fuzzer (@iceman1001)
 - Improved `hf 14a apdufuzz/apdufind` to find hidden APDUs (@ikarus23)
 - Fixed mixed up INC/DEC in MIFARE protocol defs (@vortixdev)
 - Added `lf em 4x70 info` - new support for ID48 transponders (@cmolson)
 - Fixed multiple coverity scan issues (@iceman1001)
 - Added a SIO item (@iceman1001)
 - Fixed `lf hid brute` - param (@iceman1001)
 - Changed `lf em` layouts (@iceman1001)
 - Changed many commands to cliparser (@iceman1001, @tcprst, ...)
 - Fixed issue #844 - `lf t55xx config` => recompute block0 (@cyberpunk-re)
 - Changed `lf em 4x50` - changed cli parameter from w (word) to d (data) (@tharexde)
 - Added `lf em 4x50 login` - authenticate against tag (@tharexde)
 - Added `lf em 4x50 brute` - guess password within a given password range (@tharexde)
 - Added `lf em 4x50 chk` - try passwords from dictionary (without option -> T55xx default dictionary or -f user dictionary) (@tharexde)
 - Added `lf em 4x50 reader` - read data from tag (configured data -> standard read mode), incl. option -@  (@tharexde)
 - Added `lf em 4x50 sim` - simulate dump from file or emulator/flash (@tharexde)
 - Added `lf em 4x50 restore` - restore dump file (bin, eml, json) onto tag  (@tharexde)
 - Added `lf em 4x50 esave` - dump em4x50 content in emulator memory to file (bin + eml + json) (@tharexde)
 - Added `lf em 4x50 eload` - upload em4x50 file content (bin, eml, json) to emulator memory  (@tharexde)
 - Changed EM4x50 - added LED signals (@tharexde)
 - Changed `lf em 4x50 dump` - now support json format (@tharexde)
 - Changed `lf em 4x50 wipe` - relocated write requests from device to client (@tharexde)
 - Renamed `lf em 4x50 write_passwordd` -> `writepwd` (@tharexde)
 - Changed `lf em 4x50` - all hex input parameters now have to be given in lsb format (output is still msb + lsb) (@tharexde)
 - Changed `lf em 4x50` - changed cli parameter from a (address) to b (block) (@tharexde)
 - Changed `lf em 4x50` - now supports cliparser (@tharexde)
 - Changed EM4x50 - stabilized and accelerated tag detection (@tharexde)
 - Changed EM4x50 - removed global tag structure on device side (@tharexde)
 - Fixed `hf 15 sim` - Added basic response to GET_SYSTEM_INFO and READBLOCK requests in order to fix iso15693 tag sim (@cyberpunk-re)
 - Added `mf mfu sim t 7 n <numreads>` - MFU emulation now supports automatic exit after <num> blocks read. (@cyberpunk-re)
 - Added T55xx Guide to assist in learning how to use the T55xx chip (@mwalker33)
 - Fixed `hf iclass wrbl` - dealing with tags in unsecured vs secured pagemode now is correct (@iceman1001)
 - Changed many commands to cliparser (@iceman1001, @tcprst, @mwalker33,...)
 - ...
 - Changed `hf iclass chk/lookup/loclass` speedups (@iceman1001)
 - Changed - ongoing conversion to cliparser (@tcprst, @iceman1001)
 - Added compilation options for 256k Proxmark versions, see doc (@doegox)
 - Added support for 10b UID in `hf 14a sim` (@doegox)
 - Added `HF_TCPRST` standalone mode which read and emulate IKEA Rothult cards (@tcprst)
 - Added Gallagher key checking/KDF on MIFARE Desfire (@NZSmartie)
 - Added dictionaries with common words of proper size (@will-caruana)
 - Added `hf mf supercard` (@iceman1001)
 - Added initial support for MIFARE Key Diversification, cf AN10922 (@NZSmartie)
 - Changed MIFARE detection improved (@VortixDev)
 - Changed `hf 14b sriread` to `hf 14b rdbl` and `hf 14b dump` (@iceman1001)
 - Added continuous mode to `hf 14a reader` (@doegox and @iceman1001)
 - Added `lf em 4x05_sniff` to allow extracting commands and passwords used be cloners. (@mwalker33)
 - Removed 'hf iclass replay' -  use the 'hf iclass dump' or 'hf iclass rdbl' with option "n"  instead (@iceman1001).  Concept taken from official repo (@pwpiwi)
 - Added Destron FDX-A support (@doegox and @iceman1001)
 - Added `lf em 4x05_chk` (@iceman1001)
 - Added `lf em 4x05_unlock` tear-off (@doegox and @iceman1001)
 - Added customizable 3DES key to hf mfu cauth (@socram8888)
 - Added generic `hw tearoff` and hooks in various write commands (@doegox and @iceman1001)
 - Added protect support for EM4x05 and fix various EM4x69/EM4x05 aspects (@doegox and @iceman1001)
 - Added incognito option to client to avoid mangling history & logs (@doegox)
 - Added option to hide/show plot sliders (@mwalker33)
 - Added "</>" key bindings to realign demod plot on samples (@doegox)
 - Added "T" key binding to trim plot (@doegox)
 - Added units options to `data timescale` (@doegox)
 - Added mouse scrolling to pan & zoom to plot (@doegox)
 - Added hf_14b_mobib Lua script (@iceman1001)
 - Added ASK CTx detection to hf 14b reader (@iceman1001 and @doegox)
 - Added low level support for 14b' aka Innovatron (@doegox)
 - Added doc/cliparser.md (@mwalker33)
 - Added `hf 14b apdu` - send APDU over ISO14443B (@iceman1001)
 - Added `lf t55xx chk e <EM4100> option` - Checks calculated password based on the EM4100 id from some white cloners formula by paleopterix (@mwalker33)
 - Added `lf t55xx sniff` to allow extracting commands and passwords used be cloners. (@mwalker33)
 - Added options to `lf read`, `lf cmdread`, `lf sniff` for repeated acquisitions (@doegox)
 - Changed options of `lf read` to match `lf cmdread`, this affects historical `d` and `s` options (@doegox)
 - Added `hf waveshare` to upload picture to Waveshare NFC-Powered e-Paper (@doegox)
 - Added `hf 14a config` to deal with badly configured cards: invalid ATQA/BCC/SAK (@doegox)
 - Added Mikron JSC Russia Ultralight EV1 41 pages tag type support (@McEloff)
 - Added test for Ultralight gen2 magic 'hf search' (@McEloff)
 - Added test for Ultralight EV1 gen2 magic 'hf search' (@McEloff)
 - Added `hf mf gen3*`magic gen 3 card operations (@McEloff)
 - Re-added verichip command which seems missing (@iceman1001)
 - Fixed missing t55x7 config block detection (@iceman1001)
 - Fixed missing define on proxspace (@mwalker33)
 - Added `lf em 4x50_dump` (@iceman1001)
 - Added `lf em 4x50_read` (@tharexde)
 - Added `lf em 4x50_info` (@tharexde)
 - Added `4x50_write` (@tharexde)
 - Added `4x50_write_password` (@tharexde)
 - Fixed em4x50 demodulation error (@tharexde)
 - Fixed `hf mfdes` authentication issues, DES working (@bkerler)
 - Added Android cross-compilation to client cmake (@dxl, @doegox)
 - Fixed `emv scan` - now saves in current folder and uses unique names (@iceman1001)
 - Fixed pm3.sh - parse COM ports larger than one digit (@doegox)
 - Fixed stack size and automatically use available space for BigBuf. Stack is now 5K (@slurdge)
 - Added Mifare MAD Card Holder Information decoding (@lukaskuzmiak)
 - Changed Better precision for HF and LF voltage measurements and Add theremin.py script (@rosco)
 - Added Mifare MAD Info Byte decoding. Easier to identify Card Publisher (@lukaskuzmiak)
 - Changed use system Bzip2 library instead of Zlib for hardnested tables, tables are now 7x smaller (@doegox)
 - Changed pushed some ARM source to compile with THUMB.  Saves a lot of space of ARM flash memory (@slurdge)
 - Changed from using ZLIB compression of FPGA images in ARM to LZ4.  15%+ speedups when swapping between HF & LF now (@slurdge)
 - Fixed `emv scan -w` - hash mismatch, solution similar to below (@iceman1001)
 - Fixed 'emv roca' - hash mismatch, merged from official repo (@pwpiwi)
 - Fixed 'lf ti demod' - now calculates correct crc (@iceman1001)
 - Added Python3 as possible script engine (@iceman1001, @doegox)
 - Added doc/mfu_binary_format_notes.md (@iceman1001)
 - Fixed `hf mfu dump` - now reads Ulev1 3counter vs NTAG 1 counters (@iceman1001)
 - Fixed `hf mfu info` - now print correct cfg1 - nfc protection (@iceman1001)
 - Fixed `hf mfu eload` - now detects and converts between plain/old/new mfu binary format (@iceman1001)
 - Changed hitag2crack now integrated into main Makefile, tests added (@doegox)
 - Added `make check` mechanics (@doegox)
 - Changed log files moved to subfolders (@doegox)
 - Changed use system Lua library if available (@doegox)
 - Changed use system Jansson library if available (@doegox)
 - Changed use system Whereami library if available (@doegox)
 - Changed use system Zlib library if available (@doegox)
 - Fixed release version information (@doegox)

## [ice coffee.4.9237][2020-05-21]
 - Updated documentation (@doegox, @iceman1001)
 - Changed `pm3test.sh` - more regression tests to  (@doegox, @iceman1001)
 - Changed `hf 15 dump` - now supports basic json format (@iceman1001)
 - Changed Test and fix many `-Wxxxxxx` warnings [compiler trials] (@doegox)
 - Fixed COVERITY warnings [compiler trials] (@doegox, @iceman1001)
 - Fixed CPPCHECKER warnings [compiler trials] (@doegox, @iceman1001)
 - Fixed CLANG warnings [compiler trials] (@doegox)
 - Changed stricter warnings [compiler trials] (@doegox)
 - Changed remove section version_information for client (@doegox)
 - Changed Add RELEASE_NAME (@doegox)
 - Changed version info also for client (@doegox)
 - Added `make release` (@doegox)
 - Changed `trace load` - err if offline mode (@doegox)
 - Fixed `standalone mode matty_run` eloadcard (@iceman1001)
 - Added, HF 15 traces (@FlUxIUS)
 - Fixed follow naming conventions [android] (@dxl)
 - Changed work directory supported [android] (@dxl)
 - Added `lf fdx demod` - decoding of Bio-Thermo lf tag. Thanks to Rosco! (@iceman1001)
 - Fixed  32-bit time_t compilation error on [WIN64] (@grspy)
 - Changed LogTrace error msg (@doegox)
 - Changed remove inline [compiler trials] (@iceman1001)
 - Changed adding execute/home/current working directory functions to lua (<@iceman1001)
 - Changed fix max duration in LogTrace (@doegox)
 - Changed first attempt for trace log header (@iceman1001)
 - Changed pm3.sh, attempt to detect BT dongle when udevadm is not available (@doegox)
 - Changed pm3.sh, test access to /dev/ttyXXX files as prerequisite for [linux] (@doegox)
 - iclass bf: avoid slowdown (@doegox)
 - Changed pm3tests.sh, shellcheck (@doegox)
 - Changed pm3.sh, shellcheck (@doegox)
 - Changed pm3.sh, err msgs on stderr (@doegox)
 - Changed pm3.sh, textual and --list with no device, exit 1 (@doegox)
 - Changed pm3.sh, without udev: change detection routine (@doegox)
 - Changed pm3.sh, alternative if udevadm is not available [termux] (@doegox)
 - Changed `makefiles` - allow to skip pthread [termux] (@doegox)
 - Changed `script run test_t55x7` - enabled param to swap modulation to test (@iceman1001)
 - Added mf_nonce_brute from https://github.com/@iceman1001/mf_nonce_brute and merge to pm3 Makefile (@doegox)
 - Changed `lf indala demod` - more lenient size check (@iceman1001)
 - Changed clarify BUTTON macro usages (@doegox)
 - Changed fix REV macros & hitagS (@doegox)
 - Changed `hf iclass loclass` -removed printvar function (@iceman1001)
 - Changed clock can be negative  (@iceman1001)
 - Changed `data tune` (@doegox)
 - fancy timers (@doegox)
 - Changed copied from pm3 repo (@iceman1001)
 - Changed psk demodualtion, keep starting samples to easily find the zero init bit (@iceman1001)
 - Changed use machine/endian.h with osx (Jamie Fiedler)
 - Fixed 2 issues in proxendian.h (@doegox)
 - Fixed issue #729. Avoid stalling on non-rdv4 when polling for FeliCa without card in proximity (@doegox)
 - Changed `lf nexwatch demod` - lenient demod sizes (@iceman1001)
 - Changed `lf nexwath sim/clone` - now supports 88bit format direct. (@iceman1001)
 - Rewritten `standalone mode lf_em4100rwc `, added card bruteforce mode. (Dmitriy Loginoov)
 - Changed `data load` - color (@iceman1001)
 - Changed appveyor verbose (@doegox)
 - Changed `lf nexwatch demod` - now detects type, and show parity /chksum (@iceman1001)
 - Changed `lfsampling` - interruptible only when logging not yet triggered (@doegox)
 - Changed `lf keri demod` - more leanient when it comes to bits (@iceman1001)
 - Fixed proper filtering of RL markers (@doegox)
 - Changed clean deps [compiler trials] (@doegox)
 - Changed remove c99 restrictions [compiler trials] (@doegox)
 - Added tracelog file for hf mfu sniff or a ntag (@iceman1001)
 - Changed `trace list` - different alloc (@iceman1001)
 - Changed convert OLD -> MIX  (@iceman1001)
 - Changed `trace list` - adjust messages (@iceman1001)
 - Fixed a mem corruption in `trace load/list` (@doegox)
 - Changed rework banners (@doegox)
 - Changed Let's see if the prompt suffixed spaces are still needed.(@doegox)
 - Fixed colored readline prompt bug (@doegox)
 - Changed simpler ul_print_type (@doegox)
 - Fixed cmake for reveng [compiler trials] (@doegox)
 - Changed hook reveng outputs to pm3 machinery, add regression test (@doegox)
 - Changed cmake android: add backslashes for host compatibility [compiler trials](@doegox)
 - Changed Translation some comments from Chinese to English. [android] (@dxl)
 - Changed cmake android: use common cmake files for deps [android] (@doegox)
 - Fixed proper way to fix reveng getopt bug(@doegox)
 - Changed cmake deps: add prefix pm3rrg_rdv4_  [compiler trials] (@doegox)
 - Changed cmake deps: add -fPIC so they can be integrated in a pm3 shared lib [compiler trials] (@doegox)
 - Changed platform adaptation [android] (@dxl)
 - Changed don't mix ifdef and defined [android] (@dxl)
 - Fixed `trace list`- too short array for explanation (@iceman1001)
 - Changed check for sim module fw file (@iceman1001)
 - Changed rpi aling warning [compiler trials] (@iceman1001)
 - Changed more LF demodulation tests (@iceman1001)
 - Changed update whereami and avoid compiling unnneded troublesome parts [compiler trials] (@doegox)
 - Changed rework C includes in C++ [compiler trials] (@doegox)
 - Changed leanient parsing of tcp:/bt:/socket: vars (@iceman1001)
 - Changed add bluetooth group to `make accessrights` (@iceman1001)
 - Changed `Makefile` - fix PKG_CONFIG_ENV (@uli)
 - Changed `f sim` - now only fills until bigbuffer is full (@iceman1001)
 - Changed `hf mf hardnested` - don't drop field when in test mode (@doegox)
 - Changed `lf sim` - better to return err on faulty offset, thanks @iceman (@doegox)
 - Changed `lf sim` - fix offset of last packet (@doegox)
 - Changed `client Makefile` - make sure to rebuild deps when needed [compiler trials](@doegox)
 - Changed silent err if dbus absent [linux] (@doegox)
 - Changed `pm3.sh` - for direct bt devices (@doegox)
 - Changed `pm3.sh` - moved WSL/PS3.x bt native to be tested first. Since call to Win32_serialport crashes it. (@iceman1001)
 - Changed `pm3.sh` - Bt dongle, bt direct listing of serial ports on WSL and PS3.x,  now using powershell.exe since wmic is deprecated (@iceman1001)
 - Changed remove libsndfile (@doegox)
 - Changed enforce PACKED structs [compiler trials] (@iceman1001)
 - Changed adjust number of threads according to cpu (@iceman1001)
 - Changed `hitag2crack` compile flags(@doegox)
 - Changed msdsal fix bug after var de-shadowing (@doegox)
 - Changed lighter msg for loading prefs, json will anyway always tell the filename (@doegox)
 - Changed make sure colors and emoji are disabled when not on TTY (@doegox)
 - Added `pref` command. PM3 client now support user preferences saved to a json file. (@mwalker33)
 - Added native support for Bluetooth in client [linux] (@doegox)
 - Added `LF_EM4100RSWB` (based on `LF_EM4100RWC`) standalone mode to read/sim/clone/brute EM4100 cards (@Monster1024)
 - Added `HF_MSDSAL` standalone mode which read and emulate Visa EMV cards (@Netxing)
 - Added `hf mfu tear` - Mifare Ultralight tear off experiment (@cintainfinitam, @fukmar)
 - Added Mifare Desfire Read/Write/Create files/records/values functionality and several fixes to `hf mfdes` (@bkerler)
 - Added CreateStdFile command to Mifare `hf mfdes` (@bkerler)
 - Rework des/3des/3k3des/aes auth. Port to mbedtls crypto library on device (@bkerler)
 - Port `hf mfdes` Auntentication to CommandNG structure, fix auth session key (@bkerler)
 - Updates `hf mfdes` functions, improved logging and added new commands (@bkerler)
 - Updated 'legic.lua' and 'legic_clone.lua' script - works with current command set (@Pizza_4u)
 - Rewrote `hf mfdes` functions and added apdu debugging (@bkerler)
 - Added Mifare Desfire GetDFNames and improve HF MFDES Enum output (@bkerler)
 - Fixed Mifare Desfire select appid handling (@bkerler)
 - Improved `hf 14a info` - card detection handling (@bkerler)
 - Updated helptext layout in all luascripts (@iceman1001)
 - Changed `hf mfdes info` - output and logging (@bkerler)
 - Updated texts in `hf legic` commands (@ikarus23)
 - Fixed timing bug inside 40x5 (@mwalker33)
 - Refactored all Hitag2 attacks (@doegox)
 - Added two new Hitag2 attacks (@doegox)
 - Changed `hf iclass decrypt` - limit amount of data to read if wrong app limit (@iceman1001)
 - Changed `hf search` - now continue to search in case of dual tech cards (@iceman1001) Thanks to @ikarus23 for the suggestion!
 - Added `hf topas info` - old reader command,  now also prints NDEF (@iceman1001)
 - Changed `hf topaz reader` - now only prints lighter info, like UID. (@iceman1001)
 - Changed `hf iclass readtagfile` - improved param handling (@iceman1001)
 - Changed `hf mfu ndef` - text record now correctly decode language code and test (@iceman1001) Thanks to @bettse!
 - Updated the helptexts for `hf iclass` commands (@iceman1001)
 - Fixed NDEF TLV lock / memory parsed correct (@merlokk)
 - Changed `mem spiffs` commands now parse h param correct (@iceman1001)
 - Added colour to `hf mf chk` and `hf mf fchk` (@dunderhay)
 - Changed - updated public keys info (@bkerler)
 - Changed - mfc default keys removed dublicates (@iceman1001)
 . Change - mfc default keys got new entries found on TagInfo app (@bkerler)
 - Added `hf mfu ndef` - read and decode NDEF Type2 messages (@iceman1001)
 - Added `data ndef` - decode NDEF messages (@iceman1001)
 - Changed - usb write's now waits for shiftregisters (@pwpiwi)
 - Changed - NDEF supports more signatures now (@iceman1001)
 - OSX Makefile now supports `make style` and `make checks` (@Pizza_4u)
 - Added `HF_LEGIC` standalone mode to read and simulate a Legic prime tag (@Pizza_4u)
 - Added keri MS decode/encode and update `lf keri clone` to support MS fc/cid cloning. (@mwalker33)
 - Fixed `hf mfdes enum` - now actually manages to enumerate files under all AID's. :smiley:  (@iceman1001)
 - Fixed `hf mfdes info` - now detects DESFire light and work properly Wrapped commands :+1: (@iceman1001)
 - :smiling_imp: support (@doegox)
 - Additional colour changes as recommended by @iceman (@dunderhay)
 - Changed type colour for `hf 14a` card types (@dunderhay)
 - Added colour to `hf mfdes` command (@dunderhay)
 - Added `HINTS` command.  Will turn off / on hint messages. Default mode is OFF.  (@iceman1001)
 - Added colour to `hf 14a` and `hf mfu` commands (@dunderhay)
 - Added colour to `lf hid` commands (@dunderhay)
 - Changed `script run hf_bruteforce -s start_id -e end_id -t timeout -x mifare_card_type` - The hf_bruteforce card script now requires Mifare type (mfc or mfu) (@dunderhay)
 - Updated `hf_bruteforce.lua` script - added support for brute forcing Mifare Ultralight EV1 cards (@dunderhay)
 - Added `hf mf personlize` - personalize the UID of a Mifare Classic EV1 card (@pwpiwi)
 - Changed - hint texts added to all lf clone commands (@iceman1001)
 - Changed `lf keri demod` - adjusted the internal id. (@mwalker33)
 - Added seamless integration with cryptohelper (@iceman1001)
 - Changed `lf hid brute` - new params for direction (UP/DOWN); textual and main loop actually exit. (@capnkrunchy and @iceman1001)
 - Fixed `lf hid brute` - made it work again (@capnkrunchy)
 - Fixed standalone mode HF_MATTYRUN - correct logic when all keys found in printing. partial fix  (@iceman1001)
 - Changed static nonce detection got tighter (@iceman1001)
 - Improved termux notes (@msoose)
 - Fixed `hf mf autopwn` - works on debian 10 *nix. Bad exit commands in hardnested (@iceman1001)
 - Fixed `hf mf hardnested` - bad mutex strategies (@msoose)
 - Changed `lf hitag` - now obeys `lf config` (@iceman1001)
 - Added all python 2 scripts to python 3 (@doegox and @sigwinch28)
 - Removed undefined exit behaviour from `analyzesize` tool: it now exits with code 2 when called with wrong args (@sigwinch28)
 - Replaced shebangs in scripts with more portable versions which use `/usr/bin/env` (@sigwinch28)
 - Added `hf lto restore` - restore LTO cartridge memory from dump file [.bin|.eml] (@Kevin-Nakamoto)
 - Added `LF_ICEHID` standalone mode which searches for lf HID credentials and store to RDV4 flashmem (@iceman1001)
 - Added `HF_14ASNIFF` standalone mode with storing trace to RDV4 flashmem (@micolous)
 - Added `hf lto dump` - dump 8160 bytes of data from LTO cartridge memory and save to file (@Kevin-Nakamoto)
 - Changed `data plot` - write serial port name in window title for plot / slider window (@iceman1001)
 - Added `hf lto wrbl` - write block support for LTO Cartridge memory (@Kevin-Nakamoto)
 - Fixed compilation under openSUSE (@hsanjuan)
 - Added `lf nexwatch sim` - use raw hex to simulate (@iceman1001)
 - Fixed `lf indala read` - long id 224bits preamble identification less strict (@iceman1001)
 - Added `hf mf staticnested` - useful when targeting the strange cards with a static nonce. (@iceman1001) Thanks to @xtigmh @uzlonewolf for their solutions.
 - Added `hf plot` (@pwpiwi)
 - Fixed `lf config` - when called with no params, it no longer mess up all device lf config settings. (@iceman1001)
 - Changed `lf indala clone` - new option `--Q5` writes to q5/T5555 tags. (@iceman1001)
 - Changed `lf indala clone` - new option `-c <card id>` allows cloning INDALA tag from Heden2L/Hedengren ID format (@iceman1001) - Thanks to @randomdude42 for solution
 - Changed `lf indala demod` - added decode Heden2L/Hedengren format (@iceman1001) - Thanks to @randomdude42 for solution
 - Added `commands.md` - document with all proxmark client commands. Generated with XX_internal_command_dump_markdown_XX. (@iceman1001)
 - Changed `lf pac clone` - new option `c <card id>` to allow cloning PAC/Stanley tag from card ID (@danshuk)
 - Changed `lf pac read` - decoded PAC/Stanley card ID (@danshuk)
 - Changed mifare classic keytable output refactored and uses colors (@iceman1001)
 - Fixed `hf mf nested` - now writes the correct blockno (@iceman1001)
 - Changed `lf t55xx dump` - now supports saving to JSON (@iceman1001)
 - Changed `hf mf chk | fchk`  faster authentication by lower timeout limit. (@pwpiwi)
 - Changed `hf mf hardnested` - now detects "static / fixed" nonce tags and exits (@iceman1001)
 - Changed `hf mf csave` - now uses UID in filename (@iceman1001)
 - Fixed `hf mf chk` - read block B logical error. #489 (@iceman1001)
 - Added `hf mf ekeyprn d` - new parameter to save keys to file (@iceman1001)
 - Added `mfu_magic.lua` - script to interact with your magic NTAG tag. (@iceman1001)
 - Added `hf_bruteforce.lua` - script bruteforces uid for 14a using hf 14a sim (@keld)
 - Added `tools\pm3_cs8.pl` - convert .pm3 files to wave format (@samyk)
 - Added `tools\pm3_amii_bin2eml.pl` - amiibo convering script (@samyk)
 - Changed `hf 14a info` - now detects "static / fixed" nonce tags (@iceman1001)
 - Changed `data save f <filename> w` - params reworked.  (@iceman1001)
 - Changed `hf search` - now should detect LTO-CM tags (@iceman1001)
 - Added `hf lto info` - skeleton support for LTO Cartridge memory (@iceman1001)
 - Added saving as wave format (@anon)
 - Added ISO15_WRITE annotation (@pwipiw)
 - Added `lto_dump.lua` @kevin
 - Added `hf mf cwipe` magic chinese card (gen1a) wipe to default state (@merlokk)
 - Added `pm3_mf7b_wipe.py` python script. Wipes magic S70 7B Gen2 card. (@vulnersCom)
 - Added `hf mfp chk` Mifare plus command for check keys from public keys list, from dictionary or 1 and 2-byte bruteforce (@merlokk)
 - Changed `hf 15` - some refactoring (@grspy)
 - Added `hf 15 writeafi` and `hf 15 writedsfid` (@grspy)
 - Added detailed info for SLIX2 tags in `hf 15 info` (@grspy)
 - Fixed `hf list felica` and `hf felica sniff` (@7homasSutter)
 - Added `hf felica wrunencrypted` (@7homasSutter)
 - Added `hf felica rdunencrypted` (@7homasSutter)
 - Added `hf felica rqresponse` (@7homasSutter)
 - Added `hf felica rqservice` (@7homasSutter)
 - Added polling for felica standard (@7homasSutter)
 - Added `lf t55xx dump save` and `lf t55xx restore` for .bin and .eml files (@mwalker33)
 - Added `lf t55xx detected` to try without password first (@mwalker33)
 - Changed `lf indala read` - added indala 26bit decoding (@martinbeier)
 - Changed `lf t55xx detect` to try without password first (@mwalker33)
 - Display high bit for detected Kastle HIDs to allow `lf hid clone [id]` to work properly (@swg0101)
 - Added option `-n` to scripts pm3* (@doegox)
 - Added `wiegand list/encode/decode` - wiegand format manipulation. Adapted to fit here. (@grauerfuchs)
 - Added support for color text on windows 10 (@mwalker33)
 - Added `s` <samples to skip> to `lf config` / `lf sniff` to skip samples when sniffing based on same option in Proxmark/proxmark3 by @marshmellow42. (@mwalker33)
 - Added save to .eml and .bin for `em 4x05_dump` (@mwalker33)
 - Added option `-n` to scripts pm3* (@doegox)
 - Fixed `em 4x05_dump` to print all blocks read (@mwalker33)
 - Fixed T55xx config getting displayed when using password when no password needed on read. (@mwalker33)
 - Added T55x7 downlink mode auto usage via mode detected (lf t55 detect) (@mwalker33)
 - Added `lf t55xx protect` - sets password and enables password protection on t55x7 tag (@iceman1001)
 - Changed `lf t55xx wipe` - now accepts user provided configuration block (@iceman1001)
 - Added T55x7 downlink mode support r <mode> 0 Default, 1 Long Leading 0, 2 Leading 0, 3 1 of 4 and 4 (in some commands) try all. (@mwalker33)
 - Changed proxmark3-flasher is now merged into proxmark3 client. Add pm3-flash (@doegox)
 - Changed `hf iclass clone\dump\rdbl\wrbl` - now uses NG (@iceman1001)
 - Fixed `hf iclass clone` - last block always fails (@iceman1001)
 - Changed `hf iclass clone` - retries ten times, less output (honor verbose)  (@iceman1001)
 - Changed `hf iclass dump` - retries ten times, less output (honor verbose)  (@iceman1001)
 - Rename `hf iclass writeblk` ->  `hf iclass wrbl` to match hf mf wrbl (@iceman1001)
 - Rename `hf iclass readblk` ->  `hf iclass rdbl`  to match hf mf rdbl (@iceman1001)
 - Added cmdscript example and show usage with shebang (@doegox)
 - Added instructions for Fedora (@doegox)
 - Changed reduce the list of requirements to the minimum and move to QT5 (@doegox)
 - Added `make install` and reorganize/rename stuffs accordingly (@doegox)
 - Added searchFile for several types of files (@doegox / @iceman1001)
 - Changed posix sh version of mkversion (@doegox)
 - Changed remove entirely ncurses, not needed nowadays (@doegox)
 - Changed remove deprecated termcap, use ncurses instead (@ZeroChaos-)
 - Changed `hf iclass encrypt` - now takes transport key as param. (@iceman1001)
 - Changed `hf iclass decrypt` - now takes transport key as param. (@iceman1001)
 - Changed `hf mf fchk m` - now secretly dumps card to emul, if all keys are found (@iceman1001)
 - Changed history and logfile are now saved into $HOME/.proxmark3/ (@doegox)
 - Changed optimization of iclass mac calculations on deviceside (@pwpiwi)
 - Added `hf mf autopwn` - Autopwn function for Mifare Classic, extract all keys and dump card memory (@matthiaskonrath)
 - Added Lua paths: look for scripts also in ~/.proxmark/lua{scripts,libs} and /usr/local/share/proxmark3/lua{scripts,libs} (@doegox)
 - Changed Lua directory scripts/ to luascript/ (@doegox)
 - Changed non-rdv4 PLATFORM must now use the generic PM3OTHER, simpler (@doegox)
 - Fixed reveng integration for all platforms else than WIN32 (@doegox)
 - Added cheat sheet for easy operations of the Proxmark3 (scund00r)
 - Chg commands are now in green in the helptext list (@iceman1001)
 - Fixed `script run ndefdump` - better exit messages when failing (@iceman1001)
 - Fixed `hf iclass dump` - now also saves in EML format (@iceman1001)
 - Fixed `hf iclass sim 3` - now works on legacy readers and legacy SE readers (@iceman1001)
 - Rework hitag2 read/write help (@ViRb3)
 - Added `lf nedap` - encoding / decoding  (anon)
 - Added client option `-i` to stay in interactive mode after a script or command (@DidierStevens/@doegox)
 - Added VSCode tasks (@ViRb3)
 - Better warn user of hardcoded hitag info (@ViRb3)
 - Format and docs hitag (@ViRb3)
 - Fixed hitag password write offset by 1 (@ViRb3)
 - Fixed momentarily flash read/write of dicts (@doegox/@cjbrigato)
 - Added some more default keys (@anon)
 - Added `hf thinfilm sim` simulating Thinfilm NFC barcode tags (@doegox)
 - Added `hf thinfilm list` specific trace decoding (Thinfilm NFC barcode tags) (@doegox)
 - Fixed `hf topaz reader` - don't crash when trying to read a Thinfilm tag (@iceman1001)
 - Added `hf thinfilm info` - read / decode Kovio Thinfilm NFC barcode tags (@iceman1001)
 - Added FPGA LF adc path (@anon)
 - Added ECC support / check for NID_secp128r1 (@pwpiwi)
 - Added some more default keys (ollibolli)
 - Fixed T55x7 Downlink timings backward compatible (@mwalker33)
 - Added proper Makefile halting when using incompatible STANDALONE and PLATFORM vars (@doegox)
 - Added T55x7 Downlink mode support (@mwalker33)
 - Added SPIFFS Flash filesystem support (@cjbrigato)
 - Fixed support for flashing 512K units with old bootrom (@slurdge/@doegox)
 - Fixed `hf mf sim` - wrong access rights to write key B in trailer (@McEloff)
 - Added option -i to flasher to query Pm3 for its memory size (@doegox)
 - Added support for flashing 512K units (@slurdge)
 - Added a simple python tool to check the elf sizes (@slurdge)
 - Changed new keys for Vigik badges in default_keys.dict (@luminouw)
 - Added `hw standalone` to jump to standalone mode from command line or script (@doegox)
 - Added to `hf 14a apdu` print apdu and compose apdu (@merlokk)
 - Changed buggy `mem read` removed, `mem save` renamed `mem dump`, can now display too (@doegox)
 - Fixed timeout for mem wipe was too short, thanks @cjbrigato (@doegox)
 - Fixed `hf mf sim` - Mifare Classic simulation more flexible anti-collision check (@McEloff)
 - Changed `hf mf sim` - Mifare Classic simulation not respond NACK on invalid authentication request (@McEloff)
 - Changed `read_pwd_mem.lua` now handles Mifare Classic dictionaries large than 4096 bytes (@iceman1001)
 - Changed Do not clear trace log during `hf mf chk`, to save whole process history (@McEloff)
 - Added `msleep` command, for pauses in scripts (@doegox)
 - Added support for WSL in proxmark.sh (@doegox)
 - Added documentation for usage of Proxmark3 under WSL (@doegox)
 - Changed replace aes.c with mbedtls version (@slurdge)
 - Changed replace ukbhit by kbd_enter_pressed, not requiring tcgetattr (@xianglin1998/@doegox)
 - Added config for RaspberryPi in JTAG tools (@doegox)
 - Added config for FTDI C232HM-DDHSL-0 in JTAG tools (@doegox)
 - Fixed compilation under MacOSX with binutils (@matrix)
 - Added dynamic report of the chipID for flashing purposes (@slurdge)
 - Fixed Clang warnings (@matrix)
 - Fixed EMVGPO bug (@matrix)
 - Added hitag2 write password auth (@ViRb3)
 - Added check if bootloader segment is within bounds (@slurdge)
 - Added `hf 15 csetuid` - set UID on ISO-15693 Magic tags (@t0m4-null)
 - Changed Print help if unknown arg for hitag reader/writer (@ViRb3)
 - Fixed clock deadlock in hitag sniff (@ViRb3)
 - Added compiler info in client & ARM sections (@slurdge)
 - Added support for automatic COM detection on Windows (@slurdge)
 - Added support for compilation on RaspberryPiZero (armv6) (@doegox)
 - Changed updates to README (@iceman1001)
 - Changed `hf mf/mfu dbg` => `hw dbg` (@doegox)
 - Changed replace usb_poll_validate_length() by data_available() that supports USART too (@doegox)
 - Make sure standalone modes can be launched when connected on USB without client (@doegox)
 - Changed cleaner makefile execution, use `make V=1` if you want to see full lines (@doegox)
 - Changed automate make clean when platform definitions are changed (@doegox)
 - Added STANDALONE option to Makefile.hal (@Fl0-0)
 - Changed mem info - production public key to verify rdv4.0 flash  signature (@iceman1001)
 - Fixed specify that we need TCP and not UDP connection (@phcoder)
 - Changed lf cotag demod - adjusted error trigger (@iceman1001)
 - Added documentation on BT add-on (@iceman1001/@doegox)
 - Changed new button behaviour in bootloader mode, no need to keep it pressed, press again to interrupt (@doegox)
 - Changed new keys in dicts, new mem layout to accommodate them (@iceman1001/various)
 - Fixed lf sim -  if called with empty graphbuffer all strange things happen, like turning on HF field (@iceman1001)
 - Changed hf 14a sim / hf mf sim - check buttonpress/usb frame fewer times in order not to disrupt simulation (@McEloff)
 - Changed data convertbitstream - converts bit to max/min in order to facilitate demodulation of simulation data (@iceman1001)
 - Changed lf em 410x_demod - now can demod the simulation data (@iceman1001)
 - Added HC-06 scripts for BT add-on (@doegox)
 - Fixed lf nedap sim - error when adding parity (@iceman1001)
 - Added documentation on UART and baudrates (@doegox/@iceman1001)
 - Changed prompt now shows which channel is used (@iceman1001)
 - Changed USART baudrates computation, up to 6Mbps (@iceman1001/@doegox)
 - Changed hf mf nack - keep sync if started without card over antenna
 - Added usart btfactory - to reset a misconfigured BT add-on (@doegox)
 - Changed hw status - now prints number of dictionary keys loaded (@iceman1001)
 - Added home, end, pageup, and pagedown keybinds to the plot GUI. Also fix paged movement in GUI. (@mcd1992)
 - Changed legic.lua saves data in EML and BIN formats (@iceman1001)
 - Changed hf tune - is now synchronous (for BT add-on) and can be interrupted by kbd (@doegox)
 - Changed update macOS install instruction (@ Uli Heilmeier)
 - Added trace output in hexdump format for Wireshark import (@ Uli Heilmeier)
 - Added usart btpin - to change BT add-on PIN (@doegox)
 - Added reconnection support (@iceman1001/@doegox)
 - Added usart tx/rx/... - USART developer commands (@doegox)
 - Added PLATFORM_EXTRAS, WITH_FPC_USART_HOST, BTADDON Makefile configuration (@doegox)
 - Fixed slow reconfigure on mingw of serial port (@iceman1001)
 - Fixed cross thread communictions of timeout variable (@iceman1001)
 - Changed client is now "universal", adapting to Proxmark3 capabilities (@doegox)
 - Added disconnect support to Lua (@iceman1001)
 - Changed handles FPC/FLASH FW more gracefully on non-RDV4 pm3 (@doegox)
 - Added JTAG support for Shikra (@NinjaStyle82)
 - Changed smart color handling: only if linux and on real term (@doegox)
 - Changed reconfigure uart timeouts when compiled for FPC and connecting over USB (@iceman1001)
 - Changed fast push for many commands (@iceman1001/@doegox)
 - Added: fast push for Lua (@iceman1001)
 - Added NDEF parser in Lua (@iceman1001)
 - Changed improve NDEF parser (@iceman1001)
 - Changed all commands got migrated to MIX/NG packet format (@iceman1001/@doegox)
 - Fixed Mifare Ultralight read block missing bytes (@doegox)
 - Added support new frame format in all Lua scripts (@iceman1001)
 - Added CMD_CAPABILITIES for pm3 to inform dynamically the client (@doegox)
 - Changed baudrate handling, make it clear it is only indicative for USB-CDC & BT (@doegox)
 - Changed new progressive light scheme for `hw detectreader` (@doegox)
 - Added common error definitions system for retvals (@doegox)
 - Changed USART RX & TX code and fix delays handling to make it more robust, especially over BT (@doegox)
 - Added support for new frames format, speedup & huge changes, see doc/new_frame_format.txt (@doegox)
 - Changed loadFile* & saveFile* accept filenames with (or still without) extension (@doegox)
 - Fixed LoadEML to accept final "\n", e.g. from pm3_mfd2eml.py (@doegox)
 - Changed rework shell scripts for easy client or flasher (@doegox)
 - Fixed stop poking Internet when compiling (@doegox)
 - Added support for multiple commands to "-c", e.g. proxmark3 -c "hw ping;hw version" (@doegox)
 - Fixed external flash writing bitflips issues at 24MHz (@doegox)
 - Added color support to Dbprintf & alike and rework Dbprintf flags (@doegox)
 - Changed archive (and fix) hid-flasher (@doegox)
 - Added standalone placeholder to simplify new standalone integration (@doegox)
 - Changed refactor standalone mode info string (@iceman1001)
 - Added iceman skeleton standalone mode for ppl to use as base for their new modes (@iceman1001)
 - Changed move compilation options to Makefile.hal (@doegox)
 - Fixed compilation under OSX (@iceman1001)
 - Added openocd config files for JLink (@doegox)
 - Fixed compilation dependencies for recovery (@doegox)
 - Fixed segfault when loading a file (@doegox)
 - Change/Add new dump format for Ultralight/NTAG, counters support, simulation (@mceloff)
 - Added `hf mf sim` full-byte split anticollision support (@mceloff)
 - Fix/Add `hf mf sim` bugs fix, RATS support, etc (@mceloff)
 - Fixed serial of FPC. (@ryan)
 - Fixed `data shiftgraphzero` corrupting end of GraphBuffer (@doegox)
 - Fixed `hf legic info` - unsegmented card now uses card size to calc remaining length (@iceman1001)
 - Added 36bit HID format, extend calcWiegand() to include oem bits (@davidbeauchamp)
 - Fixed `hf mf hardnested` - not verify key when reading nonce file (@iceman1001)
 - Changed optimizations for ask/bi (@iceman1001)
 - Fixed `hf mf sim` - bugs fix, refactoring (@mceloff)
 - Added WRITE and COMPATIBLE_WRITE support to Mifare Ultralight/NTAG simulation (@mceloff)
 - Changed installation instructions and add video links (@5w0rdfish)
 - Changed `hf mf sim` to support more types (@vratiskol)
 - Changed better strong wave detection for biphase (@iceman1001)
 - Added `script run test_t55x7` (@iceman1001)
 - Added new lua scripting support for some t55xx commands (@iceman1001)
 - Added FPC USART for BT add-on with pm3 client. (@doegox)
 - Added `-b baudrate` option to the pm3 client. (@doegox)
 - Changed `lf t55xx info`: tell if known configuration block0. (@iceman1001)
 - Fix/Add FPC usart: fix TX, bring RX, full speed. (@doegox)
 - Changed `lf t55xx config` options: allow to toggle on/off i/q5/st
 - Changed `lf t55xx info`: support offline block0, Q5, fix extended, add warns. (@doegox)
 - Avoid race condition when flasher finds the not yet closed pm3 port. (@doegox)
 - Fixed `lf t55xx trace`: read the proper block. (@doegox)
 - Fixed Indala 64 on T55xx: use PSK1. (@doegox)
 - Force proper Linefeed (LF) handling in ProxSpace. (@vratiskol)
 - Fixed Makefiles race conditions to allow parallel compilation, e.g. `make -j8`. (@doegox)
 - Added - dictionary key file for MFU. (not in use at the moment) (@mazodude)
 - Changed `lf fdx demod` - better biphase maxerrors. (@MalteHillmann)
 - Changed `hf mf sim` - now works better against android (@mceloff)
 - Fixed `lf t55xx brute` - now works after aquiredata adaptations (@iceman1001)
 - Fixed `lf t55xx chk` - now works after aquiredata adaptations (@iceman1001)
 - Fixed `lf t55xx recoverpwd` - now works after aquiredata adaptations (@iceman1001)
 - Fixed `data detect p` - reverted bad clock detection (@iceman1001)
 - Changed `data detect a` - better clock detection (@iceman1001)
 - Added `hf 14a info` - now detects some magic card Gen2 (@iceman1001)
 - Removed `LCD` code in armsrc compilation (@iceman1001)
 - Changed - Generic fixes of codestyle (@doegox) (@iceman1001)
 - Changed `lf indala demod` - refactoring (@iceman1001)
 - Changed - handling fault bit markers (7) and partial nibbles in hex printing (@doegox)
 - Changed - printing of fault bit markers (7) using a dot (@doegox)
 - Changed `sc upgrade` - firmware file integrity check (@pwpiwi)
 - Fixed `data rawdemod am` - last bit was missing (@doegox)
 - Fixed `hf 15 dump f` - also selects tag first (@iceman1001)
 - Fixed `hf iclass clone` - missing fileclose (@iceman1001)
 - Added `trace list hitag` - old hitag annotations now use the new trace (@iceman1001)
 - Changed `lf hitag sim` - loads bin/eml/json (@iceman1001)
 - Changed `lf hitag reader 21` - saves in bin/eml/json (@iceman1001)
 - Changed `lf hitag` - refactoring (@iceman1001)
 - Changed `lf hitag` - refactoring (@pwpiwi)
 - Fixed `lf hitag` - generic fix for missing clock init (@pwpiwi)
 - Fixed fsk sim operations on deviceside - avoid division by zero (@doegox)
 - Fixed `hf mf fchk` - condition always false (@doegox)
 - Fixed `lf t55xx recoverpw` - shift as u32 (@doegox)
 - Fixed `lf ti demod` - shift as u32 (@doegox)
 - Fixed `lf ti read` - shift as u32 (@doegox)
 - Fixed `lf t55xx chk` - condition always false (@doegox)
 - Changed `lf sim` - ledcontrol refactoring (@doegox)
 - Fixed `hf mf nack` - signedness bug (@doegox)
 - Fixed `hf epa cnonce` - check return value (@doegox)
 - Fixed `lf hitag write` - condition always true (@doegox)
 - Fixed `mem write` - added extra check (@doegox)
 - Fixed `iso15693` - bad string cpy (@doegox)
 - Fixed `make style` - EOF LF support (@doegox)
 - Added `hf 14b raw` - added -t for timeout  (@iceman1001)
 - Rename `lf hitag snoop` - renamed to `lf hitag sniff` (@iceman1001)
 - Rename `lf snoop` - renamed to `lf sniff` (@iceman1001)
 - Rename `hf snoop` - renamed to `hf sniff` (@iceman1001)
 - Fixed `hf mfp wrbl` - more blocks available (@merlokk)
 - Added `make platform` - compile for non-rdv4 devices made simpler (@doegox)
 - Changed Makefiles optimizations when recompiling (@doegox)
 - Fixed `data load` - loads TITEST.txt again (@iceman1001)
 - Changed `lf search` - now detects TI (@iceman1001)
 - Changed fixing signal cleaning for LF (@doegox)
 - Fixed `lf paradox demod` - wrong check (@iceman1001)
 - Changed `lf t55xx` - aquiredata uses getsamples (@iceman1001)
 - Fixed `lf search` - chipset detection restore demod buffer again (@iceman1001)
 - Added `make style` (@doegox)
 - Fixed mixed tabs vs spaces. Now only use 4 space as tab. (@doegox)
 - Fixed `lf visa2000 read` - too few samples (@iceman1001)
 - Fixed `lf t55xx bruteforce` - infinity loop (@doegox)
 - Fixed `analyse nuid` - correct crc (@doegox)
 - Added command history not repeating logged commands (@doegox)
 - Fixed path for aidjson (@doegox)
 - Fixed missing init i2x (@doegox)
 - Fixed `14b select card` - (@doegox)
 - Added `hf mf ndef` - parsing of NDEF messages (@merlokk)
 - Added `hf mf mad` - parsing of Mifare Application Directory (@merlokk)
 - Rename `lf snoop` -> `lf sniff` (@iceman1001)
 - Rename `hf snoop` -> `hf sniff` (@iceman1001)
 - Changed generally added more colors (@iceman1001)
 - Changed `sc upgrade` updated firmware v3.11 (RDV40) (@sentiprox)
 - Changed `data autocorrelate` - better visual representation and added extra peak detection (@iceman1001)
 - Fixed `lf search` - false positive indala identification fixed (@iceman1001)
 - Added `lf keri` - basic support for Keri tags (@iceman1001)
 - Added `hf mf list` - re-added it again (@iceman1001)
 - Fixed - A lot of bugfixes, like memory leaks (@iceman1001)
 - Changed `hf 14a antifuzz` - original implementation (@asfabw),  reworked a bit
 - Fixed `hf mf fchk` (@iceman1001)
 - Fixed `usb slow on posix based systems` (@fl0-0)
 - Changed `lf pcf7931` - improved read code (@sguerrini97)
 - Changed `hf felica list` - started with some FeliCa annotations (@iceman1001)
 - Fixed `hf tune` - now works as expected (@iceman1001)
 - Added `option to use flash memory to upload dictionary files` (RDV40) (@iceman1001)
 - Fixed `printing percentage now standard compliant` (@fabled)
 - Added `emv roca` - command to test for ROCA vuln in public RSA modulus (@merlokk / @iceman1001)
 - Added TCP ports support (on linux) (@phcoder)
 - Added HF sniff standalone mode with optional storing of ULC/NTAG/ULEV1 authentication attempts (@bogiton)
 - Fixed `Lining up plot and control window` (@anticat)
 - Fixed `annoying focus behaviour` on OSX  (@Anticat)
 - Implemented AppNap API, fixing #283 and #627 OSX USB comm issues (@AntiCat)
 - Added `sc brute` - a naive SFI bruteforcer for contact smartcards (RDV40) (@iceman1001)
 - Changed `lf t55xx detectconfig` - now optional to persist settings to flashmem (RDV40) (@iceman1001)
 - Changed `hf mf csave` - now saves both EML/BIN formats (@iceman1001)
 - Changed `hf mf esave` - now saves both EML/BIN formats (@iceman1001)
 - Fixed `compiler warning on macos and gcc7.1 or higher`  (@TomHarkness)
 - Fixed `crash on Bionic libc if CloseProxmark is called twice` (@micolous)
 - Changed `lf hid` - got an  updated to Kastle format (@xilni)
 - Added `lf t55xx deviceconfig` - enables custom t55xx timing settings. (RDV40) (@iceman1001)
 - Chg adaptations for FPC communications (work in progress)  (@iceman1001)
 - Fixed `stand-alone Colin` - remake to benefit from flashmem for persistence. (@cjbrigato)
 - Fixed `LEGIC SIM` - remake of legic sim (@drandreas)
 - Changed `proxmark3 client threading`  - remake from official repo (@micolous)
 - Added `rem` - new command that adds a line to the log file (@didierStevens)
 - Fixed `EM410xdemod  empty tag id in lfops.c` (@Defensor7)
 - Fixed `usb device descriptor` - some android phones will enumerate better when iSerialnumber is not a multiple of 8 (@micolous, @megabug)
 - Fixed `StandaloneMode LF` -  when collecting signal, justNoise detection is needed (@didierStevens, @Megabug)
 - Fixed `StandAloneMode Colin` - mifare1ksim called with right params (@cjbrigato)
 - Improved `install.sh` to install dependencies for Ubuntu 18.04 and using max number of processors during compilation (@joanbono)
 - Modified `install.sh` script to work in macOS and Linux + added the `update.sh` and `proxmark3.sh` from joanbono (@TomHarkness)
 - Fixed `hf emv` - some cards need to have Le=0x00, some do not need to have (@merlokk)
 - Fixed `hf legic`  enhancement of rx / tx in legic commands (@drandreas)
 - Fixed `data buffclear` - now frees bigbuff also (@iceman1001)
 - Fixed GET_TICKS  and signess while shifting (@drandreas)
 - Added `hf 14b dump` - now dumps to file (bin & eml)  (@iceman1001)
 - Fixed fixed xcorrelation for strong signal (@drandreas)
 - Fixed `hf mf chk` - keytype was reversed (@TomHarkness)
 - Added strange vid/pid found in wild.  Could be pm3 easy clones. (@iceman1001)
 - Fixed `make udev`  -  udev filename could be in conflict,  renamed.  (@blshkv)
 - Fixed `lf t55xx config`  - wrong sized array disabled FSK1a/FSK2a as options (@grauerfuchs)
 - Added more default keys  (@j8048188) (@iceman1001)
 - Added `sc list/info/raw/reader/upgrade` - (RDV40) smart card module functionality  (@iceman1001)
 - Fixed `download eml buffer` (@drandreas)
 - Changed `exclusion of floatingpoint lib` (@pwpiwi)
 - Changed `lua scripts bit32 calls` (@iceman1001)
 - Changed `hw version`  (@pwpiwi),   adapted to iceman fork (@iceman1001)
 - Added `amiibo functionality`  (@jamchamb),   adapted to iceman fork (@iceman1001)
 - Fixed `hf legic`  (RDV40) adaptations to FPGA HF enhanched reading distance (@iceman1001)   Thanks to @drandreas!
 - Added `script run mifare_acces` - script to decode Mifare classic accessbits (@Neuromancer)
 - Added `mem load/save/wipe` - commands to upload / download to new RDV40 onboard flashmemory (@iceman1001)
 - Added `script run mifareplus` - script to communicate with a mifare plus tag (@dceliano)
 - Added FlashMemory functionality  (RDV40)  (@willok)
 - Fixed `hf mfu dump` - partial reads lead to corrupt data (Thanks @elafargue for pointing it out)
 - Changed `hf mfu dump / read`  - now retries five times. (@jamchamb)
 - Added `hf list mf` - deciphers crypto1 stream and works with first authentication and weak nested authentications (@Merlok)
 - Adjusted `lf cmdread` to respond to client when complete and the client will then automatically call `data samples` (@marshmellow42)
 - Added a bitbang mode to `lf cmdread` if delay is 0 the cmd bits turn off and on the antenna with 0 and 1 respectively (@marshmellow42)
 - dump / restore now uses custom filenames (@brianpow)
 - Removed `hf mf sniff` ,  (@iceman1001),  use HF 14A SNIFF instead
 - Added `hf iclass lookup` (@iceman1001)
 - Added `hf iclass chk` (@iceman1001)
 - Fixed ADC mux all closed push-pull state (@iceman1001)
 - Fixed `hf mf darkside` - speed fixes (@pwpiwi)
 - Fixed `hw tune`  -  now compensates for 3% error in output,  also measure full 140v using ADC channel 5 and 7.  (@iceman1001)
 - Updated loclass gpl license (@holiman)
 - Fixed Antenna on after changed FPGA Mode. (@iceman1001)
 - Added `hf mf nack` - Mifare NACK bug detection (@iceman1001) (@doegox)
 - Fixed `hf mf mifare` - zero parity works, no more double runs for normal darkside (@iceman1001)
 - Added `hf mf fchk` - the fastest check keys implementation tothisday (@iceman1001)
 - Fixed `hf iclass` - more stable demod (@iceman1001)
 - Added `hf iclass chk`  - check keys from default_iclass_keys.dic file (@iceman1001)
 - Fixed `hf 15 dump` - no more crc faults (@iceman1001)
 - Fixed `hf 15 read` - no more crc faults (@iceman1001)
 - Fixed `hf 15 readmulti` - no more crc faults (@iceman1001)
 - Changed proxmark command line parameter `flush` to `-f` or `-flush` (@merlokk)
 - Added to proxmark command line parameters `w` - wait 20s for serial port (@merlokk)
 - Added to proxmark command line parameters `c` and `l` - execute command and lua script from command line (@merlokk)
 - Added to proxmark ability to execute commands from stdin (pipe) (@merlokk)
 - Added new standalone mode "HF Mifare ultra fast sniff/sim/clone - aka VIGIKPWN"  (@cjbrigato)
 - Added to `hf 14a apdu` - exchange apdu via iso14443-4 (@merlokk)
 - Added to `hf 14a apdu` - apdu and tlv results parser (@merlokk)
 - Added `hf emv` commands  (@merlokk)
 - lots of bug fixes (many many)
 - Changed hf mfp security. Now it works in all the modes. (@drHatson)
 - Added `hf fido` commands that work with FIDO U2F authenticators (@merlokk)
 - Added mbedtls instead of old polarssl (@merlokk)
 - Added jansson (@merlokk)
 - Added `hf emv scan` - save card data to json file (@merlokk)
 - Added `hf emv` `gpo`, `readrec`, `genac`, `challenge`, `intauth` - separate commands from `hf emc exec` (@merlokk)
 - Added  `hf fido` `assert` and `make` commands from fido2 protocol (authenticatorMakeCredential and authenticatorGetAssertion) (@merlokk)
 - Added trailer block decoding to `hf mf rdbl` and `hf mf cgetbl` (@merlokk)
 - Added `hf mf mad` and `hf mfp mad` MAD decode, check and print commands (@merlokk)
 - Added `script run luxeodump` (@0xdrrb)
 - Fixed `lf hitag reader 02` - print all bytes (@bosb)
 - Fixed hitag S simulation (still not working), write, add example HITAG S 256  (@bosb)


### Fixed
 - Changed driver file proxmark3.inf to support both old and new Product/Vendor IDs (@pwpiwi)
 - Changed start sequence in Qt mode (fix: short commands hangs main Qt thread) (@merlokk)

## [ice.3.1.0][2017-09-26]
 - proxmark3 client can reconnect to device without restart (@iceman1001)
 - lots of bug fixes (many many)
 - trace/securakey-64169.pm3 - trace of a scecurakey (@atyppo)
 - `hf mf decrypt` - got some longer input and helptext parameter (@iceman1001)
 - Updated the Reveng 1.51 sourcecode to 1.52 from Reveng project homepage (@iceman1001)
 - `hf 14a read` - disconnects when failing to read tag (@iceman1001)
 - `hf mf csave` - renamed parameter `i` to `o` as in output (@iceman1001)

## [3.0.0][2017-08-29]
  Notes on this release
  this version includes a merge with the PM3 official v3.0.1 code, a.k.a the "Monster merge"
  There is a lot of changes,  command breaking changes, which is the cause for the JUMP in version number.
  It is set to v3.0.0 to show that it is on par with PM3 Official v3 release.

 - Updated `mkversion.pl` to write a date based on file when repo is downloaded as a zip file from github (@iceman1001)
 - Update `readme.md` to fit GitHubs markup(@joanbono)
 - Added `script run ul_uid`, try to change UID on a magic UL-card. (@iceman1001)
 - Fixed `hf snoop` bug,  of wrong bool value (@ikarus23)
 - Fixed fullimage.s19, wrong offsets (@doegox)
 - Updated `77-mm-usb-device-blacklist.rules` for the pid/vid (@iceman1001)
 - `hf 14a sim` now follows Mifare UL-EV1 protocol better (@iceman1001)
 - Updated `fpga_hf.bit` file (@pwpiwi)
 - Added more card detections to `hf mfu info` (@iceman1001)
 - Fixed `hf mfu restore/dump` to use the right struct values in special data in dumpfile. (@iceman1001)
 - Added `hf mfu restore r` new parameter to use the new pwd for all further auths needed when executing (@iceman1001)
 - Added `default_keys_dic2lua.awk` script to generate default_keys file in client/lualibs (@iceman1001)
 - Fixes to lots of lua scripts, among others
    - `mifare_autopwn`, now uses PRNG detection (@iceman1001)
    - `mfkeys`, fixed bug which only tested the first key (@iceman1001)
    - `dumptoemul`, removed last newline (@iceman1001)
    - ...
 - Added USB/SERIAL communication enhancements (@micolous)
 - Changed `hf 14a cuids`, to be interrupted with keyboard press (@iceman1001)
 - Changed debugstatements for LF to show which function more unified (@iceman1001)
 - Added `script run calc_di` , to calculate some Mifare keys (@iceman1001)
 - Fixed iclass commands never shut down antenna afterwards (@iceman1001)
 - Changed 512kb detection when flashing (@iceman1001)
 - Fixed compilation GCC4.9 or higher detection (@winguru)
 - Fixed compiler warnings in Ubuntu 17.04 (@iceman1001)
 - Ripped out `standalone` code into separate folder to be continued. (@iceman1001)
 - `hf mf nested`, added key validation to entered key (@merlokk)
 - `hf mf hardnested`, added key validation to enterd key (@iceman1001)
 - Changed a lot of help texts (@iceman1001)
 - Fixed `hf mf chk` - keyblock bug, limited keys to 256. (@iceman1001)
 - Changed `hf mf dump` retries three times now before giving up (@marshmellow42)
 - Fixed `mfu authentication`, with pack-len error (@iceman1001)
 - `Script list`, change sortorder to alphabetic order (@iceman1001)
 - Changed `hf mfu gen` to read taguid (@iceman1001)
 - Changed `hf mfu pwdgen` to read taguid (@iceman1001)
 - Added `hf mf setmod` sets Mifare Classic EV1 load modulation strength to card (@angelsl)
 - Added `hf 14a read`  Mifare PRNG detection based on @doegox LIBNFC impl (@iceman1001)
 - Added `hf mf nonces`, collects Mifare Classic nonces for analysing of PRNG (@iceman1001)
 - Added new CSNS in `hf iclass sim 2` attack (@iceman1001)
 - Added more default keys (@iceman1001)
 - Added analyse nuid, enable creation of Mifare NUID (@iceman1001)
 - Updated the Reveng 1.44 sourcecode to 1.51 from Reveng project homepage (@iceman1001)
 - script run formatMifare - got an option to execute the generate strings (@iceman1001)
 - Fixed `hf mf cgetsc` (@iceman1001)
 - Fixed `hf legic info` (@iceman1001)
 - Changed version output (@iceman1001)
 - Added PAC/Stanley detection to lf search (@marshmellow42)
 - Added lf pac demod and lf pac read - extracts the raw blocks from a PAC/Stanley tag (@marshmellow42)
 - Added hf mf csave commands compatibity for 4k (@Fl0-0)
 - Added data fsktonrz, a fsk cleaning/demodulating routine for weak fsk signal. Note: follow this up with a `data rawdemod nr` to finish demoding your signal. (@marshmellow42)
 - Added lf em 410xbrute, LF EM410x reader bruteforce attack by simulating UIDs from a file (@Fl0-0)
 - Compiles on OS X
 - Compiles with gcc 4.9
 - Compiles for non-Intel CPUs
 - Added lf hitag write 24, the command writes a block to hitag2 tags in crypto mode (@henjo)
 - Added the improved `hf mf hardnested`, an attack working for hardened Mifare cards (EV1, Mifare Plus SL1)
 - Added experimental testmode write option for t55xx (danger) (@marshmellow42)
 - Added t55xx p1detect to `lf search` chip detections (@marshmellow42)
 - Added lf t55xx p1detect, detect page 1 of a t55xx tag based on E015 mfg code (@marshmellow42)
 - Added lf noralsy demod, read, clone, sim commands (@iceman1001)
 - Added lf jablotron demod, read, clone, sim commands (@iceman1001)
 - Added lf nexwatch read   - reads a nexwatch tag from the antenna
 - Added lf paradox read    - reads a paradox tag from the antenna
 - Added lf fdx sim (@iceman1001)
 - Added lf fdx clone       - clones an fdx-b animal tag to t55x7 or q5 (@iceman1001)
 - Added lf fdx read        - reads a fdx-b tag from the antenna (@iceman1001)
 - Added lf gproxii read    - reads a gproxii tag from the antenna (@marshmellow42)
 - Added lf indala read     - reads an indala tag from the antenna (@marshmellow42)
 - Added lf visa2000 demod, read, clone, sim commands (@iceman1001)

## [1.7.0 iceman fork] [2017-03-07]
 - hf mf dump - added retry loops to try each read attempt up to 3 times.  makes getting a complete dump easier with many antennas. (@marshmellow42)
 - Added markers in the graph around found Sequence Terminator after askmandemod. (@marshmellow42)
 - Added data mtrim <start> <stop> command to trim out samples between start and stop. (@marshmellow42)
 - Added data setgraphmarkers <orange> <blue> command to set two extra markers on the graph (@marshmellow42)
 - added json support in lua (@vitorio)
 - added a buspirate settings file for at91sam7s512 (@adamlaurie)
 - `lf read` timeouts is now depended on what threshold level you set in `lf config`  (@marshmellow42)
 - `hf mf sim` fixed a bug which made sim fail auths. (@iceman1001)
 - `hf 14a read` added magic tag generation 1a and 1b detection  (@iceman1001)
 - correctly using stdtypes.h printf and scanf format string macros (PRIx64 et al) (@pwpiwi)
 - Fixed linker warning re missing entry point when linking fullimage.elf (@pwpiwi)
 - small changes to lf psk and fsk demods to improve results when the trace begins with noise or the chip is not broadcasting yet (@marshmellow42)
 - NOTE CHANGED ALL `lf em4x em*` cmds to simpler `lf em ` - example: `lf em4x em410xdemod` is now `lf em 410xdemod`
 - Renamed and rebuilt `lf em readword` && readwordpwd to `lf em 4x05read` - it now demods and outputs the read block (@marshmellow42/@iceman1001)
 - Renamed and rebuilt `lf em writeword` && writewordpwd to `lf em 4x05write` - it now also reads validation output from the tag (@marshmellow42/@iceman1001)
 - Fixed bug in lf sim and continuous demods not turning off antenna when finished
 - Added lua script path fixes (@pwpiwi)
 - `lf search` - Added EM4x05/EM4x69 chip detection (@marshmellow42)
 - Added lf em 4x05dump command to read and output all the blocks of the chip (@marshmellow42)
 - Added lf em 4x05info command to read and display information about the chip (@marshmellow42)
 - `lf em4x em4x50***` refactoring of em4x50 commands. (@iceman1001)

## [1.6.9 iceman fork] [2017-02-06]
 - Serial speedup,  if possible 408600baud otherwise default to 115200baud (@iceman1001)
 - `hf emv` - Added Peter Fillmore EMV branch now compiles on iceman fork.  See separate issue. (@iceman1001)
 - `hf 14a reader` - Aztek detection. (@iceman1001)
 - `standalone mode` - added more detection of tags and refactored (@iceman1001)
 - `script run ufodump` - dumps an Aztek tag. (@iceman1001)
 - `script run hard_autopwn` - runs hardnested attack against all sectors on tag (@iceman1001)
 - Added lf cotag read, and added it to lf search (@iceman1001)
 - Added hitag2 read UID only and added that to lf search (@marshmellow42)
 - `lf search` -  check for if signal is only noice (@marshmellow42)
 - `hf 14a reader` - fixed a bug when card has sak 0x00 but still is not UL/NTAG etc. (@iceman1001)
 - `hf mf sim` / `hf 14a sim` - use random nonce. (@micolous)
 - `hw tune` - only prints out if voltage is detected from antenna. (@iceman1001)
 - `hf iclass decrypt` - only decrypt Application1 (@iceman1001)
 - `lf t55xx detect` - when finding multiple possible config blocks, see if a known configblock exists and select. (@iceman1001)

## [1.6.7 iceman fork] [2017-01-05]
 - `lf animal` - FDX-B animal commands (@iceman1001)
 - Fixed bugs in `lf sim` and other lf continuous demods not turning off antenna when finished (@marshmellow42)
 - `hf iclass write` - fixed bugs, added crc. (?)
 - `hf iclass dump` - changed layout in dump (@iceman1001)
 - Changed - debug statements are more clear (@iceman1001)
 - `lf search` - fixed the silent option when acquire data. (@iceman1001)
 - `lf search` - added presco, visa2000, noralsy detection (@iceman1001)
 - `lf precso` - fixed some bitsgeneration in precso bits (@iceman1001)
 - Added `lf noralsy` -  adds demod/clone/sime of Noralsy LF tags. (@iceman1001)
 - Added `lf visa2000` - adds demod/clone/sim of Visa2000 lF tags. (@iceman1001)
 - Added `hf mf key_brute` - adds J-Runs 2nd phase bruteforce ref: https://github.com/J-Run/mf_key_brute   (@iceman1001)
 - Added `lf jablotron` - adds demod/clone/sim of Jablotron LF tags. (@iceman1001)
 - Added `lf t55xx recoverpw` - adds a new password recovery using bitflips and partial flips if password write went bad. (@alexgrin)
 - `hf legic` - added improved legic data mapping. (jason)
 - `hf mf mifare` - added possibility to target key A|B (@douniwan5788)
 - Added `analyse lrc` - added a new main command group,  to help analysing bytes & bits & nibbles. (@iceman1001)
 - Added `lf nedap` - added identification of a NEDAP tag. (@iceman1001)
 - `lf viking clone` - fixed a bug. (@iceman1001)
 - Added bitsliced bruteforce solver in `hf mf hardnested` (@Aczid)
 - `hf mf chk` speedup (@iceman1001)
 - `hf 14a/mf sim x` attack mode,  now uses also moebius version of mfkey32 to try finding the key. (@iceman1001)
 - `hf 14a sim` Added emulation of Mifare cards with 10byte UID length. (@iceman1001)
 - `hf mf sim` Added emulation of Mifare cards with 10byte UID length. (@iceman1001)
 - Added `lf guard clone/sim` (@iceman1001)
 - Added `lf pyramd clone/sim` (@iceman1001)
 - trying to fix `hf 14b` command to be able to read CALYPSO card. (@iceman1001)
 - `hf legic load`, it now loads faster and a casting bug is gone. (@iceman1001)
 - Added `hf legic calccrc8` added a method to calculate the legic crc-8 value (@iceman1001)
 - `hf legic decode` fixed the output overflow bugs, better printing (@iceman1001)
 - Coverity Scan fixes a lot of resource leaks, etc (@iceman1001)
 - Added `lf presco *` commands started (@iceman1001)
 - Added `lf hid wiegand` added a method to calculate WIEGAND in different formats, (@iceman1001)
 - `hf mf chkkeys` better printing, same table output as nested, faster execution and added Adam Lauries "try to read Key B if Key A is found" (@iceman1001)
 - `hf mf nested` better printing and added Adam Lauries "try to read Key B if Key A is found" (@iceman1001)
 - `hf mf mifare` fixing the zero parity path, which did not get called. (@iceman1001)
 - Updated the @blapost Crapto1 implementation to v3.3 (blapost)
 - `hf mf c*` updated the calling structure and refactored of the chinese magic commands (@iceman1001, @marshmellow42)
 - Started to add Peter Fillmore  EMV fork into Iceman fork. ref: https://github.com/peterfillmore/proxmark3  (@peterfillmore,  @iceman1001)
 - Added Travis-CI automatic build integration with GitHub fork. (@iceman1001)
 - Updated the Reveng 1.30 sourcecode to 1.31 from Reveng project homepage (@iceman1001)
 - Updated the Reveng 1.31 sourcecode to 1.40 from Reveng project homepage (@iceman1001)
 - Added possibility to write direct to a Legic Prime Tag (MIM256/1024) without using values from the `BigBuffer` -> `hf legic writeRaw <addr> <value>` (@icsom)
 - Added possibility to decrease DCF values at address 0x05 & 0x06 on a Legic Prime Tag
   DCF-value will be pulled from the BigBuffer (address 0x05 & 0x06) so you have to
   load the data into the BigBuffer before with `hf legic load <path/to/legic.dump>` & then
   write the DCF-Values (both at once) with `hf legic write 0x05 0x02`  (@icsom)
 - Added script `legic.lua` for display and edit Data of Legic-Prime Tags (@icsom)
 - Added the experimental HITAG_S support (@spenneb)
 - Added topaz detection to `hf search` (@iceman1001)
 - Fixed the silent mode for 14b to be used inside `hf search` (@iceman1001)

### Added
 - Added a LF ASK Sequence Terminator detection option to the standard ask demod - and applied it to `lf search u`, `lf t55xx detect`, and `data rawdemod am s` (@marshmellow42)
 - `lf awid bruteforce <facilitycode>` - Simple bruteforce attack against a AWID reader.
 - `lf t55xx bruteforce <start password> <end password> [i <*.dic>]` - Simple bruteforce attack to find password - (@iceman1001 and others)
 - `lf viking clone`- clone viking tag to t55x7 or Q5 from 4byte hex ID input
 - `lf viking sim`  - sim full viking tag from 4byte hex ID input
 - `lf viking read` - read viking tag and output ID
 - `lf t55xx wipe`  - sets t55xx back to factory defaults
 - Added viking demod to `lf search` (@marshmellow42)
 - `data askvikingdemod` demod viking id tag from graphbuffer (@marshmellow42)
 - `lf t55xx resetread` added reset then read command - should allow determining start of stream transmissions (@marshmellow42)
 - `lf t55xx wakeup` added wake with password (AOR) to allow lf search or standard lf read after (@iceman1001, @marshmellow42)
 - `hf mf eload u` added an ultralight/ntag option. (@marshmellow42)
 - `hf iclass managekeys` to save, load and manage iclass keys.  (adjusted most commands to accept a loaded key in memory) (@marshmellow42)
 - `hf iclass readblk` to select, authenticate, and read 1 block from an iclass card (@marshmellow42)
 - `hf iclass writeblk` to select, authenticate, and write 1 block to an iclass card (or picopass) (@marshmellow42 + others)
 - `hf iclass clone` to take a saved dump file and clone selected blocks to a new tag (@marshmellow42 + others)
 - `hf iclass calcnewkey` - to calculate the div_key change to change a key - (experimental) (@marshmellow42 + others)
 - `hf iclass encryptblk` - to encrypt a data block hex to prep for writing that block (@marshmellow42)
 - ISO14443a stand-alone operation with ARM CFLAG="WITH_ISO14443a_StandAlone". This code can read & emulate two banks of 14a tag UIDs and write to "magic" cards  (Craig Young)
 - AWID26 command context added as `lf awid` containing realtime demodulation as well as cloning/simulation based on tag numbers (Craig Young)
 - Added `hw status`. This command makes the ARM print out some runtime information. (@holiman)
 - Added `hw ping`. This command just sends a usb packets and checks if the pm3 is responsive. Can be used to abort certain operations which supports abort over usb. (@holiman)
 - Added `data hex2bin` and `data bin2hex` for command line conversion between binary and hexadecimal (@holiman)
 - Added `hf snoop`. This command take digitalized signal from FPGA and put in BigBuffer. (@pwpiwi + enio)
 - Added Topaz (NFC type 1) protocol support (`hf topaz reader`, `hf list topaz`, `hf 14a raw -T`, `hf topaz snoop`). (@pwpiwi)
 - Added option c to `hf list` (mark CRC bytes) (@pwpiwi)

### Changed
 - Added `[l] <length>` option to data printdemodbuffer
 - Adjusted lf awid clone to optionally clone to Q5 tags
 - Adjusted lf t55xx detect to find Q5 tags (t5555) instead of just t55x7
 - Adjusted all lf NRZ demods - works more accurately and consistently (as long as you have strong signal)
 - Adjusted lf pskindalademod to reduce false positive reads.
 - Small adjustments to psk, nrz, and ask clock detect routines - more reliable.
 - Adjusted lf em410x em410xsim to accept a clock argument
 - Adjusted lf t55xx dump to allow overriding the safety check and warning text (@marshmellow42)
 - Adjusted lf t55xx write input variables (@marshmellow42)
 - Adjusted lf t55xx read with password safety check and warning text and adjusted the input variables (@marshmellow42 & @iceman1001)
 - Adjusted LF FSK demod to account for cross threshold fluctuations (898 count waves will adjust the 9 to 8 now...) more accurate. (@marshmellow42)
 - Adjusted timings for t55xx commands.  more reliable now. (@marshmellow42 & @iceman1001)
 - `lf cmdread` adjusted input methods and added help text (@marshmellow42 & @iceman1001)
 - changed `lf config t <threshold>` to be 0 - 128 and will trigger on + or - threshold value (@marshmellow42)
 - `hf iclass dump` cli options - can now dump AA1 and AA2 with different keys in one run (does not go to multiple pages for the larger tags yet) (@marshmellow42)
 - Revised workflow for StandAloneMode14a (Craig Young)
 - EPA functions (`hf epa`) now support both ISO 14443-A and 14443-B cards (@frederikmoellers)
 - `hw version` only talks to ARM at startup, after that the info is cached. (@pwpiwi)
 - Added `r` option to iclass functions - allows key to be provided in raw block 3/4 format

## [2.2.0][2015-07-12]

### Changed
 - Added `hf 14b raw -s` option to auto select a 14b std tag before raw command
 - Changed `hf 14b write` to `hf 14b sriwrite` as it only applied to sri tags (@marshmellow42)
 - Added `hf 14b info` to `hf search` (@marshmellow42)
 - Added compression of fpga config and data, *BOOTROM REFLASH REQUIRED* (@pwpiwi)
 - Implemented better detection of mifare-tags that are not vulnerable to classic attacks (`hf mf mifare`, `hf mf nested`) (@pwpiwi)

### Added
 - Added `hf 14b reader` to find and print general info about known 14b tags (@marshmellow42)
 - Added `hf 14b info` to find and print info about std 14b tags and sri tags (using 14b raw commands in the client)  (@marshmellow42)
 - Added PACE replay functionality (@frederikmoellers)

### Fixed
 - t55xx write timing (@marshmellow42)


## [2.1.0][2015-06-23]

### Changed
 - Added ultralight/ntag tag type detection to `hf 14a read` (@marshmellow42)
 - Improved ultralight dump command to auto detect tag type, take authentication, and dump full memory (or subset specified) of known tag types (@iceman1001 / @marshmellow42)
 - Combined ultralight read/write commands and added authentication (@iceman1001)
 - Improved LF manchester and biphase demodulation and ask clock detection especially for reads with heavy clipping. (@marshmellow42)
 - Iclass read, `hf iclass read` now also reads tag config and prints configuration. (@holiman)
 - *bootrom* needs to be flashed, due to new address boundaries between os and fpga, after a size optimization (@pwpiwi)

### Fixed
 - Fixed EM4x50 read/demod of the tags broadcasted memory blocks. `lf em4x em4x50read` (not page read) (@marshmellow42)
 - Fixed issue #19, problems with LF T55xx commands (@iceman1001, @marshmellow42)
 - Fixed various problems with iso14443b, issue #103 (@pwpiwi, @marshmellow42)

### Added
 - Added `hf search` - currently tests for 14443a tags, iclass tags, and 15693 tags (@marshmellow42)
 - Added `hf mfu info` Ultralight/NTAG info command - reads tag configuration and info, allows authentication if needed (@iceman1001, @marshmellow42)
 - Added Mifare Ultralight C and Ultralight EV1/NTAG authentication. (@iceman1001)
 - Added changelog
 - Added `data fdxbdemod` - Demodulate a FDX-B ISO11784/85 Biphase tag from GraphBuffer aka ANIMAL TAG (@marshmellow42, @iceman1001)

## [2.0.0] - 2015-03-25
### Changed
 - LF sim operations now abort when new commands arrive over the USB - not required to push the device button anymore.

### Fixed
 - Mifare simulation, `hf mf sim` (was broken a long time) (@pwpiwi)
 - Major improvements in LF area and data operations. (@marshmellow42, @iceman1001)
 - Issues regarding LF simulation (@pwpiwi)
 - Issue interpreting NXP "get sys info" command return value for icode tags. (@mjacksn)

### Added
 - iClass functionality: full simulation of iclass tags, so tags can be simulated with data (not only CSN). Not yet support for write/update, but readers do not seem to enforce update. (@holiman).
 - iClass decryption. Proxmark can now decrypt data on an iclass tag, but requires you to have the HID decryption key locally on your computer, as this is not bundled with the sourcecode.
 - `hf 15 info` can detect NTAG 5 tags
 - `hf 15 info` include an EAS status check on more of the icode tags which support EAS (SLI, SLIX, SLIX-L, and SLIX-S)
