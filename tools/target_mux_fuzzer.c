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
 * Muxer fuzzing target. The input becomes an output context that is pushed
 * through the public muxing API, writing into a discarding AVIOContext rather
 * than a file. The muxer driven is IAMF, whose writer reconciles an audio
 * element's layer count and per layer channel layouts against the number of
 * substreams, the extent of AVIAMFReconGain.recon_gain and the width of the
 * bitstream fields they are serialized into.
 *
 * Opus is the only codec used, because the specification forbids recon gain
 * parameters for fLaC and ipcm and the writer strips them for both, leaving the
 * recon gain path unreachable. Audio elements are CHANNEL_BASED unless a
 * control bit asks otherwise, because a SCENE_BASED element is restricted to a
 * single ambisonic layer.
 */

/**
 * Sink state for the AVIOContext the muxer writes into. The bytes are dropped;
 * only the position and the resulting size are kept, which is all the muxer
 * ever asks for. Writing to a real file would make the target non-hermetic, and
 * the muxer cannot tell the difference.
 */
typedef struct IOContext {
    int64_t pos;
    int64_t filesize;
} IOContext;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/**
 * End the run, for an allocation failure in the harness only; a muxing error is
 * an ordinary outcome and returns normally instead.
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
 * Nested channel layout chains for scalable audio elements. Every layer of a
 * scalable element must be a channel mask superset of its predecessor while
 * carrying strictly more channels, so its layouts have to form a chain. The
 * superset rule is waived for the layer following a Mono one, which is what
 * lets a chain span six layers: the most an element may declare, and the extent
 * of AVIAMFReconGain.recon_gain, so that chain reaches both capacities exactly
 * while still being accepted. A layer count beyond a chain's length repeats its
 * last layout, which no longer carries more channels than its predecessor, so
 * the shorter chains reach the writer's refusal of that at layer counts a valid
 * element can also have.
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

/** The chains above, in the order the input selects them. */
static const struct {
    const AVChannelLayout *layouts;
    int nb_layouts;
} scalable_chains[3] = {
    { scalable_chain_a, FF_ARRAY_ELEMS(scalable_chain_a) },
    { scalable_chain_b, FF_ARRAY_ELEMS(scalable_chain_b) },
    { scalable_chain_c, FF_ARRAY_ELEMS(scalable_chain_c) },
};

/**
 * Layouts for single-layer audio elements. Such an element has no ordering
 * constraint, so it may use the expanded loudspeaker layouts as well as the
 * standard ones. The array covers both, including the three-channel one and the
 * low-frequency-effects-only layout, which is the expanded layout at index 0.
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

/**
 * The layout every layer of a SCENE_BASED audio element carries.
 *
 * A scene element describes an Ambisonics channel layout, so its layer has to
 * be in ambisonic or in custom order. First order Ambisonics is the smallest
 * layout in ambisonic order, and its four channels are carried by four mono
 * substreams, since a scene element admits no coupled substream.
 */
static const AVChannelLayout ambisonic_layout =
    AV_CHANNEL_LAYOUT_AMBISONIC_FIRST_ORDER;

/** Channel layouts a substream may have, indexed by channel count minus one. */
static const AVChannelLayout substream_layouts[2] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
};

/** Sample rates the substreams declare, rescaled into the codec config. */
static const int sample_rates[4] = { 48000, 44100, 16000, 96000 };

/**
 * A complete muxing scenario: fuzz derived controls, each with a default so
 * that an input too short to carry a control block still describes one.
 */
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
    uint8_t recon_seed[8];  /**< expanded into the recon gain matrix */
} FuzzConfig;

static void config_defaults(FuzzConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* A two layer scalable element with recon gain on both layers, and a recon
     * gain parameter block on every packet. */
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
    /*
     * Three chains to choose from need two bits, and flags1 has only one to
     * give, so the high bit is borrowed from flags3, which has bits to spare.
     * Keeping the low bit where it was leaves every field that follows at the
     * offset it already had.
     */
    cfg->chain              = ((!!(flags3 & 0x04) << 1) | !!(flags1 & 0x02)) %
                              FF_ARRAY_ELEMS(scalable_chains);
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
    cfg->scene_element      = !!(flags3 & 0x02);

    /*
     * One to eight layers. Seven is one past the extent of
     * AVIAMFReconGain.recon_gain and eight is the first value that no longer
     * fits the three bit num_layers field of the descriptor, so both of the
     * capacities the layer count is measured against have to be reachable.
     */
    cfg->nb_layers          = 1 + bytestream2_get_byte(gbc) % 8;
    /* No layer, one or two. A scene element may only have one, and a count of
     * none requires the count to be checked before the first layer is read. */
    cfg->scene_layers       = bytestream2_get_byte(gbc) % 3;
    cfg->single_layout      = bytestream2_get_byte(gbc) %
                              FF_ARRAY_ELEMS(single_layer_layouts);
    cfg->nb_subblocks       = 1 + bytestream2_get_byte(gbc) % 3;
    cfg->dmixp_mode         = bytestream2_get_byte(gbc) & 7;
    cfg->output_gain_flags  = bytestream2_get_byte(gbc) & 0x3F;
    /* Bit 0 is forced, so layer 0 stays eligible for recon gain coverage. */
    cfg->recon_gain_layers  = bytestream2_get_byte(gbc) | 1;
    cfg->sample_rate        = sample_rates[bytestream2_get_byte(gbc) %
                                           FF_ARRAY_ELEMS(sample_rates)];
    cfg->frame_size         = bytestream2_get_le16(gbc) & 0xFFF;

    /* Parameter ids stay in a small range so that the definitions of an audio
     * element and those of a mix presentation collide often, which makes the
     * writer resolve a packet's block against a definition of another type. */
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

static int element_nb_layers(const FuzzConfig *cfg)
{
    return cfg->scene_element ? cfg->scene_layers : cfg->nb_layers;
}

/**
 * The layout of layer @p idx. An element with more layers than its chain is
 * long repeats the chain's last layout, which no longer carries more channels
 * than its predecessor and is refused for that reason.
 */
static const AVChannelLayout *layer_layout(const FuzzConfig *cfg, int idx,
                                           int nb_layers)
{
    const AVChannelLayout *chain;
    int len;

    /* The layer of a scene element describes Ambisonics, not loudspeakers, and
     * the chains below say nothing about how those layers may be stacked. */
    if (cfg->scene_element)
        return &ambisonic_layout;

    if (nb_layers == 1)
        return &single_layer_layouts[cfg->single_layout];

    chain = scalable_chains[cfg->chain].layouts;
    len   = scalable_chains[cfg->chain].nb_layouts;

    return &chain[FFMIN(idx, len - 1)];
}

/**
 * Give @p dst the channel layout of layer @p idx. A custom order layout may
 * name the same channel more than once, so the custom form builds layers that
 * all share one channel mask while each carries one channel more than the one
 * before it.
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

static int layer_nb_channels(const FuzzConfig *cfg, int idx, int nb_layers)
{
    if (cfg->custom_layers)
        return 2 + idx;

    return layer_layout(cfg, idx, nb_layers)->nb_channels;
}

/**
 * Spread the layers' channels over substreams. Each layer adds channels on top
 * of its predecessor, and every increment is split over substreams of one or
 * two channels. The extra and dropped substream knobs break that accounting by
 * one substream in either direction. An element declaring no layer still gets
 * one mono substream, because a stream group without a stream is refused before
 * its layer count is read.
 *
 * @param nb_layers how many layers the element declares
 * @return the number of substreams written to @p channels.
 */
static int plan_substreams(const FuzzConfig *cfg, int nb_layers,
                           uint8_t *channels, int max)
{
    int nb = 0, prev = 0;

    for (int i = 0; i < nb_layers && nb < max; i++) {
        int delta = layer_nb_channels(cfg, i, nb_layers) - prev;

        prev += delta;
        while (delta > 0 && nb < max) {
            /* Every substream of a scene element carries one channel: the
             * writer refuses a wider one for Ambisonics in mono mode. */
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

/**
 * An Opus identification header, in the form the muxer expects it. The IAMF
 * codec configuration writer accepts exactly nineteen bytes and byte swaps them
 * into the big-endian form the specification asks for. The same bytes are
 * offered again as replacement extradata, repeating that conversion.
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
 * Build one audio element stream group and its substreams. The stream group
 * pre-allocates its AVIAMFAudioElement, so the object is populated in place
 * rather than replaced, and everything hung off it, including the parameter
 * definitions, is freed by avformat_free_context().
 *
 * @param index which audio element this is, used to keep ids apart
 */
static int add_audio_element(AVFormatContext *oc, const FuzzConfig *cfg,
                             int index)
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

        /*
         * The recon gain matrix is only read for layers carrying this flag, so
         * without it the loop over the layers spins without touching anything.
         */
        if (cfg->recon_gain_layers & (1u << FFMIN(i, 31)))
            layer->flags |= AV_IAMF_LAYER_FLAG_RECON_GAIN;
        layer->output_gain_flags = cfg->output_gain_flags;
    }

    /*
     * Recon gain is mandatory once a scalable element has more than one layer,
     * unless the codec is fLaC or ipcm. Demixing is mandatory only for the
     * higher multi layer layouts, and is supplied for every multi layer element
     * so that its descriptor is written either way.
     */
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

        /* An id that matches no audio element exercises the reference
         * validation the writer performs on a submix. */
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

/**
 * Copy a parameter block onto a packet as side data of the matching type. The
 * buffer is sized exactly to the parameter block and handed over rather than
 * copied into one of the padded buffers av_packet_new_side_data() returns, so a
 * read past its end reaches a sanitizer's redzone, not padding.
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
 * Mux one session described by @p data. The trailing bytes of the input
 * describe the scenario, if there are enough of them, and everything before
 * them becomes packet payload, so a given input always describes the same
 * session. Most scenarios are refused somewhere along the way, which is an
 * ordinary outcome and is not reported.
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

    /* A run time lookup keeps the object usable without the muxer enabled. */
    ofmt = av_guess_format("iamf", NULL, NULL);

    if (!c) {
        av_log_set_level(AV_LOG_PANIC);
        c = 1;
    }

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
     * The output is a sink that counts bytes and drops them, so nothing reaches
     * the file system. The third argument marks the context writable and the
     * callback goes in the write slot. A seek callback is supplied only for
     * some scenarios, because the muxer takes a different route through the
     * trailer when the output is seekable.
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

    /* An invalid stream group configuration is refused here. */
    ret = avformat_write_header(oc, NULL);
    if (ret < 0)
        goto fail;

    if (!oc->nb_streams)
        goto fail;

    /* Recon gain side data is what makes the writer index its matrix once per
     * layer. */
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
     * configuration of the substream it names and, when the output is seekable,
     * rewrite the descriptors from the trailer. That is a second pass over the
     * descriptor serializer, with a configuration built while packets were
     * already being written.
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
