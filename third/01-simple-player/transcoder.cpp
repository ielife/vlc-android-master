#include <transcoder.h>

int  Transcoder::OpenInputFile(const char *filename)
{
    int ret;
    unsigned int i;

    if ((ret = avformat_open_input(&mIfmtCtx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }
    if ((ret = avformat_find_stream_info(mIfmtCtx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }
    for (i = 0; i < mIfmtCtx->nb_streams; i++) {
        AVStream *stream;
        AVCodecContext *codec_ctx;
        stream = mIfmtCtx->streams[i];
        codec_ctx = stream->codec;
        /* Reencode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* Open decoder */
            ret = avcodec_open2(codec_ctx,
                    avcodec_find_decoder(codec_ctx->codec_id), NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
                return ret;
            }
        }
    }
    av_dump_format(mIfmtCtx, 0, filename, 0);
    mInputFile = filename;
    return 0;
}
int  Transcoder::OpenOutputFile(const char *filename)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    AVCodec *encoder;
    int ret;
    unsigned int i;

    avformat_alloc_output_context2(&mOfmtCtx, NULL, NULL, filename);
    if (!mOfmtCtx) {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }
    for (i = 0; i < mIfmtCtx->nb_streams; i++) {
        out_stream = avformat_new_stream(mOfmtCtx, NULL);
        if (!out_stream) {
            av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
        }
        in_stream = mIfmtCtx->streams[i];
        dec_ctx = in_stream->codec;
        enc_ctx = out_stream->codec;
        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* in this example, we choose transcoding to same codec */
            encoder = avcodec_find_encoder(dec_ctx->codec_id);
            /* In this example, we transcode to same properties (picture size,
             * sample rate etc.). These properties can be changed for output
             * streams easily using filters */
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                enc_ctx->height = dec_ctx->height;
                enc_ctx->width = dec_ctx->width;
                enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                /* take first format from list of supported formats */
                enc_ctx->pix_fmt = encoder->pix_fmts[0];
                /* video time_base can be set to whatever is handy and supported by encoder */
                enc_ctx->time_base = dec_ctx->time_base;
            } else {
                enc_ctx->sample_rate = dec_ctx->sample_rate;
                enc_ctx->channel_layout = dec_ctx->channel_layout;
                enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
                /* take first format from list of supported formats */
                enc_ctx->sample_fmt = encoder->sample_fmts[0];
                AVRational time_base={1, enc_ctx->sample_rate};
                enc_ctx->time_base = time_base;
            }
            /* Third parameter can be used to pass settings to encoder */
            ret = avcodec_open2(enc_ctx, encoder, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", i);
                return ret;
            }
        } else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
            av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
            return AVERROR_INVALIDDATA;
        } else {
            /* if this stream must be remuxed */
            ret = avcodec_copy_context(mOfmtCtx->streams[i]->codec,
                    mIfmtCtx->streams[i]->codec);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Copying stream context failed\n");
                return ret;
            }
        }
        if (mOfmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
            enc_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }
    av_dump_format(mOfmtCtx, 0, filename, 1);
    if (!(mOfmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&mOfmtCtx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
            return ret;
        }
    }
    /* init muxer, write output file header */
    ret = avformat_write_header(mOfmtCtx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        return ret;
    }
    mOutputFile = filename;
    return 0;
}

int  Transcoder::InitFilter(FilteringContext* fctx, AVCodecContext *dec_ctx,
                AVCodecContext *enc_ctx, const char *filter_spec)
{
    char args[512];
    int ret = 0;
    AVFilter *buffersrc = NULL;
    AVFilter *buffersink = NULL;
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        buffersrc = avfilter_get_by_name("buffer");
        buffersink = avfilter_get_by_name("buffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        _snprintf(args, sizeof(args),
                "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                dec_ctx->time_base.num, dec_ctx->time_base.den,
                dec_ctx->sample_aspect_ratio.num,
                dec_ctx->sample_aspect_ratio.den);
        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
            goto end;
        }
        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
            goto end;
        }
        ret = av_opt_set_bin(buffersink_ctx, "pix_fmts",
                (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
            goto end;
        }
    } else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        buffersrc = avfilter_get_by_name("abuffer");
        buffersink = avfilter_get_by_name("abuffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        if (!dec_ctx->channel_layout)
            dec_ctx->channel_layout =
                av_get_default_channel_layout(dec_ctx->channels);
        _snprintf(args, sizeof(args),
                "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%I64x",
                dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
                av_get_sample_fmt_name(dec_ctx->sample_fmt),
                dec_ctx->channel_layout);
        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
            goto end;
        }
        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
            goto end;
        }
        ret = av_opt_set_bin(buffersink_ctx, "sample_fmts",
                (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
            goto end;
        }
        ret = av_opt_set_bin(buffersink_ctx, "channel_layouts",
                (uint8_t*)&enc_ctx->channel_layout,
                sizeof(enc_ctx->channel_layout), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
            goto end;
        }
        ret = av_opt_set_bin(buffersink_ctx, "sample_rates",
                (uint8_t*)&enc_ctx->sample_rate, sizeof(enc_ctx->sample_rate),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
            goto end;
        }
    } else {
        ret = AVERROR_UNKNOWN;
        goto end;
    }
    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;
    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
                    &inputs, &outputs, NULL)) < 0)
        goto end;
    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;
    /* Fill FilteringContext */
    fctx->buffersrc_ctx = buffersrc_ctx;
    fctx->buffersink_ctx = buffersink_ctx;
    fctx->filter_graph = filter_graph;
end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}
int  Transcoder::InitFilters()
{
    const char *filter_spec;
    unsigned int i;
    int ret;
    mFilterCtx = (FilteringContext *)av_malloc_array(mIfmtCtx->nb_streams, sizeof(FilteringContext));
    if (!mFilterCtx)
        return AVERROR(ENOMEM);
    for (i = 0; i < mIfmtCtx->nb_streams; i++) {
        mFilterCtx[i].buffersrc_ctx  = NULL;
        mFilterCtx[i].buffersink_ctx = NULL;
        mFilterCtx[i].filter_graph   = NULL;
        if (!(mIfmtCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO
                || mIfmtCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO))
            continue;
        if (mIfmtCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
            filter_spec = "null"; /* passthrough (dummy) filter for video */
        else
            filter_spec = "anull"; /* passthrough (dummy) filter for audio */
        ret = InitFilter(&mFilterCtx[i], mIfmtCtx->streams[i]->codec,
                mOfmtCtx->streams[i]->codec, filter_spec);
        if (ret)
            return ret;
    }
    return 0;
}

int  Transcoder::EncodeWriteFrame(AVFrame *filt_frame, unsigned int stream_index, int *got_frame)
{
    int ret;
    int got_frame_local;
    AVPacket enc_pkt;
    int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *) =
        (mIfmtCtx->streams[stream_index]->codec->codec_type ==
         AVMEDIA_TYPE_VIDEO) ? avcodec_encode_video2 : avcodec_encode_audio2;
    if (!got_frame)
        got_frame = &got_frame_local;
    av_log(NULL, AV_LOG_INFO, "Encoding frame\n");
    /* encode filtered frame */
    enc_pkt.data = NULL;
    enc_pkt.size = 0;
    av_init_packet(&enc_pkt);
    ret = enc_func(mOfmtCtx->streams[stream_index]->codec, &enc_pkt,
            filt_frame, got_frame);
    av_frame_free(&filt_frame);
    if (ret < 0)
        return ret;
    if (!(*got_frame))
        return 0;
    /* prepare packet for muxing */
    enc_pkt.stream_index = stream_index;
    enc_pkt.dts = av_rescale_q_rnd(enc_pkt.dts,
            mOfmtCtx->streams[stream_index]->codec->time_base,
            mOfmtCtx->streams[stream_index]->time_base,
            (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    enc_pkt.pts = av_rescale_q_rnd(enc_pkt.pts,
            mOfmtCtx->streams[stream_index]->codec->time_base,
            mOfmtCtx->streams[stream_index]->time_base,
            (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    enc_pkt.duration = av_rescale_q(enc_pkt.duration,
            mOfmtCtx->streams[stream_index]->codec->time_base,
            mOfmtCtx->streams[stream_index]->time_base);
    av_log(NULL, AV_LOG_DEBUG, "Muxing frame\n");
    /* mux encoded frame */
    ret = av_interleaved_write_frame(mOfmtCtx, &enc_pkt);
    return ret;
}

int  Transcoder::FilterEncodeWriteFrame(AVFrame *frame, unsigned int stream_index)
{
    int ret;
    AVFrame *filt_frame;
    av_log(NULL, AV_LOG_INFO, "Pushing decoded frame to filters\n");
    /* push the decoded frame into the filtergraph */
    ret = av_buffersrc_add_frame_flags(mFilterCtx[stream_index].buffersrc_ctx,
            frame, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
        return ret;
    }
    /* pull filtered frames from the filtergraph */
    while (1) {
        filt_frame = av_frame_alloc();
        if (!filt_frame) {
            ret = AVERROR(ENOMEM);
            break;
        }
        av_log(NULL, AV_LOG_INFO, "Pulling filtered frame from filters\n");
        ret = av_buffersink_get_frame(mFilterCtx[stream_index].buffersink_ctx,
                filt_frame);
        if (ret < 0) {
            /* if no more frames for output - returns AVERROR(EAGAIN)
             * if flushed and no more frames for output - returns AVERROR_EOF
             * rewrite retcode to 0 to show it as normal procedure completion
             */
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
            av_frame_free(&filt_frame);
            break;
        }
        filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
        ret = EncodeWriteFrame(filt_frame, stream_index, NULL);
        if (ret < 0)
            break;
    }
    return ret;
}

int  Transcoder::FlushEncoder(unsigned int stream_index)
{
    int ret;
    int got_frame;
    if (!(mOfmtCtx->streams[stream_index]->codec->codec->capabilities &
                CODEC_CAP_DELAY))
        return 0;
    while (1) {
        av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", stream_index);
        ret = EncodeWriteFrame(NULL, stream_index, &got_frame);
        if (ret < 0)
            break;
        if (!got_frame)
            return 0;
    }
    return ret;
}

bool Transcoder::CreateRes(const char *psrcFile, const char *pDstFile)
{
    int ret;

    if (!isInitSuccess) {
        do {
            if ((ret = OpenInputFile(psrcFile)) < 0) {
                break;
            }
            if ((ret = OpenOutputFile(pDstFile)) < 0) {
                break;
            }
            if ((ret = InitFilters()) < 0) {
                break;
            }
        } while(0);
        isInitSuccess = true;
    }
    return isInitSuccess;
}

bool Transcoder::Start()
{
    int ret;
    int got_frame;
    enum AVMediaType type;
    unsigned int stream_index;
    int (*dec_func)(AVCodecContext *, AVFrame *, int *, const AVPacket *);

    /* read all packets */
    while (1) {
        if ((ret = av_read_frame(mIfmtCtx, &mPacket)) < 0)
            break;
        stream_index = mPacket.stream_index;
        type = mIfmtCtx->streams[mPacket.stream_index]->codec->codec_type;
        av_log(NULL, AV_LOG_DEBUG, "Demuxer gave frame of stream_index %u\n",
                stream_index);
        if (mFilterCtx[stream_index].filter_graph) {
            av_log(NULL, AV_LOG_DEBUG, "Going to reencode&filter the frame\n");
            mpFrame = av_frame_alloc();
            if (!mpFrame) {
                ret = AVERROR(ENOMEM);
                break;
            }
            mPacket.dts = av_rescale_q_rnd(mPacket.dts,
                    mIfmtCtx->streams[stream_index]->time_base,
                    mIfmtCtx->streams[stream_index]->codec->time_base,
                    (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
            mPacket.pts = av_rescale_q_rnd(mPacket.pts,
                    mIfmtCtx->streams[stream_index]->time_base,
                    mIfmtCtx->streams[stream_index]->codec->time_base,
                    (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
            dec_func = (type == AVMEDIA_TYPE_VIDEO) ? avcodec_decode_video2 :
                avcodec_decode_audio4;
            ret = dec_func(mIfmtCtx->streams[stream_index]->codec, mpFrame,
                    &got_frame, &mPacket);
            if (ret < 0) {
                av_frame_free(&mpFrame);
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                break;
            }
            if (got_frame) {
                mpFrame->pts = av_frame_get_best_effort_timestamp(mpFrame);
                ret = FilterEncodeWriteFrame(mpFrame, stream_index);
                av_frame_free(&mpFrame);
                if (ret < 0)
                    goto end;
            } else {
                av_frame_free(&mpFrame);
            }
        } else {
            /* remux this frame without reencoding */
            mPacket.dts = av_rescale_q_rnd(mPacket.dts,
                    mIfmtCtx->streams[stream_index]->time_base,
                    mOfmtCtx->streams[stream_index]->time_base,
                     (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
            mPacket.pts = av_rescale_q_rnd(mPacket.pts,
                    mIfmtCtx->streams[stream_index]->time_base,
                    mOfmtCtx->streams[stream_index]->time_base,
                     (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
            ret = av_interleaved_write_frame(mOfmtCtx, &mPacket);
            if (ret < 0)
                goto end;
        }
        av_free_packet(&mPacket);
    }
    /* flush filters and encoders */
    int i;
    for (i = 0; i < mIfmtCtx->nb_streams; i++) {
        /* flush filter */
        if (!mFilterCtx[i].filter_graph)
            continue;
        ret = FilterEncodeWriteFrame(NULL, i);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing filter failed\n");
            goto end;
        }
        /* flush encoder */
        ret = FlushEncoder(i);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
            goto end;
        }
    }
    av_write_trailer(mOfmtCtx);
    return true;
end:
    return false;
}

void Transcoder::DestoryRes()
{
    int i;
    av_free_packet(&mPacket);
    av_frame_free(&mpFrame);
    for (i = 0; i < mIfmtCtx->nb_streams; i++) {
        avcodec_close(mIfmtCtx->streams[i]->codec);
        if (mOfmtCtx && mOfmtCtx->nb_streams > i && mOfmtCtx->streams[i] && mOfmtCtx->streams[i]->codec)
            avcodec_close(mOfmtCtx->streams[i]->codec);
        if (mFilterCtx && mFilterCtx[i].filter_graph)
            avfilter_graph_free(&mFilterCtx[i].filter_graph);
    }
    av_free(mFilterCtx);
    avformat_close_input(&mIfmtCtx);
    if (mOfmtCtx && !(mOfmtCtx->oformat->flags & AVFMT_NOFILE))
        avio_close(mOfmtCtx->pb);
    avformat_free_context(mOfmtCtx);

    isInitSuccess = false;
}
