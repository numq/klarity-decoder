#ifndef KLARITY_DECODER_MEDIA_H
#define KLARITY_DECODER_MEDIA_H

#include "frame.h"
#include "format.h"
#include <string>
#include <mutex>
#include <iostream>

extern "C" {
#include "libavutil/imgutils.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

struct Media {
    std::mutex mutex;
    Format *format;
    AVFormatContext *formatContext;
    AVCodecContext *audioCodecContext = nullptr;
    AVCodecContext *videoCodecContext = nullptr;
    AVStream *audioStream = nullptr;
    AVStream *videoStream = nullptr;
    SwsContext *swsContext = nullptr;
    SwrContext *swrContext = nullptr;

private:
    std::vector<uint8_t> _processVideoFrame(const AVFrame &src);

    std::vector<uint8_t> _processAudioFrame(const AVFrame &src);

public:
    explicit Media(const char *location, bool findAudioStream, bool findVideoStream);

    ~Media();

    Frame *nextFrame();

    void seekTo(long timestampMicros);

    void reset();
};

#endif //KLARITY_DECODER_MEDIA_H
