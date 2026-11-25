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
// Low frequency Xhorse super-chip (XT27A) commands.  An XT136 is identified and read
// (info / dump); configure and reset refuse it.
//
// `-t` is a chip type (IDnn, or the PCF 79nn form where one exists).
// Only XT27A types whose on-air write path is fully decoded are offered;
// ID8C (opaque) and the XT27B/XT57-only types are refused.
//-----------------------------------------------------------------------------

#include "cmdlfxhorse.h"

#include <ctype.h>
#include <string.h>

#include "cmdparser.h"    // command_t
#include "comms.h"
#include "commonutil.h"
#include "cliparser.h"
#include "pm3_cmd.h"
#include "ui.h"           // PrintAndLogEx
#include "util.h"
#include "xhorse.h"       // shared struct / recipe enum

static int CmdHelp(const char *Cmd);
// Channel-B CRC-8 (poly 0x07, init 0); defined with the xt57 helpers below, used by `info`
// to check the identify reply's CRC (docs/xt57-auth.md SS4).
static uint8_t xt57_crc8(const uint8_t *d, size_t n);
// Channel-B helpers, defined with the xt57 code below; `info` uses them to read an XT37/XT57's
// emulated type byte out of its config block (docs/xt37.md SS5o).
static const char *xt_type_name(uint8_t t);
static int xt_read_typeword(bool is37, uint32_t *word);
typedef struct xt57_ks_s xt57_ks_t_fwd;
static int xt_steer(const struct xt57_ks_s *ks, int32_t from, int32_t *at, bool verbose);

//-----------------------------------------------------------------------------
// Type catalog
//-----------------------------------------------------------------------------
typedef enum { UID_NONE, UID_REQ4, UID_REQ5, UID_REQ8 } xh_uidrule_t;

typedef struct {
    const char   *canon;     // canonical token
    const char   *aliases;   // space-separated alternative tokens, or ""
    uint8_t       recipe;    // xhorse_recipe_t
    uint8_t       typebyte;  // on-chip 0x3C low byte
    bool          has_type;  // write the type byte first? (ID8A's recipe writes 0x8A itself, last)
    bool          typeword;  // ID13: writes 0x0080008C
    xh_uidrule_t  uid;
    uint16_t      aux;       // config word for the template family
    const char   *emulates;  // short help string
} xh_type_t;

static const xh_type_t g_types[] = {
    // canon   aliases                              recipe             tb    htype tword  uidrule   aux     emulates
    { "ID11",  "",                                  XHORSE_R_ID11,     0x8C, true,  false, UID_REQ8, 0,      "Temic fixed-code" },
    { "ID12",  "",                                  XHORSE_R_ID12,     0x8C, true,  false, UID_REQ8, 0,      "Temic fixed-code" },
    { "ID13",  "",                                  XHORSE_R_ID13,     0x8C, true,  true,  UID_REQ5, 0,      "Megamos 13" },
    { "7935",  "ID33 ID40 ID41 ID42 ID43 ID44",     XHORSE_R_7935,     0x35, true,  false, UID_NONE, 0,      "PCF7935 (SECT) (blanked)" },
    { "7936",  "ID46",                              XHORSE_R_HITAG2,   0x36, true,  false, UID_REQ4, 0,      "PCF7936 (HITAG2)" },
    { "7937",  "",                                  XHORSE_R_HITAG2,   0x37, true,  false, UID_REQ4, 0,      "PCF7937 (HITAG2)" },
    { "7938",  "ID47",                              XHORSE_R_7938,     0x47, true,  false, UID_REQ4, 0,      "PCF7938" },
    { "7939",  "ID49",                              XHORSE_R_7939,     0x39, true,  false, UID_REQ4, 0,      "PCF7939" },
    { "7946",  "",                                  XHORSE_R_HITAG2,   0x46, true,  false, UID_REQ4, 0,      "PCF7946 / HITAG2 plus" },
    { "ID48",  "",                                  XHORSE_R_ID48,     0x48, true,  false, UID_REQ4, 0,      "Megamos Crypto ID48" },
    { "ID4C",  "",                                  XHORSE_R_ID4C,     0x4C, true,  false, UID_REQ8, 0,      "Texas Instruments (TIRIS)" },
    { "ID4D",  "ID60 ID63",                         XHORSE_R_TEMPLATE, 0x4D, true,  false, UID_NONE, 0xAAF3, "Texas Crypto" },
    { "4D80",  "ID70 ID83",                         XHORSE_R_TEMPLATE, 0x80, true,  false, UID_NONE, 0x9201, "Texas Crypto DST80" },
    { "ID4E",  "ID64",                              XHORSE_R_TEMPLATE, 0x64, true,  false, UID_NONE, 0x33A6, "Texas Crypto ID64" },
    { "ID8A",  "",                                  XHORSE_R_ID8A,     0x8A, false, false, UID_REQ4, 0,      "Texas Crypto DST128" },
    { "ID8E",  "",                                  XHORSE_R_ID8E,     0x8E, true,  false, UID_REQ4, 0,      "Megamos Crypto 2" },
};
#define XH_NTYPES (sizeof(g_types) / sizeof(g_types[0]))

// Types we deliberately refuse, with a reason (so the error is self-explanatory).
static const char *xh_refused(const char *tok) {
    if (strcmp(tok, "ID8C") == 0) {
        return "ID8C: write protocol not decoded (opaque) - not supported";
    }
    if (strcmp(tok, "ID4A") == 0 || strcmp(tok, "MQB") == 0 || strcmp(tok, "MQB49") == 0 ||
            strcmp(tok, "MQBRENEGADE") == 0 || strcmp(tok, "MQB45") == 0) {
        return "XT27B / XT57 only - not supported on XT27A";
    }
    return NULL;
}

static bool token_matches(const xh_type_t *t, const char *tok) {
    if (strcmp(t->canon, tok) == 0) {
        return true;
    }
    if (t->aliases[0] == '\0') {
        return false;
    }
    // search the space-separated alias list for an exact token
    const char *p = t->aliases;
    size_t toklen = strlen(tok);
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        const char *start = p;
        while (*p && *p != ' ') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len == toklen && strncmp(start, tok, len) == 0) {
            return true;
        }
    }
    return false;
}

static const xh_type_t *resolve_type(const char *raw) {
    // upper-case, strip an optional 0x prefix
    char tok[32];
    size_t n = 0;
    const char *s = raw;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (; *s && n < sizeof(tok) - 1; s++) {
        tok[n++] = (char)toupper((unsigned char) * s);
    }
    tok[n] = '\0';

    for (size_t i = 0; i < XH_NTYPES; i++) {
        if (token_matches(&g_types[i], tok)) {
            return &g_types[i];
        }
    }
    return NULL;
}

static const char *uidrule_text(xh_uidrule_t r) {
    switch (r) {
        case UID_REQ8:
            return "8 bytes";
        case UID_REQ5:
            return "5 bytes";
        case UID_REQ4:
            return "4 bytes";
        case UID_NONE:
            return "none";
    }
    return "none";
}

//-----------------------------------------------------------------------------
// types
//-----------------------------------------------------------------------------
static int CmdLFXhorseTypes(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse types",
                  "List the emulated-type names accepted by `lf xhorse configure -t`.",
                  "lf xhorse types");
    void *argtable[] = { arg_param_begin, arg_param_end };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "Xhorse XT27A - emulated types accepted by " _YELLOW_("`lf xhorse configure -t`"));
    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "  %-38s %-7s %-7s %s", "-t type", "on-chip", "UID", "emulates");
    PrintAndLogEx(INFO, "  %-38s %-7s %-7s %s", "--------------------------------------", "-------", "-------", "---------------------------");

    for (size_t i = 0; i < XH_NTYPES; i++) {
        const xh_type_t *t = &g_types[i];
        char toks[64];
        if (t->aliases[0]) {
            snprintf(toks, sizeof(toks), "%s %s", t->canon, t->aliases);
        } else {
            snprintf(toks, sizeof(toks), "%s", t->canon);
        }
        char onchip[12];
        if (t->typeword) {
            snprintf(onchip, sizeof(onchip), "0x%02X *", t->typebyte);
        } else {
            snprintf(onchip, sizeof(onchip), "0x%02X", t->typebyte);
        }
        PrintAndLogEx(INFO, "  %-38s %-7s %-7s %s", toks, onchip, uidrule_text(t->uid), t->emulates);
    }

    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "  Names are case-insensitive; " _YELLOW_("none") " = no UID accepted; otherwise -u is required.");
    PrintAndLogEx(INFO, "  Not supported: " _RED_("ID8C") " (opaque write, not decoded);");
    PrintAndLogEx(INFO, "                 " _RED_("ID4A / MQB / MQB49 / MQB-Renegade / MQB-4.5") " (XT27B / XT57 only).");
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// helpers for info/dump display
//-----------------------------------------------------------------------------
static const char *typebyte_name(uint8_t tb) {
    // ID11, ID12 and ID13 all store 0x8C (they differ in the bank-0 image), so this must be
    // checked before the table, whose first 0x8C entry is ID11.
    if (tb == 0x8C) {
        return "ID11/12/13";
    }
    for (size_t i = 0; i < XH_NTYPES; i++) {
        if (!g_types[i].typeword && g_types[i].typebyte == tb) {
            return g_types[i].canon;
        }
    }
    return "unknown";
}

// payload/len may be NULL/0; dump sends one byte, the bank to read.
static int xh_request_info(uint16_t cmd, uint8_t *payload, uint16_t len, lf_xhorse_info_t *out) {
    clearCommandBuffer();
    SendCommandNG(cmd, payload, len);
    PacketResponseNG resp;
    // An XT136 is only tried after every XT27A handshake timing has failed (~1.4 s), and its own
    // session open uses the VVDI's 130 ms off / 220 ms on: a few seconds before the reads start.
    if (WaitForResponseTimeout(cmd, &resp, 10000) == false) {
        PrintAndLogEx(WARNING, "timeout while waiting for reply");
        return PM3_ETIMEOUT;
    }
    if (resp.length < sizeof(lf_xhorse_info_t)) {
        PrintAndLogEx(FAILED, "short reply from device");
        return PM3_ESOFT;
    }
    memcpy(out, resp.data.asBytes, sizeof(lf_xhorse_info_t));
    return PM3_SUCCESS;
}

// info/dump never send the EM-recovery burst or the ID8A probe (both change the chip), so a chip
// stuck in an EM, ID48, ID8E or ID8A emulation simply doesn't answer them.  Say so.
static void xh_print_not_found(void) {
    PrintAndLogEx(FAILED, "No XT27A or XT136 answered the " _YELLOW_("XHORSE") " handshake");
    PrintAndLogEx(HINT, "An XT27A emulating an EM type (ID11/12/13), ID48, ID8E or ID8A never answers it;");
    PrintAndLogEx(HINT, "`" _YELLOW_("lf xhorse reset") "` or `" _YELLOW_("lf xhorse configure") "` can recover it.");
    PrintAndLogEx(HINT, "For an ID8A chip, `" _YELLOW_("--probe b") "` reads it without a rewrite, at the cost of its type byte.");
}

// --probe for info/dump.  A chip set to ID8A answers no handshake at any power-up delay; the only
// thing that reaches it is the VVDI's TI-style probe, and the word that is known to work (B)
// reverts the chip's emulated type byte to 0x36, which is why a read never sends one by itself.
// Word A, the one the VVDI sends first, opens the chip too, but zeroes the type byte instead of
// reverting it - so it is no safer, and 0x00 is not even a valid type (section 12.4).
// --probe-word takes a whole 98-symbol word, and --probe-type patches just the candidate type-byte
// field of word B (bits 17..24, MSB first, S = 0): both are research controls for working out what
// the 98 bits say, and the chip acts on whatever is sent.
static const char xh_cli_probe_b[] =
    "SLLSSSLLSLLLSSLSSSSLLSLLSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSLSLSSSLSLSSL";
#define XH_PROBE_TYPE_OFF  17     // first symbol of the candidate field

// Returns PM3_SUCCESS and fills opt->probe (and opt->word), or prints why and fails.
static int xh_parse_probe(struct arg_str *a, struct arg_str *wordarg, struct arg_str *typearg,
                          lf_xhorse_rdopt_t *opt) {
    opt->probe = XHORSE_PROBE_NONE;
    if (a->count && (wordarg->count || typearg->count)) {
        PrintAndLogEx(ERR, "--probe, --probe-word and --probe-type are alternatives; pick one");
        return PM3_EINVARG;
    }
    if (wordarg->count && typearg->count) {
        PrintAndLogEx(ERR, "--probe-word and --probe-type are alternatives; pick one");
        return PM3_EINVARG;
    }
    if (wordarg->count) {
        const char *w = wordarg->sval[0];
        if (strlen(w) != XHORSE_ID8A_SYMS) {
            PrintAndLogEx(ERR, "--probe-word needs exactly %d symbols, got %zu", XHORSE_ID8A_SYMS, strlen(w));
            return PM3_EINVARG;
        }
        for (int i = 0; i < XHORSE_ID8A_SYMS; i++) {
            char c = (char)toupper((unsigned char)w[i]);
            if (c != 'S' && c != 'L') {
                PrintAndLogEx(ERR, "--probe-word symbol %d is `%c`; only S and L are symbols", i, w[i]);
                return PM3_EINVARG;
            }
            opt->word[i] = c;
        }
        opt->probe = XHORSE_PROBE_CUSTOM;
        PrintAndLogEx(WARNING, "sending a " _YELLOW_("custom") " ID8A probe word - what it does to the chip is unknown");
        PrintAndLogEx(INFO, "word... %.*s", XHORSE_ID8A_SYMS, opt->word);
        return PM3_SUCCESS;
    }
    if (typearg->count) {
        uint32_t v = 0;
        if (sscanf(typearg->sval[0], "%x", &v) != 1 || v > 0xFF) {
            PrintAndLogEx(ERR, "--probe-type takes one hex byte, e.g. 8A");
            return PM3_EINVARG;
        }
        memcpy(opt->word, xh_cli_probe_b, XHORSE_ID8A_SYMS);
        for (int i = 0; i < 8; i++) {
            opt->word[XH_PROBE_TYPE_OFF + i] = ((v >> (7 - i)) & 1) ? 'L' : 'S';
        }
        opt->probe = XHORSE_PROBE_CUSTOM;
        PrintAndLogEx(WARNING, "sending word B with its candidate type field set to " _YELLOW_("0x%02X") " - untested ground", v);
        PrintAndLogEx(INFO, "word... %.*s", XHORSE_ID8A_SYMS, opt->word);
        return PM3_SUCCESS;
    }
    if (a->count == 0) {
        return PM3_SUCCESS;
    }
    uint8_t *probe = &opt->probe;
    const char *v = a->sval[0];
    if (v == NULL || strlen(v) != 1) {
        PrintAndLogEx(ERR, "--probe takes `a` or `b`");
        return PM3_EINVARG;
    }
    if (*v == 'a' || *v == 'A') {
        *probe = XHORSE_PROBE_A;
        PrintAndLogEx(WARNING, "ID8A probe " _YELLOW_("word A") " enabled - it zeroes the chip's type byte");
    } else if (*v == 'b' || *v == 'B') {
        *probe = XHORSE_PROBE_B;
        PrintAndLogEx(WARNING, "ID8A probe " _YELLOW_("word B") " enabled - it reverts the chip's type byte to 0x36");
    } else {
        PrintAndLogEx(ERR, "--probe takes `a` or `b`");
        return PM3_EINVARG;
    }
    return PM3_SUCCESS;
}

// Bank-0 pages whose reads never agreed are printed with a red '?' (see page_ok).
static bool xh_page_ok(const lf_xhorse_info_t *nfo, uint8_t pg) {
    return (nfo->page_ok[pg / 8] >> (pg % 8)) & 1;
}

// Always two visible characters, so a column after it stays aligned.
static const char *xh_page_mark(const lf_xhorse_info_t *nfo, uint8_t pg) {
    return xh_page_ok(nfo, pg) ? "  " : " " _RED_("?");
}

// `pages` is how many of image_pages were shown, so the count matches what was printed.
static void xh_print_unconfirmed(const lf_xhorse_info_t *nfo, uint8_t pages) {
    int bad = 0;
    for (uint8_t pg = 0; pg < pages; pg++) {
        bad += xh_page_ok(nfo, pg) ? 0 : 1;
    }
    if (bad) {
        PrintAndLogEx(WARNING, "bank %u: %d page(s) marked " _RED_("?") " unconfirmed: no two reads agreed, so they may be wrong; try again", (unsigned)nfo->bank, bad);
    }
}

// XT136 page meanings, from the VVDI's reset image (doc/xhorse_notes.md).
static const char *xh136_page_label(uint8_t pg) {
    switch (pg) {
        case 0x00:
            return "UID";
        case 0x01:
            return "HITAG2 password";
        case 0x02:
            return "HITAG2 key high";
        case 0x03:
            return "HITAG2 config";
        case 0x0D:
            return "config word 0";
        case 0x0E:
            return "config word 1 (high byte = type?)";
        case 0x0F:
            return "config word 2";
        default:
            break;
    }
    if (pg >= 0x04 && pg <= 0x07) {
        return "HITAG2 user page";
    }
    if (pg >= 0x08 && pg <= 0x0C) {
        return "unknown";
    }
    return "not known to exist";
}

// XT136: one flat page space, no chip ID or type page.
static void xh136_print(const lf_xhorse_info_t *nfo, const char *title) {
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("%s") " ---------------------------", title);
    PrintAndLogEx(INFO, "Family.... " _GREEN_("XT136") " (channel A, E20E signature 604A027E)");
    // The E430 reply wanders over 9BF2..9BF7 across sessions and chips - a timing counter, not a
    // per-chip value (docs/xt136.md SS2).  Shown because it is what seeds the cipher.
    PrintAndLogEx(INFO, "Seed...... E430 reply 0x%04X", nfo->seed_reply);
    PrintAndLogEx(INFO, "  page | data");
    PrintAndLogEx(INFO, "-------+-----------");
    // Firmware may still return pages above 0x0F; they only alias 0x00..0x0F, so never show them.
    uint8_t pages = (nfo->image_pages < XHORSE_XT136_PAGES) ? nfo->image_pages : XHORSE_XT136_PAGES;
    for (uint8_t pg = 0; pg < pages; pg++) {
        PrintAndLogEx(INFO, "  0x%02X | %s%s %s", pg, sprint_hex_inrow(&nfo->image[pg * 4], 4), xh_page_mark(nfo, pg), xh136_page_label(pg));
    }
    xh_print_unconfirmed(nfo, pages);
    if (pages >= 0x0F) {
        PrintAndLogEx(INFO, "Config word 1 high byte " _YELLOW_("0x%02X") " (%s, if it is the type byte)", nfo->image[0x0E * 4], typebyte_name(nfo->image[0x0E * 4]));
    }
    PrintAndLogEx(NORMAL, "");
}

static void xh_print_header(const lf_xhorse_info_t *nfo) {
    PrintAndLogEx(INFO, "Chip ID... " _GREEN_("%s"), sprint_hex_inrow(nfo->chipid, 16));
    PrintAndLogEx(INFO, "Seed...... 7372 reply 0x%04X", nfo->seed_reply);
    PrintAndLogEx(INFO, "Type byte. " _YELLOW_("0x%02X") " (%s)", nfo->typebyte, typebyte_name(nfo->typebyte));
    if (nfo->typebyte_ok == 0) {
        PrintAndLogEx(WARNING, "Type byte unconfirmed: no two reads of it agreed, so it may be wrong; try again");
    }
}

//-----------------------------------------------------------------------------
// info
//-----------------------------------------------------------------------------
static int CmdLFXhorseInfo(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse info",
                  "Identify an Xhorse super-chip.  First probes the XMICRO / channel-B identify:\n"
                  "newer families (XT57 / XT37-class) answer it and are reported as unsupported.\n"
                  "Otherwise reads the XT27A over channel A: unique chip ID, emulated type byte,\n"
                  "and the first 8 emulated-image pages.  If no XT27A answers, tries the XT136\n"
                  "handshake and reads its pages 0x00..0x0F.\n"
                  "A chip set to ID8A answers nothing until the VVDI's ID8A probe is replayed:\n"
                  "--probe sends it, at the cost of the chip's emulated type byte (word b).",
                  "lf xhorse info\n"
                  "lf xhorse info --probe b    -> read an ID8A chip; reverts its type byte to 0x36\n"
                  "lf xhorse info --probe a    -> the VVDI's other word; zeroes the type byte instead");
    void *argtable[] = {
        arg_param_begin,
        arg_str0(NULL, "probe", "<a|b>", "also try the ID8A probe to reach an ID8A chip; both words cost its type byte (b -> 0x36, a -> 0x00)"),
        arg_str0(NULL, "probe-word", "<98 x S|L>", "research: send this exact ID8A probe word instead"),
        arg_str0(NULL, "probe-type", "<hex>", "research: word B with its candidate type field set to this byte"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    lf_xhorse_rdopt_t opt;
    memset(&opt, 0, sizeof(opt));
    int pres = xh_parse_probe((struct arg_str *)argtable[1], (struct arg_str *)argtable[2],
                              (struct arg_str *)argtable[3], &opt);
    CLIParserFree(ctx);
    if (pres != PM3_SUCCESS) {
        return pres;
    }

    lf_xhorse_info_t nfo;
    int res = xh_request_info(CMD_LF_XHORSE_INFO, (uint8_t *)&opt, sizeof(opt), &nfo);
    if (res != PM3_SUCCESS) {
        return res;
    }

    // A reply to the XMICRO / channel-B identify means a newer super-chip family
    // (XT57 / XT37-class) that this command does not yet support.
    if (nfo.bchip) {
        // The reply is 4 ASCII family bytes (e.g. "XT37") followed by a CRC-8 over the 0011
        // command byte and those four - not a 5-character name, and not a seed: "XT57" -> 0x58,
        // which is why it read as "XT57X", and "XT37" -> 0x26 (docs/xt57-auth.md SS4).
        char type[5] = {0};
        size_t n = (nfo.bchip_type_len < 4) ? nfo.bchip_type_len : 4;
        size_t j = 0;
        for (size_t i = 0; i < n; i++) {
            uint8_t c = nfo.bchip_type[i];
            type[j++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        const char *type_str = (j == 0) ? "(unknown)" : type;
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "Found an Xhorse " _YELLOW_("channel-B") " (XMICRO) super-chip");
        PrintAndLogEx(INFO, "Chip type...... " _YELLOW_("%s"), type_str);
        if (nfo.bchip_type_len >= 5) {
            const uint8_t over[5] = { 0x03, nfo.bchip_type[0], nfo.bchip_type[1],
                                      nfo.bchip_type[2], nfo.bchip_type[3] };
            uint8_t want = xt57_crc8(over, sizeof(over));
            PrintAndLogEx(INFO, "Identify reply... %s  (%s + CRC %02X %s)",
                          sprint_hex_inrow(nfo.bchip_type, nfo.bchip_type_len), type_str,
                          nfo.bchip_type[4],
                          (want == nfo.bchip_type[4]) ? _GREEN_("ok") : _RED_("BAD"));
        } else {
            PrintAndLogEx(INFO, "Identify reply... %s  (%d bytes, no CRC byte)",
                          sprint_hex_inrow(nfo.bchip_type, nfo.bchip_type_len), nfo.bchip_type_len);
        }
        // The emulated type lives in the config block, not in the identify reply, so reading it
        // means opening an authenticated session - which needs a nonce this chip reaches, and that
        // is a per-tag power-up delay the search has to hunt for (docs/xt37.md SS4a).
        bool is37 = (n >= 4 && nfo.bchip_type[2] == '3' && nfo.bchip_type[3] == '7');
        uint32_t tw = 0;
        PrintAndLogEx(INFO, "reading the type byte (needs a session; this can take a moment)...");
        if (xt_read_typeword(is37, &tw) == PM3_SUCCESS) {
            PrintAndLogEx(SUCCESS, "Emulated type.. " _GREEN_("%02X") " (%s)",
                          (uint8_t)(tw & 0xFF), xt_type_name((uint8_t)(tw & 0xFF)));
            PrintAndLogEx(INFO, "config word.... %08X   (segment 3 block 1F page 4)", tw);
        } else {
            PrintAndLogEx(WARNING, "could not read the type byte - no recorded nonce came up");
            PrintAndLogEx(HINT, "the delay that reaches a nonce is per-tag; `lf xhorse xt57 read --at <us>`");
        }
        return PM3_SUCCESS;
    }

    if (nfo.found == 0) {
        xh_print_not_found();
        return PM3_ESOFT;
    }

    if (nfo.family == XHORSE_FAM_XT136) {
        xh136_print(&nfo, "Xhorse XT136");
        return PM3_SUCCESS;
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Xhorse XT27A") " ---------------------------");
    xh_print_header(&nfo);
    for (uint8_t pg = 0; pg < nfo.image_pages; pg++) {
        PrintAndLogEx(INFO, "  bank0 0x%02X: %s%s", pg, sprint_hex_inrow(&nfo.image[pg * 4], 4), xh_page_mark(&nfo, pg));
    }
    xh_print_unconfirmed(&nfo, nfo.image_pages);
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// dump
//-----------------------------------------------------------------------------
static int CmdLFXhorseDump(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse dump",
                  "Read all reachable channel-A storage of an Xhorse XT27A: the 16-byte chip ID,\n"
                  "the type byte, and pages 0x00..0x3F of both banks: bank 0 (the emulated-tag\n"
                  "image) and bank 1 (configuration and identity).  Each bank is read in its own session.\n"
                  "An XT136 has no banks and only pages 0x00..0x0F: they are read in one session.",
                  "lf xhorse dump\n"
                  "lf xhorse dump --probe b    -> dump an ID8A chip; reverts its type byte to 0x36\n"
                  "lf xhorse dump --probe a    -> the VVDI's other word; zeroes the type byte instead");
    void *argtable[] = {
        arg_param_begin,
        arg_str0(NULL, "probe", "<a|b>", "also try the ID8A probe to reach an ID8A chip; both words cost its type byte (b -> 0x36, a -> 0x00)"),
        arg_str0(NULL, "probe-word", "<98 x S|L>", "research: send this exact ID8A probe word instead"),
        arg_str0(NULL, "probe-type", "<hex>", "research: word B with its candidate type field set to this byte"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    lf_xhorse_rdopt_t popt;
    memset(&popt, 0, sizeof(popt));
    int pres = xh_parse_probe((struct arg_str *)argtable[1], (struct arg_str *)argtable[2],
                              (struct arg_str *)argtable[3], &popt);
    CLIParserFree(ctx);
    if (pres != PM3_SUCCESS) {
        return pres;
    }

    // One firmware reply holds one bank (both would exceed the reply size), so ask for each in turn.
    lf_xhorse_info_t nfo[2];
    uint8_t got = 0;
    for (uint8_t bank = 0; bank < 2; bank++) {
        lf_xhorse_rdopt_t opt = popt;
        opt.bank = bank;
        int res = xh_request_info(CMD_LF_XHORSE_DUMP, (uint8_t *)&opt, sizeof(opt), &nfo[bank]);
        if (res != PM3_SUCCESS) {
            if (bank == 0) {
                return res;
            }
            break;
        }
        if (nfo[bank].found == 0) {
            if (bank == 0) {
                xh_print_not_found();
                return PM3_ESOFT;
            }
            PrintAndLogEx(WARNING, "The chip did not answer the second session; bank 1 not read, try again");
            break;
        }
        if (nfo[bank].bank != bank) {
            PrintAndLogEx(WARNING, "Device firmware returned bank %u for bank %u; update the firmware", (unsigned)nfo[bank].bank, (unsigned)bank);
            break;
        }
        got++;
        if (nfo[bank].family == XHORSE_FAM_XT136) {
            xh136_print(&nfo[bank], "Xhorse XT136 dump");
            return PM3_SUCCESS;
        }
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Xhorse XT27A dump") " ----------------------");
    xh_print_header(&nfo[0]);

    // The banks side by side, bank 1 starting at column 33.  The unconfirmed mark is padded by hand
    // rather than with a field width, because its colour codes would count towards the width.
    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "bank 0 (emulated-tag image)%s", (got > 1) ? "      bank 1 (configuration and identity)" : "");
    PrintAndLogEx(INFO, "  page | data%s", (got > 1) ? "                      page | data" : "");
    PrintAndLogEx(INFO, "-------+-----------%s", (got > 1) ? "              -------+-----------" : "");
    for (uint8_t pg = 0; pg < nfo[0].image_pages; pg++) {
        char cell[2][24] = {{0}};
        for (uint8_t bank = 0; bank < got; bank++) {
            const uint8_t *d = &nfo[bank].image[pg * 4];
            snprintf(cell[bank], sizeof(cell[bank]), "0x%02X | %02X%02X%02X%02X", pg, d[0], d[1], d[2], d[3]);
        }
        if (got > 1) {
            PrintAndLogEx(INFO, "  %s%s                %s%s", cell[0], xh_page_mark(&nfo[0], pg), cell[1], xh_page_mark(&nfo[1], pg));
        } else {
            PrintAndLogEx(INFO, "  %s%s", cell[0], xh_page_mark(&nfo[0], pg));
        }
    }
    for (uint8_t bank = 0; bank < got; bank++) {
        xh_print_unconfirmed(&nfo[bank], nfo[bank].image_pages);
    }
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// read / write - raw channel-A page primitives
//-----------------------------------------------------------------------------
// Shared by both: send the request, wait, and sanity-check the reply.
static int xh_io_send(uint16_t cmd, const lf_xhorse_io_t *p, lf_xhorse_io_reply_t *out, int *status) {
    clearCommandBuffer();
    SendCommandNG(cmd, (uint8_t *)p, sizeof(*p));
    PacketResponseNG resp;
    // A write opens two sessions (~2.5 s each over four timings) and waits out a 13 ms program
    // cycle; a 64-page read reads every page at least twice.  30 s covers both with room to spare.
    if (WaitForResponseTimeout(cmd, &resp, 30000) == false) {
        PrintAndLogEx(WARNING, "timeout while waiting for reply");
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        return PM3_ETIMEOUT;
    }
    if (resp.length < sizeof(lf_xhorse_io_reply_t)) {
        PrintAndLogEx(FAILED, "short reply from device");
        return PM3_ESOFT;
    }
    memcpy(out, resp.data.asBytes, sizeof(*out));
    *status = resp.status;
    if (out->found == 0) {
        xh_print_not_found();
        return PM3_ESOFT;
    }
    return PM3_SUCCESS;
}

// Always two visible characters, like xh_page_mark, so a following column stays aligned.
static const char *xh_io_mark(const lf_xhorse_io_reply_t *r, uint8_t i) {
    return ((r->ok[i / 8] >> (i % 8)) & 1) ? "  " : " " _RED_("?");
}

// An XT136 has one flat page space: say so when a bank was asked for, so -b is never silently lost.
static void xh_io_note_family(const lf_xhorse_io_reply_t *r, int bank, bool bank_given) {
    if (r->family == XHORSE_FAM_XT136) {
        PrintAndLogEx(INFO, "Family.... " _GREEN_("XT136") " (no banks: one flat page space)");
        if (bank_given && bank != 0) {
            PrintAndLogEx(WARNING, "-b %d ignored: an XT136 has no banks", bank);
        }
    }
}

static int CmdLFXhorseRead(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse read",
                  "Read raw channel-A pages from an Xhorse super-chip.\n"
                  "\n"
                  "A page address is 6 bits, so 0x00..0x3F is the whole of a bank and there is no\n"
                  "page above it.  An XT27A has two banks, selected with -b: bank 0 is the emulated\n"
                  "transponder's image, bank 1 is configuration and identity (chip ID 0x33..0x36,\n"
                  "emulated type byte 0x3C, config words 0x3D..0x3E).  An XT136 has no banks and -b\n"
                  "is ignored on one.\n"
                  "\n"
                  "Every page is read until two reads agree; one marked " _RED_("?") " never did and may be wrong.",
                  "lf xhorse read -p 0                      -> bank-0 page 0x00\n"
                  "lf xhorse read -b 1 -p 0x3C              -> the emulated type byte\n"
                  "lf xhorse read -b 1 -p 0x33 -n 4         -> the 16-byte chip ID\n"
                  "lf xhorse read -p 0 -n 64                -> all of bank 0");
    void *argtable[] = {
        arg_param_begin,
        arg_int0("b", "bank",  "<dec>", "bank to read, 0..63 (def 0); ignored on an XT136"),
        arg_int1("p", "page",  "<dec>", "first page, 0..63"),
        arg_int0("n", "count", "<dec>", "pages to read (def 1)"),
        arg_param_end
    };
    // false: a bare `lf xhorse read` prints the help rather than quietly reading bank 0 page 0.
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int bank  = arg_get_int_def(ctx, 1, 0);
    int page  = arg_get_int_def(ctx, 2, 0);
    int count = arg_get_int_def(ctx, 3, 1);
    bool bank_given = (arg_get_int_def(ctx, 1, -1) != -1);
    CLIParserFree(ctx);

    if (bank < 0 || bank > 63) {
        PrintAndLogEx(FAILED, "bank must be 0..63 (the sel operand is 6 bits)");
        return PM3_EINVARG;
    }
    if (page < 0 || page > 63) {
        PrintAndLogEx(FAILED, "page must be 0..63 (the address field is 6 bits)");
        return PM3_EINVARG;
    }
    if (count < 1 || count > 64) {
        PrintAndLogEx(FAILED, "-n must be 1..64");
        return PM3_EINVARG;
    }
    if (page + count > 64) {
        PrintAndLogEx(FAILED, "page 0x%02X + %d pages runs past 0x3F, the last page of a bank", page, count);
        return PM3_EINVARG;
    }

    lf_xhorse_io_t p;
    memset(&p, 0, sizeof(p));
    p.bank = (uint8_t)bank;
    p.page = (uint8_t)page;
    p.count = (uint8_t)count;

    lf_xhorse_io_reply_t r;
    int status = PM3_SUCCESS;
    int res = xh_io_send(CMD_LF_XHORSE_READ, &p, &r, &status);
    if (res != PM3_SUCCESS) {
        return res;
    }

    PrintAndLogEx(NORMAL, "");
    xh_io_note_family(&r, bank, bank_given);
    if (status != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "sel bank %d was not echoed: the chip has no such bank, or the session faltered", bank);
        return PM3_ESOFT;
    }
    if (r.family == XHORSE_FAM_XT27A) {
        PrintAndLogEx(INFO, "bank %d", bank);
    }
    PrintAndLogEx(INFO, "  page | data");
    PrintAndLogEx(INFO, "-------+-----------");
    for (uint8_t i = 0; i < r.count; i++) {
        PrintAndLogEx(INFO, "  0x%02X | %s%s", (unsigned)(r.page + i), sprint_hex_inrow(&r.data[i * 4], 4), xh_io_mark(&r, i));
    }
    int bad = 0;
    for (uint8_t i = 0; i < r.count; i++) {
        bad += ((r.ok[i / 8] >> (i % 8)) & 1) ? 0 : 1;
    }
    if (bad) {
        PrintAndLogEx(WARNING, "%d page(s) marked " _RED_("?") " unconfirmed: no two reads agreed, so they may be wrong; try again", bad);
    }
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

static int CmdLFXhorseWrite(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse write",
                  "Write one raw channel-A page of an Xhorse super-chip and read it back.\n"
                  "\n"
                  "This writes whatever you give it with no regard for what the page means, so it\n"
                  "can leave the chip emulating nothing useful.  `lf xhorse reset` rewrites bank 0.\n"
                  "\n"
                  _RED_("Bank 1 holds pages that are dangerous to write:") "\n"
                  "  0x33..0x36  the 16-byte unique chip ID.  It IS writable, nothing else on the\n"
                  "              chip holds a copy, and overwriting it cannot be undone.  Read it\n"
                  "              first (`lf xhorse read -b 1 -p 0x33 -n 4`) and keep the value.\n"
                  "  0x3C        the emulated type byte.  A type that silences channel A (0x8C,\n"
                  "              0x48, 0x8A) needs `lf xhorse reset` to recover.\n"
                  "  0x3D..0x3F  config words; they ignore a WRITE.  Use -c, which writes them the\n"
                  "              only way that works: ctl 0x30+n (" _RED_("see -c below") ").\n"
                  "\n"
                  "An XT136 has no banks and -b is ignored on one.\n"
                  "\n"
                  _RED_("-c writes a config word, which can change what the chip emulates.") "  On an XT27A\n"
                  "`lf xhorse reset` can recover that; " _RED_("on an XT136 there is no reset recipe") ", and\n"
                  "config word 1's high byte is the suspected type field.  Read the word first and\n"
                  "keep the value.",
                  "lf xhorse write -p 0 -d 11223344             -> bank-0 page 0x00 (emulated UID)\n"
                  "lf xhorse write -b 1 -p 0x3C -d 00000036     -> set the emulated type to ID46\n"
                  "lf xhorse write -p 0x05 -d A5A5A5A5 --nv     -> write without the read-back verify\n"
                  "lf xhorse write -c 0 -d 0000000F             -> config word 0 via ctl 0x30");
    void *argtable[] = {
        arg_param_begin,
        arg_int0("b", "bank", "<dec>", "bank to write, 0..63 (def 0); ignored on an XT136"),
        arg_int0("p", "page", "<dec>", "page to write, 0..63"),
        arg_int0("c", "cfg",  "<dec>", "config word n to write via ctl 0x30+n, 0..2 (instead of -p)"),
        arg_str1("d", "data", "<hex>", "4 byte value to write"),
        arg_lit0(NULL, "nv",  "skip the read-back verify (the page is still read back and shown)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int bank = arg_get_int_def(ctx, 1, 0);
    int page = arg_get_int_def(ctx, 2, -1);
    int cfg = arg_get_int_def(ctx, 3, -1);
    uint8_t data[4] = {0};
    int dlen = 0;
    CLIGetHexWithReturn(ctx, 4, data, &dlen);
    bool nv = arg_get_lit(ctx, 5);
    bool bank_given = (arg_get_int_def(ctx, 1, -1) != -1);
    CLIParserFree(ctx);

    if (dlen != 4) {
        PrintAndLogEx(FAILED, "-d takes exactly 4 bytes, got %d", dlen);
        return PM3_EINVARG;
    }
    // Exactly one target: -p is not defaulted to page 0, and -c is a different address space.
    if ((page == -1) == (cfg == -1)) {
        PrintAndLogEx(FAILED, "give exactly one of -p (a page) or -c (a config word), %s",
                      (page == -1) ? "neither was given" : "both were given");
        return PM3_EINVARG;
    }
    if (bank < 0 || bank > 63) {
        PrintAndLogEx(FAILED, "bank must be 0..63 (the sel operand is 6 bits)");
        return PM3_EINVARG;
    }
    if (cfg != -1 && (cfg < 0 || cfg > 2)) {
        PrintAndLogEx(FAILED, "config word must be 0..2 (ctl 0x30..0x32)");
        return PM3_EINVARG;
    }
    if (page != -1 && (page < 0 || page > 63)) {
        PrintAndLogEx(FAILED, "page must be 0..63 (the address field is 6 bits)");
        return PM3_EINVARG;
    }

    lf_xhorse_io_t p;
    memset(&p, 0, sizeof(p));
    p.bank = (uint8_t)bank;
    p.cfg = (cfg != -1) ? 1 : 0;
    p.page = (uint8_t)((cfg != -1) ? cfg : page);
    p.verify = nv ? 0 : 1;
    p.value = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];

    lf_xhorse_io_reply_t r;
    int status = PM3_SUCCESS;
    int res = xh_io_send(CMD_LF_XHORSE_WRITE, &p, &r, &status);
    if (res != PM3_SUCCESS) {
        return res;
    }

    PrintAndLogEx(NORMAL, "");
    xh_io_note_family(&r, bank, bank_given);
    if (r.count == 0) {
        PrintAndLogEx(FAILED, "sel bank %d was not echoed; nothing was written", bank);
        return PM3_ESOFT;
    }

    if (cfg != -1) {
        // r.page is where the word reads back from: 0x0D+n on an XT136, bank-1 0x3D+n on an XT27A.
        PrintAndLogEx(INFO, "wrote config word %d (ctl 0x%02X) = %s", cfg, 0x30 + cfg, sprint_hex_inrow(data, 4));
        PrintAndLogEx(INFO, "page 0x%02X reads back %s%s", r.page, sprint_hex_inrow(&r.data[0], 4), xh_io_mark(&r, 0));
    } else if (r.family == XHORSE_FAM_XT136) {
        PrintAndLogEx(INFO, "wrote page 0x%02X = %s", page, sprint_hex_inrow(data, 4));
        PrintAndLogEx(INFO, "reads back      %s%s", sprint_hex_inrow(&r.data[0], 4), xh_io_mark(&r, 0));
    } else {
        PrintAndLogEx(INFO, "wrote bank %d page 0x%02X = %s", bank, page, sprint_hex_inrow(data, 4));
        PrintAndLogEx(INFO, "reads back      %s%s", sprint_hex_inrow(&r.data[0], 4), xh_io_mark(&r, 0));
    }

    if (memcmp(r.data, data, 4) == 0) {
        PrintAndLogEx(SUCCESS, "write " _GREEN_("ok"));
    } else if (status == PM3_SUCCESS) {
        // The firmware's verify saw the value but our read-back does not: a weak reply, or the
        // page changed again.  Report it rather than claiming success.
        PrintAndLogEx(WARNING, "verify passed on-chip but the read-back differs; try reading the page again");
    } else {
        PrintAndLogEx(FAILED, "write " _RED_("did not take") " - the page still reads %s", sprint_hex_inrow(&r.data[0], 4));
        if (cfg == -1) {
            PrintAndLogEx(HINT, "config words ignore a WRITE; write them with -c (ctl 0x30+n)");
        }
        return PM3_ESOFT;
    }
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// shared send-and-wait for a mutating command
//-----------------------------------------------------------------------------
// p may be NULL for a command without a payload (reset).
static int xh_send_wait(uint16_t cmd, const lf_xhorse_data_t *p, const char *what) {
    clearCommandBuffer();
    SendCommandNG(cmd, (uint8_t *)p, p ? sizeof(lf_xhorse_data_t) : 0);
    PacketResponseNG resp;
    // Configure/reset open two sessions; each can spend ~2.5 s on handshake retries (12 attempts
    // over four timings), the first may add three ~2.3 s EM-recovery attempts, and the ID8A image
    // takes a few seconds to write and verify: roughly 16-20 s in the worst case.  Allow generous time.
    if (WaitForResponseTimeout(cmd, &resp, 30000) == false) {
        PrintAndLogEx(WARNING, "timeout while waiting for reply");
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        return PM3_ETIMEOUT;
    }
    if (resp.status == PM3_ENOTIMPL) {
        PrintAndLogEx(FAILED, "Found an " _YELLOW_("XT136") "; %s is not supported on it yet, nothing was written", what);
        PrintAndLogEx(HINT, "`" _YELLOW_("lf xhorse info") "` and `" _YELLOW_("lf xhorse dump") "` read it");
        return PM3_ENOTIMPL;
    }
    if (resp.status == PM3_EINVARG) {
        // The firmware refuses the type before it opens a session, so the chip is untouched.
        PrintAndLogEx(FAILED, "Found an " _YELLOW_("XT136") ", which cannot emulate that type; nothing was written");
        PrintAndLogEx(HINT, "an XT136 takes " _YELLOW_("ID46") " / " _YELLOW_("7936") " or " _YELLOW_("7946") " with a 4 byte UID - the only two its own app offers");
        return PM3_EINVARG;
    }
    if (resp.status != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "%s failed on-chip (status %d) - read-back verify mismatch or no tag", what, resp.status);
        return PM3_ESOFT;
    }
    PrintAndLogEx(SUCCESS, "%s " _GREEN_("ok"), what);
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// configure
//-----------------------------------------------------------------------------
static int CmdLFXhorseConfigure(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse configure",
                  "Configure which transponder an Xhorse super chip emulates.  -t is a personality\n"
                  "type (see `lf xhorse types`); -u sets the emulated UID and is required for\n"
                  "every type that takes one.\n"
                  "\n"
                  "An " _YELLOW_("XT136") " takes only " _YELLOW_("ID46") "/" _YELLOW_("7936") " and " _YELLOW_("7946") ", the two its own app offers; any other\n"
                  "type is refused before anything is written.  `lf xhorse reset` undoes a set.",
                  "lf xhorse configure -t ID46 -u 11223344     -> HITAG2, 4-byte UID\n"
                  "lf xhorse configure -t ID4D                 -> Texas Crypto, no UID\n"
                  "lf xhorse configure -t ID13 -u 20471BB01B   -> EM4102, 5-byte UID\n"
                  "lf xhorse configure -t 7939 -u 11223344     -> PCF7939, 4-byte UID\n"
                  "lf xhorse configure -t ID48 -u 11223344     -> Megamos, 4-byte UID\n"
                  "lf xhorse configure -t ID4C -u D3DA834C7E39EE98\n"
                  "lf xhorse configure -t 7946 -u DE269A2F     -> PCF7946 (XT27A or XT136)");

    void *argtable[] = {
        arg_param_begin,
        arg_str1("t", "type", "<name>", "emulated-type name, e.g. ID46 / 7936 / ID13"),
        arg_str0("u", "uid",  "<hex>",   "emulated UID, required where the type takes one (length depends on type)"),
        arg_lit0(NULL, "noverify", "do not read each write back to verify"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    char typestr[32] = {0};
    int tlen = 0;
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)typestr, sizeof(typestr) - 1, &tlen);

    uint8_t uid[XHORSE_UID_MAXLEN] = {0};
    int uidlen = 0;
    CLIGetHexWithReturn(ctx, 2, uid, &uidlen);

    bool noverify = arg_get_lit(ctx, 3);
    CLIParserFree(ctx);

    const char *reason = xh_refused(typestr);   // exact match on the refused set (upper not needed, we compare raw)
    if (reason == NULL) {
        // also catch lower-case / 0x forms of refused tokens
        char up[32];
        size_t n = 0;
        for (const char *s = typestr; *s && n < sizeof(up) - 1; s++) {
            up[n++] = (char)toupper((unsigned char) * s);
        }
        up[n] = '\0';
        reason = xh_refused(up);
    }
    if (reason) {
        PrintAndLogEx(FAILED, "%s", reason);
        PrintAndLogEx(HINT, "run `" _YELLOW_("lf xhorse types") "` for the supported list");
        return PM3_EINVARG;
    }

    const xh_type_t *t = resolve_type(typestr);
    if (t == NULL) {
        PrintAndLogEx(FAILED, "unknown type token " _YELLOW_("%s"), typestr);
        PrintAndLogEx(HINT, "run `" _YELLOW_("lf xhorse types") "` for the supported list");
        return PM3_EINVARG;
    }

    // UID validation per the type's rule
    switch (t->uid) {
        case UID_NONE:
            if (uidlen > 0) {
                PrintAndLogEx(FAILED, "%s takes no UID", t->canon);
                return PM3_EINVARG;
            }
            break;
        case UID_REQ4:
            if (uidlen != 4) {
                PrintAndLogEx(FAILED, "%s needs a 4-byte UID (got %d)", t->canon, uidlen);
                return PM3_EINVARG;
            }
            break;
        case UID_REQ5:
            if (uidlen != 5) {
                PrintAndLogEx(FAILED, "%s needs a 5-byte UID (got %d)", t->canon, uidlen);
                return PM3_EINVARG;
            }
            break;
        case UID_REQ8:
            if (uidlen != 8) {
                PrintAndLogEx(FAILED, "%s needs an 8-byte UID (got %d)", t->canon, uidlen);
                return PM3_EINVARG;
            }
            break;
    }

    lf_xhorse_data_t p;
    memset(&p, 0, sizeof(p));
    p.recipe   = t->recipe;
    p.typebyte = t->typebyte;
    p.has_type = t->has_type ? 1 : 0;
    p.typeword = t->typeword ? 1 : 0;
    p.uidlen   = (uint8_t)uidlen;
    p.aux      = t->aux;
    p.verify   = noverify ? 0 : 1;
    memcpy(p.uid, uid, sizeof(p.uid));

    // A HITAG2 reader names the exact PCF part from bits 4..7 of the UID (getHitagTypeStr() in
    // cmdlfhitag.c; the VVDI's own "specific chip" field is the same nibble, docs/xt136.md SS5a).
    // Both VVDI set captures picked a UID whose nibble matches the type, so a mismatch is almost
    // certainly a mistake - but it is the caller's UID, so warn rather than refuse or rewrite it.
    if ((t->recipe == XHORSE_R_HITAG2) && (uidlen == 4)) {
        uint8_t want = (t->typebyte == 0x36) ? 1 : (t->typebyte == 0x46) ? 2 : 0;
        uint8_t have = (uid[3] >> 4) & 0x0F;
        if (want && (have != want)) {
            PrintAndLogEx(WARNING, "UID nibble is %u, but %s is read from a UID whose nibble is %u;",
                          have, t->canon, want);
            PrintAndLogEx(WARNING, "      a reader will name the chip from the UID, not from the type byte.");
        }
    }

    if (t->recipe == XHORSE_R_ID8E) {
        PrintAndLogEx(WARNING, "ID8E: only the UID store (pages 0x0A/0x0B) + type byte are written;");
        PrintAndLogEx(WARNING, "      the emulation block at 0x00..0x04 is not yet fully derived.");
    }

    PrintAndLogEx(INFO, "configuring " _GREEN_("%s") " (on-chip 0x%02X)%s",
                  t->canon, t->typebyte, p.verify ? ", verify" : "");
    return xh_send_wait(CMD_LF_XHORSE_CONFIGURE, &p, "configure");
}

//-----------------------------------------------------------------------------
// reset
//-----------------------------------------------------------------------------
static int CmdLFXhorseReset(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse reset",
                  "Restore an Xhorse super chip to its factory-default ID46/HITAG2 personality.\n"
                  "Does not blank the chip.\n"
                  "\n"
                  "XT27A: type byte 0x36 + the default UID/password/config.  Also recovers a chip\n"
                  "       stuck emulating an EM type (ID11/12/13), ID48, ID8E or ID8A, which never\n"
                  "       answers the normal handshake.\n"
                  "XT136: the VVDI's fixed reset image - config words 0x0D-0x0F then pages\n"
                  "       0x00-0x0C, UID 64B55D12.  There is no EM-style recovery for an XT136:\n"
                  "       this only works while the chip still answers channel A.",
                  "lf xhorse reset");
    void *argtable[] = { arg_param_begin, arg_param_end };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);

    return xh_send_wait(CMD_LF_XHORSE_RESET, NULL, "reset to default ID46");
}

//-----------------------------------------------------------------------------
// xt57 nonce
//-----------------------------------------------------------------------------
// The 11 nonces seen from a VVDI mini are windows of one tag-independent bit stream (doc/xt57-auth.md);
// this is the 68 contiguous bits recovered so far, used to print the window offset of each nonce.
static const char xt57_stream[] = "00101100111111100001001100101110011111001"
                                  "1011011100110101110100011" "00"
                                  "011";   // +3: 6DCD7463, the reset session's nonce

typedef struct {
    uint32_t nonce, answer, rhi, rlo;
} xt57_known_t;

static const xt57_known_t xt57_known[] = {
#define X(n, a, h, l) { n, a, h, l },
    XHORSE_XT57_KNOWN
#undef X
};

static int xt57_offset(uint32_t nonce) {
    char pat[33];
    for (int i = 0; i < 32; i++) {
        pat[i] = ((nonce >> (31 - i)) & 1) ? '1' : '0';
    }
    pat[32] = '\0';
    const char *p = strstr(xt57_stream, pat);
    return p ? (int)(p - xt57_stream) : -1;
}

static const char *const xt57_sweep_names[] = { "powerup", "ackid", "idsel", "nonans", "off", "none" };
static const char *const xt57_sweep_units[] = { "us", "T0", "T0", "T0", "us", "" };
static const int32_t xt57_sweep_defs[] = { 120140, 88, 82, 144, 20000, 0 };

static uint32_t xt57_be32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

// CRC-8, poly 0x07, init 0, no reflection, no final xor - over the ON-AIR (encrypted) bytes.
// A tag response's CRC covers the reader command bytes that provoked it (doc/xt57-auth.md SS7.1).
static uint8_t xt57_crc8(const uint8_t *d, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
        }
    }
    return c;
}

// A parsed --post frame, kept client side so the printer can show what was sent.
#define XT57_MAXFRAMES 220
typedef struct {
    uint8_t  bits;
    uint8_t  data[XHORSE_XT57_POSTBYTES];
    uint16_t gap_t0;
} xt57_frame_t;

// Pack the parsed frames into the command's script[] byte stream.  Returns the bytes used, or -1
// if they do not fit.
static int xt57_pack_script(const xt57_frame_t *f, int n, uint8_t *out, size_t outlen) {
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        uint8_t nb = (uint8_t)((f[i].bits + 7) / 8);
        size_t need = 1 + nb + (f[i].gap_t0 ? 2 : 0);
        if (at + need > outlen) {
            return -1;
        }
        if (f[i].gap_t0) {
            out[at++] = (uint8_t)(0x80 | f[i].bits);
            out[at++] = (uint8_t)(f[i].gap_t0 & 0xFF);
            out[at++] = (uint8_t)(f[i].gap_t0 >> 8);
        } else {
            out[at++] = f[i].bits;
        }
        memcpy(out + at, f[i].data, nb);
        at += nb;
    }
    return (int)at;
}

// Run one scripted post-auth session and print every frame with its response.
static int xt57_post_session(lf_xhorse_xt57_t *p, const xt57_frame_t *frames, int nframes, int tries) {
    p->count = (uint8_t)((tries < 1) ? 1 : (tries > 255 ? 255 : tries));

    clearCommandBuffer();
    SendCommandNG(CMD_LF_XHORSE_XT57_NONCE, (uint8_t *)p, sizeof(*p));
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_LF_XHORSE_XT57_NONCE, &resp, 120000) == false) {
        PrintAndLogEx(WARNING, "timeout while waiting for reply");
        return PM3_ETIMEOUT;
    }
    if (resp.length < sizeof(lf_xhorse_xt57_post_reply_t)) {
        PrintAndLogEx(FAILED, "short reply from device (%u bytes)", resp.length);
        return PM3_ESOFT;
    }
    const lf_xhorse_xt57_post_reply_t *r = (const lf_xhorse_xt57_post_reply_t *)resp.data.asBytes;

    uint32_t nonce = xt57_be32(r->rec.nonce);
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "sessions tried... %u", r->sessions);
    PrintAndLogEx(INFO, "nonce............ " _YELLOW_("%08X") "  (%u bits)", nonce, r->rec.nonce_bits);
    if (r->rec.flags & XT57_F_ANSWER) {
        PrintAndLogEx(INFO, "answer sent...... %08X", xt57_be32(r->rec.answer));
    }
    PrintAndLogEx(INFO, "reply............ %08X%08X  (%u bits)",
                  xt57_be32(r->rec.reply), xt57_be32(r->rec.reply + 4), r->rec.reply_bits);

    if (r->npost == 0) {
        PrintAndLogEx(FAILED, "the wanted nonce never came up - no frame was sent");
        return PM3_EFAILED;
    }

    uint8_t rfirst = r->rfirst ? r->rfirst : 1;
    if (r->nsent < nframes) {
        PrintAndLogEx(INFO, "frames sent...... %u of %d", r->nsent, nframes);
    }
    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, " idx  sent                 resp bits  data       crc");
    PrintAndLogEx(INFO, " ---  -------------------  ---------  ---------  ---");
    for (uint8_t i = 0; i < r->npost; i++) {
        int fi = rfirst - 1 + i;
        if (fi >= nframes) {
            break;
        }
        const xt57_frame_t *f = &frames[fi];
        const xt57_presp_t *q = &r->presp[i];

        char sent[2 * XHORSE_XT57_POSTBYTES + 8] = {0};
        for (int b = 0; b < (f->bits + 7) / 8; b++) {
            snprintf(sent + strlen(sent), 4, "%02X", f->data[b]);
        }

        if (q->bits == 0) {
            PrintAndLogEx(INFO, " %3d  %-19s  " _RED_("no answer"), fi + 1, sent);
            continue;
        }

        int nbytes = q->bits / 8;
        char got[2 * XHORSE_XT57_RESPBYTES + 8] = {0};
        for (int b = 0; b < nbytes && b < XHORSE_XT57_RESPBYTES; b++) {
            snprintf(got + strlen(got), 4, "%02X", q->data[b]);
        }

        // check the CRC where the frame has the shape of a data response: command bytes + data + crc
        const char *crcs = "-";
        if ((q->bits == 40) || (q->bits == 16)) {
            uint8_t buf[XHORSE_XT57_POSTBYTES + XHORSE_XT57_RESPBYTES];
            int n = 0;
            for (int b = 0; b < (f->bits + 7) / 8; b++) {
                buf[n++] = f->data[b];
            }
            for (int b = 0; b < nbytes - 1; b++) {
                buf[n++] = q->data[b];
            }
            crcs = (xt57_crc8(buf, n) == q->data[nbytes - 1]) ? _GREEN_("ok") : _RED_("BAD");
        }
        PrintAndLogEx(INFO, " %3d  %-19s  %3u bits   %-9s  %s", fi + 1, sent, q->bits, got, crcs);
    }
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

static int CmdLFXhorseXt57Nonce(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse xt57 nonce",
                  "Run the XT57B XMICRO session over and over and log the 32-bit nonce each one returns.\n"
                  "One gap is stepped each time (--sweep) so that the timing that selects the nonce can be found.\n"
                  "A nonce that is a known one is answered with the recorded answer, which also records the\n"
                  "64-bit reply; use --answer to always send one value or --noanswer to leave it.\n"
                  "Gaps (VVDI): powerup = field on -> XMICRO 120140 us, ackid = 0x5AA5 -> 0011 88 T0,\n"
                  "idsel = XT57X -> select 82 T0, nonans = nonce -> answer 144 T0, off = field off 20000 us.\n"
                  "T0 = 8 us, counted from the last tag edge.  Press Enter to stop.",
                  "lf xhorse xt57 nonce -n 100                           -> 100 sessions, default timing\n"
                  "lf xhorse xt57 nonce --sweep powerup --step 8 -n 200   -> field-on delay 120140, 120148, ...\n"
                  "lf xhorse xt57 nonce --sweep idsel --base 70 --step 1 -n 40\n"
                  "lf xhorse xt57 nonce --noanswer -f nonces.csv -n 500\n"
                  "lf xhorse xt57 nonce --noanswer --select FF58543537 --selbits 40   -> a 40 bit select frame\n"
                  "\n"
                  "By default the select carries the family bytes the chip itself reports to the 0011 identify,\n"
                  "so an XT37 needs no --select; a chip refuses a select carrying another family (docs/xt37.md).");
    void *argtable[] = {
        arg_param_begin,
        arg_int0("n", "count", "<dec>", "number of sessions (def 32)"),
        arg_str0(NULL, "sweep", "<name>", "gap to step: powerup, ackid, idsel, nonans, off or none (def none)"),
        arg_int0(NULL, "base", "<dec>", "swept gap for session 0, us or T0 (def: the VVDI's)"),
        arg_int0(NULL, "step", "<dec>", "added to the swept gap each session (def 1)"),
        arg_str0(NULL, "select", "<hex>", "13 byte select frame, sent verbatim (def: C0 + the chip's own family + 123456789ABCDEF0)"),
        arg_str0(NULL, "answer", "<hex>", "always send this 32-bit answer"),
        arg_lit0(NULL, "noanswer", "do not answer, only record nonces"),
        arg_str0("f", "file", "<fn>", "also write a CSV of the sessions"),
        arg_int0(NULL, "off", "<dec>", "field-off time before each session in us (def 20000; the chip keeps its nonce state through ~33000)"),
        arg_str0(NULL, "post", "<script>", "frames to send after the auth: bits:hex[:gap],... (gap in T0)"),
        arg_str0(NULL, "want", "<hex>", "with --post: only script the session whose nonce is this"),
        arg_int0(NULL, "rfirst", "<dec>", "with --post: 1-based index of the first response to report"),
        arg_int0(NULL, "powerup", "<dec>", "field on -> XMICRO in us, when another gap is swept"),
        arg_lit0(NULL, "presson", "select even if the 0011 identify went unanswered"),
        arg_int0(NULL, "selbits", "<dec>", "bits of the select frame to send (def 104 = 13 bytes)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int total = arg_get_int_def(ctx, 1, 32);

    char sweepstr[16] = {0};
    int slen = 0;
    CLIParamStrToBuf(arg_get_str(ctx, 2), (uint8_t *)sweepstr, sizeof(sweepstr) - 1, &slen);
    int sweep = XT57_SWEEP_NONE;
    if (slen > 0) {
        sweep = -1;
        for (int i = 0; i < (int)ARRAYLEN(xt57_sweep_names); i++) {
            if (strcmp(sweepstr, xt57_sweep_names[i]) == 0) {
                sweep = i;
            }
        }
        if (sweep < 0) {
            PrintAndLogEx(FAILED, "unknown sweep '%s' (powerup, ackid, idsel, nonans, off, none)", sweepstr);
            CLIParserFree(ctx);
            return PM3_EINVARG;
        }
    }
    int base = arg_get_int_def(ctx, 3, xt57_sweep_defs[sweep]);
    int step = arg_get_int_def(ctx, 4, (sweep == XT57_SWEEP_NONE) ? 0 : 1);

    uint8_t sel[XHORSE_XT57_SELLEN] = { 0xC0, 'X', 'T', '5', '7', 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 };
    int sellen = 0;
    uint8_t selin[XHORSE_XT57_SELLEN + 1] = {0};
    CLIGetHexWithReturn(ctx, 5, selin, &sellen);
    int selbits = arg_get_int_def(ctx, 15, 0);
    if (selbits < 0 || selbits > XHORSE_XT57_SELLEN * 8) {
        PrintAndLogEx(FAILED, "selbits must be 0..%d", XHORSE_XT57_SELLEN * 8);
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if (sellen > 0) {
        if (sellen > XHORSE_XT57_SELLEN || (selbits == 0 && sellen != XHORSE_XT57_SELLEN)) {
            PrintAndLogEx(FAILED, "select must be %d bytes (got %d); a shorter frame needs --selbits",
                          XHORSE_XT57_SELLEN, sellen);
            CLIParserFree(ctx);
            return PM3_EINVARG;
        }
        if (selbits > sellen * 8) {
            PrintAndLogEx(FAILED, "--selbits %d needs %d select bytes (got %d)",
                          selbits, (selbits + 7) / 8, sellen);
            CLIParserFree(ctx);
            return PM3_EINVARG;
        }
        memset(sel, 0, sizeof(sel));
        memcpy(sel, selin, sellen);
    }

    uint8_t ansb[5] = {0};
    int anslen = 0;
    CLIGetHexWithReturn(ctx, 6, ansb, &anslen);
    if (anslen > 0 && anslen != 4) {
        PrintAndLogEx(FAILED, "answer must be 4 bytes (got %d)", anslen);
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    bool noanswer = arg_get_lit(ctx, 7);

    char fname[FILE_PATH_SIZE] = {0};
    int fnlen = 0;
    CLIParamStrToBuf(arg_get_str(ctx, 8), (uint8_t *)fname, FILE_PATH_SIZE - 1, &fnlen);
    int off_us = arg_get_int_def(ctx, 9, 0);

    char poststr[1024] = {0};
    int postlen = 0;
    CLIParamStrToBuf(arg_get_str(ctx, 10), (uint8_t *)poststr, sizeof(poststr) - 1, &postlen);

    uint8_t wantb[5] = {0};
    int wantlen = 0;
    CLIGetHexWithReturn(ctx, 11, wantb, &wantlen);
    int rfirst = arg_get_int_def(ctx, 12, 0);
    int powerup = arg_get_int_def(ctx, 13, 0);
    bool presson = arg_get_lit(ctx, 14);
    if (wantlen > 0 && wantlen != 4) {
        PrintAndLogEx(FAILED, "want must be 4 bytes (got %d)", wantlen);
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    CLIParserFree(ctx);

    // Parse the post script: comma-separated bits:hex[:gap].  The hex is the frame MSB-aligned, so
    // a 48-bit write is 6 bytes and an 8-bit trigger is 1.
    static xt57_frame_t post[XT57_MAXFRAMES];
    memset(post, 0, sizeof(post));
    int npost = 0;
    if (postlen > 0) {
        char *save = NULL;
        for (char *tok = strtok_r(poststr, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (npost >= XT57_MAXFRAMES) {
                PrintAndLogEx(FAILED, "at most %d scripted frames", XT57_MAXFRAMES);
                return PM3_EINVARG;
            }
            int bits = 0, gap = 0;
            char hexs[32] = {0};
            int n = sscanf(tok, "%d:%31[0-9a-fA-F]:%d", &bits, hexs, &gap);
            if (n < 2) {
                PrintAndLogEx(FAILED, "bad frame '%s' - want bits:hex[:gap]", tok);
                return PM3_EINVARG;
            }
            size_t hl = strlen(hexs);
            if (bits <= 0 || bits > XHORSE_XT57_POSTBYTES * 8 || hl != (size_t)((bits + 7) / 8) * 2) {
                PrintAndLogEx(FAILED, "frame '%s': %d bits needs %d hex digits", tok, bits, (bits + 7) / 8 * 2);
                return PM3_EINVARG;
            }
            for (size_t b = 0; b < hl / 2; b++) {
                unsigned v = 0;
                if (sscanf(hexs + b * 2, "%2x", &v) != 1) {
                    PrintAndLogEx(FAILED, "frame '%s': bad hex", tok);
                    return PM3_EINVARG;
                }
                post[npost].data[b] = (uint8_t)v;
            }
            post[npost].bits = (uint8_t)bits;
            post[npost].gap_t0 = (uint16_t)gap;
            npost++;
        }
    }

    FILE *csv = NULL;
    if (fnlen > 0) {
        csv = fopen(fname, "w");
        if (csv == NULL) {
            PrintAndLogEx(FAILED, "cannot open %s", fname);
            return PM3_EFILE;
        }
        fprintf(csv, "session,%s,flags,ident,nonce_bits,nonce,answer,reply_bits,reply,check\n", xt57_sweep_names[sweep]);
    }

    lf_xhorse_xt57_t p;
    memset(&p, 0, sizeof(p));
    p.sweep = (uint8_t)sweep;
    p.answer_mode = noanswer ? XT57_ANS_NONE : ((anslen == 4) ? XT57_ANS_FIXED : XT57_ANS_KNOWN);
    p.answer = (anslen == 4) ? xt57_be32(ansb) : 0;
    memcpy(p.select, sel, sizeof(sel));
    p.off_us = (off_us > 0) ? (uint32_t)off_us : 0;
    p.npost = (uint8_t)((npost > 255) ? 255 : npost);
    p.want = (wantlen == 4) ? xt57_be32(wantb) : 0;
    p.rfirst = (uint8_t)rfirst;
    p.powerup_us = (uint32_t)powerup;
    p.press_on = presson ? 1 : 0;
    // With no --select, let the firmware fill the family bytes in from the chip's own identify
    // reply: a chip answers no nonce at all to a select carrying another family's bytes
    // (docs/xt37.md SS3).  An explicit --select is sent exactly as given.
    p.sel_auto = (sellen == 0) ? 1 : 0;
    p.selbits = (uint8_t)selbits;

    if (npost > 0) {
        int packed = xt57_pack_script(post, npost, p.script, sizeof(p.script));
        if (packed < 0) {
            PrintAndLogEx(FAILED, "script too long: %d frames do not fit in %d bytes",
                          npost, (int)sizeof(p.script));
            if (csv) {
                fclose(csv);
            }
            return PM3_EINVARG;
        }
        p.scriptlen = (uint16_t)packed;
        if (p.rfirst == 0 && npost > XHORSE_XT57_MAXPOST) {
            p.rfirst = (uint8_t)(npost - XHORSE_XT57_MAXPOST + 1);
            PrintAndLogEx(INFO, "only the last %d responses fit; use --rfirst to pick a window",
                          XHORSE_XT57_MAXPOST);
        }
        p.base = base;
        p.step = step;
        int rc = xt57_post_session(&p, post, npost, total);
        if (csv) {
            fclose(csv);
        }
        return rc;
    }

    PrintAndLogEx(INFO, "%d sessions, sweep " _YELLOW_("%s") " from %d step %d %s.  Press " _GREEN_("Enter") " to stop",
                  total, xt57_sweep_names[sweep], base, step, xt57_sweep_units[sweep]);
    // flags: A = XMICRO ack, I = identify named a family, C = that reply's CRC validated,
    // F = the select's family bytes came from it, N = a nonce came back.
    PrintAndLogEx(INFO, "  #  %-9s flags nonce     off  answer    reply             check",
                  (sweep == XT57_SWEEP_NONE) ? "" : xt57_sweep_names[sweep]);

    uint32_t seen[512];
    uint16_t seen_n[512];
    int nseen = 0, got = 0, done = 0, mismatch = 0, noack = 0;
    int res = PM3_SUCCESS;

    while (done < total) {
        int batch = total - done;
        if (batch > XHORSE_XT57_MAXSESS) batch = XHORSE_XT57_MAXSESS;
        p.count = (uint8_t)batch;
        p.base = base + step * done;
        p.step = step;

        clearCommandBuffer();
        SendCommandNG(CMD_LF_XHORSE_XT57_NONCE, (uint8_t *)&p, sizeof(p));
        PacketResponseNG resp;
        // a session is ~0.2 s; allow a lot more than the batch needs
        if (WaitForResponseTimeout(CMD_LF_XHORSE_XT57_NONCE, &resp, 15000 + 600 * batch) == false) {
            PrintAndLogEx(WARNING, "timeout while waiting for reply");
            SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
            res = PM3_ETIMEOUT;
            break;
        }
        if (resp.status != PM3_SUCCESS || resp.length < sizeof(lf_xhorse_xt57_reply_t)) {
            PrintAndLogEx(FAILED, "device error (status %d, %u bytes)", resp.status, (unsigned)resp.length);
            res = PM3_ESOFT;
            break;
        }
        const lf_xhorse_xt57_reply_t *rp = (const lf_xhorse_xt57_reply_t *)resp.data.asBytes;

        for (int i = 0; i < rp->count && i < batch; i++) {
            const lf_xhorse_xt57_rec_t *r = &rp->rec[i];
            int idx = done + i;
            int32_t val = (int32_t)(base + (int64_t)step * idx);

            uint32_t nonce = xt57_be32(r->nonce);
            char nstr[16] = "", ostr[16] = "", astr[16] = "", rstr[24] = "", cstr[40] = "";

            if (r->flags & XT57_F_NONCE) {
                got++;
                snprintf(nstr, sizeof(nstr), "%08X", nonce);
                int off = xt57_offset(nonce);
                if (off < 0) {
                    snprintf(ostr, sizeof(ostr), "new");
                } else {
                    snprintf(ostr, sizeof(ostr), "%d", off);
                }
                int k;
                for (k = 0; k < nseen; k++) {
                    if (seen[k] == nonce) break;
                }
                if (k == nseen && nseen < (int)ARRAYLEN(seen)) {
                    seen[nseen] = nonce;
                    seen_n[nseen++] = 1;
                } else if (k < nseen) {
                    seen_n[k]++;
                }
            } else if ((r->flags & XT57_F_ACK) == 0) {
                noack++;
                if (r->ack[0] || r->ack[1]) {
                    snprintf(cstr, sizeof(cstr), "no XMICRO ack (got %02X%02X)", r->ack[0], r->ack[1]);
                } else {
                    snprintf(cstr, sizeof(cstr), "no XMICRO ack");
                }
            } else if ((r->flags & XT57_F_IDENT) == 0) {
                snprintf(cstr, sizeof(cstr), "ack %02X%02X, no family reply: %s",
                         r->ack[0], r->ack[1], sprint_hex_inrow(r->ident, 5));
            } else if ((r->flags & XT57_F_IDCRC) == 0) {
                snprintf(cstr, sizeof(cstr), "identify CRC bad: %s", sprint_hex_inrow(r->ident, 5));
            } else {
                snprintf(cstr, sizeof(cstr), "no nonce (%u bits)", r->nonce_bits);
            }

            if (r->flags & XT57_F_ANSWER) {
                snprintf(astr, sizeof(astr), "%08X", xt57_be32(r->answer));
                snprintf(rstr, sizeof(rstr), "%s", sprint_hex_inrow(r->reply, 8));
                if (r->reply_bits < 64) {
                    snprintf(cstr, sizeof(cstr), "reply %u bits", r->reply_bits);
                } else {
                    uint32_t hi = xt57_be32(r->reply);
                    uint32_t lo = xt57_be32(r->reply + 4);
                    const xt57_known_t *kn = NULL;
                    for (size_t k = 0; k < ARRAYLEN(xt57_known); k++) {
                        if (xt57_known[k].nonce == nonce) kn = &xt57_known[k];
                    }
                    if (kn == NULL) {
                        snprintf(cstr, sizeof(cstr), "answered, reply unknown");
                    } else if (kn->rhi == hi && kn->rlo == lo) {
                        snprintf(cstr, sizeof(cstr), "reply matches");
                    } else {
                        snprintf(cstr, sizeof(cstr), "REPLY DIFFERS");
                        mismatch++;
                    }
                }
            }

            PrintAndLogEx(INFO, "%3d %9d  %c%c%c%c%c %-9s %-4s %-9s %-17s %s", idx + 1, val,
                          (r->flags & XT57_F_ACK) ? 'A' : '-', (r->flags & XT57_F_IDENT) ? 'I' : '-',
                          (r->flags & XT57_F_IDCRC) ? 'C' : '-', (r->flags & XT57_F_SELFAM) ? 'F' : '-',
                          (r->flags & XT57_F_NONCE) ? 'N' : '-', nstr, ostr, astr, rstr, cstr);
            if (csv) {
                fprintf(csv, "%d,%d,%02X,%s,%u,%s,%s,%u,%s,%s\n", idx + 1, val, r->flags, sprint_hex_inrow(r->ident, 5),
                        r->nonce_bits, nstr, astr, r->reply_bits, rstr, cstr);
            }
        }
        done += rp->count;
        if (rp->count < batch) break;                 // the device stopped early (button / host data)
        if (kbd_enter_pressed()) {
            SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
            PrintAndLogEx(INFO, "aborted");
            break;
        }
    }
    if (csv) fclose(csv);

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, "%d sessions, %d nonces (%d distinct), %d without a nonce (%d without an XMICRO ack), %d reply mismatches",
                  done, got, nseen, done - got, noack, mismatch);
    for (int k = 0; k < nseen; k++) {
        int off = xt57_offset(seen[k]);
        if (off < 0) {
            PrintAndLogEx(INFO, "  %08X x%-3u not in the known stream", seen[k], seen_n[k]);
        } else {
            PrintAndLogEx(INFO, "  %08X x%-3u stream offset %d", seen[k], seen_n[k], off);
        }
    }
    return res;
}

//-----------------------------------------------------------------------------
// xt57 reset - free a chip stuck broadcasting an emulated type
//-----------------------------------------------------------------------------
static int CmdLFXhorseXt57Reset(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse xt57 reset",
                  "Free an XT57B that is stuck emulating a talk-first transponder type.\n"
                  "Such a chip broadcasts continuously and answers nothing on XMICRO, so none of\n"
                  "the other xt57 commands reach it.\n"
                  "\n"
                  "Sends the VVDI's exit-emulation sequence: two rounds of four downlink bursts,\n"
                  "each in its own power cycle, probing XMICRO after each round; then walks the\n"
                  "burst outwards from that offset.\n"
                  "\n"
                  "There is a word per emulation family, and a chip that is stuck cannot be asked\n"
                  "which it is, so with no --word both are sent in turn:\n"
                  "  erase    ASCII \"erase\", 16 ms, at 109.7 ms - ID11/ID12/ID13 and ID8E/ID48\n"
                  "  7935     a slower 58 ms word, at 18.8 ms - a chip blanked to 7935\n"
                  "  megamos  the ID48/ID8E word, only reached by naming it\n"
                  "\n"
                  "The burst has to land at a particular phase of the chip's broadcast frame, and\n"
                  "the frame is as long as the emulated type makes it - 256 us for ID11 at RF/32,\n"
                  "but 4096 us for a blanked PCF7935, whose sweep therefore steps 128 us.\n"
                  "\n"
                  _YELLOW_("This frees the channel, it does not restore the factory personality") ".\n"
                  "Follow it with `lf xhorse xt57 restore`.",
                  "lf xhorse xt57 reset\n"
                  "lf xhorse xt57 reset --word 7935     -> only the 7935 word");
    void *argtable[] = {
        arg_param_begin,
        arg_lit0(NULL, "force", "send the bursts even if the chip already answers XMICRO"),
        arg_int0(NULL, "at",    "<dec>", "centre of the burst sweep, us from power-up (def: per word, 109700 / 18760)"),
        arg_int0(NULL, "step",  "<dec>", "sweep step in us (def: per word, 40 / 128)"),
        arg_int0("n",  "count", "<dec>", "swept positions either side of the centre (def 32)"),
        arg_int0(NULL, "probe", "<dec>", "probe XMICRO every N swept bursts (def 8)"),
        arg_int0(NULL, "on",    "<dec>", "field-on window holding one burst, us (def: per word, 156000 / 107820)"),
        arg_str0(NULL, "word",  "<name>", "burst word: erase, megamos or 7935 (def: erase then 7935)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    lf_xhorse_xt57_reset_t q;
    memset(&q, 0, sizeof(q));
    q.force = arg_get_lit(ctx, 1) ? 1 : 0;
    q.at_us = arg_get_int_def(ctx, 2, 0);
    q.step_us = arg_get_int_def(ctx, 3, 0);
    q.nsweep = (int16_t)arg_get_int_def(ctx, 4, 0);
    q.probe_every = (uint8_t)arg_get_int_def(ctx, 5, 0);
    q.on_us = arg_get_int_def(ctx, 6, 0);
    char wordstr[16] = {0};
    int wlen = 0;
    CLIParamStrToBuf(arg_get_str(ctx, 7), (uint8_t *)wordstr, sizeof(wordstr) - 1, &wlen);
    CLIParserFree(ctx);
    // 0 = try the erase word and then the 7935 one; 1/2/3 name a single word
    if (wlen > 0) {
        if (str_startswith(wordstr, "erase")) {
            q.word = 1;
        } else if (str_startswith(wordstr, "mega")) {
            q.word = 2;
        } else if (str_startswith(wordstr, "7935")) {
            q.word = 3;
        } else {
            PrintAndLogEx(FAILED, "word must be erase, megamos or 7935");
            return PM3_EINVARG;
        }
    }

    clearCommandBuffer();
    SendCommandNG(CMD_LF_XHORSE_XT57_RESET, (uint8_t *)&q, sizeof(q));

    PacketResponseNG resp;
    // Two rounds of four bursts: each burst is a 70 ms power cycle plus a 156 ms window, each
    // round adds a 568 ms drop, an XMICRO probe and a 228 ms drop.  About 2.5 s, plus margin.
    // With no --word two words are sent in turn, so the whole thing runs twice.
    const int nsw = (q.nsweep ? q.nsweep : 32) * ((q.word == 0) ? 2 : 1);
    if (WaitForResponseTimeout(CMD_LF_XHORSE_XT57_RESET, &resp, 120000 + nsw * 900) == false) {
        PrintAndLogEx(WARNING, "timeout while waiting for reply");
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        return PM3_ETIMEOUT;
    }

    const lf_xhorse_info_t *r = (const lf_xhorse_info_t *)resp.data.asBytes;
    if (resp.status != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "no XMICRO answer after the bursts - chip still stuck, or no tag");
        PrintAndLogEx(HINT, "the burst must land at the right point in the chip's broadcast frame;");
        PrintAndLogEx(HINT, "try again, and check the tag is on the antenna");
        return PM3_ESOFT;
    }

    char fam[9] = {0};
    const uint8_t n = (r->bchip_type_len < 8) ? r->bchip_type_len : 8;
    memcpy(fam, r->bchip_type, n);
    if (r->found) {
        PrintAndLogEx(SUCCESS, "chip was not stuck - it already answers XMICRO as " _GREEN_("%s"), fam);
        return PM3_SUCCESS;
    }
    if (q.force) {
        PrintAndLogEx(SUCCESS, "bursts sent - the chip answers XMICRO as " _GREEN_("%s"), fam);
        return PM3_SUCCESS;
    }
    PrintAndLogEx(SUCCESS, "freed - the chip answers XMICRO again as " _GREEN_("%s"), fam);
    PrintAndLogEx(HINT, "`" _YELLOW_("lf xhorse xt57 restore") "` rewrites the factory ID46 personality");
    if (r->em_slip_us != 0) {
        PrintAndLogEx(INFO, "the VVDI's burst offset did not work; freed by the sweep, within "
                      "8 steps of %+d us of it", r->em_slip_us);
    }
    PrintAndLogEx(INFO, "its configuration is unchanged: this restores the channel, not the personality");
    return PM3_SUCCESS;
}

static const uint8_t xt57_sel_default[XHORSE_XT57_SELLEN] =
    { 0xC0, 'X', 'T', '5', '7', 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 };

//-----------------------------------------------------------------------------
// xt57 set / restore - rewrite an XT57B's personality
//-----------------------------------------------------------------------------
// The VVDI sets a type with one XMICRO session: it writes the emulated image into block 0x00
// and the type byte into block 0x7F page 4 (docs/xt57b-fullread.md SS12.7).  These commands
// send that session, but built rather than replayed - the frames are generated from a recovered
// keystream, so the type and the UID can be chosen.
//
// The keystream is fixed by (nonce, exchange index), so a session can only be built at a nonce
// we hold keystream for, and can be no longer than that keystream reaches.  Which power-up delay
// produces a given nonce differs from chip to chip, so the nonce is searched for first and the
// session then runs at the delay that found it.
#define XT57_SESS_OFF_US   45000
// EEPROM program time: the VVDI leaves ~2000 T0 (16 ms) between a write and its read-back.
#define XT57_PROG_T0       2000
#define XT57_MAXSTEPS      200
#define XT57_SEARCH_CENTRE 119500   // default power-up search window, us
#define XT57_SEARCH_SPAN   400

// nonce 6DCD7463, 21 exchanges (docs/xt57b-fullread.md SS12.5)
static const uint8_t xt57_ks6_k0[22] = {
    0x00, 0x33, 0x78, 0xEF, 0xE1, 0xFB, 0xFF, 0xED, 0x8A, 0x08, 0xD8,
    0xB3, 0x41, 0x8E, 0x43, 0x6A, 0xE8, 0x6E, 0xF4, 0x8E, 0x39, 0xDF
};
static const uint32_t xt57_ks6_K[22] = {
    0x00000000, 0x45054F9A, 0x7EF3A1E6, 0x1D1A164E, 0xEE3C7736, 0xBE77E5B5,
    0xC7C97028, 0x238D1C76, 0xD0956578, 0xB10D9A18, 0x0A81BA77, 0xE95C1B90,
    0x7EE6A615, 0x487DA232, 0x80EE121F, 0xAC2CCA5D, 0xB477B3AE, 0x7DCAAEE9,
    0xAE8843FE, 0xC2F499D3, 0xE9C37D4E, 0x90539E75
};
// nonce CDB9AE8C, 182 exchanges.  Indices 1..23 are SS12.6's table, re-measured and unchanged;
// 24..181 were recovered the same way (docs/xt57b-fullread.md SS12.9) by sweeping an eight-way
// read at every index of the VVDI's own 182-exchange set-7935 session, which runs at this nonce.
// Index 182 is that session's terminator and carries a command byte only: no data keystream was
// measured there, so K[182] is zero and only an 'E' step may sit at that index (`nfull` below).
static const uint8_t xt57_ksC_k0[183] = {
    0x00, 0x7E, 0xA4, 0x94, 0x97, 0x6F, 0x17, 0xAA, 0x21, 0x13, 0xCB, 0x28,
    0x7F, 0xDE, 0x7D, 0xF4, 0xBE, 0x3E, 0x1A, 0x36, 0x21, 0x3C, 0x82, 0x2C,
    0xB7, 0xB9, 0x13, 0x02, 0x5F, 0xAB, 0x9A, 0xBE, 0xD4, 0xC2, 0x78, 0x41,
    0x10, 0xF6, 0x37, 0x93, 0x54, 0x82, 0xB3, 0xB0, 0xCB, 0x2D, 0xE2, 0xCA,
    0x74, 0xDC, 0xFB, 0xF7, 0x43, 0xDB, 0xF8, 0x5B, 0xE6, 0xA9, 0xA1, 0xE5,
    0x9E, 0x22, 0x19, 0x82, 0x68, 0xE6, 0xAD, 0x86, 0x1E, 0xFE, 0x92, 0xE2,
    0xC7, 0xC8, 0x01, 0xA1, 0x6E, 0x71, 0x9E, 0xF2, 0x36, 0x37, 0xD6, 0x6C,
    0xBA, 0x33, 0x1E, 0xF5, 0xA2, 0x43, 0x9C, 0x5D, 0x2D, 0x80, 0x9F, 0x61,
    0x46, 0x7C, 0xA0, 0xE1, 0x06, 0xF0, 0x20, 0x97, 0x98, 0x52, 0x55, 0xF0,
    0xD7, 0x75, 0xCE, 0x9C, 0x71, 0x8C, 0xBE, 0x85, 0x30, 0x59, 0x32, 0x21,
    0xBD, 0x0A, 0xA1, 0xD0, 0x45, 0x35, 0xAC, 0xDC, 0xEA, 0x65, 0xC1, 0xA6,
    0xAB, 0x21, 0x13, 0xE3, 0xF2, 0x90, 0xB8, 0x9E, 0x2C, 0x02, 0x8C, 0xA2,
    0x91, 0x42, 0xCE, 0xEA, 0x66, 0x30, 0x91, 0x2B, 0x1E, 0x1D, 0x5D, 0xA3,
    0x9F, 0x39, 0xB5, 0x23, 0x5D, 0x04, 0xFD, 0x87, 0x3F, 0xFC, 0xD0, 0xAA,
    0x08, 0x19, 0x98, 0x33, 0x46, 0xF0, 0x73, 0x7A, 0x8B, 0xC0, 0xCE, 0x9D,
    0xC2, 0x2D, 0x9B
};
static const uint32_t xt57_ksC_K[183] = {
    0x00000000, 0xB0CD03E4, 0xCE685B62, 0xFEACD0A7, 0x1DD19486, 0x6CC0FE4B,
    0x3675EB63, 0x725B082E, 0x6AE1800A, 0x6CAC6B67, 0xBA71687F, 0x6CD6ED8A,
    0x1E2BB45E, 0x8AAF5D70, 0x0CA182D6, 0x34D5AD94, 0x54053EFC, 0xC71CB4F1,
    0x49EEDE03, 0xA0E04E3C, 0x836981FF, 0xFEB31762, 0x60694C6A, 0x91130817,
    0xA0107BE6, 0x61C11C4F, 0xD2AA24A2, 0x7E496CD7, 0xEB5FDEA4, 0xDCC9EFD9,
    0xE979A535, 0x373EEB77, 0xF6E20227, 0x1A588D63, 0x2FAE4099, 0x6A4F7835,
    0xC27CE8E3, 0xB9F53094, 0x60192EF3, 0x65940C51, 0x57F99C1B, 0xD41980BF,
    0xD86748D9, 0xE42CA4B4, 0x42693FB1, 0x1BBC7426, 0x9A99C2FC, 0x604303D3,
    0xDDC6665E, 0xA75609E2, 0xA5538BD1, 0x65F50B05, 0xCCCCE4EB, 0xBAF67AAC,
    0x5720571C, 0x95FAF0CB, 0xDFB0D0A8, 0x0DAC5BEC, 0x543BAEE6, 0xEE91E96F,
    0x7DC7C909, 0xCF9BF7B8, 0x6B55C142, 0x0363829A, 0x209AF2BF, 0x0250B511,
    0xAA6957CF, 0x038ED482, 0xECED12CA, 0xFE3F97FE, 0x7B97BF52, 0xB44E4B2C,
    0x14A907C1, 0x74FE10CE, 0xC767D4D9, 0x80D5864B, 0x6D8F602C, 0xB85E6F7F,
    0xBA5ED59B, 0x1039725B, 0xC066631E, 0x7F211D75, 0xC3183D73, 0x03A6A87D,
    0x2A1384C4, 0x24FD2392, 0x19B995E3, 0x5A6CF318, 0x64B61F1D, 0x15EAA7D8,
    0x742511BA, 0x6923D135, 0x9E10DB8D, 0x1D214E58, 0x83E93C4B, 0x37075F83,
    0x4C4A31B6, 0x3C426719, 0x63054FC6, 0x693C0EBA, 0xF5AA0EFA, 0x2EDA21B6,
    0xFEC484B6, 0x0CD51197, 0x8E4945CA, 0xA2DA8760, 0x6E17DF60, 0xA264F488,
    0x63B13974, 0x8591F3F2, 0xBDF466F0, 0x74C4E132, 0x34FC5FCD, 0x1D66BD6D,
    0x0E7F09E7, 0x97874DF9, 0x7FAD2D96, 0xB351FD9D, 0x600ACF6B, 0x049A3246,
    0x64245B28, 0xD01FF742, 0xE6CB1871, 0xCD2BDFEF, 0xC10786F3, 0xC50AAACE,
    0x00A13C80, 0x0954F18E, 0x6FF1D1A3, 0xC73FE029, 0x38EA5671, 0x9428C93C,
    0x00F495A8, 0x50D6B33B, 0x2223BEFB, 0x8FE5AA4C, 0xDF62AA20, 0x1D1D0F69,
    0xEAE9A9E2, 0xFD3F45FE, 0xE213C07F, 0xED0A3BED, 0xF214A445, 0xA08B3C59,
    0x35FB7221, 0x5A841022, 0xAA462357, 0xC1C2DE3F, 0x4C635539, 0x4FFCF0E4,
    0xCE181B64, 0x564FD0D1, 0xBCB7ACAE, 0xAD120213, 0x7CBE3B71, 0x0AA6EF00,
    0xC5AC99F4, 0xB8A4E5A7, 0xB1CF7300, 0xFB927B4F, 0xF23D3AF3, 0xB2087776,
    0xA9A80ECA, 0x45452845, 0xEC0E394B, 0xEAC36ED8, 0xBB68ACEB, 0xD7235E47,
    0x35667FE1, 0x740A199B, 0x24128F01, 0x765B0374, 0xAB55CF21, 0x194407E9,
    0x090B9171, 0x6DD239AF, 0x77DE9047, 0x2B7C18E1, 0xB78FD77A, 0xDD5B9CB8,
    0xD90CDD4F, 0xBFD2E386, 0x00000000
};

typedef struct xt57_ks_s {
    uint32_t        nonce;
    uint32_t        answer;
    int             nex;        // exchanges this keystream reaches, 1..nex
    int             nfull;      // of those, the ones with a 32-bit data keystream too.  Past it
    //                             only a terminator ('E', a command byte alone) may be built.
    const uint8_t  *k0;
    const uint32_t *K;
    int32_t         off_us;     // field-off before each session.  This is family-critical: an XT37
    //                             holds its nonce state through 45 ms, so at the XT57's 45000 its
    //                             nonce is *pinned* and no longer tracks the power-up delay at all
    //                             (docs/xt37.md SS4a) - which makes steering impossible and reads a
    //                             lottery.  200 ms restores the delay -> nonce mapping.
    uint8_t         sel_auto;   // 1 => let the firmware fill the select's family bytes in from the
    //                             chip's own identify reply (docs/xt37.md SS3).  An XT37's recorded
    //                             sessions use `C0 "XT37" 123456789ABCDEF0`, exactly what that
    //                             produces, so one table serves any chip of the family.
    int32_t         centre;     // default centre of the power-up search, us
    int32_t         span;       // default half-width, us
    int32_t         step;       // search step, us.  The nonce window advances one stream bit per
    //                             8 us, so 8 would visit every nonce once - but the mapping
    //                             drifts by a bit either way, and a nonce visited once can be
    //                             stepped straight over.  2 gives each one a plateau of about
    //                             four steps, which it is not worth missing a session over.
} xt57_ks_t;

// XT37, nonce AD4BD054: 178 contiguous exchanges (docs/xt37.md SS5n/SS5p)
static const uint8_t xt37_ks_AD4BD054_k0[179] = {
    0x00, 0x60, 0x8D, 0xD5, 0x29, 0xBC, 0xF0, 0x5F, 0x0A, 0x7A, 0x0A, 0xB9,
    0x21, 0xD3, 0x62, 0x21, 0xF6, 0xDE, 0xD5, 0x1C, 0xFC, 0x28, 0x7D, 0x2F,
    0x53, 0x5A, 0x37, 0x70, 0x69, 0x3F, 0xC0, 0xA7, 0x2C, 0x2F, 0xC9, 0x12,
    0xFA, 0x89, 0xC8, 0x55, 0xBC, 0x9B, 0x7D, 0x1F, 0xF3, 0x3D, 0x0B, 0x07,
    0x0E, 0xB2, 0xA4, 0xBB, 0x8B, 0xC4, 0x5F, 0xF4, 0x95, 0xE4, 0x3A, 0x05,
    0x74, 0x3B, 0xAD, 0xEA, 0x28, 0x92, 0x54, 0x15, 0x5A, 0xFF, 0x8E, 0x63,
    0x65, 0xFE, 0x66, 0x54, 0xB0, 0x34, 0x2F, 0xEC, 0x69, 0xD7, 0xE7, 0x7C,
    0xA0, 0x85, 0xF1, 0x6E, 0x57, 0x3B, 0x8E, 0xF8, 0xF3, 0xB3, 0xBF, 0x11,
    0xB1, 0x13, 0x52, 0xFC, 0xD9, 0x0C, 0x0E, 0x8E, 0xB4, 0x1D, 0x5D, 0x5B,
    0x3E, 0x2B, 0x33, 0x7D, 0x61, 0x4A, 0x19, 0x6C, 0x26, 0x03, 0x8B, 0xBD,
    0x0F, 0xAD, 0xE8, 0x4E, 0xF3, 0xC8, 0x22, 0x6F, 0xBA, 0x15, 0x4A, 0x78,
    0x8A, 0x95, 0xEC, 0x66, 0x58, 0xFF, 0x98, 0x8A, 0x86, 0xB8, 0xFF, 0x8B,
    0xC8, 0x7C, 0x14, 0x6E, 0x96, 0x23, 0xAF, 0x40, 0x1A, 0xDE, 0xEB, 0xBC,
    0x6D, 0x24, 0x4B, 0xB2, 0x59, 0x3D, 0x2A, 0xB0, 0xB9, 0xA1, 0x03, 0x11,
    0x0F, 0xD2, 0x2D, 0x4D, 0x72, 0x7B, 0xAF, 0xA3, 0xA9, 0xF6, 0x98
};
static const uint32_t xt37_ks_AD4BD054_K[179] = {
    0x00000000, 0x9FA57938, 0x5A45317F, 0x86C42898, 0x2FEA11C7, 0x37A3315B,
    0x21F29B5C, 0xA162B782, 0x5916234E, 0x86A68223, 0xC0B2590C, 0x42D00D27,
    0x901E855F, 0x7142865F, 0x2383D5C0, 0x7BCA9C0B, 0x3A1F0F5A, 0x6E3084E1,
    0x539BBC75, 0xE72BA04F, 0x66824EA7, 0xDE083A76, 0x81D79509, 0xF0E54E51,
    0x05E953A8, 0x5D928518, 0xA5BA2F40, 0x243527A8, 0xACBEF7F0, 0x8C57598D,
    0xCE192DB0, 0x0D413FB3, 0xE257411F, 0x48D97B03, 0x56F18879, 0x63C197AD,
    0xA8F0D94F, 0x5BEB16DD, 0xF7632754, 0x4AFC0EDB, 0xEFA103F6, 0xD83354E1,
    0x75775A94, 0xE4694117, 0x522C58C3, 0xEEB14BA9, 0x6AD6AE88, 0x7C51EF16,
    0x726C0BF7, 0x955BEE10, 0x86398756, 0x79A0ECE9, 0x59C4430A, 0x29ABDFD4,
    0x5A63C615, 0x766DB1AD, 0x97C9C035, 0x43BB57F5, 0xF07E3A08, 0xDF5D1D4D,
    0x9ADC0631, 0x3E44F465, 0xC896D2B5, 0x8BA95C8A, 0x4CC6CE2F, 0xE4E08CE6,
    0x0696A064, 0x0BDFBA53, 0xAB54049D, 0xF91876BE, 0x6D774B6C, 0xBD454C1A,
    0xBE94A498, 0xF97118D3, 0x8093C265, 0xF2348827, 0x1309C812, 0xA541F653,
    0xD827E1B3, 0xD3E83730, 0x547E7C8C, 0xF8CE11DE, 0x026BD39A, 0x3FEE5F92,
    0x86CFFDCD, 0x392DD17C, 0x3114FF41, 0xF94AB80F, 0xAF28AE78, 0xF24FC65C,
    0xC2463BC4, 0x0651D494, 0x097D26B9, 0x30E952EF, 0x10611E07, 0x826155D3,
    0xE695CACE, 0x3A3C90A9, 0x0421472A, 0x7EDD698F, 0xD5C9C5CE, 0x08162BC7,
    0x2D1DE3C4, 0x96FD7A44, 0x5ED22052, 0x7D034F10, 0x89A872B9, 0xFB86CFEE,
    0x2EC60C96, 0xB445FDE6, 0x40E312D5, 0x34E056FC, 0xB6B44207, 0xF39FD03E,
    0x3F8AA717, 0x2D2372A5, 0x99E7D2FC, 0xE5603564, 0xBCAA917D, 0x8FF6AC86,
    0xFDAA2652, 0xEC2D3785, 0x3E950628, 0x73F25E41, 0xB7E8C7FD, 0x4357DF2D,
    0x9D0E8951, 0xD05DD2F1, 0x6B7EF40A, 0x729B19E0, 0xD07DDA7A, 0x219C873F,
    0x29D1B835, 0x96B8A6AA, 0xA4255E5C, 0xF96471E9, 0xDBDC7D42, 0xAAF069D5,
    0xA5B159AB, 0xBE36A6B2, 0xEB07FBCD, 0xF47325FA, 0x9B772AAB, 0x5E76000E,
    0xEB9213C3, 0x97A30B81, 0x61FC8E4E, 0xF5E4A516, 0xA0A8BB08, 0x37AB728E,
    0x9EB546D1, 0x664E3298, 0xBD52C60E, 0xD3D9BE38, 0x451B0798, 0xAEDD63CA,
    0x64FE9F03, 0x126EAD9F, 0x9DE24E5C, 0x3CAF310D, 0xAFD4C501, 0x0A977245,
    0xF8F4E173, 0xF3E90C5F, 0x7F589DCA, 0x7C8A90B3, 0x8451D1E5, 0xFB85E74E,
    0xB5497650, 0xE870819D, 0x42B6D7FD, 0x9209E8AD, 0x5849CDC6, 0x4E3580A3,
    0xE01E54A0, 0xB8BBC9C2, 0xBCC24F63, 0x39FFAED6, 0xBC269AA7
};
// XT37, nonce D4BD0543: 170 contiguous exchanges (docs/xt37.md SS5n/SS5p)
static const uint8_t xt37_ks_D4BD0543_k0[171] = {
    0x00, 0x83, 0x8E, 0xA3, 0xD9, 0xD7, 0xEC, 0x1E, 0x06, 0x25, 0xD2, 0x71,
    0x2E, 0x9C, 0x00, 0x97, 0x71, 0x0E, 0x5F, 0x57, 0xD2, 0x53, 0xFA, 0x02,
    0xAE, 0x5F, 0xFC, 0x40, 0x46, 0x55, 0x09, 0x55, 0x77, 0x37, 0x08, 0xFD,
    0x0D, 0xAB, 0x6A, 0x84, 0x65, 0x32, 0x26, 0x62, 0x2B, 0xBE, 0x29, 0x69,
    0xC4, 0x0F, 0x1D, 0x68, 0x32, 0xA8, 0xE2, 0x24, 0xAF, 0xB9, 0x9D, 0x2F,
    0x26, 0x02, 0x0C, 0x70, 0xBC, 0x27, 0xC5, 0x77, 0x82, 0x27, 0xD8, 0x2D,
    0x37, 0x00, 0xC1, 0x76, 0x31, 0x7A, 0x03, 0x91, 0x99, 0x83, 0x9A, 0x29,
    0x45, 0xCD, 0x90, 0x22, 0x61, 0x13, 0x98, 0xB3, 0xA9, 0x1C, 0x2B, 0x38,
    0x53, 0x60, 0x38, 0x6F, 0x5A, 0xAC, 0x7B, 0x99, 0x37, 0x0B, 0x9E, 0xDA,
    0x0E, 0xB9, 0x6B, 0xAC, 0xAE, 0x40, 0x5D, 0x38, 0x86, 0xCF, 0xB7, 0xED,
    0xB7, 0xEE, 0xE2, 0xF5, 0xBE, 0xAF, 0x3F, 0xD8, 0xFE, 0x9A, 0xFE, 0x4F,
    0xBF, 0x71, 0x44, 0xDF, 0x3B, 0x12, 0x7F, 0x78, 0xF1, 0x78, 0x90, 0x47,
    0x42, 0xC2, 0x93, 0x91, 0xB9, 0x00, 0xB0, 0x74, 0x63, 0xD4, 0x5E, 0x0C,
    0xE0, 0x7B, 0xA3, 0x12, 0xD9, 0x79, 0x48, 0x39, 0x4C, 0x8D, 0x46, 0xA5,
    0x9D, 0x64, 0x04
};
static const uint32_t xt37_ks_D4BD0543_K[171] = {
    0x00000000, 0x3C67C70E, 0x08940100, 0x06758FD3, 0x9E074368, 0xBB17627D,
    0xB15817BB, 0xA1AF9A9A, 0x081C85EB, 0x247A4766, 0x97794809, 0xB3ED3D6A,
    0x63A84A32, 0x17457CF1, 0xC5656083, 0xE93A7897, 0x3CCCECDE, 0x0C8F06A9,
    0xA5A34758, 0x6C076AF5, 0x89F76556, 0x4649423B, 0xF2D19CB5, 0xC38D58E7,
    0x957C1263, 0x20264791, 0x637E3F12, 0x1D55E31D, 0x94FF5E11, 0x13F23F2C,
    0xDA76E50C, 0x03BEF4D5, 0xF07E2E9E, 0x31C48463, 0xC5D8241F, 0xA1A64120,
    0xC5456C05, 0xA92AF477, 0x8D177533, 0xD72C5842, 0xF12BB100, 0xD861A5D4,
    0xE0F296F9, 0xE3F43368, 0x93470E7C, 0xC235E307, 0x5A323228, 0x9FE0FDC4,
    0x4508A549, 0x80C9E96E, 0xEF248FB1, 0x45C3BEF4, 0x60960F8C, 0x9C9BDBB6,
    0x2B563AFF, 0xC65548CF, 0x0EE9638A, 0x32EBF83D, 0xEA96A847, 0xF8D95476,
    0x4BD709CB, 0x5B85BD50, 0x415D0A5C, 0x2A4465EE, 0x2D6010F2, 0xED623007,
    0xD4B33ABF, 0xA53173F0, 0x1E26991B, 0xBE882ADA, 0xDB6E33E3, 0xD5587252,
    0x7D2D7EE3, 0x08370271, 0xE9E17161, 0x12359F90, 0x72197579, 0x1BE69319,
    0x9E51B3A9, 0xB9FB82D3, 0x5DFC2F17, 0x18BCA741, 0x1079CFC9, 0xCF58CC10,
    0xF55B407A, 0x55B1F81E, 0x818DA187, 0xEB02C772, 0xE8F1F393, 0x27A0905A,
    0xDD445F7B, 0x52E9ACFE, 0x947DB58A, 0x89079522, 0x902D25E3, 0x1369CCC4,
    0xF2A295C7, 0xDA27C01D, 0x802BD375, 0xE4C770BC, 0x6464E121, 0xD6E34ED6,
    0xEA7F3303, 0x1534B305, 0x52E0FCA1, 0xA7232993, 0xA8E386D5, 0x767BECE9,
    0x2564312D, 0x07684A7B, 0xA16F5C9D, 0x700CE03C, 0x5C59522B, 0x5C9E483D,
    0xF43399AF, 0xEB3C0E06, 0x50A0E9AD, 0x2F32A348, 0x3429FC7E, 0x1E50CE10,
    0x2D19BA3C, 0xD4A758D4, 0xB1E61DA6, 0xEEC33001, 0x59DF50D1, 0xAE704914,
    0x58C7CEB5, 0x26CCD001, 0x47EE4270, 0x15BE6132, 0x79052EFD, 0xA28E3210,
    0x276FDCDA, 0xD821AB4D, 0x63B36A64, 0xCB6E9019, 0xA470BAE1, 0x2A563E01,
    0x18AF09A9, 0x0A43637C, 0xE3EF615E, 0xC8527667, 0x45DA2C5F, 0xCCD4EC4F,
    0x18DA5FA9, 0x0410FF2C, 0x5AA021A8, 0x552B9955, 0x97B15150, 0x0B2A46B9,
    0xA090CCA4, 0xD47BAEF9, 0xA151DB9A, 0xD4BF6BEE, 0x1CA9FA01, 0x1E9C00BD,
    0x7325CB34, 0xF75D1DB6, 0x78A909F8, 0xA5FC9CE3, 0x1DB09C32, 0xD03D5EFC,
    0xF1E7CCD6, 0x8337320D, 0x44F01E3D, 0xB0D80C03, 0x6C6A5E36, 0x0D79CC79,
    0xA4EB6DD8, 0x08DAB2B3, 0x94B6A644
};

// Emulated images, decrypted from the Xhorse tool's own set sessions (docs/xt37.md SS5p).
// Each entry is one page write: (select operand, page, value).  The type byte is not here -
// setimage commits it separately, after the image, per xt57b-fullread.md SS12.7.
typedef struct { uint8_t arg; uint8_t page; uint32_t val; } xt_imgpage_t;
// `cfg4`/`cfg5` are the config-block words the tool wrote for this type, not just its type byte.
// The upper half is not a constant - 01F0 for most, 3FF0 for ID48 and MQB, 7FF0 for ID7935 - so it
// has to come from the capture rather than be carried over from whatever the chip held (SS5p).
// cfg5 is written only where the tool wrote page 5 at all (ID7935 alone, so far).
typedef struct {
    const char *name; uint8_t type; const xt_imgpage_t *pages; uint16_t n;
    uint32_t cfg4; uint32_t cfg5;
} xt_image_t;

static const xt_imgpage_t xt_img_ID11[5] = {
    { 0x00, 0, 0x00000010 }, { 0x00, 1, 0x00000000 }, { 0x00, 2, 0x0000BEFA }, { 0x00, 3, 0x00000000 },
    { 0x00, 4, 0x00000000 }
};
static const xt_imgpage_t xt_img_ID12[5] = {
    { 0x00, 0, 0x00000014 }, { 0x00, 1, 0x00000000 }, { 0x00, 2, 0x000066F0 }, { 0x00, 3, 0x00000000 },
    { 0x00, 4, 0x00000000 }
};
static const xt_imgpage_t xt_img_ID13[5] = {
    { 0x00, 0, 0x00000018 }, { 0x00, 1, 0x00000000 }, { 0x00, 2, 0x0000FF01 }, { 0x00, 3, 0x00000000 },
    { 0x00, 4, 0x00000000 }
};
static const xt_imgpage_t xt_img_ID48[16] = {
    { 0x00, 0, 0x00005678 }, { 0x00, 1, 0x0000A167 }, { 0x00, 2, 0x00000419 }, { 0x00, 3, 0x000078B9 },
    { 0x00, 4, 0x0000AAAA }, { 0x00, 5, 0x0000AAAA }, { 0x00, 6, 0x0000AAAA }, { 0x00, 7, 0x0000AAAA },
    { 0x01, 0, 0x0000AAAA }, { 0x01, 1, 0x0000AAAA }, { 0x01, 2, 0x0000AAAA }, { 0x01, 3, 0x0000AAAA },
    { 0x01, 4, 0x0000AAAA }, { 0x01, 5, 0x0000AAAA }, { 0x01, 6, 0x0000AAAA }, { 0x01, 7, 0x0000AAAA }
};
static const xt_imgpage_t xt_img_ID4A[12] = {
    { 0x00, 0, 0xE06C7617 }, { 0x00, 1, 0x11112222 }, { 0x00, 2, 0x33334444 }, { 0x00, 3, 0x55556666 },
    { 0x00, 4, 0x77778888 }, { 0x00, 5, 0x00FFFFFF }, { 0x00, 6, 0x55555555 }, { 0x00, 7, 0xAAAAAAAA },
    { 0x0F, 0, 0x00000000 }, { 0x0F, 1, 0x00000000 }, { 0x0F, 2, 0x00000000 }, { 0x0F, 3, 0x00000000 }
};
static const xt_imgpage_t xt_img_ID4C[3] = {
    { 0x04, 4, 0x00000000 }, { 0x04, 5, 0x00000000 }, { 0x04, 6, 0x00000000 }
};
static const xt_imgpage_t xt_img_ID4D[41] = {
    { 0x00, 0, 0x00000000 }, { 0x00, 1, 0x00000000 }, { 0x00, 2, 0x00000000 }, { 0x00, 3, 0x00000000 },
    { 0x00, 4, 0x00000000 }, { 0x00, 5, 0x00000000 }, { 0x00, 6, 0x00000000 }, { 0x00, 7, 0x00000000 },
    { 0x01, 0, 0x00000000 }, { 0x01, 1, 0x00000000 }, { 0x01, 2, 0x00000000 }, { 0x01, 3, 0x00000000 },
    { 0x01, 4, 0x00000000 }, { 0x01, 5, 0x00000000 }, { 0x01, 6, 0x00000000 }, { 0x01, 7, 0x00000000 },
    { 0x02, 0, 0x00000000 }, { 0x02, 1, 0x00000000 }, { 0x02, 2, 0x00000000 }, { 0x02, 3, 0x00000000 },
    { 0x02, 4, 0x00000000 }, { 0x02, 5, 0x00000000 }, { 0x02, 6, 0x00000000 }, { 0x02, 7, 0x00000000 },
    { 0x03, 0, 0x00000000 }, { 0x03, 1, 0x00000000 }, { 0x03, 2, 0x00000000 }, { 0x03, 3, 0x00000000 },
    { 0x03, 4, 0x00000000 }, { 0x03, 5, 0x00000000 }, { 0x03, 6, 0x00000000 }, { 0x04, 0, 0x00000003 },
    { 0x04, 1, 0x00000000 }, { 0x04, 2, 0x00109DD4 }, { 0x04, 3, 0x0000036C }, { 0x04, 4, 0x00000000 },
    { 0x04, 5, 0x00000000 }, { 0x04, 6, 0x00000000 }, { 0x04, 7, 0x00000000 }, { 0x05, 0, 0x00000000 },
    { 0x0A, 3, 0x00004600 }
};
static const xt_imgpage_t xt_img_ID4E[41] = {
    { 0x00, 0, 0x00000000 }, { 0x00, 1, 0x00000000 }, { 0x00, 2, 0x00000000 }, { 0x00, 3, 0x00000000 },
    { 0x00, 4, 0x00000000 }, { 0x00, 5, 0x00000000 }, { 0x00, 6, 0x00000000 }, { 0x00, 7, 0x00000000 },
    { 0x01, 0, 0x00000000 }, { 0x01, 1, 0x00000000 }, { 0x01, 2, 0x00000000 }, { 0x01, 3, 0x00000000 },
    { 0x01, 4, 0x00000000 }, { 0x01, 5, 0x00000000 }, { 0x01, 6, 0x00000000 }, { 0x01, 7, 0x00000000 },
    { 0x02, 0, 0x00000000 }, { 0x02, 1, 0x00000000 }, { 0x02, 2, 0x00000000 }, { 0x02, 3, 0x00000000 },
    { 0x02, 4, 0x00000000 }, { 0x02, 5, 0x00000000 }, { 0x02, 6, 0x00000000 }, { 0x02, 7, 0x00000000 },
    { 0x03, 0, 0x00000000 }, { 0x03, 1, 0x00000000 }, { 0x03, 2, 0x00000000 }, { 0x03, 3, 0x00000000 },
    { 0x03, 4, 0x00000000 }, { 0x03, 5, 0x00000000 }, { 0x03, 6, 0x00000000 }, { 0x04, 0, 0x00000003 },
    { 0x04, 1, 0x00000000 }, { 0x04, 2, 0x004AFFFE }, { 0x04, 3, 0x0000036C }, { 0x04, 4, 0x00000000 },
    { 0x04, 5, 0x00000000 }, { 0x04, 6, 0x00000000 }, { 0x04, 7, 0x00000000 }, { 0x05, 0, 0x00000000 },
    { 0x0A, 3, 0x00004600 }
};
static const xt_imgpage_t xt_img_ID7935[80] = {
    { 0x00, 0, 0x00000000 }, { 0x00, 1, 0x00000000 }, { 0x00, 2, 0x00000000 }, { 0x00, 3, 0x00000000 },
    { 0x00, 4, 0x00000000 }, { 0x00, 5, 0x00000000 }, { 0x00, 6, 0x00000000 }, { 0x00, 7, 0x00000000 },
    { 0x01, 0, 0x00000000 }, { 0x01, 1, 0x00000000 }, { 0x01, 2, 0x00000000 }, { 0x01, 3, 0x00000000 },
    { 0x01, 4, 0x00000000 }, { 0x01, 5, 0x00000000 }, { 0x01, 6, 0x00000000 }, { 0x01, 7, 0x00000000 },
    { 0x02, 0, 0x00000000 }, { 0x02, 1, 0x00000000 }, { 0x02, 2, 0x00000000 }, { 0x02, 3, 0x00000000 },
    { 0x02, 4, 0x00000000 }, { 0x02, 5, 0x00000000 }, { 0x02, 6, 0x00000000 }, { 0x02, 7, 0x00000000 },
    { 0x03, 0, 0x00000000 }, { 0x03, 1, 0x00000000 }, { 0x03, 2, 0x00000000 }, { 0x03, 3, 0x00000000 },
    { 0x03, 4, 0x00000000 }, { 0x03, 5, 0x00000000 }, { 0x03, 6, 0x00000000 }, { 0x03, 7, 0x00000007 },
    { 0x04, 0, 0x00000000 }, { 0x04, 1, 0x00000000 }, { 0x04, 2, 0x00000000 }, { 0x04, 3, 0x00000000 },
    { 0x04, 4, 0x00000000 }, { 0x04, 5, 0x00000000 }, { 0x04, 6, 0x00000000 }, { 0x04, 7, 0x00000000 },
    { 0x05, 0, 0x00000000 }, { 0x05, 1, 0x00000000 }, { 0x05, 2, 0x00000000 }, { 0x05, 3, 0x00000000 },
    { 0x05, 4, 0x00000000 }, { 0x05, 5, 0x00000000 }, { 0x05, 6, 0x00000000 }, { 0x05, 7, 0x00000000 },
    { 0x06, 0, 0x00000000 }, { 0x06, 1, 0x00000000 }, { 0x06, 2, 0x00000000 }, { 0x06, 3, 0x00000000 },
    { 0x06, 4, 0x00000000 }, { 0x06, 5, 0x00000000 }, { 0x06, 6, 0x00000000 }, { 0x06, 7, 0x00000000 },
    { 0x07, 0, 0x00000000 }, { 0x07, 1, 0x00000000 }, { 0x07, 2, 0x00000000 }, { 0x07, 3, 0x00000000 },
    { 0x07, 4, 0x00000000 }, { 0x07, 5, 0x00000000 }, { 0x07, 6, 0x00000000 }, { 0x07, 7, 0x00000000 },
    { 0x08, 0, 0x00000000 }, { 0x08, 1, 0x00000000 }, { 0x08, 2, 0x00000000 }, { 0x08, 3, 0x00000000 },
    { 0x08, 4, 0x00000000 }, { 0x08, 5, 0x00000000 }, { 0x08, 6, 0x00000000 }, { 0x08, 7, 0x00000000 },
    { 0x09, 0, 0x00000000 }, { 0x09, 1, 0x00000000 }, { 0x09, 2, 0x00000000 }, { 0x09, 3, 0x00000000 },
    { 0x09, 4, 0x00000000 }, { 0x09, 5, 0x00000000 }, { 0x09, 6, 0x00000000 }, { 0x09, 7, 0x00000000 }
};
static const xt_imgpage_t xt_img_ID7936[4] = {
    { 0x00, 0, 0x5C74DD1F }, { 0x00, 1, 0x4D494B52 }, { 0x00, 2, 0x00004F4E }, { 0x00, 3, 0x06AA4854 }
};
static const xt_imgpage_t xt_img_ID7937[4] = {
    { 0x00, 0, 0x628786BE }, { 0x00, 1, 0x4D494B52 }, { 0x00, 2, 0x00004F4E }, { 0x00, 3, 0x06AA4854 }
};
static const xt_imgpage_t xt_img_ID7938[8] = {
    { 0x00, 0, 0x850E6F16 }, { 0x00, 1, 0x11112222 }, { 0x00, 2, 0x33334444 }, { 0x00, 3, 0x55556666 },
    { 0x00, 4, 0x00FFFFFF }, { 0x0F, 0, 0x00000000 }, { 0x0F, 1, 0x00000000 }, { 0x0F, 2, 0x00000000 }
};
static const xt_imgpage_t xt_img_ID7939[12] = {
    { 0x00, 0, 0xFF358A13 }, { 0x00, 1, 0x00000000 }, { 0x00, 2, 0x00000000 }, { 0x00, 3, 0x00000000 },
    { 0x00, 4, 0x00000000 }, { 0x00, 5, 0x00000000 }, { 0x00, 6, 0x00000000 }, { 0x00, 7, 0x00000000 },
    { 0x3C, 0, 0x00000000 }, { 0x3C, 1, 0x00000000 }, { 0x3C, 2, 0x00000000 }, { 0x3C, 3, 0x00000000 }
};
static const xt_imgpage_t xt_img_ID7946[4] = {
    { 0x00, 0, 0x050B3126 }, { 0x00, 1, 0x4D494B52 }, { 0x00, 2, 0x00004F4E }, { 0x00, 3, 0x06AA4854 }
};
static const xt_imgpage_t xt_img_ID8C[11] = {
    { 0x00, 0, 0x0000003F }, { 0x00, 1, 0x00000000 }, { 0x00, 2, 0x00006500 }, { 0x00, 3, 0x00000000 },
    { 0x00, 4, 0x00006500 }, { 0x00, 5, 0x0000FF00 }, { 0x00, 6, 0x000046FF }, { 0x00, 7, 0x00004F46 },
    { 0x01, 0, 0x00000045 }, { 0x01, 1, 0x00000000 }, { 0x01, 2, 0x00000702 }
};
static const xt_imgpage_t xt_img_ID8E[32] = {
    { 0x00, 0, 0x00004471 }, { 0x00, 1, 0x00007000 }, { 0x00, 2, 0x0000D006 }, { 0x00, 3, 0x00000000 },
    { 0x00, 4, 0x00000000 }, { 0x00, 5, 0x00000000 }, { 0x00, 6, 0x00000000 }, { 0x00, 7, 0x00000000 },
    { 0x01, 0, 0x00000000 }, { 0x01, 1, 0x00000000 }, { 0x01, 2, 0x000026B1 }, { 0x01, 3, 0x0000D809 },
    { 0x01, 4, 0x00000000 }, { 0x01, 5, 0x00000000 }, { 0x01, 6, 0x00000000 }, { 0x01, 7, 0x00000000 },
    { 0x02, 0, 0x00000000 }, { 0x02, 1, 0x00000000 }, { 0x02, 2, 0x00000000 }, { 0x02, 3, 0x00000000 },
    { 0x02, 4, 0x00000000 }, { 0x02, 5, 0x00000000 }, { 0x02, 6, 0x00000000 }, { 0x02, 7, 0x00000000 },
    { 0x03, 0, 0x00000000 }, { 0x03, 1, 0x00000000 }, { 0x03, 2, 0x00000000 }, { 0x03, 3, 0x00000000 },
    { 0x03, 4, 0x00000000 }, { 0x03, 5, 0x00000000 }, { 0x03, 6, 0x00000000 }, { 0x03, 7, 0x00000000 }
};
static const xt_imgpage_t xt_img_MQB[77] = {
    { 0x00, 0, 0x00009765 }, { 0x00, 1, 0x0000A000 }, { 0x00, 2, 0x0000FFFC }, { 0x00, 3, 0x0000FFFF },
    { 0x00, 4, 0x00000000 }, { 0x00, 5, 0x00000000 }, { 0x00, 6, 0x00000000 }, { 0x00, 7, 0x00000000 },
    { 0x01, 0, 0x00000000 }, { 0x01, 1, 0x00000000 }, { 0x01, 2, 0x00000000 }, { 0x01, 3, 0x00000000 },
    { 0x01, 4, 0x00001218 }, { 0x01, 5, 0x00000A51 }, { 0x01, 6, 0x00000AB8 }, { 0x01, 7, 0x0000380A },
    { 0x03, 7, 0x00000007 }, { 0x04, 0, 0x00000000 }, { 0x04, 1, 0x00000000 }, { 0x04, 2, 0x00000000 },
    { 0x04, 3, 0x00000000 }, { 0x04, 4, 0x00000000 }, { 0x04, 5, 0x00000000 }, { 0x04, 6, 0x00000000 },
    { 0x04, 7, 0x00000000 }, { 0x07, 3, 0x000028D6 }, { 0x07, 4, 0x00008377 }, { 0x0C, 0, 0x00008810 },
    { 0x0C, 1, 0x00004788 }, { 0x0C, 2, 0x0000D645 }, { 0x0C, 3, 0x000045A0 }, { 0x0C, 4, 0x00000044 },
    { 0x0C, 5, 0x00003641 }, { 0x0C, 6, 0x00003030 }, { 0x0C, 7, 0x00003931 }, { 0x0D, 0, 0x00004748 },
    { 0x0D, 1, 0x00003341 }, { 0x0D, 2, 0x00003735 }, { 0x0D, 3, 0x00003539 }, { 0x0D, 4, 0x00003639 },
    { 0x0D, 5, 0x00003547 }, { 0x0D, 6, 0x00000000 }, { 0x0D, 7, 0x00000000 }, { 0x0E, 0, 0x00000000 },
    { 0x0E, 1, 0x00000000 }, { 0x0E, 2, 0x00000000 }, { 0x0E, 3, 0x00000000 }, { 0x0E, 4, 0x00000000 },
    { 0x0E, 5, 0x00000000 }, { 0x0E, 6, 0x00000000 }, { 0x0E, 7, 0x00000000 }, { 0x0F, 0, 0x00000000 },
    { 0x0F, 1, 0x00000000 }, { 0x0F, 2, 0x00000000 }, { 0x0F, 3, 0x00000000 }, { 0x0F, 4, 0x00000000 },
    { 0x0F, 5, 0x00000000 }, { 0x0F, 6, 0x00000000 }, { 0x0F, 7, 0x00000000 }, { 0x10, 0, 0x00002101 },
    { 0x10, 1, 0x00001100 }, { 0x10, 2, 0x00000000 }, { 0x10, 3, 0x00000000 }, { 0x10, 4, 0x00000000 },
    { 0x10, 5, 0x00000000 }, { 0x10, 6, 0x00000000 }, { 0x10, 7, 0x00000000 }, { 0x11, 0, 0x00000000 },
    { 0x11, 1, 0x00000000 }, { 0x11, 2, 0x00000000 }, { 0x11, 3, 0x00000000 }, { 0x11, 4, 0x00000000 },
    { 0x11, 5, 0x00000000 }, { 0x11, 6, 0x00000000 }, { 0x11, 7, 0x00000000 }, { 0x12, 0, 0x00000305 },
    { 0x12, 1, 0x00000000 }
};

static const xt_image_t xt_images[] = {
    { "ID11", 0x8C, xt_img_ID11, ARRAYLEN(xt_img_ID11), 0x01F0008C, 0x00000000 },
    { "ID12", 0x8C, xt_img_ID12, ARRAYLEN(xt_img_ID12), 0x01F0008C, 0x00000000 },
    { "ID13", 0x8C, xt_img_ID13, ARRAYLEN(xt_img_ID13), 0x01F0008C, 0x00000000 },
    { "ID48", 0x48, xt_img_ID48, ARRAYLEN(xt_img_ID48), 0x3FF00048, 0x00000000 },
    { "ID4A", 0x4A, xt_img_ID4A, ARRAYLEN(xt_img_ID4A), 0x01F0004A, 0x00000000 },
    { "ID4C", 0x4C, xt_img_ID4C, ARRAYLEN(xt_img_ID4C), 0x01F0004C, 0x00000000 },
    { "ID4D", 0x4D, xt_img_ID4D, ARRAYLEN(xt_img_ID4D), 0x01F0004D, 0x00000000 },
    { "ID4E", 0x64, xt_img_ID4E, ARRAYLEN(xt_img_ID4E), 0x01F00064, 0x00000000 },
    { "ID7935", 0x35, xt_img_ID7935, ARRAYLEN(xt_img_ID7935), 0x7FF00035, 0x00000240 },
    { "ID7936", 0x36, xt_img_ID7936, ARRAYLEN(xt_img_ID7936), 0x01F00036, 0x00000000 },
    { "ID7937", 0x37, xt_img_ID7937, ARRAYLEN(xt_img_ID7937), 0x01F00037, 0x00000000 },
    { "ID7938", 0x47, xt_img_ID7938, ARRAYLEN(xt_img_ID7938), 0x01F00047, 0x00000000 },
    { "ID7939", 0x39, xt_img_ID7939, ARRAYLEN(xt_img_ID7939), 0x01F00039, 0x00000000 },
    { "ID7946", 0x46, xt_img_ID7946, ARRAYLEN(xt_img_ID7946), 0x01F00046, 0x00000000 },
    { "ID8C", 0x8C, xt_img_ID8C, ARRAYLEN(xt_img_ID8C), 0x01F0008C, 0x00000000 },
    { "ID8E", 0x8E, xt_img_ID8E, ARRAYLEN(xt_img_ID8E), 0x01F0008E, 0x00000000 },
    { "MQB", 0x8B, xt_img_MQB, ARRAYLEN(xt_img_MQB), 0x3FF0008B, 0x00000000 },
};

// Which nonce a given power-up delay produces is not quite repeatable: the window the chip takes
// drifts a bit either way, so some nonces sit on a wide plateau and some are only hit now and then.
// The search tries each nonce that reaches far enough, in order, and runs at the first one it lands
// on; that ordering is the only thing separating them, since a built session is not tied to the
// nonce the VVDI happened to record it at.
static const xt57_ks_t xt57_ks_CDB9AE8C = {
    0xCDB9AE8C, 0x1BA796D1, 182, 181, xt57_ksC_k0, xt57_ksC_K, 45000, 0, 119500, 400, 2
};
static const xt57_ks_t xt57_ks_6DCD7463 = {
    0x6DCD7463, 0x519F0319, 21, 21, xt57_ks6_k0, xt57_ks6_K, 45000, 0, 119500, 400, 2
};
static const xt57_ks_t *const xt57_ks_all[] = { &xt57_ks_CDB9AE8C, &xt57_ks_6DCD7463 };

// XT37 keystream.  Measured on one chip but valid for the family: K is a function of (nonce, index)
// alone, and k0 additionally of the select payload, which is the same `C0 "XT37" 123456789ABCDEF0`
// on every XT37.  Only the power-up delay that reaches a nonce is per-tag (docs/xt37.md SS4a), which
// is what the search below is for.  `sel_auto` is 1 so the family bytes come from the chip itself.
static const xt57_ks_t xt37_ks_AD4BD054 = {
    0xAD4BD054, 0x80DC1109, 178, 178, xt37_ks_AD4BD054_k0, xt37_ks_AD4BD054_K, 200000, 1, 119684, 900, 2
};
static const xt57_ks_t xt37_ks_D4BD0543 = {
    0xD4BD0543, 0x1A14D3D0, 170, 170, xt37_ks_D4BD0543_k0, xt37_ks_D4BD0543_K, 200000, 1, 119652, 900, 2
};
// Deepest first: a long image write wants the longest keystream, and both reach any one chip.
static const xt57_ks_t *const xt37_ks_all[] = { &xt37_ks_AD4BD054, &xt37_ks_D4BD0543 };

// one plaintext exchange of a session
typedef struct {
    uint8_t  kind;        // 'S' select, 'R' read, 'W' write, 'E' the 7F terminator,
    //                       'X' end this session here and start the next (not a frame)
    uint8_t  arg;         // select: block address; read/write: page
    uint32_t data;        // write: value
} xt57_step_t;

// Returns false if a step needs a data keystream the table does not hold (see `nfull`).
static bool xt57_build_session(const xt57_step_t *st, int n, const xt57_ks_t *ks,
                               xt57_frame_t *out) {
    for (int i = 0; i < n; i++) {
        if (i + 1 > ks->nfull && st[i].kind != 'E') {
            return false;
        }
        const int idx = i + 1;                       // exchange index, 1-based
        const uint8_t k0 = ks->k0[idx];
        const uint32_t K = ks->K[idx];
        memset(&out[i], 0, sizeof(out[i]));
        switch (st[i].kind) {
            case 'S': {                              // 11011SSS, then the block
                out[i].bits = 16;
                out[i].data[0] = (uint8_t)((0xD8 | ((st[i].arg >> 5) & 7)) ^ k0);
                out[i].data[1] = (uint8_t)((st[i].arg & 0x1F) ^ ((K >> 24) & 0xFF));
                break;
            }
            case 'R': {                              // 00000AAA
                out[i].bits = 8;
                out[i].data[0] = (uint8_t)((st[i].arg & 7) ^ k0);
                break;
            }
            case 'W': {                              // 10000AAA + 4 data bytes + crc over the wire
                const uint32_t c = st[i].data ^ K;
                out[i].bits = 48;
                out[i].data[0] = (uint8_t)((0x80 | (st[i].arg & 7)) ^ k0);
                out[i].data[1] = (uint8_t)(c >> 24);
                out[i].data[2] = (uint8_t)(c >> 16);
                out[i].data[3] = (uint8_t)(c >> 8);
                out[i].data[4] = (uint8_t)(c);
                out[i].data[5] = xt57_crc8(out[i].data, 5);
                break;
            }
            default: {                               // 'E' - the 7F terminator
                out[i].bits = 8;
                out[i].data[0] = (uint8_t)(0x7F ^ k0);
                break;
            }
        }
        // give the chip its programming time after every write
        if (i > 0 && st[i - 1].kind == 'W') {
            out[i].gap_t0 = XT57_PROG_T0;
        }
    }
    return true;
}

// What the VVDI's set-7935 session puts in each of the ten addresses it wipes
// (docs/xt57b-fullread.md SS12.9).  Addresses 00-07 are channel-A bank 0 pages 0x00-0x3F,
// 08-09 are bank 1 pages 0x00-0x0F; the odd slot is bank-0 page 0x1F, which xh_recipe()'s
// own 7935 case also singles out.
static uint32_t xt57_7935_fill(uint8_t addr, uint8_t slot) {
    if (addr <= 0x03) {
        return (addr == 0x03 && slot == 7) ? 0x5A5A0707 : 0x5A5A0000;
    }
    if (addr <= 0x07) {
        return 0x5A5A5A5A;
    }
    return 0x00000000;
}

// Responses that fit one reply packet, so the most frames a session may hold if every read-back
// is to be checked.  A wiped address costs 17 (a select and eight write/read-back pairs), so
// three of them plus the terminator is the largest round number that fits.
#define XT57_CHUNK_FRAMES  (XHORSE_XT57_MAXPOST - 12)

// Build the exchanges that make the chip emulate `t` with `uid`.  Returns the number of steps,
// or -1 if the type has no XMICRO recipe.  `*arm_at` is set to the 1-based index of the write
// that selects a broadcast type, or 0 if the type does not broadcast.  The chip answers the rest
// of the session normally - it only starts broadcasting, and stops answering XMICRO, at the next
// power-up - but the index is kept so that a silent read-back after it is not called a failure.
//
// The image block and the channel-A bank 0 hold the same pages in the same order, so the page
// values and the write order are the ones xh_recipe() uses in armsrc/xhorse.c.
// Build a session from an image decoded out of the tool's own set capture: every page it wrote, in
// order, then the config block - page 5 first where the capture wrote one, then page 4, because the
// type byte is what makes the chip emulate and must land last (xt57b-fullread.md SS12.7).  'X' marks
// a chunk boundary: run_session turns each chunk into its own session, which is what keeps a
// 170-exchange image inside the 64 responses and 560 script bytes one reply can carry.
#define XT57_IMG_CHUNK   24
#define XT_ARG_CONFIG    0x7F       // segment 3, block 0x1F - the config block (docs/xt37.md SS5o)

static int xt57_build_steps_image(const xt_image_t *im, xt57_step_t *out, int maxn, int *arm_at) {
    int n = 0, in_chunk = 0;
    uint8_t cur = 0xFF;
    *arm_at = 0;
    for (int i = 0; i < im->n; i++) {
        if (n + 4 > maxn) {
            return -1;
        }
        if (in_chunk >= XT57_IMG_CHUNK) {
            out[n++] = (xt57_step_t) { 'X', 0, 0 };
            in_chunk = 0;
            cur = 0xFF;
        }
        if (im->pages[i].arg != cur) {
            out[n++] = (xt57_step_t) { 'S', im->pages[i].arg, 0 };
            cur = im->pages[i].arg;
        }
        out[n++] = (xt57_step_t) { 'W', im->pages[i].page, im->pages[i].val };
        out[n++] = (xt57_step_t) { 'R', im->pages[i].page, 0 };
        in_chunk++;
    }
    if (n + 7 > maxn) {
        return -1;
    }
    out[n++] = (xt57_step_t) { 'X', 0, 0 };
    out[n++] = (xt57_step_t) { 'S', XT_ARG_CONFIG, 0 };
    if (im->cfg5 != 0) {
        out[n++] = (xt57_step_t) { 'W', 5, im->cfg5 };
        out[n++] = (xt57_step_t) { 'R', 5, 0 };
    }
    out[n++] = (xt57_step_t) { 'W', 4, im->cfg4 };
    // `arm_at` means "this write makes the chip talk first and go deaf to XMICRO", which only the
    // broadcast types do: the EM4102-style family (type byte 0x8C - ID11/ID12/ID13/ID8C) and a
    // blanked PCF7935 (0x35).  Marking it for every type would warn about a reader-talk-first chip
    // like 7936 that stays perfectly reachable.
    if (im->type == 0x8C || im->type == 0x35) {
        *arm_at = n;
    }
    out[n++] = (xt57_step_t) { 'R', 4, 0 };
    return n;
}

// The VVDI's own reset, frame for frame from `xhorse-XT37b-reset-from-ID48.pm3` decrypted at nonce
// C1CBFC8C (docs/xt37.md SS5u).  The order is the reverse of a set: **the config word goes in first**,
// then the image.  A set must finish the image before the type byte, because the type byte is what
// makes the chip emulate (xt57b-fullread.md SS12.7); a reset is coming back *from* an emulated type,
// so it puts the chip on a reachable one immediately and only then rewrites the image - which also
// means a session that dies halfway leaves a chip that still answers.
static int xt57_build_steps_reset(const uint8_t *uid, xt57_step_t *out, int maxn, int *arm_at) {
    if (maxn < 14) {
        return -1;
    }
    int n = 0;
    *arm_at = 0;
    out[n++] = (xt57_step_t) { 'S', XT_ARG_CONFIG, 0 };
    out[n++] = (xt57_step_t) { 'R', 7, 0 };                  // the VVDI reads page 7 first
    out[n++] = (xt57_step_t) { 'W', 4, 0x01F00036 };         // ID7936/ID46, the factory personality
    out[n++] = (xt57_step_t) { 'R', 4, 0 };
    out[n++] = (xt57_step_t) { 'S', 0x00, 0 };
    out[n++] = (xt57_step_t) { 'W', 0, xt57_be32(uid) };
    out[n++] = (xt57_step_t) { 'R', 0, 0 };
    out[n++] = (xt57_step_t) { 'W', 1, 0x4D494B52 };         // "MIKR"
    out[n++] = (xt57_step_t) { 'R', 1, 0 };
    out[n++] = (xt57_step_t) { 'W', 2, 0x00004F4E };         // "\0\0ON"
    out[n++] = (xt57_step_t) { 'R', 2, 0 };
    out[n++] = (xt57_step_t) { 'W', 3, 0x06AA4854 };         // HITAG2 config page
    out[n++] = (xt57_step_t) { 'R', 3, 0 };
    out[n++] = (xt57_step_t) { 'E', 0, 0 };
    return n;
}

static int xt57_build_steps(const xh_type_t *t, const uint8_t *uid,
                            xt57_step_t *out, int maxn, int *arm_at) {
    uint32_t img[5] = {0};
    uint8_t order[5];
    int npg;

    switch (t->recipe) {   // plain uint8_t: only the recipes with an XMICRO image are listed
        case XHORSE_R_HITAG2: {                      // ID46 / 7936 / 7937 / 7946
            img[0] = xt57_be32(uid);
            img[1] = 0x4D494B52;                     // "MIKR"
            img[2] = 0x00004F4E;                     // "..ON"
            img[3] = 0x06AA4854;                     // config, "HT"
            npg = 4;
            for (int i = 0; i < 4; i++) {
                order[i] = (uint8_t)i;
            }
            break;
        }
        case XHORSE_R_ID11:
        case XHORSE_R_ID12: {
            uint16_t pages[5];
            xh_pack_em8(uid, (t->recipe == XHORSE_R_ID11) ? 0x0010 : 0x0014, pages);
            for (int i = 0; i < 5; i++) {
                img[i] = pages[i];
            }
            npg = 5;
            memcpy(order, (const uint8_t[]) { 2, 1, 4, 3, 0 }, 5);
            break;
        }
        case XHORSE_R_ID13: {
            uint8_t s8[8];
            xh_id13_stream(uid, s8);
            img[0] = 0x00000018;
            img[1] = (uint32_t)((s8[2] << 8) | s8[3]);
            img[2] = (uint32_t)((s8[0] << 8) | s8[1]);
            img[3] = (uint32_t)((s8[6] << 8) | s8[7]);
            img[4] = (uint32_t)((s8[4] << 8) | s8[5]);
            npg = 5;
            memcpy(order, (const uint8_t[]) { 2, 1, 4, 3, 0 }, 5);
            break;
        }
        case XHORSE_R_7935:                          // ID33 / ID40..ID44: no image, a full wipe
            npg = 0;
            memset(order, 0, sizeof(order));
            break;
        default:
            return -1;
    }

    // The config block, exactly as the VVDI writes it; only the type byte varies.  7935 is the
    // one type whose type byte goes in without the 0x01F00000 bits - the VVDI writes a bare
    // 00000035 - and the chip broadcasts from the next power-up all the same, so those bits are
    // not the emulation enable.  (A factory-fresh chip reads 00000036 there too: xt57-auth.md
    // SS16.4.)  What they are is still unknown.
    const uint32_t cfgtype = (t->recipe == XHORSE_R_7935)
                              ? (uint32_t)t->typebyte
                              : (0x01F00000u | t->typebyte);
    const xt57_step_t cfg[11] = {
        { 'S', 0x7F, 0 },
        { 'R', 7,    0 },
        { 'W', 4,    cfgtype },
        { 'R', 4,    0 },
        { 'R', 5,    0 },
        { 'W', 5,    0x00000049 },
        { 'R', 5,    0 },
        { 'W', 6,    0x0000C900 },
        { 'R', 6,    0 },
        { 'W', 6,    0x00000000 },
        { 'R', 6,    0 },
    };

    // The 7935 wipe is ten addresses of eight pages, which is far more than one session's worth
    // of responses, so it is cut into chunks; the type byte goes in last, as the VVDI does it.
    if (t->recipe == XHORSE_R_7935) {
        if (10 * 17 + 10 + (int)ARRAYLEN(cfg) + 1 > maxn) {
            return -1;
        }
        int n = 0, inchunk = 0;
        *arm_at = 0;
        for (uint8_t a = 0x00; a <= 0x09; a++) {
            if (inchunk + 17 > XT57_CHUNK_FRAMES) {
                out[n++] = (xt57_step_t) { 'E', 0, 0 };
                out[n++] = (xt57_step_t) { 'X', 0, 0 };
                inchunk = 0;
            }
            out[n++] = (xt57_step_t) { 'S', a, 0 };
            for (uint8_t sl = 0; sl < 8; sl++) {
                out[n++] = (xt57_step_t) { 'W', sl, xt57_7935_fill(a, sl) };
                out[n++] = (xt57_step_t) { 'R', sl, 0 };
            }
            inchunk += 17;
        }
        out[n++] = (xt57_step_t) { 'E', 0, 0 };
        out[n++] = (xt57_step_t) { 'X', 0, 0 };
        for (int i = 0; i < (int)ARRAYLEN(cfg); i++) {
            out[n++] = cfg[i];
        }
        out[n++] = (xt57_step_t) { 'E', 0, 0 };
        return n;
    }

    // For a broadcast type the image goes in before the type byte, which is the order the VVDI
    // uses (docs/xt57b-fullread.md SS12.7): once the type byte is in, the next power-up has the
    // chip emulating, and anything still missing from the image can no longer be written.
    const bool broadcast = (t->typebyte == 0x8C);
    if (2 + 2 * npg + (int)ARRAYLEN(cfg) > maxn) {
        return -1;
    }

    int n = 0;
    *arm_at = 0;
    for (int half = 0; half < 2; half++) {
        if ((half == 0) == broadcast) {              // the image block
            out[n++] = (xt57_step_t) { 'S', 0x00, 0 };
            for (int i = 0; i < npg; i++) {
                out[n++] = (xt57_step_t) { 'W', order[i], img[order[i]] };
                out[n++] = (xt57_step_t) { 'R', order[i], 0 };
            }
        } else {                                     // the config block
            for (int i = 0; i < (int)ARRAYLEN(cfg); i++) {
                out[n++] = cfg[i];
                if (broadcast && cfg[i].kind == 'W' && cfg[i].arg == 4) {
                    *arm_at = n;                     // 1-based: this write arms the chip
                }
            }
        }
    }
    out[n++] = (xt57_step_t) { 'E', 0, 0 };
    return n;
}

// Sweep the power-up delay until the session nonce is the one whose keystream we hold.
// Returns the delay in us, or -1.
static int32_t xt57_find_nonce(const xt57_ks_t *ks, int32_t lo, int32_t hi) {
    lf_xhorse_xt57_t p;
    memset(&p, 0, sizeof(p));
    p.sweep = XT57_SWEEP_POWERUP;
    p.answer_mode = XT57_ANS_NONE;
    p.step = ks->step;
    p.off_us = (uint32_t)ks->off_us;
    p.selbits = XHORSE_XT57_SELLEN * 8;
    memcpy(p.select, xt57_sel_default, XHORSE_XT57_SELLEN);
    // An XT57 table replays a keystream recorded under an "XT57" select, so that frame must go out
    // exactly as recorded; an XT37 table wants the chip's own family bytes instead, which `sel_auto`
    // asks the firmware for.  A select carrying the wrong family is simply refused (docs/xt37.md SS5).
    p.sel_auto = ks->sel_auto;

    for (int32_t base = lo; base <= hi; base += ks->step * XHORSE_XT57_MAXSESS) {
        p.base = base;
        p.count = XHORSE_XT57_MAXSESS;
        clearCommandBuffer();
        SendCommandNG(CMD_LF_XHORSE_XT57_NONCE, (uint8_t *)&p, sizeof(p));
        PacketResponseNG resp;
        if (WaitForResponseTimeout(CMD_LF_XHORSE_XT57_NONCE, &resp, 60000) == false) {
            PrintAndLogEx(WARNING, "timeout while searching for the nonce");
            return -1;
        }
        const lf_xhorse_xt57_reply_t *r = (const lf_xhorse_xt57_reply_t *)resp.data.asBytes;
        for (int i = 0; i < r->count; i++) {
            if (r->rec[i].nonce_bits != 32) {
                continue;
            }
            uint32_t n = ((uint32_t)r->rec[i].nonce[0] << 24) | ((uint32_t)r->rec[i].nonce[1] << 16)
                         | ((uint32_t)r->rec[i].nonce[2] << 8) | r->rec[i].nonce[3];
            if (n == ks->nonce) {
                return base + ks->step * i;
            }
        }
        if (kbd_enter_pressed()) {
            return -1;
        }
    }
    return -1;
}

// Run one session: the steps given, at a nonce and power-up delay already found.  Read-backs
// are decrypted and compared; the counters accumulate across the chunks of a plan.
static int xt57_run_chunk(const xt57_ks_t *ks, int32_t at, const xt57_step_t *st, int nst,
                          int arm_at, int *checked, int *bad, int *silent) {
    xt57_frame_t frames[XT57_MAXSTEPS];
    if (xt57_build_session(st, nst, ks, frames) == false) {
        PrintAndLogEx(FAILED, "the keystream at nonce %08X does not reach exchange %d",
                      ks->nonce, nst);
        return PM3_EINVARG;
    }

    lf_xhorse_xt57_t p;
    memset(&p, 0, sizeof(p));
    p.sweep = XT57_SWEEP_POWERUP;
    p.answer_mode = XT57_ANS_FIXED;
    p.answer = ks->answer;
    p.want = ks->nonce;
    p.base = at;
    p.step = 0;
    p.off_us = (uint32_t)ks->off_us;
    p.selbits = XHORSE_XT57_SELLEN * 8;
    memcpy(p.select, xt57_sel_default, XHORSE_XT57_SELLEN);
    // An XT57 table replays a keystream recorded under an "XT57" select, so that frame must go out
    // exactly as recorded; an XT37 table wants the chip's own family bytes instead, which `sel_auto`
    // asks the firmware for.  A select carrying the wrong family is simply refused (docs/xt37.md SS5).
    p.sel_auto = ks->sel_auto;
    p.npost = (uint8_t)nst;
    p.rfirst = 1;
    int packed = xt57_pack_script(frames, nst, p.script, sizeof(p.script));
    if (packed < 0) {
        PrintAndLogEx(FAILED, "script does not fit");
        return PM3_EINVARG;
    }
    p.scriptlen = (uint16_t)packed;
    p.count = 30;

    clearCommandBuffer();
    SendCommandNG(CMD_LF_XHORSE_XT57_NONCE, (uint8_t *)&p, sizeof(p));
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_LF_XHORSE_XT57_NONCE, &resp, 120000) == false) {
        PrintAndLogEx(WARNING, "timeout while waiting for reply");
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        return PM3_ETIMEOUT;
    }
    if (resp.length < sizeof(lf_xhorse_xt57_post_reply_t) - sizeof(xt57_presp_t) * XHORSE_XT57_MAXPOST) {
        PrintAndLogEx(FAILED, "the wanted nonce did not come up - this session wrote nothing");
        return PM3_ESOFT;
    }
    const lf_xhorse_xt57_post_reply_t *q = (const lf_xhorse_xt57_post_reply_t *)resp.data.asBytes;

    // Every write is followed by a read-back; decrypt it and compare.
    uint8_t addr = 0;
    for (int i = 0; i < nst; i++) {
        if (st[i].kind == 'S') {
            addr = st[i].arg;
        }
        if (st[i].kind != 'W' || i + 1 >= nst || st[i + 1].kind != 'R') {
            continue;
        }
        // A broadcast type byte is live from the next power-up, so a read-back after it should
        // still come back - but tolerate silence there rather than call it a failure.
        const bool armed = (arm_at != 0) && (i + 1 >= arm_at);
        const int rb = i + 1;                       // the read-back frame
        if (rb >= q->npost || q->presp[rb].bits < 32) {
            PrintAndLogEx(INFO, "   %02X    %d   %08X   %s", addr, st[i].arg, st[i].data,
                          armed ? _YELLOW_("silent (armed)") : _RED_("no reply"));
            if (armed) {
                (*silent)++;
            } else {
                (*bad)++;
            }
            continue;
        }
        uint32_t got = ((uint32_t)q->presp[rb].data[0] << 24) | ((uint32_t)q->presp[rb].data[1] << 16)
                       | ((uint32_t)q->presp[rb].data[2] << 8) | q->presp[rb].data[3];
        got ^= ks->K[rb + 1];                       // read-back is exchange rb+1 (1-based)
        (*checked)++;
        if (got == st[i].data) {
            PrintAndLogEx(INFO, "   %02X    %d   %08X   " _GREEN_("%08X ok"),
                          addr, st[i].arg, st[i].data, got);
        } else {
            (*bad)++;
            PrintAndLogEx(INFO, "   %02X    %d   %08X   " _RED_("%08X MISMATCH"),
                          addr, st[i].arg, st[i].data, got);
        }
    }
    return PM3_SUCCESS;
}

// Find the nonce, then run the plan.  A plan longer than one session's budget - the script
// buffer, and the responses that fit one reply packet - is cut into chunks at the 'X' steps
// the builder puts in; each chunk is its own authenticated session at the same power-up delay,
// which is free to do because a built session is not tied to anything the previous one did.
static int xt57_run_session(const xt57_step_t *st, int nst, int arm_at,
                            int32_t centre, int32_t span) {
    const xt57_ks_t *ks = NULL;
    int32_t at = -1;
    int usable = 0;

    int longest = 0, nchunk = 0;
    for (int i = 0, run = 0; i < nst; i++) {
        if (st[i].kind == 'X') {
            longest = MAX(longest, run);
            run = 0;
            nchunk++;
        } else {
            run++;
            if (i + 1 == nst) {
                longest = MAX(longest, run);
                nchunk++;
            }
        }
    }

    // Which family is on the coil decides which keystream applies - the answer that opens a session
    // is family-specific (docs/xt37.md SS5a) - so ask the chip before choosing a table.
    lf_xhorse_rdopt_t popt;
    memset(&popt, 0, sizeof(popt));
    lf_xhorse_info_t pnfo;
    bool is37 = false;
    if (xh_request_info(CMD_LF_XHORSE_INFO, (uint8_t *)&popt, sizeof(popt), &pnfo) == PM3_SUCCESS) {
        is37 = (pnfo.bchip && pnfo.bchip_type_len >= 4
                && pnfo.bchip_type[2] == '3' && pnfo.bchip_type[3] == '7');
    }
    const xt57_ks_t *const *tbl = is37 ? xt37_ks_all : xt57_ks_all;
    size_t ntbl = is37 ? ARRAYLEN(xt37_ks_all) : ARRAYLEN(xt57_ks_all);
    if (is37) {
        PrintAndLogEx(INFO, "chip family " _YELLOW_("XT37") " - using its keystream");
    }

    for (size_t i = 0; i < ntbl && at < 0; i++) {
        if (longest > tbl[i]->nex) {
            continue;                               // keystream too short for this session
        }
        usable++;
        ks = tbl[i];
        // Steer first - read where the stream is and compute the delay - and only sweep if that
        // cannot be done (docs/xt37.md SS5r).
        int32_t want = 0;
        if (xt_steer(ks, ks->centre, &want, true) == PM3_SUCCESS) {
            at = want;
            break;
        }
        PrintAndLogEx(INFO, "searching for nonce " _YELLOW_("%08X") " between %d and %d us",
                      ks->nonce, centre - span, centre + span);
        at = xt57_find_nonce(ks, centre - span, centre + span);
    }
    if (usable == 0) {
        PrintAndLogEx(FAILED, "that session is %d exchanges; no recovered keystream reaches "
                      "that far", longest);
        return PM3_EINVARG;
    }
    if (at < 0) {
        PrintAndLogEx(FAILED, "no usable nonce came up - nothing was written");
        PrintAndLogEx(HINT, "widen with `--span`, or check the tag answers `" _YELLOW_("lf xhorse info") "`");
        return PM3_ESOFT;
    }
    PrintAndLogEx(SUCCESS, "nonce %08X at power-up " _GREEN_("%d us"), ks->nonce, at);
    if (nchunk > 1) {
        PrintAndLogEx(INFO, "%d exchanges: %d sessions at that delay", nst - (nchunk - 1), nchunk);
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, " addr page   wrote      read back");
    int checked = 0, bad = 0, silent = 0;
    for (int i = 0, c = 1; i < nst; c++) {
        int n = 0;
        while (i + n < nst && st[i + n].kind != 'X') {
            n++;
        }
        if (nchunk > 1) {
            PrintAndLogEx(INFO, " --- session %d of %d, %d exchanges ---", c, nchunk, n);
        }
        // arm_at is a 1-based index into the whole plan; make it chunk-local.  A chunk that
        // starts after the arming write is entirely "after it", so its local index is 1.
        const int arm_here = (arm_at == 0) ? 0 : (arm_at > i) ? arm_at - i : 1;
        const int rc = xt57_run_chunk(ks, at, st + i, n, arm_here, &checked, &bad, &silent);
        if (rc != PM3_SUCCESS) {
            if (c > 1) {
                PrintAndLogEx(WARNING, "session %d of %d failed: %d writes are already in, the "
                              "rest are not", c, nchunk, checked);
                PrintAndLogEx(HINT, "run the same command again - the sessions before it just "
                              "rewrite what they already wrote");
            }
            return rc;
        }
        i += n + 1;                                 // skip the 'X'
    }

    PrintAndLogEx(NORMAL, "");
    if (bad) {
        PrintAndLogEx(FAILED, "%d of %d read-backs did not match", bad, checked + bad);
        return PM3_ESOFT;
    }
    if (silent) {
        PrintAndLogEx(SUCCESS, "%d writes verified; %d went unanswered after the type byte",
                      checked, silent);
    } else {
        PrintAndLogEx(SUCCESS, "all %d writes verified", checked);
    }
    return PM3_SUCCESS;
}

// Types with an XMICRO recipe: an image that fits block 0x00 and a session short enough for the
// keystream.  Everything else needs either pages outside that block or a longer session.
// An image decoded from the tool's own set session (SS5p), used for the types that have no built
// recipe.  Verbatim: whatever the capture wrote, including fields a user might want to choose.
static const xt_image_t *xt57_decoded_image(const xh_type_t *t) {
    for (size_t i = 0; i < ARRAYLEN(xt_images); i++) {
        if (xt_images[i].type == t->typebyte) {
            return &xt_images[i];
        }
    }
    return NULL;
}

static bool xt57_type_supported(const xh_type_t *t) {
    if (xt57_decoded_image(t) != NULL) {
        return true;                 // a decoded image covers it even without a built recipe
    }
    switch (t->recipe) {   // plain uint8_t: only the recipes with an XMICRO image are listed
        case XHORSE_R_HITAG2:
        case XHORSE_R_ID11:
        case XHORSE_R_ID12:
        case XHORSE_R_ID13:
        case XHORSE_R_7935:
            return true;
        default:
            return false;
    }
}

static int CmdLFXhorseXt57Set(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse xt57 set",
                  "Set which transponder an XT57B emulates - the channel-B counterpart of\n"
                  "`lf xhorse configure`.\n"
                  "\n"
                  _RED_("This writes to the chip") ".  It sends the VVDI's own set session, rebuilt from\n"
                  "a recovered keystream rather than replayed, so the type and UID can be chosen.\n"
                  "\n"
                  "Only the types there is a recovered keystream long enough for are offered:\n"
                  "  " _YELLOW_("ID46 / 7936 / 7937 / 7946") "   HITAG2 family, 4 byte UID\n"
                  "  " _YELLOW_("ID11 / ID12") "               Temic fixed-code, 8 byte UID\n"
                  "  " _YELLOW_("ID13") "                      Megamos 13 / EM4102, 5 byte UID\n"
                  "  " _YELLOW_("7935 / ID33 / ID40..ID44") "    PCF7935 (SECT), blanked, no UID\n"
                  "\n"
                  "7935 is a wipe, not an image: it fills all ten addresses of the emulated store\n"
                  "and takes no -u.  That is 182 exchanges, more than fits one reply, so it runs\n"
                  "as five sessions at the same power-up delay.  It needs " _YELLOW_("--wipe") " as well,\n"
                  "because it overwrites the factory tables at addresses 00, 01, 03, 04, 08 and 09\n"
                  "and " _RED_("nothing puts them back") " - not `restore`, and not the VVDI's own reset.\n"
                  "\n"
                  "The chip then broadcasts a 4096 us frame and goes deaf to XMICRO.  That part is\n"
                  "recoverable: `lf xhorse xt57 reset` sends the 7935 word and frees it\n"
                  "(docs/xt57b-fullread.md SS12.11).\n"
                  "\n"
                  "ID11, ID12 and ID13 broadcast.  The session itself completes normally, but from\n"
                  "the next power-up the chip talks first and stops answering XMICRO, and only\n"
                  "`lf xhorse xt57 reset` gets it back.\n"
                  "\n"
                  "ID13's -u is the VVDI's user-ID field, not what a reader sees: the encoding\n"
                  "bit-reverses each byte, so -u 20471BB01B reads as EM410x 04E2D80DD8.\n"
                  "\n"
                  "The session can only be built at a nonce there is recovered keystream for, so\n"
                  "one of those is searched for first - which power-up delay produces a given\n"
                  "nonce differs from chip to chip.",
                  "lf xhorse xt57 set -t 7946 -u DE269A2F\n"
                  "lf xhorse xt57 set -t ID46 -u 11223344\n"
                  "lf xhorse xt57 set -t ID11 -u 0011223344556677\n"
                  "lf xhorse xt57 set -t ID13 -u 20471BB01B\n"
                  "lf xhorse xt57 set -t 7935");
    void *argtable[] = {
        arg_param_begin,
        arg_str1("t", "type", "<name>", "emulated-type name, e.g. ID46 / 7946 / ID11 / ID13"),
        arg_str0("u", "uid",  "<hex>",  "emulated UID; length depends on the type"),
        arg_int0(NULL, "base", "<dec>", "centre of the power-up search in us (def 119500)"),
        arg_int0(NULL, "span", "<dec>", "half-width of the search in us (def 400)"),
        arg_lit0(NULL, "wipe", "required for 7935: yes, blank the chip"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    char typestr[32] = {0};
    int tlen = 0;
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)typestr, sizeof(typestr) - 1, &tlen);
    uint8_t uid[XHORSE_UID_MAXLEN] = {0};
    int uidlen = 0;
    CLIGetHexWithReturn(ctx, 2, uid, &uidlen);
    int centre = arg_get_int_def(ctx, 3, XT57_SEARCH_CENTRE);
    int span = arg_get_int_def(ctx, 4, XT57_SEARCH_SPAN);
    bool wipe_ok = arg_get_lit(ctx, 5);
    CLIParserFree(ctx);

    const xh_type_t *t = resolve_type(typestr);
    if (t == NULL) {
        PrintAndLogEx(FAILED, "unknown type token " _YELLOW_("%s"), typestr);
        PrintAndLogEx(HINT, "run `" _YELLOW_("lf xhorse types") "` for the names");
        return PM3_EINVARG;
    }
    // Which image to write depends on the family, so ask the chip first.  On an XT37 the decoded
    // capture wins even where a built recipe exists: the built ones carry the XT57's config pages
    // (page 5 = 0x49, page 6 = 0xC900), and an XT37's own sets write page 4 and nothing else
    // (docs/xt37.md SS5p).  On an XT57 the built recipes win, because those are the ones a caller can
    // put their own UID into.
    lf_xhorse_rdopt_t sopt;
    memset(&sopt, 0, sizeof(sopt));
    lf_xhorse_info_t snfo;
    bool set_is37 = false;
    if (xh_request_info(CMD_LF_XHORSE_INFO, (uint8_t *)&sopt, sizeof(sopt), &snfo) == PM3_SUCCESS) {
        set_is37 = (snfo.bchip && snfo.bchip_type_len >= 4
                    && snfo.bchip_type[2] == '3' && snfo.bchip_type[3] == '7');
    }
    const bool built = (t->recipe == XHORSE_R_HITAG2 || t->recipe == XHORSE_R_ID11
                        || t->recipe == XHORSE_R_ID12 || t->recipe == XHORSE_R_ID13
                        || t->recipe == XHORSE_R_7935);
    const xt_image_t *dimg = (built && set_is37 == false) ? NULL : xt57_decoded_image(t);
    if (xt57_type_supported(t) == false) {
        PrintAndLogEx(FAILED, "%s has no XT57B recipe yet", t->canon);
        PrintAndLogEx(INFO, "its session is longer than the 181 exchanges of keystream recovered");
        PrintAndLogEx(INFO, "so far, or it writes pages this channel has no recipe for");
        PrintAndLogEx(HINT, "the XT57B takes ID46/7936/7937/7946, ID11, ID12, ID13 and 7935");
        return PM3_EINVARG;
    }

    // Page 0 of block 00 is the emulated UID for the ID46 family - proved by writing one and
    // watching `lf search` follow it (§5r) - so that one stays choosable even from a decoded image.
    const bool dimg_uid = (dimg != NULL && t->recipe == XHORSE_R_HITAG2);
    if (dimg != NULL && dimg_uid == false && uidlen != 0) {
        PrintAndLogEx(WARNING, "%s is written from the captured set session, so --uid is ignored: "
                      "where that type keeps a UID is not established (docs/xt37.md SS5r)", t->canon);
        uidlen = 0;
    }
    const int want = (dimg != NULL && dimg_uid == false) ? 0
                     : (dimg_uid) ? 4
                     : (t->uid == UID_NONE) ? 0
                     : (t->uid == UID_REQ8) ? 8
                     : (t->uid == UID_REQ5) ? 5 : 4;
    if (uidlen != want) {
        if (want == 0) {
            PrintAndLogEx(FAILED, "%s is a wipe, not an image: it takes no --uid", t->canon);
        } else {
            PrintAndLogEx(FAILED, "%s takes a UID of %d bytes (got %d)", t->canon, want, uidlen);
        }
        return PM3_EINVARG;
    }

    // Types the chip goes deaf on.  `lf xhorse info`'s own hint lists them: an EM type (ID11/ID12/
    // ID13, type byte 0x8C), ID48, ID8E and ID8A "never answer" the identify, and a blanked PCF7935
    // (0x35) broadcasts and stops answering too.  Once set, the chip cannot be reached over channel B
    // again - and on an XT37 nothing in this client brought one back: `xt57 reset` was tried with
    // every word including `megamos`, with a widened burst sweep, and `lf xhorse reset` as well
    // (docs/xt37.md SS5s).  Only the Xhorse programmer re-typed it.
    const bool goes_deaf = (t->typebyte == 0x8C || t->typebyte == 0x35 || t->typebyte == 0x48
                            || t->typebyte == 0x8E || t->typebyte == 0x8A);
    if (goes_deaf && wipe_ok == false) {
        PrintAndLogEx(FAILED, "%s makes the chip stop answering channel B", t->canon);
        PrintAndLogEx(INFO, "from the next power-up it emulates and no longer answers the XMICRO");
        PrintAndLogEx(INFO, "identify, so this client cannot read, re-set or restore it afterwards");
        PrintAndLogEx(INFO, "`lf xhorse xt57 reset` is meant for this and did NOT recover an XT37 set");
        PrintAndLogEx(INFO, "to ID48 - not with any --word, not with a widened sweep (docs/xt37.md SS5s)");
        PrintAndLogEx(INFO, "recovering it needed the Xhorse programmer");
        PrintAndLogEx(HINT, "pass `" _YELLOW_("--wipe") "` to do it anyway");
        return PM3_EINVARG;
    }

    if (t->recipe == XHORSE_R_7935 && wipe_ok == false) {
        PrintAndLogEx(FAILED, "%s blanks the whole emulated store", t->canon);
        PrintAndLogEx(INFO, "addresses 00-09 are filled with 5A5A/00 patterns, and nothing puts the");
        PrintAndLogEx(INFO, "old contents back - not `lf xhorse xt57 restore`, not the VVDI's own");
        PrintAndLogEx(INFO, "reset, which rewrites the configuration and leaves the fill in place");
        PrintAndLogEx(INFO, "the channel itself is recoverable: the chip then broadcasts and goes");
        PrintAndLogEx(INFO, "deaf to XMICRO, and `lf xhorse xt57 reset` frees it again (SS12.11)");
        PrintAndLogEx(HINT, "pass `" _YELLOW_("--wipe") "` to do it anyway");
        return PM3_EINVARG;
    }

    // A HITAG2 reader names the exact PCF part from bits 4..7 of the UID, not from the type byte
    // (getHitagTypeStr() in cmdlfhitag.c), so warn on a mismatch - but it is the caller's UID.
    if (t->recipe == XHORSE_R_HITAG2) {
        uint8_t wantnib = (t->typebyte == 0x36) ? 1 : (t->typebyte == 0x46) ? 2 : 0;
        uint8_t have = (uid[3] >> 4) & 0x0F;
        if (wantnib && (have != wantnib)) {
            PrintAndLogEx(WARNING, "UID nibble is %u, but %s is read from a UID whose nibble is %u;",
                          have, t->canon, wantnib);
            PrintAndLogEx(WARNING, "      a reader will name the chip from the UID, not from the type byte.");
        }
    }

    xt57_step_t steps[XT57_MAXSTEPS];
    int arm_at = 0;
    xt_imgpage_t dpages[XT57_MAXSTEPS];
    xt_image_t dcopy;
    if (dimg_uid) {                     // substitute the chosen UID into the decoded image
        dcopy = *dimg;
        memcpy(dpages, dimg->pages, sizeof(xt_imgpage_t) * dimg->n);
        for (int ip = 0; ip < dimg->n; ip++) {
            if (dpages[ip].arg == 0x00 && dpages[ip].page == 0) {
                dpages[ip].val = xt57_be32(uid);
            }
        }
        dcopy.pages = dpages;
        dimg = &dcopy;
    }
    int nst = (dimg != NULL) ? xt57_build_steps_image(dimg, steps, XT57_MAXSTEPS, &arm_at)
                             : xt57_build_steps(t, uid, steps, XT57_MAXSTEPS, &arm_at);
    if (dimg != NULL) {
        PrintAndLogEx(INFO, "decoded %s image: %d pages, config word %08X",
                      dimg->name, dimg->n, dimg->cfg4);
    }
    if (nst < 0) {
        PrintAndLogEx(FAILED, "could not build a session for %s", t->canon);
        return PM3_EINVARG;
    }

    PrintAndLogEx(INFO, "setting " _GREEN_("%s") " (type byte 0x%02X), %d exchanges",
                  t->canon, t->typebyte, nst);
    if (arm_at) {
        PrintAndLogEx(INFO, "%s broadcasts: from the next power-up the chip talks first and "
                      "stops answering XMICRO", t->canon);
    }

    int res = xt57_run_session(steps, nst, arm_at, centre, span);
    if (res == PM3_SUCCESS) {
        if (arm_at) {
            PrintAndLogEx(HINT, "`" _YELLOW_("lf search") "` should now see the emulated tag; "
                          "`" _YELLOW_("lf xhorse xt57 reset") "` frees the chip again");
            PrintAndLogEx(INFO, "the chip no longer answers `" _YELLOW_("lf xhorse xt57") "` until it is reset");
        } else {
            PrintAndLogEx(HINT, "`" _YELLOW_("lf search") "` should now see the emulated tag");
        }
    }
    return res;
}

// The factory personality: 7936/ID46 with a chosen or random UID.
static int CmdLFXhorseXt57Restore(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse xt57 restore",
                  "Rewrite an XT57B's factory ID46/HITAG2 personality - the half of the VVDI's\n"
                  "reset that `lf xhorse xt57 reset` does not do.\n"
                  "\n"
                  _RED_("This writes to the chip") ".  It is `lf xhorse xt57 set -t 7936` with the\n"
                  "factory image - the VVDI's own reset session, rebuilt.\n"
                  "\n"
                  "A chip still broadcasting an emulated type answers nothing: run\n"
                  "`lf xhorse xt57 reset` first to free it (`--word 7935` for one blanked\n"
                  "to 7935), then this.",
                  "lf xhorse xt57 restore                 -> random UID, like the VVDI\n"
                  "lf xhorse xt57 restore -u 37786F1F     -> a chosen UID");
    void *argtable[] = {
        arg_param_begin,
        arg_str0("u", "uid", "<hex>", "4 byte emulated UID (default: random, as the VVDI does)"),
        arg_int0(NULL, "base", "<dec>", "centre of the power-up search in us (def 119500)"),
        arg_int0(NULL, "span", "<dec>", "half-width of the search in us (def 400)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint8_t uid[XHORSE_UID_MAXLEN] = {0};
    int uidlen = 0;
    CLIGetHexWithReturn(ctx, 1, uid, &uidlen);
    int centre = arg_get_int_def(ctx, 2, XT57_SEARCH_CENTRE);
    int span = arg_get_int_def(ctx, 3, XT57_SEARCH_SPAN);
    CLIParserFree(ctx);

    if (uidlen != 0 && uidlen != 4) {
        PrintAndLogEx(FAILED, "--uid must be 4 bytes (got %d)", uidlen);
        return PM3_EINVARG;
    }
    if (uidlen == 0) {
        srand((unsigned)time(NULL));
        uid[0] = (uint8_t)(rand() & 0xFF);
        uid[1] = (uint8_t)(rand() & 0xFF);
        uid[2] = (uint8_t)(rand() & 0xFF);
        uid[3] = (uint8_t)(rand() & 0xFF);
    }

    const xh_type_t *t = resolve_type("7936");
    if (t == NULL) {
        return PM3_ESOFT;
    }
    xt57_step_t steps[XT57_MAXSTEPS];
    int arm_at = 0;
    // On an XT37 use the order the tool itself uses for a reset - config word first, then the image
    // (docs/xt37.md SS5u).  The XT57 path keeps the recipe built from its own reset capture.
    lf_xhorse_rdopt_t ropt;
    memset(&ropt, 0, sizeof(ropt));
    lf_xhorse_info_t rnfo;
    bool rst_is37 = false;
    if (xh_request_info(CMD_LF_XHORSE_INFO, (uint8_t *)&ropt, sizeof(ropt), &rnfo) == PM3_SUCCESS) {
        rst_is37 = (rnfo.bchip && rnfo.bchip_type_len >= 4
                    && rnfo.bchip_type[2] == '3' && rnfo.bchip_type[3] == '7');
    }
    int nst = rst_is37 ? xt57_build_steps_reset(uid, steps, XT57_MAXSTEPS, &arm_at)
                       : xt57_build_steps(t, uid, steps, XT57_MAXSTEPS, &arm_at);
    if (rst_is37) {
        PrintAndLogEx(INFO, "XT37: config word first, then the image - the tool's own reset order");
    }
    if (nst < 0) {
        return PM3_ESOFT;
    }

    PrintAndLogEx(INFO, "restoring " _GREEN_("ID46/7936") " with UID " _GREEN_("%02X%02X%02X%02X"),
                  uid[0], uid[1], uid[2], uid[3]);
    int res = xt57_run_session(steps, nst, arm_at, centre, span);
    if (res == PM3_SUCCESS) {
        PrintAndLogEx(HINT, "`" _YELLOW_("lf xhorse xt57 reset") "` frees a chip; this rewrites its configuration");
    }
    return res;
}

static int CmdHelpXt57(const char *Cmd);
// Which blocks an XT37 is known to use (docs/xt37.md SS5l).  `arg` is the select operand:
// segment in the top 3 bits, block in the low 5 - so s3 b1F is 0x7F, the same operand the XT57B's
// config block has.
static const struct { uint8_t arg; const char *what; } xt_blocks[] = {
    { 0x00, "emulation image" },
    { 0x01, "per-type config" },
    { 0x0C, "image area" },
    { 0x0D, "image area" },
    { 0x10, "image area" },
    { 0x12, "image area" },
    { 0x7E, "identity block" },
    { 0x7F, "config block (page 4 = type byte)" },
};

// Read the eight pages of one block in a single authenticated session: select, then eight
// triggers.  Reads are decrypted with the data keystream (docs/xt37.md SS5k).
static int xt_read_block(const xt57_ks_t *ks, int32_t at, uint8_t arg,
                         uint32_t *out, bool *got) {
    xt57_step_t st[9];
    st[0] = (xt57_step_t) { 'S', arg, 0 };
    for (int i = 0; i < 8; i++) {
        st[i + 1] = (xt57_step_t) { 'R', (uint8_t)i, 0 };
    }
    xt57_frame_t frames[9];
    if (xt57_build_session(st, 9, ks, frames) == false) {
        PrintAndLogEx(FAILED, "the keystream at nonce %08X does not reach 9 exchanges", ks->nonce);
        return PM3_EINVARG;
    }

    lf_xhorse_xt57_t p;
    memset(&p, 0, sizeof(p));
    p.sweep = XT57_SWEEP_POWERUP;
    p.answer_mode = XT57_ANS_FIXED;
    p.answer = ks->answer;
    p.want = ks->nonce;
    p.base = at;
    p.off_us = (uint32_t)ks->off_us;
    p.selbits = XHORSE_XT57_SELLEN * 8;
    memcpy(p.select, xt57_sel_default, XHORSE_XT57_SELLEN);
    p.sel_auto = ks->sel_auto;
    p.npost = 9;
    p.rfirst = 1;
    int packed = xt57_pack_script(frames, 9, p.script, sizeof(p.script));
    if (packed < 0) {
        return PM3_EINVARG;
    }
    p.scriptlen = (uint16_t)packed;
    // Sessions per attempt, sweeping 2 us (one clock) apart from `base` below.  The phase drifts
    // between runs by more than the plateau is wide, so this has to cover tens of microseconds, not
    // a handful: 16 sessions missed often enough to fail a write that then succeeded by hand.
    p.count = 30;

    // The delay that reaches a nonce sits on a narrow, drifting plateau (docs/xt37.md SS4a), so
    // hammering one delay wastes most sessions.  Sweep the plateau *inside* the call instead -
    // base-6 .. base+8 at the stream's own 2 us per clock - and the wanted nonce turns up within a
    // few sessions.  Two passes cover the case where the plateau has drifted further.
    p.step = 2;
    PacketResponseNG resp;
    bool ran = false;
    for (int t = 0; t < 3 && ran == false; t++) {
        p.base = at - 20;
        clearCommandBuffer();
        SendCommandNG(CMD_LF_XHORSE_XT57_NONCE, (uint8_t *)&p, sizeof(p));
        if (WaitForResponseTimeout(CMD_LF_XHORSE_XT57_NONCE, &resp, 120000) == false) {
            PrintAndLogEx(WARNING, "timeout while waiting for reply");
            SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
            return PM3_ETIMEOUT;
        }
        ran = (resp.length >= sizeof(lf_xhorse_xt57_post_reply_t)
               - sizeof(xt57_presp_t) * XHORSE_XT57_MAXPOST);
        if (ran == false && t + 1 < 3) {
            // Closed loop: ask the chip where the stream is now and recompute, rather than widening
            // a blind search.  The phase drifts between runs, so a fresh reading is worth more than
            // more sessions at a stale delay.
            int32_t fresh = 0;
            if (xt_steer(ks, at, &fresh, false) == PM3_SUCCESS) {
                at = fresh;
            }
        }
    }
    if (ran == false) {
        PrintAndLogEx(FAILED, "nonce %08X did not come up near %d us", ks->nonce, at);
        return PM3_ESOFT;
    }
    const lf_xhorse_xt57_post_reply_t *q = (const lf_xhorse_xt57_post_reply_t *)resp.data.asBytes;
    for (int i = 0; i < 8; i++) {
        got[i] = false;
        out[i] = 0;
        int idx = i + 1;                       // exchange 1 is the select; reads are 2..9
        if (idx >= q->npost) {
            continue;
        }
        const xt57_presp_t *r = &q->presp[idx];
        if (r->bits < 40) {
            continue;
        }
        uint32_t ct = ((uint32_t)r->data[0] << 24) | ((uint32_t)r->data[1] << 16)
                      | ((uint32_t)r->data[2] << 8) | r->data[3];
        out[i] = ct ^ ks->K[idx + 1];          // the read sits at exchange idx+1
        got[i] = true;
    }
    return PM3_SUCCESS;
}

// Emulated-type byte -> what the tool calls it (mem-layout.md "type byte" table, every entry
// confirmed against a decoded set session in docs/xt37.md SS5p).
static const char *xt_type_name(uint8_t t) {
    switch (t) {
        case 0x35: return "ID7935 (ID33/40/42/43/44)";
        case 0x36: return "ID7936 / ID46";
        case 0x37: return "ID7937";
        case 0x39: return "ID7939 / ID49";
        case 0x46: return "ID7946";
        case 0x47: return "ID7938 / ID47";
        case 0x48: return "ID48";
        case 0x4A: return "ID4A";
        case 0x4C: return "ID4C";
        case 0x4D: return "ID4D (ID60/63)";
        case 0x64: return "ID4E (ID64)";
        case 0x80: return "4D(80) (ID70/83/G)";
        case 0x8A: return "ID8A (H)";
        case 0x8B: return "MQB";
        case 0x8C: return "ID11 / ID12 / ID13 / ID8C";
        case 0x8E: return "ID8E";
        default:   return "unknown";
    }
}

// The nonce register: a 32-bit LFSR clocked once per carrier cycle, taps 2/11/15/21/28/30/32
// (docs/xt37.md SS4b, data/traces/xt57b/nonce-captures/steer_to_known_nonce.py).
static const int xt_lfsr_taps[] = { 2, 11, 15, 21, 28, 30, 32 };

static uint32_t xt_lfsr_fwd(uint32_t v) {
    uint32_t nb = 0;
    for (size_t i = 0; i < ARRAYLEN(xt_lfsr_taps); i++) {
        nb ^= (v >> (xt_lfsr_taps[i] - 1)) & 1;
    }
    return (v << 1) | nb;
}

// Read the nonce the chip produces at one power-up delay.  No answer is sent: the nonce comes back
// in the clear, so this costs a handful of sessions and tells us where on the stream we are.
static int xt_observe_nonce(const xt57_ks_t *ks, int32_t at, uint32_t *nonce) {
    lf_xhorse_xt57_t p;
    memset(&p, 0, sizeof(p));
    p.sweep = XT57_SWEEP_POWERUP;
    p.answer_mode = XT57_ANS_NONE;
    p.base = at;
    p.step = 0;
    p.count = 6;
    p.off_us = (uint32_t)ks->off_us;
    p.selbits = XHORSE_XT57_SELLEN * 8;
    memcpy(p.select, xt57_sel_default, XHORSE_XT57_SELLEN);
    p.sel_auto = ks->sel_auto;

    clearCommandBuffer();
    SendCommandNG(CMD_LF_XHORSE_XT57_NONCE, (uint8_t *)&p, sizeof(p));
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_LF_XHORSE_XT57_NONCE, &resp, 60000) == false) {
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        return PM3_ETIMEOUT;
    }
    const lf_xhorse_xt57_reply_t *r = (const lf_xhorse_xt57_reply_t *)resp.data.asBytes;
    // Take the value seen most often: the window drifts a clock either way between sessions.
    uint32_t best = 0;
    int bestn = 0;
    for (int i = 0; i < r->count; i++) {
        if (r->rec[i].nonce_bits != 32) {
            continue;
        }
        uint32_t n = ((uint32_t)r->rec[i].nonce[0] << 24) | ((uint32_t)r->rec[i].nonce[1] << 16)
                     | ((uint32_t)r->rec[i].nonce[2] << 8) | r->rec[i].nonce[3];
        int c = 0;
        for (int j = 0; j < r->count; j++) {
            uint32_t m = ((uint32_t)r->rec[j].nonce[0] << 24) | ((uint32_t)r->rec[j].nonce[1] << 16)
                         | ((uint32_t)r->rec[j].nonce[2] << 8) | r->rec[j].nonce[3];
            if (r->rec[j].nonce_bits == 32 && m == n) {
                c++;
            }
        }
        if (c > bestn) {
            bestn = c;
            best = n;
        }
    }
    if (bestn == 0) {
        return PM3_ESOFT;
    }
    *nonce = best;
    return PM3_SUCCESS;
}

// Steer to a wanted nonce the way the hitager tool does: read where the stream is, step the LFSR to
// work out how far the target is, and convert that to a delay.  A *later* delay walks the nonce
// *backwards*, so a target `d` clocks forward of what we just read sits at `at - d * 8` us
// (docs/xt57-auth.md SS10.3 - the sign is the opposite of what SS5a's wording suggests).  Two
// sessions instead of the hundreds a blind sweep costs.
#define XT_STEER_LIM   200000       // clocks to search either way: 1.6 s of delay, far beyond the
//                                     XMICRO listening window, so a miss here means "not on this
//                                     stretch of stream" rather than "search harder"
static int xt_steer(const xt57_ks_t *ks, int32_t from, int32_t *at, bool verbose) {
    uint32_t here = 0;
    if (xt_observe_nonce(ks, from, &here) != PM3_SUCCESS) {
        return PM3_ESOFT;
    }
    // The target may lie either side of where the chip is: walking forward from `here` finds a
    // target ahead of us, and walking forward from the target finds one behind.  Only searching
    // forward would make a target that is a few clocks behind look like most of a 715M-state orbit
    // away - which is exactly how an XT57 behaves against these tables.
    int32_t ahead = -1, behind = -1;
    uint32_t wf = here, wb = ks->nonce;
    for (int32_t d = 0; d < XT_STEER_LIM && ahead < 0 && behind < 0; d++) {
        if (wf == ks->nonce) {
            ahead = d;
            break;
        }
        if (wb == here) {
            behind = d;
            break;
        }
        wf = xt_lfsr_fwd(wf);
        wb = xt_lfsr_fwd(wb);
    }
    {
        int32_t d = (ahead >= 0) ? ahead : behind;
        if (d >= 0) {
            // A later delay walks the nonce backwards, so a target ahead of us sits at an earlier
            // delay and one behind us at a later delay (docs/xt57-auth.md SS10.3).
            int32_t want = (ahead >= 0) ? (from - d * 8) : (from + d * 8);
            if (verbose) {
                PrintAndLogEx(INFO, "at %d us the chip is at %08X, %d clocks %s %08X -> %d us",
                              from, here, d, (ahead >= 0) ? "before" : "after", ks->nonce, want);
            }
            if (want < 20000 || want > 400000) {
                PrintAndLogEx(WARNING, "%08X would need a %d us power-up, outside the listening window",
                              ks->nonce, want);
                return PM3_ESOFT;
            }
            *at = want;
            return PM3_SUCCESS;
        }
    }
    return PM3_ESOFT;
}

// Pick the keystream table for the family on the coil and find a power-up delay that reaches one of
// its nonces.  A nonce sits on a narrow plateau that drifts, so one sweep can miss a nonce that is
// perfectly reachable (docs/xt37.md SS4a) - hence the passes.
static const xt57_ks_t *xt_pick_ks(bool is37, int at, int32_t *found, bool verbose) {
    const xt57_ks_t *const *tbl = is37 ? xt37_ks_all : xt57_ks_all;
    size_t ntbl = is37 ? ARRAYLEN(xt37_ks_all) : ARRAYLEN(xt57_ks_all);
    // Widen progressively instead of sweeping the whole span every pass: the recorded centre is
    // usually within a few tens of us on the tag it was measured from, and sweeping +-900 us at a
    // 2 us step costs minutes - too slow to sit inside `lf xhorse info`.  A narrow pass that usually
    // hits is tried first, and only a failure pays for the wide one.
    // First ask the chip where the stream is and compute the delay (xt_steer).  Only if that fails
    // - a mis-read nonce, or a target too far along the stream - fall back to sweeping.
    for (size_t i = 0; i < ntbl && at <= 0; i++) {
        int32_t want = 0;
        if (xt_steer(tbl[i], tbl[i]->centre, &want, verbose) == PM3_SUCCESS) {
            // Trust the arithmetic: the caller's session sweeps a few clocks around this itself, so
            // confirming with a separate search would just pay for the same sessions twice.
            *found = want;
            return tbl[i];
        }
    }

    static const int32_t spans[] = { 60, 300, 900 };
    for (size_t sp = 0; sp < ARRAYLEN(spans); sp++) {
        for (size_t i = 0; i < ntbl; i++) {
            int32_t span = (spans[sp] < tbl[i]->span) ? spans[sp] : tbl[i]->span;
            int32_t lo = (at > 0) ? at : tbl[i]->centre - span;
            int32_t hi = (at > 0) ? at : tbl[i]->centre + span;
            if (verbose) {
                PrintAndLogEx(INFO, "searching for nonce %08X between %d and %d us...",
                              tbl[i]->nonce, lo, hi);
            }
            int32_t a = xt57_find_nonce(tbl[i], lo, hi);
            if (a >= 0) {
                *found = a;
                return tbl[i];
            }
            if (at > 0) {
                return NULL;               // an explicit --at names one delay; do not widen it
            }
        }
    }
    return NULL;
}

// Read the emulated type byte: config block, page 4 (docs/xt37.md SS5o).  Returns the whole word,
// whose low byte is the type; the upper half varies by type and is not understood (SS5p).
static int xt_read_typeword(bool is37, uint32_t *word) {
    int32_t at = 0;
    const xt57_ks_t *ks = xt_pick_ks(is37, 0, &at, false);
    if (ks == NULL) {
        return PM3_ESOFT;
    }
    uint32_t v[8];
    bool got[8];
    if (xt_read_block(ks, at, XT_ARG_CONFIG, v, got) != PM3_SUCCESS) {
        return PM3_ESOFT;
    }
    if (got[4] == false) {
        return PM3_ESOFT;
    }
    *word = v[4];
    return PM3_SUCCESS;
}

// Write one page: select, write, read it back.  The read-back is the whole point - a write is only
// reported good if the chip returns what went in (docs/xt37.md SS5n, where 354 of 358 recorded
// writes verified this way).
static int xt_write_page(const xt57_ks_t *ks, int32_t at, uint8_t arg, uint8_t page,
                         uint32_t value, uint32_t *readback) {
    xt57_step_t st[3] = {
        { 'S', arg, 0 },
        { 'W', page, value },
        { 'R', page, 0 },
    };
    xt57_frame_t frames[3];
    if (xt57_build_session(st, 3, ks, frames) == false) {
        return PM3_EINVARG;
    }

    lf_xhorse_xt57_t p;
    memset(&p, 0, sizeof(p));
    p.sweep = XT57_SWEEP_POWERUP;
    p.answer_mode = XT57_ANS_FIXED;
    p.answer = ks->answer;
    p.want = ks->nonce;
    p.off_us = (uint32_t)ks->off_us;
    p.selbits = XHORSE_XT57_SELLEN * 8;
    memcpy(p.select, xt57_sel_default, XHORSE_XT57_SELLEN);
    p.sel_auto = ks->sel_auto;
    p.npost = 3;
    p.rfirst = 1;
    int packed = xt57_pack_script(frames, 3, p.script, sizeof(p.script));
    if (packed < 0) {
        return PM3_EINVARG;
    }
    p.scriptlen = (uint16_t)packed;
    p.count = 30;
    p.step = 2;

    PacketResponseNG resp;
    bool ran = false;
    for (int t = 0; t < 3 && ran == false; t++) {
        p.base = at - 20;
        clearCommandBuffer();
        SendCommandNG(CMD_LF_XHORSE_XT57_NONCE, (uint8_t *)&p, sizeof(p));
        if (WaitForResponseTimeout(CMD_LF_XHORSE_XT57_NONCE, &resp, 120000) == false) {
            SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
            return PM3_ETIMEOUT;
        }
        ran = (resp.length >= sizeof(lf_xhorse_xt57_post_reply_t)
               - sizeof(xt57_presp_t) * XHORSE_XT57_MAXPOST);
        if (ran == false && t + 1 < 3) {
            int32_t fresh = 0;
            if (xt_steer(ks, at, &fresh, false) == PM3_SUCCESS) {
                at = fresh;
            }
        }
    }
    if (ran == false) {
        PrintAndLogEx(FAILED, "nonce %08X did not come up - nothing was written", ks->nonce);
        return PM3_ESOFT;
    }
    const lf_xhorse_xt57_post_reply_t *q = (const lf_xhorse_xt57_post_reply_t *)resp.data.asBytes;
    if (q->npost < 3 || q->presp[2].bits < 40) {
        PrintAndLogEx(WARNING, "no read-back came back - the write may or may not have landed");
        return PM3_ESOFT;
    }
    const xt57_presp_t *r = &q->presp[2];
    uint32_t ct = ((uint32_t)r->data[0] << 24) | ((uint32_t)r->data[1] << 16)
                  | ((uint32_t)r->data[2] << 8) | r->data[3];
    *readback = ct ^ ks->K[3];
    return PM3_SUCCESS;
}

// Types that share one image, so switching between them is only a type-byte write (set-type.md:
// "same default ID46 image").  Anything else needs its image written too, which this does not do.
static bool xt_type_shares_id46_image(uint8_t t) {
    return (t == 0x36 || t == 0x37 || t == 0x46);
}


// The ID46-family image: a UID then three constants (docs/xt37.md SS5p - every 7936/7937/7946 set and
// the reset write exactly these four pages of s0 b00, pages 4-7 staying zero).
#define XT_IMG_MIKR     0x4D494B52      // "MIKR" - HITAG2 key low
#define XT_IMG_ON       0x00004F4E      // "\0\0ON" - HITAG2 key high
#define XT_IMG_CFG      0x06AA4854      // HITAG2 config page

static int CmdLFXhorseXt57SetType(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse xt57 settype",
                  "Write the emulated type byte of an XT37 / XT57: config block, page 4\n"
                  "(docs/xt37.md SS5o).  This writes to the chip.\n"
                  "\n"
                  "It writes the type byte ONLY.  ID7936 (0x36), ID7937 (0x37) and ID7946 (0x46)\n"
                  "share one emulated image, so switching among those three is complete on its own.\n"
                  "Any other type also needs its image written, which this command does not do - the\n"
                  "chip would emulate a type whose image is not there.  --force allows it anyway.",
                  "lf xhorse xt57 settype -t 37      -> ID7937\n"
                  "lf xhorse xt57 settype -t 36      -> ID7936 / ID46, the factory personality");
    void *argtable[] = {
        arg_param_begin,
        arg_str1("t", "type", "<hex>", "type byte to write"),
        arg_lit0(NULL, "force", "write a type whose image this command cannot write"),
        arg_int0(NULL, "at", "<dec>", "power-up delay to use, us (default: steer to one)"),
        arg_param_end,
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int tlen = 0;
    uint8_t tb[2] = {0};
    CLIGetHexWithReturn(ctx, 1, tb, &tlen);
    bool force = arg_get_lit(ctx, 2);
    int at = arg_get_int_def(ctx, 3, 0);
    CLIParserFree(ctx);
    if (tlen != 1) {
        PrintAndLogEx(FAILED, "--type takes one byte");
        return PM3_EINVARG;
    }
    uint8_t type = tb[0];

    if (xt_type_shares_id46_image(type) == false && force == false) {
        PrintAndLogEx(FAILED, "type %02X (%s) needs its emulated image written too, which this "
                      "command does not do", type, xt_type_name(type));
        PrintAndLogEx(INFO, "only %02X, %02X and %02X share one image; --force overrides",
                      0x36, 0x37, 0x46);
        return PM3_EINVARG;
    }

    lf_xhorse_rdopt_t opt;
    memset(&opt, 0, sizeof(opt));
    lf_xhorse_info_t nfo;
    if (xh_request_info(CMD_LF_XHORSE_INFO, (uint8_t *)&opt, sizeof(opt), &nfo) != PM3_SUCCESS) {
        return PM3_ESOFT;
    }
    if (nfo.bchip == 0) {
        PrintAndLogEx(FAILED, "no channel-B super-chip answered");
        return PM3_ESOFT;
    }
    bool is37 = (nfo.bchip_type_len >= 4 && nfo.bchip_type[2] == '3' && nfo.bchip_type[3] == '7');

    int32_t found = 0;
    const xt57_ks_t *ks = xt_pick_ks(is37, at, &found, true);
    if (ks == NULL) {
        PrintAndLogEx(FAILED, "no recorded nonce came up - nothing was written");
        return PM3_ESOFT;
    }

    // Read the word first: the upper half varies by type and is not understood (SS5p), so it is
    // preserved rather than invented, and the old value is printed in case it has to go back.
    uint32_t v[8];
    bool got[8];
    if (xt_read_block(ks, found, XT_ARG_CONFIG, v, got) != PM3_SUCCESS || got[4] == false) {
        PrintAndLogEx(FAILED, "could not read the current type word - nothing was written");
        return PM3_ESOFT;
    }
    uint32_t oldw = v[4];
    // Prefer the whole word the tool wrote for this type: its upper half is not a constant (01F0,
    // 3FF0, 7FF0 - SS5p), so carrying the chip's current upper half over would write a word no set
    // session ever produced.  Only a type with no recorded word falls back to patching the low byte.
    uint32_t neww = (oldw & 0xFFFFFF00) | type;
    bool recorded = false;
    for (size_t ic = 0; ic < ARRAYLEN(xt_images); ic++) {
        if (xt_images[ic].type == type && xt_images[ic].cfg4 != 0) {
            neww = xt_images[ic].cfg4;
            recorded = true;
            break;
        }
    }
    if (recorded == false) {
        PrintAndLogEx(WARNING, "no recorded config word for type %02X - keeping the current upper "
                      "half, which may not be what a real set writes", type);
    }
    PrintAndLogEx(INFO, "config word.... %08X (%02X, %s)", oldw, (uint8_t)(oldw & 0xFF),
                  xt_type_name((uint8_t)(oldw & 0xFF)));
    if (neww == oldw) {
        PrintAndLogEx(SUCCESS, "already %02X - nothing to do", type);
        return PM3_SUCCESS;
    }
    PrintAndLogEx(INFO, "writing........ %08X (%02X, %s)%s", neww, type, xt_type_name(type),
                  ((neww & 0xFFFFFF00) != (oldw & 0xFFFFFF00)) ? "  [upper half changes too]" : "");

    uint32_t back = 0;
    int res = xt_write_page(ks, found, XT_ARG_CONFIG, 4, neww, &back);
    if (res != PM3_SUCCESS) {
        return res;
    }
    if (back == neww) {
        PrintAndLogEx(SUCCESS, "read back...... " _GREEN_("%08X") "  type is now %02X (%s)",
                      back, type, xt_type_name(type));
        PrintAndLogEx(HINT, "to put it back: `lf xhorse xt57 settype -t %02X`", (uint8_t)(oldw & 0xFF));
        return PM3_SUCCESS;
    }
    PrintAndLogEx(FAILED, "read back...... " _RED_("%08X") ", expected %08X", back, neww);
    return PM3_ESOFT;
}

static int CmdLFXhorseXt57Read(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse xt57 read",
                  "Read channel-B pages of an XT37 or XT57 super-chip.\n"
                  "Each block is eight 4-byte pages; `--arg` is the select operand, segment in the\n"
                  "top 3 bits and block in the low 5 (docs/xt37.md SS5k).  With no argument it reads\n"
                  "every block the family is known to use.",
                  "lf xhorse xt57 read                 -> every known block\n"
                  "lf xhorse xt57 read --arg 0x7F      -> the config block, page 4 is the type byte\n"
                  "lf xhorse xt57 read --arg 0x00      -> the emulation image");
    void *argtable[] = {
        arg_param_begin,
        arg_str0(NULL, "arg", "<hex>", "select operand: segment << 5 | block"),
        arg_int0(NULL, "at", "<dec>", "power-up delay to use, us (default: search for one)"),
        arg_param_end,
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    int arglen = 0;
    uint8_t argb[2] = {0};
    CLIGetHexWithReturn(ctx, 1, argb, &arglen);
    int at = arg_get_int_def(ctx, 2, 0);
    CLIParserFree(ctx);

    // Which family is on the coil decides which keystream applies: the answer that opens a session
    // is family-specific (docs/xt37.md SS5a).
    lf_xhorse_rdopt_t opt;
    memset(&opt, 0, sizeof(opt));
    lf_xhorse_info_t nfo;
    if (xh_request_info(CMD_LF_XHORSE_INFO, (uint8_t *)&opt, sizeof(opt), &nfo) != PM3_SUCCESS) {
        return PM3_ESOFT;
    }
    if (nfo.bchip == 0) {
        PrintAndLogEx(FAILED, "no channel-B super-chip answered - this command is for XT37/XT57");
        return PM3_ESOFT;
    }
    bool is37 = (nfo.bchip_type_len >= 4 && nfo.bchip_type[2] == '3' && nfo.bchip_type[3] == '7');
    PrintAndLogEx(INFO, "chip family... %.4s", (const char *)nfo.bchip_type);

    int32_t found = 0;
    const xt57_ks_t *ks = xt_pick_ks(is37, at, &found, true);
    if (ks == NULL) {
        PrintAndLogEx(FAILED, "none of this family's recorded nonces came up");
        PrintAndLogEx(HINT, "the delay that reaches a nonce is per-tag; widen it with --at");
        return PM3_ESOFT;
    }
    PrintAndLogEx(SUCCESS, "nonce %08X at %d us", ks->nonce, found);

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, " seg blk  p0       p1       p2       p3       p4       p5       p6       p7");
    PrintAndLogEx(INFO, " --- ---  -------- -------- -------- -------- -------- -------- -------- --------");
    for (size_t b = 0; b < ARRAYLEN(xt_blocks); b++) {
        uint8_t a = (arglen > 0) ? argb[0] : xt_blocks[b].arg;
        uint32_t v[8];
        bool got[8];
        if (xt_read_block(ks, found, a, v, got) != PM3_SUCCESS) {
            break;
        }
        char line[160] = {0};
        for (int i = 0; i < 8; i++) {
            char cell[12];
            snprintf(cell, sizeof(cell), got[i] ? "%08X " : "   --    ", v[i]);
            strncat(line, cell, sizeof(line) - strlen(line) - 1);
        }
        PrintAndLogEx(INFO, "  %d  %02X  %s", (a >> 5) & 7, a & 0x1F, line);
        if (arglen > 0) {
            break;
        }
    }
    return PM3_SUCCESS;
}

static command_t Xt57CommandTable[] = {
    {"help",  CmdHelpXt57,           AlwaysAvailable, "This help"},
    {"nonce", CmdLFXhorseXt57Nonce,  IfPm3Hitag,      "Collect XT57B nonces over a range of timings"},
    {"read",  CmdLFXhorseXt57Read,   IfPm3Hitag,      "Read channel-B pages of an XT37 / XT57"},
    {"settype", CmdLFXhorseXt57SetType, IfPm3Hitag,    "Advanced: write only the config word (use `set` normally)"},
    {"reset", CmdLFXhorseXt57Reset,  IfPm3Hitag,      "Free a chip stuck emulating a broadcast type"},
    {"set",   CmdLFXhorseXt57Set,    IfPm3Hitag,      "Set which transponder the chip emulates"},
    {"restore", CmdLFXhorseXt57Restore, IfPm3Hitag,   "Rewrite the factory ID46 personality"},
    {NULL, NULL, 0, NULL}
};

static int CmdHelpXt57(const char *Cmd) {
    (void)Cmd;
    CmdsHelp(Xt57CommandTable);
    return PM3_SUCCESS;
}

static int CmdLFXhorseXt57(const char *Cmd) {
    clearCommandBuffer();
    return CmdsParse(Xt57CommandTable, Cmd);
}

//-----------------------------------------------------------------------------
static command_t CommandTable[] = {
    {"help",      CmdHelp,             AlwaysAvailable, "This help"},
    {"-----------", CmdHelp,           AlwaysAvailable, "----------------- " _CYAN_("Xhorse XT27A / XT136") " -----------------"},
    {"types",     CmdLFXhorseTypes,    AlwaysAvailable, "List the -t type names accepted by configure"},
    {"info",      CmdLFXhorseInfo,     IfPm3Hitag,      "Identify a chip: family, ID, type, emulated image"},
    {"dump",      CmdLFXhorseDump,     IfPm3Hitag,      "Read all reachable channel-A storage"},
    {"configure", CmdLFXhorseConfigure, IfPm3Hitag,     "Set which transponder the chip emulates"},
    {"reset",     CmdLFXhorseReset,    IfPm3Hitag,      "Restore the default ID46 personality"},
    {"read",      CmdLFXhorseRead,     IfPm3Hitag,      "Read raw channel-A pages"},
    {"write",     CmdLFXhorseWrite,    IfPm3Hitag,      "Write one raw channel-A page"},
    {"xt57",      CmdLFXhorseXt57,     AlwaysAvailable, "XT57B (XMICRO channel) experiments"},
    {NULL, NULL, 0, NULL}
};

static int CmdHelp(const char *Cmd) {
    (void)Cmd;
    CmdsHelp(CommandTable);
    return PM3_SUCCESS;
}

int CmdLFXhorse(const char *Cmd) {
    clearCommandBuffer();
    return CmdsParse(CommandTable, Cmd);
}
