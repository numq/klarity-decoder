#include "media.h"

std::vector<uint8_t> Media::_processVideoFrame(const AVFrame &src) {
    if (format->width <= 0 || format->height <= 0) {
        std::cerr << "Invalid format." << std::endl;
        return {};
    }

    auto dstWidth = static_cast<int>(format->width);
    auto dstHeight = static_cast<int>(format->height);
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
            std::cerr << "Could not initialize the conversion context." << std::endl;
            return {};
        }

        int bufferSize = av_image_get_buffer_size(dstFormat, dstWidth, dstHeight, 1);
        if (bufferSize < 0) {
            std::cerr << "Could not get buffer size, error: " << bufferSize << std::endl;
            return {};
        }

        std::vector<uint8_t> output(bufferSize + AV_INPUT_BUFFER_PADDING_SIZE);

        uint8_t *dst[3] = {nullptr};
        av_image_fill_pointers(dst, dstFormat, dstHeight, output.data(), dstLinesize);

        if (sws_scale(swsContext, src.data, src.linesize, 0, src.height, dst, dstLinesize) < 0) {
            std::cerr << "Error while converting the video frame." << std::endl;
            return {};
        }

        return output;
    }

    std::vector<uint8_t> output(src.linesize[0] * src.height);

    memcpy(output.data(), src.data[0], src.linesize[0] * src.height);

    return output;
}

std::vector<uint8_t> Media::_processAudioFrame(const AVFrame &src) {
    if (format->sampleRate <= 0 || format->channels <= 0) {
        std::cerr << "Invalid format." << std::endl;
        return {};
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
            std::cerr << "Could not initialize the resampling context." << std::endl;
            return {};
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
            std::cerr << "Error while converting the audio frame." << std::endl;
            return {};
        }

        data.resize(converted_samples * src.channels * sizeof(float));

        return {data.begin(), data.end()};
    }

    std::vector<uint8_t> output(src.nb_samples * src.channels * sizeof(float));

    memcpy(output.data(), src.data[0], src.nb_samples * src.channels * sizeof(float));

    return output;
}

Media::Media(const char *location, bool findAudioStream, bool findVideoStream) {
    std::lock_guard<std::mutex> lock(mutex);

    formatContext = avformat_alloc_context();

    if (!formatContext) {
        std::cerr << "Could not allocate format context." << std::endl;
        return;
    }

    if (avformat_open_input(&formatContext, location, nullptr, nullptr) < 0) {
        std::cerr << "Couldn't open input stream." << std::endl;
        avformat_free_context(formatContext);
        formatContext = nullptr;
        return;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        std::cerr << "Couldn't find stream information." << std::endl;
        avformat_close_input(&formatContext);
        return;
    }

    format = new Format(location, static_cast<uint64_t>(static_cast<double>(formatContext->duration) /
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

    if ((findAudioStream && !audioStream) && (findVideoStream && !videoStream)) {
        std::cerr << "No valid streams found." << std::endl;
        avformat_free_context(formatContext);
        formatContext = nullptr;
        return;
    }
}

Media::~Media() {
    std::lock_guard<std::mutex> lock(mutex);

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

Frame *Media::nextFrame() {
    std::lock_guard<std::mutex> lock(mutex);

    if (!format || !format->durationMicros) {
        return nullptr;
    }

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

void Media::seekTo(long timestampMicros) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!format || !format->durationMicros) {
        return;
    }

    if (0 < timestampMicros <= format->durationMicros) {
        if (av_seek_frame(formatContext, -1, timestampMicros, AVSEEK_FLAG_BACKWARD) < 0) {
            std::cerr << "Error seeking to timestamp: " << timestampMicros << std::endl;
        }

        if (videoCodecContext) {
            avcodec_flush_buffers(videoCodecContext);
        }

        if (audioCodecContext) {
            avcodec_flush_buffers(audioCodecContext);
        }
    }
}

void Media::reset() {
    std::lock_guard<std::mutex> lock(mutex);

    if (!format || !format->durationMicros) {
        return;
    }

    if (av_seek_frame(formatContext, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        std::cerr << "Error resetting stream." << std::endl;
    }

    if (videoCodecContext) {
        avcodec_flush_buffers(videoCodecContext);
    }

    if (audioCodecContext) {
        avcodec_flush_buffers(audioCodecContext);
    }
}