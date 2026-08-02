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
 * The input's leading CONTROL_BLOCK_SIZE bytes are the scenario, one optional
 * knob per byte, and the input in full is the payload the packets carry; see
 * config_parse(). They lead rather than trail so that a single byte already
 * selects a scenario: a muxer copies a packet's bytes out verbatim, so an input
 * earns no coverage by growing towards controls kept behind a size threshold.
 *
 * The public API also allows a session to be initialized in a step of its own
 * before its header is written, which leaves a stream group writable after the
 * muxer has already validated it. Control bits take that route and mutate the
 * graph in that window and after the header: appending layers to an audio
 * element, replacing a layer it was validated with, and appending a submix, an
 * element or a layout to a mix presentation; see apply_mutations(). The
 * descriptor and parameter block serializers then meet something other than
 * what the muxer checked. What the muxer accepts is judged too, by
 * rejection_reason(): a guard that stops refusing an invalid configuration
 * serializes it instead of faulting.
 *
 * Linking this target needs a toolchain that supplies libFuzzer, or an explicit
 * --libfuzzer=PATH. --enable-ossfuzz is neither, and must not stand in for one:
 * it leaves LIBFUZZER_PATH empty, so the link fails on an undefined main, and
 * it stubs the codec list out to NULL entries, leaving a binary with no
 * encoders.
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

/**
 * Report that the muxer accepted a configuration it has to refuse.
 *
 * A fault is not the only way this component fails: an invalid configuration
 * that is serialized instead of refused yields a bitstream violating the
 * specification, or channel accounting no longer matching the capacity it is
 * measured against, and neither shows up as a sanitizer report. Aborting hands
 * such a configuration to the engine as the finding it is.
 */
static void report_finding(const char *what)
{
    fprintf(stderr, "Muxed a configuration that must be refused: %s\n", what);
    abort();
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
 * The most layers a channel based element may declare: IAMF defines six channel
 * groups, gives num_layers three bits and carries one recon gain row per layer,
 * so the extent of that public matrix is the same bound and taking the value
 * from it keeps the two from drifting apart. sizeof does not evaluate its
 * operand, so the null pointer is never dereferenced.
 */
#define MAX_LAYERS \
    ((int)FF_ARRAY_ELEMS(((const AVIAMFReconGain *)NULL)->recon_gain))

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

/**
 * A chain whose second layer carries a layout only the expanded loudspeaker
 * layouts describe, written as loudspeaker layout 15 followed by an expanded
 * one, a field only present when the element declares a single layer. Stacking
 * it on another layer therefore has to be refused, and nothing else about the
 * chain is wrong: the second layout is a mask superset of the first carrying
 * one channel more, so the stacking rule itself is satisfied.
 */
static const AVChannelLayout scalable_chain_d[2] = {
    AV_CHANNEL_LAYOUT_STEREO,
    AV_CHANNEL_LAYOUT_SURROUND,
};

static const struct {
    const AVChannelLayout *layouts;
    int nb_layouts;
    int expanded;   /**< a layer of the chain uses an expanded layout */
} scalable_chains[4] = {
    { scalable_chain_a, FF_ARRAY_ELEMS(scalable_chain_a), 0 },
    { scalable_chain_b, FF_ARRAY_ELEMS(scalable_chain_b), 0 },
    { scalable_chain_c, FF_ARRAY_ELEMS(scalable_chain_c), 0 },
    { scalable_chain_d, FF_ARRAY_ELEMS(scalable_chain_d), 1 },
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
 * The layout a SCENE_BASED element's layer is given for the ordinary form at
 * the four channels this predefined first order one describes: four mono
 * substreams, a scene element admitting no coupled one. The other forms and
 * channel counts are given a custom order map or a projection layout instead,
 * by set_layer_layout(), so that channel mapping is exercised too.
 */
static const AVChannelLayout ambisonic_layout =
    AV_CHANNEL_LAYOUT_AMBISONIC_FIRST_ORDER;

/**
 * Channel counts a SCENE_BASED element's layer may declare. An Ambisonics
 * layout carries a complete order, so its channel count is a perfect square;
 * the entries that are not are here so the completeness check has input
 * reaching it, which nothing else in this target produces. Kept small because a
 * scene element's substreams are mono, so each channel costs one stream.
 */
static const uint8_t scene_channels[4] = { 4, 1, 9, 3 };

/**
 * How a SCENE_BASED element's layer is built, from one control byte.
 *
 * The projection forms come last and are tested for as a group, so anything
 * added has to go before SCENE_PROJECTION or it is taken for one of them.
 */
enum SceneMode {
    SCENE_AMBISONIC,        /**< native order, mono mode: the ordinary form   */
    SCENE_CUSTOM_ACN,       /**< custom order carrying ACN relative ids       */
    SCENE_CUSTOM_FOREIGN,   /**< custom order carrying ids that are not ACN   */
    SCENE_CUSTOM_WIDE,      /**< an ACN index past the byte it is written to  */
    SCENE_BAD_MODE,         /**< an ambisonics mode outside the enumeration   */
    SCENE_PROJECTION,       /**< projection mode with a matrix that fits      */
    SCENE_PROJECTION_NULL,  /**< a matrix declared and not allocated          */
    SCENE_PROJECTION_SHORT, /**< a matrix of the wrong cardinality            */
    SCENE_NB
};

/** Which caller supplied rational is made impossible to scale, if any. */
enum BadRational {
    BAD_RATIONAL_NONE,
    BAD_RATIONAL_OUTPUT_GAIN,       /**< a layer's output gain               */
    BAD_RATIONAL_SUBMIX_GAIN,       /**< a submix's default mix gain         */
    BAD_RATIONAL_ELEMENT_GAIN,      /**< a submix element's default mix gain */
    BAD_RATIONAL_INTEGRATED,        /**< a layout's integrated loudness      */
    BAD_RATIONAL_TRUE_PEAK,         /**< a layout's true peak, a gated field */
    BAD_RATIONAL_MIX_SUBBLOCK,      /**< a mix gain subblock's start point   */
    BAD_RATIONAL_MATRIX,            /**< a projection matrix entry           */
    BAD_RATIONAL_NB
};

/** Which enumerated field is given a value outside its enumeration, if any. */
enum BadEnum {
    BAD_ENUM_NONE,
    BAD_ENUM_ELEMENT_TYPE,      /**< audio_element_type                 */
    BAD_ENUM_HEADPHONES,        /**< headphones_rendering_mode          */
    BAD_ENUM_LAYOUT_TYPE,       /**< a submix layout's layout_type      */
    BAD_ENUM_SIDE_DATA_TYPE,    /**< a parameter block's type field     */
    BAD_ENUM_ANIMATION_TYPE,    /**< a mix gain subblock's animation    */
    BAD_ENUM_NB
};

/**
 * Channel layouts a substream may be given, indexed by channel count.
 *
 * A substream carries one channel or a coupled pair, so the entries at 0 and 3
 * are widths that have to be refused; they are here so a control byte can hand
 * one substream such a width, without which the check on the widths an
 * element's streams have has no input reaching it. The muxer refuses a stream
 * wider than two channels before the writer sees it, so the three channel entry
 * reaches only that earlier check while the empty one reaches the writer's
 * own.
 */
static const AVChannelLayout substream_layouts[4] = {
    { .order = AV_CHANNEL_ORDER_UNSPEC, .nb_channels = 0 },
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
    AV_CHANNEL_LAYOUT_SURROUND,
};

static const int sample_rates[4] = { 48000, 44100, 16000, 96000 };

/**
 * Leading bytes of the input config_parse() can consume: the whole scenario
 * space fits in this one flat block, so no input has to be longer to reach any
 * part of it, and a shorter one reaches the block's leading knobs.
 */
#define CONTROL_BLOCK_SIZE 39

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
    int invalid_width;      /**< channels forced onto one substream, -1 none */
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
    int split_init;         /**< initialize the output before the header */
    int new_extradata;
    unsigned side_data;     /**< which parameter blocks to attach to packets */
    int param_truncate;     /**< bytes withheld from an attached block's side data */
    int param_inflate;      /**< announce subblocks the attached side data lacks */
    int nb_subblocks;
    int sample_rate;
    int frame_size;
    unsigned mix_id;
    unsigned demix_id;
    unsigned recon_id;
    int io_buffer_size;
    int max_pkt_size;
    int grow_layers;        /**< layers appended once validated, 0 to 8 */
    int mutate_layer;       /**< how an already validated layer is replaced */
    unsigned grow_mix;      /**< what is appended to an already validated mix */
    uint8_t recon_seed[8];  /**< expanded into the recon gain matrix */
    int scene_mode;         /**< enum SceneMode: how a scene layer is built */
    int scene_channels;     /**< index into scene_channels */
    int bad_rational;       /**< enum BadRational */
    int bad_enum;           /**< enum BadEnum */
    int output_gain;        /**< a layer's output gain numerator, signed */
    int omit_mandatory;     /**< withhold the definitions a descriptor needs */
    int same_param_id;      /**< give every role one parameter id */
} FuzzConfig;

static void config_defaults(FuzzConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* A two layer scalable element with recon gain, and a block per packet. */
    cfg->nb_elements       = 1;
    cfg->nb_layers         = 2;
    cfg->single_layout     = 1;
    cfg->invalid_width     = -1;
    cfg->recon_gain_layers = UINT_MAX;
    cfg->with_extradata    = 1;
    cfg->with_recon_info   = 1;
    cfg->with_demix_info   = 1;
    cfg->dmixp_mode        = 1;
    cfg->seekable          = 1;
    /* Initialization split from the header, with the layer count grown in
     * between to eight: past recon_gain's extent and past what the three bit
     * num_layers field holds, so even an empty input measures the count against
     * both capacities. */
    cfg->split_init        = 1;
    cfg->grow_layers       = 6;
    cfg->side_data         = 7;
    cfg->nb_subblocks      = 1;
    cfg->sample_rate       = sample_rates[0];
    cfg->frame_size        = 960;
    cfg->mix_id            = 100;
    cfg->demix_id          = 998;
    cfg->recon_id          = 101;
    /* Small enough that a partial OBU written before a rejection is flushed
     * rather than discarded unwritten with the context, which is what makes a
     * stray byte reach the sink at all. */
    cfg->io_buffer_size    = 64;
    cfg->max_pkt_size      = 1024;
    /* A gain of minus one, a legitimate negative that has to be accepted and
     * that no other control in this target produces. */
    cfg->output_gain       = -256;

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
    /* The chains need two bits and flags1 has one to give; the other comes
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
    cfg->split_init         = !!(flags3 & 0x08);
    /* None, a valid layout of another channel count, or one no loudspeaker
     * layout describes, put over a layer the muxer has already validated. This
     * shares a byte with the knobs above rather than taking one of its own, so
     * three bytes already reach it; zeroing the appended layer count below then
     * leaves a replaced layer as the only thing the muxer did not validate. */
    cfg->mutate_layer       = ((flags3 >> 4) & 3) % 3;

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

    /* The whole byte, so values past six bits reach the width the flags are
     * serialized in; masking them away left that check with no input. */
    cfg->output_gain_flags  = v;

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

    /* None to eight layers appended once the muxer has validated the count. On
     * top of a count of one to eight that reaches sixteen, well past both
     * recon_gain's extent and the three bit num_layers field, and none of these
     * layers was seen by the validation that already ran. */
    cfg->grow_layers        = v % 9;

    if (!config_byte(gbc, &v))
        return;

    /* A width no substream may have, forced onto one of them, or none at all.
     * Three channels is one past what a coupled substream carries and no
     * channel at all one short of what an uncoupled one does, so both bounds a
     * width is measured against are reachable. */
    switch (v % 3) {
    case 1:  cfg->invalid_width =  0; break;
    case 2:  cfg->invalid_width =  3; break;
    default: cfg->invalid_width = -1; break;
    }

    if (!config_byte(gbc, &v))
        return;

    /* What is appended to a mix presentation the muxer has already validated: a
     * submix, an element, a layout, any combination of the three. Every one of
     * them is something ff_iamf_add_mix_presentation() would have refused or
     * registered had it been there, so none of them can be serialized. */
    cfg->grow_mix           = v & 7;

    if (!config_byte(gbc, &v))
        return;

    /* Which of the Ambisonics forms the scene element's layer takes: a native
     * order layout, a custom order one carrying ACN indices or ids that are not
     * ACN at all or an index past the byte it goes in, and projection mode with
     * a matrix that fits, none at all, or one of the wrong size. */
    cfg->scene_mode         = v % SCENE_NB;

    if (!config_byte(gbc, &v))
        return;

    cfg->scene_channels     = v % FF_ARRAY_ELEMS(scene_channels);

    if (!config_byte(gbc, &v))
        return;

    /* One rational the writer scales into a fixed point field, given a
     * denominator it cannot be scaled by. Each selects a different sink, and
     * between them they cover every field that reaches the scaling. */
    cfg->bad_rational       = v % BAD_RATIONAL_NB;

    if (!config_byte(gbc, &v))
        return;

    /* One enumerated field given a value the enumeration does not name. Each is
     * either serialized into a field of its own width or selects the serializer
     * to use, so none of them has anything to write for such a value. */
    cfg->bad_enum           = v % BAD_ENUM_NB;

    if (!config_byte(gbc, &v))
        return;

    /* Signed, and over the range the field holds and a little past it: the
     * gain is scaled by 256 into sixteen signed bits, so a numerator beyond
     * 128 in either direction has no field to go in. */
    cfg->output_gain        = ((int)v - 128) * 256;

    if (!config_byte(gbc, &v))
        return;

    /* Withhold the definitions the descriptor needs. Without this the roles are
     * forced on for every element of more than one layer, leaving the checks
     * for a declared definition that is not carried with no input. */
    cfg->omit_mandatory     = v & 1;

    if (!config_byte(gbc, &v))
        return;

    /* One parameter id for every role, so a descriptor resolves a role against
     * a definition registered for another. The ids are a byte each and collide
     * by chance already; this makes it happen on purpose. */
    cfg->same_param_id      = v & 1;

    if (!config_byte(gbc, &v))
        return;

    /* Bytes withheld from the end of every block attached as side data, and
     * whether the subblock count it announces is raised past what it then
     * holds. A block is read in place out of the packet, so the size it is
     * carried with is all that bounds what may be read out of it: too small a
     * one has no structure to read, and too high a count no subblock where one
     * is computed. Both are as much the caller's to choose as the block is. */
    cfg->param_truncate     = v >> 1;
    cfg->param_inflate      = v & 1;
}

static int element_nb_layers(const FuzzConfig *cfg)
{
    return cfg->scene_element ? cfg->scene_layers : cfg->nb_layers;
}

/** Channels a SCENE_BASED element's layer declares. */
static int scene_nb_channels(const FuzzConfig *cfg)
{
    return scene_channels[cfg->scene_channels];
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

    if (cfg->scene_element) {
        const int nb_channels = scene_nb_channels(cfg);

        /* The predefined first order layout, for the ordinary form at the four
         * channels it describes. A projection form is handled just below, and
         * everything else is given a custom order map, deliberately, so that
         * the ACN indices a map is serialized as are exercised too. */
        if (cfg->scene_mode == SCENE_AMBISONIC && nb_channels == 4)
            return av_channel_layout_copy(dst, &ambisonic_layout);

        if (cfg->scene_mode >= SCENE_PROJECTION) {
            /* Projection mode carries no map: the layout is the channel count
             * and the matrix describes how the substreams reach it. */
            dst->order       = AV_CHANNEL_ORDER_AMBISONIC;
            dst->nb_channels = nb_channels;

            return 0;
        }

        ret = av_channel_layout_custom_init(dst, nb_channels);
        if (ret < 0)
            return ret;

        for (int i = 0; i < nb_channels; i++)
            dst->u.map[i].id =
                cfg->scene_mode == SCENE_CUSTOM_FOREIGN ?
                    AV_CHAN_FRONT_LEFT + i :
                cfg->scene_mode == SCENE_CUSTOM_WIDE ?
                    AV_CHAN_AMBISONIC_BASE + 256 + i :
                    AV_CHAN_AMBISONIC_BASE + i;

        return 0;
    }

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
    if (cfg->scene_element)
        return scene_nb_channels(cfg);

    if (cfg->custom_layers)
        return 2 + idx;

    return layer_layout(cfg, idx, nb_layers)->nb_channels;
}

/**
 * Spread the layers' channels over substreams of one or two channels, each
 * layer adding to its predecessor; the extra and dropped knobs break that
 * accounting by one either way. An element with no layer still gets one, as a
 * group without a stream is refused first. Returns the count written.
 *
 * @param accounted receives how many substreams the layers do account for,
 *                  before the knobs above break that accounting
 */
static int plan_substreams(const FuzzConfig *cfg, int nb_layers,
                           uint8_t *channels, int max, int *accounted)
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

    *accounted = nb;

    if (cfg->drop_substream && nb > 1)
        nb--;
    if (cfg->extra_substream && nb < max)
        channels[nb++] = 1;
    if (!nb)
        channels[nb++] = 1;

    return nb;
}

/**
 * The parameter id an audio element's role names, for element @p index.
 *
 * A definition is resolved by parameter id alone, so an id shared between roles
 * of different types leaves whichever was registered under it first as what the
 * others are serialized out of. The ids come from the control block a byte
 * apiece and so collide by themselves; same_param_id forces the collision so it
 * is reached without waiting for one.
 */
static unsigned param_id(const FuzzConfig *cfg, unsigned id, int index)
{
    return cfg->same_param_id ? cfg->mix_id : id + index;
}

/**
 * Whether an audio element registers a definition under the parameter id the
 * mix presentation's mix gain roles name.
 *
 * The audio elements are added first, so a definition of theirs is what a mix
 * gain role resolves to whenever the ids meet, and a mix gain role serialized
 * out of a demixing or recon gain definition writes a type ahead of one that
 * was never of it. Which of the two roles an element carries is the same
 * condition add_audio_element() creates them under.
 */
static int mix_id_taken_by_element(const FuzzConfig *cfg)
{
    const int nb_layers = element_nb_layers(cfg);
    const int has_demix = !cfg->omit_mandatory &&
                          (cfg->with_demix_info || nb_layers > 1);
    const int has_recon = !cfg->omit_mandatory &&
                          (cfg->with_recon_info || nb_layers > 1);

    for (int i = 0; i < cfg->nb_elements; i++) {
        if (has_demix && param_id(cfg, cfg->demix_id, i) == cfg->mix_id)
            return 1;
        if (has_recon && param_id(cfg, cfg->recon_id, i) == cfg->mix_id)
            return 1;
    }

    return 0;
}

/**
 * Why the muxer cannot accept @p cfg, or NULL if it may.
 *
 * Only invariants the writer is required to enforce are listed, and only for
 * configurations that violate one beyond doubt, because the caller reports a
 * finding when such a configuration is accepted. Erring towards NULL is the
 * safe direction: a configuration called muxable here is not claimed to be
 * valid, only not provably invalid, and being refused is always ordinary.
 *
 * Only what the descriptors are written from is judged, because the caller
 * measures this against the header having been written. A control reaching the
 * writer through a packet's side data instead is left out however certainly it
 * is refused there: the header is right to succeed for it, and listing it would
 * report every such input. Those controls are judged by the run not aborting,
 * which is what the checks they reach replaced.
 *
 * This is the half a sanitizer cannot supply. A guard that stops refusing an
 * invalid configuration does not fault; it serializes a descriptor violating
 * the specification, or accounts channels against a capacity that no longer
 * matches, and the run looks like every other accepted one.
 */
static const char *rejection_reason(const FuzzConfig *cfg)
{
    uint8_t substream_channels[32];
    const int nb_layers = element_nb_layers(cfg);
    int nb_substreams, accounted;

    /* A substream carries one channel or a coupled pair, and nothing else. */
    if (cfg->invalid_width >= 0)
        return "a substream carrying other than one or two channels";

    /* A submix element names an audio element that has to exist. */
    if (cfg->dangling_element)
        return "a submix referring to an audio element that is not present";

    /* Every one of these is scaled into a fixed point field by a division whose
     * divisor is the denominator, so none of them can be serialized. The one
     * carried by a parameter block is left to the caller's other reading, being
     * reached through a packet rather than through a descriptor. */
    if (cfg->bad_rational != BAD_RATIONAL_NONE &&
        cfg->bad_rational != BAD_RATIONAL_MIX_SUBBLOCK &&
        /* Except the two that are only reached under a condition of their own:
         * a layer's output gain only for a layer carrying flags, and a matrix
         * entry only for a projection mode layer that has a matrix allocated to
         * put one in. */
        (cfg->bad_rational != BAD_RATIONAL_OUTPUT_GAIN ||
         (!cfg->scene_element && cfg->output_gain_flags)) &&
        (cfg->bad_rational != BAD_RATIONAL_MATRIX ||
         (cfg->scene_element && cfg->scene_mode >= SCENE_PROJECTION &&
          cfg->scene_mode != SCENE_PROJECTION_NULL)))
        return "a rational with a denominator it cannot be scaled by";

    /* Each of these is serialized into a field of its own width, or selects
     * which serializer runs, and no value outside the enumeration has either.
     * The two a parameter block carries, its type field and a mix gain
     * subblock's animation, are likewise left out, reaching the writer through
     * a packet. */
    if (cfg->bad_enum == BAD_ENUM_ELEMENT_TYPE)
        return "an audio element type outside the enumeration";
    if (cfg->bad_enum == BAD_ENUM_HEADPHONES)
        return "a headphones rendering mode outside the enumeration";
    if (cfg->bad_enum == BAD_ENUM_LAYOUT_TYPE)
        return "a submix layout type outside the enumeration";

    /* A mix gain role is always serialized, and out of whichever definition was
     * registered under the id it names, which an audio element got to first. */
    if (mix_id_taken_by_element(cfg))
        return "a mix gain role resolving to a definition of another type";

    if (cfg->scene_element) {
        const int nb_channels = scene_nb_channels(cfg);

        /* A scene element describes one Ambisonics layout, so it has one layer,
         * and a count of none has no layer to describe it with. */
        if (nb_layers != 1)
            return "a scene based audio element with other than one layer";

        /* An Ambisonics layout carries every harmonic of a complete order, so
         * its channel count is a perfect square. */
        if (nb_channels != 1 && nb_channels != 4 && nb_channels != 9)
            return "an Ambisonics layout of an incomplete order";

        switch (cfg->scene_mode) {
        /* A custom order map is written as one ACN index per channel, in one
         * byte each, so every id has to be ambisonic and within that byte. */
        case SCENE_CUSTOM_FOREIGN:
            return "a custom order map carrying ids that are not Ambisonics";
        case SCENE_CUSTOM_WIDE:
            return "an ACN index past the byte it is serialized into";
        /* The matrix is read entry by entry for every channel of every
         * substream, so a count short of that geometry, or none allocated at
         * all, leaves the serializer nothing to write those entries from. */
        case SCENE_PROJECTION_NULL:
            return "a demixing matrix count with no matrix allocated";
        case SCENE_PROJECTION_SHORT:
            /* One channel squares to one entry, of which one less is none, and
             * set_projection_matrix() declares one either way; that leaves the
             * count agreeing with the geometry and nothing to object to. */
            if (nb_channels > 1)
                return "a demixing matrix shorter than its geometry";
            break;
        case SCENE_BAD_MODE:
            return "an Ambisonics mode outside the enumeration";
        default:
            break;
        }
    } else {
        /* The layer count bounds the loop indexing the recon gain matrix and is
         * written into a three bit field, so it has an upper bound. */
        if (nb_layers > MAX_LAYERS)
            return "more layers than the recon gain matrix has rows";

        /* Serialized into a six bit field of its own, which is also the width
         * the parser reads the flags back from. */
        if (cfg->output_gain_flags > 0x3F)
            return "output gain flags wider than the field they go in";

        /*
         * Past one layer the descriptor declares a recon gain definition, which
         * it then serializes out of the audio element, so declaring the type
         * without carrying one describes nothing. Only past one layer: a single
         * layer element declares no recon gain, and the demixing type a single
         * layer may declare is dropped rather than refused when uncarried. The
         * codec here is Opus, which is neither of the two that would have the
         * type cleared again.
         */
        if (cfg->omit_mandatory && nb_layers > 1)
            return "a declared recon gain definition that is not carried";

        /* Every custom layer shares one channel mask while carrying a channel
         * count of its own, so from the second on no layout has both. */
        if (cfg->custom_layers && nb_layers > 1)
            return "layers sharing a channel mask but not a channel count";

        /* An expanded layout is only serializable in a single layer element. A
         * chain is only consulted for layers that are not custom ones. */
        if (!cfg->custom_layers && nb_layers > 1 &&
            scalable_chains[cfg->chain].expanded)
            return "an expanded loudspeaker layout in a multi layer element";
    }

    /*
     * The layers must between them account for every substream: one too many
     * leaves a phantom the layers never reach, one too few a layer's channels
     * unaccounted for. A dropped and an added substream cancel out in the
     * count while still leaving the widths at odds with the layers, so that
     * pairing is not reported here; the writer refuses it for a reason this
     * does not claim to predict.
     */
    nb_substreams = plan_substreams(cfg, nb_layers, substream_channels,
                                    FF_ARRAY_ELEMS(substream_channels),
                                    &accounted);
    if (nb_substreams != accounted)
        return "more or fewer substreams than the layers account for";

    return NULL;
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
 * Give a projection mode layer its demixing matrix.
 *
 * The cardinality the layer must declare is its substreams times its channels,
 * which for a scene element's mono substreams is the channel count squared. The
 * absent form declares that count and allocates nothing, and the short form
 * declares one less than the geometry calls for; both are things the writer can
 * see. The array is always exactly as long as the count declared, because an
 * array shorter than its own count is not something any amount of validation
 * can detect and reporting it would say nothing about the writer.
 */
static int set_projection_matrix(AVIAMFLayer *layer, const FuzzConfig *cfg)
{
    const int nb_channels = scene_nb_channels(cfg);
    const int nb_entries  = nb_channels * nb_channels;
    int nb_declared = nb_entries;

    if (cfg->scene_mode == SCENE_PROJECTION_SHORT)
        nb_declared = FFMAX(nb_entries - 1, 1);

    layer->nb_demixing_matrix = nb_declared;

    if (cfg->scene_mode == SCENE_PROJECTION_NULL)
        return 0;

    layer->demixing_matrix = av_malloc_array(nb_declared, sizeof(AVRational));
    if (!layer->demixing_matrix)
        return AVERROR(ENOMEM);

    for (int i = 0; i < nb_declared; i++)
        layer->demixing_matrix[i] = av_make_q(cfg->recon_seed[i & 7] - 128,
                                              1 << 7);

    /* The last entry is the one written last, so a denominator it cannot be
     * scaled by is only reached once every entry before it already was. */
    if (cfg->bad_rational == BAD_RATIONAL_MATRIX)
        layer->demixing_matrix[nb_declared - 1] = av_make_q(1, 0);

    return 0;
}

/**
 * Build one audio element stream group and its substreams. The group
 * pre-allocates its AVIAMFAudioElement, so it is filled in place and freed with
 * the format context.
 */
static int add_audio_element(AVFormatContext *oc, const FuzzConfig *cfg,
                             int index)
{
    uint8_t substream_channels[32];
    const int nb_layers = element_nb_layers(cfg);
    AVIAMFAudioElement *ae;
    AVStreamGroup *stg;
    int nb_substreams, accounted, ret;

    stg = avformat_stream_group_create(
        oc, AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT, NULL);
    if (!stg)
        return AVERROR(ENOMEM);

    stg->id = index + 1;
    ae = stg->params.iamf_audio_element;
    ae->audio_element_type = cfg->scene_element ?
                             AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE :
                             AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL;
    /* Neither of the two types the enumeration names, so there is no field it
     * fits in and no serializer for the layers it would select. */
    if (cfg->bad_enum == BAD_ENUM_ELEMENT_TYPE)
        ae->audio_element_type = (enum AVIAMFAudioElementType)3;
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
        /* Only scaled for a layer carrying flags, so the flags decide whether
         * this reaches the scaling at all. */
        layer->output_gain = cfg->bad_rational == BAD_RATIONAL_OUTPUT_GAIN ?
                             av_make_q(1, 0) : av_make_q(cfg->output_gain, 256);

        /* Mono mode is the default and needs nothing set. Projection mode is
         * the three forms from SCENE_PROJECTION on, each of which wants a
         * matrix; the bad mode sorts before them so that it is not taken for
         * one, its layer being otherwise ordinary. */
        if (cfg->scene_element && cfg->scene_mode >= SCENE_PROJECTION) {
            layer->ambisonics_mode = AV_IAMF_AMBISONICS_MODE_PROJECTION;
            ret = set_projection_matrix(layer, cfg);
            if (ret < 0)
                return ret;
        } else if (cfg->scene_element && cfg->scene_mode == SCENE_BAD_MODE) {
            layer->ambisonics_mode = (enum AVIAMFAmbisonicsMode)3;
        }
    }

    /* Recon gain is mandatory past one layer unless the codec is fLaC or ipcm,
     * demixing only for the higher layouts; both are otherwise always given, so
     * omit_mandatory is what leaves a declared definition uncarried. */
    if (!cfg->omit_mandatory && (cfg->with_demix_info || nb_layers > 1)) {
        AVIAMFParamDefinition *demix;
        AVIAMFDemixingInfo *info;

        /* A demixing definition with anything but one subblock is refused. */
        demix = alloc_param(AV_IAMF_PARAMETER_DEFINITION_DEMIXING, 1);
        if (!demix)
            return AVERROR(ENOMEM);
        ae->demixing_info = demix;

        demix->parameter_id = param_id(cfg, cfg->demix_id, index);
        info = av_iamf_param_definition_get_subblock(demix, 0);
        info->dmixp_mode = cfg->dmixp_mode;
    }
    if (!cfg->omit_mandatory && (cfg->with_recon_info || nb_layers > 1)) {
        AVIAMFParamDefinition *recon;

        recon = alloc_param(AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN, 1);
        if (!recon)
            return AVERROR(ENOMEM);
        ae->recon_gain_info = recon;

        recon->parameter_id = param_id(cfg, cfg->recon_id, index);
    }

    nb_substreams = plan_substreams(cfg, nb_layers, substream_channels,
                                    FF_ARRAY_ELEMS(substream_channels),
                                    &accounted);

    /* Give the last substream a width no substream may have. The last one, so
     * the channels the layers account for are unaffected up to that point and
     * the width is the one thing left to object to. */
    if (cfg->invalid_width >= 0)
        substream_channels[nb_substreams - 1] = cfg->invalid_width;

    for (int i = 0; i < nb_substreams; i++) {
        const AVChannelLayout *sub = &substream_layouts[substream_channels[i]];
        AVStream *st = avformat_new_stream(oc, NULL);

        if (!st)
            return AVERROR(ENOMEM);

        /*
         * The substream id comes from the stream id and duplicates are refused,
         * so each element gets its own block. The blocks start at one because a
         * substream the layers never account for stays zero initialized, id
         * included: were a legitimate substream given id zero, that phantom
         * would collide with it and be refused as a duplicate, hiding it from
         * the cross-check between the substream count and what the layers
         * account for. Nothing else wants zero, as parameter blocks are
         * accepted on the first stream the muxer was given, by position.
         */
        st->id = 1 + index * 64 + i;
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
 * Append layers to every audio element once the muxer has validated them.
 *
 * An AVIAMFAudioElement stays reachable and writable through its stream group
 * for as long as the format context lives, and adding a layer only extends the
 * array it holds, so the count a caller declares can grow after the muxer has
 * validated it and sized its own state from it. Two documented sequences open
 * that window: avformat_init_output() before avformat_write_header(), the first
 * running the muxer's init callback where the validation lives and the second
 * the header callback where the descriptors are serialized; and the trailer,
 * which serializes them a second time. Anything the writer indexes per layer,
 * or bounds by a layer count, therefore has to be measured against what it
 * validated and not against what the element says when it is read.
 *
 * The appended layers are built exactly like the ones added before, flags
 * included, so nothing downstream tells them apart by inspection; only the
 * count they are counted by differs.
 */
static int grow_audio_elements(AVFormatContext *oc, const FuzzConfig *cfg,
                               const char **reason)
{
    if (!cfg->grow_layers)
        return 0;

    for (unsigned i = 0; i < oc->nb_stream_groups; i++) {
        AVStreamGroup *stg = oc->stream_groups[i];
        AVIAMFAudioElement *ae;
        int nb_layers, ret;

        if (stg->type != AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT)
            continue;

        ae        = stg->params.iamf_audio_element;
        nb_layers = element_nb_layers(cfg);

        for (int j = 0; j < cfg->grow_layers; j++) {
            AVIAMFLayer *layer = av_iamf_audio_element_add_layer(ae);

            if (!layer)
                return AVERROR(ENOMEM);

            /* Continue the layouts of the layers already there, so an appended
             * layer is indistinguishable from one that was validated. */
            ret = set_layer_layout(&layer->ch_layout, cfg, nb_layers + j,
                                   nb_layers);
            if (ret < 0)
                return ret;

            if (cfg->recon_gain_layers & (1u << FFMIN(nb_layers + j, 31)))
                layer->flags |= AV_IAMF_LAYER_FLAG_RECON_GAIN;
            layer->output_gain_flags = cfg->output_gain_flags;
        }

        if (!*reason)
            *reason = "layers appended past the count the muxer validated";
    }

    return 0;
}

/**
 * Replace a layer the muxer has already validated with a different one.
 *
 * Appending layers is caught by the count alone. This leaves the count as it
 * was and changes a layer that count covers, so what the descriptor serializes
 * for that layer and what the layer was accounted for come from two different
 * layouts, with nothing about the element saying so afterwards.
 *
 * The two forms are the two ways that goes wrong, and they are deliberately
 * exclusive so that each reaches the check it is aimed at:
 *
 * - A layout that is perfectly valid on its own, carrying a channel count the
 *   replaced one did not. The substream counts recorded for the layer still
 *   describe the channels the old layout added, so the layer's own accounting
 *   is what has to be measured for this to be noticed at all.
 * - A layout carrying the same channel count, leaving the accounting intact,
 *   but naming one channel over and over. No loudspeaker layout describes it,
 *   since every entry of either table carries each of its channels once, so the
 *   descriptor writer has nothing to serialize the layer as. Only put over a
 *   channel based element, a scene based one being described by its custom
 *   order map itself rather than by a layout matched to it.
 */
static int mutate_audio_elements(AVFormatContext *oc, const FuzzConfig *cfg,
                                 const char **reason)
{
    if (!cfg->mutate_layer)
        return 0;

    for (unsigned i = 0; i < oc->nb_stream_groups; i++) {
        AVStreamGroup *stg = oc->stream_groups[i];
        AVChannelLayout *ch_layout;
        AVIAMFAudioElement *ae;
        int nb_channels, ret;

        if (stg->type != AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT)
            continue;

        ae = stg->params.iamf_audio_element;
        /* An element validated with no layer at all has none to replace, and
         * was refused for having none. */
        if (!ae->nb_layers)
            continue;

        ch_layout   = &ae->layers[0]->ch_layout;
        nb_channels = ch_layout->nb_channels;

        if (cfg->mutate_layer == 1) {
            /* Three channels, or two where the layer already carries three: no
             * layout here carries both counts, so the accounting always
             * moves. */
            const AVChannelLayout *replacement = nb_channels == 3 ?
                                                 &substream_layouts[2] :
                                                 &substream_layouts[3];

            /* Releases what the layer held, so no custom order map leaks. */
            ret = av_channel_layout_copy(ch_layout, replacement);
            if (ret < 0)
                return ret;

            if (!*reason)
                *reason = "a validated layer replaced with a layout of another "
                          "channel count";
        } else if (nb_channels > 0 &&
                   ae->audio_element_type ==
                   AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL) {
            /* Copying does this first; a custom layout's own initializer does
             * not. */
            av_channel_layout_uninit(ch_layout);
            ret = av_channel_layout_custom_init(ch_layout, nb_channels);
            if (ret < 0)
                return ret;

            for (int j = 0; j < nb_channels; j++)
                ch_layout->u.map[j].id = AV_CHAN_FRONT_LEFT;

            if (!*reason)
                *reason = "a validated layer replaced with a layout no "
                          "loudspeaker layout describes";
        }
    }

    return 0;
}

/**
 * Grow a mix presentation the muxer has already validated.
 *
 * ff_iamf_add_mix_presentation() requires a mix configuration on every submix
 * and every element it is given, and registers a parameter definition for each,
 * so what it validated is a graph carrying one everywhere. A submix or an
 * element appended afterwards carries none at all, and a layout appended
 * afterwards may describe a channel layout no sound system does. All three are
 * read while serializing a mix presentation, which comes after the Sequence
 * Header, the Codec Configs and the Audio Elements, so objecting to them only
 * then is objecting to them too late.
 */
static int grow_mix_presentations(AVFormatContext *oc, const FuzzConfig *cfg,
                                  const char **reason)
{
    if (!cfg->grow_mix)
        return 0;

    for (unsigned i = 0; i < oc->nb_stream_groups; i++) {
        AVStreamGroup *stg = oc->stream_groups[i];
        AVIAMFMixPresentation *mix;
        AVIAMFSubmix *submix;

        if (stg->type != AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION)
            continue;

        mix = stg->params.iamf_mix_presentation;

        if (cfg->grow_mix & 1) {
            if (!av_iamf_mix_presentation_add_submix(mix))
                return AVERROR(ENOMEM);

            if (!*reason)
                *reason = "a submix appended with no output mix configuration";
        }

        /* Read after the append above, which may have moved the array. */
        if (!mix->nb_submixes)
            continue;
        submix = mix->submixes[0];

        if (cfg->grow_mix & 2) {
            AVIAMFSubmixElement *element = av_iamf_submix_add_element(submix);

            if (!element)
                return AVERROR(ENOMEM);

            /* An audio element that does exist, so the mix configuration this
             * element has none of is the only thing left to object to. */
            element->audio_element_id = 1;

            if (!*reason)
                *reason = "a submix element appended with no mix configuration";
        }

        if (cfg->grow_mix & 4) {
            AVIAMFSubmixLayout *layout = av_iamf_submix_add_layout(submix);
            int ret;

            if (!layout)
                return AVERROR(ENOMEM);

            layout->layout_type = AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS;
            /* One channel named five times, as in a replaced layer above: every
             * sound system carries each of its channels once, so none of them
             * describes this and the layout has no value to be written as. */
            av_channel_layout_uninit(&layout->sound_system);
            ret = av_channel_layout_custom_init(&layout->sound_system, 5);
            if (ret < 0)
                return ret;

            for (int j = 0; j < layout->sound_system.nb_channels; j++)
                layout->sound_system.u.map[j].id = AV_CHAN_FRONT_LEFT;

            if (!*reason)
                *reason = "a submix layout appended that no sound system "
                          "describes";
        }
    }

    return 0;
}

/**
 * Apply every change the scenario asks of a session the muxer has already
 * validated, reporting the first one applied.
 *
 * @param reason receives why the resulting session cannot be muxed, left alone
 *               when nothing was changed. Only set for a change that did take
 *               effect, so a scenario asking for one the session has nowhere to
 *               apply is never reported.
 */
static int apply_mutations(AVFormatContext *oc, const FuzzConfig *cfg,
                           const char **reason)
{
    int ret = grow_audio_elements(oc, cfg, reason);

    if (ret < 0)
        return ret;

    ret = mutate_audio_elements(oc, cfg, reason);
    if (ret < 0)
        return ret;

    return grow_mix_presentations(oc, cfg, reason);
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
    if (cfg->bad_rational == BAD_RATIONAL_SUBMIX_GAIN)
        submix->default_mix_gain = av_make_q(1, 0);

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
        /* Serialized into a two bit field that the enumeration's two values
         * fill, so a third has nothing to be written as. */
        if (cfg->bad_enum == BAD_ENUM_HEADPHONES)
            element->headphones_rendering_mode =
                (enum AVIAMFHeadphonesMode)2;
        if (cfg->bad_rational == BAD_RATIONAL_ELEMENT_GAIN)
            element->default_mix_gain = av_make_q(1, 0);
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
                                     &substream_layouts[2]);
        if (ret < 0)
            return ret;
    }

    /* Serialized into a two bit field, and it selects whether a sound system is
     * written at all, so a value naming neither layout has neither. */
    if (cfg->bad_enum == BAD_ENUM_LAYOUT_TYPE)
        layout->layout_type = (enum AVIAMFSubmixLayoutType)1;

    /* Always scaled, so this always reaches it. */
    if (cfg->bad_rational == BAD_RATIONAL_INTEGRATED)
        layout->integrated_loudness = av_make_q(1, 0);
    /* Only scaled when both parts are non zero, and a negative denominator
     * satisfies that and then reaches the scaling anyway. */
    if (cfg->bad_rational == BAD_RATIONAL_TRUE_PEAK)
        layout->true_peak = av_make_q(3, -4);

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
        /* The subblocks are filled through the type they were allocated with,
         * before the field is overwritten below, so each one is a whole valid
         * subblock of a kind the type field then contradicts. */
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
            /* Serialized as a leb128 of whatever the field holds and deciding
             * which of the values above are written with it, so a value the
             * enumeration does not name is written by nothing that reads it
             * back. Set here rather than seeded above, where the animation is
             * taken modulo the count of named values. */
            if (cfg->bad_enum == BAD_ENUM_ANIMATION_TYPE)
                gain->animation_type =
                    (enum AVIAMFAnimationType)(AV_IAMF_ANIMATION_TYPE_BEZIER + 1);

            /* The first rational the subblock writes, so nothing of the
             * animation is out when it is reached. */
            if (cfg->bad_rational == BAD_RATIONAL_MIX_SUBBLOCK)
                gain->start_point_value = av_make_q(1, 0);
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

    /* Side data is read in place, so the type field holds whatever a caller put
     * there. A value the enumeration does not name selects no subblock
     * serializer, and this is the only way anything but the three named ones
     * reaches the writer. */
    if (cfg->bad_enum == BAD_ENUM_SIDE_DATA_TYPE)
        param->type = (enum AVIAMFParamDefinitionType)(type + 8);

    /* Raised after the subblocks were filled, so the count announces one the
     * allocation does not hold and the last is computed past its end. */
    if (cfg->param_inflate)
        param->nb_subblocks++;

    return param;
}

/**
 * Withhold @p truncate bytes from the end of what a block is attached with, one
 * byte always being kept so the packet still carries the side data. Nothing
 * else tells the writer where a block read in place ends.
 */
static size_t truncated_param_size(size_t size, int truncate)
{
    if (!size)
        return 0;

    return size - FFMIN((size_t)truncate, size - 1);
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
    AVFormatContext *oc = NULL;
    AVIOContext *fuzzed_pb;
    AVPacket *pkt = NULL;
    IOContext opaque = { 0 };
    const char *late_reason = NULL;
    int rewrites_descriptors = 0;
    const char *reason;
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

    /* Decided from the configuration alone, before anything is built. */
    reason = rejection_reason(&cfg);

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
        ret = add_audio_element(oc, &cfg, i);
        if (ret < 0)
            goto fail;
    }
    ret = add_mix_presentation(oc, &cfg);
    if (ret < 0)
        goto fail;

    /* An invalid stream group configuration is refused while the output is
     * initialized, which avformat_write_header() does on its own. Doing that as
     * a separate step first is equally supported and leaves the session usable
     * in between, which is when the stream groups are changed: the descriptors
     * the header writes are then serialized from a configuration other than the
     * validated one. Changing them after the header instead leaves that to the
     * trailer, which serializes them a second time, so the two routes are taken
     * apart. */
    if (cfg.split_init) {
        ret = avformat_init_output(oc, NULL);
        if (ret < 0)
            goto fail;

        ret = apply_mutations(oc, &cfg, &reason);
        if (ret < 0)
            goto fail;
    }

    ret = avformat_write_header(oc, NULL);
    if (ret < 0)
        goto fail;

    /*
     * Being refused above is an ordinary outcome and says nothing, but being
     * accepted does: the descriptors have been serialized by now, so a
     * configuration breaking an invariant the writer is required to enforce has
     * just been written out as if it were valid.
     */
    if (reason)
        report_finding(reason);

    if (!oc->nb_streams)
        goto fail;

    if (!cfg.split_init) {
        ret = apply_mutations(oc, &cfg, &late_reason);
        if (ret < 0)
            goto fail;
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

    /* What the packets carry, which is not what was allocated when a block is
     * attached with fewer bytes than it was built with. */
    recon_size = truncated_param_size(recon_size, cfg.param_truncate);
    demix_size = truncated_param_size(demix_size, cfg.param_truncate);
    mix_size   = truncated_param_size(mix_size, cfg.param_truncate);

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
        /* Flushed straight away, as an interleaved packet is otherwise held
         * back until the trailer and whether the muxer received it before the
         * trailer ran would be unknown. Both calls succeeding means it did, so
         * a seekable output has its descriptors serialized a second time. */
        if (av_interleaved_write_frame(oc, pkt) >= 0 &&
            av_interleaved_write_frame(oc, NULL) >= 0)
            rewrites_descriptors = cfg.seekable;
    }

    ret = av_write_trailer(oc);

    /*
     * The same reading as above, for a session changed after the header
     * instead: that second pass over the descriptor serializer is where a
     * change made once the header was written is first read back, so it is
     * where such a change has to be refused. Serializing it there means a
     * descriptor describing other than the validated configuration has replaced
     * a correct one already written, which the first pass having been correct
     * does nothing to excuse.
     */
    if (late_reason && rewrites_descriptors && ret >= 0)
        report_finding(late_reason);

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
