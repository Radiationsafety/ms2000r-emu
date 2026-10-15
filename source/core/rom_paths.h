#pragma once
// ROM-PATHS-1 (2026-10-15): one place that knows where the user's ROM files may sit in the MS2000 folder.
// Users put the DSP boot ROM beside flash.bin as often as in the "full FW" subfolder (forum, v0.9.7), so both
// are accepted; the corrected copy (boot-362.ms2000.bin) is preferred wherever it is. "Full FW" is listed for
// case-sensitive file systems. Nothing here reads a ROM - it only finds them and says what is missing.
#include <filesystem>
#include <string>
#include <system_error>

namespace ms2k {

inline const char* const kBootRomNames[] = {
    "full FW/boot-362.ms2000.bin", "Full FW/boot-362.ms2000.bin", "boot-362.ms2000.bin",
    "full FW/boot-362.bin",        "Full FW/boot-362.bin",        "boot-362.bin",
};
inline const char* const kLcdRomNames[] = { "hd44780_a00.bin", "full FW/hd44780_a00.bin", "Full FW/hd44780_a00.bin" };

inline bool romExists(const std::filesystem::path& p) { std::error_code ec; return std::filesystem::is_regular_file(p, ec); }

// The boot ROM path relative to dir ("" if none); the first match in kBootRomNames order.
inline std::string findBootRom(const std::filesystem::path& dir = ".") {
    for (const char* n : kBootRomNames) if (romExists(dir / n)) return n;
    return {};
}
inline std::string findLcdRom(const std::filesystem::path& dir = ".") {
    for (const char* n : kLcdRomNames) if (romExists(dir / n)) return n;
    return {};
}
// A usable MS2000 folder: flash.bin and a DSP boot ROM.
inline bool hasRoms(const std::filesystem::path& dir) { return romExists(dir / "flash.bin") && !findBootRom(dir).empty(); }

// What is missing in dir, for status lines ("" = nothing).
inline std::string missingRoms(const std::filesystem::path& dir) {
    std::string m;
    if (!romExists(dir / "flash.bin")) m += "flash.bin";
    if (findBootRom(dir).empty()) m += std::string(m.empty() ? "" : " + ") + "boot-362.bin (beside flash.bin or in its 'full FW' subfolder)";
    return m;
}

} // namespace ms2k
