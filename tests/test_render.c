/* SPDX-License-Identifier: MIT
 * Unit tests for pocketfox_render — runs on any host (no Cocoa/Tiger needed).
 *   cc -I.. -o /tmp/test_render test_render.c ../pocketfox_render.c && /tmp/test_render
 */
#include "../pocketfox_render.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;

static void expect_contains(const char *html, const char *needle, const char *label) {
    char *out = pf_strip_html(html, strlen(html));
    int ok = out && strstr(out, needle) != NULL;
    printf(ok ? "  PASS: %s\n" : "  FAIL: %s\n", label);
    if (!ok) { fails++; printf("        got: %s\n", out ? out : "(null)"); }
    free(out);
}

static void expect_absent(const char *html, const char *needle, const char *label) {
    char *out = pf_strip_html(html, strlen(html));
    int ok = out && strstr(out, needle) == NULL;
    printf(ok ? "  PASS: %s\n" : "  FAIL: %s\n", label);
    if (!ok) { fails++; printf("        got: %s\n", out ? out : "(null)"); }
    free(out);
}

int main(void) {
    char title[128];

    /* 1. THE bug we fixed: numeric entities used to render '?' */
    expect_contains("<p>caf&#233;</p>", "caf\xc3\xa9", "decimal entity &#233; -> UTF-8 e-acute");
    expect_contains("<p>&#8364;5</p>", "\xe2\x82\xac", "decimal entity &#8364; -> UTF-8 euro");
    expect_contains("<p>&#x2764;</p>", "\xe2\x9d\xa4", "hex entity &#x2764; -> UTF-8 heart");
    expect_absent("<p>caf&#233;</p>", "?", "no '?' placeholder left for numeric entities");

    /* 2. links are now visible (were stripped/invisible) */
    expect_contains("<a href=\"https://rustchain.org\">RustChain</a>",
                    "RustChain <https://rustchain.org>", "anchor surfaces its href");
    expect_contains("<a href='/page'>x</a>", "<", "single-quoted href captured");

    /* 3. named entities still work */
    expect_contains("a &amp; b", "a & b", "named &amp;");
    expect_contains("&copy;2026", "(c)2026", "named &copy; -> (c)");

    /* 4. script/style stripped, blocks -> newlines */
    expect_absent("<script>var x=1;</script>hi", "var x", "script body stripped");
    expect_contains("<li>one</li>", "- one", "li -> bullet");
    expect_contains("x<br>y", "x\ny", "<br> mid-buffer -> newline");
    expect_contains("x<br>", "x\n", "trailing <br> at buffer end -> newline (off-by-one)");

    /* 5. bounds safety: a li/h-heavy page must not crash (latent overflow fix) */
    {
        char big[4096]; int p = 0, n;
        for (n = 0; n < 200; n++) p += sprintf(big + p, "<li>x</li>");
        char *out = pf_strip_html(big, strlen(big));
        printf(out ? "  PASS: 200x <li> no crash/overflow\n" : "  FAIL: overflow\n");
        if (!out) fails++;
        free(out);
    }

    /* 6. title extraction */
    pf_extract_title("<html><head><title>Hello</title></head>", 40, title, sizeof(title));
    printf(strcmp(title, "Hello") == 0 ? "  PASS: title extracted\n" : "  FAIL: title (%s)\n", title);
    if (strcmp(title, "Hello") != 0) fails++;

    printf("\n%s\n", fails == 0 ? "ALL PASS" : "FAILED");
    return fails ? 1 : 0;
}
