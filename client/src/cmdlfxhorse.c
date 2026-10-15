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
// Low frequency Xhorse super-chip (XT27A) commands.
//
// `-t` is a chip type (IDnn, or the PCF 79nn form where one exists).
// Only XT27A types whose on-air write path is fully decoded are offered;
// ID8C (opaque) and the XT27B/XT57-only types are refused.
//-----------------------------------------------------------------------------

#include "cmdlfxhorse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cmdparser.h"    // command_t
#include "comms.h"
#include "commonutil.h"
#include "cliparser.h"
#include "pm3_cmd.h"
#include "ui.h"           // PrintAndLogEx
#include "util.h"
#include "util_posix.h"   // msclock (random seed)
#include "xhorse.h"       // shared struct / recipe enum

static int CmdHelp(const char *Cmd);

//-----------------------------------------------------------------------------
// Type catalog
//-----------------------------------------------------------------------------
typedef enum { UID_NONE, UID_OPT4, UID_REQ4, UID_REQ5, UID_REQ8 } xh_uidrule_t;

typedef struct {
    const char   *canon;     // canonical token
    const char   *aliases;   // space-separated alternative tokens, or ""
    uint8_t       recipe;    // xhorse_recipe_t
    uint8_t       typebyte;  // on-chip 0x3C low byte
    bool          has_type;  // write the type byte?
    bool          typeword;  // ID13: writes 0x0080008C
    xh_uidrule_t  uid;
    uint16_t      aux;       // config word for the template family
    const char   *emulates;  // short help string
} xh_type_t;

static const xh_type_t g_types[] = {
    // canon   aliases                              recipe             tb    htype tword  uidrule   aux     emulates
    { "ID11",  "",                                  XHORSE_R_ID11,     0x8C, true,  false, UID_REQ8, 0,      "EM fixed-code (auto 7D5F..)" },
    { "ID12",  "",                                  XHORSE_R_ID12,     0x8C, true,  false, UID_REQ8, 0,      "EM fixed-code (auto 660F..)" },
    { "ID13",  "",                                  XHORSE_R_ID13,     0x8C, true,  true,  UID_REQ5, 0,      "EM4102 (auto 20..)" },
    { "7935",  "ID33 ID40 ID41 ID42 ID43 ID44",     XHORSE_R_7935,     0x35, true,  false, UID_NONE, 0,      "PCF7935 / SECT (blanked)" },
    { "7936",  "ID46",                              XHORSE_R_HITAG2,   0x36, true,  false, UID_OPT4, 0,      "PCF7936 / HITAG2" },
    { "7937",  "",                                  XHORSE_R_HITAG2,   0x37, true,  false, UID_OPT4, 0,      "PCF7937 (HITAG2 image)" },
    { "7938",  "ID47",                              XHORSE_R_7938,     0x47, true,  false, UID_OPT4, 0,      "PCF7938" },
    { "7939",  "ID49",                              XHORSE_R_7939,     0x39, true,  false, UID_NONE, 0,      "PCF7939 (config only)" },
    { "7946",  "",                                  XHORSE_R_HITAG2,   0x46, true,  false, UID_OPT4, 0,      "PCF7946 (HITAG2 image)" },
    { "ID48",  "",                                  XHORSE_R_ID48,     0x48, true,  false, UID_REQ4, 0,      "Megamos / ID48" },
    { "ID4C",  "",                                  XHORSE_R_ID4C,     0x4C, true,  false, UID_REQ8, 0,      "CRC16-KERMIT UID" },
    { "ID4D",  "ID60 ID63",                         XHORSE_R_TEMPLATE, 0x4D, true,  false, UID_NONE, 0xAAF3, "config-template" },
    { "4D80",  "ID70 ID83",                         XHORSE_R_TEMPLATE, 0x80, true,  false, UID_NONE, 0x9201, "config-template" },
    { "ID4E",  "ID64",                              XHORSE_R_TEMPLATE, 0x64, true,  false, UID_NONE, 0x33A6, "config-template" },
    { "ID8A",  "",                                  XHORSE_R_ID8A,     0x00, false, false, UID_OPT4, 0,      "full image, no type byte" },
    { "ID8E",  "",                                  XHORSE_R_ID8E,     0x8E, true,  false, UID_OPT4, 0,      "UID at pages 0x0A/0x0B" },
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
        case UID_OPT4:
            return "4 bytes";
        case UID_REQ4:
            return "4 (req)";
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
    PrintAndLogEx(INFO, "  %-38s %-7s %-7s %s", "-t token(s)", "on-chip", "UID", "emulates");
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
        if (!t->has_type) {
            snprintf(onchip, sizeof(onchip), "(image)");
        } else if (t->typeword) {
            snprintf(onchip, sizeof(onchip), "0x%02X *", t->typebyte);
        } else {
            snprintf(onchip, sizeof(onchip), "0x%02X", t->typebyte);
        }
        PrintAndLogEx(INFO, "  %-38s %-7s %-7s %s", toks, onchip, uidrule_text(t->uid), t->emulates);
    }

    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "  * ID13 writes 0x3C = 0x0080008C (high half set).");
    PrintAndLogEx(INFO, "  Names are case-insensitive; " _YELLOW_("none") " = no UID accepted.");
    PrintAndLogEx(INFO, "  " _YELLOW_("4 bytes") " = auto-generated if -u is omitted (as the app does); " _YELLOW_("4 (req)") " = -u required.");
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
        if (g_types[i].has_type && !g_types[i].typeword && g_types[i].typebyte == tb) {
            return g_types[i].canon;
        }
    }
    return "unknown";
}

static int xh_request_info(uint16_t cmd, lf_xhorse_info_t *out) {
    clearCommandBuffer();
    SendCommandNG(cmd, NULL, 0);
    PacketResponseNG resp;
    if (WaitForResponseTimeout(cmd, &resp, 4000) == false) {
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

// info/dump never send the EM-recovery burst (it changes the chip), so a chip stuck in an EM or
// ID48 emulation simply doesn't answer them.  Say so.
static void xh_print_not_found(void) {
    PrintAndLogEx(FAILED, "No XT27A answered the " _YELLOW_("XHORSE") " handshake");
    PrintAndLogEx(HINT, "An XT27A emulating an EM type (ID11/12/13) or ID48 broadcasts continuously and never answers;");
    PrintAndLogEx(HINT, "`" _YELLOW_("lf xhorse reset") "` or `" _YELLOW_("lf xhorse configure") "` can recover it.");
}

static void xh_print_header(const lf_xhorse_info_t *nfo) {
    PrintAndLogEx(INFO, "Chip ID... " _GREEN_("%s"), sprint_hex_inrow(nfo->chipid, 16));
    PrintAndLogEx(INFO, "Type byte. " _YELLOW_("0x%02X") " (%s)", nfo->typebyte, typebyte_name(nfo->typebyte));
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
                  "and the first 8 emulated-image pages.",
                  "lf xhorse info");
    void *argtable[] = { arg_param_begin, arg_param_end };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);

    lf_xhorse_info_t nfo;
    int res = xh_request_info(CMD_LF_XHORSE_INFO, &nfo);
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

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Xhorse XT27A") " ---------------------------");
    xh_print_header(&nfo);
    for (uint8_t pg = 0; pg < nfo.image_pages; pg++) {
        PrintAndLogEx(INFO, "  bank0 0x%02X: %s", pg, sprint_hex_inrow(&nfo.image[pg * 4], 4));
    }
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
                  "the type byte, and bank-0 pages 0x00..0x3F (the emulated-tag image).",
                  "lf xhorse dump");
    void *argtable[] = { arg_param_begin, arg_param_end };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);

    lf_xhorse_info_t nfo;
    int res = xh_request_info(CMD_LF_XHORSE_DUMP, &nfo);
    if (res != PM3_SUCCESS) {
        return res;
    }
    if (nfo.found == 0) {
        xh_print_not_found();
        return PM3_ESOFT;
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Xhorse XT27A dump") " ----------------------");
    xh_print_header(&nfo);
    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, " page | data");
    PrintAndLogEx(INFO, "------+------------");
    for (uint8_t pg = 0; pg < nfo.image_pages; pg++) {
        PrintAndLogEx(INFO, "  0x%02X | %s", pg, sprint_hex_inrow(&nfo.image[pg * 4], 4));
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
    // Configure/reset open two sessions; each can spend a second or so on handshake retries,
    // and the first may add up to three ~2.2 s EM-recovery attempts (worst case ~9.5 s in total).
    // Allow generous time.
    if (WaitForResponseTimeout(cmd, &resp, 30000) == false) {
        PrintAndLogEx(WARNING, "timeout while waiting for reply");
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        return PM3_ETIMEOUT;
    }
    if (resp.status != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "%s failed on-chip (status %d) - read-back verify mismatch or no tag", what, resp.status);
        return PM3_ESOFT;
    }
    PrintAndLogEx(SUCCESS, "%s " _GREEN_("ok"), what);
    return PM3_SUCCESS;
}

static void xh_fill_random4(uint8_t *u) {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)msclock());
        seeded = true;
    }
    for (int i = 0; i < 4; i++) {
        u[i] = (uint8_t)(rand() & 0xFF);
    }
}

//-----------------------------------------------------------------------------
// configure
//-----------------------------------------------------------------------------
static int CmdLFXhorseConfigure(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf xhorse configure",
                  "Configure which transponder an Xhorse XT27A emulates.  -t is a personality\n"
                  "type (see `lf xhorse types`); -u sets the emulated UID where the type takes one.",
                  "lf xhorse configure -t ID46                 -> HITAG2, auto 4-byte UID\n"
                  "lf xhorse configure -t 7936 -u 11223344     -> same type, chosen UID\n"
                  "lf xhorse configure -t ID13 -u 20471BB01B   -> EM4102, 5-byte UID\n"
                  "lf xhorse configure -t ID48 -u 11223344     -> Megamos, 4-byte UID\n"
                  "lf xhorse configure -t ID4C -u D3DA834C7E39EE98");

    void *argtable[] = {
        arg_param_begin,
        arg_str1("t", "type", "<name>", "emulated-type name, e.g. ID46 / 7936 / ID13"),
        arg_str0("u", "uid",  "<hex>",   "emulated UID (length depends on type)"),
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
        case UID_OPT4:
            if (uidlen == 0) {
                xh_fill_random4(uid);
                if (t->recipe == XHORSE_R_ID8A) {   // auto-UID format: byte0==byte2, byte3=0x59
                    uid[2] = uid[0];
                    uid[3] = 0x59;
                }
                uidlen = 4;
                PrintAndLogEx(INFO, "auto UID.. " _YELLOW_("%s"), sprint_hex_inrow(uid, 4));
            } else if (uidlen != 4) {
                PrintAndLogEx(FAILED, "%s needs a 4-byte UID (got %d)", t->canon, uidlen);
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

    PrintAndLogEx(INFO, "configuring " _GREEN_("%s") " (on-chip %s0x%02X)%s",
                  t->canon, t->has_type ? "" : "image, no ", t->typebyte, p.verify ? ", verify" : "");
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
                  "Also recovers a chip stuck emulating an EM type (ID11/12/13) or ID48, which\n"
                  "never answers the normal handshake.",
                  "lf xhorse reset");
    void *argtable[] = { arg_param_begin, arg_param_end };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);

    return xh_send_wait(CMD_LF_XHORSE_RESET, NULL, "reset to default ID46");
}

//-----------------------------------------------------------------------------
static command_t CommandTable[] = {
    {"help",      CmdHelp,             AlwaysAvailable, "This help"},
    {"-----------", CmdHelp,           AlwaysAvailable, "----------------- " _CYAN_("Xhorse XT27A") " -----------------"},
    {"types",     CmdLFXhorseTypes,    AlwaysAvailable, "List the -t type names accepted by configure"},
    {"info",      CmdLFXhorseInfo,     IfPm3Hitag,      "Identify a chip: ID, type, emulated image"},
    {"dump",      CmdLFXhorseDump,     IfPm3Hitag,      "Read all reachable channel-A storage"},
    {"configure", CmdLFXhorseConfigure, IfPm3Hitag,     "Set which transponder the chip emulates"},
    {"reset",     CmdLFXhorseReset,    IfPm3Hitag,      "Restore the default ID46 personality"},
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
