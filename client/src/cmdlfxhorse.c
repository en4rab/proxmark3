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
        PrintAndLogEx(INFO, "Identify reply... %s", sprint_hex_inrow(nfo.bchip_type, nfo.bchip_type_len));
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
                  "lf xhorse xt57 nonce --noanswer --select FF58543537 --selbits 40   -> a 40 bit select frame");
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
                if (r->ack[0] || r->ack[1]) {
                    snprintf(cstr, sizeof(cstr), "no XMICRO ack (got %02X%02X)", r->ack[0], r->ack[1]);
                } else {
                    snprintf(cstr, sizeof(cstr), "no XMICRO ack");
                }
            } else if ((r->flags & XT57_F_IDENT) == 0) {
                snprintf(cstr, sizeof(cstr), "ack %02X%02X, no XT57X: %s", r->ack[0], r->ack[1], sprint_hex_inrow(r->ident, 5));
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

typedef struct {
    uint32_t        nonce;
    uint32_t        answer;
    int             nex;        // exchanges this keystream reaches, 1..nex
    int             nfull;      // of those, the ones with a 32-bit data keystream too.  Past it
    //                             only a terminator ('E', a command byte alone) may be built.
    const uint8_t  *k0;
    const uint32_t *K;
    int32_t         centre;     // default centre of the power-up search, us
    int32_t         span;       // default half-width, us
    int32_t         step;       // search step, us.  The nonce window advances one stream bit per
    //                             8 us, so 8 would visit every nonce once - but the mapping
    //                             drifts by a bit either way, and a nonce visited once can be
    //                             stepped straight over.  2 gives each one a plateau of about
    //                             four steps, which it is not worth missing a session over.
} xt57_ks_t;

// Which nonce a given power-up delay produces is not quite repeatable: the window the chip
// takes drifts a bit either way, so some nonces sit on a wide plateau and some are only hit
// now and then.  On the tag here CDB9AE8C holds for about 10 us while 6DCD7463 is interleaved
// with its two neighbours and turns up in roughly one sample in forty - which is a session
// that silently does nothing.  So the search tries each nonce that reaches far enough, in
// order, and runs at the first one it lands on; that ordering is the only thing separating
// them, since a built session is not tied to the nonce the VVDI happened to record it at.
static const xt57_ks_t xt57_ks_CDB9AE8C = {
    0xCDB9AE8C, 0x1BA796D1, 182, 181, xt57_ksC_k0, xt57_ksC_K, 119500, 400, 2
};
static const xt57_ks_t xt57_ks_6DCD7463 = {
    0x6DCD7463, 0x519F0319, 21, 21, xt57_ks6_k0, xt57_ks6_K, 119500, 400, 2
};
static const xt57_ks_t *const xt57_ks_all[] = { &xt57_ks_CDB9AE8C, &xt57_ks_6DCD7463 };

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
    p.off_us = XT57_SESS_OFF_US;
    p.selbits = XHORSE_XT57_SELLEN * 8;
    memcpy(p.select, xt57_sel_default, XHORSE_XT57_SELLEN);

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
    p.off_us = XT57_SESS_OFF_US;
    p.selbits = XHORSE_XT57_SELLEN * 8;
    memcpy(p.select, xt57_sel_default, XHORSE_XT57_SELLEN);
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

    for (size_t i = 0; i < ARRAYLEN(xt57_ks_all) && at < 0; i++) {
        if (longest > xt57_ks_all[i]->nex) {
            continue;                               // keystream too short for this session
        }
        usable++;
        ks = xt57_ks_all[i];
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
static bool xt57_type_supported(const xh_type_t *t) {
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
    if (xt57_type_supported(t) == false) {
        PrintAndLogEx(FAILED, "%s has no XT57B recipe yet", t->canon);
        PrintAndLogEx(INFO, "its session is longer than the 181 exchanges of keystream recovered");
        PrintAndLogEx(INFO, "so far, or it writes pages this channel has no recipe for");
        PrintAndLogEx(HINT, "the XT57B takes ID46/7936/7937/7946, ID11, ID12, ID13 and 7935");
        return PM3_EINVARG;
    }

    const int want = (t->uid == UID_NONE) ? 0
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
    int nst = xt57_build_steps(t, uid, steps, XT57_MAXSTEPS, &arm_at);
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
    int nst = xt57_build_steps(t, uid, steps, XT57_MAXSTEPS, &arm_at);
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
static command_t Xt57CommandTable[] = {
    {"help",  CmdHelpXt57,           AlwaysAvailable, "This help"},
    {"nonce", CmdLFXhorseXt57Nonce,  IfPm3Hitag,      "Collect XT57B nonces over a range of timings"},
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
