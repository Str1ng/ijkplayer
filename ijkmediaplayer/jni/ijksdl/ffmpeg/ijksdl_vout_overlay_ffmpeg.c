/*****************************************************************************
 * ijksdl_vout_overlay_ffmpeg.c
 *****************************************************************************
 *
 * copyright (c) 2013 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ijksdl_vout_overlay_ffmpeg.h"

#include <assert.h>
#include "../ijksdl_stdinc.h"
#include "../ijksdl_mutex.h"
#include "../ijksdl_vout_internal.h"
#include "../ijksdl_video.h"
#include "ijksdl_inc_ffmpeg.h"
#include "ijksdl_image_convert.h"

typedef struct SDL_VoutOverlay_Opaque {
    SDL_mutex *mutex;

    AVFrame *frame;
    AVBufferRef *frame_buffer;
    int planes;

    AVFrame *linked_frame;

    Uint16 pitches[AV_NUM_DATA_POINTERS];
    Uint8 *pixels[AV_NUM_DATA_POINTERS];

    int no_neon_warned;
} SDL_VoutOverlay_Opaque;

/* Always assume a linesize alignment of 1 here */
// TODO: 9 alignment to speed up memcpy when display
static AVFrame *alloc_avframe(SDL_VoutOverlay_Opaque* opaque, enum AVPixelFormat format, int width, int height)
{
    int frame_bytes = avpicture_get_size(format, width, height);
    AVBufferRef *frame_buffer_ref = av_buffer_alloc(frame_bytes);
    if (!frame_buffer_ref)
        return NULL;

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        av_buffer_unref(&frame_buffer_ref);
        return NULL;
    }

    AVFrame *linked_frame = av_frame_alloc();
    if (!frame) {
        av_frame_free(&frame);
        av_buffer_unref(&frame_buffer_ref);
        return NULL;
    }

    AVPicture *pic = (AVPicture *) frame;
    avcodec_get_frame_defaults(frame);
    avpicture_fill(pic, frame_buffer_ref->data, format, width, height);
    opaque->frame_buffer = frame_buffer_ref;
    opaque->linked_frame = linked_frame;
    return frame;
}

static void overlay_free_l(SDL_VoutOverlay *overlay)
{
    ALOGE("SDL_Overlay(ffmpeg): overlay_free_l(%p)", overlay);
    if (!overlay)
        return;

    SDL_VoutOverlay_Opaque *opaque = overlay->opaque;
    if (!opaque)
        return;

    if (opaque->frame)
        av_frame_free(&opaque->frame);

    if (opaque->linked_frame) {
        av_frame_unref(opaque->linked_frame);
        av_frame_free(&opaque->linked_frame);
    }

    if (opaque->frame_buffer)
        av_buffer_unref(&opaque->frame_buffer);

    if (opaque->mutex)
        SDL_DestroyMutex(opaque->mutex);

    SDL_VoutOverlay_FreeInternal(overlay);
}

static void overlay_fill(SDL_VoutOverlay *overlay, AVFrame *frame, int planes)
{
    AVPicture *pic = (AVPicture *) frame;
    overlay->planes = planes;

    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
        overlay->pixels[i] = pic->data[i];
        overlay->pitches[i] = pic->linesize[i];
    }
}

static int overlay_lock(SDL_VoutOverlay *overlay)
{
    SDL_VoutOverlay_Opaque *opaque = overlay->opaque;
    return SDL_LockMutex(opaque->mutex);
}

static int overlay_unlock(SDL_VoutOverlay *overlay)
{
    SDL_VoutOverlay_Opaque *opaque = overlay->opaque;
    return SDL_UnlockMutex(opaque->mutex);
}

SDL_VoutOverlay *SDL_VoutFFmpeg_CreateOverlay(int width, int height, Uint32 format, SDL_Vout *display)
{
    SDLTRACE("SDL_VoutFFmpeg_CreateOverlay(w=%d, h=%d, fmt=%.4s(0x%x, dp=%p)",
        width, height, (const char*) &format, format, display);
    SDL_VoutOverlay *overlay = SDL_VoutOverlay_CreateInternal(sizeof(SDL_VoutOverlay_Opaque));
    if (!overlay) {
        ALOGE("overlay allocation failed");
        return NULL;
    }

    SDL_VoutOverlay_Opaque *opaque = overlay->opaque;
    overlay->format = format;
    overlay->pitches = opaque->pitches;
    overlay->pixels = opaque->pixels;
    overlay->w = width;
    overlay->h = height;

    enum AVPixelFormat ff_format = AV_PIX_FMT_NONE;
    int buf_width = width;  // must be aligned to 16 bytes pitch for arm-neon image-convert
    int buf_height = height;
    switch (format) {
    case SDL_FCC_YV12: {
        ff_format = AV_PIX_FMT_YUV420P;
        buf_width = IJKALIGN(width, 16); // 1 bytes per pixel for Y-plane
        opaque->planes = 3;
        break;
    }
    case SDL_FCC_RV16: {
        ff_format = AV_PIX_FMT_RGB565;
        buf_width = IJKALIGN(width, 8); // 2 bytes per pixel
        opaque->planes = 1;
        break;
    }
    case SDL_FCC_RV32: {
        ff_format = AV_PIX_FMT_0BGR32;
        buf_width = IJKALIGN(width, 4); // 4 bytes per pixel
        opaque->planes = 1;
        break;
    }
    default:
        ALOGE("SDL_VoutFFmpeg_CreateOverlay(...): unknown format %.4s(0x%x)", (char*)&format, format);
        goto fail;
    }

    opaque->frame = alloc_avframe(opaque, ff_format, buf_width, buf_height);
    if (!opaque->frame) {
        ALOGE("overlay->opaque->frame allocation failed");
        goto fail;
    }
    opaque->mutex = SDL_CreateMutex();
    overlay_fill(overlay, opaque->frame, opaque->planes);

    overlay->free_l = overlay_free_l;
    overlay->lock = overlay_lock;
    overlay->unlock = overlay_unlock;

    return overlay;

    fail:
    overlay_free_l(overlay);
    return NULL;
}

int SDL_VoutFFmpeg_ConvertFrame(
    SDL_VoutOverlay *overlay, AVFrame *frame,
    struct SwsContext **p_sws_ctx, int sws_flags)
{
    assert(overlay);
    assert(p_sws_ctx);
    SDL_VoutOverlay_Opaque *opaque = overlay->opaque;
    AVPicture dest_pic = { { 0 } };

    for (int i = 0; i < overlay->planes; ++i) {
        dest_pic.data[i] = overlay->pixels[i];
        dest_pic.linesize[i] = overlay->pitches[i];
    }

    av_frame_unref(opaque->linked_frame);

    int use_linked_frame = 0;
    enum AVPixelFormat dst_format = AV_PIX_FMT_NONE;
    switch (overlay->format) {
    case SDL_FCC_YV12:
        if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
            // ALOGE("direct draw frame");
            use_linked_frame = 1;
            av_frame_ref(opaque->linked_frame, frame);
            overlay_fill(overlay, opaque->linked_frame, opaque->planes);
            FFSWAP(Uint8*, overlay->pixels[1], overlay->pixels[2]);
        } else {
            // ALOGE("copy draw frame");
            overlay_fill(overlay, opaque->frame, opaque->planes);
            dest_pic.data[2] = overlay->pixels[1];
            dest_pic.data[1] = overlay->pixels[2];
        }
        break;
    case SDL_FCC_RV32:
        // TODO: 9 android only
        overlay_fill(overlay, opaque->frame, opaque->planes);
        dst_format = AV_PIX_FMT_0BGR32;
        break;
    case SDL_FCC_RV16:
        // TODO: 9 android only
        overlay_fill(overlay, opaque->frame, opaque->planes);
        dst_format = AV_PIX_FMT_RGB565;
        break;
    default:
        ALOGE("SDL_VoutFFmpeg_ConvertPicture: unexpected overlay format %s(%d)",
            (char*)&overlay->format, overlay->format);
        return -1;
    }

    if (use_linked_frame) {
        // do nothing
    } else if (ijk_image_convert(frame->width, frame->height,
        dst_format, dest_pic.data, dest_pic.linesize,
        frame->format, (const uint8_t**) frame->data, frame->linesize)) {
        *p_sws_ctx = sws_getCachedContext(*p_sws_ctx,
            frame->width, frame->height, frame->format, frame->width, frame->height,
            dst_format, sws_flags, NULL, NULL, NULL);
        if (*p_sws_ctx == NULL) {
            ALOGE("sws_getCachedContext failed");
            return -1;
        }

        sws_scale(*p_sws_ctx, (const uint8_t**) frame->data, frame->linesize,
            0, frame->height, dest_pic.data, dest_pic.linesize);

        if (!opaque->no_neon_warned) {
            opaque->no_neon_warned = 1;
            ALOGE("non-neon image convert %s -> %s", av_get_pix_fmt_name(frame->format), av_get_pix_fmt_name(dst_format));
        }
    }

    // TODO: 9 draw black if overlay is larger than screen
    return 0;
}
