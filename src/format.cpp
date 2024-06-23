#include "format.h"

Format::Format(const char *location, uint64_t durationMicros) : location(location), durationMicros(durationMicros) {}

Format::~Format() = default;