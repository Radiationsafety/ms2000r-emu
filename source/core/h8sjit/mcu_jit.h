#pragma once
// JIT-0 (2026-10-20): the H8S/2350 MCU JIT scaffold, through the vendored asmjit the DSP JIT
// already uses (external/dsp56300/source/asmjit; JitRuntime owns the MAP_JIT / icache handling).
// A "block" here is one host function per hot PC. JIT-0's blocks did nothing but call back into
// the interpreter's lean body (executeInstructionFastBody): zero semantic surface, validating the
// compile pipeline, the calling-convention bridge and the (pc, flash-gen, DRAM-page-gen)
// invalidation against the real firmware.
// JIT-1 (same file, same gates) replaces them with Tier-1a blocks: runs of fused NOPs terminated
// by a fused Bcc, translated natively. The block replays per instruction exactly what the lean
// body does - the SP watch, g_ms2kInsnIndex++, the PC advance, the branch condition through the
// interpreter's own ms2kCcTake, the updateLastExec snapshot, the batching predicate and the
// pend/budget/dmac/cycles commit - and hands anything it cannot prove to the lean body verbatim.
// Cycle surcharges (BUG105's fetchExtraStates) fold to compile-time constants guarded against the
// four bus-programming registers; every step()-tail duty of a non-final instruction (executed++,
// taint cycle, the diag-1000 counter) runs inside the block, so one block of k instructions is
// observably k interpreter steps. Off by default; MS2K_MCUJIT=1 turns it on.
// The emulator object never moves - but nothing here stores a back-pointer: blocks address the
// emulator only through the JitView below, and take the context as their one argument.

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

// asmjit's types live in an ABI inline namespace, so they cannot be forward-declared cheaply:
// the runtime is pimpl'd into mcu_jit.cpp and this header (included by h8s2350_emulator.h,
// which is included everywhere) stays free of asmjit.
namespace ms2k {
namespace mcujit {

// Tier-1a block length cap; also the scan budget per compile.
constexpr uint32_t kMaxInsns = 8;

// Everything a Tier-1a block needs to reach at run time, as plain addresses the emulator fills
// once per compile (jitFillView). Hot members are addressed through this pointer table because
// their offsets inside the ~1.2 MB emulator object exceed ARM64's scaled-displacement range; the
// table itself is small, so every access is ldr[slot] + ldr/str[addr]. The function pointers are
// the shared tails: the interpreter's own code, called exactly as the interpreter would run it.
struct JitView
{
    uint64_t* cycles      = nullptr;   // m_cycles (the bus-time accumulator)
    uint32_t* pc          = nullptr;   // m_registers.pc
    uint32_t* r7          = nullptr;   // m_registers.er[7] - the SP watch reads it, the block never moves it
    void*     flags       = nullptr;   // &m_flags - ms2kCcTake's argument
    uint64_t* insnIndex   = nullptr;   // &g_ms2kInsnIndex
    uint32_t* executed    = nullptr;   // m_cycles_executed (the per-step tail the block must replay)
    uint64_t* taintCycle  = nullptr;   // m_stackTaint.currentCycle
    uint64_t* perBudget   = nullptr;
    uint32_t* perPend     = nullptr;
    uint32_t* irqPending  = nullptr;   // m_irqPendingCount
    bool*     dmacFast    = nullptr;   // m_perDmacFast
    uint32_t* dmacAcc     = nullptr;   // m_dmac_pace_accum
    void*     peripheral  = nullptr;   // &m_peripheral - tested as its first word (verified at fill)
    uint16_t* sciScr      = nullptr;   // &m_sci[0].SCR
    uint16_t* sciSsr      = nullptr;   // &m_sci[0].SSR
    size_t*   midiIdx     = nullptr;
    size_t*   midiEnd     = nullptr;   // m_midiSchedEnd mirror of m_midiSched.size()
    uint8_t*  midiHostPending = nullptr;  // std::atomic<bool> storage, relaxed-load equivalent
    uint8_t*  abwcr = nullptr; uint8_t* astcr = nullptr;
    uint8_t*  wcrh  = nullptr; uint8_t* wcrl  = nullptr;   // BUG105: the fetch-surcharge inputs
    uint8_t   quietBootVal = 0;   // compile-time snapshots: the block re-tests the live bytes
    uint8_t   abwcrVal = 0, astcrVal = 0, wcrhVal = 0, wcrlVal = 0;
    uint32_t* clockStep   = nullptr;   // m_clock_cycles_per_step; blocks commit once per instruction, so 1
    bool*     spReported  = nullptr;   // m_spUnderflowReported
    void*     lastExec    = nullptr;   // &m_last_exec; the field offsets ride below
    uint32_t  leStart = 0, lePc = 0, leSize = 0, lePrimary = 0;
    uint32_t* diagCounter = nullptr;   // the step()'s 1-in-1000 diag counter (file-scope static)
    uint8_t*  quietBoot   = nullptr;   // ::g_h8s_quiet_boot - the fused dispatch reads it live

    void (*handoff)(void* ctx) noexcept      = nullptr;  // executeInstructionFast(): this instruction, interpreter
    void (*commitTick)(void* ctx, uint32_t cycles) noexcept = nullptr;  // tickPeripherals(c); addCycles(c)
    void (*diag1000)(void* ctx) noexcept     = nullptr;  // the g_diag calls at the counter wrap
    int  (*ccTake)(const void* flags, uint32_t cc) noexcept = nullptr;  // ms2kCcTake, the one truth for Bcc

    bool tier1 = false;   // the shared_ptr / atomic<bool> layouts could not be verified: JIT-0 only
};

// One scanned instruction: everything the emitter needs, all of it compile-time constant under
// the guards the block checks at entry (H8S2350Emulator::jitScan). Kind is the interpreter's own
// fused-kind enum: 1 = NOP, 2 = Bcc d:8, 3 = Bcc d:16 (0/4 never reach a Tier-1a block).
struct JitInsnDesc
{
    uint32_t pc = 0;
    uint32_t size = 2;
    uint32_t kind = 0;
    uint32_t op0 = 0;
    uint32_t opcode = 0;
    uint32_t cycles = 0;   // baseCycles + fetchExtraStates(pc,size,op0), guarded by the bus bytes
    uint32_t cc = 0;       // Bcc condition, 0..15
    uint32_t pcFall = 0;   // (pc+size)&0xFFFFFF
    uint32_t pcTaken = 0;  // branch target if taken (== pcFall for NOP)
    uint32_t busOff = 0;   // MS2K_BUSTIMING=off: the surcharge is 0 and needs no register guard
    uint8_t  abwcr = 0, astcr = 0, wcrh = 0, wcrl = 0;   // the guard bytes fetchExtraStates folded through
};

using McuJitFunc = void (*)(void* ctx) noexcept;

class McuJitImpl;

class McuJit final
{
public:
    McuJit();
    ~McuJit();

    // If a compiled block for (pc, gen, pageGen) exists (or this is its kCompileThreshold-th
    // hit at that key), run it and return true. A Tier-1a block retires one to eight fused
    // instructions; it retires at least the keyed one. Otherwise return false: the caller runs
    // the interpreter body for this instruction.
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

    McuJitFunc compileBlock(void* ctx, uint32_t pc);   // Tier-1a if jitScan agrees, else the JIT-0 thunk

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
    uint64_t m_statLen[9] = {};   // compiled blocks by scanned length (0 = fell back to JIT-0)
    bool m_strictShown = false;   // one-shot [MCUJIT/TIER1] rejection dump under MS2K_JITSTRICT
};

}} // namespace ms2k::mcujit
