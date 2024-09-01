#include "format.h"

#include <utility>

Format::Format(std::string location, uint64_t durationMicros) : location(std::move(location)),
                                                                durationMicros(durationMicros) {}

Format::~Format() = default;