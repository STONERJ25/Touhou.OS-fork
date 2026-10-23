/*
 * Kappa media helpers: the user's wallpaper, screenshot file names, and picture thumbnails for file browsers.
 * These sit on top of libimg; programs that only read or write the wallpaper setting (the `wallpaper` command) use just the
 * configuration half and do not pull in any drawing code.
 *
 * Wallpaper setting: ~/.config/hakurei/wallpaper is a text file. The first line is the path of the picture, the optional second
 * line the mode: cover (fill the screen, cropping the edges: the default), fit (whole picture, bars around it), center (original
 * size), tile (repeated), stretch (fill, ignoring the proportions). Blank lines and lines starting with # are ignored. When
 * the file is missing the system-wide /etc/hakurei/wallpaper is tried (the login screen uses that one, since nobody is signed in
 * yet), and when neither names a readable picture the procedural night scene (k_scenery_night) is drawn.
 */
#ifndef KAPPA_MEDIA_H
#define KAPPA_MEDIA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <kappa.h>

typedef enum { KW_COVER, KW_FIT, KW_CENTER, KW_TILE, KW_STRETCH } KWallMode;

typedef struct KWallConfig {
    char path[512];
    KWallMode mode;
} KWallConfig;

const char *k_wall_mode_name(KWallMode m);                         /* "cover", "fit", ... */
bool        k_wall_mode_parse(const char *s, KWallMode *m);        /* false for an unknown word */

/* Reads the setting (user file first, then the system-wide one). False: no picture is configured. */
bool        k_wallpaper_config(KWallConfig *cfg);
/* Writes ~/.config/hakurei/wallpaper (creating the directories). path == NULL removes the setting. Returns 0 or -errno. */
int         k_wallpaper_set(const char *path, KWallMode mode);
/* A number that changes whenever the setting or the picture file it names changes (modification time and size); 0 when
 * nothing is configured. A desktop calls this once a second and re-renders when it differs from the last value. */
uint64_t    k_wallpaper_stamp(void);
/* Draws the wallpaper into a pixel buffer of the screen's size: the configured picture (decoded, scaled once to the
 * screen, the original dropped) or, when there is none or it cannot be read, the built-in night scene. */
void        k_wallpaper_render(uint32_t *px, int w, int h, int stride);

/* ~/Pictures (created on demand); false when it cannot be made. */
bool        k_pictures_dir(char *buf, size_t n);
/* A fresh name like ~/Pictures/Screenshot-2026-10-23-142530.png (-2, -3 ... when that exists); falls back to /tmp. */
void        k_screenshot_path(char *buf, size_t n);

/* Thumbnails for file browsers. Returns true with `out` set to a canvas that holds the picture scaled to fit inside
 * size x size (never enlarged), transparency flattened onto the paper colour. The pixels belong to a small cache (32 entries,
 * least recently used goes first, keyed by path, size and the file's modification time and size): draw them at once and do
 * not keep the pointer. False means the file is not a picture this system can read (or is too big to be worth decoding
 * for a thumbnail: more than 8 MiB or 24 megapixels); the failure is remembered, so a broken file costs nothing the second time. */
bool        k_image_thumbnail(const char *path, int size, KCanvas *out);
/* Same, but only looks in the cache: never reads a file. Use it while painting, and fill the cache between frames. */
bool        k_image_thumbnail_cached(const char *path, int size, KCanvas *out);
void        k_image_thumbnail_flush(void);

#endif
