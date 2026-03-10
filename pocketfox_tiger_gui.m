/*
 * PocketFox Browser - Web Browser for PowerPC Tiger / Leopard
 *
 * Features:
 *   - HTTPS via embedded mbedTLS (TLS 1.2)
 *   - HTTP via plain sockets
 *   - Redirect following (301/302/307)
 *   - Chunked transfer decoding
 *   - Back/Forward navigation
 *   - Bookmarks
 *   - Status bar with connection info
 *   - View Source mode
 *
 * Build on Tiger:
 *   gcc -arch ppc -O2 -mcpu=7450 -framework Cocoa -DHAVE_MBEDTLS \
 *       -I./mbedtls-2.28.8/include -o PocketFox \
 *       pocketfox_tiger_gui.m pocketfox_http.c pocketfox_ssl_tiger.c \
 *       -L./mbedtls-2.28.8/library -lmbedtls -lmbedx509 -lmbedcrypto
 */

#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pocketfox_http.h"

/* Forward declarations from pocketfox_ssl_tiger.c */
extern int pocketfox_ssl_init(void);
extern void pocketfox_ssl_shutdown(void);

/* ============================================
 * HTML Entity Decoder & Tag Stripper
 * ============================================ */

static char *strip_html(const char *html, size_t html_len) {
    char *text = (char *)malloc(html_len + 1);
    size_t j = 0;
    int in_tag = 0;
    int in_script = 0;
    size_t i;

    for (i = 0; i < html_len; i++) {
        char c = html[i];

        if (i + 7 < html_len && strncasecmp(html + i, "<script", 7) == 0) in_script = 1;
        if (i + 9 < html_len && strncasecmp(html + i, "</script>", 9) == 0) { in_script = 0; i += 8; continue; }
        if (i + 6 < html_len && strncasecmp(html + i, "<style", 6) == 0) in_script = 1;
        if (i + 8 < html_len && strncasecmp(html + i, "</style>", 8) == 0) { in_script = 0; i += 7; continue; }

        /* Convert <br>, <br/>, <p>, <div>, <li> to newlines */
        if (c == '<') {
            if (i + 4 < html_len && (strncasecmp(html + i, "<br>", 4) == 0 ||
                                      strncasecmp(html + i, "<br/", 4) == 0 ||
                                      strncasecmp(html + i, "<br ", 4) == 0)) {
                text[j++] = '\n';
            }
            if (i + 2 < html_len && (strncasecmp(html + i, "<p", 2) == 0 ||
                                      strncasecmp(html + i, "<d", 2) == 0)) {
                if (j > 0 && text[j-1] != '\n') text[j++] = '\n';
            }
            if (i + 3 < html_len && strncasecmp(html + i, "<li", 3) == 0) {
                if (j > 0 && text[j-1] != '\n') text[j++] = '\n';
                text[j++] = ' '; text[j++] = '-'; text[j++] = ' ';
            }
            if (i + 3 < html_len && (strncasecmp(html + i, "<h1", 3) == 0 ||
                                      strncasecmp(html + i, "<h2", 3) == 0 ||
                                      strncasecmp(html + i, "<h3", 3) == 0)) {
                if (j > 0 && text[j-1] != '\n') text[j++] = '\n';
                text[j++] = '\n';
            }
            in_tag = 1;
            continue;
        }
        if (c == '>') { in_tag = 0; continue; }

        if (!in_tag && !in_script) {
            if (c == '&') {
                if (strncmp(html + i, "&nbsp;", 6) == 0) { text[j++] = ' '; i += 5; }
                else if (strncmp(html + i, "&lt;", 4) == 0) { text[j++] = '<'; i += 3; }
                else if (strncmp(html + i, "&gt;", 4) == 0) { text[j++] = '>'; i += 3; }
                else if (strncmp(html + i, "&amp;", 5) == 0) { text[j++] = '&'; i += 4; }
                else if (strncmp(html + i, "&quot;", 6) == 0) { text[j++] = '"'; i += 5; }
                else if (strncmp(html + i, "&#39;", 5) == 0) { text[j++] = '\''; i += 4; }
                else if (strncmp(html + i, "&apos;", 6) == 0) { text[j++] = '\''; i += 5; }
                else if (strncmp(html + i, "&mdash;", 7) == 0) { text[j++] = '-'; text[j++] = '-'; i += 6; }
                else if (strncmp(html + i, "&ndash;", 7) == 0) { text[j++] = '-'; i += 6; }
                else if (strncmp(html + i, "&hellip;", 8) == 0) { text[j++] = '.'; text[j++] = '.'; text[j++] = '.'; i += 7; }
                else if (html[i+1] == '#') {
                    /* Numeric entity &#NNN; or &#xHH; */
                    const char *semi = strchr(html + i, ';');
                    if (semi && semi - (html + i) < 10) {
                        i = semi - html;
                        text[j++] = '?'; /* placeholder */
                    } else {
                        text[j++] = '&';
                    }
                }
                else text[j++] = '&';
            } else {
                text[j++] = c;
            }
        }
    }

    text[j] = '\0';

    /* Collapse multiple blank lines */
    char *clean = (char *)malloc(j + 1);
    size_t k = 0;
    int blank_count = 0;
    for (i = 0; i < j; i++) {
        if (text[i] == '\n') {
            blank_count++;
            if (blank_count <= 2) clean[k++] = '\n';
        } else {
            blank_count = 0;
            clean[k++] = text[i];
        }
    }
    clean[k] = '\0';
    free(text);
    return clean;
}

/* Extract <title>...</title> from HTML */
static void extract_title(const char *html, size_t len, char *title_buf, size_t bufsz) {
    title_buf[0] = '\0';
    const char *ts = NULL;
    size_t i;
    for (i = 0; i + 7 < len; i++) {
        if (strncasecmp(html + i, "<title>", 7) == 0) { ts = html + i + 7; break; }
        if (strncasecmp(html + i, "<title ", 7) == 0) {
            const char *gt = strchr(html + i, '>');
            if (gt) { ts = gt + 1; break; }
        }
    }
    if (!ts) return;

    const char *te = NULL;
    for (i = ts - html; i + 8 < len; i++) {
        if (strncasecmp(html + i, "</title>", 8) == 0) { te = html + i; break; }
    }
    if (!te || te <= ts) return;

    size_t tlen = te - ts;
    if (tlen >= bufsz) tlen = bufsz - 1;
    memcpy(title_buf, ts, tlen);
    title_buf[tlen] = '\0';

    /* Trim whitespace */
    char *s = title_buf;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (s != title_buf) memmove(title_buf, s, strlen(s) + 1);
    char *e = title_buf + strlen(title_buf) - 1;
    while (e > title_buf && (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r')) *e-- = '\0';
}

/* ============================================
 * History Stack
 * ============================================ */

#define MAX_HISTORY 100

typedef struct {
    char urls[MAX_HISTORY][2048];
    int count;
    int current;  /* -1 = none */
} History;

static void history_init(History *h) {
    h->count = 0;
    h->current = -1;
}

static void history_push(History *h, const char *url) {
    /* If we're not at the end, truncate forward history */
    if (h->current < h->count - 1) {
        h->count = h->current + 1;
    }
    if (h->count >= MAX_HISTORY) {
        /* Shift everything down */
        memmove(h->urls[0], h->urls[1], (MAX_HISTORY - 1) * 2048);
        h->count = MAX_HISTORY - 1;
    }
    strncpy(h->urls[h->count], url, 2047);
    h->urls[h->count][2047] = '\0';
    h->current = h->count;
    h->count++;
}

static const char *history_back(History *h) {
    if (h->current > 0) {
        h->current--;
        return h->urls[h->current];
    }
    return NULL;
}

static const char *history_forward(History *h) {
    if (h->current < h->count - 1) {
        h->current++;
        return h->urls[h->current];
    }
    return NULL;
}

/* ============================================
 * Bookmarks (simple file-based)
 * ============================================ */

#define MAX_BOOKMARKS 50
#define BOOKMARKS_FILE "~/.pocketfox_bookmarks"

typedef struct {
    char urls[MAX_BOOKMARKS][2048];
    char titles[MAX_BOOKMARKS][256];
    int count;
} Bookmarks;

static char *expand_tilde(const char *path) {
    if (path[0] != '~') return strdup(path);
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    size_t len = strlen(home) + strlen(path);
    char *out = (char *)malloc(len + 1);
    snprintf(out, len + 1, "%s%s", home, path + 1);
    return out;
}

static void bookmarks_load(Bookmarks *bm) {
    memset(bm, 0, sizeof(Bookmarks));
    char *path = expand_tilde(BOOKMARKS_FILE);
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return;

    char line[2304];
    while (fgets(line, sizeof(line), f) && bm->count < MAX_BOOKMARKS) {
        /* Format: URL<tab>Title */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = '\0';
            strncpy(bm->urls[bm->count], line, 2047);
            strncpy(bm->titles[bm->count], tab + 1, 255);
        } else {
            strncpy(bm->urls[bm->count], line, 2047);
            strncpy(bm->titles[bm->count], line, 255);
        }
        bm->count++;
    }
    fclose(f);
}

static void bookmarks_save(Bookmarks *bm) {
    char *path = expand_tilde(BOOKMARKS_FILE);
    FILE *f = fopen(path, "w");
    free(path);
    if (!f) return;
    int i;
    for (i = 0; i < bm->count; i++) {
        fprintf(f, "%s\t%s\n", bm->urls[i], bm->titles[i]);
    }
    fclose(f);
}

static void bookmarks_add(Bookmarks *bm, const char *url, const char *title) {
    if (bm->count >= MAX_BOOKMARKS) return;
    strncpy(bm->urls[bm->count], url, 2047);
    strncpy(bm->titles[bm->count], title[0] ? title : url, 255);
    bm->count++;
    bookmarks_save(bm);
}

/* ============================================
 * Application Controller
 * ============================================ */

@interface PocketFoxApp : NSObject {
    NSWindow     *window;
    NSTextField  *urlField;
    NSTextView   *contentView;
    NSTextField  *statusBar;
    NSButton     *backBtn;
    NSButton     *fwdBtn;
    NSButton     *goBtn;
    NSButton     *bookmarkBtn;
    NSPopUpButton *bookmarkMenu;
    History       history;
    Bookmarks     bookmarks;
    char          pageTitle[256];
    int           viewSource;
}
- (void)createUI;
- (void)navigateTo:(NSString *)url pushHistory:(BOOL)push;
- (void)goAction:(id)sender;
- (void)backAction:(id)sender;
- (void)forwardAction:(id)sender;
- (void)addBookmarkAction:(id)sender;
- (void)bookmarkSelected:(id)sender;
- (void)viewSourceAction:(id)sender;
- (void)updateNavButtons;
- (void)setStatus:(NSString *)text;
@end

@implementation PocketFoxApp

- (void)createUI {
    history_init(&history);
    bookmarks_load(&bookmarks);
    pageTitle[0] = '\0';
    viewSource = 0;

    /* Create window */
    window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(50, 50, 860, 650)
        styleMask:(NSTitledWindowMask | NSClosableWindowMask |
                   NSMiniaturizableWindowMask | NSResizableWindowMask)
        backing:NSBackingStoreBuffered
        defer:NO];

    [window setTitle:@"PocketFox"];
    [window setMinSize:NSMakeSize(500, 400)];

    NSView *content = [window contentView];

    /* ---- Toolbar row ---- */
    int y = 616;

    /* Back button */
    backBtn = [[NSButton alloc] initWithFrame:NSMakeRect(10, y, 30, 28)];
    [backBtn setTitle:@"<"];
    [backBtn setTarget:self];
    [backBtn setAction:@selector(backAction:)];
    [backBtn setBezelStyle:NSRoundedBezelStyle];
    [backBtn setEnabled:NO];
    [content addSubview:backBtn];

    /* Forward button */
    fwdBtn = [[NSButton alloc] initWithFrame:NSMakeRect(44, y, 30, 28)];
    [fwdBtn setTitle:@">"];
    [fwdBtn setTarget:self];
    [fwdBtn setAction:@selector(forwardAction:)];
    [fwdBtn setBezelStyle:NSRoundedBezelStyle];
    [fwdBtn setEnabled:NO];
    [content addSubview:fwdBtn];

    /* URL bar */
    urlField = [[NSTextField alloc] initWithFrame:NSMakeRect(82, y + 2, 600, 24)];
    [urlField setStringValue:@"https://example.com"];
    [urlField setTarget:self];
    [urlField setAction:@selector(goAction:)];
    [urlField setFont:[NSFont systemFontOfSize:13]];
    [urlField setAutoresizingMask:NSViewWidthSizable];
    [content addSubview:urlField];

    /* Go button */
    goBtn = [[NSButton alloc] initWithFrame:NSMakeRect(690, y, 50, 28)];
    [goBtn setTitle:@"Go"];
    [goBtn setTarget:self];
    [goBtn setAction:@selector(goAction:)];
    [goBtn setBezelStyle:NSRoundedBezelStyle];
    [goBtn setAutoresizingMask:NSViewMinXMargin];
    [content addSubview:goBtn];

    /* Bookmark add button */
    bookmarkBtn = [[NSButton alloc] initWithFrame:NSMakeRect(746, y, 24, 28)];
    [bookmarkBtn setTitle:@"+"];
    [bookmarkBtn setTarget:self];
    [bookmarkBtn setAction:@selector(addBookmarkAction:)];
    [bookmarkBtn setBezelStyle:NSRoundedBezelStyle];
    [bookmarkBtn setAutoresizingMask:NSViewMinXMargin];
    [content addSubview:bookmarkBtn];

    /* Bookmarks dropdown */
    bookmarkMenu = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(774, y, 76, 28) pullsDown:YES];
    [[bookmarkMenu cell] setArrowPosition:NSPopUpArrowAtBottom];
    [bookmarkMenu setTitle:@"Bookmarks"];
    [bookmarkMenu setTarget:self];
    [bookmarkMenu setAction:@selector(bookmarkSelected:)];
    [bookmarkMenu setAutoresizingMask:NSViewMinXMargin];
    [self rebuildBookmarkMenu];
    [content addSubview:bookmarkMenu];

    /* ---- Content area ---- */
    NSScrollView *scrollView = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(10, 26, 840, 585)];
    [scrollView setHasVerticalScroller:YES];
    [scrollView setHasHorizontalScroller:NO];
    [scrollView setAutohidesScrollers:NO];
    [scrollView setBorderType:NSBezelBorder];
    [scrollView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

    NSSize sz = [scrollView contentSize];
    contentView = [[NSTextView alloc]
        initWithFrame:NSMakeRect(0, 0, sz.width, sz.height)];
    [contentView setEditable:NO];
    [contentView setFont:[NSFont fontWithName:@"Monaco" size:11]];
    [contentView setMinSize:NSMakeSize(0, sz.height)];
    [contentView setMaxSize:NSMakeSize(FLT_MAX, FLT_MAX)];
    [contentView setVerticallyResizable:YES];
    [contentView setHorizontallyResizable:NO];
    [contentView setAutoresizingMask:NSViewWidthSizable];
    [[contentView textContainer] setContainerSize:NSMakeSize(sz.width, FLT_MAX)];
    [[contentView textContainer] setWidthTracksTextView:YES];

    [scrollView setDocumentView:contentView];
    [content addSubview:scrollView];

    /* ---- Status bar ---- */
    statusBar = [[NSTextField alloc] initWithFrame:NSMakeRect(10, 2, 840, 20)];
    [statusBar setEditable:NO];
    [statusBar setBezeled:NO];
    [statusBar setDrawsBackground:NO];
    [statusBar setFont:[NSFont systemFontOfSize:10]];
    [statusBar setTextColor:[NSColor grayColor]];
    [statusBar setStringValue:@"Ready"];
    [statusBar setAutoresizingMask:NSViewWidthSizable];
    [content addSubview:statusBar];

    /* Welcome message */
    [contentView setString:
        @"   ____            _        _   _____          \n"
        @"  |  _ \\ ___   ___| | _____| |_|  ___|____  __\n"
        @"  | |_) / _ \\ / __| |/ / _ \\ __| |_ / _ \\ \\/ /\n"
        @"  |  __/ (_) | (__|   <  __/ |_|  _| (_) >  < \n"
        @"  |_|   \\___/ \\___|_|\\_\\___|\\__|_|  \\___/_/\\_\\\n"
        @"\n"
        @"  Modern HTTPS on PowerPC Mac OS X Tiger\n"
        @"  Version 2.0 — TLS 1.2 via mbedTLS 2.28\n\n"
        @"  Enter a URL and press Go (or Return).\n\n"
        @"  Features:\n"
        @"    - TLS 1.2 (HTTPS) and plain HTTP\n"
        @"    - Redirect following (301/302/307)\n"
        @"    - Chunked transfer decoding\n"
        @"    - Back/Forward navigation\n"
        @"    - Bookmarks\n"
        @"    - View Source (Cmd+U)\n\n"
        @"  Try:  example.com  |  info.cern.ch  |  wttr.in/Paris\n\n"
        @"  github.com/Scottcjn/pocketfox\n"];

    [window makeKeyAndOrderFront:nil];
}

- (void)rebuildBookmarkMenu {
    [bookmarkMenu removeAllItems];
    [bookmarkMenu addItemWithTitle:@"Bookmarks"];

    if (bookmarks.count == 0) {
        [bookmarkMenu addItemWithTitle:@"(no bookmarks)"];
        [[bookmarkMenu lastItem] setEnabled:NO];
    } else {
        int i;
        for (i = 0; i < bookmarks.count; i++) {
            NSString *title = [NSString stringWithUTF8String:bookmarks.titles[i]];
            if (!title) title = [NSString stringWithCString:bookmarks.titles[i]
                                                   encoding:NSISOLatin1StringEncoding];
            [bookmarkMenu addItemWithTitle:title];
            [[bookmarkMenu lastItem] setTag:i];
        }
    }
}

- (void)updateNavButtons {
    [backBtn setEnabled:(history.current > 0)];
    [fwdBtn setEnabled:(history.current < history.count - 1)];
}

- (void)setStatus:(NSString *)text {
    [statusBar setStringValue:text];
    [statusBar display];
}

- (void)navigateTo:(NSString *)urlString pushHistory:(BOOL)push {
    /* Add scheme if missing */
    if (![urlString hasPrefix:@"http://"] && ![urlString hasPrefix:@"https://"]) {
        urlString = [@"https://" stringByAppendingString:urlString];
    }

    [urlField setStringValue:urlString];
    [self setStatus:[NSString stringWithFormat:@"Connecting to %@...", urlString]];
    [contentView setString:@"Loading..."];
    [[contentView window] display];

    const char *url = [urlString UTF8String];

    /* Fetch with up to 10 redirects */
    HttpResponse *resp = http_fetch(url, 10);

    if (!resp || resp->status_code < 0) {
        NSString *err = resp ? [NSString stringWithFormat:@"Error: %s",
                                resp->error] : @"Error: NULL response";
        [contentView setString:err];
        [self setStatus:@"Failed"];
        if (resp) http_response_free(resp);
        return;
    }

    /* Update URL bar with final URL (after redirects) */
    NSString *finalUrl = [NSString stringWithUTF8String:resp->final_url];
    if (finalUrl) [urlField setStringValue:finalUrl];

    /* Extract title */
    extract_title(resp->body, resp->body_len, pageTitle, sizeof(pageTitle));

    /* Update window title */
    if (pageTitle[0]) {
        NSString *wtitle = [NSString stringWithUTF8String:pageTitle];
        if (wtitle)
            [window setTitle:[NSString stringWithFormat:@"%@ - PocketFox", wtitle]];
    } else {
        [window setTitle:@"PocketFox"];
    }

    /* Display content */
    NSString *displayText;
    if (viewSource) {
        displayText = [NSString stringWithUTF8String:resp->body];
    } else {
        char *stripped = strip_html(resp->body, resp->body_len);
        displayText = [NSString stringWithUTF8String:stripped];
        if (!displayText)
            displayText = [NSString stringWithCString:stripped
                                             encoding:NSISOLatin1StringEncoding];
        free(stripped);
    }

    if (!displayText) displayText = @"(Could not decode response)";
    [contentView setString:displayText];
    [contentView scrollRangeToVisible:NSMakeRange(0, 0)];

    /* Status bar */
    NSString *status = [NSString stringWithFormat:@"HTTP %d | %s | %lu bytes",
                        resp->status_code,
                        resp->content_type[0] ? resp->content_type : "unknown",
                        (unsigned long)resp->body_len];
    if (resp->redirect_count > 0)
        status = [status stringByAppendingFormat:@" | %d redirect(s)", resp->redirect_count];
    [self setStatus:status];

    /* Push to history */
    if (push) {
        history_push(&history, resp->final_url);
        [self updateNavButtons];
    }

    http_response_free(resp);
}

- (void)goAction:(id)sender {
    NSString *urlString = [urlField stringValue];
    if ([urlString length] == 0) return;
    viewSource = 0;
    [self navigateTo:urlString pushHistory:YES];
}

- (void)backAction:(id)sender {
    const char *url = history_back(&history);
    if (url) {
        [self navigateTo:[NSString stringWithUTF8String:url] pushHistory:NO];
        [self updateNavButtons];
    }
}

- (void)forwardAction:(id)sender {
    const char *url = history_forward(&history);
    if (url) {
        [self navigateTo:[NSString stringWithUTF8String:url] pushHistory:NO];
        [self updateNavButtons];
    }
}

- (void)addBookmarkAction:(id)sender {
    const char *url = [[urlField stringValue] UTF8String];
    if (url && strlen(url) > 0) {
        bookmarks_add(&bookmarks, url, pageTitle);
        [self rebuildBookmarkMenu];
        [self setStatus:@"Bookmark added"];
    }
}

- (void)bookmarkSelected:(id)sender {
    int idx = (int)[[bookmarkMenu selectedItem] tag];
    if (idx >= 0 && idx < bookmarks.count) {
        NSString *url = [NSString stringWithUTF8String:bookmarks.urls[idx]];
        viewSource = 0;
        [self navigateTo:url pushHistory:YES];
    }
}

- (void)viewSourceAction:(id)sender {
    viewSource = !viewSource;
    /* Reload current page in source mode */
    NSString *url = [urlField stringValue];
    if ([url length] > 0) {
        [self navigateTo:url pushHistory:NO];
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    return YES;
}

@end

/* ============================================
 * Main
 * ============================================ */

int main(int argc, char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    pocketfox_ssl_init();

    [NSApplication sharedApplication];

    PocketFoxApp *app = [[PocketFoxApp alloc] init];
    [NSApp setDelegate:app];

    /* ---- Menu bar ---- */
    NSMenu *mainMenu = [[NSMenu alloc] init];

    /* App menu */
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:appMenuItem];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"PocketFox"];
    [appMenu addItemWithTitle:@"About PocketFox" action:nil keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit PocketFox"
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    [appMenuItem setSubmenu:appMenu];

    /* View menu */
    NSMenuItem *viewMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:viewMenuItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *srcItem = [[NSMenuItem alloc]
        initWithTitle:@"View Source"
               action:@selector(viewSourceAction:)
        keyEquivalent:@"u"];
    [srcItem setTarget:app];
    [viewMenu addItem:srcItem];
    [viewMenuItem setSubmenu:viewMenu];

    /* Navigate menu */
    NSMenuItem *navMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:navMenuItem];
    NSMenu *navMenu = [[NSMenu alloc] initWithTitle:@"Navigate"];

    NSMenuItem *backItem = [[NSMenuItem alloc]
        initWithTitle:@"Back" action:@selector(backAction:) keyEquivalent:@"["];
    [backItem setTarget:app];
    [navMenu addItem:backItem];

    NSMenuItem *fwdItem = [[NSMenuItem alloc]
        initWithTitle:@"Forward" action:@selector(forwardAction:) keyEquivalent:@"]"];
    [fwdItem setTarget:app];
    [navMenu addItem:fwdItem];

    [navMenuItem setSubmenu:navMenu];

    [NSApp setMainMenu:mainMenu];

    /* Create UI */
    [app createUI];

    /* Run */
    [NSApp run];

    pocketfox_ssl_shutdown();
    [pool release];

    return 0;
}
