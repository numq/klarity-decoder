#include "decoder.h"

Media *Decoder::_acquireMedia(uint64_t id) {
    auto it = mediaPool.find(id);
    if (it == mediaPool.end()) {
        return nullptr;
    }
    return it->second;
}

void Decoder::_releaseMedia(uint64_t id) {
    auto it = mediaPool.find(id);
    if (it != mediaPool.end()) {
        delete it->second;
        mediaPool.erase(it);
    }
}

Decoder::~Decoder() {
    std::lock_guard<std::mutex> lock(mutex);

    for (auto &media: mediaPool) {
        delete media.second;
    }

    mediaPool.clear();
}

bool Decoder::initialize(uint64_t id, const char *location, bool findAudioStream, bool findVideoStream) {
    std::lock_guard<std::mutex> lock(mutex);

    if (mediaPool.find(id) == mediaPool.end()) {
        auto media = new Media(location, findAudioStream, findVideoStream);

        mediaPool.emplace(id, media);

        return true;
    }

    return false;
}

Format *Decoder::getFormat(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    auto media = _acquireMedia(id);
    if (!media) return nullptr;

    return new Format(*media->format);
}

Frame *Decoder::nextFrame(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    auto media = _acquireMedia(id);
    if (!media) return nullptr;

    return media->nextFrame();
}

void Decoder::seekTo(uint64_t id, long timestampMicros) {
    std::lock_guard<std::mutex> lock(mutex);

    auto media = _acquireMedia(id);
    if (media) {
        media->seekTo(timestampMicros);
    }
}

void Decoder::reset(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    auto media = _acquireMedia(id);
    if (media) {
        media->reset();
    }
}

void Decoder::close(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    _releaseMedia(id);
}