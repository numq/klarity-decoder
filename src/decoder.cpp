#include "decoder.h"

Decoder::Decoder(const std::string &location, bool findAudioStream, bool findVideoStream) {
    std::lock_guard<std::mutex> lock(mutex);

    formatContext = avformat_alloc_context();

    if (!formatContext) {
        std::cerr << "Could not allocate format context." << std::endl;
        return;
    }

    if (avformat_open_input(&formatContext, location.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Couldn't open input stream." << std::endl;
        _cleanUp();
        return;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        std::cerr << "Couldn't find stream information." << std::endl;
        _cleanUp();
        return;
    }

    format = new Format(static_cast<uint64_t>(static_cast<double>(formatContext->duration) /
                                              (av_q2d(AV_TIME_BASE_Q) * 1000000)));

    if (findAudioStream) {
        for (unsigned i = 0; i < formatContext->nb_streams; i++) {
            if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStream = formatContext->streams[i];
                auto codec = avcodec_find_decoder(audioStream->codecpar->codec_id);
                if (!codec) {
                    std::cerr << "Audio codec not found." << std::endl;
                    continue;
                }
                audioCodecContext = avcodec_alloc_context3(codec);
                if (!audioCodecContext) {
                    std::cerr << "Could not allocate audio codec context." << std::endl;
                    continue;
                }
                if (avcodec_parameters_to_context(audioCodecContext, audioStream->codecpar) < 0) {
                    std::cerr << "Could not copy audio codec parameters to context." << std::endl;
                    avcodec_free_context(&audioCodecContext);
                    continue;
                }
                if (avcodec_open2(audioCodecContext, codec, nullptr) < 0) {
                    std::cerr << "Could not open audio codec." << std::endl;
                    avcodec_free_context(&audioCodecContext);
                    continue;
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
                    std::cerr << "Video codec not found." << std::endl;
                    continue;
                }
                videoCodecContext = avcodec_alloc_context3(codec);
                if (!videoCodecContext) {
                    std::cerr << "Could not allocate video codec context." << std::endl;
                    continue;
                }
                if (avcodec_parameters_to_context(videoCodecContext, videoStream->codecpar) < 0) {
                    std::cerr << "Could not copy video codec parameters to context." << std::endl;
                    avcodec_free_context(&videoCodecContext);
                    continue;
                }
                if (avcodec_open2(videoCodecContext, codec, nullptr) < 0) {
                    std::cerr << "Could not open video codec." << std::endl;
                    avcodec_free_context(&videoCodecContext);
                    continue;
                }

                format->width = videoCodecContext->width;
                format->height = videoCodecContext->height;
                const auto rational = videoStream->avg_frame_rate;
                format->frameRate = rational.den > 0 ? (double) rational.num / rational.den : 0.0;

                break;
            }
        }
    }

    if (!audioStream && !videoStream) {
        std::cerr << "No valid streams found." << std::endl;
        _cleanUp();
        return;
    }
}

Decoder::~Decoder() {
    std::lock_guard<std::mutex> lock(mutex);
    _cleanUp();
}

Frame *Decoder::nextFrame() {
    std::lock_guard<std::mutex> lock(mutex);
    return _nextFrame();
}

void Decoder::seekTo(long timestampMicros) {
    std::lock_guard<std::mutex> lock(mutex);
    _seekTo(timestampMicros);
}

void Decoder::reset() {
    std::lock_guard<std::mutex> lock(mutex);
    _reset();
}

Frame *Decoder::_nextFrame() {
    auto packet = av_packet_alloc();

    if (!packet) {
        std::cerr << "Could not initialize the conversion context." << std::endl;
        return nullptr;
    }

    while (av_read_frame(formatContext, packet) == 0) {
        if (audioStream) {
            if (packet->stream_index == audioStream->index) {
                avcodec_send_packet(audioCodecContext, packet);
                AVFrame *frame = av_frame_alloc();
                if (avcodec_receive_frame(audioCodecContext, frame) == 0) {
                    std::vector<uint8_t> data = _processAudioFrame(*frame);
                    const auto timestampMicros = static_cast<int64_t>(
                            std::round((double) frame->best_effort_timestamp * av_q2d(audioStream->time_base) * 1000000)
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
                    std::vector<uint8_t> data = _processVideoFrame(*frame);
                    const auto timestampMicros = static_cast<int64_t>(
                            std::round((double) frame->best_effort_timestamp * av_q2d(videoStream->time_base) * 1000000)
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

std::vector<uint8_t> Decoder::_processVideoFrame(const AVFrame &src) {
    int width = src.width;
    int height = src.height;
    auto srcFormat = static_cast<AVPixelFormat>(src.format);
    auto dstFormat = AV_PIX_FMT_BGRA;

    std::vector<uint8_t> data(width * height * 4);
    uint8_t *dst[4] = {data.data(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {width * 4, 0, 0, 0};

    if (srcFormat != dstFormat) {
        if (!swsContext) {
            swsContext = sws_getContext(
                    width, height, srcFormat,
                    width, height, dstFormat,
                    SWS_BICUBIC,
                    nullptr,
                    nullptr,
                    nullptr
            );

            if (!swsContext) {
                std::cerr << "Could not initialize the conversion context." << std::endl;
                return {};
            }
        }

        if (sws_scale(swsContext, src.data, src.linesize, 0, height, dst, dstLinesize) < 0) {
            std::cerr << "Error while converting the video frame." << std::endl;
            return {};
        }
    } else {
        for (int i = 0; i < height; ++i) {
            memcpy(dst[0] + i * dstLinesize[0], src.data[0] + i * src.linesize[0], dstLinesize[0]);
        }
    }

    return data;
}

std::vector<uint8_t> Decoder::_processAudioFrame(const AVFrame &src) {
    if (!swrContext) {
        swrContext = swr_alloc_set_opts(
                nullptr,
                av_get_default_channel_layout(src.channels),
                AV_SAMPLE_FMT_FLT,
                src.sample_rate,
                av_get_default_channel_layout(src.channels),
                static_cast<AVSampleFormat>(src.format),
                src.sample_rate,
                0, nullptr);

        if (!swrContext || swr_init(swrContext) < 0) {
            std::cerr << "Could not initialize the resampling context." << std::endl;
            return {};
        }
    }

    auto output_samples = av_rescale_rnd(
            swr_get_delay(swrContext, src.sample_rate) + src.nb_samples,
            src.sample_rate, src.sample_rate, AV_ROUND_UP);

    std::vector<uint8_t> data(output_samples * src.channels * sizeof(float));
    uint8_t *output_data[1] = {data.data()};

    if (src.format != AVSampleFormat::AV_SAMPLE_FMT_FLT) {
        int converted_samples = swr_convert(
                swrContext,
                output_data,
                (int) output_samples,
                (const uint8_t **) src.data,
                src.nb_samples
        );

        if (converted_samples < 0) {
            std::cerr << "Error while converting the audio frame." << std::endl;
            return {};
        }

        data.resize(converted_samples * src.channels * sizeof(float));
    }

    return data;
}

void Decoder::_seekTo(long timestampMicros) {
    if (formatContext) {
        av_seek_frame(formatContext, -1, timestampMicros, AVSEEK_FLAG_ANY);
        if (audioCodecContext) avcodec_flush_buffers(audioCodecContext);
        if (videoCodecContext) avcodec_flush_buffers(videoCodecContext);
    }
}

void Decoder::_reset() {
    if (formatContext) {
        av_seek_frame(formatContext, -1, 0, AVSEEK_FLAG_BACKWARD);
        if (audioCodecContext) avcodec_flush_buffers(audioCodecContext);
        if (videoCodecContext) avcodec_flush_buffers(videoCodecContext);
    }
}

void Decoder::_cleanUp() {
    if (audioCodecContext) {
        avcodec_close(audioCodecContext);
        avcodec_free_context(&audioCodecContext);
        audioCodecContext = nullptr;
        delete audioCodecContext;
    }
    if (videoCodecContext) {
        avcodec_close(videoCodecContext);
        avcodec_free_context(&videoCodecContext);
        videoCodecContext = nullptr;
        delete videoCodecContext;
    }
    if (formatContext) {
        avformat_close_input(&formatContext);
        formatContext = nullptr;
        delete formatContext;
    }
    if (swsContext) {
        sws_freeContext(swsContext);
        swsContext = nullptr;
    }
    if (swrContext) {
        swr_free(&swrContext);
        swrContext = nullptr;
    }
    format = nullptr;
    delete format;
}