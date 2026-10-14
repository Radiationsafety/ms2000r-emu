// MAC-2: --play - the machine on the host's audio device and MIDI ports, without a GUI. The same wiring as the
// thin GUI (thin_gui.cpp): the DSP's frames go through its host-audio ring to the device, the emulation is
// paced by the device's clock (50 ms ahead), MIDI IN goes to the emulated MIDI IN, the SCI1 bytes to MIDI OUT.
// The panel stands where the GUI's default leaves it: every pot at its centre, VOLUME 0.7.
#include "play_mode.h"
#include "../core/ms2000_runner.h"
#include "../core/h8s2350_emulator.h"
#include "../core/dsp56362_emulator.h"
#include "../core/master_vr.h"
#include "../core/lcd_gui.h"
#include "../audio/host_io.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>

namespace MS2000 {

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

// The first name containing `want` (case-insensitive); -1 if none or `want` is empty.
int findByName(const std::vector<std::string>& names, const std::string& want)
{
    if (want.empty()) return -1;
    const std::string w = lower(want);
    for (int i = 0; i < int(names.size()); ++i) if (lower(names[i]).find(w) != std::string::npos) return i;
    return -1;
}

} // namespace

void listHostDevices()
{
    const auto outs = HostAudioOut::list();
    std::printf("Audio outputs:\n");
    for (size_t i = 0; i < outs.size(); ++i) std::printf("  %zu: %s%s\n", i, outs[i].name.c_str(), i == 0 ? "  (default)" : "");
    const auto mi = HostMidiIn::list();
    std::printf("MIDI inputs:\n");
    if (mi.empty()) std::printf("  (none)\n");
    for (size_t i = 0; i < mi.size(); ++i) std::printf("  %zu: %s\n", i, mi[i].c_str());
    const auto mo = HostMidiOut::list();
    std::printf("MIDI outputs:\n");
    if (mo.empty()) std::printf("  (none)\n");
    for (size_t i = 0; i < mo.size(); ++i) std::printf("  %zu: %s\n", i, mo[i].c_str());
}

int runPlayMode(Ms2kRunner& R, const PlayOptions& o)
{
    DSP56362Emulator* dsp = R.getEmulator().dsp();
    if (!dsp) { std::fprintf(stderr, "ERROR: --play needs the DSP (its boot ROM was not found) - no sound without it\n"); return 2; }

    // The audio device, chosen before the machine starts so a bad name fails fast.
    const auto outs = HostAudioOut::list();
    std::vector<std::string> outNames;
    for (const auto& d : outs) outNames.push_back(d.name);
    int outSel = o.audioDevice.empty() ? (outs.empty() ? -1 : 0) : findByName(outNames, o.audioDevice);
    if (outSel < 0) { std::fprintf(stderr, "ERROR: no audio output%s%s\n", o.audioDevice.empty() ? "" : " matching ", o.audioDevice.c_str()); return 2; }

    // PANEL-IO / PWR-SW-1: the panel as it stands before the firmware first reads it.
    dsp->enableAudioRing(true);
    for (unsigned m = 0; m < 4; ++m) for (unsigned x = 0; x < 8; ++x) R.getEmulator().setPanelKnob(m, x, 512);

    HostMidiOut midiOut;
    if (!o.midiOut.empty()) {
        const int i = findByName(HostMidiOut::list(), o.midiOut);
        if (i < 0 || !midiOut.open(unsigned(i))) std::fprintf(stderr, "WARNING: MIDI OUT '%s' not opened\n", o.midiOut.c_str());
        else std::printf("[PLAY] MIDI OUT -> %s\n", HostMidiOut::list()[size_t(i)].c_str());
    }
    // Bytes leave as the firmware writes them - ~50 ms (the audio ring) before they are heard. The thin GUI
    // delays them to the frame they belong to (MIDI-OUT-TIMING); not done here yet.
    R.getEmulator().setMidiOutSink([&midiOut](uint8_t b) { midiOut.byte(b); });

    // MVR-1: the VOLUME pot's law; the DSP ring holds 24-bit samples.
    const float gain = ms2kMasterVrGain(o.volume) / 8388608.0f;
    std::atomic<bool> primed{false};
    { int32_t l, r; while (dsp->popAudio(l, r)) {} }
    HostAudioOut audio;
    const bool audioOk = audio.open(outs[size_t(outSel)].id, [dsp, gain, &primed](float* lr, uint32_t frames) -> uint32_t {
        uint32_t n = 0; int32_t l, r;
        while (n < frames && dsp->popAudio(l, r)) { lr[2 * n] = l * gain; lr[2 * n + 1] = r * gain; ++n; }
        if (n) primed = true;
        // Before the DSP's first frame (boot) an empty ring is not an underrun.
        if (!primed) { std::fill(lr, lr + 2 * frames, 0.0f); return frames; }
        return n;
    });
    if (!audioOk) {
        std::fprintf(stderr, "ERROR: audio output '%s' did not open: %s\n", outs[size_t(outSel)].name.c_str(), audio.lastError().c_str());
        R.getEmulator().setMidiOutSink(nullptr);
        return 2;
    }
    R.setAudioPaced(true, 2400);                                 // 50 ms of emulated audio ahead
    // The device opens before the machine starts (as in the thin GUI): until it runs, nothing paces the machine.
    R.start();
    std::printf("[PLAY] audio -> %s (device %u Hz), VOLUME %.2f\n", outs[size_t(outSel)].name.c_str(), audio.deviceRate(), o.volume);

    HostMidiIn midiIn;
    if (!o.midiIn.empty()) {
        const auto names = HostMidiIn::list();
        const int i = findByName(names, o.midiIn);
        if (i < 0 || !midiIn.open(unsigned(i), [&R](const uint8_t* d, size_t n) { R.sendMIDIData(d, n); }))
            std::fprintf(stderr, "WARNING: MIDI IN '%s' not opened\n", o.midiIn.c_str());
        else std::printf("[PLAY] MIDI IN <- %s\n", names[size_t(i)].c_str());
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::printf("[PLAY] running%s - Ctrl+C to stop\n", o.durationSec > 0 ? " for the given duration" : "");
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    std::string lastLcd;
    uint64_t lastUnderrun = 0; int tick = 0;
    while (!g_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (++tick % 20 == 0) {                                  // once a second: dropouts, as they happen
            const uint64_t u = audio.underrunFrames();
            if (u != lastUnderrun) {
                std::printf("[PLAY] t=%.0f s: %llu frames of dropout (%.1f ms) in the last second\n",
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                            (unsigned long long)(u - lastUnderrun), double(u - lastUnderrun) / 48.0);
                lastUnderrun = u;
            }
        }
        const auto snap = R.getLcdGuiSnapshot();
        std::string lcd = std::string(snap.line0) + "|" + std::string(snap.line1);
        if (lcd != lastLcd) {
            lastLcd = lcd;
            std::printf("[LCD] %-16.16s | %-16.16s\n", snap.line0, snap.line1);
            std::fflush(stdout);
        }
        if (audio.dead()) { std::fprintf(stderr, "ERROR: the audio device stopped\n"); break; }
        if (o.durationSec > 0 && std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(o.durationSec)) break;
    }

    midiIn.close();
    audio.close();
    R.setAudioPaced(false);
    R.getEmulator().setMidiOutSink(nullptr);
    midiOut.close();
    std::printf("[PLAY] stopped - audio underrun frames %llu, MIDI in %llu bytes, out %llu bytes\n",
                (unsigned long long)audio.underrunFrames(), (unsigned long long)midiIn.bytesIn(), (unsigned long long)midiOut.bytesOut());
    R.stop();
    return 0;
}

} // namespace MS2000
