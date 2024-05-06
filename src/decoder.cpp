#include "decoder.h"

AVFormatContext *initializeFormatContext(const std::string &location) {
    auto formatContext = avformat_alloc_context();

    if (!formatContext) {
        throw std::logic_error("Error allocating AV format context");
    }

    if (avformat_open_input(&formatContext, location.c_str(), nullptr, nullptr) != 0) {
        throw std::logic_error(
                ("Error finding stream information for file " + location + ": avformat_open_input failed").c_str());
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        avformat_close_input(&formatContext);
        throw std::logic_error(("Error finding stream information for file " + location).c_str());
    }

    return formatContext;
}

AVStream *initializeStream(const AVFormatContext &formatContext, const AVMediaType type) {
    for (int i = 0; i < formatContext.nb_streams; ++i) {
        const auto stream = formatContext.streams[i];

        if (stream->codecpar->codec_type == type) {
            return stream;
        }
    }

    return nullptr;
}

AVCodecContext *initializeCodecContext(const AVStream &stream) {
    const auto codec = avcodec_find_decoder(stream.codecpar->codec_id);

    if (!codec) {
        throw std::logic_error(
                ("Codec not found for stream with codec ID: " + std::to_string(stream.codecpar->codec_id)).c_str());
    }

    if (codec->type != AVMEDIA_TYPE_AUDIO && codec->type != AVMEDIA_TYPE_VIDEO) {
        throw std::logic_error("Unsupported media type for codec");
    }

    auto codecContext = avcodec_alloc_context3(codec);

    if (!codecContext) {
        throw std::logic_error("Could not allocate codec context");
    }

    if (avcodec_parameters_to_context(codecContext, stream.codecpar) < 0) {
        avcodec_free_context(&codecContext);
        throw std::logic_error(("Could not set parameters to context for codec ID: " +
                                std::to_string(stream.codecpar->codec_id)).c_str());
    }

    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        avcodec_free_context(&codecContext);
        throw std::logic_error(
                ("Could not open codec for codec ID: " + std::to_string(stream.codecpar->codec_id)).c_str());
    }

    return codecContext;
}

AVPacket *nextPacket(AVFormatContext *formatContext) {
    auto packet = av_packet_alloc();

    if (!packet) {
        throw std::logic_error("Error allocating packet");
    }

    int ret = av_read_frame(formatContext, packet);

    if (ret < 0) {
        av_packet_free(&packet);
        return nullptr;
    }

    return packet;
}

std::vector<uint8_t> Decoder::_processVideoFrame(const AVFrame &src) {
    const int dstWidth = src.width, dstHeight = src.height, dstFormat = AV_PIX_FMT_BGRA;

    std::vector<uint8_t *> dstData(4);
    std::vector<int> dstLinesize(4);

    int ret = av_image_alloc(
            dstData.data(),
            dstLinesize.data(),
            dstWidth,
            dstHeight,
            (AVPixelFormat) dstFormat,
            1
    );
    if (ret < 0) {
        throw std::logic_error("Failed to allocate memory for destination frame");
    }

    if (!swsContext) {
        swsContext = sws_getCachedContext(
                swsContext,
                src.width, src.height, (AVPixelFormat) src.format,
                dstWidth, dstHeight, (AVPixelFormat) dstFormat,
                SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (!swsContext) {
            av_freep(&dstData[0]);
            throw std::logic_error("SwsContext is not initialized");
        }
    }

    ret = sws_scale(
            swsContext,
            src.data, src.linesize, 0, src.height,
            dstData.data(), dstLinesize.data()
    );

    if (ret < 0) {
        av_freep(&dstData[0]);
        throw std::logic_error("Failed to rescale video");
    }

    const int bufferSize = dstLinesize[0] * dstHeight;

    std::vector<uint8_t> bytes(dstData[0], dstData[0] + bufferSize);

    av_freep(&dstData[0]);

    return bytes;
}

std::vector<uint8_t> Decoder::_processAudioFrame(const AVFrame &src) {
    const int dstChannels = src.channels, dstSampleRate = src.sample_rate, dstSampleFormat = AV_SAMPLE_FMT_FLT;

    const int64_t srcChannelLayout = av_get_default_channel_layout(src.channels), dstChannelLayout = srcChannelLayout;

    swrContext = swr_alloc_set_opts(
            swrContext,
            dstChannelLayout,
            (AVSampleFormat) dstSampleFormat,
            dstSampleRate,
            srcChannelLayout,
            (AVSampleFormat) src.format,
            src.sample_rate,
            0,
            nullptr
    );

    if (!swrContext) {
        throw std::logic_error("SwrContext is not initialized");
    }

    if (swr_init(swrContext) < 0) {
        swr_free(&swrContext);
        throw std::logic_error("Failed to initialize SwrContext");
    }

    const int64_t outSamples = swr_get_delay(swrContext, src.sample_rate) + src.nb_samples;

    std::vector<uint8_t> dstData(
            av_samples_get_buffer_size(
                    nullptr,
                    dstChannels,
                    (int) outSamples,
                    (AVSampleFormat) dstSampleFormat,
                    0
            )
    );

    int convertedSamples = swr_convert(
            swrContext,
            (uint8_t **) &dstData,
            (int) outSamples,
            (const uint8_t **) src.data,
            src.nb_samples
    );

    if (convertedSamples < 0) {
        swr_free(&swrContext);
        throw std::logic_error("Failed to resample audio");
    }

    swr_free(&swrContext);

    return dstData;
}

Frame *Decoder::_nextFrame() {
    auto avFrame = av_frame_alloc();

    if (!avFrame) {
        throw std::logic_error("Failed to allocate frame");
    }

    Frame *frame = nullptr;

    AVPacket *packet = nullptr;

    while (!frame) {
        packet = nextPacket(formatContext);

        if (!packet) {
            break;
        }

        auto hasAudio = audioCodecContext != nullptr && audioStream != nullptr && audioStream->index >= 0 &&
                        packet->stream_index == audioStream->index;

        auto hasVideo = videoCodecContext != nullptr && videoStream != nullptr && videoStream->index >= 0 &&
                        packet->stream_index == videoStream->index;

        if (hasAudio) {
            if (avcodec_send_packet(audioCodecContext, packet) < 0) {
                av_packet_unref(packet);
                continue;
            }

            int ret = avcodec_receive_frame(audioCodecContext, avFrame);

            if (ret < 0) {
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    av_packet_unref(packet);
                    continue;
                } else {
                    throw std::logic_error("Failed to receive frame");
                }
            }

            const auto timestampMicros = static_cast<int64_t>(
                    std::round((double) avFrame->best_effort_timestamp * av_q2d(audioStream->time_base) * 1000000)
            );

            auto samples = _processAudioFrame(*avFrame);

            if (!samples.empty()) frame = new Frame(Frame::FrameType::AUDIO, timestampMicros, samples);
        } else if (hasVideo) {
            int ret = avcodec_send_packet(videoCodecContext, packet);

            if (ret < 0) {
                av_packet_unref(packet);
                continue;
            }

            ret = avcodec_receive_frame(videoCodecContext, avFrame);

            if (ret < 0) {
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    av_packet_unref(packet);
                    continue;
                } else {
                    throw std::logic_error("Failed to receive frame");
                }
            }

            const auto timestampMicros = static_cast<int64_t>(
                    std::round((double) avFrame->best_effort_timestamp * av_q2d(videoStream->time_base) * 1000000)
            );

            auto pixels = _processVideoFrame(*avFrame);

            if (!pixels.empty()) frame = new Frame(Frame::FrameType::VIDEO, timestampMicros, pixels);
        }

        av_packet_unref(packet);
    }

    av_frame_free(&avFrame);

    av_packet_free(&packet);

    return frame;
}

void Decoder::_seekTo(const long timestampMicros) {
    if (audioCodecContext) {
        avcodec_flush_buffers(audioCodecContext);
    }

    if (videoCodecContext) {
        avcodec_flush_buffers(videoCodecContext);
    }

    avformat_seek_file(formatContext, -1, INT64_MIN, timestampMicros, INT64_MAX, AVSEEK_FLAG_ANY);
}

void Decoder::_reset() {
    if (audioCodecContext) {
        avcodec_flush_buffers(audioCodecContext);
    }

    if (videoCodecContext) {
        avcodec_flush_buffers(videoCodecContext);
    }

    avformat_seek_file(formatContext, -1, INT64_MIN, 0, INT64_MAX, AVSEEK_FLAG_BACKWARD);
}

void Decoder::_cleanUp() {
    if (swrContext) {
        swr_free(&swrContext);
        swrContext = nullptr;
    }

    if (swsContext) {
        sws_freeContext(swsContext);
        swsContext = nullptr;
    }

    if (formatContext) {
        avformat_close_input(&formatContext);
        formatContext = nullptr;
    }

    if (audioCodecContext) {
        avcodec_free_context(&audioCodecContext);
        audioCodecContext = nullptr;
    }

    if (videoCodecContext) {
        avcodec_free_context(&videoCodecContext);
        videoCodecContext = nullptr;
    }

    audioStream = nullptr;

    videoStream = nullptr;
}

Decoder::Decoder(const std::string &location, const bool findAudioStream, const bool findVideoStream) {
    std::lock_guard<std::mutex> lock(mutex);

    av_log_set_level(AV_LOG_FATAL);

    avformat_network_init();

    try {
        formatContext = initializeFormatContext(location);

        if (!formatContext) {
            throw std::logic_error(("Failed to initialize format context for location: " + location).c_str());
        }

        int64_t durationMicros = 0;
        for (int i = 0; i < formatContext->nb_streams; ++i) {
            const AVStream *stream = formatContext->streams[i];
            int64_t streamDurationMicros = av_rescale_q(
                    stream->duration,
                    stream->time_base,
                    AVRational{1, AV_TIME_BASE}
            );
            durationMicros = std::max(durationMicros, streamDurationMicros);
        }

        format = new Format(durationMicros);

        if (findAudioStream) {
            audioStream = initializeStream(*formatContext, AVMEDIA_TYPE_AUDIO);

            if (audioStream) {
                audioCodecContext = initializeCodecContext(*audioStream);
                format->sampleRate = audioCodecContext->sample_rate;
                format->channels = audioCodecContext->channels;
            }
        }

        if (findVideoStream) {
            videoStream = initializeStream(*formatContext, AVMEDIA_TYPE_VIDEO);

            if (videoStream) {
                videoCodecContext = initializeCodecContext(*videoStream);
                format->width = videoCodecContext->width;
                format->height = videoCodecContext->height;
                const auto rational = videoStream->avg_frame_rate;
                format->frameRate = rational.den > 0 ? (double) rational.num / rational.den : 0.0;
            }
        }

        if (!audioStream && !videoStream) {
            throw std::logic_error("Unable to open media");
        }
    } catch (...) {
        _cleanUp();
        throw;
    }
}

Decoder::~Decoder() {
    std::lock_guard<std::mutex> lock(mutex);

    _cleanUp();
}

Frame *Decoder::nextFrame() {
    std::lock_guard<std::mutex> lock(mutex);

    if (!format) {
        throw std::logic_error("Not initialized");
    }

    return _nextFrame();
}

void Decoder::seekTo(const long timestampMicros) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!format) {
        throw std::logic_error("Not initialized");
    }

    if (timestampMicros < 0 || timestampMicros > format->durationMicros) {
        throw std::logic_error("Invalid timestamp");
    }

    _seekTo(timestampMicros);
}

void Decoder::reset() {
    std::lock_guard<std::mutex> lock(mutex);

    if (!format) {
        throw std::logic_error("Not initialized");
    }

    _reset();
}