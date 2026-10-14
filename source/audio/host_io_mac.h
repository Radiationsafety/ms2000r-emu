#pragma once
// MAC-2: host audio out (CoreAudio AUHAL) and MIDI in/out (CoreMIDI) for macOS - the same public API as
// host_io_win.h. Nothing here models MS2000 hardware: it is the bridge between the emulated board's
// outputs/inputs and the Mac's devices.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

namespace MS2000 {

// id: the CoreAudio device UID (wide, as on Windows, so the callers are the same).
struct HostDevice { std::wstring id; std::string name; };

// AUHAL render. The emulator's audio is 48 kHz stereo float; the unit converts to the device's own format.
class HostAudioOut {
public:
    // Fill `frames` interleaved stereo float frames in [-1, 1]; return how many were real audio
    // (the rest must be zero-filled by the callee) - the shortfall is counted as underrun frames.
    using Pull = std::function<uint32_t(float* lr, uint32_t frames)>;

    static std::vector<HostDevice> list();            // output devices, default first
    bool open(const std::wstring& deviceId, Pull pull, uint32_t bufferMs = 20);
    void close();
    bool isOpen() const { return m_unit != nullptr; }
    std::string lastError() const { std::lock_guard<std::mutex> l(m_errMx); return m_err; }
    uint64_t underrunFrames() const { return m_underrun; }
    uint32_t deviceRate() const { return m_devRate; }
    bool dead() const { return m_dead.load(); }
    ~HostAudioOut() { close(); }
    // called from the CoreAudio IO thread
    void render(float* lr, uint32_t frames);
private:
    void fail(const std::string& e) { std::lock_guard<std::mutex> l(m_errMx); m_err = e; }
    Pull m_pull;
    void* m_unit = nullptr;                            // AudioComponentInstance
    std::atomic<bool> m_dead{false};
    std::atomic<uint64_t> m_underrun{0};
    std::atomic<uint32_t> m_devRate{0};
    mutable std::mutex m_errMx;
    std::string m_err;
};

// AUHAL capture (AUDIO-IN): 48 kHz stereo float frames, handed on as they arrive. MAC-2: not yet implemented on
// macOS - list() is empty and open() fails, so the inputs stay silent.
class HostAudioIn {
public:
    using Sink = std::function<void(const float* lr, uint32_t frames)>;
    static std::vector<HostDevice> list() { return {}; }
    bool open(const std::wstring&, Sink) { return false; }
    void close() {}
    bool isOpen() const { return false; }
    std::string lastError() const { return "audio input is not implemented on macOS yet"; }
    uint64_t frames() const { return 0; }
    bool dead() const { return false; }
};

// CoreMIDI IN. Short messages and SysEx are handed on as raw bytes, in order.
class HostMidiIn {
public:
    using Sink = std::function<void(const uint8_t* data, size_t n)>;
    static std::vector<std::string> list();
    bool open(unsigned index, Sink sink);
    void close();
    bool isOpen() const { return m_port != 0; }
    uint64_t bytesIn() const { return m_bytes; }
    ~HostMidiIn() { close(); }
    // called from the CoreMIDI thread
    void onBytes(const uint8_t* data, size_t n);
private:
    uint32_t m_port = 0, m_source = 0;                 // MIDIPortRef, MIDIEndpointRef
    Sink m_sink;
    std::atomic<uint64_t> m_bytes{0};
};

// CoreMIDI OUT: the MS2000's SCI1 byte stream (TXD1 = MIDI OUT) regrouped into complete messages - running
// status, one- and two-byte system messages, realtime bytes anywhere, SysEx F0..F7 as one message - as
// host_io_win does. byte() is called from the emulation thread.
class HostMidiOut {
public:
    static std::vector<std::string> list();
    bool open(unsigned index);
    void close();
    bool isOpen() const { return m_port != 0; }
    void byte(uint8_t b);
    uint64_t bytesOut() const { return m_bytes; }
    ~HostMidiOut() { close(); }
private:
    void send(const uint8_t* m, size_t n);
    std::mutex m_mx;
    uint32_t m_port = 0, m_dest = 0;                   // MIDIPortRef, MIDIEndpointRef
    std::atomic<uint64_t> m_bytes{0};
    uint8_t m_status = 0, m_msg[3] = {}; size_t m_have = 0, m_need = 0;
    bool m_inSysex = false;
    std::vector<uint8_t> m_sysex;
};

} // namespace MS2000
