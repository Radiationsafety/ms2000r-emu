#pragma once
// MAC-2: the host audio/MIDI bridge for this platform - one public API (HostAudioOut, HostAudioIn, HostMidiIn,
// HostMidiOut, HostDevice), one implementation per OS.
#if defined(_WIN32)
#include "host_io_win.h"
#elif defined(__APPLE__)
#include "host_io_mac.h"
#else
#error "no host audio/MIDI backend for this platform"
#endif
