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
 *
 * Based on target_dem_fuzzer
 */

#include "config.h"
#include "libavutil/channel_layout.h"
#include "libavutil/iamf.h"
#include "libavutil/mem.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/bytestream.h"
#include "libavformat/avformat.h"
#ifdef FFMPEG_MUXER
#include "libavformat/mux.h"
#endif

/*
 * Muxing fuzzing target.
 *
 * Every other target in this directory drives a decoder, an encoder, a
 * bitstream filter, a demuxer or a rescaler. Muxers were never driven by one,
 * so the whole serialization half of libavformat had no fuzzing coverage at
 * all. This target closes that gap.
 *
 * It builds a complete output context from a flat byte string and pushes it
 * through the public muxing API: avformat_alloc_output_context2(),
 * avformat_new_stream(), avformat_stream_group_create(),
 * avformat_stream_group_add_stream(), avformat_write_header(),
 * av_interleaved_write_frame() and av_write_trailer(). Nothing private is
 * touched and nothing is written to the file system: the AVIOContext handed to
 * the muxer is a discarding sink that only accounts for the bytes it is given.
 *
 * The muxer exercised is IAMF. It is the muxer with the most input derived
 * accounting in the tree: an audio element declares a number of layers and a
 * channel layout per layer, and the writer has to reconcile those against the
 * number of substreams, against the fixed extent of
 * AVIAMFReconGain.recon_gain, and against the width of the bitstream fields it
 * serializes them into. Getting that reconciliation wrong is what a fuzzer is
 * for, and reaching it needs a structurally valid stream group graph that no
 * amount of random bytes fed to a demuxer would ever produce.
 *
 * Two things about the configurations built below are deliberate and easy to
 * undo by accident.
 *
 * Opus is the only codec used. IAMF permits mp4a, Opus, fLaC and ipcm, but the
 * specification forbids recon gain parameters for fLaC and ipcm, so the writer
 * strips them for those two and the recon gain path becomes unreachable, and
 * the codec config writer does not implement mp4a at all. Switching codec
 * would make this target decorative.
 *
 * Audio elements are always CHANNEL_BASED. SCENE_BASED elements are restricted
 * to exactly one ambisonic layer, so they cannot express the multi layer
 * accounting this target exists to cover.
 */

/**
 * Sink state for the AVIOContext the muxer writes into.
 *
 * The bytes themselves are dropped; only the position and the resulting size
 * are kept, which is all the muxer ever asks for. Writing to a real file would
 * make the target non hermetic and slow, and the muxer cannot tell the
 * difference.
 */
typedef struct IOContext {
    int64_t pos;
    int64_t filesize;
} IOContext;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/**
 * Bail out hard.
 *
 * Reserved for allocation failures, which are an environment problem rather
 * than a finding. A rejected configuration must never come through here: the
 * muxer returning an error is the normal, expected outcome for most inputs.
 */
static void error(const char *err)
{
    fprintf(stderr, "%s", err);
    exit(1);
}

static int io_write(void *opaque, const uint8_t *buf, int buf_size)
{
    IOContext *c = opaque;

    if (buf_size < 0)
        return AVERROR(EINVAL);
    if (c->pos > INT64_MAX - buf_size)
        return AVERROR(EIO);

    c->pos     += buf_size;
    c->filesize = FFMAX(c->filesize, c->pos);

    return buf_size;
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
    IOContext *c = opaque;

    if (whence == SEEK_CUR) {
        if (offset > INT64_MAX - c->pos)
            return -1;
        offset += c->pos;
    } else if (whence == SEEK_END) {
        if (offset > INT64_MAX - c->filesize)
            return -1;
        offset += c->filesize;
    } else if (whence == AVSEEK_SIZE) {
        return c->filesize;
    }
    if (offset < 0 || offset > c->filesize)
        return -1;
    c->pos = offset;
    return 0;
}

// Ensure we don't loop forever
const uint32_t maxiteration = 8096;

/**
 * Nested channel layout chains for scalable audio elements.
 *
 * A scalable audio element requires every layer to be a channel mask superset
 * of its predecessor while carrying strictly more channels, so the layouts a
 * multi layer element may use are not freely chosen: they have to form a chain.
 * These are the two longest chains the loudspeaker layout tables admit, which
 * is five and four layers respectively. A layer count beyond a chain's length
 * repeats its last entry, which no longer carries more channels than its
 * predecessor and must therefore be rejected by the writer. That rejection is
 * itself worth reaching.
 */
static const AVChannelLayout scalable_chain_a[5] = {
    AV_CHANNEL_LAYOUT_STEREO,
    AV_CHANNEL_LAYOUT_3POINT1POINT2,
    AV_CHANNEL_LAYOUT_5POINT1POINT2,
    AV_CHANNEL_LAYOUT_5POINT1POINT4_BACK,
    AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK,
};

static const AVChannelLayout scalable_chain_b[4] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_3POINT1POINT2,
    AV_CHANNEL_LAYOUT_7POINT1POINT2,
    AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK,
};

/**
 * Layouts for single layer audio elements.
 *
 * A single layer element has no ordering constraint, so it can use the
 * expanded loudspeaker layouts as well as the standard ones. Several entries
 * are here for a specific reason: the low frequency effects only layout is the
 * expanded layout at index 0, and the three channel front subset is the one
 * whose channel count differs from every standard layout it shares a mask
 * prefix with.
 */
static const AVChannelLayout single_layer_layouts[] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
    AV_CHANNEL_LAYOUT_5POINT1,
    AV_CHANNEL_LAYOUT_3POINT1POINT2,
    AV_CHANNEL_LAYOUT_BINAURAL,
    /* Three channels, L/C/R: the front subset of Sound System J. */
    AV_CHANNEL_LAYOUT_SURROUND,
    /* One channel, LFE: the low frequency effects subset of Sound System J. */
    {
        .nb_channels = 1,
        .order       = AV_CHANNEL_ORDER_NATIVE,
        .u.mask      = AV_CH_LOW_FREQUENCY,
    },
    /* Two channels, Ls/Rs: the surround subset of Sound System I. */
    {
        .nb_channels = 2,
        .order       = AV_CHANNEL_ORDER_NATIVE,
        .u.mask      = AV_CH_SIDE_LEFT | AV_CH_SIDE_RIGHT,
    },
    /* Two channels, Lrs/Rrs: the rear surround subset of Sound System J. */
    {
        .nb_channels = 2,
        .order       = AV_CHANNEL_ORDER_NATIVE,
        .u.mask      = AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT,
    },
    /* Two channels, Ltf/Rtf: the top front subset of Sound System J. */
    {
        .nb_channels = 2,
        .order       = AV_CHANNEL_ORDER_NATIVE,
        .u.mask      = AV_CH_TOP_FRONT_LEFT | AV_CH_TOP_FRONT_RIGHT,
    },
    /* Four channels, Ltf/Rtf/Ltb/Rtb: the top subset of Sound System J. */
    {
        .nb_channels = 4,
        .order       = AV_CHANNEL_ORDER_NATIVE,
        .u.mask      = AV_CH_TOP_FRONT_LEFT | AV_CH_TOP_FRONT_RIGHT |
                       AV_CH_TOP_BACK_LEFT  | AV_CH_TOP_BACK_RIGHT,
    },
    /* Sixteen channels: a subset of Sound System H. */
    AV_CHANNEL_LAYOUT_9POINT1POINT6,
};

/** Channel layouts a substream may have, indexed by channel count minus one. */
static const AVChannelLayout substream_layouts[2] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
};

/** Sample rates that survive the muxer's stream checks. */
static const int sample_rates[4] = { 48000, 44100, 16000, 96000 };

/**
 * A complete muxing scenario, derived from the trailing bytes of the input.
 *
 * Every field has a value that makes sense on its own, so the target still
 * exercises a meaningful configuration when the input is too short to carry a
 * control block.
 */
typedef struct FuzzConfig {
    int nb_elements;        /**< audio element stream groups, 1 or 2 */
    int nb_layers;          /**< layers per audio element, 1 to 8 */
    int chain;              /**< nested layout chain the layers come from */
    int custom_layers;      /**< build the layers from custom order layouts */
    int single_layout;      /**< index into single_layer_layouts */
    int mono_substreams;    /**< split channels into mono, not stereo, parts */
    int extra_substream;    /**< add a substream the layers do not need */
    int drop_substream;     /**< remove a substream the layers do need */
    unsigned recon_gain_layers; /**< bitmask of layers flagged for recon gain */
    unsigned output_gain_flags; /**< output gain flags set on every layer */
    int with_extradata;     /**< give the streams OpusHead extradata */
    int with_recon_info;    /**< attach recon_gain_info to the audio element */
    int with_demix_info;    /**< attach demixing_info to the audio element */
    int dmixp_mode;         /**< demixing mode written into the descriptor */
    int dangling_element;   /**< point the submix at no existing element */
    int binaural_layout;    /**< submix layout is binaural, not loudspeakers */
    int binaural_rendering; /**< submix elements render binaurally */
    int seekable;           /**< give the AVIOContext a seek callback */
    int new_extradata;      /**< end with an empty packet holding extradata */
    unsigned side_data;     /**< which parameter blocks to attach to packets */
    int nb_subblocks;       /**< subblocks in the parameter block side data */
    int sample_rate;
    int frame_size;
    unsigned mix_id;        /**< parameter id of the mix gain definitions */
    unsigned demix_id;      /**< parameter id of the demixing definitions */
    unsigned recon_id;      /**< parameter id of the recon gain definitions */
    int io_buffer_size;
    int max_pkt_size;
    uint8_t recon_seed[8];  /**< expanded into the recon gain matrix */
} FuzzConfig;

static void config_defaults(FuzzConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /*
     * A two layer scalable element with recon gain on both layers and a recon
     * gain parameter block on every packet, which is the shortest path to the
     * code this target was written for.
     */
    cfg->nb_elements       = 1;
    cfg->nb_layers         = 2;
    cfg->single_layout     = 1;
    cfg->recon_gain_layers = UINT_MAX;
    cfg->with_extradata    = 1;
    cfg->with_recon_info   = 1;
    cfg->with_demix_info   = 1;
    cfg->dmixp_mode        = 1;
    cfg->seekable          = 1;
    cfg->side_data         = 7;
    cfg->nb_subblocks      = 1;
    cfg->sample_rate       = sample_rates[0];
    cfg->frame_size        = 960;
    cfg->mix_id            = 100;
    cfg->demix_id          = 998;
    cfg->recon_id          = 101;
    cfg->io_buffer_size    = 32768;
    cfg->max_pkt_size      = 1024;

    for (int i = 0; i < (int)FF_ARRAY_ELEMS(cfg->recon_seed); i++)
        cfg->recon_seed[i] = i & 1 ? 0 : 1 + i * 31;
}

static void config_parse(FuzzConfig *cfg, GetByteContext *gbc)
{
    unsigned flags1 = bytestream2_get_byte(gbc);
    unsigned flags2 = bytestream2_get_byte(gbc);
    unsigned flags3 = bytestream2_get_byte(gbc);

    cfg->nb_elements        = 1 + !!(flags1 & 0x01);
    cfg->chain              = !!(flags1 & 0x02);
    cfg->mono_substreams    = !!(flags1 & 0x04);
    cfg->extra_substream    = !!(flags1 & 0x08);
    cfg->drop_substream     = !!(flags1 & 0x10);
    cfg->dangling_element   = !!(flags1 & 0x20);
    cfg->binaural_layout    = !!(flags1 & 0x40);
    cfg->seekable           = !!(flags1 & 0x80);

    cfg->with_extradata     = !!(flags2 & 0x01);
    cfg->with_recon_info    = !!(flags2 & 0x02);
    cfg->with_demix_info    = !!(flags2 & 0x04);
    cfg->new_extradata      = !!(flags2 & 0x08);
    cfg->binaural_rendering = !!(flags2 & 0x10);
    cfg->side_data          = (flags2 >> 5) & 7;

    cfg->custom_layers      = !!(flags3 & 0x01);

    /*
     * One to eight layers. Seven is one past the extent of
     * AVIAMFReconGain.recon_gain and eight is the first value that no longer
     * fits the three bit num_layers field of the descriptor, so both of the
     * capacities the layer count is measured against have to be reachable.
     */
    cfg->nb_layers          = 1 + bytestream2_get_byte(gbc) % 8;
    cfg->single_layout      = bytestream2_get_byte(gbc) %
                              FF_ARRAY_ELEMS(single_layer_layouts);
    cfg->nb_subblocks       = 1 + bytestream2_get_byte(gbc) % 3;
    cfg->dmixp_mode         = bytestream2_get_byte(gbc) & 7;
    cfg->output_gain_flags  = bytestream2_get_byte(gbc) & 0x3F;
    /* Layer 0 always carries recon gain so the matrix is always indexed. */
    cfg->recon_gain_layers  = bytestream2_get_byte(gbc) | 1;
    cfg->sample_rate        = sample_rates[bytestream2_get_byte(gbc) %
                                           FF_ARRAY_ELEMS(sample_rates)];
    cfg->frame_size         = bytestream2_get_le16(gbc) & 0xFFF;

    /*
     * Parameter ids are kept in a small range on purpose, so that the
     * definitions of an audio element and those of a mix presentation collide
     * often. A collision makes the writer resolve a packet's parameter block
     * against a definition of a different type, which is a path of its own.
     */
    cfg->mix_id             = bytestream2_get_le32(gbc) & 0xFF;
    cfg->demix_id           = bytestream2_get_le32(gbc) & 0xFF;
    cfg->recon_id           = bytestream2_get_le32(gbc) & 0xFF;

    /*
     * The AVIO buffer size decides how often the sink is called, and a small
     * one is worth exercising, but it must not be zero: with no room at all the
     * write path can never flush and so can never make progress. Clamp with an
     * explicit comparison rather than with FFMAX(), because that macro
     * evaluates its argument twice and would consume the input twice over.
     */
    cfg->io_buffer_size     = bytestream2_get_le32(gbc) & 0xFFFF;
    if (cfg->io_buffer_size < 64)
        cfg->io_buffer_size = 64;
    cfg->max_pkt_size       = 1 + (bytestream2_get_le16(gbc) & 0xFFF);

    bytestream2_get_buffer(gbc, cfg->recon_seed, sizeof(cfg->recon_seed));
}

/** The layout of layer @p idx in an element that has @p nb_layers layers. */
static const AVChannelLayout *layer_layout(const FuzzConfig *cfg, int idx,
                                           int nb_layers)
{
    const AVChannelLayout *chain;
    int len;

    if (nb_layers == 1)
        return &single_layer_layouts[cfg->single_layout];

    if (cfg->chain) {
        chain = scalable_chain_b;
        len   = (int)FF_ARRAY_ELEMS(scalable_chain_b);
    } else {
        chain = scalable_chain_a;
        len   = (int)FF_ARRAY_ELEMS(scalable_chain_a);
    }

    return &chain[FFMIN(idx, len - 1)];
}

/**
 * Give @p dst the channel layout of layer @p idx.
 *
 * The custom order form is the interesting one. A custom order layout may name
 * the same channel more than once, so every layer of the chain built here shows
 * the very same channel mask while carrying one channel more than the layer
 * before it. Matching a layer to a loudspeaker layout by channel mask alone
 * accepts all of them, which is how a chain of any length can be assembled and
 * how a layer ends up matched to a layout that carries fewer channels than the
 * layer really has. Requiring the channel count to agree as well is what turns
 * that into a rejection, and both answers are worth reaching.
 */
static int set_layer_layout(AVChannelLayout *dst, const FuzzConfig *cfg,
                            int idx, int nb_layers)
{
    int ret;

    if (!cfg->custom_layers)
        return av_channel_layout_copy(dst, layer_layout(cfg, idx, nb_layers));

    ret = av_channel_layout_custom_init(dst, 2 + idx);
    if (ret < 0)
        return ret;

    /* Front left, front right, and then front left over and over again. */
    for (int i = 0; i < dst->nb_channels; i++)
        dst->u.map[i].id = i == 1 ? AV_CHAN_FRONT_RIGHT : AV_CHAN_FRONT_LEFT;

    return 0;
}

/** The number of channels the layout of layer @p idx carries. */
static int layer_nb_channels(const FuzzConfig *cfg, int idx, int nb_layers)
{
    if (cfg->custom_layers)
        return 2 + idx;

    return layer_layout(cfg, idx, nb_layers)->nb_channels;
}

/**
 * Work out how the layers' channels are spread over substreams.
 *
 * Each layer adds a number of channels on top of its predecessor, and those
 * channels are carried by one or more substreams of one or two channels each.
 * Splitting every increment exactly is what a valid element looks like; the
 * extra and dropped substream knobs then break the accounting in the two ways
 * the writer has to notice, one substream too many and one too few.
 *
 * @return the number of substreams written to @p channels.
 */
static int plan_substreams(const FuzzConfig *cfg, uint8_t *channels, int max)
{
    int nb = 0, prev = 0;

    for (int i = 0; i < cfg->nb_layers && nb < max; i++) {
        int delta = layer_nb_channels(cfg, i, cfg->nb_layers) - prev;

        prev += delta;
        while (delta > 0 && nb < max) {
            int nb_channels = delta >= 2 && !cfg->mono_substreams ? 2 : 1;

            channels[nb++] = nb_channels;
            delta -= nb_channels;
        }
    }

    if (cfg->drop_substream && nb > 1)
        nb--;
    if (cfg->extra_substream && nb < max)
        channels[nb++] = 1;
    /* An audio element without a single stream is refused before it is read. */
    if (!nb)
        channels[nb++] = 1;

    return nb;
}

/**
 * An Opus identification header, in the form the muxer expects it.
 *
 * The IAMF codec configuration writer only accepts an Opus identification
 * header of exactly nineteen bytes, and byte swaps it into the big endian form
 * the specification asks for, so anything else is rejected outright. These same
 * nineteen bytes are what an empty packet later offers as replacement
 * extradata, which sends the writer through that conversion a second time.
 */
static const uint8_t opus_head[19] = {
    'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
    1,                      /* version */
    2,                      /* channel count */
    0x38, 0x01,             /* pre-skip, 312 */
    0x80, 0xBB, 0x00, 0x00, /* input sample rate, 48000 */
    0x00, 0x00,             /* output gain */
    0,                      /* channel mapping family */
};

/** Give a stream the OpusHead extradata, padded as the API requires. */
static int set_opus_extradata(AVCodecParameters *par)
{
    par->extradata = av_malloc(sizeof(opus_head) +
                               AV_INPUT_BUFFER_PADDING_SIZE);
    if (!par->extradata)
        return AVERROR(ENOMEM);

    memcpy(par->extradata, opus_head, sizeof(opus_head));
    memset(par->extradata + sizeof(opus_head), 0, AV_INPUT_BUFFER_PADDING_SIZE);
    par->extradata_size = sizeof(opus_head);

    return 0;
}

/**
 * Allocate a parameter definition that is handed to a stream group.
 *
 * The size of the object is of no interest here, because these definitions are
 * given to an audio element or a submix, which is what then owns them; only the
 * definitions that travel as packet side data have to be measured.
 */
static AVIAMFParamDefinition *alloc_param(enum AVIAMFParamDefinitionType type,
                                          unsigned nb_subblocks)
{
    return av_iamf_param_definition_alloc(type, nb_subblocks, NULL);
}


/**
 * Build one CHANNEL_BASED audio element stream group and its substreams.
 *
 * The stream group pre-allocates its AVIAMFAudioElement, so the object is
 * populated in place rather than replaced, and everything hung off it,
 * including the parameter definitions, is freed by avformat_free_context().
 *
 * @param index which audio element this is, used to keep ids apart
 */
static int add_audio_element(AVFormatContext *oc, const FuzzConfig *cfg,
                            int index)
{
    uint8_t substream_channels[32];
    AVIAMFAudioElement *ae;
    AVStreamGroup *stg;
    int nb_substreams, ret;

    stg = avformat_stream_group_create(
        oc, AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT, NULL);
    if (!stg)
        return AVERROR(ENOMEM);

    stg->id = index + 1;
    ae = stg->params.iamf_audio_element;
    ae->audio_element_type = AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL;
    ae->default_w = 10;

    for (int i = 0; i < cfg->nb_layers; i++) {
        AVIAMFLayer *layer = av_iamf_audio_element_add_layer(ae);

        if (!layer)
            return AVERROR(ENOMEM);

        ret = set_layer_layout(&layer->ch_layout, cfg, i, cfg->nb_layers);
        if (ret < 0)
            return ret;

        /*
         * The recon gain matrix is only read for layers carrying this flag, so
         * without it the loop over the layers spins without touching anything.
         */
        if (cfg->recon_gain_layers & (1u << FFMIN(i, 31)))
            layer->flags |= AV_IAMF_LAYER_FLAG_RECON_GAIN;
        layer->output_gain_flags = cfg->output_gain_flags;
    }

    /*
     * The demixing and recon gain parameters are mandatory for a scalable
     * element once it has more than one layer, so they are always provided in
     * that case and only optional for a single layer element.
     */
    if (cfg->with_demix_info || cfg->nb_layers > 1) {
        AVIAMFParamDefinition *demix;
        AVIAMFDemixingInfo *info;

        /* A demixing definition with anything but one subblock is refused. */
        demix = alloc_param(AV_IAMF_PARAMETER_DEFINITION_DEMIXING, 1);
        if (!demix)
            return AVERROR(ENOMEM);
        ae->demixing_info = demix;

        demix->parameter_id = cfg->demix_id + index;
        info = av_iamf_param_definition_get_subblock(demix, 0);
        info->dmixp_mode = cfg->dmixp_mode;
    }
    if (cfg->with_recon_info || cfg->nb_layers > 1) {
        AVIAMFParamDefinition *recon;

        recon = alloc_param(AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN, 1);
        if (!recon)
            return AVERROR(ENOMEM);
        ae->recon_gain_info = recon;

        recon->parameter_id = cfg->recon_id + index;
    }

    nb_substreams = plan_substreams(cfg, substream_channels,
                                    FF_ARRAY_ELEMS(substream_channels));

    for (int i = 0; i < nb_substreams; i++) {
        const AVChannelLayout *sub =
            &substream_layouts[substream_channels[i] - 1];
        AVStream *st = avformat_new_stream(oc, NULL);

        if (!st)
            return AVERROR(ENOMEM);

        /*
         * The substream id is taken from the stream id, and the muxer refuses
         * duplicates, so every element gets its own block of ids. Element zero
         * starts at zero, which also makes its first stream the one the muxer
         * accepts parameter blocks on.
         */
        st->id = index * 64 + i;
        st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id    = AV_CODEC_ID_OPUS;
        st->codecpar->codec_tag   = MKTAG('O', 'p', 'u', 's');
        st->codecpar->sample_rate = cfg->sample_rate;
        st->codecpar->frame_size  = cfg->frame_size;

        ret = av_channel_layout_copy(&st->codecpar->ch_layout, sub);
        if (ret < 0)
            return ret;

        if (cfg->with_extradata) {
            ret = set_opus_extradata(st->codecpar);
            if (ret < 0)
                return ret;
        }

        ret = avformat_stream_group_add_stream(stg, st);
        if (ret < 0)
            return ret;
    }

    return 0;
}

/**
 * Build the mix presentation stream group that references the audio elements.
 *
 * A submix element's mix configuration is not allocated for us, and the muxer
 * refuses a submix whose configurations are missing, so both are allocated
 * here; the mix presentation takes ownership of them.
 */
static int add_mix_presentation(AVFormatContext *oc, const FuzzConfig *cfg)
{
    AVIAMFMixPresentation *mix;
    AVIAMFSubmixLayout *layout;
    AVIAMFSubmix *submix;
    AVStreamGroup *stg;
    int ret;

    stg = avformat_stream_group_create(
        oc, AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION, NULL);
    if (!stg)
        return AVERROR(ENOMEM);

    stg->id = cfg->nb_elements + 1;
    mix = stg->params.iamf_mix_presentation;

    submix = av_iamf_mix_presentation_add_submix(mix);
    if (!submix)
        return AVERROR(ENOMEM);

    submix->output_mix_config =
        alloc_param(AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN, 0);
    if (!submix->output_mix_config)
        return AVERROR(ENOMEM);

    /*
     * A definition that does not belong to an audio element has no codec
     * configuration to take a rate from, so the rate has to be set here or the
     * mix presentation is refused.
     */
    submix->output_mix_config->parameter_id   = cfg->mix_id;
    submix->output_mix_config->parameter_rate = cfg->sample_rate;

    for (int i = 0; i < cfg->nb_elements; i++) {
        AVIAMFSubmixElement *element = av_iamf_submix_add_element(submix);

        if (!element)
            return AVERROR(ENOMEM);

        element->element_mix_config =
            alloc_param(AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN, 0);
        if (!element->element_mix_config)
            return AVERROR(ENOMEM);

        element->element_mix_config->parameter_id   = cfg->mix_id;
        element->element_mix_config->parameter_rate = cfg->sample_rate;

        /*
         * A submix may name an audio element that was never declared. The
         * writer has to resolve that reference while serializing the mix
         * presentation and report it, so both the resolvable and the dangling
         * case are worth producing.
         */
        element->audio_element_id = cfg->dangling_element ? UINT_MAX - i
                                                          : (unsigned)(i + 1);
        element->headphones_rendering_mode = cfg->binaural_rendering ?
                                             AV_IAMF_HEADPHONES_MODE_BINAURAL :
                                             AV_IAMF_HEADPHONES_MODE_STEREO;
    }

    layout = av_iamf_submix_add_layout(submix);
    if (!layout)
        return AVERROR(ENOMEM);

    if (cfg->binaural_layout)
        layout->layout_type = AV_IAMF_SUBMIX_LAYOUT_TYPE_BINAURAL;
    else {
        layout->layout_type = AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS;
        /* Stereo is Sound System A, so it always resolves. */
        ret = av_channel_layout_copy(&layout->sound_system,
                                     &substream_layouts[1]);
        if (ret < 0)
            return ret;
    }

    /* A mix presentation covers every stream the audio elements contributed. */
    for (unsigned i = 0; i < oc->nb_streams; i++) {
        ret = avformat_stream_group_add_stream(stg, oc->streams[i]);
        if (ret < 0)
            return ret;
    }

    return 0;
}

/**
 * Build a parameter block to hand to the muxer as packet side data.
 *
 * The subblock count is whatever was allocated, so the writer's loop over the
 * subblocks always stays inside the allocation. The recon gain matrix is filled
 * from the input so that some entries are zero and some are not, which is what
 * decides both the flags the writer computes and the bytes it emits.
 *
 * @param size the size in bytes of the returned object, for the side data
 * @return the parameter block, to be freed by the caller with av_free()
 */
static AVIAMFParamDefinition *param_block(enum AVIAMFParamDefinitionType type,
                                          const FuzzConfig *cfg, unsigned id,
                                          size_t *size)
{
    AVIAMFParamDefinition *param =
        av_iamf_param_definition_alloc(type, cfg->nb_subblocks, size);

    if (!param)
        return NULL;

    param->parameter_id   = id;
    param->parameter_rate = cfg->sample_rate;
    param->duration       = cfg->frame_size;
    param->constant_subblock_duration = cfg->frame_size;

    for (int i = 0; i < cfg->nb_subblocks; i++) {
        void *subblock = av_iamf_param_definition_get_subblock(param, i);

        switch (type) {
        case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN: {
            AVIAMFMixGain *gain = subblock;

            gain->subblock_duration = cfg->frame_size;
            gain->animation_type    = cfg->recon_seed[i & 7] % 3;
            gain->start_point_value = av_make_q(cfg->recon_seed[0], 1 << 8);
            gain->end_point_value   = av_make_q(cfg->recon_seed[1], 1 << 8);
            gain->control_point_value = av_make_q(cfg->recon_seed[2], 1 << 8);
            gain->control_point_relative_time = av_make_q(cfg->recon_seed[3],
                                                          1 << 8);
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_DEMIXING: {
            AVIAMFDemixingInfo *demix = subblock;

            demix->subblock_duration = cfg->frame_size;
            demix->dmixp_mode        = cfg->dmixp_mode;
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN: {
            AVIAMFReconGain *recon = subblock;

            recon->subblock_duration = cfg->frame_size;
            for (int j = 0; j < (int)FF_ARRAY_ELEMS(recon->recon_gain); j++)
                for (int k = 0; k < (int)FF_ARRAY_ELEMS(recon->recon_gain[0]);
                     k++)
                    recon->recon_gain[j][k] =
                        cfg->recon_seed[(j * 12 + k + i) & 7];
            break;
        }
        }
    }

    return param;
}

/** Copy a parameter block onto a packet as side data of the matching type. */
static int attach_param_block(AVPacket *pkt, enum AVPacketSideDataType type,
                              const AVIAMFParamDefinition *param, size_t size)
{
    uint8_t *side_data;

    if (!param)
        return 0;

    side_data = av_packet_new_side_data(pkt, type, size);
    if (!side_data)
        return AVERROR(ENOMEM);

    memcpy(side_data, param, size);

    return 0;
}

/**
 * Mux one session described by @p data.
 *
 * The trailing bytes of the input, if there are enough of them, describe the
 * scenario; everything before them becomes the payload of the packets. The
 * sequence is always the same, and mirrors what any application does: allocate
 * the output context, declare the streams and the stream groups, write the
 * header, write packets, write the trailer.
 *
 * Every one of those steps may fail, and for most scenarios one of them will,
 * because that is precisely what the muxer's validation is for. A failure is
 * therefore the ordinary outcome and is never reported: the return value is
 * always zero, and the process is only ever ended by a genuine allocation
 * failure. What a run is looking for is a sanitizer report or a crash.
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    AVIAMFParamDefinition *recon_block = NULL, *demix_block = NULL;
    AVIAMFParamDefinition *mix_block = NULL;
    size_t recon_size = 0, demix_size = 0, mix_size = 0;
    const AVOutputFormat *ofmt = NULL;
    AVFormatContext *oc = NULL;
    AVIOContext *fuzzed_pb;
    AVPacket *pkt = NULL;
    IOContext opaque = { 0 };
    const uint8_t *end;
    uint8_t *io_buffer;
    FuzzConfig cfg;
    static int c;
    int ret;

#ifdef FFMPEG_MUXER
#define MUXER_SYMBOL0(MUXER) ff_##MUXER##_muxer
#define MUXER_SYMBOL(MUXER) MUXER_SYMBOL0(MUXER)
    extern const FFOutputFormat MUXER_SYMBOL(FFMPEG_MUXER);
    ofmt = &MUXER_SYMBOL(FFMPEG_MUXER).p;
#else
    /*
     * Nothing names a muxer at build time, so the one this target was written
     * around is looked up through the public API instead. Doing it at run time
     * is what keeps the object free of any build time dependency on that muxer
     * being enabled, which matters because a configuration without it simply
     * has nothing here to test.
     */
    ofmt = av_guess_format("iamf", NULL, NULL);
#endif

    if (!c) {
        av_log_set_level(AV_LOG_PANIC);
        c = 1;
    }

    /* Not built with the muxer, so there is nothing to test and no finding. */
    if (!ofmt)
        return 0;

    config_defaults(&cfg);
    if (size > 1024) {
        GetByteContext gbc;

        size -= 1024;
        bytestream2_init(&gbc, data + size, 1024);
        config_parse(&cfg, &gbc);
    }
    end = data + size;

    ret = avformat_alloc_output_context2(&oc, ofmt, NULL, NULL);
    if (ret == AVERROR(ENOMEM))
        error("Failed avformat_alloc_output_context2()");
    if (ret < 0 || !oc)
        goto fail;

    pkt = av_packet_alloc();
    if (!pkt)
        error("Failed to allocate pkt");

    io_buffer = av_malloc(cfg.io_buffer_size);
    if (!io_buffer)
        error("Failed to allocate io_buffer");

    /*
     * The output is a sink that counts bytes and throws them away: a fuzz
     * target must not touch the file system, and nothing here needs to read
     * back what was written. The third argument marks the context writable and
     * the callback goes in the write slot, which is what makes the muxer use
     * it. A seek callback is supplied only for some of the scenarios, because
     * the muxer takes a different route through the trailer depending on
     * whether the output can be seeked.
     */
    fuzzed_pb = avio_alloc_context(io_buffer, cfg.io_buffer_size, 1, &opaque,
                                   NULL, io_write,
                                   cfg.seekable ? io_seek : NULL);
    if (!fuzzed_pb) {
        av_free(io_buffer);
        error("avio_alloc_context failed");
    }
    oc->pb = fuzzed_pb;

    /*
     * Both kinds of stream group are needed. The muxer refuses anything with
     * fewer than two groups, and it wants one or two audio elements together
     * with at least one mix presentation, so a session that declares only an
     * audio element never reaches the element validation at all.
     */
    for (int i = 0; i < cfg.nb_elements; i++) {
        ret = add_audio_element(oc, &cfg, i);
        if (ret < 0)
            goto fail;
    }
    ret = add_mix_presentation(oc, &cfg);
    if (ret < 0)
        goto fail;

    /*
     * Everything the writer decides about the layer count, the channel layouts
     * of the layers, the substream accounting and the references a submix makes
     * is reported from here. An error is the normal answer for most scenarios
     * and means nothing more than that the input was refused, which is the
     * whole point of the validation being there.
     */
    ret = avformat_write_header(oc, NULL);
    if (ret < 0)
        goto fail;

    /* A header cannot have been written without a stream to write it for. */
    if (!oc->nb_streams)
        goto fail;

    /*
     * The parameter blocks travel as packet side data, and the writer resolves
     * each one against the definitions the descriptors declared. The recon gain
     * block is the interesting one: its matrix is indexed once per layer, so it
     * is what measures the layer count against the capacity of that matrix.
     */
    if (cfg.side_data & 1)
        recon_block = param_block(AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN,
                                  &cfg, cfg.recon_id, &recon_size);
    if (cfg.side_data & 2)
        demix_block = param_block(AV_IAMF_PARAMETER_DEFINITION_DEMIXING,
                                  &cfg, cfg.demix_id, &demix_size);
    if (cfg.side_data & 4)
        mix_block = param_block(AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN,
                                &cfg, cfg.mix_id, &mix_size);

    for (uint32_t it = 0; it < maxiteration && data < end; it++) {
        size_t left = end - data;
        int pkt_size = left < (size_t)cfg.max_pkt_size ? (int)left
                                                       : cfg.max_pkt_size;

        ret = av_new_packet(pkt, pkt_size);
        if (ret < 0)
            error("Failed to allocate packet payload");

        memcpy(pkt->data, data, pkt_size);
        data += pkt_size;

        /*
         * Only the substream that comes first carries parameter blocks, and
         * that is the first stream of the first audio element, so a round robin
         * over the streams reaches both the substream that takes them and the
         * ones that do not.
         */
        pkt->stream_index = it % oc->nb_streams;
        pkt->duration     = FFMAX(cfg.frame_size, 1);
        pkt->pts          = pkt->dts = (int64_t)(it / oc->nb_streams) *
                                       pkt->duration;
        pkt->time_base    = av_make_q(1, cfg.sample_rate);
        pkt->flags       |= AV_PKT_FLAG_KEY;

        if (attach_param_block(pkt, AV_PKT_DATA_IAMF_RECON_GAIN_INFO_PARAM,
                               recon_block, recon_size) < 0 ||
            attach_param_block(pkt, AV_PKT_DATA_IAMF_DEMIXING_INFO_PARAM,
                               demix_block, demix_size) < 0 ||
            attach_param_block(pkt, AV_PKT_DATA_IAMF_MIX_GAIN_PARAM,
                               mix_block, mix_size) < 0)
            error("Failed to allocate packet side data");

        /* The call takes the packet, on success and on failure alike. */
        if (av_interleaved_write_frame(oc, pkt) < 0)
            break;
    }

    /*
     * An empty packet carrying new extradata makes the muxer replace the codec
     * configuration of the substream it names and, when the output can be
     * seeked, rewrite the descriptors from the trailer. That is a second pass
     * over the whole descriptor serializer, with a configuration that was built
     * while packets were already being written.
     */
    if (cfg.new_extradata) {
        uint8_t *side_data;

        av_packet_unref(pkt);
        pkt->stream_index = 0;
        pkt->duration     = FFMAX(cfg.frame_size, 1);
        /* Past every timestamp written above, so the order still holds. */
        pkt->pts          = pkt->dts = (int64_t)maxiteration * pkt->duration;
        pkt->time_base    = av_make_q(1, cfg.sample_rate);
        side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                            sizeof(opus_head));
        if (!side_data)
            error("Failed to allocate new extradata side data");

        memcpy(side_data, opus_head, sizeof(opus_head));
        av_interleaved_write_frame(oc, pkt);
    }

    /* Flushes what interleaving still holds and finishes the descriptors. */
    av_write_trailer(oc);

fail:
    av_packet_free(&pkt);
    /*
     * The parameter blocks handed over as side data were copied, so these are
     * still owned here. Everything reachable from a stream group, including the
     * parameter definitions and the mix configurations, belongs to the format
     * context and must not be freed here.
     */
    av_free(recon_block);
    av_free(demix_block);
    av_free(mix_block);
    /*
     * An AVIOContext does not take ownership of the buffer it was given, so the
     * buffer is released first and the context afterwards, which also clears
     * the pointer the format context holds before that context is torn down.
     */
    if (oc && oc->pb) {
        av_freep(&oc->pb->buffer);
        avio_context_free(&oc->pb);
    }
    avformat_free_context(oc);

    return 0;
}
