/*
 * Immersive Audio Model and Formats muxing helpers and structs
 * Copyright (c) 2023 James Almer <jamrial@gmail.com>
 *
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

#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/iamf.h"
#include "libavutil/mem.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/put_bits.h"
#include "avformat.h"
#include "avio_internal.h"
#include "iamf.h"
#include "iamf_writer.h"


static int update_extradata(IAMFCodecConfig *codec_config)
{
    GetBitContext gb;
    PutBitContext pb;
    int ret;

    switch(codec_config->codec_id) {
    case AV_CODEC_ID_OPUS:
        if (codec_config->extradata_size != 19)
            return AVERROR_INVALIDDATA;
        codec_config->extradata_size -= 8;
        AV_WB8(codec_config->extradata   + 0,  AV_RL8(codec_config->extradata + 8)); // version
        AV_WB8(codec_config->extradata   + 1,  2); // set channels to stereo
        AV_WB16A(codec_config->extradata + 2,  AV_RL16A(codec_config->extradata + 10)); // Byte swap pre-skip
        AV_WB32A(codec_config->extradata + 4,  AV_RL32A(codec_config->extradata + 12)); // Byte swap sample rate
        AV_WB16A(codec_config->extradata + 8,  0); // set Output Gain to 0
        AV_WB8(codec_config->extradata   + 10, AV_RL8(codec_config->extradata + 18)); // Mapping family
        break;
    case AV_CODEC_ID_FLAC: {
        uint8_t buf[13];

        init_put_bits(&pb, buf, sizeof(buf));
        ret = init_get_bits8(&gb, codec_config->extradata, codec_config->extradata_size);
        if (ret < 0)
            return ret;

        put_bits32(&pb, get_bits_long(&gb, 32)); // min/max blocksize
        put_bits63(&pb, 48, get_bits64(&gb, 48)); // min/max framesize
        put_bits(&pb, 20, get_bits(&gb, 20)); // samplerate
        skip_bits(&gb, 3);
        put_bits(&pb, 3, 1); // set channels to stereo
        ret = put_bits_left(&pb);
        put_bits(&pb, ret, get_bits(&gb, ret));
        flush_put_bits(&pb);

        memcpy(codec_config->extradata, buf, sizeof(buf));
        break;
    }
    default:
        break;
    }

    return 0;
}

static int populate_audio_roll_distance(IAMFCodecConfig *codec_config)
{
    switch (codec_config->codec_id) {
    case AV_CODEC_ID_OPUS:
        if (!codec_config->nb_samples)
            return AVERROR(EINVAL);
        // ceil(3840 / nb_samples)
        codec_config->audio_roll_distance = -(1 + ((3840 - 1) / codec_config->nb_samples));
        break;
    case AV_CODEC_ID_AAC:
        codec_config->audio_roll_distance = -1;
        break;
    case AV_CODEC_ID_FLAC:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_S32BE:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S32LE:
        codec_config->audio_roll_distance = 0;
        break;
    default:
        return AVERROR(EINVAL);
    }

    return 0;
}

static int fill_codec_config(IAMFContext *iamf, const AVStreamGroup *stg,
                             IAMFCodecConfig *codec_config)
{
    const AVStream *st = stg->streams[0];
    IAMFCodecConfig **tmp;
    int j, ret = 0;

    codec_config->codec_id = st->codecpar->codec_id;
    codec_config->codec_tag = st->codecpar->codec_tag;
    switch (codec_config->codec_id) {
    case AV_CODEC_ID_OPUS:
        codec_config->sample_rate = 48000;
        codec_config->nb_samples = av_rescale(st->codecpar->frame_size, 48000, st->codecpar->sample_rate);
        break;
    default:
        codec_config->sample_rate = st->codecpar->sample_rate;
        codec_config->nb_samples = st->codecpar->frame_size;
        break;
    }
    populate_audio_roll_distance(codec_config);
    if (st->codecpar->extradata_size) {
        if (st->codecpar->extradata_size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
            return AVERROR_INVALIDDATA;

        codec_config->extradata = av_malloc(st->codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!codec_config->extradata)
            return AVERROR(ENOMEM);
        memcpy(codec_config->extradata, st->codecpar->extradata, st->codecpar->extradata_size);
        memset(codec_config->extradata + st->codecpar->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        codec_config->extradata_size = st->codecpar->extradata_size;
        ret = update_extradata(codec_config);
        if (ret < 0)
            goto fail;
    }

    for (j = 0; j < iamf->nb_codec_configs; j++) {
        if (!memcmp(iamf->codec_configs[j], codec_config, offsetof(IAMFCodecConfig, extradata)) &&
            (!codec_config->extradata_size || !memcmp(iamf->codec_configs[j]->extradata,
                                                      codec_config->extradata, codec_config->extradata_size)))
            break;
    }

    if (j < iamf->nb_codec_configs) {
        av_free(iamf->codec_configs[j]->extradata);
        av_free(iamf->codec_configs[j]);
        iamf->codec_configs[j] = codec_config;
        return j;
    }

    tmp = av_realloc_array(iamf->codec_configs, iamf->nb_codec_configs + 1, sizeof(*iamf->codec_configs));
    if (!tmp) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    iamf->codec_configs = tmp;
    iamf->codec_configs[iamf->nb_codec_configs] = codec_config;
    codec_config->codec_config_id = iamf->nb_codec_configs;

    return iamf->nb_codec_configs++;

fail:
    av_freep(&codec_config->extradata);
    return ret;
}

static int add_param_definition(IAMFContext *iamf, AVIAMFParamDefinition *param,
                                const IAMFAudioElement *audio_element, void *log_ctx)
{
    IAMFParamDefinition **tmp, *param_definition;
    IAMFCodecConfig *codec_config = NULL;

    tmp = av_realloc_array(iamf->param_definitions, iamf->nb_param_definitions + 1,
                           sizeof(*iamf->param_definitions));
    if (!tmp)
        return AVERROR(ENOMEM);

    iamf->param_definitions = tmp;

    if (audio_element)
        codec_config = iamf->codec_configs[audio_element->codec_config_id];

    if (!param->parameter_rate) {
        if (!codec_config) {
            av_log(log_ctx, AV_LOG_ERROR, "parameter_rate needed but not set for parameter_id %u\n",
                   param->parameter_id);
            return AVERROR(EINVAL);
        }
        param->parameter_rate = codec_config->sample_rate;
    }
    if (codec_config) {
        if (!param->duration)
            param->duration = av_rescale(codec_config->nb_samples, param->parameter_rate, codec_config->sample_rate);
        if (!param->constant_subblock_duration)
            param->constant_subblock_duration = av_rescale(codec_config->nb_samples, param->parameter_rate, codec_config->sample_rate);
    }

    param_definition = av_mallocz(sizeof(*param_definition));
    if (!param_definition)
        return AVERROR(ENOMEM);

    param_definition->mode = !!param->duration;
    param_definition->param = param;
    param_definition->audio_element = audio_element;
    iamf->param_definitions[iamf->nb_param_definitions++] = param_definition;

    return 0;
}

/**
 * Check that a Parameter Definition carries the type the role naming it mandates.
 *
 * A Parameter Definition is resolved by parameter id alone, so what a descriptor is
 * serialized out of is whichever definition was registered under that id first, whatever
 * the object naming it holds. Where a role is written, both have to be of its type, or a
 * recon_gain_info would be serialized out of a demixing definition: the type is written to
 * the bitstream ahead of the definition, so the two would disagree in the descriptor itself
 * and be read back as a kind of parameter the definition never was.
 *
 * @param param            the definition the role's owner carries
 * @param param_definition what is registered under @p param's id and what the role is
 *                         therefore serialized out of, or NULL to require only the type of
 *                         the definition @p param itself is. An id may legitimately be
 *                         shared with a role that is never serialized, and which definition
 *                         a role is written out of is not settled until it is written, so
 *                         only a role actually being serialized may require the registered
 *                         one to agree. NULL is not an error.
 * @param type             the type the role mandates
 * @param name             the role, for the diagnostic
 * @return 0, or AVERROR(EINVAL) having reported which type does not match
 */
static int check_param_definition_type(const AVIAMFParamDefinition *param,
                                       const IAMFParamDefinition *param_definition,
                                       enum AVIAMFParamDefinitionType type,
                                       const char *name, void *log_ctx)
{
    if (param->type != type) {
        av_log(log_ctx, AV_LOG_ERROR, "Parameter Definition with ID %u in %s is of type %d. "
               "Must be %d\n", param->parameter_id, name, param->type, type);
        return AVERROR(EINVAL);
    }

    if (param_definition && param_definition->param->type != type) {
        av_log(log_ctx, AV_LOG_ERROR, "Parameter Definition with ID %u in %s is registered "
               "with type %d. Must be %d\n", param->parameter_id, name,
               param_definition->param->type, type);
        return AVERROR(EINVAL);
    }

    return 0;
}

int ff_iamf_add_audio_element(IAMFContext *iamf, const AVStreamGroup *stg, void *log_ctx)
{
    const AVIAMFAudioElement *iamf_audio_element;
    IAMFAudioElement **tmp, *audio_element;
    IAMFCodecConfig *codec_config;
    int ret;

    if (stg->type != AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT)
        return AVERROR(EINVAL);
    if (!stg->nb_streams) {
        av_log(log_ctx, AV_LOG_ERROR, "Audio Element id %"PRId64" has no streams\n", stg->id);
        return AVERROR(EINVAL);
    }

    /* An IAMF substream carries either one channel or a coupled stereo pair. Any other
     * width can't be accounted for by the substream and coupled substream counts derived
     * from it below, and would leave the amount of channels the element declares at odds
     * with the amount its substreams add up to. Same widths the parser assigns when
     * demuxing. */
    for (int i = 0; i < stg->nb_streams; i++) {
        const int nb_channels = stg->streams[i]->codecpar->ch_layout.nb_channels;

        if (nb_channels < 1 || nb_channels > 2) {
            av_log(log_ctx, AV_LOG_ERROR, "Invalid amount of channels %d in stream %d from Audio Element id %"PRId64
                   ". Must be 1 or 2\n", nb_channels, i, stg->id);
            return AVERROR(EINVAL);
        }
    }

    iamf_audio_element = stg->params.iamf_audio_element;
    if (iamf_audio_element->audio_element_type == AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE) {
        if (iamf_audio_element->nb_layers != 1) {
            av_log(log_ctx, AV_LOG_ERROR, "Invalid amount of layers for SCENE_BASED audio element. Must be 1\n");
            return AVERROR(EINVAL);
        }

        /* Only valid once the guard above established that a layer is present. */
        const AVIAMFLayer *layer = iamf_audio_element->layers[0];
        if (layer->ch_layout.order != AV_CHANNEL_ORDER_CUSTOM &&
            layer->ch_layout.order != AV_CHANNEL_ORDER_AMBISONIC) {
            av_log(log_ctx, AV_LOG_ERROR, "Invalid channel layout for SCENE_BASED audio element\n");
            return AVERROR(EINVAL);
        }
        /* Compared against the modes themselves rather than bounded from above, the field
         * being an enum whose underlying type may be signed, which would let a negative
         * value pass a bound and reach a serializer that has no branch for it. */
        if (layer->ambisonics_mode != AV_IAMF_AMBISONICS_MODE_MONO &&
            layer->ambisonics_mode != AV_IAMF_AMBISONICS_MODE_PROJECTION) {
            av_log(log_ctx, AV_LOG_ERROR, "Unsupported ambisonics mode %d\n", layer->ambisonics_mode);
            return AVERROR_PATCHWELCOME;
        }
        for (int i = 0; i < stg->nb_streams; i++) {
            if (stg->streams[i]->codecpar->ch_layout.nb_channels > 1) {
                av_log(log_ctx, AV_LOG_ERROR, "Invalid amount of channels in a stream for MONO mode ambisonics\n");
                return AVERROR(EINVAL);
            }
        }
    } else {
        AVBPrint bp;

        /* The layer count is both the bound of the loop that indexes
         * AVIAMFReconGain.recon_gain, whose extent is MAX_IAMF_LAYERS, and the value of
         * the 3 bit num_layers field written by scalable_channel_layout_config(), so it
         * has to be bounded above as well as below. Same invariant the parser enforces
         * when demuxing. */
        if (iamf_audio_element->nb_layers < 1 ||
            iamf_audio_element->nb_layers > MAX_IAMF_LAYERS) {
            av_log(log_ctx, AV_LOG_ERROR, "Invalid amount of layers %u in Audio Element id %"PRId64
                   " for CHANNEL_BASED audio element. Must be >= 1 and <= %d\n",
                   iamf_audio_element->nb_layers, stg->id, MAX_IAMF_LAYERS);
            return AVERROR(EINVAL);
        }

        for (int j, i = 0; i < iamf_audio_element->nb_layers; i++) {
            const AVIAMFLayer *layer = iamf_audio_element->layers[i];

            /* Matching by channel mask alone lets a custom-order layout that repeats a
             * channel pass as a standard layout carrying fewer channels, leaving the
             * amount of channels the layer actually has at odds with the layout
             * serialized for it and with every capacity derived from that layout.
             * Require the channel count to agree as well. */
            for (j = 0; j < FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts); j++)
                if (av_channel_layout_subset(&layer->ch_layout, UINT64_MAX) ==
                    av_channel_layout_subset(&ff_iamf_scalable_ch_layouts[j], UINT64_MAX) &&
                    layer->ch_layout.nb_channels == ff_iamf_scalable_ch_layouts[j].nb_channels)
                    break;

            if (j >= FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts)) {
                for (j = 0; j < FF_ARRAY_ELEMS(ff_iamf_expanded_scalable_ch_layouts); j++)
                    if (av_channel_layout_subset(&layer->ch_layout, UINT64_MAX) ==
                        av_channel_layout_subset(&ff_iamf_expanded_scalable_ch_layouts[j], UINT64_MAX) &&
                        layer->ch_layout.nb_channels == ff_iamf_expanded_scalable_ch_layouts[j].nb_channels)
                        break;

                if (j >= FF_ARRAY_ELEMS(ff_iamf_expanded_scalable_ch_layouts)) {
                    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
                    av_channel_layout_describe_bprint(&layer->ch_layout, &bp);
                    av_log(log_ctx, AV_LOG_ERROR, "Unsupported channel layout in Audio Element id %"PRId64
                           ", Layer %d: %s\n",
                           stg->id, i, bp.str);
                    av_bprint_finalize(&bp, NULL);
                    return AVERROR(EINVAL);
                }

                /* A layer only the expanded table matches is written as loudspeaker_layout
                 * 15 followed by an expanded_loudspeaker_layout, and that field is only
                 * present when num_layers is one. Same invariant the parser enforces when
                 * demuxing. */
                if (iamf_audio_element->nb_layers != 1) {
                    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
                    av_channel_layout_describe_bprint(&layer->ch_layout, &bp);
                    av_log(log_ctx, AV_LOG_ERROR, "Expanded channel layout in Audio Element id %"PRId64
                           ", Layer %d: %s. Only an Audio Element with a single layer may use one\n",
                           stg->id, i, bp.str);
                    av_bprint_finalize(&bp, NULL);
                    return AVERROR(EINVAL);
                }
            }

            if (!i)
                continue;

            const AVIAMFLayer *prev_layer = iamf_audio_element->layers[i-1];
            uint64_t prev_mask = av_channel_layout_subset(&prev_layer->ch_layout, UINT64_MAX);
            /* A layer carries the channels of the one before it plus at least one more,
             * except after a Mono layer, which the layer following it may drop the center
             * channel of. Same exception the parser makes when demuxing. */
            if ((prev_layer->ch_layout.nb_channels > 1 &&
                 av_channel_layout_subset(&layer->ch_layout, prev_mask) != prev_mask) ||
                layer->ch_layout.nb_channels <= prev_layer->ch_layout.nb_channels) {
                av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
                av_bprintf(&bp, "Channel layout \"");
                av_channel_layout_describe_bprint(&layer->ch_layout, &bp);
                av_bprintf(&bp, "\" can't follow channel layout \"");
                av_channel_layout_describe_bprint(&prev_layer->ch_layout, &bp);
                av_bprintf(&bp, "\" in Scalable Audio Element id %"PRId64, stg->id);
                av_log(log_ctx, AV_LOG_ERROR, "%s\n", bp.str);
                av_bprint_finalize(&bp, NULL);
                return AVERROR(EINVAL);
            }
        }
    }

    for (int i = 0; i < iamf->nb_audio_elements; i++) {
        if (stg->id == iamf->audio_elements[i]->audio_element_id) {
            av_log(log_ctx, AV_LOG_ERROR, "Duplicated Audio Element id %"PRId64"\n", stg->id);
            return AVERROR(EINVAL);
        }
    }

    codec_config = av_mallocz(sizeof(*codec_config));
    if (!codec_config)
        return AVERROR(ENOMEM);

    ret = fill_codec_config(iamf, stg, codec_config);
    if (ret < 0) {
        av_free(codec_config);
        return ret;
    }

    audio_element = av_mallocz(sizeof(*audio_element));
    if (!audio_element)
        return AVERROR(ENOMEM);

    audio_element->celement = stg->params.iamf_audio_element;
    audio_element->audio_element_id = stg->id;
    audio_element->codec_config_id = ret;

    audio_element->substreams = av_calloc(stg->nb_streams, sizeof(*audio_element->substreams));
    if (!audio_element->substreams) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    audio_element->nb_substreams = stg->nb_streams;

    audio_element->layers = av_calloc(iamf_audio_element->nb_layers, sizeof(*audio_element->layers));
    if (!audio_element->layers) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    /* Record the extent of the array allocated above, the layer count validated by the
     * guard further up. The count in the AVIAMFAudioElement may grow afterwards, through
     * the public av_iamf_audio_element_add_layer() among others, and the descriptors are
     * serialized again when writing the trailer, so everything that indexes this array or
     * derives a capacity from it has to read this copy instead. Same field the parser
     * fills in when demuxing. */
    audio_element->nb_layers = iamf_audio_element->nb_layers;

    int substream_idx = 0;
    for (int i = 0; i < iamf_audio_element->nb_layers; i++) {
        int nb_channels = iamf_audio_element->layers[i]->ch_layout.nb_channels;

        IAMFLayer *layer = &audio_element->layers[i];

        /* Recorded alongside the counts below, for the same reason: the descriptor
         * declares it, the parameter blocks written per packet long afterwards are read
         * per layer that declared it, and the flag it comes from stays writable. */
        layer->recon_gain_present =
            !!(iamf_audio_element->layers[i]->flags & AV_IAMF_LAYER_FLAG_RECON_GAIN);

        if (i)
            nb_channels -= iamf_audio_element->layers[i - 1]->ch_layout.nb_channels;
        for (; nb_channels > 0 && substream_idx < stg->nb_streams; substream_idx++) {
            const AVStream *st = stg->streams[substream_idx];
            IAMFSubStream *substream = &audio_element->substreams[substream_idx];

            substream->audio_substream_id = st->id;
            layer->substream_count++;
            layer->coupled_substream_count += st->codecpar->ch_layout.nb_channels == 2;
            nb_channels -= st->codecpar->ch_layout.nb_channels;
        }
        if (nb_channels) {
            av_log(log_ctx, AV_LOG_ERROR, "Invalid channel count across substreams in layer %u from stream group %u\n",
                   i, stg->index);
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }
    /* The layers must between them account for every substream. Any left over remains a
     * zero initialized entry of the array allocated above and would be serialized as a
     * phantom substream. Same cross-check the parser applies when demuxing. */
    if (substream_idx != stg->nb_streams) {
        av_log(log_ctx, AV_LOG_ERROR, "Invalid substream count in stream group %u: its layers "
               "account for %d of %u substreams\n",
               stg->index, substream_idx, stg->nb_streams);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    for (int i = 0; i < audio_element->nb_substreams; i++) {
        for (int j = i + 1; j < audio_element->nb_substreams; j++)
            if (audio_element->substreams[i].audio_substream_id ==
                audio_element->substreams[j].audio_substream_id) {
                av_log(log_ctx, AV_LOG_ERROR, "Duplicate id %u in streams %u and %u from stream group %u\n",
                       audio_element->substreams[i].audio_substream_id, i, j, stg->index);
                ret = AVERROR(EINVAL);
                goto fail;
            }
    }

    if (iamf_audio_element->demixing_info) {
        AVIAMFParamDefinition *param = iamf_audio_element->demixing_info;
        const IAMFParamDefinition *param_definition = ff_iamf_get_param_definition(iamf, param->parameter_id);

        if (param->nb_subblocks != 1) {
            av_log(log_ctx, AV_LOG_ERROR, "nb_subblocks in demixing_info for stream group %u is not 1\n", stg->index);
            ret = AVERROR(EINVAL);
            goto fail;
        }

        /* Only the type of the definition the role carries can be required here. Whether
         * this role ends up serialized at all depends on the layers and the codec, and is
         * not decided until the descriptor is written, so an id shared with a role that is
         * never written is not a conflict. iamf_write_audio_element() requires the
         * registered definition to agree for the roles it does write. */
        ret = check_param_definition_type(param, NULL,
                                          AV_IAMF_PARAMETER_DEFINITION_DEMIXING,
                                          "demixing_info", log_ctx);
        if (ret < 0)
            goto fail;

        if (!param_definition) {
            ret = add_param_definition(iamf, param, audio_element, log_ctx);
            if (ret < 0)
                goto fail;
        }
    }
    if (iamf_audio_element->recon_gain_info) {
        AVIAMFParamDefinition *param = iamf_audio_element->recon_gain_info;
        const IAMFParamDefinition *param_definition = ff_iamf_get_param_definition(iamf, param->parameter_id);

        if (param->nb_subblocks != 1) {
            av_log(log_ctx, AV_LOG_ERROR, "nb_subblocks in recon_gain_info for stream group %u is not 1\n", stg->index);
            ret = AVERROR(EINVAL);
            goto fail;
        }

        ret = check_param_definition_type(param, NULL,
                                          AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN,
                                          "recon_gain_info", log_ctx);
        if (ret < 0)
            goto fail;

        if (!param_definition) {
            ret = add_param_definition(iamf, param, audio_element, log_ctx);
            if (ret < 0)
                goto fail;
        }
    }

    tmp = av_realloc_array(iamf->audio_elements, iamf->nb_audio_elements + 1, sizeof(*iamf->audio_elements));
    if (!tmp) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    iamf->audio_elements = tmp;
    iamf->audio_elements[iamf->nb_audio_elements++] = audio_element;

    return 0;
fail:
    ff_iamf_free_audio_element(&audio_element);
    return ret;
}

int ff_iamf_add_mix_presentation(IAMFContext *iamf, const AVStreamGroup *stg, void *log_ctx)
{
    IAMFMixPresentation **tmp, *mix_presentation;
    int ret;

    if (stg->type != AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION)
        return AVERROR(EINVAL);
    if (!stg->nb_streams) {
        av_log(log_ctx, AV_LOG_ERROR, "Mix Presentation id %"PRId64" has no streams\n", stg->id);
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < iamf->nb_mix_presentations; i++) {
        if (stg->id == iamf->mix_presentations[i]->mix_presentation_id) {
            av_log(log_ctx, AV_LOG_ERROR, "Duplicate Mix Presentation id %"PRId64"\n", stg->id);
            return AVERROR(EINVAL);
        }
    }

    mix_presentation = av_mallocz(sizeof(*mix_presentation));
    if (!mix_presentation)
        return AVERROR(ENOMEM);

    mix_presentation->cmix = stg->params.iamf_mix_presentation;
    mix_presentation->mix_presentation_id = stg->id;

    for (int i = 0; i < mix_presentation->cmix->nb_submixes; i++) {
        const AVIAMFSubmix *submix = mix_presentation->cmix->submixes[i];
        AVIAMFParamDefinition *param = submix->output_mix_config;
        IAMFParamDefinition *param_definition;

        if (!param) {
            av_log(log_ctx, AV_LOG_ERROR, "output_mix_config is not present in submix %u from "
                                          "Mix Presentation ID %"PRId64"\n", i, stg->id);
            ret = AVERROR(EINVAL);
            goto fail;
        }

        param_definition = ff_iamf_get_param_definition(iamf, param->parameter_id);
        ret = check_param_definition_type(param, param_definition,
                                          AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN,
                                          "output_mix_config", log_ctx);
        if (ret < 0)
            goto fail;

        if (!param_definition) {
            ret = add_param_definition(iamf, param, NULL, log_ctx);
            if (ret < 0)
                goto fail;
        }

        for (int j = 0; j < submix->nb_elements; j++) {
            const AVIAMFSubmixElement *element = submix->elements[j];
            param = element->element_mix_config;

            if (!param) {
                av_log(log_ctx, AV_LOG_ERROR, "element_mix_config is not present for element %u in submix %u from "
                                              "Mix Presentation ID %"PRId64"\n", j, i, stg->id);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            param_definition = ff_iamf_get_param_definition(iamf, param->parameter_id);
            ret = check_param_definition_type(param, param_definition,
                                              AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN,
                                              "element_mix_config", log_ctx);
            if (ret < 0)
                goto fail;

            if (!param_definition) {
                ret = add_param_definition(iamf, param, NULL, log_ctx);
                if (ret < 0)
                    goto fail;
            }
        }
    }

    tmp = av_realloc_array(iamf->mix_presentations, iamf->nb_mix_presentations + 1, sizeof(*iamf->mix_presentations));
    if (!tmp) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    iamf->mix_presentations = tmp;
    iamf->mix_presentations[iamf->nb_mix_presentations++] = mix_presentation;

    return 0;
fail:
    ff_iamf_free_mix_presentation(&mix_presentation);
    return ret;
}

static int iamf_write_codec_config(const IAMFContext *iamf,
                                   const IAMFCodecConfig *codec_config,
                                   AVIOContext *pb)
{
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    AVIOContext *dyn_bc;
    uint8_t *dyn_buf = NULL;
    PutBitContext pbc;
    int dyn_size;

    int ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    ffio_write_leb(dyn_bc, codec_config->codec_config_id);
    avio_wl32(dyn_bc, codec_config->codec_tag);

    ffio_write_leb(dyn_bc, codec_config->nb_samples);
    avio_wb16(dyn_bc, codec_config->audio_roll_distance);

    switch(codec_config->codec_id) {
    case AV_CODEC_ID_OPUS:
        avio_write(dyn_bc, codec_config->extradata, codec_config->extradata_size);
        break;
    case AV_CODEC_ID_AAC:
        ret = AVERROR_PATCHWELCOME;
        goto fail;
    case AV_CODEC_ID_FLAC:
        avio_w8(dyn_bc, 0x80);
        avio_wb24(dyn_bc, codec_config->extradata_size);
        avio_write(dyn_bc, codec_config->extradata, codec_config->extradata_size);
        break;
    case AV_CODEC_ID_PCM_S16LE:
        avio_w8(dyn_bc, 1);
        avio_w8(dyn_bc, 16);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    case AV_CODEC_ID_PCM_S24LE:
        avio_w8(dyn_bc, 1);
        avio_w8(dyn_bc, 24);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    case AV_CODEC_ID_PCM_S32LE:
        avio_w8(dyn_bc, 1);
        avio_w8(dyn_bc, 32);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    case AV_CODEC_ID_PCM_S16BE:
        avio_w8(dyn_bc, 0);
        avio_w8(dyn_bc, 16);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    case AV_CODEC_ID_PCM_S24BE:
        avio_w8(dyn_bc, 0);
        avio_w8(dyn_bc, 24);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    case AV_CODEC_ID_PCM_S32BE:
        avio_w8(dyn_bc, 0);
        avio_w8(dyn_bc, 32);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    default:
        break;
    }

    init_put_bits(&pbc, header, sizeof(header));
    put_bits(&pbc, 5, IAMF_OBU_IA_CODEC_CONFIG);
    put_bits(&pbc, 3, 0);
    flush_put_bits(&pbc);

    dyn_size = avio_get_dyn_buf(dyn_bc, &dyn_buf);
    avio_write(pb, header, put_bytes_count(&pbc, 1));
    ffio_write_leb(pb, dyn_size);
    avio_write(pb, dyn_buf, dyn_size);
    ret = 0;
/* Every exit past avio_open_dyn_buf() comes through here, or the partially
 * serialized OBU leaks. */
fail:
    ffio_free_dyn_buf(&dyn_bc);

    return ret;
}

static inline int rescale_rational(AVRational q, int b)
{
    return av_clip_int16(av_rescale(q.num, b, q.den));
}

/**
 * Check that a rational the caller supplied may be scaled into the fixed point form
 * these descriptors carry.
 *
 * rescale_rational() and the subblock writers scale a rational with av_rescale(), which
 * requires a positive denominator: a non-positive one is only objected to by av_assert2,
 * compiled out of a default build, and yields INT64_MIN instead, so the value serialized
 * would be the clip of that rather than anything the caller expressed. The numerator is
 * left unconstrained, every one of these fields being documented as a range around zero
 * and serialized into a signed field.
 *
 * @param field what the rational is, for the diagnostic
 * @param where what carries it, for the diagnostic
 * @param idx   index of @p where among its siblings, for the diagnostic
 * @return 0, or AVERROR(EINVAL) having reported the offending value
 */
static int check_rational(AVRational q, const char *field,
                          const char *where, int idx, void *log_ctx)
{
    if (q.den > 0)
        return 0;

    av_log(log_ctx, AV_LOG_ERROR, "%s %d carries an invalid %s %d/%d. The denominator "
           "must be positive\n", where, idx, field, q.num, q.den);

    return AVERROR(EINVAL);
}

/**
 * Resolve the loudspeaker_layout, and where only an expanded loudspeaker layout
 * describes it the expanded_loudspeaker_layout, a layer's channel layout is
 * serialized as.
 *
 * Every scan requires the channel count to agree with the table entry's, so a
 * layout carrying more channels than the entry it shares a channel mask with is
 * not matched and the layout written for a layer can never contradict the
 * channels that layer is accounted for.
 *
 * @param pexpanded_layout receives an index into
 *                         ff_iamf_expanded_scalable_ch_layouts, or -1 when the
 *                         layer is described by a standard loudspeaker layout
 * @return 0 on success, a negative AVERROR code if neither table describes
 *         @p layer, in which case it cannot be serialized at all
 */
static int get_loudspeaker_layout(const AVIAMFLayer *layer,
                                  int *playout, int *pexpanded_layout,
                                  void *log_ctx)
{
    int layout, expanded_layout = -1;

    for (layout = 0; layout < FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts); layout++) {
        if (!av_channel_layout_compare(&layer->ch_layout, &ff_iamf_scalable_ch_layouts[layout]))
            break;
    }
    if (layout >= FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts)) {
        /* Mask only fallback, so it has to require the channel count to agree too,
         * exactly like the matching done when the audio element was added. */
        for (layout = 0; layout < FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts); layout++)
            if (av_channel_layout_subset(&layer->ch_layout, UINT64_MAX) ==
                av_channel_layout_subset(&ff_iamf_scalable_ch_layouts[layout], UINT64_MAX) &&
                layer->ch_layout.nb_channels == ff_iamf_scalable_ch_layouts[layout].nb_channels)
                break;
    }
    if (layout >= FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts)) {
        layout = 15;
        for (expanded_layout = 0; expanded_layout < FF_ARRAY_ELEMS(ff_iamf_expanded_scalable_ch_layouts); expanded_layout++) {
            if (!av_channel_layout_compare(&layer->ch_layout, &ff_iamf_expanded_scalable_ch_layouts[expanded_layout]))
                break;
        }
        if (expanded_layout >= FF_ARRAY_ELEMS(ff_iamf_expanded_scalable_ch_layouts)) {
            for (expanded_layout = 0; expanded_layout < FF_ARRAY_ELEMS(ff_iamf_expanded_scalable_ch_layouts); expanded_layout++)
                if (av_channel_layout_subset(&layer->ch_layout, UINT64_MAX) ==
                    av_channel_layout_subset(&ff_iamf_expanded_scalable_ch_layouts[expanded_layout], UINT64_MAX) &&
                    layer->ch_layout.nb_channels == ff_iamf_expanded_scalable_ch_layouts[expanded_layout].nb_channels)
                    break;
        }
    }
    /* A match leaves expanded_layout within the expanded table, index 0, the LFE-only
     * layout, included; searching that table and matching nothing leaves it at the table's
     * extent, and a layer a standard loudspeaker layout describes leaves it at -1, never
     * having searched it. A layer neither table describes has no layout to be written as;
     * the layouts come from input, so that is reported and refused rather than asserted
     * on. */
    if (!((expanded_layout >= 0 && expanded_layout < FF_ARRAY_ELEMS(ff_iamf_expanded_scalable_ch_layouts)) ||
          layout < FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts))) {
        AVBPrint bp;

        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
        av_channel_layout_describe_bprint(&layer->ch_layout, &bp);
        av_log(log_ctx, AV_LOG_ERROR, "No loudspeaker layout describes the channel layout "
               "of a layer: %s\n", bp.str);
        av_bprint_finalize(&bp, NULL);
        return AVERROR(EINVAL);
    }

    *playout = layout;
    *pexpanded_layout = expanded_layout;

    return 0;
}

static int scalable_channel_layout_config(const IAMFAudioElement *audio_element,
                                          AVIOContext *dyn_bc, void *log_ctx)
{
    const AVIAMFAudioElement *element = audio_element->celement;
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    PutBitContext pb;
    /* Both the 3-bit num_layers field and the amount of layer records following it have to
     * come from the validated layer count the audio_element->layers array was allocated
     * with, and not from the count in the AVIAMFAudioElement, which the caller may grow
     * after this element was added. Clamped as well, so neither the field nor that array
     * can be exceeded even if this is reached without ff_iamf_add_audio_element(). */
    const unsigned nb_layers = FFMIN(audio_element->nb_layers, MAX_IAMF_LAYERS);

    init_put_bits(&pb, header, sizeof(header));
    put_bits(&pb, 3, nb_layers);
    put_bits(&pb, 5, 0);
    flush_put_bits(&pb);
    avio_write(dyn_bc, header, put_bytes_count(&pb, 1));
    for (int i = 0; i < nb_layers; i++) {
        const AVIAMFLayer *layer = element->layers[i];
        int layout, expanded_layout, ret;

        ret = get_loudspeaker_layout(layer, &layout, &expanded_layout, log_ctx);
        if (ret < 0)
            return ret;

        init_put_bits(&pb, header, sizeof(header));
        put_bits(&pb, 4, layout);
        put_bits(&pb, 1, !!layer->output_gain_flags);
        /* The flag the layer carried when the element was validated, which is what the
         * parameter blocks written later are measured against. */
        put_bits(&pb, 1, audio_element->layers[i].recon_gain_present);
        put_bits(&pb, 2, 0); // reserved
        put_bits(&pb, 8, audio_element->layers[i].substream_count);
        put_bits(&pb, 8, audio_element->layers[i].coupled_substream_count);
        if (layer->output_gain_flags) {
            put_bits(&pb, 6, layer->output_gain_flags);
            put_bits(&pb, 2, 0);
            /* output_gain is a signed 16 bit field, and rescale_rational() returns the
             * signed value it holds. put_bits() takes an unsigned one and only objects to
             * a wider value under av_assert2, compiled out of a default build, where a
             * negative gain would instead be written across the bits around it. Narrow it
             * to the field's two's complement form here, which is the form the parser
             * sign extends back. */
            put_bits(&pb, 16, rescale_rational(layer->output_gain, 1 << 8) & 0xFFFF);
        }
        if (expanded_layout >= 0)
            put_bits(&pb, 8, expanded_layout);
        flush_put_bits(&pb);
        avio_write(dyn_bc, header, put_bytes_count(&pb, 1));
    }

    return 0;
}

/**
 * Check that a SCENE_BASED Audio Element's layer describes an Ambisonics configuration
 * ambisonics_config() can serialize in full.
 *
 * Every invariant here mirrors the one libavformat/iamf_parse.c enforces on the same field
 * when demuxing, so a configuration accepted for writing is one that parser reads back:
 * the mode selects between the two serializations and nothing else has one at all;
 * output_channel_count, substream_count and coupled_substream_count each go into a single
 * byte; output_channel_count is a complete ambisonic order; a custom order map is written
 * out as one ACN index per channel, so it may only name ambisonic channels whose index fits
 * that byte; and a projection mode reads a demixing matrix whose extent the counts fix.
 *
 * Only valid once the caller has established that a layer is present.
 */
static int check_ambisonics_layer(const IAMFAudioElement *audio_element, void *log_ctx)
{
    const AVIAMFLayer *layer = audio_element->celement->layers[0];
    const IAMFLayer *ilayer = &audio_element->layers[0];
    const int nb_channels = layer->ch_layout.nb_channels;
    int order = 0;

    /* Each mode selects a serialization of its own and a third value has none. Compared
     * against the modes themselves rather than bounded from above, the field being an enum
     * whose underlying type may be signed, which would let a negative value pass a bound
     * and then match neither branch below. */
    if (layer->ambisonics_mode != AV_IAMF_AMBISONICS_MODE_MONO &&
        layer->ambisonics_mode != AV_IAMF_AMBISONICS_MODE_PROJECTION) {
        av_log(log_ctx, AV_LOG_ERROR, "Unsupported ambisonics mode %d in Audio Element id %u\n",
               layer->ambisonics_mode, audio_element->audio_element_id);
        return AVERROR_PATCHWELCOME;
    }

    /* Serialized into the single byte output_channel_count field. */
    if (nb_channels < 1 || nb_channels > UINT8_MAX) {
        av_log(log_ctx, AV_LOG_ERROR, "Invalid amount of channels %d in Audio Element id %u. "
               "Must be >= 1 and <= %d\n",
               nb_channels, audio_element->audio_element_id, UINT8_MAX);
        return AVERROR(EINVAL);
    }
    /* An Ambisonics layout carries every harmonic up to its order, so the channel count is
     * (order + 1)^2. Searched for rather than computed from a square root, so no floating
     * point is needed; the bound above keeps the search to at most sixteen steps. */
    while ((order + 1) * (order + 1) < nb_channels)
        order++;
    if ((order + 1) * (order + 1) != nb_channels) {
        av_log(log_ctx, AV_LOG_ERROR, "Incomplete ambisonic order for %d channels in Audio "
               "Element id %u. Some harmonics are missing\n",
               nb_channels, audio_element->audio_element_id);
        return AVERROR(EINVAL);
    }

    /* substream_count is serialized from the substreams the element was given, into a
     * single byte, and the count recorded for the layer is what everything else is
     * accounted against. */
    if (audio_element->nb_substreams != ilayer->substream_count) {
        av_log(log_ctx, AV_LOG_ERROR, "Audio Element id %u carries %u substreams, %u accounted "
               "for by its layer\n", audio_element->audio_element_id,
               audio_element->nb_substreams, ilayer->substream_count);
        return AVERROR(EINVAL);
    }
    if (audio_element->nb_substreams > UINT8_MAX ||
        ilayer->coupled_substream_count > UINT8_MAX) {
        av_log(log_ctx, AV_LOG_ERROR, "Invalid substream counts %u and %u in Audio Element id "
               "%u. Both must be <= %d\n", audio_element->nb_substreams,
               ilayer->coupled_substream_count, audio_element->audio_element_id, UINT8_MAX);
        return AVERROR(EINVAL);
    }

    if (layer->ambisonics_mode == AV_IAMF_AMBISONICS_MODE_MONO) {
        /* A custom order map is serialized as the ACN index of each channel it names, one
         * byte each, so a channel that is not an ambisonic one has no index to be written
         * as. Same range the parser produces, reading a byte and adding the base to it. */
        if (layer->ch_layout.order == AV_CHANNEL_ORDER_CUSTOM)
            for (int i = 0; i < nb_channels; i++) {
                const int id = layer->ch_layout.u.map[i].id;

                if (id < AV_CHAN_AMBISONIC_BASE ||
                    id > AV_CHAN_AMBISONIC_BASE + UINT8_MAX) {
                    av_log(log_ctx, AV_LOG_ERROR, "Channel %d of Audio Element id %u is not an "
                           "Ambisonics channel with an ACN index that fits in one byte\n",
                           i, audio_element->audio_element_id);
                    return AVERROR(EINVAL);
                }
            }
    } else {
        /* The demixing matrix is read once per substream and channel, so the counts above
         * fix its extent, and it has to be there to be read at all: a layer may carry a
         * matching count with no matrix set. */
        const unsigned nb_demixing_matrix =
            (ilayer->substream_count + ilayer->coupled_substream_count) * nb_channels;

        if (!layer->demixing_matrix || layer->nb_demixing_matrix != nb_demixing_matrix) {
            av_log(log_ctx, AV_LOG_ERROR, "Audio Element id %u declares %u demixing matrix "
                   "entries%s, %u accounted for by its substreams and channels\n",
                   audio_element->audio_element_id, layer->nb_demixing_matrix,
                   layer->demixing_matrix ? "" : " with no matrix set", nb_demixing_matrix);
            return AVERROR(EINVAL);
        }

        /* Every entry is scaled into a signed fixed point field. */
        for (int i = 0; i < layer->nb_demixing_matrix; i++) {
            int ret = check_rational(layer->demixing_matrix[i], "value",
                                     "Demixing matrix entry", i, log_ctx);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static int ambisonics_config(const IAMFAudioElement *audio_element,
                             AVIOContext *dyn_bc, void *log_ctx)
{
    const AVIAMFAudioElement *element = audio_element->celement;
    const IAMFLayer *ilayer = &audio_element->layers[0];
    const AVIAMFLayer *layer = element->layers[0];
    /* Checked again here, and not only by the validation pass running before any of this,
     * so nothing below is reached with a configuration it has no serialization for however
     * this function is arrived at. */
    int ret = check_ambisonics_layer(audio_element, log_ctx);

    if (ret < 0)
        return ret;

    ffio_write_leb(dyn_bc, layer->ambisonics_mode);
    avio_w8(dyn_bc, layer->ch_layout.nb_channels); // output_channel_count
    avio_w8(dyn_bc, audio_element->nb_substreams); // substream_count

    if (layer->ambisonics_mode == AV_IAMF_AMBISONICS_MODE_MONO) {
        if (layer->ch_layout.order == AV_CHANNEL_ORDER_AMBISONIC)
            for (int i = 0; i < layer->ch_layout.nb_channels; i++)
                avio_w8(dyn_bc, i);
        else
            /* channel_mapping is a list of ACN indices, which is what an ambisonic channel
             * id is relative to AV_CHAN_AMBISONIC_BASE, and the form the parser reads back
             * by adding that base to the byte it read. */
            for (int i = 0; i < layer->ch_layout.nb_channels; i++)
                avio_w8(dyn_bc, layer->ch_layout.u.map[i].id - AV_CHAN_AMBISONIC_BASE);
    } else {
        avio_w8(dyn_bc, ilayer->coupled_substream_count);
        for (int i = 0; i < layer->nb_demixing_matrix; i++)
            avio_wb16(dyn_bc, rescale_rational(layer->demixing_matrix[i], 1 << 15));
    }

    return 0;
}

static int param_definition(const IAMFContext *iamf,
                            const IAMFParamDefinition *param_def,
                            AVIOContext *dyn_bc, void *log_ctx)
{
    /* Nothing is registered under the parameter id the descriptor being serialized names,
     * so there is no definition to write. The validation passes running before any of this
     * report which descriptor is at fault; this keeps the lookup they cover from being
     * dereferenced whatever reaches here. */
    if (!param_def) {
        av_log(log_ctx, AV_LOG_ERROR, "A descriptor references a Parameter Definition that was never added\n");
        return AVERROR(EINVAL);
    }

    const AVIAMFParamDefinition *param = param_def->param;

    ffio_write_leb(dyn_bc, param->parameter_id);
    ffio_write_leb(dyn_bc, param->parameter_rate);
    avio_w8(dyn_bc, param->duration ? 0 : 1 << 7);
    if (param->duration) {
        ffio_write_leb(dyn_bc, param->duration);
        ffio_write_leb(dyn_bc, param->constant_subblock_duration);
        if (param->constant_subblock_duration == 0) {
            ffio_write_leb(dyn_bc, param->nb_subblocks);
            for (int i = 0; i < param->nb_subblocks; i++) {
                const void *subblock = av_iamf_param_definition_get_subblock(param, i);

                switch (param->type) {
                case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN: {
                    const AVIAMFMixGain *mix = subblock;
                    ffio_write_leb(dyn_bc, mix->subblock_duration);
                    break;
                }
                case AV_IAMF_PARAMETER_DEFINITION_DEMIXING: {
                    const AVIAMFDemixingInfo *demix = subblock;
                    ffio_write_leb(dyn_bc, demix->subblock_duration);
                    break;
                }
                case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN: {
                    const AVIAMFReconGain *recon = subblock;
                    ffio_write_leb(dyn_bc, recon->subblock_duration);
                    break;
                }
                }
            }
        }
    }

    return 0;
}

/**
 * Resolve which Parameter Definition types an Audio Element's descriptor declares.
 *
 * Every decision here describes what iamf_write_audio_element() is about to serialize, and
 * two of them refuse the element outright: a scalable element whose highest loudspeaker
 * layout needs demixing information it was not given, and one that needs recon gain
 * information and was not given that. Deciding it in its own function lets
 * validate_audio_element() reach the same verdict before ff_iamf_write_descriptors() has
 * written a single OBU, so an Audio Element that cannot be described produces a diagnostic
 * and no output at all, rather than a Sequence Header and a Codec Config followed by an
 * error.
 *
 * @param types set to the mask of AVIAMFParamDefinitionType values the descriptor declares.
 *              Zero for a scene based element, which declares none.
 * @return 0, or a negative AVERROR code if no descriptor can be written for @p audio_element
 */
static int get_param_definition_types(const IAMFContext *iamf,
                                      const IAMFAudioElement *audio_element,
                                      int *types, void *log_ctx)
{
    const AVIAMFAudioElement *element = audio_element->celement;
    const IAMFCodecConfig *codec_config = iamf->codec_configs[audio_element->codec_config_id];
    int param_definition_types = AV_IAMF_PARAMETER_DEFINITION_DEMIXING;
    int layout = 0, expanded_layout = 0, ret;
    unsigned nb_layers;

    *types = 0;

    /* When audio_element_type = 1, num_parameters SHALL be set to 0 */
    if (element->audio_element_type == AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE)
        return 0;

    /* Every decision below describes the layers scalable_channel_layout_config() is
     * about to serialize, so it has to read the same validated layer count that
     * function does rather than the one in the AVIAMFAudioElement, which the caller
     * may have grown since this element was added. */
    nb_layers = FFMIN(audio_element->nb_layers, MAX_IAMF_LAYERS);

    ret = get_loudspeaker_layout(element->layers[0], &layout, &expanded_layout, log_ctx);
    if (ret < 0)
        return ret;

    /* When the loudspeaker_layout = 15, the type PARAMETER_DEFINITION_DEMIXING SHALL NOT be present. */
    if (layout == 15) {
        param_definition_types &= ~AV_IAMF_PARAMETER_DEFINITION_DEMIXING;
        /* expanded_loudspeaker_layout SHALL only be present when num_layers = 1 and loudspeaker_layout is set to 15 */
        if (nb_layers > 1) {
            av_log(log_ctx, AV_LOG_ERROR, "expanded_loudspeaker_layout present when using more than one layer in "
                                          "Stream Group #%u\n",
                   audio_element->audio_element_id);
            return AVERROR(EINVAL);
        }
    }
    /* When the loudspeaker_layout of the (non-)scalable channel audio (i.e., num_layers = 1) is less than or equal to 3.1.2ch,
     * (i.e., Mono, Stereo, or 3.1.2ch), the type PARAMETER_DEFINITION_DEMIXING SHALL NOT be present. */
    else if (nb_layers == 1 && (layout == 0 || layout == 1 || layout == 8))
        param_definition_types &= ~AV_IAMF_PARAMETER_DEFINITION_DEMIXING;
    /* When num_layers > 1, the type PARAMETER_DEFINITION_RECON_GAIN SHALL be present */
    if (nb_layers > 1)
        param_definition_types |= AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN;
    /* When codec_id = fLaC or ipcm, the type PARAMETER_DEFINITION_RECON_GAIN SHALL NOT be present. */
    if (codec_config->codec_tag == MKTAG('f','L','a','C') ||
        codec_config->codec_tag == MKTAG('i','p','c','m'))
        param_definition_types &= ~AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN;
    if ((param_definition_types & AV_IAMF_PARAMETER_DEFINITION_DEMIXING) && !element->demixing_info) {
        if (nb_layers > 1) {
            ret = get_loudspeaker_layout(element->layers[nb_layers-1], &layout,
                                         &expanded_layout, log_ctx);
            if (ret < 0)
                return ret;

            /* When the highest loudspeaker_layout of the scalable channel audio (i.e., num_layers > 1) is greater than 3.1.2ch,
             * (i.e., 5.1.2ch, 5.1.4ch, 7.1.2ch, or 7.1.4ch), type PARAMETER_DEFINITION_DEMIXING SHALL be present. */
            if (layout == 3 || layout == 4 || layout == 6 || layout == 7) {
                av_log(log_ctx, AV_LOG_ERROR, "demixing_info needed but not set in Stream Group #%u\n",
                       audio_element->audio_element_id);
                return AVERROR(EINVAL);
            }
        }
        param_definition_types &= ~AV_IAMF_PARAMETER_DEFINITION_DEMIXING;
    }
    /* The recon gain definition the descriptor declares is serialized out of the Audio
     * Element, so declaring the type without carrying one describes nothing. */
    if ((param_definition_types & AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN) &&
        !element->recon_gain_info) {
        av_log(log_ctx, AV_LOG_ERROR, "recon_gain_info needed but not set in Stream Group #%u\n",
               audio_element->audio_element_id);
        return AVERROR(EINVAL);
    }

    *types = param_definition_types;

    return 0;
}

static int iamf_write_audio_element(const IAMFContext *iamf,
                                    const IAMFAudioElement *audio_element,
                                    AVIOContext *pb, void *log_ctx)
{
    const AVIAMFAudioElement *element = audio_element->celement;
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    AVIOContext *dyn_bc;
    uint8_t *dyn_buf = NULL;
    PutBitContext pbc;
    int param_definition_types, dyn_size;

    /* Decided before anything is written, and again by validate_audio_element() before any
     * descriptor at all was, so this cannot be the first thing to refuse the element. */
    int ret = get_param_definition_types(iamf, audio_element, &param_definition_types, log_ctx);
    if (ret < 0)
        return ret;

    ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    ffio_write_leb(dyn_bc, audio_element->audio_element_id);

    init_put_bits(&pbc, header, sizeof(header));
    put_bits(&pbc, 3, element->audio_element_type);
    put_bits(&pbc, 5, 0);
    flush_put_bits(&pbc);
    avio_write(dyn_bc, header, put_bytes_count(&pbc, 1));

    ffio_write_leb(dyn_bc, audio_element->codec_config_id);
    ffio_write_leb(dyn_bc, audio_element->nb_substreams);

    for (int i = 0; i < audio_element->nb_substreams; i++)
        ffio_write_leb(dyn_bc, audio_element->substreams[i].audio_substream_id);

    ffio_write_leb(dyn_bc, av_popcount(param_definition_types)); // num_parameters

    if (param_definition_types & AV_IAMF_PARAMETER_DEFINITION_DEMIXING) {
        const AVIAMFParamDefinition *param = element->demixing_info;
        const IAMFParamDefinition *param_def;
        const AVIAMFDemixingInfo *demix;

        demix = av_iamf_param_definition_get_subblock(param, 0);
        ffio_write_leb(dyn_bc, AV_IAMF_PARAMETER_DEFINITION_DEMIXING); // type

        param_def = ff_iamf_get_param_definition(iamf, param->parameter_id);
        /* This role is written out of whatever is registered under the id it names, so that
         * definition has to be of the type just written above it. */
        ret = check_param_definition_type(param, param_def,
                                          AV_IAMF_PARAMETER_DEFINITION_DEMIXING,
                                          "demixing_info", log_ctx);
        if (ret < 0)
            goto fail;

        ret = param_definition(iamf, param_def, dyn_bc, log_ctx);
        if (ret < 0)
            goto fail;

        avio_w8(dyn_bc, demix->dmixp_mode << 5); // dmixp_mode
        avio_w8(dyn_bc, element->default_w << 4); // default_w
    }
    if (param_definition_types & AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN) {
        const AVIAMFParamDefinition *param = element->recon_gain_info;
        const IAMFParamDefinition *param_def;

        /* get_param_definition_types() only declares this type for an element carrying one,
         * so this cannot be reached; kept because the pointer is dereferenced below. */
        if (!param) {
            av_log(log_ctx, AV_LOG_ERROR, "recon_gain_info needed but not set in Stream Group #%u\n",
                   audio_element->audio_element_id);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        ffio_write_leb(dyn_bc, AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN); // type

        param_def = ff_iamf_get_param_definition(iamf, param->parameter_id);
        ret = check_param_definition_type(param, param_def,
                                          AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN,
                                          "recon_gain_info", log_ctx);
        if (ret < 0)
            goto fail;

        ret = param_definition(iamf, param_def, dyn_bc, log_ctx);
        if (ret < 0)
            goto fail;
    }

    if (element->audio_element_type == AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL) {
        ret = scalable_channel_layout_config(audio_element, dyn_bc, log_ctx);
        if (ret < 0)
            goto fail;
    } else {
        ret = ambisonics_config(audio_element, dyn_bc, log_ctx);
        if (ret < 0)
            goto fail;
    }

    init_put_bits(&pbc, header, sizeof(header));
    put_bits(&pbc, 5, IAMF_OBU_IA_AUDIO_ELEMENT);
    put_bits(&pbc, 3, 0);
    flush_put_bits(&pbc);

    dyn_size = avio_get_dyn_buf(dyn_bc, &dyn_buf);
    avio_write(pb, header, put_bytes_count(&pbc, 1));
    ffio_write_leb(pb, dyn_size);
    avio_write(pb, dyn_buf, dyn_size);
    ret = 0;
/* Every exit past avio_open_dyn_buf() comes through here, or the partially
 * serialized OBU leaks. */
fail:
    ffio_free_dyn_buf(&dyn_bc);

    return ret;
}

/**
 * Resolve the sound_system a submix layout's channel layout is serialized as.
 *
 * @return an index into ff_iamf_sound_system_map, or a negative AVERROR code if no sound
 *         system describes @p ch_layout, in which case the layout cannot be serialized.
 */
static int get_sound_system(const AVChannelLayout *ch_layout)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(ff_iamf_sound_system_map); i++)
        if (!av_channel_layout_compare(ch_layout, &ff_iamf_sound_system_map[i].layout))
            return i;

    return AVERROR(EINVAL);
}

static int iamf_write_mixing_presentation(const IAMFContext *iamf,
                                          const IAMFMixPresentation *mix_presentation,
                                          AVIOContext *pb, void *log_ctx)
{
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    const AVIAMFMixPresentation *mix = mix_presentation->cmix;
    const AVDictionaryEntry *tag = NULL;
    PutBitContext pbc;
    AVIOContext *dyn_bc;
    uint8_t *dyn_buf = NULL;
    int dyn_size;

    int ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    ffio_write_leb(dyn_bc, mix_presentation->mix_presentation_id); // mix_presentation_id
    ffio_write_leb(dyn_bc, av_dict_count(mix->annotations)); // count_label

    while ((tag = av_dict_iterate(mix->annotations, tag)))
        avio_put_str(dyn_bc, tag->key);
    while ((tag = av_dict_iterate(mix->annotations, tag)))
        avio_put_str(dyn_bc, tag->value);

    ffio_write_leb(dyn_bc, mix->nb_submixes);
    for (int i = 0; i < mix->nb_submixes; i++) {
        const AVIAMFSubmix *sub_mix = mix->submixes[i];
        const IAMFParamDefinition *param_def;

        ffio_write_leb(dyn_bc, sub_mix->nb_elements);
        for (int j = 0; j < sub_mix->nb_elements; j++) {
            const IAMFAudioElement *audio_element = NULL;
            const AVIAMFSubmixElement *submix_element = sub_mix->elements[j];

            for (int k = 0; k < iamf->nb_audio_elements; k++)
                if (iamf->audio_elements[k]->audio_element_id == submix_element->audio_element_id) {
                    audio_element = iamf->audio_elements[k];
                    break;
                }

            /* The search above may legitimately fail to match, so this is input
             * validation and must not be an assertion. */
            if (!audio_element) {
                av_log(log_ctx, AV_LOG_ERROR, "Invalid Audio Element id %u referenced by element %d in submix %d "
                                              "from Mix Presentation id #%u\n",
                       submix_element->audio_element_id, j, i,
                       mix_presentation->mix_presentation_id);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            ffio_write_leb(dyn_bc, submix_element->audio_element_id);

            if (av_dict_count(submix_element->annotations) != av_dict_count(mix->annotations)) {
                av_log(log_ctx, AV_LOG_ERROR, "Inconsistent amount of labels in submix %d from Mix Presentation id #%u\n",
                       j, audio_element->audio_element_id);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            while ((tag = av_dict_iterate(submix_element->annotations, tag)))
                avio_put_str(dyn_bc, tag->value);

            init_put_bits(&pbc, header, sizeof(header));
            put_bits(&pbc, 2, submix_element->headphones_rendering_mode);
            put_bits(&pbc, 6, 0); // reserved
            flush_put_bits(&pbc);
            avio_write(dyn_bc, header, put_bytes_count(&pbc, 1));
            ffio_write_leb(dyn_bc, 0); // rendering_config_extension_size

            param_def = ff_iamf_get_param_definition(iamf, submix_element->element_mix_config->parameter_id);
            /* A submix mix gain slot is always written, out of whatever is registered under
             * the id it names, so that definition has to be a mix gain one. Checked before
             * the Mix Presentations are serialized as well; kept here so no definition of
             * another kind can fill this slot whatever reaches it. */
            ret = check_param_definition_type(submix_element->element_mix_config, param_def,
                                              AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN,
                                              "element_mix_config", log_ctx);
            if (ret < 0)
                goto fail;

            ret = param_definition(iamf, param_def, dyn_bc, log_ctx);
            if (ret < 0)
                goto fail;

            avio_wb16(dyn_bc, rescale_rational(submix_element->default_mix_gain, 1 << 8));
        }

        param_def = ff_iamf_get_param_definition(iamf, sub_mix->output_mix_config->parameter_id);
        ret = check_param_definition_type(sub_mix->output_mix_config, param_def,
                                          AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN,
                                          "output_mix_config", log_ctx);
        if (ret < 0)
            goto fail;

        ret = param_definition(iamf, param_def, dyn_bc, log_ctx);
        if (ret < 0)
            goto fail;
        avio_wb16(dyn_bc, rescale_rational(sub_mix->default_mix_gain, 1 << 8));

        ffio_write_leb(dyn_bc, sub_mix->nb_layouts); // nb_layouts
        for (int i = 0; i < sub_mix->nb_layouts; i++) {
            const AVIAMFSubmixLayout *submix_layout = sub_mix->layouts[i];
            int layout, info_type;
            int dialogue = submix_layout->dialogue_anchored_loudness.num &&
                           submix_layout->dialogue_anchored_loudness.den;
            int album = submix_layout->album_anchored_loudness.num &&
                        submix_layout->album_anchored_loudness.den;

            if (submix_layout->layout_type == AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS) {
                layout = get_sound_system(&submix_layout->sound_system);
                if (layout < 0) {
                    av_log(log_ctx, AV_LOG_ERROR, "Invalid Sound System value in a submix\n");
                    ret = layout;
                    goto fail;
                }
            } else if (submix_layout->layout_type != AV_IAMF_SUBMIX_LAYOUT_TYPE_BINAURAL) {
                av_log(log_ctx, AV_LOG_ERROR, "Unsupported Layout Type value in a submix\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }
            init_put_bits(&pbc, header, sizeof(header));
            put_bits(&pbc, 2, submix_layout->layout_type); // layout_type
            if (submix_layout->layout_type == AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS) {
                put_bits(&pbc, 4, ff_iamf_sound_system_map[layout].id); // sound_system
                put_bits(&pbc, 2, 0); // reserved
            } else
                put_bits(&pbc, 6, 0); // reserved
            flush_put_bits(&pbc);
            avio_write(dyn_bc, header, put_bytes_count(&pbc, 1));

            info_type  = (submix_layout->true_peak.num && submix_layout->true_peak.den);
            info_type |= (dialogue || album) << 1;
            avio_w8(dyn_bc, info_type);
            avio_wb16(dyn_bc, rescale_rational(submix_layout->integrated_loudness, 1 << 8));
            avio_wb16(dyn_bc, rescale_rational(submix_layout->digital_peak, 1 << 8));
            if (info_type & 1)
                avio_wb16(dyn_bc, rescale_rational(submix_layout->true_peak, 1 << 8));
            if (info_type & 2) {
                avio_w8(dyn_bc, dialogue + album); // num_anchored_loudness
                if (dialogue) {
                    avio_w8(dyn_bc, IAMF_ANCHOR_ELEMENT_DIALOGUE);
                    avio_wb16(dyn_bc, rescale_rational(submix_layout->dialogue_anchored_loudness, 1 << 8));
                }
                if (album) {
                    avio_w8(dyn_bc, IAMF_ANCHOR_ELEMENT_ALBUM);
                    avio_wb16(dyn_bc, rescale_rational(submix_layout->album_anchored_loudness, 1 << 8));
                }
            }
        }
    }

    init_put_bits(&pbc, header, sizeof(header));
    put_bits(&pbc, 5, IAMF_OBU_IA_MIX_PRESENTATION);
    put_bits(&pbc, 3, 0);
    flush_put_bits(&pbc);

    dyn_size = avio_get_dyn_buf(dyn_bc, &dyn_buf);
    avio_write(pb, header, put_bytes_count(&pbc, 1));
    ffio_write_leb(pb, dyn_size);
    avio_write(pb, dyn_buf, dyn_size);
    ret = 0;
/* Every exit past avio_open_dyn_buf() comes through here, or the partially
 * serialized OBU leaks. */
fail:
    ffio_free_dyn_buf(&dyn_bc);

    return ret;
}

/**
 * Resolve the Audio Element a submix element references.
 *
 * @return the Audio Element carrying @p audio_element_id, or NULL if no Audio Element
 *         was added under that id.
 */
static const IAMFAudioElement *find_audio_element(const IAMFContext *iamf,
                                                  unsigned audio_element_id)
{
    for (int i = 0; i < iamf->nb_audio_elements; i++)
        if (iamf->audio_elements[i]->audio_element_id == audio_element_id)
            return iamf->audio_elements[i];

    return NULL;
}

/**
 * Check that an Audio Element still describes what was validated when it was added, and
 * that everything its descriptor reads through it resolves.
 *
 * ff_iamf_add_audio_element() validates the AVIAMFAudioElement a stream group carries and
 * records from it the layer count, the per layer substream counts and the recon gain flags
 * the descriptor declares. That structure belongs to the caller and stays writable: the
 * muxer may be initialized with avformat_init_output() and only written to later, the
 * descriptors are serialized a second time when writing the trailer, and libavutil/iamf.h
 * offers public functions that add layers to it. A layer added or replaced in between
 * would be serialized against the counts recorded for a different one, and a channel
 * layout no loudspeaker layout describes would reach the descriptor writers.
 *
 * So every invariant ff_iamf_add_audio_element() established is checked again here,
 * against what the caller has now.
 */
static int validate_audio_element(const IAMFContext *iamf,
                                  const IAMFAudioElement *audio_element,
                                  void *log_ctx)
{
    const AVIAMFAudioElement *element = audio_element->celement;
    int param_definition_types, ret;

    /* Both the num_layers field and the amount of layer records serialized come from the
     * count recorded when this element was added, as does the extent of everything indexed
     * per layer, so the layers present now have to be the ones that count was validated
     * for. */
    if (element->nb_layers != audio_element->nb_layers) {
        av_log(log_ctx, AV_LOG_ERROR, "Audio Element id %u carries %u layers, %u when it was added\n",
               audio_element->audio_element_id, element->nb_layers, audio_element->nb_layers);
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < audio_element->nb_layers; i++) {
        const AVIAMFLayer *layer = element->layers[i];
        const IAMFLayer *ilayer = &audio_element->layers[i];
        const int nb_substream_channels = ilayer->substream_count + ilayer->coupled_substream_count;
        int nb_channels = layer->ch_layout.nb_channels;

        /* The descriptor declares recon gain per layer from the flag recorded when this
         * element was added, and every parameter block written afterwards is read per layer
         * that declared it, so the flag the layer carries now has to agree. */
        if (!!(layer->flags & AV_IAMF_LAYER_FLAG_RECON_GAIN) != ilayer->recon_gain_present) {
            av_log(log_ctx, AV_LOG_ERROR, "Recon gain flag of layer %d in Audio Element id %u changed "
                   "since it was added\n", i, audio_element->audio_element_id);
            return AVERROR(EINVAL);
        }

        if (i)
            nb_channels -= element->layers[i-1]->ch_layout.nb_channels;
        /* The substreams accounted for the layer carry one channel each, two for a coupled
         * pair, and between them exactly the channels the layer adds to the one before it.
         * Those two counts are serialized for the layer, so a layer whose channels no
         * longer add up to them would be described by substreams it does not have. */
        if (nb_channels != nb_substream_channels) {
            av_log(log_ctx, AV_LOG_ERROR, "Layer %d in Audio Element id %u adds %d channels, %d "
                   "accounted for by the substreams it was given\n",
                   i, audio_element->audio_element_id, nb_channels, nb_substream_channels);
            return AVERROR(EINVAL);
        }
    }

    /* The type is serialized into a 3 bit field, and each of the two selects a different
     * serializer reading different members of a layer's channel layout, so a third value
     * has neither a field it fits in nor a description of the layers to write. */
    if (element->audio_element_type != AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL &&
        element->audio_element_type != AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE) {
        av_log(log_ctx, AV_LOG_ERROR, "Invalid audio element type %d in Audio Element id %u\n",
               element->audio_element_type, audio_element->audio_element_id);
        return AVERROR(EINVAL);
    }

    if (element->audio_element_type == AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE) {
        const AVIAMFLayer *layer;

        if (audio_element->nb_layers != 1) {
            av_log(log_ctx, AV_LOG_ERROR, "Invalid amount of layers %u for SCENE_BASED Audio Element "
                   "id %u. Must be 1\n", audio_element->nb_layers, audio_element->audio_element_id);
            return AVERROR(EINVAL);
        }

        layer = element->layers[0];
        /* ambisonics_config() writes the channel ids out of the custom order map, so a
         * layer that is no longer ambisonic has none to be serialized from. */
        if (layer->ch_layout.order != AV_CHANNEL_ORDER_CUSTOM &&
            layer->ch_layout.order != AV_CHANNEL_ORDER_AMBISONIC) {
            av_log(log_ctx, AV_LOG_ERROR, "Invalid channel layout for SCENE_BASED Audio Element id %u\n",
                   audio_element->audio_element_id);
            return AVERROR(EINVAL);
        }

        /* Everything else ambisonics_config() reads through the layer: the mode, the
         * channel and substream counts each field's width admits, a complete ambisonic
         * order, the ACN indices a custom order map is written as, and the demixing matrix
         * a projection mode indexes. Checked here so an Audio Element with no serialization
         * produces a diagnostic and no output at all. */
        ret = check_ambisonics_layer(audio_element, log_ctx);
        if (ret < 0)
            return ret;
    } else {
        for (int i = 0; i < audio_element->nb_layers; i++) {
            const AVIAMFLayer *layer = element->layers[i];
            int layout, expanded_layout;

            /* Refuses a layer neither loudspeaker layout table describes, and by requiring
             * the channel count to agree with the entry matched, one carrying more channels
             * than the layout serialized for it accounts for. */
            ret = get_loudspeaker_layout(layer, &layout, &expanded_layout, log_ctx);
            if (ret < 0)
                return ret;

            /* expanded_loudspeaker_layout is only present when num_layers is one. */
            if (expanded_layout >= 0 && audio_element->nb_layers != 1) {
                av_log(log_ctx, AV_LOG_ERROR, "Expanded channel layout in layer %d of Audio Element "
                       "id %u. Only an Audio Element with a single layer may use one\n",
                       i, audio_element->audio_element_id);
                return AVERROR(EINVAL);
            }

            /* Serialized into the layer's 6 bit output_gain_flags field, the width the
             * parser reads it back from. A wider value has no field to go in, and the bit
             * writer only objects to one under av_assert2, which is compiled out of a
             * default build and would leave the flags corrupting the bits around them. */
            if (layer->output_gain_flags > 0x3F) {
                av_log(log_ctx, AV_LOG_ERROR, "Invalid output gain flags 0x%X in layer %d of Audio "
                       "Element id %u. Must fit in 6 bits\n",
                       layer->output_gain_flags, i, audio_element->audio_element_id);
                return AVERROR(EINVAL);
            }

            /* Only serialized for a layer carrying output gain flags, which is the same
             * condition scalable_channel_layout_config() writes it under, so a layer that
             * never reaches the scaling is not constrained by it. */
            if (layer->output_gain_flags) {
                ret = check_rational(layer->output_gain, "output_gain", "Layer", i, log_ctx);
                if (ret < 0)
                    return ret;
            }

            if (!i)
                continue;

            const AVIAMFLayer *prev_layer = element->layers[i-1];
            uint64_t prev_mask = av_channel_layout_subset(&prev_layer->ch_layout, UINT64_MAX);
            /* A layer carries the channels of the one before it plus at least one more,
             * except after a Mono layer, which the layer following it may drop the center
             * channel of. Same chain required when this element was added. */
            if ((prev_layer->ch_layout.nb_channels > 1 &&
                 av_channel_layout_subset(&layer->ch_layout, prev_mask) != prev_mask) ||
                layer->ch_layout.nb_channels <= prev_layer->ch_layout.nb_channels) {
                av_log(log_ctx, AV_LOG_ERROR, "Channel layout of layer %d in Audio Element id %u can't "
                       "follow the one of layer %d\n",
                       i, audio_element->audio_element_id, i - 1);
                return AVERROR(EINVAL);
            }
        }
    }

    /* Which parameter definition types the descriptor declares, so that an Audio Element
     * needing demixing or recon gain information it does not carry is refused here, before
     * ff_iamf_write_descriptors() has written anything, and not part way through its own
     * descriptor. Also tells which of the roles below is serialized out of the definition
     * registered under the parameter id it names. */
    ret = get_param_definition_types(iamf, audio_element, &param_definition_types, log_ctx);
    if (ret < 0)
        return ret;

    /* Each is serialized through the definition registered under the parameter id it names,
     * out of the single subblock it carries. */
    if (element->demixing_info) {
        const AVIAMFParamDefinition *param = element->demixing_info;
        const IAMFParamDefinition *param_definition;

        if (param->nb_subblocks != 1) {
            av_log(log_ctx, AV_LOG_ERROR, "nb_subblocks in demixing_info of Audio Element id %u is not 1\n",
                   audio_element->audio_element_id);
            return AVERROR(EINVAL);
        }
        param_definition = ff_iamf_get_param_definition(iamf, param->parameter_id);
        if (!param_definition) {
            av_log(log_ctx, AV_LOG_ERROR, "Invalid Parameter Definition with ID %u in demixing_info of "
                   "Audio Element id %u\n", param->parameter_id, audio_element->audio_element_id);
            return AVERROR(EINVAL);
        }
        /* The registered definition is only what this role is serialized out of when the
         * descriptor declares the type; an id shared with a role it does not declare is not
         * a conflict, and the CLI allocates both roles for every audio element it is given. */
        ret = check_param_definition_type(param,
                                          param_definition_types & AV_IAMF_PARAMETER_DEFINITION_DEMIXING
                                          ? param_definition : NULL,
                                          AV_IAMF_PARAMETER_DEFINITION_DEMIXING,
                                          "demixing_info", log_ctx);
        if (ret < 0)
            return ret;
    }
    if (element->recon_gain_info) {
        const AVIAMFParamDefinition *param = element->recon_gain_info;
        const IAMFParamDefinition *param_definition;

        if (param->nb_subblocks != 1) {
            av_log(log_ctx, AV_LOG_ERROR, "nb_subblocks in recon_gain_info of Audio Element id %u is not 1\n",
                   audio_element->audio_element_id);
            return AVERROR(EINVAL);
        }
        param_definition = ff_iamf_get_param_definition(iamf, param->parameter_id);
        if (!param_definition) {
            av_log(log_ctx, AV_LOG_ERROR, "Invalid Parameter Definition with ID %u in recon_gain_info of "
                   "Audio Element id %u\n", param->parameter_id, audio_element->audio_element_id);
            return AVERROR(EINVAL);
        }
        ret = check_param_definition_type(param,
                                          param_definition_types & AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN
                                          ? param_definition : NULL,
                                          AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN,
                                          "recon_gain_info", log_ctx);
        if (ret < 0)
            return ret;
    }

    return 0;
}

/**
 * Check every Audio Element added, before anything is serialized from one.
 *
 * A descriptor at odds with itself is otherwise only detectable while serializing it, by
 * which point the Sequence Header and everything before it has been written. Running this
 * first means an Audio Element that no longer describes what was validated produces a
 * diagnostic and no output at all.
 */
static int validate_audio_elements(const IAMFContext *iamf, void *log_ctx)
{
    for (int i = 0; i < iamf->nb_audio_elements; i++) {
        int ret = validate_audio_element(iamf, iamf->audio_elements[i], log_ctx);
        if (ret < 0)
            return ret;
    }

    return 0;
}

/**
 * Check that everything a Mix Presentation's descriptor is serialized out of is present
 * and resolves.
 *
 * ff_iamf_add_mix_presentation() requires a mix configuration on the submixes and elements
 * the Mix Presentation has when it is added, and registers a parameter definition for each.
 * The AVIAMFMixPresentation belongs to the caller and stays writable afterwards: the muxer
 * may be initialized with avformat_init_output() and only written to later, the descriptors
 * are serialized a second time when writing the trailer, and libavutil/iamf.h offers public
 * functions that add submixes, elements and layouts to it. A submix or element appended in
 * between carries no mix configuration at all, and one whose configuration was replaced
 * names a parameter id nothing is registered under.
 *
 * A Mix Presentation is serialized after the Sequence Header, the Codec Configs and the
 * Audio Elements, so objecting to any of this while serializing it would only reject the
 * configuration once those OBUs had already been emitted. Everything is therefore checked
 * here, before any of them is written, and nothing at all is emitted for a Mix Presentation
 * that cannot be serialized in full.
 *
 * The Audio Element references are resolved here rather than in
 * ff_iamf_add_mix_presentation() because a Mix Presentation may be added before the Audio
 * Element it references: both muxers add the stream groups they are given, and
 * libavformat/movenc.c adds them in the order they appear in the AVFormatContext. Refusing
 * a reference that is merely not resolvable yet would reject a configuration that becomes
 * valid once the remaining stream groups have been added. Every Audio Element is known by
 * the time the descriptors are written, whatever order the stream groups were in.
 */
static int validate_mix_presentation(const IAMFContext *iamf,
                                     const IAMFMixPresentation *mix_presentation,
                                     void *log_ctx)
{
    const AVIAMFMixPresentation *mix = mix_presentation->cmix;
    const int nb_labels = av_dict_count(mix->annotations);
    const IAMFParamDefinition *param_definition;
    int ret;

    for (int i = 0; i < mix->nb_submixes; i++) {
        const AVIAMFSubmix *submix = mix->submixes[i];

        /* Scaled into the submix's signed fixed point default_mix_gain field. */
        ret = check_rational(submix->default_mix_gain, "default_mix_gain", "Submix", i, log_ctx);
        if (ret < 0)
            return ret;

        /* The submix is serialized out of its output mix configuration, through the
         * definition registered under the parameter id it names. Same presence
         * ff_iamf_add_mix_presentation() required of every submix it was given. */
        if (!submix->output_mix_config) {
            av_log(log_ctx, AV_LOG_ERROR, "output_mix_config is not present in submix %d from "
                   "Mix Presentation id #%u\n", i, mix_presentation->mix_presentation_id);
            return AVERROR(EINVAL);
        }
        param_definition = ff_iamf_get_param_definition(iamf, submix->output_mix_config->parameter_id);
        if (!param_definition) {
            av_log(log_ctx, AV_LOG_ERROR, "Invalid Parameter Definition with ID %u in output_mix_config "
                   "of submix %d from Mix Presentation id #%u\n",
                   submix->output_mix_config->parameter_id, i,
                   mix_presentation->mix_presentation_id);
            return AVERROR(EINVAL);
        }
        /* The role mandates the type, of the definition it carries and of the one actually
         * registered under that id, which need not be the same object. */
        ret = check_param_definition_type(submix->output_mix_config, param_definition,
                                          AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN,
                                          "output_mix_config", log_ctx);
        if (ret < 0)
            return ret;

        for (int j = 0; j < submix->nb_elements; j++) {
            const AVIAMFSubmixElement *element = submix->elements[j];
            const AVDictionaryEntry *tag = NULL;

            /* Scaled into the element's signed fixed point default_mix_gain field. */
            ret = check_rational(element->default_mix_gain, "default_mix_gain",
                                 "Submix element", j, log_ctx);
            if (ret < 0)
                return ret;

            if (!find_audio_element(iamf, element->audio_element_id)) {
                av_log(log_ctx, AV_LOG_ERROR, "Invalid Audio Element id %u referenced by element %d "
                                              "in submix %d from Mix Presentation id #%u\n",
                       element->audio_element_id, j, i, mix_presentation->mix_presentation_id);
                return AVERROR(EINVAL);
            }

            if (!element->element_mix_config) {
                av_log(log_ctx, AV_LOG_ERROR, "element_mix_config is not present for element %d in "
                       "submix %d from Mix Presentation id #%u\n",
                       j, i, mix_presentation->mix_presentation_id);
                return AVERROR(EINVAL);
            }
            param_definition = ff_iamf_get_param_definition(iamf, element->element_mix_config->parameter_id);
            if (!param_definition) {
                av_log(log_ctx, AV_LOG_ERROR, "Invalid Parameter Definition with ID %u in "
                       "element_mix_config of element %d in submix %d from Mix Presentation id #%u\n",
                       element->element_mix_config->parameter_id, j, i,
                       mix_presentation->mix_presentation_id);
                return AVERROR(EINVAL);
            }
            ret = check_param_definition_type(element->element_mix_config, param_definition,
                                              AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN,
                                              "element_mix_config", log_ctx);
            if (ret < 0)
                return ret;

            /* The element's labels are serialized one per label the Mix Presentation
             * declares, in the order the Mix Presentation declares them, so it needs one
             * under each of those names and no others. */
            if (av_dict_count(element->annotations) != nb_labels) {
                av_log(log_ctx, AV_LOG_ERROR, "Inconsistent amount of labels for element %d in submix "
                       "%d from Mix Presentation id #%u\n",
                       j, i, mix_presentation->mix_presentation_id);
                return AVERROR(EINVAL);
            }
            while ((tag = av_dict_iterate(mix->annotations, tag))) {
                if (!av_dict_get(element->annotations, tag->key, NULL, 0)) {
                    av_log(log_ctx, AV_LOG_ERROR, "Label \"%s\" is missing for element %d in submix %d "
                           "from Mix Presentation id #%u\n",
                           tag->key, j, i, mix_presentation->mix_presentation_id);
                    return AVERROR(EINVAL);
                }
            }

            /* Serialized into a 2 bit field. A value that is neither mode has no field to
             * go in, and the bit writer only objects to one under av_assert2, which is
             * compiled out of a default build and would leave it corrupting the bits
             * written around it. */
            if (element->headphones_rendering_mode != AV_IAMF_HEADPHONES_MODE_STEREO &&
                element->headphones_rendering_mode != AV_IAMF_HEADPHONES_MODE_BINAURAL) {
                av_log(log_ctx, AV_LOG_ERROR, "Invalid headphones rendering mode %d for element %d in "
                       "submix %d from Mix Presentation id #%u\n",
                       element->headphones_rendering_mode, j, i,
                       mix_presentation->mix_presentation_id);
                return AVERROR(EINVAL);
            }
        }

        for (int j = 0; j < submix->nb_layouts; j++) {
            const AVIAMFSubmixLayout *submix_layout = submix->layouts[j];

            /* integrated_loudness and digital_peak are always scaled into the layout's
             * loudness information; the three below are only serialized when both their
             * numerator and denominator are non-zero, a condition a negative denominator
             * satisfies, so each is checked under that same gate. */
            ret = check_rational(submix_layout->integrated_loudness, "integrated_loudness",
                                 "Submix layout", j, log_ctx);
            if (ret < 0)
                return ret;
            ret = check_rational(submix_layout->digital_peak, "digital_peak",
                                 "Submix layout", j, log_ctx);
            if (ret < 0)
                return ret;
            if (submix_layout->true_peak.num && submix_layout->true_peak.den) {
                ret = check_rational(submix_layout->true_peak, "true_peak",
                                     "Submix layout", j, log_ctx);
                if (ret < 0)
                    return ret;
            }
            if (submix_layout->dialogue_anchored_loudness.num &&
                submix_layout->dialogue_anchored_loudness.den) {
                ret = check_rational(submix_layout->dialogue_anchored_loudness,
                                     "dialogue_anchored_loudness", "Submix layout", j, log_ctx);
                if (ret < 0)
                    return ret;
            }
            if (submix_layout->album_anchored_loudness.num &&
                submix_layout->album_anchored_loudness.den) {
                ret = check_rational(submix_layout->album_anchored_loudness,
                                     "album_anchored_loudness", "Submix layout", j, log_ctx);
                if (ret < 0)
                    return ret;
            }

            /* A loudspeakers layout is serialized as the sound_system describing its
             * channel layout, and no other layout type has a serialization at all. */
            if (submix_layout->layout_type == AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS) {
                if (get_sound_system(&submix_layout->sound_system) < 0) {
                    av_log(log_ctx, AV_LOG_ERROR, "Invalid Sound System value in layout %d of submix %d "
                           "from Mix Presentation id #%u\n",
                           j, i, mix_presentation->mix_presentation_id);
                    return AVERROR(EINVAL);
                }
            } else if (submix_layout->layout_type != AV_IAMF_SUBMIX_LAYOUT_TYPE_BINAURAL) {
                av_log(log_ctx, AV_LOG_ERROR, "Unsupported Layout Type value %d in layout %d of submix "
                       "%d from Mix Presentation id #%u\n",
                       submix_layout->layout_type, j, i, mix_presentation->mix_presentation_id);
                return AVERROR(EINVAL);
            }
        }
    }

    return 0;
}

/**
 * Check every Mix Presentation added, before anything is serialized from one.
 */
static int validate_mix_presentations(const IAMFContext *iamf, void *log_ctx)
{
    for (int i = 0; i < iamf->nb_mix_presentations; i++) {
        int ret = validate_mix_presentation(iamf, iamf->mix_presentations[i], log_ctx);
        if (ret < 0)
            return ret;
    }

    return 0;
}

int ff_iamf_write_descriptors(const IAMFContext *iamf, AVIOContext *pb, void *log_ctx)
{
    int ret;

    /* Everything serialized below is described by structures the caller owns and may have
     * changed since they were validated, so all of them are checked here, before the first
     * byte of the sequence goes out. */
    ret = validate_audio_elements(iamf, log_ctx);
    if (ret < 0)
        return ret;

    ret = validate_mix_presentations(iamf, log_ctx);
    if (ret < 0)
        return ret;

    // Sequence Header
    avio_w8(pb, IAMF_OBU_IA_SEQUENCE_HEADER << 3);

    ffio_write_leb(pb, 6);
    avio_wb32(pb, MKBETAG('i','a','m','f'));
    avio_w8(pb, iamf->nb_audio_elements > 1); // primary_profile
    avio_w8(pb, iamf->nb_audio_elements > 1); // additional_profile

    for (int i = 0; i < iamf->nb_codec_configs; i++) {
        ret = iamf_write_codec_config(iamf, iamf->codec_configs[i], pb);
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < iamf->nb_audio_elements; i++) {
        ret = iamf_write_audio_element(iamf, iamf->audio_elements[i], pb, log_ctx);
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < iamf->nb_mix_presentations; i++) {
        ret = iamf_write_mixing_presentation(iamf, iamf->mix_presentations[i], pb, log_ctx);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int write_parameter_block(const IAMFContext *iamf, AVIOContext *pb,
                                 const AVIAMFParamDefinition *param, void *log_ctx)
{
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    const IAMFParamDefinition *param_definition = ff_iamf_get_param_definition(iamf, param->parameter_id);
    PutBitContext pbc;
    AVIOContext *dyn_bc;
    uint8_t *dyn_buf = NULL;
    int dyn_size, ret;

    /* This structure is the packet's side data read in place, so the type field holds
     * whatever bytes the caller put there and not only the values the enumeration names.
     * Enumerated rather than bounded from above, because the type an enumeration is
     * represented as is implementation defined: unsigned here, where every value an upper
     * bound admits is one of the three below, but a signed choice is just as conforming and
     * there a value under the first enumerator would pass that bound and then match no case
     * of the switch serializing the subblocks, reaching its av_unreachable. Listing the
     * types that switch handles keeps its default unreachable however the enumeration is
     * represented, rather than by a property of this compiler. */
    switch (param->type) {
    case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN:
    case AV_IAMF_PARAMETER_DEFINITION_DEMIXING:
    case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN:
        break;
    default:
        av_log(log_ctx, AV_LOG_DEBUG, "Ignoring side data with unknown type %d\n",
               param->type);
        return 0;
    }

    if (!param_definition) {
        av_log(log_ctx, AV_LOG_ERROR, "Non-existent Parameter Definition with ID %u referenced by a packet\n",
               param->parameter_id);
        return AVERROR(EINVAL);
    }

    if (param->type != param_definition->param->type) {
        av_log(log_ctx, AV_LOG_ERROR, "Inconsistent values for Parameter Definition "
                                "with ID %u in a packet\n",
               param->parameter_id);
        return AVERROR(EINVAL);
    }

    /* A recon gain block is serialized per layer of the Audio Element its definition
     * belongs to, and a definition registered from a Mix Presentation belongs to none.
     * Refused here rather than where it is read, so a packet that cannot be serialized
     * contributes nothing at all instead of an OBU header with no body behind it. */
    if (param->type == AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN &&
        !param_definition->audio_element) {
        av_log(log_ctx, AV_LOG_ERROR, "Invalid Parameter Definition with ID %u referenced by a packet\n",
               param->parameter_id);
        return AVERROR(EINVAL);
    }

    ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    ffio_write_leb(dyn_bc, param->parameter_id);
    if (!param_definition->mode) {
        ffio_write_leb(dyn_bc, param->duration);
        ffio_write_leb(dyn_bc, param->constant_subblock_duration);
        if (param->constant_subblock_duration == 0)
            ffio_write_leb(dyn_bc, param->nb_subblocks);
    }

    for (int i = 0; i < param->nb_subblocks; i++) {
        const void *subblock = av_iamf_param_definition_get_subblock(param, i);

        switch (param->type) {
        case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN: {
            const AVIAMFMixGain *mix = subblock;

            /* Every rational this subblock serializes is checked before the first one is
             * written, so a subblock carrying one that cannot be scaled leaves no partial
             * animation behind. The ones after the first are only written for the
             * animation types that carry them, and only those are constrained. */
            ret = check_rational(mix->start_point_value, "start_point_value",
                                 "Mix gain subblock", i, log_ctx);
            if (ret < 0)
                goto fail;
            if (mix->animation_type >= AV_IAMF_ANIMATION_TYPE_LINEAR) {
                ret = check_rational(mix->end_point_value, "end_point_value",
                                     "Mix gain subblock", i, log_ctx);
                if (ret < 0)
                    goto fail;
            }
            if (mix->animation_type == AV_IAMF_ANIMATION_TYPE_BEZIER) {
                ret = check_rational(mix->control_point_value, "control_point_value",
                                     "Mix gain subblock", i, log_ctx);
                if (ret < 0)
                    goto fail;
                ret = check_rational(mix->control_point_relative_time,
                                     "control_point_relative_time",
                                     "Mix gain subblock", i, log_ctx);
                if (ret < 0)
                    goto fail;
            }

            if (!param_definition->mode && param->constant_subblock_duration == 0)
                ffio_write_leb(dyn_bc, mix->subblock_duration);

            ffio_write_leb(dyn_bc, mix->animation_type);

            avio_wb16(dyn_bc, rescale_rational(mix->start_point_value, 1 << 8));
            if (mix->animation_type >= AV_IAMF_ANIMATION_TYPE_LINEAR)
                avio_wb16(dyn_bc, rescale_rational(mix->end_point_value, 1 << 8));
            if (mix->animation_type == AV_IAMF_ANIMATION_TYPE_BEZIER) {
                avio_wb16(dyn_bc, rescale_rational(mix->control_point_value, 1 << 8));
                avio_w8(dyn_bc, av_clip_uint8(av_rescale(mix->control_point_relative_time.num, 1 << 8,
                                                         mix->control_point_relative_time.den)));
            }
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_DEMIXING: {
            const AVIAMFDemixingInfo *demix = subblock;
            if (!param_definition->mode && param->constant_subblock_duration == 0)
                ffio_write_leb(dyn_bc, demix->subblock_duration);

            avio_w8(dyn_bc, demix->dmixp_mode << 5);
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN: {
            const AVIAMFReconGain *recon = subblock;
            const IAMFAudioElement *audio_element = param_definition->audio_element;

            if (!param_definition->mode && param->constant_subblock_duration == 0)
                ffio_write_leb(dyn_bc, recon->subblock_duration);

            /* Refused before this OBU was opened, so this cannot be reached; kept because
             * the Audio Element is dereferenced below. */
            if (!audio_element) {
                av_log(log_ctx, AV_LOG_ERROR, "Invalid Parameter Definition with ID %u referenced by a packet\n", param->parameter_id);
                ret = AVERROR(EINVAL);
                goto fail;
            }

            /* One recon gain record per layer the descriptor declared, so this reads the
             * same validated layer count scalable_channel_layout_config() serialized and
             * not the one in the AVIAMFAudioElement, which the caller may have grown
             * since. Clamped to the recon_gain matrix on top of that, which bounds the
             * rows read below even for a parameter definition that never went through
             * ff_iamf_add_audio_element(). */
            const int nb_layers = FFMIN(audio_element->nb_layers,
                                        FF_ARRAY_ELEMS(recon->recon_gain));

            for (int j = 0; j < nb_layers; j++) {
                /* Read per layer that declared recon gain in the descriptor, which is
                 * the flag recorded then and not the one the layer carries now. */
                if (audio_element->layers[j].recon_gain_present) {
                    unsigned int recon_gain_flags = 0;
                    int k = 0;

                    for (; k < 7; k++)
                        recon_gain_flags |= (1 << k) * !!recon->recon_gain[j][k];
                    for (; k < 12; k++)
                        recon_gain_flags |= (2 << k) * !!recon->recon_gain[j][k];
                    if (recon_gain_flags >> 8)
                        recon_gain_flags |= (1 << k);

                    ffio_write_leb(dyn_bc, recon_gain_flags);
                    for (k = 0; k < 12; k++) {
                        if (recon->recon_gain[j][k])
                            avio_w8(dyn_bc, recon->recon_gain[j][k]);
                    }
                }
            }
            break;
        }
        default:
            av_unreachable("param_definition_type should have been checked above");
        }
    }

    /* Written only now that the whole body has been serialized. Emitting it up front would
     * leave an OBU header on the output with nothing behind it for any subblock refused
     * above, and a header announcing a parameter block that is not there is not something a
     * reader can skip. The bytes reach pb in the same order either way. */
    init_put_bits(&pbc, header, sizeof(header));
    put_bits(&pbc, 5, IAMF_OBU_IA_PARAMETER_BLOCK);
    put_bits(&pbc, 3, 0);
    flush_put_bits(&pbc);

    dyn_size = avio_get_dyn_buf(dyn_bc, &dyn_buf);
    avio_write(pb, header, put_bytes_count(&pbc, 1));
    ffio_write_leb(pb, dyn_size);
    avio_write(pb, dyn_buf, dyn_size);
    ret = 0;
/* Every exit past avio_open_dyn_buf() comes through here, or the partially
 * serialized OBU leaks. */
fail:
    ffio_free_dyn_buf(&dyn_bc);

    return ret;
}

int ff_iamf_write_parameter_blocks(const IAMFContext *iamf, AVIOContext *pb,
                                   const AVPacket *pkt, void *log_ctx)
{
    AVIAMFParamDefinition *mix =
        (AVIAMFParamDefinition *)av_packet_get_side_data(pkt,
                                                         AV_PKT_DATA_IAMF_MIX_GAIN_PARAM,
                                                         NULL);
    AVIAMFParamDefinition *demix =
        (AVIAMFParamDefinition *)av_packet_get_side_data(pkt,
                                                         AV_PKT_DATA_IAMF_DEMIXING_INFO_PARAM,
                                                         NULL);
    AVIAMFParamDefinition *recon =
        (AVIAMFParamDefinition *)av_packet_get_side_data(pkt,
                                                         AV_PKT_DATA_IAMF_RECON_GAIN_INFO_PARAM,
                                                         NULL);

    if (mix) {
        int ret = write_parameter_block(iamf, pb, mix, log_ctx);
        if (ret < 0)
           return ret;
    }
    if (demix) {
        int ret = write_parameter_block(iamf, pb, demix, log_ctx);
        if (ret < 0)
            return ret;
    }
    if (recon) {
        int ret = write_parameter_block(iamf, pb, recon, log_ctx);
        if (ret < 0)
           return ret;
    }

    return 0;
}

static IAMFAudioElement *get_audio_element(const IAMFContext *c,
                                           unsigned int audio_substream_id)
{
    for (int i = 0; i < c->nb_audio_elements; i++) {
        IAMFAudioElement *audio_element = c->audio_elements[i];
        for (int j = 0; j < audio_element->nb_substreams; j++) {
            IAMFSubStream *substream = &audio_element->substreams[j];
            if (substream->audio_substream_id == audio_substream_id)
                return audio_element;
        }
    }

    return NULL;
}

int ff_iamf_write_audio_frame(const IAMFContext *iamf, AVIOContext *pb,
                              unsigned audio_substream_id, const AVPacket *pkt)
{
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    PutBitContext pbc;
    const IAMFAudioElement *audio_element;
    IAMFCodecConfig *codec_config;
    AVIOContext *dyn_bc;
    const uint8_t *side_data;
    uint8_t *dyn_buf = NULL;
    unsigned int skip_samples = 0, discard_padding = 0;
    size_t side_data_size;
    int dyn_size, type = audio_substream_id <= 17 ?
                         audio_substream_id + IAMF_OBU_IA_AUDIO_FRAME_ID0 : IAMF_OBU_IA_AUDIO_FRAME;
    int ret;

    audio_element = get_audio_element(iamf, audio_substream_id);
    if (!audio_element)
        return AVERROR(EINVAL);
    codec_config = ff_iamf_get_codec_config(iamf, audio_element->codec_config_id);
    if (!codec_config)
        return AVERROR(EINVAL);

    if (!pkt->size) {
        size_t new_extradata_size;
        const uint8_t *new_extradata = av_packet_get_side_data(pkt,
                                                               AV_PKT_DATA_NEW_EXTRADATA,
                                                               &new_extradata_size);

        if (!new_extradata || new_extradata_size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
            return AVERROR_INVALIDDATA;

        av_free(codec_config->extradata);
        codec_config->extradata = av_malloc(new_extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!codec_config->extradata) {
            codec_config->extradata_size = 0;
            return AVERROR(ENOMEM);
        }
        memcpy(codec_config->extradata, new_extradata, new_extradata_size);
        memset(codec_config->extradata + new_extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        codec_config->extradata_size = new_extradata_size;

        return update_extradata(codec_config);
    }

    side_data = av_packet_get_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES,
                                        &side_data_size);

    if (side_data && side_data_size >= 10) {
        skip_samples = AV_RL32(side_data);
        discard_padding = AV_RL32(side_data + 4);
    }

    if (codec_config->codec_id == AV_CODEC_ID_OPUS) {
        // IAMF's num_samples_to_trim_at_start is the same as Opus's pre-skip.
        skip_samples = pkt->dts < 0
            ? av_rescale(-pkt->dts, 48000, pkt->time_base.den)
            : 0;
        discard_padding = av_rescale(discard_padding, 48000, pkt->time_base.den);
    }

    ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    init_put_bits(&pbc, header, sizeof(header));
    put_bits(&pbc, 5, type);
    put_bits(&pbc, 1, 0); // obu_redundant_copy
    put_bits(&pbc, 1, skip_samples || discard_padding);
    put_bits(&pbc, 1, 0); // obu_extension_flag
    flush_put_bits(&pbc);
    avio_write(pb, header, put_bytes_count(&pbc, 1));

    if (skip_samples || discard_padding) {
        ffio_write_leb(dyn_bc, discard_padding);
        ffio_write_leb(dyn_bc, skip_samples);
    }

    if (audio_substream_id > 17)
        ffio_write_leb(dyn_bc, audio_substream_id);

    dyn_size = avio_get_dyn_buf(dyn_bc, &dyn_buf);
    ffio_write_leb(pb, dyn_size + pkt->size);
    avio_write(pb, dyn_buf, dyn_size);
    ffio_free_dyn_buf(&dyn_bc);
    avio_write(pb, pkt->data, pkt->size);

    return 0;
}
