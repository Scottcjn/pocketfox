/* SPDX-License-Identifier: MIT
 * pocketfox_render — HTML-to-text rendering for PocketFox.
 *
 * Extracted from the GUI into a standalone, unit-testable C module (the
 * renderer should not live buried in the Cocoa layer). Pure C89/C99, no
 * dependencies — builds and tests on any host, ships to PowerPC Tiger.
 */
#ifndef POCKETFOX_RENDER_H
#define POCKETFOX_RENDER_H

#include <stddef.h>

/* Render HTML to readable plain text.
 *   - strips <script>/<style>, converts block elements to newlines
 *   - decodes named AND numeric (&#NNN; / &#xHH;) entities to UTF-8
 *   - makes links visible: <a href="U">text</a> -> "text <U>"
 * Returns a malloc'd NUL-terminated string the caller must free(); NULL on
 * allocation failure. Output is bounds-checked (never overruns its buffer). */
char *pf_strip_html(const char *html, size_t html_len);

/* Extract the document <title> into title_buf (NUL-terminated, truncated to
 * bufsz). Sets an empty string if no title. */
void pf_extract_title(const char *html, size_t len, char *title_buf, size_t bufsz);

#endif /* POCKETFOX_RENDER_H */
