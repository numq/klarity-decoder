#include "decoder.h"

AVFormatContext *initializeFormatContext(const std::string &location) {
    auto formatContext = avformat_alloc_context();

    if (!formatContext) {
        std::cerr << "Error allocating AV format context" << std::endl;
        return nullptr;
    }

    if (avformat_open_input(&formatContext, location.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "Error opening file " << location << ": avformat_open_input failed" << std::endl;
        return nullptr;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        std::cerr << "Error finding stream information for file " << location << std::endl;
        avformat_close_input(&formatContext);
        return nullptr;
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

    std::cerr << "Error: No stream of type " << av_get_media_type_string(type) << " found" << std::endl;

    return nullptr;
}

AVCodecContext *initializeCodecContext(const AVStream &stream) {
    const auto codec = avcodec_find_decoder(stream.codecpar->codec_id);

    if (!codec) {
        std::cerr << "Error: Codec not found for stream with codec ID: " << stream.codecpar->codec_id << std::endl;
        return nullptr;
    }

    if (codec->type != AVMEDIA_TYPE_AUDIO && codec->type != AVMEDIA_TYPE_VIDEO) {
        std::cerr << "Error: Unsupported media type for codec" << std::endl;
        return nullptr;
    }

    auto codecContext = avcodec_alloc_context3(codec);

    if (!codecContext) {
        std::cerr << "Error: Could not allocate codec context" << std::endl;
        return nullptr;
    }

    if (avcodec_parameters_to_context(codecContext, stream.codecpar) < 0) {
        std::cerr << "Error: Could not set parameters to context for codec ID: " << stream.codecpar->codec_id
                  << std::endl;
        avcodec_free_context(&codecContext);
        return nullptr;
    }

    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        std::cerr << "Error: Could not open codec for codec ID: " << stream.codecpar->codec_id << std::endl;
        avcodec_free_context(&codecContext);
        return nullptr;
    }

    return codecContext;
}

AVPacket *nextPacket(AVFormatContext *formatContext) {
    auto packet = av_packet_alloc();

    if (!packet) {
        std::cerr << "Error allocating packet" << std::endl;
        return nullptr;
    }

    int ret = av_read_frame(formatContext, packet);

    if (ret < 0) {
        if (ret != AVERROR_EOF) {
            std::cerr << "Error reading frame" << std::endl;
        }
        av_packet_free(&packet);
        return nullptr;
    }

    return packet;
}

std::vector<uint8_t> Decoder::_processVideoFrame(const AVFrame &src) {
    const int dstWidth = src.width, dstHeight = src.height, dstFormat = AV_PIX_FMT_BGRA;

    std::vector<uint8_t *> dstData(4);
    std::vector<int> dstLinesize(4);

    int ret = av_image_alloc(dstData.data(), dstLinesize.data(), dstWidth, dstHeight, (AVPixelFormat) dstFormat, 1);
    if (ret < 0) {
        std::cerr << "Error: Failed to allocate memory for destination frame" << std::endl;
        return {};
    }

    if (!swsContext) {
        swsContext = sws_getCachedContext(
                swsContext,
                src.width, src.height, (AVPixelFormat) src.format,
                dstWidth, dstHeight, (AVPixelFormat) dstFormat,
                SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (!swsContext) {
            std::cerr << "Error: SwsContext is not initialized" << std::endl;
            av_freep(&dstData[0]);
            return {};
        }
    }

    ret = sws_scale(
            swsContext,
            src.data, src.linesize, 0, src.height,
            dstData.data(), dstLinesize.data()
    );

    if (ret < 0) {
        std::cerr << "Error: Failed to rescale video" << std::endl;
        av_freep(&dstData[0]);
        return {};
    }

    const int bufferSize = dstLinesize[0] * dstHeight;

    std::vector<uint8_t> bytes(dstData[0], dstData[0] + bufferSize);

    av_freep(&dstData[0]);

    return bytes;
}

std::vector<uint8_t> Decoder::_processAudioFrame(const AVFrame &src) {
    const int outChannels = src.channels, outSampleRate = src.sample_rate, outSampleFormat = AV_SAMPLE_FMT_DBL;

    if (src.format == outSampleFormat) {
        const auto bufferSize = av_samples_get_buffer_size(
                nullptr,
                src.channels,
                src.nb_samples,
                (AVSampleFormat) src.format,
                0
        );

        return {src.data[0], src.data[0] + bufferSize};
    }

    swrContext = swr_alloc_set_opts(
            swrContext,
            av_get_default_channel_layout(outChannels),
            (AVSampleFormat) outSampleFormat,
            outSampleRate,
            (int) src.channel_layout,
            (AVSampleFormat) src.format,
            src.sample_rate,
            0,
            nullptr
    );

    if (!swrContext) {
        std::cerr << "Error: SwrContext is not initialized" << std::endl;
        return {};
    }

    if (swr_init(swrContext) < 0) {
        std::cerr << "Error: Failed to initialize SwrContext" << std::endl;
        swr_free(&swrContext);
        return {};
    }

    auto dst = av_frame_alloc();
    if (!dst) {
        std::cerr << "Error: Failed to allocate memory for audio frame" << std::endl;
        return {};
    }

    const auto outSamples = (int) av_rescale_rnd(
            swr_get_delay(swrContext, src.sample_rate) + src.nb_samples,
            outSampleRate,
            src.sample_rate,
            AV_ROUND_UP
    );

    dst->channel_layout = src.channel_layout;
    dst->sample_rate = src.sample_rate;
    dst->format = outSampleFormat;
    dst->nb_samples = outSamples;

    if (av_frame_get_buffer(dst, 0) < 0) {
        std::cerr << "Error: Failed to allocate memory for audio buffer" << std::endl;
        av_frame_free(&dst);
        return {};
    }

    int ret = swr_convert_frame(swrContext, dst, &src);

    if (ret < 0) {
        std::cerr << "Error: Failed to resample audio" << std::endl;
        av_frame_free(&dst);
        return {};
    }

    const auto bufferSize = av_samples_get_buffer_size(
            nullptr,
            dst->channels,
            dst->nb_samples,
            (AVSampleFormat) dst->format,
            0
    );

    std::vector<uint8_t> bytes(dst->data[0], dst->data[0] + bufferSize);

    av_frame_free(&dst);

    return bytes;
}

Frame *Decoder::_readFrame(bool doVideo, bool doAudio) {
    auto avFrame = av_frame_alloc();

    if (!avFrame) {
        std::cerr << "Error allocating frame" << std::endl;
        return nullptr;
    }

    Frame *frame = nullptr;

    AVPacket *packet = nullptr;

    while (!frame) {
        packet = nextPacket(formatContext);

        if (!packet) break;

        auto hasVideo = doVideo && videoCodecContext != nullptr && videoStream != nullptr && videoStream->index >= 0 &&
                        packet->stream_index == videoStream->index;

        auto hasAudio = doAudio && audioCodecContext != nullptr && audioStream != nullptr && audioStream->index >= 0 &&
                        packet->stream_index == audioStream->index;

        if (hasVideo) {
            packet->dts = av_rescale_q(packet->dts, videoStream->time_base, av_make_q(1, AV_TIME_BASE));

            packet->pts = av_rescale_q(packet->pts, videoStream->time_base, av_make_q(1, AV_TIME_BASE));

            int ret = avcodec_send_packet(videoCodecContext, packet);

            if (ret < 0) {
                std::cerr << "Error sending packet to video codec" << std::endl;
                av_packet_unref(packet);
                continue;
            }

            ret = avcodec_receive_frame(videoCodecContext, avFrame);

            if (ret < 0) {
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    av_packet_unref(packet);
                    continue;
                } else {
                    std::cerr << "Error receiving frame" << std::endl;
                    break;
                }
            }

            const auto timestampMicros = avFrame->best_effort_timestamp;

            auto pixels = _processVideoFrame(*avFrame);

            if (!pixels.empty()) frame = new Frame(Frame::FrameType::VIDEO, timestampMicros, pixels);
        } else if (hasAudio) {
            packet->dts = av_rescale_q(packet->dts, audioStream->time_base, av_make_q(1, AV_TIME_BASE));

            packet->pts = av_rescale_q(packet->pts, audioStream->time_base, av_make_q(1, AV_TIME_BASE));

            if (avcodec_send_packet(audioCodecContext, packet) < 0) {
                std::cerr << "Error sending packet to audio codec" << std::endl;
                av_packet_unref(packet);
                continue;
            }

            int ret = avcodec_receive_frame(audioCodecContext, avFrame);

            if (ret < 0) {
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    av_packet_unref(packet);
                    continue;
                } else {
                    std::cerr << "Error receiving frame" << std::endl;
                    break;
                }
            }

            const auto timestampMicros = avFrame->best_effort_timestamp;

            auto samples = _processAudioFrame(*avFrame);

            if (!samples.empty()) frame = new Frame(Frame::FrameType::AUDIO, timestampMicros, samples);
        }

        av_packet_unref(packet);
    }

    av_frame_free(&avFrame);

    av_packet_free(&packet);

    return frame;
}

void Decoder::_seekTo(const long timestampMicros) {
    if (videoCodecContext) {
        avcodec_flush_buffers(videoCodecContext);
    }

    if (audioCodecContext) {
        avcodec_flush_buffers(audioCodecContext);
    }

    av_seek_frame(formatContext, -1, timestampMicros, AVSEEK_FLAG_BACKWARD);
}

void Decoder::_reset() {
    if (videoCodecContext) {
        avcodec_flush_buffers(videoCodecContext);
    }

    if (audioCodecContext) {
        avcodec_flush_buffers(audioCodecContext);
    }

    av_seek_frame(formatContext, -1, 0, AVSEEK_FLAG_ANY);
}

void Decoder::_cleanUp() {
    if (swsContext) {
        sws_freeContext(swsContext);
        swsContext = nullptr;
    }

    if (swrContext) {
        swr_free(&swrContext);
        swrContext = nullptr;
    }

    if (formatContext) {
        avformat_close_input(&formatContext);
        formatContext = nullptr;
    }

    if (videoCodecContext) {
        avcodec_free_context(&videoCodecContext);
        videoCodecContext = nullptr;
    }

    if (audioCodecContext) {
        avcodec_free_context(&audioCodecContext);
        audioCodecContext = nullptr;
    }

    videoStream = nullptr;

    audioStream = nullptr;
}

Decoder::Decoder(const std::string &location, const bool findAudioStream, const bool findVideoStream) {
    std::lock_guard<std::mutex> lock(mutex);

    av_log_set_level(AV_LOG_FATAL);

    avformat_network_init();

    try {
        if (formatContext) _cleanUp();

        formatContext = initializeFormatContext(location);

        if (!formatContext) {
            throw std::logic_error(("Failed to initialize format context for location: " + location).c_str());
        }

        format = new Format(formatContext->duration);

        if (findAudioStream) audioStream = initializeStream(*formatContext, AVMEDIA_TYPE_AUDIO);

        if (audioStream) {
            audioCodecContext = initializeCodecContext(*audioStream);

            format->sampleRate = audioCodecContext->sample_rate;

            format->channels = audioCodecContext->channels;
        }

        if (findVideoStream) videoStream = initializeStream(*formatContext, AVMEDIA_TYPE_VIDEO);

        if (videoStream) {
            videoCodecContext = initializeCodecContext(*videoStream);

            format->width = videoCodecContext->width;

            format->height = videoCodecContext->height;

            const auto rational = videoStream->avg_frame_rate;
            format->frameRate = rational.den > 0 ? (double) rational.num / rational.den : 0.0;
        }

        if (!audioStream && !videoStream) throw std::logic_error("Unable to create empty decoder");
    } catch (...) {
        _cleanUp();
        throw;
    }
}

Decoder::~Decoder() {
    std::lock_guard<std::mutex> lock(mutex);

    _cleanUp();
}

Frame *Decoder::readFrame(const bool doVideo, const bool doAudio) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!formatContext) return nullptr;

    return _readFrame(doVideo, doAudio);
}

void Decoder::seekTo(const long timestampMicros) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!formatContext) return;

    _seekTo(timestampMicros);
}

void Decoder::reset() {
    std::lock_guard<std::mutex> lock(mutex);

    _reset();
}