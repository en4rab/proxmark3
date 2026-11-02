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
}

// Bank-0 pages whose reads never agreed are printed with a red '?' (see page_ok).
static bool xh_page_ok(const lf_xhorse_info_t *nfo, uint8_t pg) {
    return (nfo->page_ok[pg / 8] >> (pg % 8)) & 1;
}

// Always two visible characters, so a column after it stays aligned.
static const char *xh_page_mark(const lf_xhorse_info_t *nfo, uint8_t pg) {
    return xh_page_ok(nfo, pg) ? "  " : " " _RED_("?");
}

static void xh_print_unconfirmed(const lf_xhorse_info_t *nfo) {
    int bad = 0;
    for (uint8_t pg = 0; pg < nfo->image_pages; pg++) {
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
    PrintAndLogEx(INFO, "  page | data");
    PrintAndLogEx(INFO, "-------+-----------");
    for (uint8_t pg = 0; pg < nfo->image_pages; pg++) {
        PrintAndLogEx(INFO, "  0x%02X | %s%s %s", pg, sprint_hex_inrow(&nfo->image[pg * 4], 4), xh_page_mark(nfo, pg), xh136_page_label(pg));
    }
    xh_print_unconfirmed(nfo);
    if (nfo->image_pages > 0x0F) {
        PrintAndLogEx(INFO, "Config word 1 high byte " _YELLOW_("0x%02X") " (%s, if it is the type byte)", nfo->image[0x0E * 4], typebyte_name(nfo->image[0x0E * 4]));
    }
    if (nfo->image_pages > 0x10) {
        PrintAndLogEx(HINT, "Pages above 0x0F have never been seen used; their values may be aliases or noise.");
    }
    PrintAndLogEx(NORMAL, "");
}

static void xh_print_header(const lf_xhorse_info_t *nfo) {
    PrintAndLogEx(INFO, "Chip ID... " _GREEN_("%s"), sprint_hex_inrow(nfo->chipid, 16));
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
                  "handshake and reads its pages 0x00..0x0F.",
                  "lf xhorse info");
    void *argtable[] = { arg_param_begin, arg_param_end };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);

    lf_xhorse_info_t nfo;
    int res = xh_request_info(CMD_LF_XHORSE_INFO, NULL, 0, &nfo);
    if (res != PM3_SUCCESS) {
        return res;
    }

    // A reply to the XMICRO / channel-B identify means a newer super-chip family
    // (XT57 / XT37-class) that this command does not yet support.
    if (nfo.bchip) {
        // The family tag is a fixed 4 ASCII chars (e.g. "XT37").  A 5th byte often follows in
        // the reply; it is not part of the name and is ignored here.  It varies, and may be a
        // seed for channel-B encryption, as the 7372 reply is for channel A.
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
        PrintAndLogEx(WARNING, "Chip type " _YELLOW_("%s") " is not currently supported.", type_str);
        PrintAndLogEx(HINT, "Support for newer families may be added once the channel-B protocol is decoded.");
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
    xh_print_unconfirmed(&nfo);
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
                  "An XT136 has no banks: its pages 0x00..0x3F are read in one session (only\n"
                  "0x00..0x0F are known to exist).",
                  "lf xhorse dump");
    void *argtable[] = { arg_param_begin, arg_param_end };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);

    // One firmware reply holds one bank (both would exceed the reply size), so ask for each in turn.
    lf_xhorse_info_t nfo[2];
    uint8_t got = 0;
    for (uint8_t bank = 0; bank < 2; bank++) {
        int res = xh_request_info(CMD_LF_XHORSE_DUMP, &bank, 1, &nfo[bank]);
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
        xh_print_unconfirmed(&nfo[bank]);
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
                  "Configure which transponder an Xhorse XT27A emulates.  -t is a personality\n"
                  "type (see `lf xhorse types`); -u sets the emulated UID and is required for\n"
                  "every type that takes one.",
                  "lf xhorse configure -t ID46 -u 11223344     -> HITAG2, 4-byte UID\n"
                  "lf xhorse configure -t ID4D                 -> Texas Crypto, no UID\n"
                  "lf xhorse configure -t ID13 -u 20471BB01B   -> EM4102, 5-byte UID\n"
                  "lf xhorse configure -t 7939 -u 11223344     -> PCF7939, 4-byte UID\n"
                  "lf xhorse configure -t ID48 -u 11223344     -> Megamos, 4-byte UID\n"
                  "lf xhorse configure -t ID4C -u D3DA834C7E39EE98");

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
                  "Restore an Xhorse XT27A to its factory-default ID46/HITAG2 personality\n"
                  "(type 0x36 + default UID/password/config).  Does not blank the chip.\n"
                  "Also recovers a chip stuck emulating an EM type (ID11/12/13), ID48, ID8E or\n"
                  "ID8A, which never answers the normal handshake.",
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
                                  "1011011100110101110100011" "00";

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
                  "lf xhorse xt57 nonce --noanswer -f nonces.csv -n 500");
    void *argtable[] = {
        arg_param_begin,
        arg_int0("n", "count", "<dec>", "number of sessions (def 32)"),
        arg_str0(NULL, "sweep", "<name>", "gap to step: powerup, ackid, idsel, nonans, off or none (def none)"),
        arg_int0(NULL, "base", "<dec>", "swept gap for session 0, us or T0 (def: the VVDI's)"),
        arg_int0(NULL, "step", "<dec>", "added to the swept gap each session (def 1)"),
        arg_str0(NULL, "select", "<hex>", "13 byte select frame (def C0 'XT57' 123456789ABCDEF0)"),
        arg_str0(NULL, "answer", "<hex>", "always send this 32-bit answer"),
        arg_lit0(NULL, "noanswer", "do not answer, only record nonces"),
        arg_str0("f", "file", "<fn>", "also write a CSV of the sessions"),
        arg_int0(NULL, "off", "<dec>", "field-off time before each session in us (def 20000; the chip keeps its nonce state through ~33000)"),
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
    if (sellen > 0) {
        if (sellen != XHORSE_XT57_SELLEN) {
            PrintAndLogEx(FAILED, "select must be %d bytes (got %d)", XHORSE_XT57_SELLEN, sellen);
            CLIParserFree(ctx);
            return PM3_EINVARG;
        }
        memcpy(sel, selin, sizeof(sel));
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
    CLIParserFree(ctx);

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

    PrintAndLogEx(INFO, "%d sessions, sweep " _YELLOW_("%s") " from %d step %d %s.  Press " _GREEN_("Enter") " to stop",
                  total, xt57_sweep_names[sweep], base, step, xt57_sweep_units[sweep]);
    PrintAndLogEx(INFO, "  #  %-9s flg nonce     off  answer    reply             check",
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
                snprintf(cstr, sizeof(cstr), "no XMICRO ack");
            } else if ((r->flags & XT57_F_IDENT) == 0) {
                snprintf(cstr, sizeof(cstr), "no XT57X: %s", sprint_hex_inrow(r->ident, 5));
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

            PrintAndLogEx(INFO, "%3d %9d  %c%c%c %-9s %-4s %-9s %-17s %s", idx + 1, val,
                          (r->flags & XT57_F_ACK) ? 'A' : '-', (r->flags & XT57_F_IDENT) ? 'I' : '-',
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

static int CmdHelpXt57(const char *Cmd);
static command_t Xt57CommandTable[] = {
    {"help",  CmdHelpXt57,           AlwaysAvailable, "This help"},
    {"nonce", CmdLFXhorseXt57Nonce,  IfPm3Hitag,      "Collect XT57B nonces over a range of timings"},
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
