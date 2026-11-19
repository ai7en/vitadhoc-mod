/* ra_vita.c -- see ra_vita.h. */
#include "ra_vita.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "rc_client.h"
#include "rc_api_runtime.h"
#include "rc_consoles.h"

#include "../frontend-common/fe_evt.h"
#include "../frontend-common/fe_host.h"
#include "../psp/osd_psp.h"
#include "cheats_vita.h"
#include "plat.h"
#include "ra_http.h"

#define RA_FILE   "retroachievements.txt"
#define RA_OFF_FILE "retroachievements-offline.txt"
#define RA_QUEUE  "ra_queue.txt"
#define RA_CACHE  "ra_cache"
#define DAY_S     (24L * 3600L)
#define KEYCAP    96

enum { JOB_NONE = 0, JOB_CALL, JOB_HASH };

/* ---- the server's calls, one at a time ------------------------------------ */

typedef struct ra_call
{
   char *url, *post, *type;
   char  api[24];                  /* the post's r= (for the log) */
   char  key[KEYCAP];              /* offline mode: the cache entry it fills */
   unsigned game;                  /* a start-session's game (unlock overlay) */
   char *fake;                     /* answered here, not by the server */
   int   fake_status;
   rc_client_server_callback_t cb;
   void *cb_data;
   int   sent;
   struct ra_call *next;
} ra_call;

static struct
{
   rc_client_t *rc;
   ra_call *head, *tail;
   char path[600];                 /* the account file */
   char user[64], token[96];       /* as kept on the card */
   int  hardcore;                  /* the setting */
   int  login_busy;
   char login_name[64];
   int  game_on;                   /* between ra_game_begin and _end */
   int  game_hardcore;             /* the setting as the game started */
   int  game_state;                /* 0 loading, 1 has achievements, -1 none */
   char game_title[64];
   int  framed;                    /* a frame was emulated since ra_pump */
   int  inflight;                  /* the net thread's job: 0, CALL, HASH */
   int  hash_want;                 /* the game's file is to be hashed */
   unsigned game_gen, hash_gen;    /* which game a hash answer is for */
   char hash_path[600];
   int  was_linked;
   int  stale;                     /* progress must start over */
   ra_ach *list;
   int  list_n;
   int  (*online)(void);
   int  retry_login;               /* the kept token's sign-in failed on the
                                    * way (offline): try again */
   unsigned retry_wait;            /* pumps until the next try */
   int  retry_told;
   /* offline mode */
   int  offline;                   /* the setting (default on) */
   long last_sync;                 /* when the queue last reached the server */
   char set_path[600], queue_path[600], cache_dir[600], host[96];
   char game_hash[40];
   int  queued;                    /* unlocks waiting for the server */
   int  sync_on, sync_left;
   unsigned sync_wait;
   struct qent *sq;
   char *sq_ok;
   int  sq_n;
} R;

typedef struct qent
{
   unsigned game, ach, hard;
   long when;
   char hash[36], user[48];
} qent;

#define RETRY_PUMPS (60u * 30u)     /* about half a minute of refreshes */

static void toast(const char *fmt, const char *a)
{
   osd_toast(fmt, a ? a : "");
}

/* What the post asks for: r=<api>. */
static void api_of(const char *post, char *out, size_t cap)
{
   const char *r = post ? strstr(post, "r=") : NULL;
   size_t n = 0;
   if (r && (r == post || r[-1] == '&'))
      for (r += 2; r[n] && r[n] != '&' && n + 1 < cap; n++)
         out[n] = r[n];
   out[n] = '\0';
   if (!n)
      snprintf(out, cap, "%s", post ? "?" : "get");
}

/* A post may carry the password (r=login2): not left behind in the heap. */
static void wipe_str(char *p)
{
   volatile char *v = p;
   while (v && *v)
      *v++ = 0;
}


static void call_free(ra_call *c);

/* ---- offline mode ------------------------------------------------------------
 * The server's answers a game needs (sign-in, its hash, its achievements, the
 * account's unlocks) are kept in <data>/ra_cache/ and used for 24 hours; an
 * unlock is answered here at once and written to <data>/ra_queue.txt. The queue
 * is sent when 24 hours have passed since the last time (and Wi-Fi is there).
 * With the setting off the server answers everything, as before, and a
 * waiting queue is sent as soon as the network is there. */

static void off_save(void)
{
   FILE *f = fopen(R.set_path, "w");
   if (!f)
      return;
   fprintf(f, "# RetroAchievements offline mode.\noffline=%d\nlast_sync=%ld\n",
           R.offline, R.last_sync);
   fclose(f);
}

static void off_load(void)
{
   FILE *f = fopen(R.set_path, "r");
   char line[96];
   R.offline = 1;
   R.last_sync = 0;
   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      if (!strncmp(line, "offline=", 8))
         R.offline = atoi(line + 8) != 0;
      else if (!strncmp(line, "last_sync=", 10))
         R.last_sync = atol(line + 10);
   }
   fclose(f);
}

static int param(const char *post, const char *name, char *out, size_t cap)
{
   size_t n = strlen(name), i = 0;
   const char *p = post;
   out[0] = '\0';
   while (p && *p)
   {
      if (!strncmp(p, name, n) && p[n] == '=')
      {
         for (p += n + 1; *p && *p != '&' && i + 1 < cap; p++)
            out[i++] = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                       (*p >= 'A' && *p <= 'Z') || *p == '-' ? *p : '_';
         out[i] = '\0';
         return 1;
      }
      p = strchr(p, '&');
      if (p)
         p++;
   }
   return 0;
}

static char *cache_read(const char *key, size_t *len, long *when)
{
   char path[700], line[48];
   FILE *f;
   long n;
   char *b;
   snprintf(path, sizeof(path), "%s/%s.txt", R.cache_dir, key);
   f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (!fgets(line, sizeof(line), f) || strncmp(line, "T ", 2))
   {
      fclose(f);
      return NULL;
   }
   *when = atol(line + 2);
   n = ftell(f);
   fseek(f, 0, SEEK_END);
   *len = (size_t)(ftell(f) - n);
   fseek(f, n, SEEK_SET);
   b = (char *)malloc(*len + 1);
   if (b && fread(b, 1, *len, f) != *len)
   {
      free(b);
      b = NULL;
   }
   fclose(f);
   if (b)
      b[*len] = '\0';
   return b;
}

static void cache_write(const char *key, const char *body, size_t len)
{
   char path[700], tmp[710];
   FILE *f;
   mkdir(R.cache_dir, 0777);
   snprintf(path, sizeof(path), "%s/%s.txt", R.cache_dir, key);
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   f = fopen(tmp, "wb");
   if (!f)
      return;
   fprintf(f, "T %ld\n", (long)time(NULL));
   if (fwrite(body, 1, len, f) != len || fclose(f) != 0)
   {
      remove(tmp);
      return;
   }
   remove(path);
   if (rename(tmp, path) != 0)
      remove(tmp);
}

static qent *queue_read(int *n)
{
   FILE *f = fopen(R.queue_path, "r");
   qent *q = NULL, e;
   int cnt = 0, cap = 0;
   *n = 0;
   if (!f)
      return NULL;
   while (fscanf(f, "%u %u %u %ld %35s %47s", &e.game, &e.ach, &e.hard,
                 &e.when, e.hash, e.user) == 6)
   {
      if (cnt == cap)
      {
         qent *g = (qent *)realloc(q, (size_t)(cap = cap ? cap * 2 : 16) *
                                         sizeof(*q));
         if (!g)
            break;
         q = g;
      }
      q[cnt++] = e;
   }
   fclose(f);
   *n = cnt;
   return q;
}

static void queue_write(const qent *q, int n)
{
   int i;
   if (n <= 0)
   {
      remove(R.queue_path);
      R.queued = 0;
      return;
   }
   {
      FILE *f = fopen(R.queue_path, "w");
      if (!f)
         return;
      for (i = 0; i < n; i++)
         fprintf(f, "%u %u %u %ld %s %s\n", q[i].game, q[i].ach, q[i].hard,
                 q[i].when, q[i].hash, q[i].user);
      fclose(f);
   }
   R.queued = n;
}

static void queue_add(const qent *e)
{
   FILE *f = fopen(R.queue_path, "a");
   if (!f)
      return;
   fprintf(f, "%u %u %u %ld %s %s\n", e->game, e->ach, e->hard, e->when,
           e->hash, e->user);
   fclose(f);
   R.queued++;
}

/* The queued unlocks of a game, put into a start-session answer. */
static char *insert_after(char *s, const char *marker, const char *ins)
{
   char *p = strstr(s, marker), *o;
   size_t a, il = strlen(ins), sl = strlen(s);
   int empty;
   if (!p || !il)
      return s;
   p += strlen(marker);
   a = (size_t)(p - s);
   empty = *p == ']';
   o = (char *)malloc(sl + il + 2);
   if (!o)
      return s;
   memcpy(o, s, a);
   memcpy(o + a, ins, il);
   o[a + il] = empty ? '\0' : ',';
   memcpy(o + a + il + (empty ? 0 : 1), s + a, sl - a + 1);
   free(s);
   return o;
}

static char *overlay(char *body, unsigned game)
{
   int n, i;
   qent *q = queue_read(&n);
   char soft[512], hard[512];
   size_t ls = 0, lh = 0;
   soft[0] = hard[0] = '\0';
   for (i = 0; i < n; i++)
   {
      char e[64];
      int l;
      if (q[i].game != game)
         continue;
      l = snprintf(e, sizeof(e), "{\"ID\":%u,\"When\":%ld}", q[i].ach,
                   q[i].when);
      if (q[i].hard && lh + l + 2 < sizeof(hard))
         lh += snprintf(hard + lh, sizeof(hard) - lh, "%s%s", lh ? "," : "", e);
      else if (!q[i].hard && ls + l + 2 < sizeof(soft))
         ls += snprintf(soft + ls, sizeof(soft) - ls, "%s%s", ls ? "," : "", e);
   }
   free(q);
   body = insert_after(body, "\"Unlocks\":[", soft);
   return insert_after(body, "\"HardcoreUnlocks\":[", hard);
}

static int online_now(void)
{
   return !R.online || R.online();
}

/* Offline mode's answer to a request: 1 if it is answered here (c->fake) or
 * is to fill the cache (c->key); 0 for the server as it is. */
static int offline_plan(ra_call *c)
{
   char api[24], a[48], b[48];
   const char *post = c->post;
   if (!R.offline || !post)
      return 0;
   api_of(post, api, sizeof(api));
   if (!strcmp(api, "ping") || !strcmp(api, "submitlboard"))
   {
      c->fake = strdup(!strcmp(api, "ping")
         ? "{\"Success\":true}"
         : "{\"Success\":true,\"Response\":{\"Score\":0,\"BestScore\":0,"
           "\"RankInfo\":{\"Rank\":0,\"NumEntries\":0},\"TopEntries\":[]}}");
      c->fake_status = 200;
      return c->fake != NULL;
   }
   if (!strcmp(api, "awardachievement"))
   {
      char body[160];
      qent e;
      const rc_client_game_t *g = R.rc ? rc_client_get_game_info(R.rc) : NULL;
      const rc_client_user_t *u = R.rc ? rc_client_get_user_info(R.rc) : NULL;
      memset(&e, 0, sizeof(e));
      param(post, "a", a, sizeof(a));
      param(post, "h", b, sizeof(b));
      e.ach = (unsigned)atoi(a);
      e.hard = (unsigned)atoi(b);
      e.game = g ? g->id : 0;
      e.when = (long)time(NULL);
      snprintf(e.hash, sizeof(e.hash), "%s", R.game_hash[0] ? R.game_hash : "-");
      snprintf(e.user, sizeof(e.user), "%s", u ? u->username : R.user);
      queue_add(&e);
      fe_evt("ra_queued ach=%u hardcore=%u waiting=%d", e.ach, e.hard,
             R.queued);
      snprintf(body, sizeof(body),
               "{\"Success\":true,\"Score\":0,\"SoftcoreScore\":0,"
               "\"AchievementID\":%u}", e.ach);
      c->fake = strdup(body);
      c->fake_status = 200;
      return c->fake != NULL;
   }
   if (!strcmp(api, "login2"))
   {
      param(post, "u", a, sizeof(a));
      snprintf(c->key, KEYCAP, "login-%s", a);
      if (param(post, "p", b, sizeof(b)))
         return 0;                 /* a password sign-in asks the server */
   }
   else if (!strcmp(api, "gameid"))
   {
      param(post, "m", a, sizeof(a));
      snprintf(c->key, KEYCAP, "gameid-%s", a);
   }
   else if (!strcmp(api, "patch"))
   {
      if (!param(post, "g", a, sizeof(a)))
         param(post, "m", a, sizeof(a));
      snprintf(c->key, KEYCAP, "patch-%s", a);
   }
   else if (!strcmp(api, "startsession"))
   {
      param(post, "g", a, sizeof(a));
      param(post, "h", b, sizeof(b));
      snprintf(c->key, KEYCAP, "session-%s-%s", a, b);
      c->game = (unsigned)atoi(a);
   }
   else
      return 0;
   {
      size_t len = 0;
      long when = 0;
      char *body = cache_read(c->key, &len, &when);
      long now = (long)time(NULL);
      int fresh = body && now >= when && now - when < DAY_S;
      if (body && (fresh || !online_now()))
      {
         c->fake = c->game ? overlay(body, c->game) : body;
         c->fake_status = 200;
         fe_evt("ra_cache hit api=%s key=%s fresh=%d", api, c->key, fresh);
         return 1;
      }
      free(body);
      if (!online_now())
      {
         /* nothing kept and no network: a session starts empty (the queue's
          * unlocks added), the rest is not there */
         if (c->game)
         {
            c->fake = overlay(strdup("{\"Success\":true,\"Unlocks\":[],"
                                     "\"HardcoreUnlocks\":[]}"), c->game);
            c->fake_status = 200;
         }
         else
         {
            c->fake = strdup("");
            c->fake_status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
         }
         fe_evt("ra_cache miss api=%s key=%s", api, c->key);
         return c->fake != NULL;
      }
   }
   return 0;                       /* the server answers, the cache keeps it */
}

static void RC_CCONV server_call(const rc_api_request_t *req,
                                 rc_client_server_callback_t cb, void *data,
                                 rc_client_t *client)
{
   ra_call *c = (ra_call *)calloc(1, sizeof(*c));
   (void)client;
   if (c)
   {
      c->url = strdup(req->url);
      c->post = req->post_data ? strdup(req->post_data) : NULL;
      c->type = req->content_type ? strdup(req->content_type) : NULL;
   }
   if (!c || !c->url || (req->post_data && !c->post) ||
       (req->content_type && !c->type))
   {
      rc_api_server_response_t r;
      if (c)
      {
         free(c->url);
         wipe_str(c->post);
         free(c->post);
         free(c->type);
         free(c);
      }
      memset(&r, 0, sizeof(r));
      r.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
      r.body = "";
      cb(&r, data);
      return;
   }
   api_of(c->post, c->api, sizeof(c->api));
   c->cb = cb;
   c->cb_data = data;
   offline_plan(c);
   if (R.tail)
      R.tail->next = c;
   else
      R.head = c;
   R.tail = c;
}

static void call_free(ra_call *c)
{
   free(c->fake);
   free(c->url);
   wipe_str(c->post);
   free(c->post);
   free(c->type);
   free(c);
}

/* ---- memory ----------------------------------------------------------------- */

static uint32_t RC_CCONV read_memory(uint32_t addr, uint8_t *buf, uint32_t n,
                                     rc_client_t *client)
{
   (void)client;
   return fe_host_ra_read(addr, buf, n);
}

/* ---- the account file ------------------------------------------------------- */

static void account_load(void)
{
   FILE *f;
   char line[256];
   {
      /* a save cut short between its two renames */
      char old[sizeof(R.path) + 8];
      snprintf(old, sizeof(old), "%s.old", R.path);
      f = fopen(R.path, "r");
      if (!f)
         rename(old, R.path);
      else
         fclose(f);
   }
   f = fopen(R.path, "r");
   R.user[0] = R.token[0] = '\0';
   R.hardcore = 0;
   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      char *v = strchr(line, '='), *e;
      if (!v)
         continue;
      *v++ = '\0';
      for (e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r');)
         *--e = '\0';
      if (!strcmp(line, "user"))
         snprintf(R.user, sizeof(R.user), "%s", v);
      else if (!strcmp(line, "token"))
         snprintf(R.token, sizeof(R.token), "%s", v);
      else if (!strcmp(line, "hardcore"))
         R.hardcore = atoi(v) != 0;
   }
   fclose(f);
}

static int account_save(void)
{
   char tmp[sizeof(R.path) + 8];
   FILE *f;
   snprintf(tmp, sizeof(tmp), "%s.tmp", R.path);
   f = fopen(tmp, "w");
   if (!f)
      return -1;
   fprintf(f, "# RetroAchievements: the account this console signs in with.\n"
              "# The token is the server's, not the password (which is never "
              "kept).\n");
   if (R.user[0] && R.token[0])
      fprintf(f, "user=%s\ntoken=%s\n", R.user, R.token);
   fprintf(f, "hardcore=%d\n", R.hardcore);
   if (fclose(f) != 0)
   {
      remove(tmp);
      return -1;
   }
   {
      /* the old one aside, the new one in, the old one gone: a rename the
       * card refuses leaves the old file (account_load takes it back) */
      char old[sizeof(R.path) + 8];
      FILE *e = fopen(R.path, "r");
      int had = e != NULL;
      if (e)
         fclose(e);
      snprintf(old, sizeof(old), "%s.old", R.path);
      remove(old);
      if (had && rename(R.path, old) != 0)
      {
         remove(tmp);
         return -1;
      }
      if (rename(tmp, R.path) != 0)
      {
         if (had)
            rename(old, R.path);
         remove(tmp);
         return -1;
      }
      remove(old);
   }
   return 0;
}

/* ---- events ----------------------------------------------------------------- */

static void RC_CCONV on_event(const rc_client_event_t *e, rc_client_t *client)
{
   char t[96];
   (void)client;
   switch (e->type)
   {
   case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      fe_evt("ra_unlock id=%u points=%u title=\"%s\"", e->achievement->id,
             e->achievement->points, e->achievement->title);
      snprintf(t, sizeof(t), "%s (%u)", e->achievement->title,
               e->achievement->points);
      toast("Achievement: %s", t);
      if (R.list)
         ra_list_build();           /* the page shows it at once */
      break;
   case RC_CLIENT_EVENT_GAME_COMPLETED:
      fe_evt("ra_completed hardcore=%d", R.game_hardcore);
      toast(R.game_hardcore ? "Mastered: %s" : "Completed: %s", R.game_title);
      break;
   case RC_CLIENT_EVENT_SUBSET_COMPLETED:
      toast("Set completed: %s", e->subset ? e->subset->title : "");
      break;
   case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
      fe_evt("ra_lboard start id=%u", e->leaderboard->id);
      toast("Leaderboard attempt: %s", e->leaderboard->title);
      break;
   case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
      fe_evt("ra_lboard failed id=%u", e->leaderboard->id);
      toast("Leaderboard attempt failed: %s", e->leaderboard->title);
      break;
   case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
      fe_evt("ra_lboard submit id=%u value=%s", e->leaderboard->id,
             e->leaderboard->tracker_value ? e->leaderboard->tracker_value
                                           : "");
      snprintf(t, sizeof(t), "%s %s", e->leaderboard->title,
               e->leaderboard->tracker_value ? e->leaderboard->tracker_value
                                             : "");
      toast("Submitted: %s", t);
      break;
   case RC_CLIENT_EVENT_SERVER_ERROR:
      fe_evt("ra_server_error api=%s rc=%d msg=\"%s\"",
             e->server_error->api ? e->server_error->api : "",
             e->server_error->result,
             e->server_error->error_message ? e->server_error->error_message
                                            : "");
      toast("RetroAchievements: %s", e->server_error->error_message);
      break;
   case RC_CLIENT_EVENT_DISCONNECTED:
      fe_evt("ra_disconnected");
      toast("%s", "Achievements will be sent when back online");
      break;
   case RC_CLIENT_EVENT_RECONNECTED:
      fe_evt("ra_reconnected");
      toast("%s", "Achievements sent");
      break;
   case RC_CLIENT_EVENT_RESET:
      /* Only asked for when Hardcore is switched on mid-game, which this
       * app does not do (the setting applies from the next game). */
      fe_evt("ra_reset_asked");
      break;
   default:
      break;
   }
}

static void RC_CCONV on_log(const char *msg, const rc_client_t *client)
{
   (void)client;
   fe_evt("ra_log %s", msg);
}

/* ---- boot ------------------------------------------------------------------- */

static void RC_CCONV login_done(int result, const char *error,
                                rc_client_t *client, void *ud);

static void token_login(void)
{
   R.login_busy = 1;
   snprintf(R.login_name, sizeof(R.login_name), "%s", R.user);
   rc_client_begin_login_with_token(R.rc, R.user, R.token, login_done, NULL);
}

static void RC_CCONV login_done(int result, const char *error,
                                rc_client_t *client, void *ud)
{
   const rc_client_user_t *u = rc_client_get_user_info(client);
   int with_password = ud != NULL;
   R.login_busy = 0;
   fe_evt("ra_login ok=%d via=%s rc=%d user=%s", result == RC_OK,
          with_password ? "password" : "token", result,
          u ? u->username : R.login_name);
   if (result == RC_OK && u)
   {
      int changed = strcmp(R.user, u->username) ||
                    strcmp(R.token, u->token ? u->token : "");
      R.retry_login = 0;
      R.retry_told = 0;
      snprintf(R.user, sizeof(R.user), "%s", u->username);
      snprintf(R.token, sizeof(R.token), "%s", u->token ? u->token : "");
      if (!with_password && changed)
         account_save();          /* the server handed out a new token */
      if (with_password)
      {
         if (account_save() != 0)
            toast("%s", "Signed in, but the account could not be kept");
         else
            toast("Signed in to RetroAchievements as %s", u->display_name);
      }
      return;
   }
   if (with_password)
   {
      toast("Sign-in failed: %s", error ? error : "no answer");
      /* the account that was signed in before stays the one */
      if (R.user[0] && R.token[0])
         token_login();
   }
   else if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN ||
            result == RC_ACCESS_DENIED)
   {
      /* The kept token is no longer good: forget it, say so once. */
      R.token[0] = '\0';
      R.retry_login = 0;
      account_save();
      toast("%s", "RetroAchievements: please sign in again");
   }
   else
   {
      /* Not reached (no Wi-Fi yet, a timeout): the token is still good.
       * Tried again every half minute while the network is there
       * (ra_pump), and when a game starts. */
      R.retry_login = 1;
      R.retry_wait = RETRY_PUMPS;
      if (!R.retry_told)
         toast("RetroAchievements: %s",
               error ? error : "could not sign in, will try again");
      R.retry_told = 1;
   }
}

void ra_boot(const char *data, const char *host, const char *version,
             int (*online)(void))
{
   char ua[192], clause[64];
   memset(&R, 0, sizeof(R));
   snprintf(R.path, sizeof(R.path), "%s/%s", data, RA_FILE);
   snprintf(R.set_path, sizeof(R.set_path), "%s/%s", data, RA_OFF_FILE);
   snprintf(R.queue_path, sizeof(R.queue_path), "%s/%s", data, RA_QUEUE);
   snprintf(R.cache_dir, sizeof(R.cache_dir), "%s/%s", data, RA_CACHE);
   snprintf(R.host, sizeof(R.host), "%s", host ? host : "");
   account_load();
   off_load();
   {
      int n;
      qent *q = queue_read(&n);
      free(q);
      R.queued = n;
   }
   R.rc = rc_client_create(read_memory, server_call);
   if (!R.rc)
   {
      fe_evt("ra_boot FAILED");
      return;
   }
   rc_client_enable_logging(R.rc, RC_CLIENT_LOG_LEVEL_WARN, on_log);
   rc_client_set_event_handler(R.rc, on_event);
   if (host && host[0])
      rc_client_set_host(R.rc, host);
   clause[0] = '\0';
   rc_client_get_user_agent_clause(R.rc, clause, sizeof(clause));
   snprintf(ua, sizeof(ua), "RetroAdhoc/%s (PlayStation Vita) %s",
            version ? version : "dev", clause);
   ra_http_config(ua, online);
   R.online = online;
   fe_evt("ra_boot user=%s token=%d hardcore=%d offline=%d queued=%d host=%s",
          R.user[0] ? R.user : "-", R.token[0] != 0, R.hardcore, R.offline,
          R.queued, host && host[0] ? host : "ra");
   if (R.user[0] && R.token[0])
      token_login();
}

void ra_shutdown(void)
{
   int i;
   ra_list_free();
   /* What is queued (an unlock earned a moment ago) gets three seconds to
    * reach the server before the client goes. */
   for (i = 0; R.rc && (R.head || R.inflight) && i < 300; i++)
   {
      ra_pump();
      plat_thread_sleep_us(10000);
   }
   if (R.head || R.inflight)
      fe_evt("ra_shutdown unsent=1");
   if (R.rc)
   {
      rc_client_destroy(R.rc);
      R.rc = NULL;
   }
   while (R.head)
   {
      ra_call *c = R.head;
      R.head = c->next;
      call_free(c);
   }
   R.tail = NULL;
   free(R.sq);
   free(R.sq_ok);
   R.sq = NULL;
   R.sq_ok = NULL;
   ra_http_stop(2000000);
}

static void hash_done(const ra_http_res *res);

/* An answer is in: the cache keeps it (offline mode), a failed call takes the
 * kept one, a start-session gets the queued unlocks. */
static void deliver(ra_call *c, int status, char *body, size_t len)
{
   rc_api_server_response_t r;
   char *own = NULL, *kept = NULL;
   if (c->key[0] && !c->fake)
   {
      int ok = status == 200 && body && strstr(body, "\"Success\":true");
      if (ok)
         cache_write(c->key, body, len);
      else if (status != 200)
      {
         size_t kl = 0;
         long when = 0;
         kept = cache_read(c->key, &kl, &when);
         if (kept)
         {
            fe_evt("ra_cache fallback key=%s status=%d", c->key, status);
            body = kept;
            len = kl;
            status = 200;
         }
      }
   }
   if (c->game && status == 200 && body)
   {
      own = overlay(strdup(body), c->game);
      if (own)
      {
         body = own;
         len = strlen(own);
      }
   }
   r.body = body ? body : "";
   r.body_length = len;
   r.http_status_code = status;
   c->cb(&r, c->cb_data);           /* may queue the next call */
   free(own);
   free(kept);
}

static void sync_tick(void);

void ra_pump(void)
{
   ra_http_res res;
   if (!R.rc)
      return;
   /* answered here: offline mode's, at the next pump (never inside the
    * emulation frame that asked) */
   while (!R.inflight && R.head && R.head->fake)
   {
      ra_call *c = R.head;
      R.head = c->next;
      if (!R.head)
         R.tail = NULL;
      deliver(c, c->fake_status, c->fake, strlen(c->fake));
      call_free(c);
   }
   if (R.inflight && ra_http_take(&res))
   {
      if (R.inflight == JOB_HASH)
         hash_done(&res);
      else if (R.head)
      {
         ra_call *c = R.head;
         R.head = c->next;
         if (!R.head)
            R.tail = NULL;
         fe_evt("ra_http api=%s status=%d len=%u note=%s", c->api,
                res.status, (unsigned)res.len, res.note);
         deliver(c, res.status, res.body, res.len);
         call_free(c);
      }
      free(res.body);
      R.inflight = JOB_NONE;
   }
   if (!R.inflight && ra_http_idle())
   {
      /* A game's hash first: its achievements wait on it. */
      if (R.hash_want)
      {
         if (ra_http_hash_file(R.hash_path) == 0)
         {
            R.hash_want = 0;
            R.hash_gen = R.game_gen;
            R.inflight = JOB_HASH;
         }
      }
      else if (R.head &&
               ra_http_submit(R.head->url, R.head->post, R.head->type) == 0)
      {
         R.head->sent = 1;
         R.inflight = JOB_CALL;
      }
   }
   if (R.retry_login && !R.login_busy && !rc_client_get_user_info(R.rc) &&
       R.token[0] && (R.retry_wait == 0 || --R.retry_wait == 0))
   {
      R.retry_wait = RETRY_PUMPS;
      if (!R.online || R.online())
         token_login();
   }
   sync_tick();
   if (!R.framed)
      rc_client_idle(R.rc);
   R.framed = 0;
}

/* ---- sending the queue --------------------------------------------------------- */

static void RC_CCONV sync_cb(const rc_api_server_response_t *r, void *ud)
{
   int i = (int)(uintptr_t)ud - 1;
   int ok = r->http_status_code == 200 && r->body &&
            (strstr(r->body, "\"Success\":true") ||
             strstr(r->body, "User already has"));
   if (R.sq_ok && i >= 0 && i < R.sq_n && ok)
      R.sq_ok[i] = 1;
   if (--R.sync_left > 0)
      return;
   {
      int i2, sent = 0, keep = 0;
      for (i2 = 0; R.sq && i2 < R.sq_n; i2++)
      {
         if (R.sq_ok[i2])
            sent++;
         else
            R.sq[keep++] = R.sq[i2];
      }
      /* what came in meanwhile stays (the file may have grown) */
      if (R.sq)
      {
         int n;
         qent *now = queue_read(&n);
         if (now && n > R.sq_n)
         {
            qent *all = (qent *)malloc((size_t)(keep + n - R.sq_n) *
                                       sizeof(*all));
            if (all)
            {
               memcpy(all, R.sq, (size_t)keep * sizeof(*all));
               memcpy(all + keep, now + R.sq_n,
                      (size_t)(n - R.sq_n) * sizeof(*all));
               queue_write(all, keep + n - R.sq_n);
               free(all);
               keep = -1;
            }
         }
         free(now);
         if (keep >= 0)
            queue_write(R.sq, keep);
      }
      free(R.sq);
      free(R.sq_ok);
      R.sq = NULL;
      R.sq_ok = NULL;
      R.sq_n = 0;
      R.sync_on = 0;
      fe_evt("ra_sync sent=%d left=%d", sent, R.queued);
      if (sent)
      {
         char t[48];
         R.last_sync = (long)time(NULL);
         off_save();
         snprintf(t, sizeof(t), "%d", sent);
         toast("Achievements sent to the server: %s", t);
      }
      else
         R.sync_wait = RETRY_PUMPS * 10u;   /* about five minutes */
   }
}

static void sync_tick(void)
{
   const rc_client_user_t *u;
   int i;
   long now;
   if (!R.queued || R.sync_on)
      return;
   now = (long)time(NULL);
   /* Offline mode: once a day.  With it off: as soon as the network is
    * there. */
   if (R.offline && R.last_sync && now >= R.last_sync &&
       now - R.last_sync < DAY_S)
      return;
   if (R.sync_wait && --R.sync_wait)
      return;
   u = rc_client_get_user_info(R.rc);
   if (!u || !online_now() || R.head || R.inflight)
      return;
   R.sq = queue_read(&R.sq_n);
   R.sq_ok = R.sq_n ? (char *)calloc((size_t)R.sq_n, 1) : NULL;
   if (!R.sq || !R.sq_ok)
   {
      free(R.sq);
      free(R.sq_ok);
      R.sq = NULL;
      R.sq_ok = NULL;
      R.sq_n = 0;
      R.sync_wait = RETRY_PUMPS;
      return;
   }
   R.sync_on = 1;
   R.sync_left = 0;
   for (i = 0; i < R.sq_n; i++)
   {
      rc_api_award_achievement_request_t p;
      rc_api_host_t h;
      rc_api_request_t req;
      ra_call *c;
      if (strcmp(R.sq[i].user, u->username))
         continue;                 /* another account's: stays */
      memset(&p, 0, sizeof(p));
      p.username = u->username;
      p.api_token = u->token;
      p.achievement_id = R.sq[i].ach;
      p.hardcore = R.sq[i].hard;
      p.game_hash = strcmp(R.sq[i].hash, "-") ? R.sq[i].hash : NULL;
      p.seconds_since_unlock = now > R.sq[i].when
                               ? (uint32_t)(now - R.sq[i].when) : 0;
      h.host = R.host[0] ? R.host : NULL;
      h.media_host = NULL;
      if (rc_api_init_award_achievement_request_hosted(&req, &p, &h) != RC_OK)
         continue;
      c = (ra_call *)calloc(1, sizeof(*c));
      if (c)
      {
         c->url = strdup(req.url);
         c->post = req.post_data ? strdup(req.post_data) : NULL;
         c->type = req.content_type ? strdup(req.content_type) : NULL;
         snprintf(c->api, sizeof(c->api), "sync");
         c->cb = sync_cb;
         c->cb_data = (void *)(uintptr_t)(i + 1);
         if (R.tail)
            R.tail->next = c;
         else
            R.head = c;
         R.tail = c;
         R.sync_left++;
      }
      rc_api_destroy_request(&req);
   }
   if (!R.sync_left)
   {
      free(R.sq);
      free(R.sq_ok);
      R.sq = NULL;
      R.sq_ok = NULL;
      R.sq_n = 0;
      R.sync_on = 0;
      R.sync_wait = RETRY_PUMPS * 10u;
   }
}

int ra_offline_pref(void)
{
   return R.offline;
}

void ra_offline_set(int on)
{
   R.offline = on ? 1 : 0;
   off_save();
   fe_evt("ra_offline_setting on=%d", R.offline);
}

int ra_offline_waiting(void)
{
   return R.queued;
}

/* ---- the account ------------------------------------------------------------ */

int ra_account(char *name, size_t cap)
{
   const rc_client_user_t *u = R.rc ? rc_client_get_user_info(R.rc) : NULL;
   if (u)
   {
      snprintf(name, cap, "%s", u->display_name ? u->display_name
                                                : u->username);
      return RA_ACCT_IN;
   }
   if (R.login_busy || (R.retry_login && R.token[0]))
   {
      snprintf(name, cap, "%s", R.login_busy ? R.login_name : R.user);
      return RA_ACCT_BUSY;
   }
   snprintf(name, cap, "%s", "");
   return RA_ACCT_OUT;
}

int ra_login(const char *user, const char *password)
{
   if (!R.rc || R.login_busy || !user || !user[0] || !password ||
       !password[0])
      return -1;
   if (rc_client_get_user_info(R.rc))
      rc_client_logout(R.rc);
   R.login_busy = 1;
   snprintf(R.login_name, sizeof(R.login_name), "%s", user);
   fe_evt("ra_login asked user=%s", user);
   /* (the userdata only says "with a password": login_done) */
   rc_client_begin_login_with_password(R.rc, user, password, login_done,
                                       (void *)1);
   return 0;
}

void ra_logout(void)
{
   if (!R.rc)
      return;
   rc_client_logout(R.rc);
   R.user[0] = R.token[0] = '\0';
   account_save();
   fe_evt("ra_logout");
}

int ra_hardcore_pref(void)
{
   return R.hardcore;
}

void ra_hardcore_set(int on)
{
   R.hardcore = on ? 1 : 0;
   account_save();
   fe_evt("ra_hardcore_setting on=%d", R.hardcore);
}

/* ---- a game ------------------------------------------------------------------ */

static void RC_CCONV game_done(int result, const char *error,
                               rc_client_t *client, void *ud)
{
   const rc_client_game_t *g = rc_client_get_game_info(client);
   rc_client_user_game_summary_t s;
   char t[96];
   if (!R.game_on || (unsigned)(uintptr_t)ud != R.game_gen)
      return;                      /* another game's answer */
   memset(&s, 0, sizeof(s));
   if (result == RC_OK && g && g->id)
   {
      rc_client_get_user_game_summary(client, &s);
      snprintf(R.game_title, sizeof(R.game_title), "%s",
               g->title ? g->title : "");
      /* Leaderboards are Hardcore's too: a score is sent as one. */
      R.game_state = s.num_core_achievements ||
                     rc_client_has_leaderboards(client) ? 1 : -1;
      fe_evt("ra_game ok=1 id=%u core=%u unlocked=%u hardcore=%d title=\"%s\"",
             g->id, s.num_core_achievements, s.num_unlocked_achievements,
             R.game_hardcore, R.game_title);
      if (s.num_core_achievements)
      {
         snprintf(t, sizeof(t), "%u of %u achievements%s",
                  s.num_unlocked_achievements, s.num_core_achievements,
                  R.game_hardcore ? " - Hardcore" : "");
         toast("%s", t);
      }
      else
         toast("%s", "No achievements for this game yet");
   }
   else
   {
      R.game_state = -1;
      fe_evt("ra_game ok=0 rc=%d msg=\"%s\"", result, error ? error : "");
      if (result == RC_NO_GAME_LOADED)
         toast("%s", "RetroAchievements does not know this version");
      else if (result != RC_ABORTED && result != RC_LOGIN_REQUIRED)
         toast("Achievements unavailable: %s", error ? error : "no answer");
   }
   if (R.game_state < 0)
   {
      cheats_hardcore(0);           /* nothing to protect */
      rc_client_set_hardcore_enabled(client, 0);
   }
}


void ra_game_begin(const char *boot_path, fe_console_t console)
{
   (void)console;
   R.game_on = 0;
   R.game_state = 0;
   R.game_title[0] = '\0';
   R.stale = 0;
   R.was_linked = 0;
   R.game_gen++;
   if (R.rc && !rc_client_get_user_info(R.rc) && !R.login_busy &&
       R.user[0] && R.token[0])
      token_login();               /* a sign-in that did not get through */
   if (!R.rc || (!rc_client_get_user_info(R.rc) && !R.login_busy))
      return;
   /* Hardcore is in force from the first frame; the achievements come
    * once the file's hash (the net thread's, ra_pump) and the server's
    * answer are in. */
   R.game_on = 1;
   R.game_hardcore = R.hardcore;
   rc_client_set_hardcore_enabled(R.rc, R.game_hardcore);
   cheats_hardcore(R.game_hardcore);
   snprintf(R.hash_path, sizeof(R.hash_path), "%s", boot_path);
   R.hash_want = 1;
}

/* The game's MD5 is in (or not): ask for its achievements. */
static void hash_done(const ra_http_res *res)
{
   if (!R.game_on || R.hash_gen != R.game_gen)
      return;                      /* another game's, or none now */
   if (res->status != 0)
   {
      fe_evt("ra_game hash FAILED path=%s why=%s", R.hash_path, res->note);
      R.game_state = -1;
      cheats_hardcore(0);
      rc_client_set_hardcore_enabled(R.rc, 0);
      return;
   }
   snprintf(R.game_hash, sizeof(R.game_hash), "%s", res->note);
   fe_evt("ra_game_begin hash=%s hardcore=%d", res->note, R.game_hardcore);
   rc_client_begin_load_game(R.rc, res->note, game_done,
                             (void *)(uintptr_t)R.game_gen);
}

void ra_game_end(void)
{
   ra_list_free();
   if (!R.game_on)
      return;
   R.game_on = 0;
   R.game_state = 0;
   R.hash_want = 0;
   R.game_gen++;
   cheats_hardcore(0);
   if (R.rc)
      rc_client_unload_game(R.rc);
   fe_evt("ra_game_end");
}

void ra_frame(int linked)
{
   if (!R.game_on || !R.rc)
      return;
   if (linked)
   {
      R.was_linked = 1;
      return;                      /* ra_pump runs the idle work */
   }
   if (R.was_linked)
   {
      R.was_linked = 0;
      R.stale = 1;
   }
   if (R.stale)
   {
      R.stale = 0;
      rc_client_deserialize_progress_sized(R.rc, NULL, 0);
   }
   rc_client_do_frame(R.rc);
   R.framed = 1;
}

void ra_state_loaded(void)
{
   if (R.game_on)
      R.stale = 1;
}

void ra_game_reset(void)
{
   if (R.game_on && R.rc)
      rc_client_reset(R.rc);
}

int ra_hardcore_active(void)
{
   return R.game_on && R.game_hardcore && R.game_state >= 0;
}

int ra_hardcore_pending(void)
{
   return R.game_on && R.game_hardcore && R.game_state == 0;
}

/* ---- the list --------------------------------------------------------------- */

int ra_summary(unsigned *unlocked, unsigned *total, unsigned *points_have,
               unsigned *points_total, int *hardcore)
{
   rc_client_user_game_summary_t s;
   if (!R.game_on || R.game_state != 1 || !R.rc)
      return 0;
   memset(&s, 0, sizeof(s));
   rc_client_get_user_game_summary(R.rc, &s);
   if (unlocked) *unlocked = s.num_unlocked_achievements;
   if (total) *total = s.num_core_achievements;
   if (points_have) *points_have = s.points_unlocked;
   if (points_total) *points_total = s.points_core;
   if (hardcore) *hardcore = R.game_hardcore;
   return 1;
}

const char *ra_game_title(void)
{
   return R.game_title;
}

void ra_list_free(void)
{
   free(R.list);
   R.list = NULL;
   R.list_n = 0;
}

int ra_list_build(void)
{
   rc_client_achievement_list_t *l;
   uint32_t b, k;
   int n = 0;
   ra_list_free();
   if (!R.game_on || R.game_state != 1 || !R.rc)
      return 0;
   l = rc_client_create_achievement_list(R.rc,
          RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
          RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
   if (!l)
      return 0;
   for (b = 0; b < l->num_buckets; b++)
      n += (int)l->buckets[b].num_achievements;
   R.list = n ? (ra_ach *)calloc((size_t)n, sizeof(ra_ach)) : NULL;
   n = 0;
   for (b = 0; R.list && b < l->num_buckets; b++)
      for (k = 0; k < l->buckets[b].num_achievements; k++)
      {
         const rc_client_achievement_t *a = l->buckets[b].achievements[k];
         ra_ach *o = &R.list[n++];
         snprintf(o->title, sizeof(o->title), "%s", a->title ? a->title : "");
         snprintf(o->desc, sizeof(o->desc), "%s",
                  a->description ? a->description : "");
         snprintf(o->progress, sizeof(o->progress), "%s",
                  a->measured_progress);
         o->points = a->points;
         o->unlocked = a->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
      }
   rc_client_destroy_achievement_list(l);
   R.list_n = n;
   return n;
}

const ra_ach *ra_list_get(int i)
{
   return i >= 0 && i < R.list_n ? &R.list[i] : NULL;
}
