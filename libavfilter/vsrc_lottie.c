/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Lottie animation video source using the ThorVG rendering library.
 *
 * The filter loads a Lottie JSON animation file and outputs RGBA video
 * frames rendered by ThorVG.  By default the animation loops forever;
 * set loop=0 to play it once.
 *
 * Example – overlay a looping Lottie animation over a video:
 * @code
 * ffmpeg -i input.mp4 \
 *   -filter_complex \
 *     "[0:v]scale=1280:720[bg]; \
 *      lottie=f=animation.json:s=370x400[fg]; \
 *      [bg][fg]overlay=W-w-10:H-h-10" \
 *   output.mp4
 * @endcode
 */

#include <math.h>

#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

#include <thorvg_capi.h>

typedef struct LottieContext {
    const AVClass *class;

    /* options */
    char      *filename;
    int        w, h;
    AVRational frame_rate;
    int        loop;

    /* derived timing */
    AVRational time_base;
    int64_t    pts;

    /* ThorVG objects */
    Tvg_Canvas    canvas;
    Tvg_Animation animation;

    /* animation properties */
    float      total_frames; /* from ThorVG */
    float      duration;     /* animation duration in seconds */
} LottieContext;

#define OFFSET(x) offsetof(LottieContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption lottie_options[] = {
    { "filename", "set Lottie JSON filename", OFFSET(filename),   AV_OPT_TYPE_STRING,     { .str = NULL }, 0, 0, FLAGS },
    { "f",        "set Lottie JSON filename", OFFSET(filename),   AV_OPT_TYPE_STRING,     { .str = NULL }, 0, 0, FLAGS },
    { "size",     "set output video size",    OFFSET(w),          AV_OPT_TYPE_IMAGE_SIZE, { .str = NULL }, 0, 0, FLAGS },
    { "s",        "set output video size",    OFFSET(w),          AV_OPT_TYPE_IMAGE_SIZE, { .str = NULL }, 0, 0, FLAGS },
    { "rate",     "set output frame rate",    OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "25" }, 0, INT_MAX, FLAGS },
    { "r",        "set output frame rate",    OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "25" }, 0, INT_MAX, FLAGS },
    { "loop",     "loop the animation",       OFFSET(loop),       AV_OPT_TYPE_INT,        { .i64 = 1    }, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(lottie);

static av_cold int lottie_init(AVFilterContext *ctx)
{
    LottieContext *s = ctx->priv;
    Tvg_Paint picture;
    float pic_w = 0.0f, pic_h = 0.0f;

    if (!s->filename) {
        av_log(ctx, AV_LOG_ERROR, "The filename option is required.\n");
        return AVERROR(EINVAL);
    }

    if (tvg_engine_init(0) != TVG_RESULT_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialise ThorVG engine.\n");
        return AVERROR_EXTERNAL;
    }

    s->animation = tvg_animation_new();
    if (!s->animation) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create ThorVG animation.\n");
        tvg_engine_term();
        return AVERROR(ENOMEM);
    }

    picture = tvg_animation_get_picture(s->animation);
    if (!picture) {
        av_log(ctx, AV_LOG_ERROR, "Failed to obtain ThorVG picture from animation.\n");
        tvg_animation_del(s->animation);
        tvg_engine_term();
        return AVERROR_EXTERNAL;
    }

    if (tvg_picture_load(picture, s->filename) != TVG_RESULT_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to load Lottie file '%s'.\n", s->filename);
        tvg_animation_del(s->animation);
        tvg_engine_term();
        return AVERROR_EXTERNAL;
    }

    /* Retrieve animation timing. */
    tvg_animation_get_total_frame(s->animation, &s->total_frames);
    tvg_animation_get_duration(s->animation, &s->duration);

    if (s->total_frames <= 0.0f || s->duration <= 0.0f) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid animation: total_frames=%.1f duration=%.3fs\n",
               s->total_frames, s->duration);
        tvg_animation_del(s->animation);
        tvg_engine_term();
        return AVERROR_EXTERNAL;
    }

    av_log(ctx, AV_LOG_VERBOSE,
           "Lottie: total_frames=%.1f duration=%.3fs native_fps=%.3f\n",
           s->total_frames, s->duration, s->total_frames / s->duration);

    tvg_picture_get_size(picture, &pic_w, &pic_h);
    if (s->w <= 0 || s->h <= 0) {
        s->w = (pic_w > 0.0f) ? (int)pic_w : 512;
        s->h = (pic_h > 0.0f) ? (int)pic_h : 512;
    }

    if (tvg_picture_set_size(picture, (float)s->w, (float)s->h) != TVG_RESULT_SUCCESS)
        av_log(ctx, AV_LOG_WARNING, "Failed to set picture size.\n");

    s->canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
    if (!s->canvas) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create ThorVG SW canvas.\n");
        tvg_animation_del(s->animation);
        tvg_engine_term();
        return AVERROR(ENOMEM);
    }

    if (tvg_canvas_add(s->canvas, picture) != TVG_RESULT_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to add picture to canvas.\n");
        tvg_canvas_destroy(s->canvas);
        s->canvas = NULL;
        tvg_animation_del(s->animation);
        tvg_engine_term();
        return AVERROR_EXTERNAL;
    }

    s->time_base = av_inv_q(s->frame_rate);
    s->pts       = 0;

    return 0;
}

static av_cold void lottie_uninit(AVFilterContext *ctx)
{
    LottieContext *s = ctx->priv;

    if (s->canvas) {
        tvg_canvas_destroy(s->canvas);
        s->canvas = NULL;
    }
    if (s->animation) {
        tvg_animation_del(s->animation);
        s->animation = NULL;
    }
    tvg_engine_term();
}

static int lottie_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    LottieContext   *s   = ctx->priv;
    FilterLink      *l   = ff_filter_link(outlink);

    outlink->w                  = s->w;
    outlink->h                  = s->h;
    outlink->sample_aspect_ratio = (AVRational){ 1, 1 };
    l->frame_rate               = s->frame_rate;
    outlink->time_base          = s->time_base;

    return 0;
}

static int lottie_activate(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    LottieContext *s      = ctx->priv;
    AVFrame       *frame;
    double anim_time;
    float  anim_frame;
    int ret;

    if (!ff_outlink_frame_wanted(outlink))
        return FFERROR_NOT_READY;

    anim_time = s->pts * av_q2d(s->time_base);

    if (!s->loop && anim_time >= s->duration) {
        ff_outlink_set_status(outlink, AVERROR_EOF, s->pts);
        return 0;
    }

    if (s->duration > 0.0)
        anim_time = fmod(anim_time, s->duration);

    anim_frame = (float)(anim_time * s->total_frames / s->duration);
    if (anim_frame >= s->total_frames)
        anim_frame = s->total_frames - 1.0f;

    {
        Tvg_Result r = tvg_animation_set_frame(s->animation, anim_frame);
        if (r != TVG_RESULT_SUCCESS && r != TVG_RESULT_INSUFFICIENT_CONDITION) {
            av_log(ctx, AV_LOG_ERROR,
                   "Failed to set animation frame %.2f (tvg error %d).\n",
                   anim_frame, (int)r);
            return AVERROR_EXTERNAL;
        }
    }

    frame = ff_get_video_buffer(outlink, s->w, s->h);
    if (!frame)
        return AVERROR(ENOMEM);

    if (tvg_swcanvas_set_target(s->canvas,
                                (uint32_t *)frame->data[0],
                                frame->linesize[0] / sizeof(uint32_t),
                                s->w, s->h,
                                TVG_COLORSPACE_ABGR8888S) != TVG_RESULT_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to set canvas render target.\n");
        av_frame_free(&frame);
        return AVERROR_EXTERNAL;
    }

    if (tvg_canvas_update(s->canvas) != TVG_RESULT_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to update canvas.\n");
        av_frame_free(&frame);
        return AVERROR_EXTERNAL;
    }

    if (tvg_canvas_draw(s->canvas, true) != TVG_RESULT_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to draw canvas.\n");
        av_frame_free(&frame);
        return AVERROR_EXTERNAL;
    }

    if (tvg_canvas_sync(s->canvas) != TVG_RESULT_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to sync canvas.\n");
        av_frame_free(&frame);
        return AVERROR_EXTERNAL;
    }

    frame->pts                 = s->pts++;
    frame->duration            = 1;
    frame->flags              |= AV_FRAME_FLAG_KEY;
    frame->pict_type           = AV_PICTURE_TYPE_I;
    frame->sample_aspect_ratio = (AVRational){ 1, 1 };

    ret = ff_filter_frame(outlink, frame);
    return ret;
}

static int lottie_query_formats(const AVFilterContext *ctx,
                                AVFilterFormatsConfig **cfg_in,
                                AVFilterFormatsConfig **cfg_out)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_NONE
    };
    return ff_set_pixel_formats_from_list2(ctx, cfg_in, cfg_out, pix_fmts);
}

static const AVFilterPad lottie_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = lottie_config_props,
    },
};

const FFFilter ff_vsrc_lottie = {
    .p.name        = "lottie",
    .p.description = NULL_IF_CONFIG_SMALL("Render a Lottie JSON animation using ThorVG."),
    .p.priv_class  = &lottie_class,
    .p.inputs      = NULL,
    .priv_size     = sizeof(LottieContext),
    .init          = lottie_init,
    .uninit        = lottie_uninit,
    .activate      = lottie_activate,
    FILTER_OUTPUTS(lottie_outputs),
    FILTER_QUERY_FUNC2(lottie_query_formats),
};
