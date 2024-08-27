#include "decoder.h"


std::vector<uint8_t> Decoder::_processAudioFrame(const AVFrame &src) {
    if (format->sampleRate <= 0 || format->channels <= 0) {
        throw DecoderException("Invalid audio format");
    }

    if (src.format != AV_SAMPLE_FMT_FLT) {
        swrContext = swr_alloc_set_opts(
                swrContext,
                av_get_default_channel_layout(src.channels),
                AV_SAMPLE_FMT_FLT,
                src.sample_rate,
                av_get_default_channel_layout(src.channels),
                static_cast<AVSampleFormat>(src.format),
                src.sample_rate,
                0, nullptr);

        if (!swrContext || swr_init(swrContext) < 0) {
            throw DecoderException("Could not initialize the resampling context");
        }

        auto output_samples = av_rescale_rnd(
                swr_get_delay(swrContext, src.sample_rate) + src.nb_samples,
                src.sample_rate, src.sample_rate, AV_ROUND_UP);

        std::vector<uint8_t> data(output_samples * src.channels * sizeof(float));

        auto dataPtr = data.data();

        int converted_samples = swr_convert(
                swrContext,
                &dataPtr,
                (int) output_samples,
                (const uint8_t **) src.data,
                src.nb_samples
        );

        if (converted_samples < 0) {
            throw DecoderException("Error while converting the audio frame");
        }

        data.resize(converted_samples * src.channels * sizeof(float));

        return {data.begin(), data.end()};
    }

    std::vector<uint8_t> output(src.nb_samples * src.channels * sizeof(float));

    memcpy(output.data(), src.data[0], src.nb_samples * src.channels * sizeof(float));

    return output;
}

std::vector<uint8_t> Decoder::_processVideoFrame(const AVFrame &src, int64_t width, int64_t height) {
    if (format->width <= 0 || format->height <= 0) {
        throw DecoderException("Invalid video format");
    }

    auto dstWidth = static_cast<int>(width > 0 && width <= format->width ? width : format->width);
    auto dstHeight = static_cast<int>(height > 0 && height <= format->height ? height : format->height);

    auto srcFormat = static_cast<AVPixelFormat>(src.format);
    auto dstFormat = AV_PIX_FMT_RGB565;

    int dstLinesize[3];
    av_image_fill_linesizes(dstLinesize, dstFormat, dstWidth);

    if (src.width != dstWidth || src.height != dstHeight || src.format != dstFormat) {
        swsContext = sws_getCachedContext(
                swsContext,
                src.width, src.height, srcFormat,
                dstWidth, dstHeight, dstFormat,
                SWS_BICUBIC,
                nullptr, nullptr, nullptr
        );

        if (!swsContext) {
            throw DecoderException("Could not initialize the conversion context");
        }

        int bufferSize = av_image_get_buffer_size(dstFormat, dstWidth, dstHeight, 1);
        if (bufferSize < 0) {
            throw DecoderException("Could not get buffer size, error: " + std::to_string(bufferSize));
        }

        std::vector<uint8_t> output(bufferSize + AV_INPUT_BUFFER_PADDING_SIZE);

        uint8_t *dst[3] = {nullptr};
        av_image_fill_pointers(dst, dstFormat, dstHeight, output.data(), dstLinesize);

        if (sws_scale(swsContext, src.data, src.linesize, 0, src.height, dst, dstLinesize) < 0) {
            throw DecoderException("Error while converting the video frame");
        }

        return output;
    }

    std::vector<uint8_t> output(src.linesize[0] * src.height);

    memcpy(output.data(), src.data[0], src.linesize[0] * src.height);

    return output;
}

Decoder::Decoder(const char *location, bool findAudioStream, bool findVideoStream) {
    std::lock_guard<std::shared_mutex> lock(mutex);

    formatContext = avformat_alloc_context();
    if (!formatContext) {
        throw DecoderException("Could not allocate format context");
    }

    if (avformat_open_input(&formatContext, location, nullptr, nullptr) < 0) {
        avformat_free_context(formatContext);
        formatContext = nullptr;
        throw DecoderException("Couldn't open input stream");
    }

    formatContext->flags = AVFMT_SEEK_TO_PTS;

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        avformat_close_input(&formatContext);
        throw DecoderException("Couldn't find stream information");
    }

    format = new Format(location, av_rescale_q(formatContext->duration, AV_TIME_BASE_Q, (AVRational) {1, 1000000}));

    if (findAudioStream) {
        for (unsigned i = 0; i < formatContext->nb_streams; i++) {
            if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStream = formatContext->streams[i];
                auto codec = avcodec_find_decoder(audioStream->codecpar->codec_id);
                if (!codec) {
                    throw DecoderException("Audio codec not found");
                }

                audioCodecContext = avcodec_alloc_context3(codec);
                if (!audioCodecContext) {
                    throw DecoderException("Could not allocate audio codec context");
                }

                if (avcodec_parameters_to_context(audioCodecContext, audioStream->codecpar) < 0) {
                    avcodec_free_context(&audioCodecContext);
                    throw DecoderException("Could not copy audio codec parameters to context");
                }

                if (avcodec_open2(audioCodecContext, codec, nullptr) < 0) {
                    avcodec_free_context(&audioCodecContext);
                    throw DecoderException("Could not open audio codec");
                }

                format->sampleRate = audioCodecContext->sample_rate;
                format->channels = audioCodecContext->channels;

                break;
            }
        }
    }

    if (findVideoStream) {
        for (unsigned i = 0; i < formatContext->nb_streams; i++) {
            if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStream = formatContext->streams[i];

                auto codec = avcodec_find_decoder(videoStream->codecpar->codec_id);
                if (!codec) {
                    throw DecoderException("Video codec not found");
                }

                videoCodecContext = avcodec_alloc_context3(codec);
                if (!videoCodecContext) {
                    throw DecoderException("Could not allocate video codec context");
                }

                if (avcodec_parameters_to_context(videoCodecContext, videoStream->codecpar) < 0) {
                    avcodec_free_context(&videoCodecContext);
                    throw DecoderException("Could not copy video codec parameters to context");
                }

                if (avcodec_open2(videoCodecContext, codec, nullptr) < 0) {
                    avcodec_free_context(&videoCodecContext);
                    throw DecoderException("Could not open video codec");
                }

                format->width = videoCodecContext->width;
                format->height = videoCodecContext->height;

                const auto rational = videoStream->avg_frame_rate;
                format->frameRate = rational.den > 0 ? static_cast<double>(rational.num) / rational.den : 0.0;

                break;
            }
        }
    }

    if ((findAudioStream && !audioStream) && (findVideoStream && !videoStream)) {
        avformat_free_context(formatContext);
        formatContext = nullptr;
        throw DecoderException("No valid streams found");
    }
}

Decoder::~Decoder() {
    std::lock_guard<std::shared_mutex> lock(mutex);

    if (audioCodecContext) {
        avcodec_free_context(&audioCodecContext);
    }

    if (videoCodecContext) {
        avcodec_free_context(&videoCodecContext);
    }

    if (formatContext) {
        avformat_close_input(&formatContext);
        avformat_free_context(formatContext);
    }

    if (swrContext) {
        swr_free(&swrContext);
    }

    if (swsContext) {
        sws_freeContext(swsContext);
    }

    delete format;
}

Frame *Decoder::nextFrame(int64_t width, int64_t height) {
    std::lock_guard<std::shared_mutex> lock(mutex);

    if (!format) {
        throw DecoderException("Unable to use uninitialized decoder");
    }

    auto packet = av_packet_alloc();

    if (!packet) {
        throw DecoderException("Could not allocate packet");
    }

    while (av_read_frame(formatContext, packet) == 0) {
        if (audioStream) {
            if (packet->stream_index == audioStream->index) {
                avcodec_send_packet(audioCodecContext, packet);
                AVFrame *frame = av_frame_alloc();
                if (avcodec_receive_frame(audioCodecContext, frame) == 0) {
                    std::vector<uint8_t> data = _processAudioFrame(*frame);
                    const auto timestampMicros = static_cast<int64_t>(
                            std::round(
                                    static_cast<double>(frame->best_effort_timestamp) * av_q2d(audioStream->time_base) *
                                    1000000)
                    );
                    av_frame_free(&frame);
                    av_packet_free(&packet);
                    return new Frame(Frame::AUDIO, timestampMicros, data);
                }
            }
        }

        if (videoStream) {
            if (packet->stream_index == videoStream->index) {
                avcodec_send_packet(videoCodecContext, packet);
                AVFrame *frame = av_frame_alloc();
                if (avcodec_receive_frame(videoCodecContext, frame) == 0) {
                    std::vector<uint8_t> data = _processVideoFrame(*frame, width, height);
                    const auto timestampMicros = static_cast<int64_t>(
                            std::round(
                                    static_cast<double>(frame->best_effort_timestamp) * av_q2d(videoStream->time_base) *
                                    1000000)
                    );
                    av_frame_free(&frame);
                    av_packet_free(&packet);
                    return new Frame(Frame::VIDEO, timestampMicros, data);
                }
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    return nullptr;
}

void Decoder::seekTo(long timestampMicros, bool keyframesOnly) {
    std::lock_guard<std::shared_mutex> lock(mutex);

    if (!format) {
        throw DecoderException("Unable to use uninitialized decoder");
    }

    if (timestampMicros < 0 || timestampMicros > format->durationMicros) {
        throw DecoderException("Timestamp out of bounds");
    }

    int ret = av_seek_frame(formatContext, -1, timestampMicros, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        throw DecoderException("Error seeking to timestamp: " + std::to_string(timestampMicros));
    }

    if (audioCodecContext) {
        avcodec_flush_buffers(audioCodecContext);
    }

    if (videoCodecContext) {
        avcodec_flush_buffers(videoCodecContext);
    }

    if (!keyframesOnly) {
        auto packet = av_packet_alloc();
        auto frame = av_frame_alloc();

        if (!packet || !frame) {
            if (packet) av_packet_free(&packet);
            if (frame) av_frame_free(&frame);
            throw DecoderException("Error allocating packet or frame");
        }

        bool found = false;

        int64_t maxFrames = 0;

        int64_t audioTimestamp = (audioStream) ? av_rescale_q(timestampMicros, AV_TIME_BASE_Q, audioStream->time_base)
                                               : 0;
        int64_t videoTimestamp = (videoStream) ? av_rescale_q(timestampMicros, AV_TIME_BASE_Q, videoStream->time_base)
                                               : 0;

        if (audioCodecContext && audioStream) {
            maxFrames = std::max(static_cast<int64_t>(static_cast<double>(timestampMicros) /
                                                      (static_cast<double>(format->durationMicros) /
                                                       static_cast<double>(audioStream->nb_frames))), maxFrames);
        }

        if (videoCodecContext && videoStream) {
            maxFrames = std::max(static_cast<int64_t>(static_cast<double>(timestampMicros) /
                                                      (static_cast<double>(format->durationMicros) /
                                                       static_cast<double>(videoStream->nb_frames))), maxFrames);
        }

        while (maxFrames-- > 0 && av_read_frame(formatContext, packet) >= 0) {
            if (audioCodecContext && audioStream && packet->stream_index == audioStream->index) {
                avcodec_send_packet(audioCodecContext, packet);

                while (avcodec_receive_frame(audioCodecContext, frame) == 0) {
                    if (frame->best_effort_timestamp >= audioTimestamp) {
                        found = true;
                        break;
                    }
                }
            } else if (videoCodecContext && videoStream && packet->stream_index == videoStream->index) {
                avcodec_send_packet(videoCodecContext, packet);

                while (avcodec_receive_frame(videoCodecContext, frame) == 0) {
                    if (frame->best_effort_timestamp >= videoTimestamp) {
                        found = true;
                        break;
                    }
                }
            }

            av_packet_unref(packet);

            if (found) break;
        }

        av_frame_free(&frame);
        av_packet_free(&packet);
    }
}

void Decoder::reset() {
    std::lock_guard<std::shared_mutex> lock(mutex);

    if (!format) {
        throw DecoderException("Unable to use uninitialized decoder");
    }

    if (av_seek_frame(formatContext, -1, 0, AVSEEK_FLAG_FRAME) < 0) {
        throw DecoderException("Error resetting stream");
    }

    if (audioCodecContext) {
        avcodec_flush_buffers(audioCodecContext);
    }

    if (videoCodecContext) {
        avcodec_flush_buffers(videoCodecContext);
    }
}