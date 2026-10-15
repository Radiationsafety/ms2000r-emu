#pragma once
// MAC-2: --play (see play_mode.cpp). Built where a host audio/MIDI backend is (MS2K_HOST_IO).
#include <string>

namespace MS2000 {

class Ms2kRunner;

struct PlayOptions {
    std::string audioDevice;   // part of the output's name; empty = the system default
    std::string midiIn;        // part of a MIDI source's name; empty = none
    std::string midiOut;       // part of a MIDI destination's name; empty = none
    float volume = 0.7f;       // the VOLUME pot, 0..1 (the thin GUI's default)
    int durationSec = 0;       // 0 = until Ctrl+C
};

void listHostDevices();
int runPlayMode(Ms2kRunner& runner, const PlayOptions& options);

} // namespace MS2000
