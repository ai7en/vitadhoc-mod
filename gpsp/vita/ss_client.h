/* ss_client.h -- ScreenScraper (https://www.screenscraper.fr) for ONE game.
 *
 * The game list's covers come from the libretro thumbnail server by
 * themselves (artfetch.h).  This is the manual way for a game those do not
 * have, or for what they never do: the cartridge's picture, its title
 * screen, and its video preview, fetched from ScreenScraper's API v2 and
 * written where the list already looks:
 *
 *    boxart/<name>.png|jpg    "box-2D"          (replaces the libretro one)
 *    titles/<name>.png|jpg    "sstitle"
 *    videos/<name>.mp4        "video-normalized", else "video"
 *
 * One request for the game's record (jeuInfos.php: the file's MD5 and name
 * identify it), then one download per picture.  ScreenScraper needs a
 * developer login (devid/devpassword, issued by its forum to a software)
 * and, for more than a trickle, the user's own account:
 *
 *    vita.ini   ss_devid = ...   ss_devpass = ...
 *               ss_user  = ...   ss_pass    = ...      (the user's login)
 *
 * Driven from the UI thread, a frame at a time: ss_start, then ss_pump
 * until it stops answering "working".  The HTTP is ra_http.c's thread. */
#ifndef GBADHOC_SS_CLIENT_H
#define GBADHOC_SS_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void ss_config(const char *devid, const char *devpass, const char *user,
               const char *pass);
/* 1 when a developer login is set (without it the server refuses). */
int  ss_configured(void);
/* Begin one game.  `root` = the data folder, `path` = the ROM's whole path,
 * `name` = its file name with extension, `console` 0..5 (fe_console_t:
 * GBA, GB, GBC, MD, NES, SNES).  0 = started; -1 = busy / no login / bad
 * arguments (ss_pump says why). */
int  ss_start(const char *root, const char *path, const char *name,
              int console);
int  ss_busy(void);
enum { SS_IDLE = 0, SS_WORKING = 1, SS_DONE = 2, SS_FAILED = -1 };
/* One step.  Returns SS_*; `msg` always holds a line fit for a toast.  SS_DONE
 * and SS_FAILED are each returned once, then SS_IDLE.  `got` (may be NULL)
 * gets a bit set per thing written: 1 cover, 2 title, 4 video. */
int  ss_pump(char *msg, size_t cap, int *got);

/* Pure helpers, public for the tests. */
/* The media of `json` that is wanted: copies the url of the best entry of
 * `type` ("box-2D") into url, its format into fmt; 0 = none.  Regions are
 * tried in the order wor, us, eu, ss, jp, then any. */
int  ss_pick_media(const char *json, const char *type, char *url,
                   size_t ucap, char *fmt, size_t fcap);
/* Percent-encoding of s (RFC 3986 unreserved kept). */
void ss_urlenc(const char *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
