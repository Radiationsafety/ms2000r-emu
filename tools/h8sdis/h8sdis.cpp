// RE-3: offline H8S/2350 analysis of a flash dump - linear disassembly, the vector table, and the
// cross-check of the machine's MS2K_OPCOV site list against the decoder. Everything through the
// SAME decoder the emulator executes with (decode<StaticByteSource>), so what this tool says a
// site decodes to is what the machine's decode-cache would have made of it.
//
// The bytes belong to whoever dumped the hardware. Output is written to --out (default dumps/h8s,
// gitignored) and this tool never prints listing content to stdout. Usage:
//   h8sdis <flash.bin> [--out DIR] [--sweep START:END] [--opcov FILE ...] [--vectors]
// With no --sweep and no --opcov only the vector table is written.
#include "core/h8s2350_instructions.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

extern bool g_h8s_quiet_boot;      // the decoder's debug-print gate (defined in h8s2350_instructions.cpp)

using MS2000::H8S2350Instruction;
using MS2000::H8S2350InstructionDecoder;
using MS2000::StaticByteSource;

namespace
{
bool decodeSizeSane(uint8_t size) { return size == 2 || size == 4 || size == 6 || size == 8 || size == 10; }

// The machine's own rule for the mnemonic column: undecoded sites carry "" or a mnemonic
// containing UNKNOWN; everything else is a name the executor actually has.
bool decodedAsUsable(const H8S2350Instruction& insn)
{
    return !insn.mnemonic.empty() && insn.mnemonic.find("UNKNOWN") == std::string::npos;
}

struct Site { uint32_t pc; uint8_t size; uint8_t bytes[10]; };

bool parseSites(const std::string& path, std::vector<Site>& out)
{
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "h8sdis: cannot open OPCOV file %s\n", path.c_str()); return false; }
    char line[256];
    while (in.getline(line, sizeof line))
    {
        // "<pc in hex> <size in decimal> [byte in hex]..."
        Site s{};
        char* p = line;
        s.pc = uint32_t(std::strtoul(p, &p, 16));
        s.size = uint8_t(std::strtoul(p, &p, 10));
        if (s.size == 0 || s.size > 10) continue;
        for (uint32_t i = 0; i < s.size; ++i)
        {
            unsigned long v = std::strtoul(p, &p, 16);
            if (p == line) break;                       // strtoul saw no digits (end or junk)
            s.bytes[i] = uint8_t(v);
        }
        out.push_back(s);
    }
    return true;
}
}

int main(int argc, char** argv)
{
    g_h8s_quiet_boot = true;                    // a static sweep must not print per-site to the console

    if (argc < 2)
    {
        std::fprintf(stderr, "usage: h8sdis <flash.bin> [--out DIR] [--sweep START:END] "
                             "[--opcov FILE]... [--vectors]\n");
        return 2;
    }
    const char* romPath = argv[1];
    std::string outDir = "dumps/h8s";
    bool haveSweep = false; uint32_t sweepBegin = 0, sweepEnd = 0;
    std::vector<std::string> opcovs;
    bool wantVectors = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--opcov" && i + 1 < argc) opcovs.push_back(argv[++i]);
        else if (a == "--vectors") wantVectors = true;
        else if (a == "--sweep" && i + 1 < argc)
        {
            if (std::sscanf(argv[++i], "%X:%X", &sweepBegin, &sweepEnd) != 2 || sweepEnd <= sweepBegin)
            { std::fprintf(stderr, "h8sdis: bad --sweep %s\n", argv[i]); return 2; }
            haveSweep = true;
        }
        else { std::fprintf(stderr, "h8sdis: unknown argument %s\n", a.c_str()); return 2; }
    }

    std::ifstream rom(romPath, std::ios::binary);
    if (!rom) { std::fprintf(stderr, "h8sdis: cannot read %s\n", romPath); return 1; }
    std::vector<uint8_t> flash((std::istreambuf_iterator<char>(rom)), std::istreambuf_iterator<char>());
    if (flash.size() < 0x100) { std::fprintf(stderr, "h8sdis: %s too small for a flash dump\n", romPath); return 1; }
    StaticByteSource bus{ flash.data(), uint32_t(flash.size()) };
    H8S2350InstructionDecoder dec;

    std::printf("h8sdis: %zu ROM bytes, out dir %s\n", flash.size(), outDir.c_str());

    if (wantVectors || !haveSweep)
    {
        // The machine's rule (irqTryService): handler = readLong(VBR + vec*4) & 0xFFFFFF, VBR = 0
        // at reset. 0x400 = the vector area in the memory map; 0x810 is the IPL entry documented
        // in docs/ipl_protocol.md.
        char path[1024]; std::snprintf(path, sizeof path, "%s/vectors.txt", outDir.c_str());
        std::FILE* f = std::fopen(path, "wb");
        if (!f) { std::fprintf(stderr, "h8sdis: cannot write %s\n", path); return 1; }
        std::fprintf(f, "# H8S/2350 vector table, VBR=000000, longwords at vec*4, 24-bit masked\n");
        for (uint32_t vec = 0; vec <= 0xFF; ++vec)
        {
            const uint32_t a = vec * 4;
            if (a + 4 > flash.size()) break;
            const uint32_t h = (uint32_t(flash[a]) << 24 | uint32_t(flash[a + 1]) << 16 |
                                uint32_t(flash[a + 2]) << 8 | flash[a + 3]) & 0xFFFFFF;
            std::fprintf(f, "vec %02X @%06X -> %06X\n", vec, a, h);
        }
        std::fprintf(f, "# IPL entry long at 000810: %06X\n",
                     (uint32_t(flash[0x810]) << 16 | uint32_t(flash[0x811]) << 8 | flash[0x812]) & 0xFFFFFF);
        std::fclose(f);
        std::printf("h8sdis: vectors.txt written\n");
    }

    if (haveSweep)
    {
        char path[1024]; std::snprintf(path, sizeof path, "%s/dis_%06X-%06X.s", outDir.c_str(), sweepBegin, sweepEnd);
        std::FILE* f = std::fopen(path, "wb");
        if (!f) { std::fprintf(stderr, "h8sdis: cannot write %s\n", path); return 1; }
        uint64_t sites = 0, undecoded = 0;
        for (uint32_t pc = sweepBegin; pc + 2 <= sweepEnd && pc < flash.size(); )
        {
            const H8S2350Instruction insn = dec.decode(bus, pc);
            uint8_t size = decodeSizeSane(insn.size) ? insn.size : 1;
            std::fprintf(f, "%06X:", pc);
            for (uint8_t b = 0; b < size; ++b)
                std::fprintf(f, " %02X", pc + b < flash.size() ? flash[pc + b] : 0xFF);
            if (decodedAsUsable(insn)) std::fprintf(f, "   %s", insn.mnemonic.c_str());
            else { std::fprintf(f, "   .byte"); ++undecoded; }
            std::fprintf(f, "   ; op=%02X am=%02X s=%02X d=%02X imm=%08X disp=%04X\n",
                         insn.opcode, insn.addressing_mode, insn.source_operand, insn.destination_operand,
                         insn.immediate_value, insn.displacement);
            pc += size; ++sites;
        }
        std::fclose(f);
        std::printf("h8sdis: sweep %" PRIu64 " sites, %" PRIu64 " undecoded -> %s\n", sites, undecoded, path);
    }

    for (const std::string& oc : opcovs)
    {
        std::vector<Site> sites;
        if (!parseSites(oc, sites)) return 1;
        char path[1024]; std::snprintf(path, sizeof path, "%s/coverage.txt", outDir.c_str());
        std::FILE* f = std::fopen(path, "wb");
        if (!f) { std::fprintf(stderr, "h8sdis: cannot write %s\n", path); return 1; }
        std::fprintf(f, "# decoder coverage of executed sites from %s\n", oc.c_str());
        uint64_t inRom = 0, outRom = 0, usable = 0;
        std::map<uint8_t, std::pair<uint32_t, uint32_t>> byOpcode;   // b0 -> {sites, undecoded}
        for (const Site& s : sites)
        {
            H8S2350Instruction insn;
            bool fromRom = false;
            if (s.pc < flash.size())
            {
                // cross-check the recorded bytes against the ROM before trusting the ROM decode
                bool same = s.pc + s.size <= flash.size();
                for (uint32_t i = 0; same && i < s.size; ++i) same = flash[s.pc + i] == s.bytes[i];
                if (same) { insn = dec.decode(bus, s.pc); fromRom = true; ++inRom; }
            }
            if (!fromRom)
            {
                // decode the RECORDED encoding: a window at pc=0, reads past the site see 0xFF,
                // as an unmapped bus read would
                uint8_t win[16]; for (uint8_t& x : win) x = 0xFF;
                for (uint32_t i = 0; i < s.size && i < 16; ++i) win[i] = s.bytes[i];
                StaticByteSource sb{ win, s.size };
                insn = dec.decode(sb, 0);
                ++outRom;
            }
            const bool ok = decodedAsUsable(insn);
            if (ok) ++usable;
            auto& e = byOpcode[insn.opcode]; ++e.first; if (!ok) ++e.second;
            if (!ok) std::fprintf(f, "UND %06X %u", s.pc, unsigned(s.size));
            for (uint32_t i = 0; i < s.size; ++i) std::fprintf(f, " %02X", s.bytes[i]);
            std::fprintf(f, "\n");
        }
        std::fprintf(f, "# totals: %zu sites, in-rom %" PRIu64 ", other %" PRIu64
                     ", decoded %" PRIu64 " (%.1f%%)\n",
                     sites.size(), inRom, outRom, usable, sites.empty() ? 0.0 : 100.0 * double(usable) / double(sites.size()));
        std::fprintf(f, "# per opcode: opcode site-count undecoded-count\n");
        std::vector<std::pair<uint8_t, std::pair<uint32_t, uint32_t>>> sorted(byOpcode.begin(), byOpcode.end());
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
        for (auto& kv : sorted)
            std::fprintf(f, "OP %02X %u %u\n", kv.first, kv.second.first, kv.second.second);
        std::fclose(f);
        std::printf("h8sdis: coverage.txt written (%" PRIu64 " of %zu sites decoded)\n", usable, sites.size());
    }
    return 0;
}
