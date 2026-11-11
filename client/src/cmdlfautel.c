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
// Low frequency Autel AT100 programmable transponder commands.
//
// The AT100 is an emulator blank.  Its management interface is stock HITAG2 with a
// shared device key; page 0xFE is the emulated-type register and the low pages hold
// the emulated transponder's image.  Protocol and per-type images: docs/at100.md.
//
// All per-type knowledge lives in this file: the air protocol is uniform (write
// 0xFE, then the image pages), so `configure` just hands the firmware the page list
// to replay.  Every image below is the exact decrypted byte sequence the Autel
// programmer wrote in data/traces/at100/.
//-----------------------------------------------------------------------------

#include "cmdlfautel.h"

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
#include "util_posix.h"   // msclock
#include "autel.h"        // shared struct definitions

static int CmdHelp(const char *Cmd);

// Firmware can spend ~6 s on an ID8A burst (41 pages x ~100 ms EEPROM plus verify
// reads) on top of up to six session-open attempts.  60 s covers every command here.
#define AUTEL_TIMEOUT_MS  60000

//-----------------------------------------------------------------------------
// Type catalogue
//-----------------------------------------------------------------------------
// One page write of an image.  Pages whose value depends on the user's -u are
// listed with value 0 and filled in by at_place().
typedef struct {
    uint8_t  page;
    uint32_t value;
} at_pw_t;

typedef enum { AT_UID_NONE = 0, AT_UID_4, AT_UID_5, AT_UID_8 } at_uidrule_t;

// Where -u goes, and how it is encoded.
typedef enum {
    AT_PLACE_NONE = 0,
    AT_PLACE_P00,       // 4 bytes -> page 0x00
    AT_PLACE_P06,       // 4 bytes -> page 0x06   (the ID4D / ID4E family)
    AT_PLACE_PC8,       // 4 bytes -> page 0xC8   (MQB49 / ID5C)
    AT_PLACE_P00P01,    // 4 bytes -> pages 0x00 and 0x01, same value twice (ID8C)
    AT_PLACE_ID4C,      // 8 bytes -> 0x00, 0x01 + CRC-16/XMODEM in the high half of 0x02
    AT_PLACE_RAW64,     // 8 bytes verbatim -> pages 0x01, 0x02 (ID11 / ID12)
    AT_PLACE_EM4102,    // 5-byte ID -> a 64-bit EM4102 frame -> pages 0x01, 0x02 (ID13)
} at_place_t;

// --- images, in the exact order the Autel writes them ------------------------

static const at_pw_t img_id11[] = {
    { 0x00, 0x02D00000 }, { 0xFE, 0x9B04019B }, { 0x01, 0 }, { 0x02, 0 },
};
static const at_pw_t img_id12[] = {
    { 0x00, 0x02B00000 }, { 0xFE, 0x9B04019B }, { 0x01, 0 }, { 0x02, 0 },
};
static const at_pw_t img_id13[] = {
    { 0x00, 0x02900000 }, { 0xFE, 0x9B04019B }, { 0x01, 0 }, { 0x02, 0 },
};
// The factory MIKRON / ID46 HITAG2 image: key "MIKRON", config 0x06, password AA4854.
static const at_pw_t img_7936[] = {
    { 0xFE, 0x8F08029B }, { 0x00, 0 },
    { 0x01, 0x4D494B52 }, { 0x02, 0x00004F4E }, { 0x03, 0x06AA4854 },
};
// 7946 and 7937 write the ID46 image, then re-type the chip with a second 0xFE write.
static const at_pw_t img_7946[] = {
    { 0xFE, 0x8F08029B }, { 0x00, 0 },
    { 0x01, 0x4D494B52 }, { 0x02, 0x00004F4E }, { 0x03, 0x06AA4854 },
    { 0xFE, 0x8F0C039B },
};
static const at_pw_t img_7937[] = {
    { 0xFE, 0x8F08029B }, { 0x00, 0 },
    { 0x01, 0x4D494B52 }, { 0x02, 0x00004F4E }, { 0x03, 0x06AA4854 },
    { 0x78, 0 }, { 0x79, 0 }, { 0x7A, 0 }, { 0x7B, 0 },
    { 0xFE, 0x8F10049B },
};
static const at_pw_t img_7938[] = {
    { 0xFE, 0x8F14059B }, { 0x00, 0 },
    { 0x01, 0x11112222 }, { 0x02, 0x33334444 }, { 0x03, 0x55556666 },
    { 0x04, 0x00FFFFFF }, { 0x05, 0xFFFFFFFF }, { 0x06, 0xFFFFFFFF }, { 0x07, 0xFFFFFFFF },
    { 0x78, 0 }, { 0x79, 0 }, { 0x7A, 0 }, { 0x7B, 0 },
};
static const at_pw_t img_id4a[] = {
    { 0xFE, 0x8F18069B }, { 0x00, 0 },
    { 0x01, 0x11112222 }, { 0x02, 0x33334444 }, { 0x03, 0x55556666 }, { 0x04, 0x77778888 },
    { 0x05, 0x00FFFFFF },
    { 0x78, 0 }, { 0x79, 0 }, { 0x7A, 0 }, { 0x7B, 0 },
};
static const at_pw_t img_7939[] = {
    { 0xFE, 0x8F1C079B }, { 0x00, 0 },
    { 0x01, 0 }, { 0x02, 0 }, { 0x03, 0 }, { 0x04, 0 },
    { 0x05, 0 }, { 0x06, 0 }, { 0x07, 0 }, { 0x08, 0 },
    { 0xF0, 0 }, { 0xF1, 0 }, { 0xF2, 0 }, { 0xF3, 0 }, { 0xF4, 0 },
};
static const at_pw_t img_id5c[] = {
    { 0xFE, 0x8F20089B }, { 0xC8, 0 },
    { 0x00, 0xFFFFFFFF }, { 0x01, 0xFFFFFFFF }, { 0x02, 0xFFFFFFFF }, { 0x03, 0xFFFFFFFF },
    { 0x04, 0 }, { 0x05, 0 }, { 0x06, 0 }, { 0x07, 0 },
    { 0x0C, 0x1AAE47EE }, { 0x0D, 0xF4050000 }, { 0x11, 0x841EB56A }, { 0x50, 0x16030900 },
    { 0xF0, 0x81898585 }, { 0xF1, 0x9B83828A }, { 0xF2, 0x8A8A8C8F }, { 0xF3, 0x8E8E8C88 },
    { 0xF4, 0x00000100 },
};
static const at_pw_t img_id4c[] = {
    { 0xFE, 0x9B25099B }, { 0x00, 0 }, { 0x01, 0 }, { 0x02, 0 },
};
static const at_pw_t img_id4d[] = {
    { 0xFE, 0x9B290A9B }, { 0x06, 0 },
    { 0x3C, 0x00460000 }, { 0x3D, 0 }, { 0x02, 0x03000000 },
    { 0x03, 0 }, { 0x04, 0 }, { 0x05, 0 }, { 0x07, 0 },
    { 0x08, 0 }, { 0x09, 0 }, { 0x0A, 0 }, { 0x0B, 0 },
};
static const at_pw_t img_4d80[] = {
    { 0xFE, 0x9B290A9B }, { 0x06, 0 },
    { 0x02, 0x03000000 }, { 0x03, 0 }, { 0x04, 0 }, { 0x05, 0 },
    { 0x07, 0 }, { 0x08, 0 }, { 0x09, 0 }, { 0x0A, 0 }, { 0x0B, 0 },
    { 0x0D, 0 }, { 0x0F, 0 }, { 0x11, 0 }, { 0x15, 0 },
    { 0x3A, 0 }, { 0x3B, 0 }, { 0x3C, 0x08460000 }, { 0x3D, 0 },
};
static const at_pw_t img_id4e[] = {
    { 0xFE, 0x9B2D0B9B }, { 0x06, 0 },
    { 0x3C, 0x00460000 }, { 0x3D, 0 }, { 0x02, 0xFF000000 },
    { 0x03, 0 }, { 0x04, 0 }, { 0x05, 0 },
    { 0x08, 0 }, { 0x09, 0 }, { 0x0A, 0 }, { 0x0B, 0 },
};
static const at_pw_t img_id8a[] = {
    { 0xFE, 0x9B310C9B },
    { 0x00, 0 },          { 0x01, 0 },          { 0x02, 0xA3480000 }, { 0x03, 0 },
    { 0x04, 0x39395420 }, { 0x05, 0 },          { 0x06, 0x00004D65 }, { 0x07, 0 },
    { 0x08, 0xFFFF060E }, { 0x09, 0 },          { 0x0A, 0xA3480000 }, { 0x0B, 0 },
    { 0x0C, 0 },          { 0x0D, 0 },          { 0x0E, 0 },          { 0x0F, 0 },
    { 0x10, 0x8A000000 }, { 0x11, 0 },          { 0x12, 0 },          { 0x13, 0 },
    { 0x14, 0 },          { 0x15, 0 },          { 0x16, 0 },          { 0x17, 0 },
    { 0x18, 0 },          { 0x19, 0 },          { 0x1A, 0 },          { 0x1B, 0 },
    { 0x1C, 0 },          { 0x1D, 0 },          { 0x1E, 0 },          { 0x1F, 0 },
    { 0x20, 0 },          { 0x21, 0 },          { 0x22, 0 },          { 0x23, 0 },
    { 0x24, 0 },          { 0x25, 0 },          { 0x26, 0 },          { 0x27, 0 },
};
static const at_pw_t img_id48[] = {
    { 0xFE, 0x8F360D9B }, { 0x00, 0 }, { 0x01, 0xDD92A118 },
    { 0x02, 0 }, { 0x03, 0 }, { 0x04, 0 }, { 0x05, 0 },
    { 0x06, 0xAAAAAAAA }, { 0x07, 0xAAAAAAAA },
};
static const at_pw_t img_mqb[] = {
    { 0xFE, 0x8F3A0E9B }, { 0x00, 0 }, { 0x01, 0x9052ECD8 },
    { 0x16, 0x85CAB39F }, { 0x17, 0x2348BB6A }, { 0x1D, 0x0000E42B }, { 0x1E, 0x8C090000 },
    { 0x02, 0 }, { 0x03, 0 }, { 0x04, 0 }, { 0x05, 0 },
    { 0x06, 0xAAAAAAAA }, { 0x07, 0xAAAAAAAA },
    { 0x0F, 0x00000700 }, { 0x1F, 0 },
};
static const at_pw_t img_id8c[] = {
    { 0xFE, 0x8F3E0F9B }, { 0x00, 0 }, { 0x01, 0 },
    { 0x02, 0 }, { 0x03, 0 }, { 0x04, 0 }, { 0x05, 0 },
};

typedef struct {
    const char    *canon;
    const char    *aliases;     // space-separated, or ""
    uint32_t       cfg;         // the type's final page-0xFE word
    at_uidrule_t   uidrule;
    at_place_t     place;
    const at_pw_t *img;
    uint8_t        nimg;
    const char    *emulates;
    const char    *note;        // extra caveat for `list`, or NULL
} at_type_t;

#define IMG(x)  x, (uint8_t)ARRAYLEN(x)

static const at_type_t g_types[] = {
    { "ID11", "",          0x9B04019B, AT_UID_8, AT_PLACE_RAW64,  IMG(img_id11), "Temic fixed-code",        "-u is the raw 64-bit payload: the ID-to-payload encoding is not decoded" },
    { "ID12", "",          0x9B04019B, AT_UID_8, AT_PLACE_RAW64,  IMG(img_id12), "Temic fixed-code",        "-u is the raw 64-bit payload: the ID-to-payload encoding is not decoded" },
    { "ID13", "",          0x9B04019B, AT_UID_5, AT_PLACE_EM4102, IMG(img_id13), "EM4102",                  "-u is the 5-byte EM4102 ID; the frame and its parities are built here" },
    { "7936", "ID46",      0x8F08029B, AT_UID_4, AT_PLACE_P00,    IMG(img_7936), "PCF7936 (HITAG2)",        NULL },
    { "7946", "",          0x8F0C039B, AT_UID_4, AT_PLACE_P00,    IMG(img_7946), "PCF7946 (HITAG2)",        NULL },
    { "7937", "",          0x8F10049B, AT_UID_4, AT_PLACE_P00,    IMG(img_7937), "PCF7937 (HITAG2)",        NULL },
    { "7938", "ID47",      0x8F14059B, AT_UID_4, AT_PLACE_P00,    IMG(img_7938), "PCF7938 / ID47",          NULL },
    { "ID4A", "",          0x8F18069B, AT_UID_4, AT_PLACE_P00,    IMG(img_id4a), "ID4A",                    NULL },
    { "7939", "ID49",      0x8F1C079B, AT_UID_4, AT_PLACE_P00,    IMG(img_7939), "PCF7939 / ID49",          NULL },
    { "ID5C", "MQB49",     0x8F20089B, AT_UID_4, AT_PLACE_PC8,    IMG(img_id5c), "MQB49",                   "serial goes to page 0xC8; pages 0x0C-0x50 and 0xF0-0xF4 are captured constants" },
    { "ID4C", "",          0x9B25099B, AT_UID_8, AT_PLACE_ID4C,   IMG(img_id4c), "Texas Instruments (TIRIS)", NULL },
    { "ID4D", "ID60 ID63", 0x9B290A9B, AT_UID_4, AT_PLACE_P06,    IMG(img_id4d), "Texas Crypto DST40",      "serial goes to page 0x06; every captured one began 0x03" },
    { "4D80", "ID70 ID83", 0x9B290A9B, AT_UID_4, AT_PLACE_P06,    IMG(img_4d80), "Texas Crypto DST80",      "same type code as ID4D; the variant is page 0x3C = 08460000" },
    { "ID4E", "ID64",      0x9B2D0B9B, AT_UID_4, AT_PLACE_P06,    IMG(img_id4e), "Texas Crypto ID4E",       "serial goes to page 0x06" },
    { "ID8A", "",          0x9B310C9B, AT_UID_4, AT_PLACE_P00,    IMG(img_id8a), "Texas Crypto DST128",     "40-page image; pages 0x02-0x10 are captured constants" },
    { "ID48", "",          0x8F360D9B, AT_UID_4, AT_PLACE_P00,    IMG(img_id48), "Megamos Crypto ID48",     "page 0x01 keeps a captured constant; its meaning is unknown" },
    { "MQB",  "ID8B",      0x8F3A0E9B, AT_UID_4, AT_PLACE_P00,    IMG(img_mqb),  "MQB (Megamos AES)",       "pages 0x01/0x16/0x17/0x1D/0x1E are captured constants" },
    { "ID8C", "",          0x8F3E0F9B, AT_UID_4, AT_PLACE_P00P01, IMG(img_id8c), "ID8C",                    "the 4-byte ID is written to pages 0x00 and 0x01" },
};
#define AT_NTYPES  ARRAYLEN(g_types)

// `reset` restores the as-shipped state: every blank read before programming held
// page 0xFE = 9B290A9B, which is exactly what the Autel's ID4D entry writes.
#define AT_RESET_TYPE  "ID4D"

//-----------------------------------------------------------------------------
// Type lookup
//-----------------------------------------------------------------------------
static bool token_matches(const at_type_t *t, const char *tok) {
    if (strcmp(t->canon, tok) == 0) {
        return true;
    }
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
        if (len && (len == toklen) && (strncmp(start, tok, len) == 0)) {
            return true;
        }
    }
    return false;
}

static const at_type_t *resolve_type(const char *raw) {
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

    for (size_t i = 0; i < AT_NTYPES; i++) {
        if (token_matches(&g_types[i], tok)) {
            return &g_types[i];
        }
    }
    return NULL;
}

// Name a page-0xFE word.  ID4D and 4D80 share one, so the first match wins and the
// caller is told both.
static const char *cfg_name(uint32_t cfg) {
    if (cfg == 0x9B290A9B) {
        return "ID4D / 4D80 (also the factory default)";
    }
    if (cfg == 0x9B04019B) {
        return "ID11 / ID12 / ID13";
    }
    for (size_t i = 0; i < AT_NTYPES; i++) {
        if (g_types[i].cfg == cfg) {
            return g_types[i].canon;
        }
    }
    return NULL;
}

static const char *uidrule_text(at_uidrule_t r) {
    switch (r) {
        case AT_UID_8:
            return "8 bytes";
        case AT_UID_5:
            return "5 bytes";
        case AT_UID_4:
            return "4 bytes";
        case AT_UID_NONE:
        default:
            return "none";
    }
}

static uint8_t uidrule_len(at_uidrule_t r) {
    switch (r) {
        case AT_UID_8:
            return 8;
        case AT_UID_5:
            return 5;
        case AT_UID_4:
            return 4;
        case AT_UID_NONE:
        default:
            return 0;
    }
}

//-----------------------------------------------------------------------------
// Encoders (both reproduce the captured bytes exactly - docs/at100.md SS10)
//-----------------------------------------------------------------------------
static uint32_t be32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

// CRC-16/XMODEM: poly 0x1021, init 0, no reflection, no final xor.  Verified
// against the ID4C capture: D210A09A00000000 -> 0x0F34, the high half of page 0x02.
static uint16_t at_crc16_xmodem(const uint8_t *d, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// Build the 64-bit EM4102 frame for a 5-byte ID: nine '1' header bits, ten rows of
// 4 data bits + even row parity, four even column-parity bits, one '0' stop bit.
// Verified against the ID13 capture: 0456B307C8 -> FF812A65CC07E238.
static void at_em4102(const uint8_t *id5, uint32_t *hi, uint32_t *lo) {
    uint8_t bits[64];
    size_t n = 0;
    for (int i = 0; i < 9; i++) {
        bits[n++] = 1;
    }
    uint8_t col[4] = {0};
    for (int i = 0; i < 10; i++) {
        uint8_t nib = (i & 1) ? (uint8_t)(id5[i / 2] & 0x0F) : (uint8_t)(id5[i / 2] >> 4);
        uint8_t par = 0;
        for (int b = 0; b < 4; b++) {
            uint8_t v = (uint8_t)((nib >> (3 - b)) & 1);
            bits[n++] = v;
            par ^= v;
            col[b] ^= v;
        }
        bits[n++] = par;
    }
    for (int b = 0; b < 4; b++) {
        bits[n++] = col[b];
    }
    bits[n++] = 0;

    uint32_t h = 0, l = 0;
    for (int i = 0; i < 32; i++) {
        h = (h << 1) | bits[i];
    }
    for (int i = 32; i < 64; i++) {
        l = (l << 1) | bits[i];
    }
    *hi = h;
    *lo = l;
}

//-----------------------------------------------------------------------------
// Build a configure payload
//-----------------------------------------------------------------------------
// Set every entry addressing `page` to `value`.  A page can appear more than once
// (7937/7946 write 0xFE twice); only the UID pages are patched, and those appear once.
static void at_patch(lf_autel_cfg_t *p, uint8_t page, uint32_t value) {
    for (uint8_t i = 0; i < p->count; i++) {
        if (p->w[i].page == page) {
            p->w[i].value = value;
        }
    }
}

static int at_build(lf_autel_cfg_t *p, const at_type_t *t, const uint8_t *uid) {
    memset(p, 0, sizeof(*p));
    if (t->nimg > AUTEL_MAX_WRITES) {
        PrintAndLogEx(FAILED, "internal: %s image is %u pages, the limit is %d",
                      t->canon, t->nimg, AUTEL_MAX_WRITES);
        return PM3_ESOFT;
    }
    p->count = t->nimg;
    for (uint8_t i = 0; i < t->nimg; i++) {
        p->w[i].page  = t->img[i].page;
        p->w[i].value = t->img[i].value;
    }

    switch (t->place) {
        case AT_PLACE_NONE:
            break;
        case AT_PLACE_P00:
            at_patch(p, 0x00, be32(uid));
            break;
        case AT_PLACE_P06:
            at_patch(p, 0x06, be32(uid));
            break;
        case AT_PLACE_PC8:
            at_patch(p, 0xC8, be32(uid));
            break;
        case AT_PLACE_P00P01:
            at_patch(p, 0x00, be32(uid));
            at_patch(p, 0x01, be32(uid));
            break;
        case AT_PLACE_ID4C:
            at_patch(p, 0x00, be32(uid));
            at_patch(p, 0x01, be32(uid + 4));
            at_patch(p, 0x02, (uint32_t)at_crc16_xmodem(uid, 8) << 16);
            break;
        case AT_PLACE_RAW64:
            at_patch(p, 0x01, be32(uid));
            at_patch(p, 0x02, be32(uid + 4));
            break;
        case AT_PLACE_EM4102: {
            uint32_t hi = 0, lo = 0;
            at_em4102(uid, &hi, &lo);
            at_patch(p, 0x01, hi);
            at_patch(p, 0x02, lo);
            break;
        }
    }
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// Transport helpers
//-----------------------------------------------------------------------------
static void at_print_not_found(void) {
    PrintAndLogEx(FAILED, "No AT100 answered the " _YELLOW_("1010") " wake handshake");
    PrintAndLogEx(HINT, "The AT100 ignores the standard HITAG2 START_AUTH, so `" _YELLOW_("lf hitag info") "` will not see it either.");
    PrintAndLogEx(HINT, "Check placement on the antenna and try again.");
}

static void at_print_auth_warning(const lf_autel_io_reply_t *r) {
    if (r->auth_ok) {
        return;
    }
    PrintAndLogEx(WARNING, "Authentication is " _RED_("suspect") ": page 0xFE came back as 0x%08X,", r->config);
    PrintAndLogEx(WARNING, "      and every config word seen so far ends in 0x9B.");
    PrintAndLogEx(HINT, "Either the UID demodulated wrongly (just retry) or this blank uses a different");
    PrintAndLogEx(HINT, "device key - pass it with " _YELLOW_("-k") ".  Values below may be nonsense.");
}

static int at_io_send(uint16_t cmd, const lf_autel_io_t *p, lf_autel_io_reply_t *out, int *status) {
    clearCommandBuffer();
    SendCommandNG(cmd, (uint8_t *)p, sizeof(*p));
    PacketResponseNG resp;
    if (WaitForResponseTimeout(cmd, &resp, AUTEL_TIMEOUT_MS) == false) {
        PrintAndLogEx(WARNING, "timeout while waiting for reply");
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        return PM3_ETIMEOUT;
    }
    if (resp.length < sizeof(lf_autel_io_reply_t)) {
        PrintAndLogEx(FAILED, "short reply from device");
        return PM3_ESOFT;
    }
    memcpy(out, resp.data.asBytes, sizeof(*out));
    if (status) {
        *status = resp.status;
    }
    if (out->found == 0) {
        at_print_not_found();
        return PM3_ESOFT;
    }
    return PM3_SUCCESS;
}

// Always two visible characters, so a following column stays aligned.
static const char *at_mark(const lf_autel_io_reply_t *r, uint8_t i) {
    return ((r->ok[i / 8] >> (i % 8)) & 1) ? "  " : " " _RED_("?");
}

static void at_print_header(const lf_autel_io_reply_t *r) {
    PrintAndLogEx(INFO, "UID....... " _GREEN_("%08X"), r->uid);
    const char *name = cfg_name(r->config);
    PrintAndLogEx(INFO, "Type...... page 0xFE = " _YELLOW_("%08X") " (%s)", r->config,
                  name ? name : _RED_("unknown type code"));
    if (name == NULL) {
        PrintAndLogEx(HINT, "index byte 0x%02X is not one this client knows; `" _YELLOW_("lf autel list") "` lists the known ones",
                      (uint8_t)((r->config >> 8) & 0xFF));
    }
}

static void at_warn_unconfirmed(const lf_autel_io_reply_t *r) {
    int bad = 0;
    for (uint8_t i = 0; i < r->count; i++) {
        bad += ((r->ok[i / 8] >> (i % 8)) & 1) ? 0 : 1;
    }
    if (bad) {
        PrintAndLogEx(WARNING, "%d page(s) marked " _RED_("?") " unconfirmed: no two reads agreed, so they may be wrong; try again", bad);
    }
}

// -k is optional everywhere; 0 tells the firmware to use the shared default key.
static int at_get_key(CLIParserContext *ctx, int idx, uint64_t *key) {
    uint8_t kb[8] = {0};
    int klen = 0;
    if (CLIParamHexToBuf(arg_get_str(ctx, idx), kb, sizeof(kb), &klen) != 0) {
        return PM3_EINVARG;
    }
    if (klen == 0) {
        *key = 0;
        return PM3_SUCCESS;
    }
    if (klen != 6) {
        PrintAndLogEx(FAILED, "-k takes a 6 byte (48-bit) key, got %d", klen);
        return PM3_EINVARG;
    }
    uint64_t k = 0;
    for (int i = 0; i < 6; i++) {
        k = (k << 8) | kb[i];
    }
    *key = k;
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// list
//-----------------------------------------------------------------------------
static int CmdLFAutelList(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf autel list",
                  "List the emulated-type names accepted by `lf autel configure -t`, with the\n"
                  "page-0xFE type code each one writes and the UID it takes.\n"
                  "\n"
                  "Every image is the exact byte sequence the Autel programmer writes, decrypted\n"
                  "out of data/traces/at100/.  Where an image contains constants whose meaning is\n"
                  "not decoded they are written as captured, and the type is noted below.",
                  "lf autel list");
    void *argtable[] = { arg_param_begin, arg_param_end };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "Autel AT100 - emulated types accepted by " _YELLOW_("`lf autel configure -t`"));
    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "  %-20s %-10s %-8s %-6s %s", "-t type", "page 0xFE", "UID", "pages", "emulates");
    PrintAndLogEx(INFO, "  %-20s %-10s %-8s %-6s %s", "--------------------", "----------", "--------", "------", "-------------------------");

    for (size_t i = 0; i < AT_NTYPES; i++) {
        const at_type_t *t = &g_types[i];
        char toks[32];
        if (t->aliases[0]) {
            snprintf(toks, sizeof(toks), "%s %s", t->canon, t->aliases);
        } else {
            snprintf(toks, sizeof(toks), "%s", t->canon);
        }
        PrintAndLogEx(INFO, "  %-20s %08X   %-8s %-6u %s", toks, t->cfg, uidrule_text(t->uidrule), t->nimg, t->emulates);
    }

    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "Notes:");
    for (size_t i = 0; i < AT_NTYPES; i++) {
        if (g_types[i].note) {
            PrintAndLogEx(INFO, "  " _YELLOW_("%-6s") " %s", g_types[i].canon, g_types[i].note);
        }
    }
    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "  Names are case-insensitive.  ID11/ID12/ID13 share one type code and ID4D/4D80");
    PrintAndLogEx(INFO, "  share another; those variants differ in the image, not in page 0xFE.");
    PrintAndLogEx(INFO, "  Factory default is " _YELLOW_("9B290A9B") " (ID4D) - what `" _YELLOW_("lf autel reset") "` restores.");
    PrintAndLogEx(INFO, "  Not captured, so not offered: 7935, ID8E, MQB Renegade, MQB 4.5.");
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// dump
//-----------------------------------------------------------------------------
static int CmdLFAutelDump(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf autel dump",
                  "Read every page of an AT100 that the Autel programmer is ever seen to use:\n"
                  "the emulated image at 0x00-0x27, the ID4D/ID4E registers at 0x3A-0x3D, 0x50,\n"
                  "0x78-0x7B, 0xC8, 0xF0-0xF4, the device-key page 0xFC and the type register 0xFE.\n"
                  "\n"
                  "The rest of the 256-page space is untouched in every capture; reach it with\n"
                  "`lf autel read` if you want to probe it.\n"
                  "\n"
                  "Every page is read until two reads agree; one marked " _RED_("?") " never did and may be wrong.",
                  "lf autel dump\n"
                  "lf autel dump -k 9F63A5E8B14B");
    void *argtable[] = {
        arg_param_begin,
        arg_str0("k", "key", "<hex>", "48-bit device key (def 9F63A5E8B14B)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    uint64_t key = 0;
    if (at_get_key(ctx, 1, &key) != PM3_SUCCESS) {
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    CLIParserFree(ctx);

    lf_autel_io_t p;
    memset(&p, 0, sizeof(p));
    p.key = key;

    lf_autel_io_reply_t r;
    int res = at_io_send(CMD_LF_AUTEL_DUMP, &p, &r, NULL);
    if (res != PM3_SUCCESS) {
        return res;
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Autel AT100 dump") " -----------------------");
    at_print_header(&r);
    at_print_auth_warning(&r);
    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "  page | data       | note");
    PrintAndLogEx(INFO, "-------+------------+-----------------------------");

    for (uint8_t i = 0; i < r.count; i++) {
        uint8_t pg = r.page[i];
        uint32_t v = be32(&r.data[i * 4]);
        const char *note = "";
        if (pg == AUTEL_PG_KEYSIG) {
            note = (v == AUTEL_KEYSIG) ? "device key, low 32 bits" : _RED_("device key page, unexpected value");
        } else if (pg == AUTEL_PG_CONFIG) {
            note = "emulated-type register";
        }
        PrintAndLogEx(INFO, "  0x%02X | %s%s | %s", pg, sprint_hex_inrow(&r.data[i * 4], 4), at_mark(&r, i), note);
    }
    at_warn_unconfirmed(&r);
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// read
//-----------------------------------------------------------------------------
static int CmdLFAutelRead(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf autel read",
                  "Read raw pages from an AT100.\n"
                  "\n"
                  "The page address is 8 bits, so 0x00-0xFF is the whole space.  Only the pages\n"
                  "`lf autel dump` lists are ever used by the Autel programmer; the rest have never\n"
                  "been seen read or written and may not be real storage.\n"
                  "\n"
                  "Every page is read until two reads agree; one marked " _RED_("?") " never did and may be wrong.\n"
                  "\n"
                  "A chip left emulating a talk-first type (EM4102 and friends) load-modulates\n"
                  "continuously and only listens for the wake after a genuine cold power-down, so\n"
                  "the built-in schedule works up to a 3 s field-off.  " _YELLOW_("--off") " / " _YELLOW_("--on") " override that\n"
                  "schedule with a single timing, for bring-up on a chip it cannot reach.",
                  "lf autel read -p 0xFE              -> the emulated-type register\n"
                  "lf autel read -p 0xFC              -> the device-key page\n"
                  "lf autel read -p 0 -n 8            -> the first 8 image pages\n"
                  "lf autel read -p 0 -n 64 -k 9F63A5E8B14B");
    void *argtable[] = {
        arg_param_begin,
        arg_int1("p", "page",  "<dec>", "first page, 0..255"),
        arg_int0("n", "count", "<dec>", "pages to read, 1..64 (def 1)"),
        arg_str0("k", "key",   "<hex>", "48-bit device key (def 9F63A5E8B14B)"),
        arg_int0(NULL, "off",  "<ms>",  "override the field-off time before the wake (bring-up)"),
        arg_int0(NULL, "on",   "<us>",  "override the field-on settle before the wake (bring-up)"),
        arg_param_end
    };
    // false: a bare `lf autel read` prints the help rather than quietly reading page 0.
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int page  = arg_get_int_def(ctx, 1, 0);
    int count = arg_get_int_def(ctx, 2, 1);
    uint64_t key = 0;
    if (at_get_key(ctx, 3, &key) != PM3_SUCCESS) {
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    int off_ms = arg_get_int_def(ctx, 4, 0);
    int on_us  = arg_get_int_def(ctx, 5, 0);
    CLIParserFree(ctx);

    if (off_ms < 0 || off_ms > 60000 || on_us < 0 || on_us > 65535) {
        PrintAndLogEx(FAILED, "--off must be 0..60000 ms and --on 0..65535 us");
        return PM3_EINVARG;
    }

    if (page < 0 || page > 255) {
        PrintAndLogEx(FAILED, "page must be 0..255 (the address field is 8 bits)");
        return PM3_EINVARG;
    }
    if (count < 1 || count > AUTEL_MAX_PAGES) {
        PrintAndLogEx(FAILED, "-n must be 1..%d", AUTEL_MAX_PAGES);
        return PM3_EINVARG;
    }
    if (page + count > 256) {
        PrintAndLogEx(FAILED, "page 0x%02X + %d pages runs past 0xFF, the last page", page, count);
        return PM3_EINVARG;
    }

    lf_autel_io_t p;
    memset(&p, 0, sizeof(p));
    p.page = (uint8_t)page;
    p.count = (uint8_t)count;
    p.key = key;
    p.off_ms = (uint16_t)off_ms;
    p.on_us = (uint16_t)on_us;

    lf_autel_io_reply_t r;
    int res = at_io_send(CMD_LF_AUTEL_READ, &p, &r, NULL);
    if (res != PM3_SUCCESS) {
        return res;
    }

    PrintAndLogEx(NORMAL, "");
    at_print_header(&r);
    at_print_auth_warning(&r);
    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "  page | data");
    PrintAndLogEx(INFO, "-------+-----------");
    for (uint8_t i = 0; i < r.count; i++) {
        PrintAndLogEx(INFO, "  0x%02X | %s%s", r.page[i], sprint_hex_inrow(&r.data[i * 4], 4), at_mark(&r, i));
    }
    at_warn_unconfirmed(&r);
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// write
//-----------------------------------------------------------------------------
static int CmdLFAutelWrite(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf autel write",
                  "Write one raw page of an AT100 and read it back.\n"
                  "\n"
                  "This writes whatever you give it with no regard for what the page means, so it\n"
                  "can leave the chip emulating nothing useful.  `lf autel configure` writes whole\n"
                  "coherent images and `lf autel reset` restores the factory type.\n"
                  "\n"
                  "The chip cannot be locked out this way: its UID, its device key and pages 0xFC /\n"
                  "0xFE live outside the emulated image, and every capture shows the management\n"
                  "interface answering whatever the chip is emulating.  " _YELLOW_("Page 0xFE is still the\n"
                  "type register") " - write a code no emulation engine knows and the chip may emulate\n"
                  "nothing until you set a valid one again (`lf autel list`).",
                  "lf autel write -p 0 -d 11223344         -> emulated image page 0x00\n"
                  "lf autel write -p 0xFE -d 8F08029B      -> set the emulated type to ID46\n"
                  "lf autel write -p 0x06 -d 033A09B0 --nv -> write without the read-back verify");
    void *argtable[] = {
        arg_param_begin,
        arg_int1("p", "page", "<dec>", "page to write, 0..255"),
        arg_str1("d", "data", "<hex>", "4 byte value to write"),
        arg_str0("k", "key",  "<hex>", "48-bit device key (def 9F63A5E8B14B)"),
        arg_lit0(NULL, "nv",  "skip the read-back verify (the page is still read back and shown)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int page = arg_get_int_def(ctx, 1, -1);
    uint8_t data[4] = {0};
    int dlen = 0;
    CLIGetHexWithReturn(ctx, 2, data, &dlen);
    uint64_t key = 0;
    if (at_get_key(ctx, 3, &key) != PM3_SUCCESS) {
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    bool nv = arg_get_lit(ctx, 4);
    CLIParserFree(ctx);

    if (dlen != 4) {
        PrintAndLogEx(FAILED, "-d takes exactly 4 bytes, got %d", dlen);
        return PM3_EINVARG;
    }
    if (page < 0 || page > 255) {
        PrintAndLogEx(FAILED, "page must be 0..255 (the address field is 8 bits)");
        return PM3_EINVARG;
    }

    lf_autel_io_t p;
    memset(&p, 0, sizeof(p));
    p.page = (uint8_t)page;
    p.value = be32(data);
    p.verify = nv ? 0 : 1;
    p.key = key;

    lf_autel_io_reply_t r;
    int status = PM3_SUCCESS;
    int res = at_io_send(CMD_LF_AUTEL_WRITE, &p, &r, &status);
    if (res != PM3_SUCCESS) {
        return res;
    }

    PrintAndLogEx(NORMAL, "");
    at_print_header(&r);
    at_print_auth_warning(&r);
    PrintAndLogEx(INFO, "wrote page 0x%02X = %s", page, sprint_hex_inrow(data, 4));
    PrintAndLogEx(INFO, "reads back      %s%s", sprint_hex_inrow(&r.data[0], 4), at_mark(&r, 0));

    if (memcmp(r.data, data, 4) == 0) {
        PrintAndLogEx(SUCCESS, "write " _GREEN_("ok"));
    } else if (status == PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "verify passed on-chip but the read-back differs; try reading the page again");
    } else {
        PrintAndLogEx(FAILED, "write " _RED_("did not take") " - the page still reads %s", sprint_hex_inrow(&r.data[0], 4));
        PrintAndLogEx(HINT, "the Autel only ever reads pages 0xFC and 0xFE, so a page that refuses a");
        PrintAndLogEx(HINT, "read-back may still have been written; re-run with " _YELLOW_("--nv") " to skip the check");
        return PM3_ESOFT;
    }
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// configure / reset - shared send and report
//-----------------------------------------------------------------------------
static int at_cfg_send(uint16_t cmd, const lf_autel_cfg_t *p, const at_type_t *t) {
    clearCommandBuffer();
    SendCommandNG(cmd, (uint8_t *)p, sizeof(*p));
    PacketResponseNG resp;
    if (WaitForResponseTimeout(cmd, &resp, AUTEL_TIMEOUT_MS) == false) {
        PrintAndLogEx(WARNING, "timeout while waiting for reply");
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        return PM3_ETIMEOUT;
    }
    if (resp.length < sizeof(lf_autel_cfg_reply_t)) {
        PrintAndLogEx(FAILED, "short reply from device");
        return PM3_ESOFT;
    }
    lf_autel_cfg_reply_t r;
    memcpy(&r, resp.data.asBytes, sizeof(r));

    if (r.found == 0) {
        at_print_not_found();
        return PM3_ESOFT;
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "UID....... " _GREEN_("%08X"), r.uid);
    const char *was = cfg_name(r.config_before);
    PrintAndLogEx(INFO, "Was....... %08X (%s)", r.config_before, was ? was : "unknown type code");
    if (r.auth_ok == 0) {
        PrintAndLogEx(WARNING, "Authentication was " _RED_("suspect") " (page 0xFE did not end in 0x9B);");
        PrintAndLogEx(WARNING, "      the write burst may have gone nowhere.  Re-read with `" _YELLOW_("lf autel dump") "`.");
    }

    if (resp.status != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "stopped after %u of %u page writes, at page " _RED_("0x%02X"),
                      r.written, p->count, r.failed_page);
        PrintAndLogEx(INFO, "Now....... %08X", r.config_after);
        PrintAndLogEx(HINT, "The chip is half-programmed; run the same command again.");
        return PM3_ESOFT;
    }

    const char *now = cfg_name(r.config_after);
    PrintAndLogEx(INFO, "Now....... " _GREEN_("%08X") " (%s)", r.config_after, now ? now : "unknown type code");
    if (t && (r.config_after != t->cfg)) {
        PrintAndLogEx(WARNING, "page 0xFE reads %08X but %s writes %08X; the read-back may be weak,",
                      r.config_after, t->canon, t->cfg);
        PrintAndLogEx(WARNING, "      check with `" _YELLOW_("lf autel read -p 0xFE") "`.");
    }
    PrintAndLogEx(SUCCESS, "%u page writes " _GREEN_("ok"), r.written);
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// configure
//-----------------------------------------------------------------------------
static int CmdLFAutelConfigure(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf autel configure",
                  "Configure which transponder an AT100 emulates.  -t is a type name (see\n"
                  "`lf autel list`); -u sets the emulated serial and is required for every type\n"
                  "that takes one.\n"
                  "\n"
                  "The whole burst runs in one authenticated session: page 0xFE first (which\n"
                  "selects the type), then that type's image pages, exactly as the Autel does.\n"
                  "Each page takes about 100 ms to commit, so a large image (ID8A writes 41 pages)\n"
                  "takes several seconds.\n"
                  "\n"
                  "`lf autel reset` restores the factory type.",
                  "lf autel configure -t ID46 -u 11223344        -> HITAG2 / MIKRON image\n"
                  "lf autel configure -t ID13 -u 0456B307C8      -> EM4102, 5-byte ID\n"
                  "lf autel configure -t ID48 -u 785626A1        -> Megamos ID48\n"
                  "lf autel configure -t ID4D -u 033A09B0        -> Texas Crypto DST40\n"
                  "lf autel configure -t ID4C -u D210A09A00000000\n"
                  "lf autel configure -t ID8A -u 59D4E00A --nv   -> skip the per-page verify");
    void *argtable[] = {
        arg_param_begin,
        arg_str1("t", "type", "<name>", "emulated-type name, e.g. ID46 / 7936 / ID13"),
        arg_str0("u", "uid",  "<hex>",  "emulated serial, required where the type takes one"),
        arg_str0("k", "key",  "<hex>",  "48-bit device key (def 9F63A5E8B14B)"),
        arg_lit0(NULL, "nv",  "skip the per-page read-back verify (much faster)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    char typestr[32] = {0};
    int tlen = 0;
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)typestr, sizeof(typestr) - 1, &tlen);

    uint8_t uid[8] = {0};
    int uidlen = 0;
    CLIGetHexWithReturn(ctx, 2, uid, &uidlen);

    uint64_t key = 0;
    if (at_get_key(ctx, 3, &key) != PM3_SUCCESS) {
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    bool nv = arg_get_lit(ctx, 4);
    CLIParserFree(ctx);

    const at_type_t *t = resolve_type(typestr);
    if (t == NULL) {
        PrintAndLogEx(FAILED, "unknown type token " _YELLOW_("%s"), typestr);
        PrintAndLogEx(HINT, "run `" _YELLOW_("lf autel list") "` for the supported list");
        return PM3_EINVARG;
    }

    uint8_t want = uidrule_len(t->uidrule);
    if (uidlen != want) {
        if (want == 0) {
            PrintAndLogEx(FAILED, "%s takes no -u", t->canon);
        } else {
            PrintAndLogEx(FAILED, "%s needs a %u-byte -u (got %d)", t->canon, want, uidlen);
        }
        return PM3_EINVARG;
    }

    lf_autel_cfg_t p;
    if (at_build(&p, t, uid) != PM3_SUCCESS) {
        return PM3_ESOFT;
    }
    p.verify = nv ? 0 : 1;
    p.key = key;

    // Advisory only.  A HITAG2 reader names the exact PCF part from bits 4..7 of the
    // UID, and the captures that set 7936 and 7946 chose UIDs whose nibble matched.
    // The 7937 capture did not, so this warns rather than refuses or rewrites.
    if ((strcmp(t->canon, "7936") == 0) || (strcmp(t->canon, "7946") == 0)) {
        uint8_t wantnib = (strcmp(t->canon, "7936") == 0) ? 1 : 2;
        uint8_t have = (uint8_t)((uid[3] >> 4) & 0x0F);
        if (have != wantnib) {
            PrintAndLogEx(WARNING, "UID nibble is %u, but %s is read from a UID whose nibble is %u;",
                          have, t->canon, wantnib);
            PrintAndLogEx(WARNING, "      a HITAG2 reader names the chip from the UID, not from the type code.");
        }
    }
    if ((t->place == AT_PLACE_P06) && (uid[0] != 0x03)) {
        PrintAndLogEx(WARNING, "every captured %s serial began 0x03; yours begins 0x%02X.", t->canon, uid[0]);
    }
    if (t->note) {
        PrintAndLogEx(INFO, _YELLOW_("note") ": %s", t->note);
    }

    PrintAndLogEx(INFO, "configuring " _GREEN_("%s") " (page 0xFE = %08X), %u page writes%s",
                  t->canon, t->cfg, p.count, p.verify ? ", verify" : "");
    return at_cfg_send(CMD_LF_AUTEL_CONFIGURE, &p, t);
}

//-----------------------------------------------------------------------------
// reset
//-----------------------------------------------------------------------------
static int CmdLFAutelReset(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf autel reset",
                  "Restore an AT100 to its factory-shipped configuration.\n"
                  "\n"
                  "Every blank read before it was ever programmed held page 0xFE = " _YELLOW_("9B290A9B") ",\n"
                  "which is exactly the type code the Autel's own ID4D entry writes, so this writes\n"
                  "the ID4D image and that type code.\n"
                  "\n"
                  "Caveat: only the type register is known to match a blank.  A blank's image pages\n"
                  "were never read (the Autel reads only 0xFC and 0xFE), so the image written here\n"
                  "is the one its ID4D menu entry produces, not a captured factory image.\n"
                  "\n"
                  "The serial at page 0x06 is random unless you give -u; every captured one began\n"
                  "0x03, so a generated one does too.",
                  "lf autel reset\n"
                  "lf autel reset -u 033A09B0");
    void *argtable[] = {
        arg_param_begin,
        arg_str0("u", "uid", "<hex>", "4 byte serial for page 0x06 (def: random, high byte 0x03)"),
        arg_str0("k", "key", "<hex>", "48-bit device key (def 9F63A5E8B14B)"),
        arg_lit0(NULL, "nv", "skip the per-page read-back verify"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint8_t uid[8] = {0};
    int uidlen = 0;
    CLIGetHexWithReturn(ctx, 1, uid, &uidlen);
    uint64_t key = 0;
    if (at_get_key(ctx, 2, &key) != PM3_SUCCESS) {
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    bool nv = arg_get_lit(ctx, 3);
    CLIParserFree(ctx);

    if (uidlen == 0) {
        srand((unsigned int)msclock());
        uid[0] = 0x03;
        uid[1] = (uint8_t)(rand() & 0xFF);
        uid[2] = (uint8_t)(rand() & 0xFF);
        uid[3] = (uint8_t)(rand() & 0xFF);
    } else if (uidlen != 4) {
        PrintAndLogEx(FAILED, "-u takes a 4 byte serial, got %d", uidlen);
        return PM3_EINVARG;
    }

    const at_type_t *t = resolve_type(AT_RESET_TYPE);
    if (t == NULL) {
        PrintAndLogEx(FAILED, "internal: reset type " AT_RESET_TYPE " is missing from the catalogue");
        return PM3_ESOFT;
    }

    lf_autel_cfg_t p;
    if (at_build(&p, t, uid) != PM3_SUCCESS) {
        return PM3_ESOFT;
    }
    p.verify = nv ? 0 : 1;
    p.key = key;

    PrintAndLogEx(INFO, "resetting to the factory type " _GREEN_("%08X") " (%s), serial %s",
                  t->cfg, t->canon, sprint_hex_inrow(uid, 4));
    return at_cfg_send(CMD_LF_AUTEL_RESET, &p, t);
}

//-----------------------------------------------------------------------------
// menu
//-----------------------------------------------------------------------------
static command_t CommandTable[] = {
    {"-----------", CmdHelp,           AlwaysAvailable, "----------------- " _CYAN_("Autel AT100") " -----------------"},
    {"help",      CmdHelp,             AlwaysAvailable, "This help"},
    {"list",      CmdLFAutelList,      AlwaysAvailable, "List the emulated types `configure` accepts"},
    {"dump",      CmdLFAutelDump,      IfPm3Hitag,      "Read every page the Autel programmer uses"},
    {"read",      CmdLFAutelRead,      IfPm3Hitag,      "Read raw pages"},
    {"write",     CmdLFAutelWrite,     IfPm3Hitag,      "Write one raw page"},
    {"configure", CmdLFAutelConfigure, IfPm3Hitag,      "Set which transponder the chip emulates"},
    {"reset",     CmdLFAutelReset,     IfPm3Hitag,      "Restore the factory-shipped configuration"},
    {NULL, NULL, NULL, NULL}
};

static int CmdHelp(const char *Cmd) {
    (void)Cmd;
    CmdsHelp(CommandTable);
    return PM3_SUCCESS;
}

int CmdLFAutel(const char *Cmd) {
    clearCommandBuffer();
    return CmdsParse(CommandTable, Cmd);
}
