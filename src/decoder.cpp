#include "decoder.h"

Media *Decoder::_acquireMedia(int64_t id) {
    auto it = mediaPool.find(id);
    if (it == mediaPool.end()) {
        throw MediaNotFoundException();
    }
    return it->second;
}

void Decoder::_releaseMedia(int64_t id) {
    auto it = mediaPool.find(id);
    if (it == mediaPool.end()) {
        throw MediaNotFoundException();
    }

    delete it->second;
    mediaPool.erase(it);
}

Decoder::~Decoder() {
    std::lock_guard<std::mutex> lock(mutex);

    for (auto &media: mediaPool) {
        _releaseMedia(media.first);
    }
}

void Decoder::initialize(int64_t id, const char *location, bool findAudioStream, bool findVideoStream) {
    std::lock_guard<std::mutex> lock(mutex);

    if (mediaPool.find(id) != mediaPool.end()) {
        throw MediaException("Media with the same id already exists");
    }

    auto media = new Media(location, findAudioStream, findVideoStream);
    mediaPool.emplace(id, media);
}

Format *Decoder::getFormat(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    return _acquireMedia(id)->format;
}

Frame *Decoder::nextFrame(int64_t id, int64_t width, int64_t height) {
    std::lock_guard<std::mutex> lock(mutex);

    return _acquireMedia(id)->nextFrame(width, height);
}

void Decoder::seekTo(int64_t id, long timestampMicros, bool keyframesOnly) {
    std::lock_guard<std::mutex> lock(mutex);

    _acquireMedia(id)->seekTo(timestampMicros, keyframesOnly);
}

void Decoder::reset(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    _acquireMedia(id)->reset();
}

void Decoder::close(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    _releaseMedia(id);
}