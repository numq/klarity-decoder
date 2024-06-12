#include "frame.h"

Frame::Frame(Frame::FrameType type, int64_t timestampMicros, std::vector<uint8_t> bytes)
        : type(type), timestampMicros(timestampMicros), bytes(std::move(bytes)) {}

Frame::~Frame() = default;