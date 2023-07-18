/*
 * This file is part of txproto.
 *
 * txproto is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * txproto is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with txproto; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <libtxproto/utils.h>

#include <libtxproto/encode.h>

static const AVCodecHWConfig *get_codec_hw_config(EncodingContext *ctx)
{
    int idx = 0;
    const AVCodecHWConfig *cfg;
    while ((cfg = avcodec_get_hw_config(ctx->codec, idx++)))
        if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX)
            return cfg;
    return NULL;
}

static enum AVSampleFormat pick_codec_sample_fmt(const AVCodec *codec,
                                                 enum AVSampleFormat ifmt,
                                                 int ibps)
{
    int i = 0;
    int max_bps = 0;
    enum AVSampleFormat max_bps_fmt = AV_SAMPLE_FMT_NONE;

    ibps = ibps >> 3;

    /* Accepts anything */
    if (!codec->sample_fmts)
        return ifmt;

    /* Try to match the input sample format first */
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        if (codec->sample_fmts[i] == ifmt)
            return codec->sample_fmts[i];
        i++;
    }

    i = 0;

    /* Try to match bits per sample */
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        int bps = av_get_bytes_per_sample(codec->sample_fmts[i]);
        if (bps > max_bps) {
            max_bps = bps;
            max_bps_fmt = codec->sample_fmts[i];
        }
        if (bps >= ibps)
            return codec->sample_fmts[i];
        i++;
    }

    /* Return the best one */
    return max_bps_fmt;
}

static int pick_codec_sample_rate(const AVCodec *codec, int irate)
{
    int i = 0, ret;
    if (!codec->supported_samplerates)
        return irate;

    /* Go to the array terminator (0) */
    while (codec->supported_samplerates[++i] > 0);
    /* Alloc, copy and sort array upwards */
    int *tmp = av_malloc(i*sizeof(int));
    memcpy(tmp, codec->supported_samplerates, i*sizeof(int));
    qsort(tmp, i, sizeof(int), cmp_numbers);

    /* Pick lowest one above the input rate, otherwise just use the highest one */
    for (int j = 0; j < i; j++) {
        ret = tmp[j];
        if (ret >= irate)
            break;
    }

    av_free(tmp);

    return ret;
}

static const uint64_t pick_codec_channel_layout(const AVCodec *codec,
                                                uint64_t ilayout)
{
    int i = 0;
    int max_channels = 0;
    int in_channels = av_get_channel_layout_nb_channels(ilayout);
    uint64_t best_layout = 0;

    /* Supports anything */
    if (!codec->channel_layouts)
        return ilayout;

    /* Try to match */
    while (1) {
        if (!codec->channel_layouts[i])
            break;
        if (codec->channel_layouts[i] == ilayout)
            return codec->channel_layouts[i];
        i++;
    }

    i = 0;

    /* Try to match channel counts */
    while (1) {
        if (!codec->channel_layouts[i])
            break;
        int num = av_get_channel_layout_nb_channels(codec->channel_layouts[i]);
        if (num > max_channels) {
            max_channels = num;
            best_layout = codec->channel_layouts[i];
        }
        if (num >= in_channels)
            return codec->channel_layouts[i];
        i++;
    }

    /* Whatever */
    return best_layout;
}

static int64_t get_next_audio_pts(EncodingContext *ctx, AVFrame *in)
{
    const int64_t m = (int64_t)ctx->swr_configured_rate * ctx->avctx->sample_rate;
    const int64_t b = (int64_t)ctx->avctx->time_base.num * m;
    const int64_t c = ctx->avctx->time_base.den;
    const int64_t in_pts = in ? in->pts : AV_NOPTS_VALUE;

    int64_t npts = in_pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale(in_pts, b, c);

    npts = swr_next_pts(ctx->swr, npts);

    int64_t out_pts = av_rescale(npts, c, b);

    return out_pts;
}
