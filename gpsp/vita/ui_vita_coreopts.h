/* ui_vita_coreopts.h -- Settings > Core options: the options of the
 * libretro core that runs the game (Genesis Plus GX, FCEUmm, Snes9x).
 *
 * Included by psp/ui_psp.c under -DVITA.  The options and their file are
 * frontend-common/fe_retro.c's (fe_retro_opt_*): the core gives a label and
 * a list of values for each, the page steps through the list, and the
 * choice is written to ux0:data/RetroAdhoc/cores/<console>.ini when the
 * page is left.  The core reads a changed value on its next frame; a few
 * options only take effect when the game is started again.
 *
 *   UP / DOWN       choose an option     LEFT / RIGHT, X   next / previous value
 *   L / R           a page at a time     O                 back to Settings
 */
#ifndef UI_VITA_COREOPTS_H
#define UI_VITA_COREOPTS_H

#define CO_ROW_H    29
#define CO_VIEW_TOP 70
#define CO_ROWS     5
#define CO_VIEW_H   (CO_ROWS * CO_ROW_H)
#define CO_TEXT_DY  ((CO_ROW_H - FE_FONT_H) / 2 + 1)

static struct
{
   int cur, scroll, free_scroll;
   tch_scroll_t ts;
} g_co;

/* Whether Settings shows the row: a libretro core runs the game and offers
 * options. */
static int co_available(void)
{
   return g_pgame.on && fe_retro_active() && fe_retro_opt_count() > 0;
}

static void co_open(void)
{
   screen_to(SCR_COREOPT);
   memset(&g_co, 0, sizeof(g_co));
}

static void co_close(void)
{
   (void)fe_retro_opt_save();      /* once, when the page is left */
   screen_to(SCR_SETTINGS);
   g_cursor = SET_COREOPT;
}

static ui_action screen_coreopt(unsigned edges)
{
   int n = fe_retro_opt_count(), max, i;

   if (n <= 0)
   {
      co_close();
      return UI_ACT_NONE;
   }
   if (tch_rows(&g_co.cur, &edges) || g_tch.hover_row != TCH_NONE)
      g_co.free_scroll = 1;
   if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LTRIGGER |
                PSP_CTRL_RTRIGGER))
   {
      if (edges & PSP_CTRL_UP)
         g_co.cur = (g_co.cur + n - 1) % n;
      else if (edges & PSP_CTRL_DOWN)
         g_co.cur = (g_co.cur + 1) % n;
      else if (edges & PSP_CTRL_LTRIGGER)
         g_co.cur = g_co.cur - CO_ROWS < 0 ? 0 : g_co.cur - CO_ROWS;
      else
         g_co.cur = g_co.cur + CO_ROWS >= n ? n - 1 : g_co.cur + CO_ROWS;
      g_co.free_scroll = 0;
      tch_scroll_stop(&g_co.ts);
   }
   if (g_co.cur >= n) g_co.cur = n - 1;
   if (g_co.cur < 0) g_co.cur = 0;
   if (edges & PSP_CTRL_CIRCLE)
   {
      co_close();
      return UI_ACT_NONE;
   }
   if (edges & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_CROSS))
   {
      const char *l = "", *v = "";
      (void)fe_retro_opt_step(g_co.cur, (edges & PSP_CTRL_LEFT) ? -1 : 1);
      (void)fe_retro_opt_get(g_co.cur, &l, &v);
      fe_evt("core_option label=\"%s\" value=\"%s\"", l, v);
   }

   max = n * CO_ROW_H - CO_VIEW_H;
   if (max < 0)
      max = 0;
   if (tch_scroll(&g_co.ts, &g_co.scroll, max, 0, CO_VIEW_TOP, 456,
                  CO_VIEW_H))
      g_co.free_scroll = 1;
   else if (!g_co.free_scroll)
   {
      if (g_co.cur * CO_ROW_H < g_co.scroll)
         g_co.scroll = g_co.cur * CO_ROW_H;
      if ((g_co.cur + 1) * CO_ROW_H > g_co.scroll + CO_VIEW_H)
         g_co.scroll = (g_co.cur + 1) * CO_ROW_H - CO_VIEW_H;
   }
   if (g_co.scroll > max) g_co.scroll = max;
   if (g_co.scroll < 0) g_co.scroll = 0;
   if (g_co.free_scroll)
   {
      int first = (g_co.scroll + CO_ROW_H - 1) / CO_ROW_H;
      int last = (g_co.scroll + CO_VIEW_H) / CO_ROW_H - 1;
      if (last >= n) last = n - 1;
      if (g_co.cur < first) g_co.cur = first;
      if (g_co.cur > last) g_co.cur = last;
   }

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(g_ov_title, "CORE OPTIONS");
   vid_clip(0, CO_VIEW_TOP - 1, VID_SCR_W, CO_VIEW_H + 2);
   for (i = g_co.scroll / CO_ROW_H; i < n; i++)
   {
      const char *l = "", *v = "";
      char lab[64];
      int y = CO_VIEW_TOP + CO_TEXT_DY + i * CO_ROW_H - g_co.scroll;
      int cur = i == g_co.cur, ty = y - CO_TEXT_DY + 1, th = CO_ROW_H;
      if (y >= CO_VIEW_TOP + CO_VIEW_H)
         break;
      (void)fe_retro_opt_get(i, &l, &v);
      if (cur)
         ov_band(y, 456, CO_ROW_H);
      snprintf(lab, sizeof(lab), "%s", l);
      clip_title(lab, 270);
      vid_text(22, y, lab, cur ? C_SEL : C_ITEM);
      ov_text_right(448, y, v, cur ? C_SEL : C_VALUE);
      if (ty < CO_VIEW_TOP) { th -= CO_VIEW_TOP - ty; ty = CO_VIEW_TOP; }
      if (ty + th > CO_VIEW_TOP + CO_VIEW_H)
         th = CO_VIEW_TOP + CO_VIEW_H - ty;
      tch_row(i, 0, ty, 456, th);
   }
   vid_clip_off();
   ov_scrollbar(470, CO_VIEW_TOP, CO_VIEW_H, g_co.scroll, CO_VIEW_H,
                n * CO_ROW_H);
   vid_rect(20, CO_VIEW_TOP + CO_VIEW_H + 3, 440, 1, C_ACCENT_DK, 120);
   vw_wrap(22, CO_VIEW_TOP + CO_VIEW_H + 8, 436,
           "Saved for every game on this core. Some options apply when the "
           "game is started again.", C_DIM, 2);
   return UI_ACT_NONE;
}

#endif
