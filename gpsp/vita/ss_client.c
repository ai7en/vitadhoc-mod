/* ss_client.c -- see ss_client.h. */
#include "ss_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ra_http.h"

#define SS_API   "https://api.screenscraper.fr/api2/jeuInfos.php"
#define SS_SOFT  "RetroAdhoc"
/* ScreenScraper's system ids, by fe_console_t: GBA 12, GB 9, GBC 10,
 * Mega Drive 1, NES 3, SNES 4. */
static const int SS_SYSTEM[6] = { 12, 9, 10, 1, 3, 4 };

enum { ST_IDLE = 0, ST_HASH, ST_INFO, ST_NEXT, ST_DL };
enum { K_BOX = 0, K_TITLE, K_VIDEO, K_N };

static struct
{
   char devid[48], devpass[48], user[48], pass[48];
   int  state, console, got, result;      /* result: 0 none, else SS_DONE/FAILED, once */
   int  kind;                              /* the one being fetched */
   char root[512], path[600], name[256], stem[256], md5[40];
   char url[K_N][700], fmt[K_N][8];
   char note[160];
   char *json;                             /* the record, kept for the picks */
} S;

void ss_config(const char *devid, const char *devpass, const char *user,
               const char *pass)
{
   snprintf(S.devid, sizeof(S.devid), "%s", devid ? devid : "");
   snprintf(S.devpass, sizeof(S.devpass), "%s", devpass ? devpass : "");
   snprintf(S.user, sizeof(S.user), "%s", user ? user : "");
   snprintf(S.pass, sizeof(S.pass), "%s", pass ? pass : "");
}

int ss_configured(void) { return S.devid[0] && S.devpass[0]; }
int ss_busy(void) { return S.state != ST_IDLE; }

void ss_urlenc(const char *s, char *out, size_t cap)
{
   static const char hex[] = "0123456789ABCDEF";
   size_t n = 0;
   for (; *s && n + 4 < cap; s++)
   {
      unsigned char c = (unsigned char)*s;
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
         out[n++] = (char)c;
      else
      {
         out[n++] = '%';
         out[n++] = hex[c >> 4];
         out[n++] = hex[c & 15];
      }
   }
   out[n] = '\0';
}

/* ---- a small, forgiving JSON reader: strings of flat objects only -------------- */

/* The string value of "key" inside [a, b); 1 and unescaped into out. */
static int jstr(const char *a, const char *b, const char *key, char *out,
                size_t cap)
{
   char pat[40];
   const char *p = a;
   size_t n = 0;
   snprintf(pat, sizeof(pat), "\"%s\"", key);
   for (;;)
   {
      p = strstr(p, pat);
      if (!p || p >= b)
         return 0;
      p += strlen(pat);
      while (p < b && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
         p++;
      if (p < b && *p == ':')
         break;
   }
   p++;
   while (p < b && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
      p++;
   if (p >= b || *p != '"')
      return 0;
   for (p++; p < b && *p != '"' && n + 1 < cap; p++)
   {
      if (*p == '\\' && p + 1 < b)
      {
         p++;
         if (*p == 'u' && p + 4 < b)
         {
            unsigned v = 0;
            int k;
            for (k = 1; k <= 4; k++)
               v = v * 16 + (unsigned)(isdigit((unsigned char)p[k])
                    ? p[k] - '0' : (tolower((unsigned char)p[k]) - 'a' + 10));
            p += 4;
            out[n++] = v < 128 ? (char)v : '?';
         }
         else
            out[n++] = *p == 'n' ? '\n' : *p;      /* \/ \\ \" */
      }
      else
         out[n++] = *p;
   }
   out[n] = '\0';
   return 1;
}

int ss_pick_media(const char *json, const char *type, char *url, size_t ucap,
                  char *fmt, size_t fcap)
{
   static const char *const REG[] = { "wor", "us", "eu", "ss", "jp", NULL };
   const char *m = strstr(json, "\"medias\"");
   int best = 99;
   if (!m)
      return 0;
   m = strchr(m, '[');
   if (!m)
      return 0;
   url[0] = fmt[0] = '\0';
   for (m++; *m && *m != ']';)
   {
      const char *o = strchr(m, '{'), *e;
      char t[40], r[16], u[700], f[8];
      int rank, i;
      if (!o)
         break;
      e = strchr(o, '}');
      if (!e)
         break;
      m = e + 1;
      if (!jstr(o, e, "type", t, sizeof(t)) || strcmp(t, type) ||
          !jstr(o, e, "url", u, sizeof(u)))
         continue;
      if (!jstr(o, e, "region", r, sizeof(r)))
         r[0] = '\0';
      if (!jstr(o, e, "format", f, sizeof(f)))
         snprintf(f, sizeof(f), "%s", strcmp(type, "video") ? "png" : "mp4");
      rank = 50;
      for (i = 0; REG[i]; i++)
         if (!strcmp(r, REG[i]))
         {
            rank = i;
            break;
         }
      if (rank < best)
      {
         best = rank;
         snprintf(url, ucap, "%s", u);
         snprintf(fmt, fcap, "%s", f);
      }
   }
   return url[0] != '\0';
}

/* ---- the job -------------------------------------------------------------------- */

static void finish(int result, const char *note)
{
   free(S.json);
   S.json = NULL;
   S.state = ST_IDLE;
   S.result = result;
   snprintf(S.note, sizeof(S.note), "%s", note);
}

int ss_start(const char *root, const char *path, const char *name,
             int console)
{
   const char *dot;
   if (S.state != ST_IDLE || !root || !path || !name || console < 0 ||
       console > 5 || !ss_configured())
      return -1;
   memset(S.url, 0, sizeof(S.url));
   memset(S.fmt, 0, sizeof(S.fmt));
   S.got = 0;
   S.result = 0;
   S.console = console;
   snprintf(S.root, sizeof(S.root), "%s", root);
   snprintf(S.path, sizeof(S.path), "%s", path);
   snprintf(S.name, sizeof(S.name), "%s", name);
   snprintf(S.stem, sizeof(S.stem), "%s", name);
   dot = strrchr(S.stem, '.');
   if (dot)
      S.stem[dot - S.stem] = '\0';
   S.md5[0] = '\0';
   if (ra_http_hash_file(path) != 0)
      return -1;
   S.state = ST_HASH;
   snprintf(S.note, sizeof(S.note), "Looking the game up...");
   return 0;
}

static const char *const DIR[K_N] = { "boxart", "titles", "videos" };

static int submit_info(void)
{
   char nm[600], url[1400], dev[120], usr[200];
   char *p;
   ss_urlenc(S.name, nm, sizeof(nm));
   ss_urlenc(S.devid, dev, sizeof(dev));
   snprintf(url, sizeof(url),
            "%s?devid=%s&devpassword=", SS_API, dev);
   p = url + strlen(url);
   ss_urlenc(S.devpass, p, sizeof(url) - (size_t)(p - url));
   p = url + strlen(url);
   snprintf(p, sizeof(url) - (size_t)(p - url),
            "&softname=%s&output=json&systemeid=%d&romnom=%s", SS_SOFT,
            SS_SYSTEM[S.console], nm);
   if (S.md5[0])
   {
      p = url + strlen(url);
      snprintf(p, sizeof(url) - (size_t)(p - url), "&md5=%s", S.md5);
   }
   if (S.user[0])
   {
      p = url + strlen(url);
      ss_urlenc(S.user, usr, sizeof(usr));
      snprintf(p, sizeof(url) - (size_t)(p - url), "&ssid=%s&sspassword=", usr);
      p = url + strlen(url);
      ss_urlenc(S.pass, p, sizeof(url) - (size_t)(p - url));
   }
   return ra_http_submit(url, NULL, NULL);
}

/* The next wanted file after S.kind, or the end. */
static void next_download(void)
{
   static const char *const LABEL[K_N] = { "cover", "title screen", "video" };
   while (++S.kind < K_N)
   {
      char path[1100], dir[800];
      if (!S.url[S.kind][0])
         continue;
      snprintf(dir, sizeof(dir), "%s/%s", S.root, DIR[S.kind]);
      mkdir(dir, 0777);
      snprintf(path, sizeof(path), "%s/%s.%s", dir, S.stem, S.fmt[S.kind]);
      if (S.kind == K_BOX)
      {
         /* the old cover and its tile thumbnail go: the new one is made */
         char old[1100];
         snprintf(old, sizeof(old), "%s/%s.png", dir, S.stem);
         remove(old);
         snprintf(old, sizeof(old), "%s/%s.jpg", dir, S.stem);
         remove(old);
         snprintf(old, sizeof(old), "%s/%s.t2.565", dir, S.stem);
         remove(old);
      }
      if (ra_http_download(S.url[S.kind], path) == 0)
      {
         S.state = ST_DL;
         snprintf(S.note, sizeof(S.note), "Downloading the %s...",
                  LABEL[S.kind]);
         return;
      }
   }
   if (!S.got)
      finish(SS_FAILED, "ScreenScraper has nothing for this game");
   else
      finish(SS_DONE, (S.got & 4) ? "Scraped: cover, title and video"
                                  : "Scraped from ScreenScraper");
}

int ss_pump(char *msg, size_t cap, int *got)
{
   ra_http_res r;
   int rc = SS_WORKING;
   if (got)
      *got = S.got;
   if (S.result)
   {
      rc = S.result;
      S.result = 0;
      snprintf(msg, cap, "%s", S.note);
      return rc;
   }
   if (S.state == ST_IDLE)
   {
      snprintf(msg, cap, "%s", S.note);
      return SS_IDLE;
   }
   if (ra_http_take(&r))
   {
      if (S.state == ST_HASH)
      {
         /* status 0 and the 32 hex digits in `note`, or -1: unreadable file */
         if (r.status == 0 && strlen(r.note) == 32)
            snprintf(S.md5, sizeof(S.md5), "%s", r.note);
         free(r.body);
         if (submit_info() != 0)
            finish(SS_FAILED, "Network busy, try again");
         else
            S.state = ST_INFO;
      }
      else if (S.state == ST_INFO)
      {
         if (r.status == 200 && r.body && strstr(r.body, "\"medias\""))
         {
            static const char *const T[K_N][2] = {
               { "box-2D", NULL }, { "sstitle", NULL },
               { "video-normalized", "video" } };
            int k, i;
            for (k = 0; k < K_N; k++)
               for (i = 0; i < 2 && T[k][i] && !S.url[k][0]; i++)
                  ss_pick_media(r.body, T[k][i], S.url[k], sizeof(S.url[k]),
                                S.fmt[k], sizeof(S.fmt[k]));
            free(r.body);
            S.kind = -1;
            next_download();
         }
         else
         {
            const char *why =
               r.status == RA_HTTP_RETRY ? "No connection to ScreenScraper"
             : r.status == 404 ? "ScreenScraper does not know this game"
             : r.status == 401 ? "ScreenScraper API is closed to this account"
             : r.status == 403 ? "ScreenScraper refused the login"
             : r.status == 423 ? "ScreenScraper is offline"
             : r.status == 426 ? "ScreenScraper blocked this version"
             : r.status == 429 ? "ScreenScraper: too many requests"
             : r.status == 430 ? "ScreenScraper: daily limit reached"
             : r.status == 431 ? "ScreenScraper: unknown-game limit reached"
             : r.status == 200 ? "ScreenScraper has no pictures for this game"
             : "ScreenScraper answered an error";
            if (r.body && strstr(r.body, "Erreur de login"))
               why = "ScreenScraper refused the login";
            free(r.body);
            finish(SS_FAILED, why);
         }
      }
      else if (S.state == ST_DL)
      {
         if (r.status == 200)
            S.got |= 1 << S.kind;
         free(r.body);
         next_download();
      }
      else
         free(r.body);
   }
   if (S.result)
   {
      rc = S.result;
      S.result = 0;
   }
   else if (S.state == ST_IDLE)
      rc = SS_IDLE;
   if (got)
      *got = S.got;
   snprintf(msg, cap, "%s", S.note);
   return rc;
}
