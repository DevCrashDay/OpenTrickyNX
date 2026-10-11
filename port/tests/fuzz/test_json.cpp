/*
 * test_json.cpp -- regression test of the defects the JSON fuzzer found
 *. Runs the inputs of regress/ and checks the decoded strings.
 *   out/test_json && echo ok
 */
#include "ot_update.h"

#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;

static void expect_name(const char *json, const std::string &want)
{
    std::vector<OtRelease> r;
    bool ok = ot_releases_parse(json, r) && r.size() == 1 && r[0].name == want;
    if (!ok) { fprintf(stderr, "FAIL %s\n", json); fails++; }
}

int main()
{
    /* a pair still makes one character */
    expect_name("[{\"name\":\"\\ud83d\\ude00\"}]", "\xF0\x9F\x98\x80");
    /* a lone high / low surrogate: U+FFFD, not 3 bytes of a surrogate */
    expect_name("[{\"name\":\"\\ud800\"}]", "\xEF\xBF\xBD");
    expect_name("[{\"name\":\"\\udc00x\"}]", "\xEF\xBF\xBDx");
    /* a high surrogate before a \u that is not a low one: both kept apart */
    expect_name("[{\"name\":\"\\ud83d\\u0041\"}]", "\xEF\xBF\xBD" "A");
    expect_name("[{\"name\":\"\\udbff\\uffff\"}]", "\xEF\xBF\xBD\xEF\xBF\xBF");
    expect_name("[{\"name\":\"\\ud800\\ud800\\udc00\"}]", "\xEF\xBF\xBD\xF0\x90\x80\x80");
    if (fails) return 1;
    puts("test_json: ok");
    return 0;
}
