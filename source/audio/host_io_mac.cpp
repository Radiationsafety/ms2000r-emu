// MAC-2: host_io_mac.h - CoreAudio (AUHAL) output and CoreMIDI in/out.
#ifdef __APPLE__
#include "host_io_mac.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreMIDI/CoreMIDI.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

// The byte-stream MIDI API (MIDIPacketList, MIDIInputPortCreate) is deprecated since macOS 11 in favour of
// MIDI 2.0 UMP packets; it still works, and the emulator speaks MIDI 1.0 bytes on both sides.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace MS2000 {

namespace {

std::string cfToUtf8(CFStringRef s)
{
    if (!s) return {};
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
    return {};
}
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // UIDs are ASCII
std::string narrow(const std::wstring& s) { std::string o; for (wchar_t c : s) o.push_back(char(c)); return o; }

std::string osErr(const char* what, OSStatus st)
{
    char b[96]; std::snprintf(b, sizeof b, "%s failed (OSStatus %d)", what, int(st)); return b;
}

template<typename T> bool getProp(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, T& out)
{
    AudioObjectPropertyAddress a{ sel, scope, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof(T);
    return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &sz, &out) == noErr;
}

std::string deviceString(AudioObjectID dev, AudioObjectPropertySelector sel)
{
    CFStringRef s = nullptr;
    if (!getProp(dev, sel, kAudioObjectPropertyScopeGlobal, s) || !s) return {};
    std::string r = cfToUtf8(s);
    CFRelease(s);
    return r;
}

UInt32 channelCount(AudioObjectID dev, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a{ kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr || !sz) return 0;
    std::vector<uint8_t> buf(sz);
    auto* bl = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, bl) != noErr) return 0;
    UInt32 ch = 0;
    for (UInt32 i = 0; i < bl->mNumberBuffers; ++i) ch += bl->mBuffers[i].mNumberChannels;
    return ch;
}
bool hasOutput(AudioObjectID dev) { return channelCount(dev, kAudioDevicePropertyScopeOutput) > 0; }

std::vector<AudioObjectID> allDevices()
{
    AudioObjectPropertyAddress a{ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr) return {};
    std::vector<AudioObjectID> devs(sz / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, devs.data()) != noErr) return {};
    return devs;
}

AudioObjectID deviceFromUid(const std::string& uid)
{
    AudioObjectID dev = kAudioObjectUnknown;
    CFStringRef s = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    AudioObjectPropertyAddress a{ kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof dev;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof s, &s, &sz, &dev);
    CFRelease(s);
    return dev;
}

OSStatus renderProc(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io)
{
    auto* self = static_cast<HostAudioOut*>(ref);
    self->render(static_cast<float*>(io->mBuffers[0].mData), frames);
    return noErr;
}

MIDIClientRef midiClient()
{
    static MIDIClientRef c = [] {
        MIDIClientRef r = 0;
        MIDIClientCreate(CFSTR("MS2000R"), nullptr, nullptr, &r);
        return r;
    }();
    return c;
}

std::string endpointName(MIDIEndpointRef e)
{
    CFStringRef s = nullptr;
    if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) != noErr || !s) return "?";
    std::string r = cfToUtf8(s);
    CFRelease(s);
    return r;
}

} // namespace

// ---------------------------------------------------------------- AUDIO OUT
std::vector<HostDevice> HostAudioOut::list()
{
    std::vector<HostDevice> out;
    const std::vector<AudioObjectID> devs = allDevices();
    AudioObjectID def = kAudioObjectUnknown;
    getProp(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, def);
    for (AudioObjectID d : devs) {
        if (!hasOutput(d)) continue;
        HostDevice h{ widen(deviceString(d, kAudioDevicePropertyDeviceUID)), deviceString(d, kAudioObjectPropertyName) };
        if (d == def) out.insert(out.begin(), h); else out.push_back(h);
    }
    return out;
}

bool HostAudioOut::open(const std::wstring& deviceId, Pull pull, uint32_t bufferMs)
{
    close();
    m_pull = std::move(pull);
    m_dead = false; m_underrun = 0;

    AudioObjectID dev = kAudioObjectUnknown;
    if (!deviceId.empty()) dev = deviceFromUid(narrow(deviceId));
    if (dev == kAudioObjectUnknown)
        getProp(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, dev);
    if (dev == kAudioObjectUnknown) { fail("no output device"); return false; }

    AudioComponentDescription desc{ kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    AudioComponentInstance unit = nullptr;
    OSStatus st = comp ? AudioComponentInstanceNew(comp, &unit) : -1;
    if (st != noErr) { fail(osErr("AudioComponentInstanceNew", st)); return false; }

    auto bail = [&](const char* what) { fail(osErr(what, st)); AudioComponentInstanceDispose(unit); return false; };
    if ((st = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev)) != noErr)
        return bail("set output device");

    // The emulator's side: 48 kHz, stereo, interleaved float. AUHAL converts to whatever the device runs at.
    AudioStreamBasicDescription f{};
    f.mSampleRate = 48000.0;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = f.mBytesPerPacket = 8;
    if ((st = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f)) != noErr)
        return bail("set stream format");

    // The device's IO buffer: half the asked-for latency per callback (WASAPI's bufferMs is the whole buffer).
    UInt32 ioFrames = UInt32(bufferMs * 48 / 2);
    AudioUnitSetProperty(unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &ioFrames, sizeof ioFrames);

    AURenderCallbackStruct cb{ renderProc, this };
    if ((st = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb)) != noErr)
        return bail("set render callback");
    if ((st = AudioUnitInitialize(unit)) != noErr) return bail("AudioUnitInitialize");

    Float64 rate = 0;
    getProp(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate);
    m_devRate = uint32_t(rate);

    m_unit = unit;
    if ((st = AudioOutputUnitStart(unit)) != noErr) {
        fail(osErr("AudioOutputUnitStart", st));
        AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); m_unit = nullptr;
        return false;
    }
    return true;
}

void HostAudioOut::render(float* lr, uint32_t frames)
{
    const uint32_t n = m_pull ? m_pull(lr, frames) : 0;
    if (n < frames) {
        std::memset(lr + 2 * n, 0, size_t(frames - n) * 2 * sizeof(float));
        m_underrun += frames - n;
    }
}

void HostAudioOut::close()
{
    if (!m_unit) return;
    auto unit = static_cast<AudioComponentInstance>(m_unit);
    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    m_unit = nullptr;
}

// ---------------------------------------------------------------- AUDIO IN
// id = "<device UID>|<first input channel, 0-based>|<1 = mono, 2 = pair>"
std::vector<HostDevice> HostAudioIn::list()
{
    std::vector<HostDevice> out;
    AudioObjectID def = kAudioObjectUnknown;
    getProp(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, def);
    for (AudioObjectID d : allDevices()) {
        const UInt32 ch = channelCount(d, kAudioDevicePropertyScopeInput);
        if (!ch) continue;
        const std::string uid = deviceString(d, kAudioDevicePropertyDeviceUID), name = deviceString(d, kAudioObjectPropertyName);
        std::vector<HostDevice> mine;
        if (ch <= 2) mine.push_back({ widen(uid + "|0|" + std::to_string(ch)), name });
        else
            for (UInt32 c = 0; c < ch; c += 2) {
                const bool pair = c + 1 < ch;
                char lab[48];
                if (pair) std::snprintf(lab, sizeof lab, " - IN %u-%u", c + 1, c + 2); else std::snprintf(lab, sizeof lab, " - IN %u", c + 1);
                mine.push_back({ widen(uid + "|" + std::to_string(c) + "|" + (pair ? "2" : "1")), name + lab });
            }
        out.insert(d == def ? out.begin() : out.end(), mine.begin(), mine.end());
    }
    return out;
}

static OSStatus inputProc(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32, UInt32 frames, AudioBufferList*)
{
    static_cast<HostAudioIn*>(ref)->input(flags, ts, frames);
    return noErr;
}

static OSStatus converterInput(AudioConverterRef, UInt32* packets, AudioBufferList* io, AudioStreamPacketDescription**, void* ref)
{
    float* data = nullptr; uint32_t n = 0;
    if (!static_cast<HostAudioIn*>(ref)->pullDevice(data, n)) { *packets = 0; return 1; }   // 1: no more for now
    io->mBuffers[0].mData = data;
    io->mBuffers[0].mDataByteSize = n * 8;
    io->mBuffers[0].mNumberChannels = 2;
    *packets = n;
    return noErr;
}

bool HostAudioIn::open(const std::wstring& deviceId, Sink sink)
{
    close();
    m_sink = std::move(sink);
    m_dead = false; m_frames = 0;
    const std::string id = narrow(deviceId);
    const auto b1 = id.find('|'), b2 = id.find('|', b1 + 1);
    if (b1 == std::string::npos || b2 == std::string::npos) { fail("bad input id"); return false; }
    const AudioObjectID dev = deviceFromUid(id.substr(0, b1));
    const SInt32 first = SInt32(std::atoi(id.c_str() + b1 + 1));
    const bool pair = std::atoi(id.c_str() + b2 + 1) == 2;
    if (dev == kAudioObjectUnknown) { fail("input device not found"); return false; }

    AudioComponentDescription desc{ kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    AudioComponentInstance unit = nullptr;
    OSStatus st = comp ? AudioComponentInstanceNew(comp, &unit) : -1;
    if (st != noErr) { fail(osErr("AudioComponentInstanceNew", st)); return false; }
    auto bail = [&](const char* what) { fail(osErr(what, st)); AudioComponentInstanceDispose(unit); return false; };

    UInt32 on = 1, off = 0;
    if ((st = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof on)) != noErr) return bail("enable input");
    if ((st = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof off)) != noErr) return bail("disable output");
    AudioObjectID d = dev;
    if ((st = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &d, sizeof d)) != noErr) return bail("set input device");

    // Our side of the input element: the device's own rate (AUHAL does not resample input), stereo float.
    Float64 rate = 48000.0;
    getProp(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate);
    m_devRate = rate > 0 ? rate : 48000.0;
    AudioStreamBasicDescription f{};
    f.mSampleRate = m_devRate; f.mFormatID = kAudioFormatLinearPCM; f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32; f.mFramesPerPacket = 1; f.mBytesPerFrame = f.mBytesPerPacket = 8;
    if ((st = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f)) != noErr) return bail("set input format");
    // which device inputs feed our left and right: the pair, or the one input on both sides
    const SInt32 map[2] = { first, pair ? first + 1 : first };
    if ((st = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 1, map, sizeof map)) != noErr) return bail("set channel map");

    AURenderCallbackStruct cb{ inputProc, this };
    if ((st = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb)) != noErr) return bail("set input callback");
    UInt32 maxFrames = 4096;
    AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
    m_dev.assign(size_t(maxFrames) * 2, 0.0f);
    m_out.assign(size_t(double(maxFrames) * 48000.0 / m_devRate + 64) * 2, 0.0f);

    if (m_devRate != 48000.0) {
        AudioStreamBasicDescription t = f; t.mSampleRate = 48000.0;
        AudioConverterRef conv = nullptr;
        if ((st = AudioConverterNew(&f, &t, &conv)) != noErr) return bail("AudioConverterNew");
        m_conv = conv;
    }
    if ((st = AudioUnitInitialize(unit)) != noErr) { if (m_conv) { AudioConverterDispose((AudioConverterRef)m_conv); m_conv = nullptr; } return bail("AudioUnitInitialize"); }
    m_unit = unit;
    if ((st = AudioOutputUnitStart(unit)) != noErr) { fail(osErr("AudioOutputUnitStart", st)); close(); return false; }
    return true;
}

bool HostAudioIn::pullDevice(float*& data, uint32_t& frames)
{
    if (m_devTaken) return false;
    m_devTaken = true;
    data = m_dev.data(); frames = m_devFrames;
    return frames > 0;
}

void HostAudioIn::input(void* flags, const void* timeStamp, uint32_t frames)
{
    if (!m_unit || frames * 2 > m_dev.size()) return;
    AudioBufferList bl{};
    bl.mNumberBuffers = 1;
    bl.mBuffers[0] = { 2, frames * 8, m_dev.data() };
    if (AudioUnitRender(static_cast<AudioComponentInstance>(m_unit), static_cast<AudioUnitRenderActionFlags*>(flags),
                        static_cast<const AudioTimeStamp*>(timeStamp), 1, frames, &bl) != noErr) return;
    if (!m_conv) {
        m_frames += frames;
        if (m_sink) m_sink(m_dev.data(), frames);
        return;
    }
    m_devFrames = frames; m_devTaken = false;
    UInt32 outFrames = UInt32(m_out.size() / 2);
    AudioBufferList ob{};
    ob.mNumberBuffers = 1;
    ob.mBuffers[0] = { 2, outFrames * 8, m_out.data() };
    AudioConverterFillComplexBuffer(static_cast<AudioConverterRef>(m_conv), converterInput, this, &outFrames, &ob, nullptr);
    if (outFrames) { m_frames += outFrames; if (m_sink) m_sink(m_out.data(), outFrames); }
}

void HostAudioIn::close()
{
    if (m_unit) {
        auto unit = static_cast<AudioComponentInstance>(m_unit);
        AudioOutputUnitStop(unit);
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        m_unit = nullptr;
    }
    if (m_conv) { AudioConverterDispose(static_cast<AudioConverterRef>(m_conv)); m_conv = nullptr; }
}

// ---------------------------------------------------------------- MIDI IN
static void midiReadProc(const MIDIPacketList* pl, void* ref, void*)
{
    auto* self = static_cast<HostMidiIn*>(ref);
    const MIDIPacket* p = &pl->packet[0];
    for (UInt32 i = 0; i < pl->numPackets; ++i) {
        self->onBytes(p->data, p->length);
        p = MIDIPacketNext(p);
    }
}

std::vector<std::string> HostMidiIn::list()
{
    std::vector<std::string> out;
    const ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; ++i) out.push_back(endpointName(MIDIGetSource(i)));
    return out;
}

bool HostMidiIn::open(unsigned index, Sink sink)
{
    close();
    if (index >= MIDIGetNumberOfSources() || !midiClient()) return false;
    m_sink = std::move(sink);
    MIDIPortRef port = 0;
    if (MIDIInputPortCreate(midiClient(), CFSTR("MS2000R MIDI IN"), midiReadProc, this, &port) != noErr) return false;
    const MIDIEndpointRef src = MIDIGetSource(index);
    if (MIDIPortConnectSource(port, src, nullptr) != noErr) { MIDIPortDispose(port); return false; }
    m_port = port; m_source = src;
    return true;
}

void HostMidiIn::onBytes(const uint8_t* data, size_t n)
{
    m_bytes += n;
    if (m_sink) m_sink(data, n);
}

void HostMidiIn::close()
{
    if (!m_port) return;
    MIDIPortDisconnectSource(m_port, m_source);
    MIDIPortDispose(m_port);   // no read callback runs after this returns
    m_port = 0; m_source = 0;
}

// ---------------------------------------------------------------- MIDI OUT
std::vector<std::string> HostMidiOut::list()
{
    std::vector<std::string> out;
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) out.push_back(endpointName(MIDIGetDestination(i)));
    return out;
}

bool HostMidiOut::open(unsigned index)
{
    close();
    if (index >= MIDIGetNumberOfDestinations() || !midiClient()) return false;
    MIDIPortRef port = 0;
    if (MIDIOutputPortCreate(midiClient(), CFSTR("MS2000R MIDI OUT"), &port) != noErr) return false;
    std::lock_guard<std::mutex> l(m_mx);
    m_port = port; m_dest = MIDIGetDestination(index);
    m_status = 0; m_have = 0; m_inSysex = false; m_sysex.clear();
    return true;
}

void HostMidiOut::close()
{
    std::lock_guard<std::mutex> l(m_mx);
    if (!m_port) return;
    MIDIPortDispose(m_port);
    m_port = 0; m_dest = 0;
}

void HostMidiOut::send(const uint8_t* m, size_t n)
{
    std::vector<uint8_t> buf(n + 64);
    auto* pl = reinterpret_cast<MIDIPacketList*>(buf.data());
    MIDIPacket* p = MIDIPacketListInit(pl);
    p = MIDIPacketListAdd(pl, buf.size(), p, 0, n, m);   // timestamp 0 = now
    if (p) MIDISend(m_port, m_dest, pl);
}

// The same regrouping as HostMidiOut::byte in host_io_win.cpp.
void HostMidiOut::byte(uint8_t b)
{
    std::lock_guard<std::mutex> l(m_mx);
    if (!m_port) return;
    ++m_bytes;
    if (b >= 0xF8) { send(&b, 1); return; }                      // realtime: anywhere, even inside SysEx
    if (b == 0xF0) { m_inSysex = true; m_sysex.assign(1, b); m_status = 0; return; }
    if (m_inSysex) {
        if (b == 0xF7) { m_sysex.push_back(b); send(m_sysex.data(), m_sysex.size()); m_inSysex = false; m_sysex.clear(); return; }
        if (b & 0x80) { m_inSysex = false; m_sysex.clear(); }  // any other status ends a broken SysEx
        else { m_sysex.push_back(b); return; }
    }
    if (b & 0x80) {
        m_status = 0; m_have = 0;
        if (b < 0xF0) { m_status = b; m_need = ((b & 0xE0) == 0xC0) ? 1 : 2; m_msg[0] = b; return; }
        if (b == 0xF1 || b == 0xF3) { m_msg[0] = b; m_need = 1; m_status = b; return; }
        if (b == 0xF2) { m_msg[0] = b; m_need = 2; m_status = b; return; }
        if (b == 0xF6) send(&b, 1);                              // tune request; F4/F5/F7 alone: dropped
        return;
    }
    if (!m_status) return;                                       // data with no status: dropped
    m_msg[1 + m_have++] = b;
    if (m_have == m_need) {
        send(m_msg, 1 + m_need);
        m_have = 0;
        if (m_status >= 0xF0) m_status = 0;                      // system common has no running status
    }
}

} // namespace MS2000
#endif
