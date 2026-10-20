#pragma once
// JIT-0 (2026-10-20): the H8S/2350 MCU JIT scaffold, through the vendored asmjit the DSP JIT
// already uses (external/dsp56300/source/asmjit; JitRuntime owns the MAP_JIT / icache handling).
// A "block" here is one host function per PC that calls straight back into the interpreter's lean
// body (executeInstructionFastBody). Zero semantic surface: it validates the compile pipeline, the
// calling-convention bridge, and the invalidation wiring (pc + flash generation + DRAM page
// generation, the same key the decode cache uses) before JIT-1 emits instruction semantics itself.
// Off by default; MS2K_MCUJIT=1 turns it on. The emulator object never moves - but nothing here
// even stores a back-pointer: blocks take the context as their first argument and pass it on.

#include <cstdint>
#include <memory>
#include <vector>

// asmjit's types live in an ABI inline namespace, so they cannot be forward-declared cheaply:
// the runtime is pimpl'd into mcu_jit.cpp and this header (included by h8s2350_emulator.h,
// which is included everywhere) stays free of asmjit.
namespace ms2k {
namespace mcujit {

using McuJitFunc = void (*)(void* ctx) noexcept;

class McuJitImpl;

class McuJit final
{
public:
    McuJit();
    ~McuJit();

    // If a compiled block for (pc, gen, pageGen) exists (or this is its kCompileThreshold-th
    // hit at that key), run it - it executes exactly one instruction - and return true.
    // Otherwise return false: the caller runs the interpreter body for this instruction.
    bool run(void* ctx, uint32_t pc, uint32_t gen, uint32_t pageGen);

private:
    struct Entry
    {
        uint32_t pc = 0xFFFFFFFFu;   // tag; a collision is resolved by replacing the entry
        McuJitFunc fn = nullptr;
        uint32_t gen = 0;
        uint32_t pageGen = 0;
        uint32_t seen = 0;           // hits at this (gen,pageGen) since the key last changed;
                                     // kBlacklist means this PC lost a block to invalidation and
                                     // will not get a new one (its code keeps changing under flash
                                     // writes - compiling again would only feed a recompile storm)
    };

    McuJitFunc compileBlock();   // arch-identical: the block only forwards ctx to the thunk

    static constexpr uint32_t kTableBits = 16;          // 65536 slots vs ~10.5k executed sites
    static constexpr uint32_t kCompileThreshold = 64;   // hits at one key before compiling
    static constexpr uint32_t kBlacklist = 0xFFFFFFFFu; // seen value: never compile this key again
    static constexpr uint32_t kMaxCompiles = 200000;    // then give up (compile storm guard)

    std::unique_ptr<McuJitImpl> m_impl;
    std::vector<Entry> m_table;
    bool m_disabled = false;

    uint64_t m_statExec = 0;      // instructions run through blocks
    uint64_t m_statCompile = 0;   // blocks compiled
    uint64_t m_statInval = 0;     // keys invalidated by a generation bump
    uint64_t m_statFall = 0;      // dispatches the block did not take
};

}} // namespace ms2k::mcujit
