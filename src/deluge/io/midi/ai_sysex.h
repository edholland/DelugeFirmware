#pragma once

#include <cstdint>

class MIDICable;

namespace AISysex {

void sysexReceived(MIDICable& cable, uint8_t* data, int32_t len);

} // namespace AISysex
