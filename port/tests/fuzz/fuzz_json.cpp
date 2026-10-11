/*
 * fuzz_json.cpp -- libFuzzer harness for the launcher's JSON reader
 * (Json + ot_releases_parse in port/launcher/app/ot_update.cpp).
 *
 * The production files are compiled unchanged (see build.sh); the few SDL
 * calls they make outside the parser are link-time stubs (sdl_stubs.cpp).
 * Checks beyond "no crash / no ASan / no UBSan report":
 *  - every string handed back is valid UTF-8 (the launcher draws it);
 *  - ot_update_notes() and ot_update_pick() run on what was parsed.
 */
#include "ot_update.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static bool utf8_ok(const std::string &s)
{
    const unsigned char *p = (const unsigned char *)s.data(), *e = p + s.size();
    while (p < e) {
        unsigned c = *p++;
        if (c < 0x80) continue;
        int n;
        unsigned cp, min;
        if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (e - p < n) return false;
        for (int i = 0; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += n;
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    }
    return true;
}

static void check(const std::string &what, const std::string &s, const std::string &input)
{
    /* Raw bytes from the input are copied as they are; only check strings
     * when the input itself is valid UTF-8 (the reader must not make it worse). */
    if (!utf8_ok(input) || utf8_ok(s)) return;
    fprintf(stderr, "invalid UTF-8 in %s (%zu bytes)\n", what.c_str(), s.size());
    abort();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    std::string in((const char *)data, size);
    std::vector<OtRelease> rel;
    if (!ot_releases_parse(in, rel)) return 0;
    for (const OtRelease &r : rel) {
        check("tag", r.tag, in); check("name", r.name, in);
        check("url", r.url, in); check("body", r.body, in);
        for (const OtAsset &a : r.assets) { check("asset name", a.name, in); check("asset url", a.url, in); }
        OtNotes n = ot_update_notes(r);
        check("notes title", n.title, in);
    }
    (void)ot_update_pick(rel, false);
    (void)ot_update_pick(rel, true);
    return 0;
}
