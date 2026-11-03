/*
 * Addresses: what the address bar and the command line accept, and the few URL operations the browser does itself. The engine
 * resolves links against the page's URL (user/lib/web/src/url.c) and libhttp parses http(s) URLs; this file covers the
 * rest: turning what a person typed into a URL, file: URLs and their paths, and telling the schemes apart.
 *
 * Follows RFC 3986 for the pieces it touches (scheme syntax, percent-encoding, the fragment) and RFC 8089 for file: URLs.
 */
#ifndef BROWSER_URLS_H
#define BROWSER_URLS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { BR_URL_NONE = 0, BR_URL_HTTP, BR_URL_HTTPS, BR_URL_FILE, BR_URL_ABOUT, BR_URL_DATA, BR_URL_OTHER } BrUrlKind;

/* The kind of a URL by its scheme (case-insensitive); BR_URL_NONE when it has none ("/a/b", "host.example/x"). */
BrUrlKind br_url_kind(const char *url);

/*
 * What a person typed or passed on the command line, as a URL:
 *   ""                         about:blank
 *   http://..., https://..., file:..., about:..., data:..., mailto:..., javascript:...    kept (spaces made %20)
 *   /dir/file, ./file, ../file, ~/file, and a bare name that exists in `cwd`              file:///... (made absolute against cwd, HOME for ~)
 *   localhost, 10.0.2.2:8000/x, localhost:8080                                           http://...
 *   example.org/path, www.example.org                                                    https://...
 *   a single word with no dot                                                            http://word/ (names from /etc/hosts)
 * Returns false (and out = "") for text that cannot be an address (it has spaces and is no path).
 */
bool br_url_from_input(const char *input, const char *cwd, const char *home, char *out, size_t cap);

/* The URL without its #fragment in `base`; *frag points at the text after '#' inside `url` (or NULL when there is none). */
void br_url_split_fragment(const char *url, char *base, size_t cap, const char **frag);
bool br_url_same_document(const char *a, const char *b);        /* equal up to the fragment */

/* file: URLs. The path is percent-decoded; only an empty host or "localhost" is accepted. False for anything else. */
bool br_file_url_to_path(const char *url, char *path, size_t cap);
void br_path_to_file_url(const char *path, char *url, size_t cap);

/* A short form of a URL for the status line and the title: "example.org/path" (scheme dropped for http(s)), cut with "..." */
void br_url_display(const char *url, char *out, size_t cap, size_t max_chars);

/* Percent-decodes in place; returns the new length. */
size_t br_percent_decode(char *s);

#endif
