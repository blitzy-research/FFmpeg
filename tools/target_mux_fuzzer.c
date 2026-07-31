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

#include "config.h"
#include "libavutil/channel_layout.h"
#include "libavutil/iamf.h"
#include "libavutil/mem.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/bytestream.h"
#include "libavformat/avformat.h"

/*
 * Muxer fuzzing target: the input becomes an output context pushed through the
 * public muxing API into a discarding AVIOContext. IAMF is driven because its
 * writer reconciles an element's layer count and layouts against the substream
 * count, recon_gain's extent and the fields these are serialized into. Only
 * Opus is used, as recon gain is stripped for fLaC and ipcm.
 *
 * The input's leading CONTROL_BLOCK_SIZE bytes are the scenario, one knob per
 * byte and every one of them optional, and the input in full is the payload the
 * packets carry; see config_parse(). A single byte already selects a scenario,
 * so no seed corpus is needed to reach them: the decoder, encoder and bitstream
 * filter targets keep their controls past a size threshold instead, which suits
 * them because their leading bytes are a coded bitstream that earns coverage on
 * its own and so grows an input up to that threshold. A muxer copies a packet's
 * bytes out verbatim, so here nothing below a threshold could earn any, and the
 * controls behind one would stay unreachable however long a campaign ran.
 *
 * A libFuzzer toolchain is what links this target:
 *   ./configure --toolchain=clang-asan-fuzz --assert-level=2 --enable-gpl \
 *               --enable-nonfree --enable-memory-poisoning
 *   make tools/target_mux_fuzzer
 * Any toolchain whose sanitizer list holds fuzz works, as does --libfuzzer=PATH
 * on its own. --enable-ossfuzz is not one of them and must not stand in for
 * them: it leaves LIBFUZZER_PATH empty, so the link fails on an undefined main,
 * and it stubs out the codec list, leaving a binary with next to no encoders.
 */

/** Byte sink for the muxer's output; dropping the bytes keeps this hermetic. */
typedef struct IOContext {
    int64_t pos;
    int64_t filesize;
} IOContext;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/** Fail the run on a harness allocation failure; a muxing error is not one. */
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
 * Nested layouts for scalable elements: each layer must be a mask superset of
 * its predecessor with more channels, waived after a Mono layer, which lets
 * chain c span six -- the declarable maximum and recon_gain's extent. Past a
 * chain's length the last layout repeats, adding no channels.
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

static const AVChannelLayout scalable_chain_c[6] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
    AV_CHANNEL_LAYOUT_3POINT1POINT2,
    AV_CHANNEL_LAYOUT_5POINT1POINT2,
    AV_CHANNEL_LAYOUT_5POINT1POINT4_BACK,
    AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK,
};

static const struct {
    const AVChannelLayout *layouts;
    int nb_layouts;
} scalable_chains[3] = {
    { scalable_chain_a, FF_ARRAY_ELEMS(scalable_chain_a) },
    { scalable_chain_b, FF_ARRAY_ELEMS(scalable_chain_b) },
    { scalable_chain_c, FF_ARRAY_ELEMS(scalable_chain_c) },
};

/**
 * Layouts for single-layer elements, which carry no ordering constraint and so
 * may use the expanded loudspeaker layouts too: SURROUND is the three-channel
 * one, the anonymous ones low-frequency-effects-only (expanded layout 0),
 * Ls/Rs, Lrs/Rrs, Ltf/Rtf and the four top channels.
 */
static const AVChannelLayout single_layer_layouts[] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
    AV_CHANNEL_LAYOUT_5POINT1,
    AV_CHANNEL_LAYOUT_3POINT1POINT2,
    AV_CHANNEL_LAYOUT_BINAURAL,
    AV_CHANNEL_LAYOUT_SURROUND,
    {
        .nb_channels = 1,
        .order       = AV_CHANNEL_ORDER_NATIVE,
        .u.mask      = AV_CH_LOW_FREQUENCY,
    },
    {
        .nb_channels = 2,
        .order       = AV_CHANNEL_ORDER_NATIVE,
        .u.mask      = AV_CH_SIDE_LEFT | AV_CH_SIDE_RIGHT,
    },
    {
        .nb_channels = 2,
        .order       = AV_CHANNEL_ORDER_NATIVE,
        .u.mask      = AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT,
    },
    {
        .nb_channels = 2,
        .order       = AV_CHANNEL_ORDER_NATIVE,
        .u.mask      = AV_CH_TOP_FRONT_LEFT | AV_CH_TOP_FRONT_RIGHT,
    },
    {
        .nb_channels = 4,
        .order       = AV_CHANNEL_ORDER_NATIVE,
        .u.mask      = AV_CH_TOP_FRONT_LEFT | AV_CH_TOP_FRONT_RIGHT |
                       AV_CH_TOP_BACK_LEFT  | AV_CH_TOP_BACK_RIGHT,
    },
    AV_CHANNEL_LAYOUT_9POINT1POINT6,
};

/**
 * A SCENE_BASED element's layer must be ambisonic or custom-order, and first
 * order is the smallest such layout: four channels over four mono substreams,
 * a scene element admitting no coupled one.
 */
static const AVChannelLayout ambisonic_layout =
    AV_CHANNEL_LAYOUT_AMBISONIC_FIRST_ORDER;

/** Channel layouts a substream may have, indexed by channel count minus one. */
static const AVChannelLayout substream_layouts[2] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
};

static const int sample_rates[4] = { 48000, 44100, 16000, 96000 };

/**
 * Leading bytes of the input config_parse() can consume: the whole scenario
 * space fits in this one flat block, so no input has to be longer to reach any
 * part of it, and a shorter one reaches the block's leading knobs.
 */
#define CONTROL_BLOCK_SIZE 29

/** A muxing scenario: fuzz-derived controls, each with a default. */
typedef struct FuzzConfig {
    int nb_elements;        /**< audio element stream groups, 1 or 2 */
    int nb_layers;          /**< layers per CHANNEL_BASED element, 1 to 8 */
    int scene_element;
    int scene_layers;       /**< layers per SCENE_BASED element, 0 to 2 */
    int chain;              /**< index into scalable_chains */
    int custom_layers;
    int single_layout;      /**< index into single_layer_layouts */
    int mono_substreams;
    int extra_substream;
    int drop_substream;
    unsigned recon_gain_layers; /**< bitmask of layers flagged for recon gain */
    unsigned output_gain_flags; /**< output gain flags set on every layer */
    int with_extradata;
    int with_recon_info;
    int with_demix_info;
    int dmixp_mode;         /**< demixing mode written into the descriptor */
    int dangling_element;
    int binaural_layout;
    int binaural_rendering;
    int seekable;
    int new_extradata;
    unsigned side_data;     /**< which parameter blocks to attach to packets */
    int nb_subblocks;
    int sample_rate;
    int frame_size;
    unsigned mix_id;
    unsigned demix_id;
    unsigned recon_id;
    int io_buffer_size;
    int max_pkt_size;
    int grow_layers;        /**< layers appended after the header, 0 to 3 */
    uint8_t recon_seed[8];  /**< expanded into the recon gain matrix */
} FuzzConfig;

static void config_defaults(FuzzConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* A two layer scalable element with recon gain, and a block per packet. */
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

/**
 * Take the control block's next byte into @p val, or report that the block ran
 * out so the caller stops and every knob it did not reach keeps its default.
 */
static int config_byte(GetByteContext *gbc, unsigned *val)
{
    if (bytestream2_get_bytes_left(gbc) < 1)
        return 0;

    *val = bytestream2_get_byte(gbc);

    return 1;
}

/** As config_byte(), for the knobs that want more than a byte of range. */
static int config_le16(GetByteContext *gbc, unsigned *val)
{
    if (bytestream2_get_bytes_left(gbc) < 2)
        return 0;

    *val = bytestream2_get_le16(gbc);

    return 1;
}

/**
 * Derive a scenario from the control block, at most one knob per byte and in a
 * fixed order, so a block of n bytes refines the default scenario in its first
 * n knobs and leaves the rest alone. Every added byte therefore reaches one
 * more knob, which is the gradient libFuzzer needs to grow an input by itself.
 */
static void config_parse(FuzzConfig *cfg, GetByteContext *gbc)
{
    unsigned flags1 = 0, flags3 = 0, v = 0;

    if (!config_byte(gbc, &flags1))
        return;

    cfg->nb_elements        = 1 + !!(flags1 & 0x01);
    /* Three chains need two bits and flags1 has one to give; the other comes
     * from flags3 below, until which this one selects on its own. */
    cfg->chain              = !!(flags1 & 0x02);
    cfg->mono_substreams    = !!(flags1 & 0x04);
    cfg->extra_substream    = !!(flags1 & 0x08);
    cfg->drop_substream     = !!(flags1 & 0x10);
    cfg->dangling_element   = !!(flags1 & 0x20);
    cfg->binaural_layout    = !!(flags1 & 0x40);
    cfg->seekable           = !!(flags1 & 0x80);

    if (!config_byte(gbc, &v))
        return;

    cfg->with_extradata     = !!(v & 0x01);
    cfg->with_recon_info    = !!(v & 0x02);
    cfg->with_demix_info    = !!(v & 0x04);
    cfg->new_extradata      = !!(v & 0x08);
    cfg->binaural_rendering = !!(v & 0x10);
    cfg->side_data          = (v >> 5) & 7;

    if (!config_byte(gbc, &flags3))
        return;

    cfg->custom_layers      = !!(flags3 & 0x01);
    cfg->scene_element      = !!(flags3 & 0x02);
    cfg->chain              = ((!!(flags3 & 0x04) << 1) | cfg->chain) %
                              FF_ARRAY_ELEMS(scalable_chains);

    if (!config_byte(gbc, &v))
        return;

    /* Seven is one past recon_gain's extent and eight the first value the three
     * bit num_layers field cannot hold, so both capacities stay reachable. */
    cfg->nb_layers          = 1 + v % 8;

    if (!config_byte(gbc, &v))
        return;

    /* A scene element may have only one layer; none must be caught early. */
    cfg->scene_layers       = v % 3;

    if (!config_byte(gbc, &v))
        return;

    cfg->single_layout      = v % FF_ARRAY_ELEMS(single_layer_layouts);

    if (!config_byte(gbc, &v))
        return;

    cfg->nb_subblocks       = 1 + v % 3;

    if (!config_byte(gbc, &v))
        return;

    cfg->dmixp_mode         = v & 7;

    if (!config_byte(gbc, &v))
        return;

    cfg->output_gain_flags  = v & 0x3F;

    if (!config_byte(gbc, &v))
        return;

    /* Bit 0 is forced, so layer 0 stays eligible for recon gain coverage. */
    cfg->recon_gain_layers  = v | 1;

    if (!config_byte(gbc, &v))
        return;

    cfg->sample_rate        = sample_rates[v % FF_ARRAY_ELEMS(sample_rates)];

    if (!config_le16(gbc, &v))
        return;

    cfg->frame_size         = v & 0xFFF;

    /* A small range makes element and mix ids collide often, so the writer
     * resolves a packet's block against a definition of another type. */
    if (!config_byte(gbc, &v))
        return;

    cfg->mix_id             = v;

    if (!config_byte(gbc, &v))
        return;

    cfg->demix_id           = v;

    if (!config_byte(gbc, &v))
        return;

    cfg->recon_id           = v;

    /* A small AVIO buffer is worth exercising but must not be zero, or the
     * write path can never flush. */
    if (!config_le16(gbc, &v))
        return;

    cfg->io_buffer_size     = FFMAX((int)v, 64);

    if (!config_le16(gbc, &v))
        return;

    cfg->max_pkt_size       = 1 + (v & 0xFFF);

    for (int i = 0; i < (int)FF_ARRAY_ELEMS(cfg->recon_seed); i++) {
        if (!config_byte(gbc, &v))
            return;

        cfg->recon_seed[i] = v;
    }

    if (!config_byte(gbc, &v))
        return;

    cfg->grow_layers        = v % 4;
}

static int element_nb_layers(const FuzzConfig *cfg)
{
    return cfg->scene_element ? cfg->scene_layers : cfg->nb_layers;
}

/** The layout of layer @p idx; past a chain's length the last one repeats. */
static const AVChannelLayout *layer_layout(const FuzzConfig *cfg, int idx,
                                           int nb_layers)
{
    const AVChannelLayout *chain;
    int len;

    /* A scene element's layer describes Ambisonics, which the chains do not. */
    if (cfg->scene_element)
        return &ambisonic_layout;

    if (nb_layers == 1)
        return &single_layer_layouts[cfg->single_layout];

    chain = scalable_chains[cfg->chain].layouts;
    len   = scalable_chains[cfg->chain].nb_layouts;

    return &chain[FFMIN(idx, len - 1)];
}

/**
 * Give @p dst the layout of layer @p idx. The custom form names one channel
 * repeatedly, so all layers share a mask while each adds a channel.
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

    for (int i = 0; i < dst->nb_channels; i++)
        dst->u.map[i].id = i == 1 ? AV_CHAN_FRONT_RIGHT : AV_CHAN_FRONT_LEFT;

    return 0;
}

static int layer_nb_channels(const FuzzConfig *cfg, int idx, int nb_layers)
{
    if (cfg->custom_layers)
        return 2 + idx;

    return layer_layout(cfg, idx, nb_layers)->nb_channels;
}

/**
 * Spread the layers' channels over substreams of one or two channels, each
 * layer adding to its predecessor; the extra and dropped knobs break that
 * accounting by one either way. An element with no layer still gets one, as a
 * group without a stream is refused first. Returns the count written.
 */
static int plan_substreams(const FuzzConfig *cfg, int nb_layers,
                           uint8_t *channels, int max)
{
    int nb = 0, prev = 0;

    for (int i = 0; i < nb_layers && nb < max; i++) {
        int delta = layer_nb_channels(cfg, i, nb_layers) - prev;

        prev += delta;
        while (delta > 0 && nb < max) {
            /* A scene element's substreams are mono: wider ones are refused. */
            int nb_channels = delta >= 2 && !cfg->mono_substreams &&
                              !cfg->scene_element ? 2 : 1;

            channels[nb++] = nb_channels;
            delta -= nb_channels;
        }
    }

    if (cfg->drop_substream && nb > 1)
        nb--;
    if (cfg->extra_substream && nb < max)
        channels[nb++] = 1;
    if (!nb)
        channels[nb++] = 1;

    return nb;
}

/* An Opus identification header: the codec configuration writer accepts exactly
 * nineteen bytes and byte swaps them into big-endian form. */
static const uint8_t opus_head[19] = {
    'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
    1,                      /* version */
    2,                      /* channel count */
    0x38, 0x01,             /* pre-skip, 312 */
    0x80, 0xBB, 0x00, 0x00, /* input sample rate, 48000 */
    0x00, 0x00,             /* output gain */
    0,                      /* channel mapping family */
};

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

/** A parameter definition owned by the element or submix it is given to. */
static AVIAMFParamDefinition *alloc_param(enum AVIAMFParamDefinitionType type,
                                          unsigned nb_subblocks)
{
    return av_iamf_param_definition_alloc(type, nb_subblocks, NULL);
}

/**
 * Build one audio element stream group and its substreams. The group
 * pre-allocates its AVIAMFAudioElement, so it is filled in place and freed with
 * the format context. @p pae, when given, hands that element back.
 */
static int add_audio_element(AVFormatContext *oc, const FuzzConfig *cfg,
                             int index, AVIAMFAudioElement **pae)
{
    uint8_t substream_channels[32];
    const int nb_layers = element_nb_layers(cfg);
    AVIAMFAudioElement *ae;
    AVStreamGroup *stg;
    int nb_substreams, ret;

    stg = avformat_stream_group_create(
        oc, AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT, NULL);
    if (!stg)
        return AVERROR(ENOMEM);

    stg->id = index + 1;
    ae = stg->params.iamf_audio_element;
    if (pae)
        *pae = ae;
    ae->audio_element_type = cfg->scene_element ?
                             AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE :
                             AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL;
    ae->default_w = 10;

    for (int i = 0; i < nb_layers; i++) {
        AVIAMFLayer *layer = av_iamf_audio_element_add_layer(ae);

        if (!layer)
            return AVERROR(ENOMEM);

        ret = set_layer_layout(&layer->ch_layout, cfg, i, nb_layers);
        if (ret < 0)
            return ret;

        /* The matrix is only read for layers carrying this flag. */
        if (cfg->recon_gain_layers & (1u << FFMIN(i, 31)))
            layer->flags |= AV_IAMF_LAYER_FLAG_RECON_GAIN;
        layer->output_gain_flags = cfg->output_gain_flags;
    }

    /* Recon gain is mandatory past one layer unless the codec is fLaC or ipcm,
     * demixing only for the higher layouts; both are always given anyway. */
    if (cfg->with_demix_info || nb_layers > 1) {
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
    if (cfg->with_recon_info || nb_layers > 1) {
        AVIAMFParamDefinition *recon;

        recon = alloc_param(AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN, 1);
        if (!recon)
            return AVERROR(ENOMEM);
        ae->recon_gain_info = recon;

        recon->parameter_id = cfg->recon_id + index;
    }

    nb_substreams = plan_substreams(cfg, nb_layers, substream_channels,
                                    FF_ARRAY_ELEMS(substream_channels));

    for (int i = 0; i < nb_substreams; i++) {
        const AVChannelLayout *sub =
            &substream_layouts[substream_channels[i] - 1];
        AVStream *st = avformat_new_stream(oc, NULL);

        if (!st)
            return AVERROR(ENOMEM);

        /* The substream id comes from the stream id and duplicates are refused,
         * so each element gets its own block; element zero starts at zero, so
         * its first stream is the one parameter blocks are accepted on. */
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
 * Build the mix presentation stream group referencing the audio elements. Mix
 * configurations are not pre-allocated and a submix missing one is refused, so
 * both are allocated here, owned by the mix presentation.
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

    /* A definition outside an audio element has no codec configuration to take
     * a rate from, so it must be set here or the presentation is refused. */
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

        /* An id matching no element exercises the submix reference check. */
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

    for (unsigned i = 0; i < oc->nb_streams; i++) {
        ret = avformat_stream_group_add_stream(stg, oc->streams[i]);
        if (ret < 0)
            return ret;
    }

    return 0;
}

/**
 * Build a parameter block for packet side data; @p size receives its size and
 * the caller av_free()s it. The subblock count is whatever was allocated, so
 * the writer's loop stays inside it, and the recon gain matrix is filled from
 * the input so some entries are zero, which decides the flags and the bytes.
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

/**
 * Copy a parameter block onto a packet as side data, in an exactly sized buffer
 * rather than av_packet_new_side_data()'s padded one, so an overread lands in
 * a sanitizer's redzone.
 */
static int attach_param_block(AVPacket *pkt, enum AVPacketSideDataType type,
                              const AVIAMFParamDefinition *param, size_t size)
{
    uint8_t *side_data;
    int ret;

    if (!param)
        return 0;

    side_data = av_memdup(param, size);
    if (!side_data)
        return AVERROR(ENOMEM);

    ret = av_packet_add_side_data(pkt, type, side_data, size);
    if (ret < 0)
        av_free(side_data);

    return ret;
}

/**
 * Mux one session described by @p data: its leading bytes carry the scenario,
 * as far as they reach, and the input in full is the packet payload. Most
 * scenarios are refused somewhere, which is ordinary and not reported.
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    AVIAMFParamDefinition *recon_block = NULL, *demix_block = NULL;
    AVIAMFParamDefinition *mix_block = NULL;
    size_t recon_size = 0, demix_size = 0, mix_size = 0;
    const AVOutputFormat *ofmt = NULL;
    AVIAMFAudioElement *element = NULL;
    AVFormatContext *oc = NULL;
    AVIOContext *fuzzed_pb;
    AVPacket *pkt = NULL;
    IOContext opaque = { 0 };
    const uint8_t *end;
    uint8_t *io_buffer;
    GetByteContext gbc;
    FuzzConfig cfg;
    static int c;
    int ret;

    /* A runtime lookup keeps the object usable without the muxer enabled. */
    ofmt = av_guess_format("iamf", NULL, NULL);

    if (!c) {
        av_log_set_level(AV_LOG_PANIC);
        c = 1;
    }

    if (!ofmt)
        return 0;

    /* The control block is the input's leading bytes, however few there are, so
     * every input steers the scenario and none has to reach a length first. The
     * bytes are payload as well: a muxer copies a packet's bytes out untouched,
     * so which ones they are decides nothing and withholding them from the
     * payload would only keep short inputs from reaching the write path. */
    config_defaults(&cfg);
    bytestream2_init(&gbc, data, (int)FFMIN(size, (size_t)CONTROL_BLOCK_SIZE));
    config_parse(&cfg, &gbc);

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

    /* The third argument marks the context writable. A seek callback is given
     * only sometimes, as the muxer's trailer path differs when seekable. */
    fuzzed_pb = avio_alloc_context(io_buffer, cfg.io_buffer_size, 1, &opaque,
                                   NULL, io_write,
                                   cfg.seekable ? io_seek : NULL);
    if (!fuzzed_pb) {
        av_free(io_buffer);
        error("avio_alloc_context failed");
    }
    oc->pb = fuzzed_pb;

    /* Fewer than two groups is refused, and one or two audio elements with a
     * mix presentation are wanted, so a session declaring only an audio element
     * never reaches the element validation at all. */
    for (int i = 0; i < cfg.nb_elements; i++) {
        ret = add_audio_element(oc, &cfg, i, i ? NULL : &element);
        if (ret < 0)
            goto fail;
    }
    ret = add_mix_presentation(oc, &cfg);
    if (ret < 0)
        goto fail;

    ret = avformat_write_header(oc, NULL);
    if (ret < 0)
        goto fail;

    if (!oc->nb_streams)
        goto fail;

    /* Append layers once the muxer has sized its own per-layer array from the
     * count it validated. Anything reading the count from the element rather
     * than from that extent indexes past the array and exceeds the three-bit
     * num_layers field, both of which the trailer serializes again. */
    for (int i = 0; element && i < cfg.grow_layers; i++) {
        AVIAMFLayer *layer = av_iamf_audio_element_add_layer(element);

        if (!layer)
            error("Failed to allocate a grown layer");
        if (set_layer_layout(&layer->ch_layout, &cfg, element->nb_layers - 1,
                             element_nb_layers(&cfg)) < 0)
            error("Failed to set a grown layer's channel layout");
        layer->flags |= AV_IAMF_LAYER_FLAG_RECON_GAIN;
    }

    /* Recon gain side data is what makes the writer index its matrix. */
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

        /* Only the first stream of the first element carries parameter blocks,
         * so a round robin reaches both it and the streams that do not. */
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

    /* An empty packet carrying new extradata replaces a substream's codec
     * configuration and, when seekable, rewrites the descriptors from the
     * trailer: a second pass over the serializer, after the layers above. */
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

    av_write_trailer(oc);

fail:
    av_packet_free(&pkt);
    /* The blocks handed over as side data were copied, so these are still owned
     * here; everything reachable from a stream group belongs to the context. */
    av_free(recon_block);
    av_free(demix_block);
    av_free(mix_block);
    /* An AVIOContext does not own the buffer it was given, so free the buffer
     * first and the context after, clearing the format context's pointer. */
    if (oc && oc->pb) {
        av_freep(&oc->pb->buffer);
        avio_context_free(&oc->pb);
    }
    avformat_free_context(oc);

    return 0;
}
