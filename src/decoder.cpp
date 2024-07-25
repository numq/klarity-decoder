#include "decoder.h"

Media *Decoder::_acquireMedia(int64_t id) {
    auto it = mediaPool.find(id);
    if (it == mediaPool.end()) {
        return nullptr;
    }
    return it->second;
}

void Decoder::_releaseMedia(int64_t id) {
    auto it = mediaPool.find(id);
    if (it == mediaPool.end()) {
        return;
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

bool Decoder::initialize(int64_t id, const char *location, bool findAudioStream, bool findVideoStream) {
    std::lock_guard<std::mutex> lock(mutex);

    if (mediaPool.find(id) != mediaPool.end()) {
        std::cerr << "Media with the same id already exists." << std::endl;
        return false;
    }

    auto media = new Media(location, findAudioStream, findVideoStream);
    mediaPool.emplace(id, media);
    return true;
}

Format *Decoder::getFormat(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    auto media = _acquireMedia(id);
    if (!media) {
        std::cerr << "Unable to find media." << std::endl;
        return nullptr;
    }

    return new Format(*media->format);
}

Frame *Decoder::nextFrame(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    auto media = _acquireMedia(id);
    if (!media) {
        std::cerr << "Unable to find media." << std::endl;
        return nullptr;
    }

    return media->nextFrame();
}

void Decoder::seekTo(int64_t id, long timestampMicros) {
    std::lock_guard<std::mutex> lock(mutex);

    auto media = _acquireMedia(id);
    if (!media) {
        std::cerr << "Unable to find media." << std::endl;
        return;
    }

    media->seekTo(timestampMicros);
}

void Decoder::reset(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    auto media = _acquireMedia(id);
    if (!media) {
        std::cerr << "Unable to find media." << std::endl;
        return;
    }

    media->reset();
}

void Decoder::close(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex);

    _releaseMedia(id);
}