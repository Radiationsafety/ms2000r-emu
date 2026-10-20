#include "mcu_jit.h"

#include "dsp56kBase/buildconfig.h"   // HAVE_ARM64 / HAVE_X86_64, as the DSP JIT sees them
#include "../h8s2350_emulator.h"

#include <asmjit/asmjit.h>
#if defined(HAVE_ARM64)
#include <asmjit/arm/a64builder.h>
#include <asmjit/arm/a64operand.h>
#else
#include <asmjit/x86/x86builder.h>
#include <asmjit/x86/x86operand.h>
#endif

#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace ms2k {
namespace mcujit {

namespace {

// The whole of JIT-0's native semantics: forward the context to the interpreter's lean body.
// The block arrives with ctx already in argument register 0 (the C ABI it was called through),
// the thunk takes it in argument register 0 - so the block needs no moves at all.
void ms2kMcuJitThunk(void* ctx) noexcept
{
    static_cast<MS2000::H8S2350Emulator*>(ctx)->mcuJitExecOne();
}

// JIT-1's native code will address these fields directly; pin the layout the emitted
// displacements will assume. If the struct changes, this is where the JIT notices first.
static_assert(offsetof(MS2000::H8S2350Registers, er) == 0, "MCU JIT ABI: er[8] must stay first");
static_assert(sizeof(MS2000::H8S2350Registers::er) == 32, "MCU JIT ABI: er[] is 8 x uint32");
static_assert(offsetof(MS2000::H8S2350Registers, pc) > offsetof(MS2000::H8S2350Registers, rl), "MCU JIT ABI: pc after the mirrors");
static_assert(offsetof(MS2000::H8S2350Registers, pc) == offsetof(MS2000::H8S2350Registers, rl) + sizeof(MS2000::H8S2350Registers::rl),
              "MCU JIT ABI: pc directly behind rl[]");

} // namespace

struct McuJitImpl { asmjit::JitRuntime rt; };

McuJit::McuJit()
    : m_impl(std::make_unique<McuJitImpl>())
    , m_table(1u << kTableBits)
{
}

McuJit::~McuJit()
{
    if (const char* s = std::getenv("MS2K_JITSTATS"); s && *s && *s != '0')
        std::printf("[MCUJIT] blocks=%llu exec=%llu inval=%llu fall=%llu%s\n",
                    (unsigned long long)m_statCompile, (unsigned long long)m_statExec,
                    (unsigned long long)m_statInval, (unsigned long long)m_statFall,
                    m_disabled ? " [disabled]" : "");
}

McuJitFunc McuJit::compileBlock()
{
    try {
        asmjit::CodeHolder code;
        code.init(m_impl->rt.environment());
        {
#if defined(HAVE_ARM64)
            asmjit::a64::Builder b(&code);
            // The saved-link frame is NOT optional: blr overwrites x30 with the block's own next
            // instruction, and the callee restores exactly that value before returning - so a bare
            // "mov x30, target; blr x30; ret" makes the final ret jump to ITSELF (measured: the
            // process spins on one ret inside the executable page at 99% CPU). The DSP JIT's
            // trampoline likewise pushes x30 around its calls. x16 (IP0) carries the target; the
            // 16-byte slot keeps SP aligned at the thunk entry, as the PCS requires.
            b.str(asmjit::a64::regs::x30, asmjit::a64::ptr_pre(asmjit::a64::regs::sp, -16));
            b.mov(asmjit::a64::regs::x16, asmjit::Imm(reinterpret_cast<uint64_t>(&ms2kMcuJitThunk)));
            b.blr(asmjit::a64::regs::x16);
            b.ldr(asmjit::a64::regs::x30, asmjit::a64::ptr_post(asmjit::a64::regs::sp, 16));
            b.ret(asmjit::a64::regs::x30);
#else
            asmjit::x86::Builder b(&code);
            // entered with rsp = 8 (mod 16) per the C ABI; at the call the callee must see
            // the same, hence the +/- 8 around it.
            b.sub(asmjit::x86::rsp, 8);
            b.call(reinterpret_cast<const void*>(&ms2kMcuJitThunk));
            b.add(asmjit::x86::rsp, 8);
            b.ret();
#endif
            if (b.finalize()) { m_disabled = true; return nullptr; }
        }
        McuJitFunc fn = nullptr;
        if (m_impl->rt.add(&fn, &code) || !fn) { m_disabled = true; return nullptr; }
        ++m_statCompile;
        return fn;
    } catch (...) {   // asmjit throws only if the runtime cannot allocate executable memory
        m_disabled = true;
        return nullptr;
    }
}

bool McuJit::run(void* ctx, uint32_t pc, uint32_t gen, uint32_t pageGen)
{
    if (m_disabled) return false;
    Entry& e = m_table[(pc >> 1) & ((1u << kTableBits) - 1)];
    if (e.pc != pc) {
        if (e.fn) { m_impl->rt.release(reinterpret_cast<void*>(e.fn)); e.fn = nullptr; }
        e.pc = pc; e.gen = gen; e.pageGen = pageGen; e.seen = 1;
        ++m_statFall;
        return false;                       // first sight of this key: the interpreter takes it
    }
    if (e.gen != gen || e.pageGen != pageGen) {
        ++m_statInval;
        // A PC whose block was already invalidated once lives under code that keeps changing
        // (flash writes bump the generation on EVERY bus write - boot traffic is thousands per
        // second). Re-compiling it feeds a storm of mmap/mprotect/munmap, so it is blacklisted:
        // the interpreter runs it from here on. This is what made JIT-0 unmeasurably slow before.
        e.gen = gen; e.pageGen = pageGen;
        if (e.fn) { m_impl->rt.release(reinterpret_cast<void*>(e.fn)); e.fn = nullptr; e.seen = kBlacklist; }
        else e.seen = 1;
        return false;                       // the code under this PC changed: interpreter again
    }
    if (!e.fn) {
        if (e.seen == kBlacklist) { ++m_statFall; return false; }
        if (++e.seen < kCompileThreshold) { ++m_statFall; return false; }
        if (m_statCompile >= kMaxCompiles) { m_disabled = true; ++m_statFall; return false; }
        e.fn = compileBlock();
        if (!e.fn) { ++m_statFall; return false; }
    }
    e.fn(ctx);                              // exactly one instruction, lean body, via native code
    ++m_statExec;
    return true;
}

}} // namespace ms2k::mcujit
