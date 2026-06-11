/* SPDX-License-Identifier: MIT
 * pocketfox_render — HTML-to-text rendering for PocketFox. See header.
 */
#include "pocketfox_render.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strncasecmp */

/* Append one byte to a bounds-checked output buffer. */
#define PUT(ch) do { if (j + 1 < cap) out[j++] = (char)(ch); } while (0)

/* UTF-8 encode a Unicode code point into the bounds-checked buffer. */
static void put_utf8(char *out, size_t *jp, size_t cap, unsigned long cp) {
    size_t j = *jp;
    if (cp < 0x80) {
        PUT(cp);
    } else if (cp < 0x800) {
        PUT(0xC0 | (cp >> 6));
        PUT(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        PUT(0xE0 | (cp >> 12));
        PUT(0x80 | ((cp >> 6) & 0x3F));
        PUT(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        PUT(0xF0 | (cp >> 18));
        PUT(0x80 | ((cp >> 12) & 0x3F));
        PUT(0x80 | ((cp >> 6) & 0x3F));
        PUT(0x80 | (cp & 0x3F));
    } else {
        PUT('?');  /* out of Unicode range */
    }
    *jp = j;
}

/* If html+i begins an <a ...> open tag, copy its href value into url
 * (bounded). Returns 1 if an href was captured. */
static int capture_href(const char *html, size_t i, size_t len,
                        char *url, size_t urlsz) {
    const char *p, *end, *q;
    if (!(i + 2 < len) || strncasecmp(html + i, "<a", 2) != 0) return 0;
    if (!(html[i + 2] == ' ' || html[i + 2] == '\t' || html[i + 2] == '\n')) return 0;
    end = memchr(html + i, '>', len - i);
    if (!end) return 0;
    /* find href= within the tag */
    for (p = html + i; p < end - 5; p++) {
        if (strncasecmp(p, "href=", 5) == 0) {
            p += 5;
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                q = memchr(p, quote, end - p);
                if (!q) q = end;
            } else {
                q = p;
                while (q < end && *q != ' ' && *q != '>' && *q != '\t') q++;
            }
            {
                size_t n = (size_t)(q - p);
                if (n >= urlsz) n = urlsz - 1;
                memcpy(url, p, n);
                url[n] = '\0';
            }
            return url[0] != '\0';
        }
    }
    return 0;
}

char *pf_strip_html(const char *html, size_t html_len) {
    /* 2x + slack: block-element expansion and " <url>" link annotations can
     * make output longer than the input, so the buffer must exceed html_len
     * (the previous in-GUI version sized it html_len+1 with NO bounds check —
     * a latent overflow on <li>/<h*>-heavy or link-heavy pages). */
    size_t cap = html_len * 2 + 64;
    char *out = (char *)malloc(cap);
    size_t i, j = 0;
    int in_tag = 0, in_script = 0;
    char href[1024];
    if (!out) return NULL;

    for (i = 0; i < html_len; i++) {
        char c = html[i];

        if (i + 7 < html_len && strncasecmp(html + i, "<script", 7) == 0) in_script = 1;
        if (i + 9 < html_len && strncasecmp(html + i, "</script>", 9) == 0) { in_script = 0; i += 8; continue; }
        if (i + 6 < html_len && strncasecmp(html + i, "<style", 6) == 0) in_script = 1;
        if (i + 8 < html_len && strncasecmp(html + i, "</style>", 8) == 0) { in_script = 0; i += 7; continue; }

        if (c == '<') {
            if (i + 4 < html_len && (strncasecmp(html + i, "<br>", 4) == 0 ||
                                      strncasecmp(html + i, "<br/", 4) == 0 ||
                                      strncasecmp(html + i, "<br ", 4) == 0)) {
                PUT('\n');
            }
            if (i + 2 < html_len && (strncasecmp(html + i, "<p", 2) == 0 ||
                                      strncasecmp(html + i, "<d", 2) == 0)) {
                if (j > 0 && out[j-1] != '\n') PUT('\n');
            }
            if (i + 3 < html_len && strncasecmp(html + i, "<li", 3) == 0) {
                if (j > 0 && out[j-1] != '\n') PUT('\n');
                PUT(' '); PUT('-'); PUT(' ');
            }
            if (i + 3 < html_len && (strncasecmp(html + i, "<h1", 3) == 0 ||
                                      strncasecmp(html + i, "<h2", 3) == 0 ||
                                      strncasecmp(html + i, "<h3", 3) == 0)) {
                if (j > 0 && out[j-1] != '\n') PUT('\n');
                PUT('\n');
            }
            /* Link: capture href now; emit it after the anchor text closes. */
            if (capture_href(html, i, html_len, href, sizeof(href))) {
                /* href stays stashed; rendered at </a> below */
            }
            /* Close of an anchor: surface the URL so links are not invisible. */
            if (i + 4 <= html_len && strncasecmp(html + i, "</a>", 4) == 0 && href[0]) {
                PUT(' '); PUT('<');
                { const char *u = href; while (*u) PUT(*u++); }
                PUT('>');
                href[0] = '\0';
            }
            in_tag = 1;
            continue;
        }
        if (c == '>') { in_tag = 0; continue; }

        if (!in_tag && !in_script) {
            if (c == '&') {
                if (strncmp(html + i, "&nbsp;", 6) == 0) { PUT(' '); i += 5; }
                else if (strncmp(html + i, "&lt;", 4) == 0) { PUT('<'); i += 3; }
                else if (strncmp(html + i, "&gt;", 4) == 0) { PUT('>'); i += 3; }
                else if (strncmp(html + i, "&amp;", 5) == 0) { PUT('&'); i += 4; }
                else if (strncmp(html + i, "&quot;", 6) == 0) { PUT('"'); i += 5; }
                else if (strncmp(html + i, "&#39;", 5) == 0) { PUT('\''); i += 4; }
                else if (strncmp(html + i, "&apos;", 6) == 0) { PUT('\''); i += 5; }
                else if (strncmp(html + i, "&mdash;", 7) == 0) { PUT('-'); PUT('-'); i += 6; }
                else if (strncmp(html + i, "&ndash;", 7) == 0) { PUT('-'); i += 6; }
                else if (strncmp(html + i, "&hellip;", 8) == 0) { PUT('.'); PUT('.'); PUT('.'); i += 7; }
                else if (strncmp(html + i, "&copy;", 6) == 0) { PUT('('); PUT('c'); PUT(')'); i += 5; }
                else if (strncmp(html + i, "&reg;", 5) == 0) { PUT('('); PUT('R'); PUT(')'); i += 4; }
                else if (strncmp(html + i, "&trade;", 7) == 0) { PUT('('); PUT('t'); PUT('m'); PUT(')'); i += 6; }
                else if (html[i+1] == '#') {
                    /* Numeric entity &#NNN; (decimal) or &#xHH; (hex) — decode
                     * to the actual code point and UTF-8 encode it (previously
                     * emitted '?' as a placeholder). */
                    const char *semi = strchr(html + i, ';');
                    if (semi && semi - (html + i) < 12) {
                        unsigned long cp;
                        char *endp = NULL;
                        const char *num = html + i + 2;
                        if (*num == 'x' || *num == 'X')
                            cp = strtoul(num + 1, &endp, 16);
                        else
                            cp = strtoul(num, &endp, 10);
                        if (cp > 0) put_utf8(out, &j, cap, cp);
                        i = (size_t)(semi - html);
                    } else {
                        PUT('&');
                    }
                }
                else PUT('&');
            } else {
                PUT(c);
            }
        }
    }
    out[j] = '\0';

    /* Collapse runs of >2 blank lines. */
    {
        char *clean = (char *)malloc(j + 1);
        size_t k = 0, m;
        int blank = 0;
        if (!clean) return out;  /* return uncollapsed rather than fail */
        for (m = 0; m < j; m++) {
            if (out[m] == '\n') {
                if (++blank <= 2) clean[k++] = '\n';
            } else {
                blank = 0;
                clean[k++] = out[m];
            }
        }
        clean[k] = '\0';
        free(out);
        return clean;
    }
}

void pf_extract_title(const char *html, size_t len, char *title_buf, size_t bufsz) {
    const char *ts = NULL, *te = NULL;
    size_t i, n;
    title_buf[0] = '\0';
    for (i = 0; i + 7 < len; i++) {
        if (strncasecmp(html + i, "<title>", 7) == 0) { ts = html + i + 7; break; }
        if (strncasecmp(html + i, "<title ", 7) == 0) {
            const char *gt = strchr(html + i, '>');
            if (gt) { ts = gt + 1; break; }
        }
    }
    if (!ts) return;
    for (i = 0; ts + i < html + len && i + 8 < len; i++) {
        if (strncasecmp(ts + i, "</title>", 8) == 0) { te = ts + i; break; }
    }
    if (!te) te = html + len;
    n = (size_t)(te - ts);
    if (n >= bufsz) n = bufsz - 1;
    memcpy(title_buf, ts, n);
    title_buf[n] = '\0';
}
