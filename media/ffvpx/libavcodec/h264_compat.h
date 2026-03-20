/* H.264 compatibility shims for ffvpx (Tiger PPC) */
#ifndef H264_COMPAT_H
#define H264_COMPAT_H

#include <string.h>
#include "libavutil/frame.h"
#include "threadframe.h"

#ifndef emms_c
#define emms_c() ((void)0)
#endif

static inline void ff_thread_release_buffer(AVCodecContext *avctx, ThreadFrame *tf)
{
    av_frame_unref(tf->f);
}

static inline enum AVPixelFormat ff_thread_get_format(AVCodecContext *avctx,
                                                       const enum AVPixelFormat *fmt)
{
    return fmt[0];
}

static inline void ff_color_frame(AVFrame *frame, const int c[4])
{
    int p;
    for (p = 0; p < 3; p++) {
        int h, y;
        if (!frame->data[p]) continue;
        h = (p == 0) ? frame->height : (frame->height + 1) >> 1;
        for (y = 0; y < h; y++)
            memset(frame->data[p] + y * frame->linesize[p],
                   c[p], frame->linesize[p]);
    }
}

#endif
