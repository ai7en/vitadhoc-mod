/* avclip_vita.c -- see avclip_vita.h. */
#include <stdlib.h>
#include <string.h>
#include "avclip_vita.h"

#ifdef __vita__
#include <malloc.h>
#include <psp2/avplayer.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include "fe_evt.h"

#define AV_GIVE_UP_US 5000000u    /* no first picture by then: broken file */

struct av_player
{
   SceAvPlayerHandle h;
   int got;                       /* a picture has come                    */
   uint64_t last_ts;
   unsigned opened_us;
   unsigned frames;
   unsigned logged;
};

static int g_uv_swap, g_stride_align = 8;
static int g_module;               /* 1 loaded, -1 failed                  */

void av_set_options(int uv_swap, int stride_align)
{
   g_uv_swap = uv_swap ? 1 : 0;
   g_stride_align = stride_align == 16 || stride_align == 32 ? stride_align : 8;
}

static void *av_alloc(void *arg, uint32_t align, uint32_t size)
{
   (void)arg;
   return memalign(align < 8 ? 8 : align, size);
}
static void av_free(void *arg, void *ptr) { (void)arg; free(ptr); }

/* the decoder writes its pictures into physically contiguous memory */
static void *av_alloc_frame(void *arg, uint32_t align, uint32_t size)
{
   SceUID uid;
   void *base = NULL;
   (void)arg; (void)align;
   size = (size + 0xFFFFFu) & ~0xFFFFFu;
   uid = sceKernelAllocMemBlock("avclip_frame",
                                SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
                                size, NULL);
   if (uid < 0 || sceKernelGetMemBlockBase(uid, &base) < 0)
   {
      if (uid >= 0)
         sceKernelFreeMemBlock(uid);
      return NULL;
   }
   return base;
}
static void av_free_frame(void *arg, void *ptr)
{
   SceUID uid = sceKernelFindMemBlockByAddr(ptr, 0);
   (void)arg;
   if (uid >= 0)
      sceKernelFreeMemBlock(uid);
}

static int av_module(void)
{
   if (!g_module)
   {
      int rc = sceSysmoduleIsLoaded(SCE_SYSMODULE_AVPLAYER) == SCE_SYSMODULE_LOADED
               ? 0 : sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
      g_module = rc >= 0 ? 1 : -1;
      fe_evt("video_module rc=%08x", (unsigned)rc);
   }
   return g_module > 0;
}

av_player *av_open(const char *path)
{
   SceAvPlayerInitData init;
   av_player *p;
   int rc;
   SceIoStat st;
   if (!path || sceIoGetstat(path, &st) < 0 || st.st_size <= 0 || !av_module())
      return NULL;            /* (a missing file is found here, not in 5 s) */
   p = (av_player *)calloc(1, sizeof(*p));
   if (!p)
      return NULL;
   memset(&init, 0, sizeof(init));
   init.memoryReplacement.allocate = av_alloc;
   init.memoryReplacement.deallocate = av_free;
   init.memoryReplacement.allocateTexture = av_alloc_frame;
   init.memoryReplacement.deallocateTexture = av_free_frame;
   init.basePriority = 0xA0;
   init.numOutputVideoFrameBuffers = 3;
   init.autoStart = 1;
   p->h = sceAvPlayerInit(&init);
   /* the handle is a pointer into user memory (0x8xxxxxxx, negative as an
    * int); the errors are 0x806Axxxx and the like, far below any pointer */
   if (p->h == 0 || (unsigned)p->h < 0x81000000u)
   {
      fe_evt("video_open init=%08x", (unsigned)p->h);
      free(p);
      return NULL;
   }
   rc = sceAvPlayerAddSource(p->h, path);
   fe_evt("video_open rc=%08x path=%s", (unsigned)rc, path);
   if (rc < 0)
   {
      sceAvPlayerClose(p->h);
      free(p);
      return NULL;
   }
   sceAvPlayerSetLooping(p->h, 0);
   p->opened_us = (unsigned)sceKernelGetProcessTimeLow();
   return p;
}

void av_close(av_player *p)
{
   if (!p)
      return;
   sceAvPlayerStop(p->h);
   sceAvPlayerClose(p->h);
   free(p);
}

static unsigned char av_clip8(int v)
{
   return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* BT.601, limited range: what a standard-definition video is */
static void av_convert(const SceAvPlayerFrameInfo *f, uint16_t *out,
                       unsigned vw, unsigned vh, unsigned ow, unsigned oh)
{
   unsigned stride = (vw + (unsigned)g_stride_align - 1) & ~((unsigned)g_stride_align - 1);
   const uint8_t *yp = f->pData;
   const uint8_t *cp = yp + (size_t)stride * ((vh + 1) & ~1u);
   unsigned x, y;
   unsigned xs = (vw << 16) / ow, ys = (vh << 16) / oh;
   for (y = 0; y < oh; y++)
   {
      unsigned sy = (y * ys) >> 16;
      const uint8_t *yr = yp + (size_t)sy * stride;
      const uint8_t *cr = cp + (size_t)(sy >> 1) * stride;
      uint16_t *o = out + (size_t)y * ow;
      for (x = 0; x < ow; x++)
      {
         unsigned sx = (x * xs) >> 16;
         int c = (int)yr[sx] - 16, d, e, r, g, b;
         int v = cr[sx & ~1u], u = cr[(sx & ~1u) + 1];   /* the plane holds V, U */
         if (g_uv_swap)
         {
            int t = u; u = v; v = t;
         }
         d = u - 128; e = v - 128;
         if (c < 0) c = 0;
         c *= 298;
         r = (c + 409 * e + 128) >> 8;
         g = (c - 100 * d - 208 * e + 128) >> 8;
         b = (c + 516 * d + 128) >> 8;
         o[x] = (uint16_t)(((av_clip8(r) >> 3) << 11) |
                           ((av_clip8(g) >> 2) << 5) | (av_clip8(b) >> 3));
      }
   }
}

int av_next(av_player *p, uint16_t *out, unsigned *w, unsigned *h,
            unsigned *delay_ms)
{
   SceAvPlayerFrameInfo f;
   int i;
   if (!p)
      return AV_BAD;
   /* nobody plays the sound; the player must still be emptied of it */
   for (i = 0; i < 8 && sceAvPlayerGetAudioData(p->h, &f); i++)
      ;
   memset(&f, 0, sizeof(f));
   if (sceAvPlayerGetVideoData(p->h, &f) && f.pData)
   {
      unsigned vw = f.details.video.width, vh = f.details.video.height;
      unsigned ow, oh;
      uint64_t d;
      if (!vw || !vh || vw > 2048 || vh > 2048)
         return AV_BAD;
      ow = vw < AV_W_MAX ? vw : AV_W_MAX;
      oh = (unsigned)((uint64_t)vh * ow / vw);
      if (oh > AV_H_MAX)
      {
         oh = AV_H_MAX;
         ow = (unsigned)((uint64_t)vw * oh / vh);
      }
      if (ow < 2) ow = 2;
      if (oh < 2) oh = 2;
      d = p->got && f.timeStamp > p->last_ts ? f.timeStamp - p->last_ts : 0;
      if (d > 250) d = 250;
      p->last_ts = f.timeStamp;
      if (!p->got || p->logged < 3)
      {
         fe_evt("video_frame %ux%u -> %ux%u ts=%u stride_align=%d uv_swap=%d",
                vw, vh, ow, oh, (unsigned)f.timeStamp, g_stride_align, g_uv_swap);
         p->logged++;
      }
      p->got = 1;
      p->frames++;
      if (out)
         av_convert(&f, out, vw, vh, ow, oh);
      *w = ow; *h = oh; *delay_ms = (unsigned)d;
      return AV_FRAME;
   }
   if (p->got && !sceAvPlayerIsActive(p->h))
      return AV_END;
   if (!p->got && (unsigned)((unsigned)sceKernelGetProcessTimeLow() - p->opened_us) > AV_GIVE_UP_US)
   {
      fe_evt("video_bad no picture in %u us", AV_GIVE_UP_US);
      return AV_BAD;
   }
   return AV_NONE;
}

#else /* the twin: no SceAvPlayer */

struct av_player { int unused; };
void av_set_options(int uv_swap, int stride_align) { (void)uv_swap; (void)stride_align; }
av_player *av_open(const char *path) { (void)path; return NULL; }
void av_close(av_player *p) { (void)p; }
int av_next(av_player *p, uint16_t *out, unsigned *w, unsigned *h,
            unsigned *delay_ms)
{
   (void)p; (void)out; (void)w; (void)h; (void)delay_ms;
   return AV_BAD;
}

#endif
