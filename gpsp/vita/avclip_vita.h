/* avclip_vita.h -- a scraped video (videos/<game>.mp4) as a home-screen preview.
 *
 * The system's SceAvPlayer decodes the H.264 file (hardware decoder, its own
 * threads) and keeps its own clock; av_next, called from the preview worker
 * (preview_vita.c), takes the frame that is due, converts it from YUV 4:2:0
 * to RGB565 on the CPU at up to AV_W_MAX x AV_H_MAX, and drops the sound.
 * Without SceAvPlayer (the twin, a module that does not load) av_open
 * returns NULL and the game keeps its still picture.
 *
 * vita.ini: video_uv_swap = 0 reads the chroma plane as VU instead of UV (1, the default,
 * is right on the console: with VU the colours came out blue-for-red);
 * video_stride = 8 | 16 | 32 is the alignment of a frame's rows (a picture
 * that comes out sheared has a different one). */
#ifndef GBADHOC_AVCLIP_VITA_H
#define GBADHOC_AVCLIP_VITA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AV_W_MAX 320
#define AV_H_MAX 240

typedef struct av_player av_player;

void av_set_options(int uv_swap, int stride_align);

/* NULL: no player, or the file does not open. */
av_player *av_open(const char *path);
void av_close(av_player *p);

enum { AV_NONE = 0, AV_FRAME = 1, AV_END = 2, AV_BAD = -1 };
/* One call a poll.  AV_FRAME: `out` (AV_W_MAX * AV_H_MAX u16 or NULL to drop
 * the frame) holds *w x *h RGB565 pixels, *delay_ms after the one before.
 * AV_END: the video is over.  AV_BAD: it never produced a picture. */
int av_next(av_player *p, uint16_t *out, unsigned *w, unsigned *h,
            unsigned *delay_ms);

#ifdef __cplusplus
}
#endif

#endif
