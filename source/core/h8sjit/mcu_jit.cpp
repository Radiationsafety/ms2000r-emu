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
// the thunk takes it in argument register 0 - so the block needs no moves at all. Tier-1a
// blocks hand off through V.handoff, the same call through the table; the fallback here is for
// the view-less JIT-0 shape when jitFillView itself could not run.
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

// The plan one block: what jitScan said about PC and its successors, plus the entry-guard
// snapshots. Built by compileBlock; consumed by the arch emitter.
struct BlockPlan
{
    JitInsnDesc d[kMaxInsns];
    uint32_t n = 0;
    bool busOff = false;      // MS2K_BUSTIMING=off: surcharges are 0, no bus-register guard
    uint8_t quiet = 0;        // ::g_h8s_quiet_boot at compile; entry guard re-tests the byte
    bool spTest = false;      // SP-underflow not yet reported: the watch must run per instruction
};

// JitView slot offsets, for the block's two-step addressing (ldr [view+slot] -> ldr/str [addr]).
#define SLOT(f) uint64_t(offsetof(JitView, f))

} // namespace

class McuJitImpl { public: asmjit::JitRuntime rt; JitView view; };

McuJit::McuJit()
    : m_impl(std::make_unique<McuJitImpl>())
    , m_table(1u << kTableBits)
{
}

McuJit::~McuJit()
{
    if (const char* s = std::getenv("MS2K_JITSTATS"); s && *s && *s != '0') {
        std::printf("[MCUJIT] blocks=%llu exec=%llu inval=%llu fall=%llu%s\n",
                    (unsigned long long)m_statCompile, (unsigned long long)m_statExec,
                    (unsigned long long)m_statInval, (unsigned long long)m_statFall,
                    m_disabled ? " [disabled]" : "");
        std::printf("[MCUJIT] len:");
        for (uint32_t i = 0; i < 9; ++i)
            std::printf(" %u=%llu", i, (unsigned long long)m_statLen[i]);
        std::printf("\n");
    }
}

// ===========================================================================
// JIT-0 shape (and the Tier-1a hand-off tail): exactly one instruction, back into the
// lean body through native code. Kept for every PC jitScan will not translate.
// ===========================================================================
#if defined(HAVE_ARM64)

static void emitJit0(asmjit::a64::Builder& b)
{
    using namespace asmjit::a64;
    using asmjit::Label; using asmjit::Imm;
    // The saved-link frame is NOT optional: blr overwrites x30 with the block's own next
    // instruction, and the callee restores exactly that value before returning - so a bare
    // "mov x30, target; blr x30; ret" makes the final ret jump to ITSELF (measured: the
    // process spins on one ret inside the executable page at 99% CPU). The DSP JIT's
    // trampoline likewise pushes x30 around its calls. x16 (IP0) carries the target; the
    // 16-byte slot keeps SP aligned at the thunk entry, as the PCS requires.
    b.str(asmjit::a64::regs::x30, ptr_pre(asmjit::a64::regs::sp, -16));
    b.mov(asmjit::a64::regs::x16, asmjit::Imm(reinterpret_cast<uint64_t>(&ms2kMcuJitThunk)));
    b.blr(asmjit::a64::regs::x16);
    b.ldr(asmjit::a64::regs::x30, ptr_post(asmjit::a64::regs::sp, 16));
    b.ret(asmjit::a64::regs::x30);
}

// ===========================================================================
// Tier-1a (JIT-1): a run of fused NOPs, ended by a fused Bcc or by whatever jitScan
// stopped at, translated into the lean body's own sequence of state changes. Nothing
// here re-implements instruction semantics: the branch condition goes through the
// interpreter's ms2kCcTake, a batching-predicate failure commits through the
// interpreter's own tickPeripherals+addCycles, a guard failure runs the current
// instruction through the interpreter's executeInstructionFastBody. What the block
// removes is the per-instruction OVERHEAD around those: the dispatch gates, the decode
// lookup, the re-evaluated env tests, the re-materialised constants.
//
// Fidelity rules the emitter encodes - one block of k instructions is observably k
// interpreter steps:
//   - the SP watch, ++g_ms2kInsnIndex, the PC advance, the updateLastExec snapshot, the
//     batching predicate and the pend/budget/dmac/cycles commit run per instruction, in
//     the body's own order;
//   - every instruction except the block's last also runs step()'s tail (executed++, the
//     taint cycle, the diag-1000 counter) inside the block; the last one's tail is the
//     step() that wrapped this dispatch - the totals and the wrap position match exactly;
//   - a batching-predicate failure commits through the helper and ENDS the block, so the
//     step()-tail irqTryService lands on the same instruction it would without the block
//     (only a tick can raise m_irqPendingCount, and every path through a tick ends here);
//   - the entry guards re-test live what the compile-time constants assumed: the clock's
//     one-cycle-per-step mode, the four BUG105 bus-programming registers, the quiet-boot
//     flag the fused dispatch reads, an already-reported SP underflow.
//
// x19 = ctx, x20 = &view, both callee-saved (the helpers may clobber everything else);
// x9-x13 scratch, x16 the call target. Every 32-bit member store goes through a 64-bit
// register: ARM64 has no 32-bit store immediate, so the constant is materialised first.
// ===========================================================================
static void emitTier1(asmjit::a64::Builder& b, const JitView& V, const BlockPlan& P)
{
    using namespace asmjit::a64;
    using asmjit::Label; using asmjit::Imm;
    const Gp CTX = asmjit::a64::regs::x19, VIEW = asmjit::a64::regs::x20;
    const Gp A = asmjit::a64::regs::x9, B = asmjit::a64::regs::x10, D = asmjit::a64::regs::x12;
    const Gp FN = asmjit::a64::regs::x16;

    const Label hand = b.newLabel(), exitL = b.newLabel();

    b.sub(asmjit::a64::regs::sp, asmjit::a64::regs::sp, 32);                 // 16-aligned frame: x19, x30, x20 + pad
    b.stp(CTX, asmjit::a64::regs::x30, ptr(asmjit::a64::regs::sp));
    b.str(VIEW, ptr(asmjit::a64::regs::sp, 16));
    b.mov(CTX, asmjit::a64::regs::x0);
    b.mov(VIEW, asmjit::Imm(reinterpret_cast<uint64_t>(&V)));

    // ---- entry guards: fail -> hand (the body runs this instruction, exactly as today)
    b.ldr(A, ptr(VIEW, SLOT(clockStep)));
    b.ldr(asmjit::a64::regs::w13, ptr(A));
    b.cmp(asmjit::a64::regs::w13, 1);
    b.b_ne(hand);
    if (!P.busOff) {
        const uint64_t gslot[4] = { SLOT(abwcr), SLOT(astcr), SLOT(wcrl), SLOT(wcrh) };
        const uint8_t  gval[4]  = { V.abwcrVal, V.astcrVal, V.wcrlVal, V.wcrhVal };
        for (int k = 0; k < 4; ++k) {
            b.ldr(A, ptr(VIEW, gslot[k]));
            b.ldrb(asmjit::a64::regs::w13, ptr(A));
            b.cmp(asmjit::a64::regs::w13, Imm(gval[k]));
            b.b_ne(hand);
        }
    }
    b.ldr(A, ptr(VIEW, SLOT(quietBoot)));
    b.ldrb(asmjit::a64::regs::w13, ptr(A));
    b.cmp(asmjit::a64::regs::w13, Imm(P.quiet));
    b.b_ne(hand);

    for (uint32_t i = 0; i < P.n; ++i) {
        const JitInsnDesc& d = P.d[i];
        const Imm CY(d.cycles);

        if (P.spTest) {                             // the body's SP watch, before the insn runs
            b.ldr(A, ptr(VIEW, SLOT(r7)));
            b.ldr(asmjit::a64::regs::w13, ptr(A));
            b.and_(asmjit::a64::regs::w13, asmjit::a64::regs::w13, 0x00FFFFFFu);
            b.mov(asmjit::a64::regs::w14, asmjit::Imm(0x00FFFC00u));
            b.sub(asmjit::a64::regs::w13, asmjit::a64::regs::w13, asmjit::a64::regs::w14);
            b.mov(asmjit::a64::regs::w14, asmjit::Imm(0x240u));
            b.cmp(asmjit::a64::regs::w13, asmjit::a64::regs::w14);
            b.b_lo(hand);
        }
        b.ldr(A, ptr(VIEW, SLOT(insnIndex)));
        b.ldr(B, ptr(A));
        b.add(B, B, 1);
        b.str(B, ptr(A));

        b.mov(asmjit::a64::regs::w11, asmjit::Imm(d.pcFall));
        b.ldr(A, ptr(VIEW, SLOT(pc)));
        b.str(asmjit::a64::regs::w11, ptr(A));
        if (d.kind != 1) {                          // fused Bcc: the interpreter's own table decides
            b.ldr(asmjit::a64::regs::x0, ptr(VIEW, SLOT(flags)));
            b.mov(asmjit::a64::regs::w1, Imm(d.cc));
            b.ldr(FN, ptr(VIEW, SLOT(ccTake)));
            b.blr(FN);
            const Label fall = b.newLabel();
            b.cbz(asmjit::a64::regs::w0, fall);
            b.mov(asmjit::a64::regs::w14, asmjit::Imm(d.pcTaken));
            b.ldr(A, ptr(VIEW, SLOT(pc)));
            b.str(asmjit::a64::regs::w14, ptr(A));
            b.bind(fall);
        }
        {                                            // updateLastExec, as the fused path leaves it
            b.ldr(A, ptr(VIEW, SLOT(lastExec)));
            b.mov(asmjit::a64::regs::w11, asmjit::Imm(d.pc));
            b.str(asmjit::a64::regs::w11, ptr(A, V.leStart));
            b.ldr(B, ptr(VIEW, SLOT(pc)));
            b.ldr(asmjit::a64::regs::w11, ptr(B));
            b.str(asmjit::a64::regs::w11, ptr(A, V.lePc));
            b.mov(asmjit::a64::regs::w15, Imm(d.size));
            b.strb(asmjit::a64::regs::w15, ptr(A, V.leSize));
            b.mov(asmjit::a64::regs::w15, Imm(d.opcode & 0xFFu));
            b.strb(asmjit::a64::regs::w15, ptr(A, V.lePrimary));
        }

        // ---- the batching predicate, verbatim; fail -> commit through the helper, end block
        const Label fail = b.newLabel(), ok = b.newLabel(), sciNo = b.newLabel();
        b.ldr(A, ptr(VIEW, SLOT(peripheral)));
        b.ldr(B, ptr(A));
        b.cbnz(B, fail);
        b.ldr(A, ptr(VIEW, SLOT(perBudget)));
        b.ldr(B, ptr(A));
        b.cmp(B, CY);
        b.b_ls(fail);                   // !(cycles < budget)
        b.ldr(A, ptr(VIEW, SLOT(irqPending)));
        b.ldr(asmjit::a64::regs::w13, ptr(A));
        b.cbnz(asmjit::a64::regs::w13, fail);
        b.ldr(A, ptr(VIEW, SLOT(sciScr)));
        b.ldrh(asmjit::a64::regs::w13, ptr(A));
        b.tst(asmjit::a64::regs::w13, 0x40u);
        b.b_eq(sciNo);
        b.tst(asmjit::a64::regs::w13, 0x10u);
        b.b_eq(sciNo);
        b.ldr(D, ptr(VIEW, SLOT(sciSsr)));
        b.ldrh(asmjit::a64::regs::w14, ptr(D));
        b.tst(asmjit::a64::regs::w14, 0x40u);
        b.b_ne(fail);
        b.bind(sciNo);
        b.ldr(A, ptr(VIEW, SLOT(midiIdx)));
        b.ldr(B, ptr(A));
        b.ldr(D, ptr(VIEW, SLOT(midiEnd)));
        b.ldr(asmjit::a64::regs::x11, ptr(D));
        b.cmp(B, asmjit::a64::regs::x11);
        b.b_lo(fail);                   // idx < size: sched in flight - do not batch
        b.ldr(A, ptr(VIEW, SLOT(midiHostPending)));
        b.ldrb(asmjit::a64::regs::w13, ptr(A));
        b.cbnz(asmjit::a64::regs::w13, fail);
        b.b(ok);

        b.bind(fail);
        b.mov(asmjit::a64::regs::x0, CTX);
        b.mov(asmjit::a64::regs::w1, CY);
        b.ldr(FN, ptr(VIEW, SLOT(commitTick)));
        b.blr(FN);
        b.b(exitL);

        b.bind(ok);
        // pend += c; budget -= c; dmac pace; cycles += c - the body's exact order
        b.ldr(A, ptr(VIEW, SLOT(perPend)));
        b.ldr(asmjit::a64::regs::w13, ptr(A));
        b.add(asmjit::a64::regs::w13, asmjit::a64::regs::w13, CY);
        b.str(asmjit::a64::regs::w13, ptr(A));
        b.ldr(A, ptr(VIEW, SLOT(perBudget)));
        b.ldr(B, ptr(A));
        b.sub(B, B, CY);
        b.str(B, ptr(A));
        {
            const Label dmacDone = b.newLabel();
            b.ldr(A, ptr(VIEW, SLOT(dmacFast)));
            b.ldrb(asmjit::a64::regs::w13, ptr(A));
            b.cbz(asmjit::a64::regs::w13, dmacDone);
            b.ldr(A, ptr(VIEW, SLOT(dmacAcc)));
            b.ldr(asmjit::a64::regs::w13, ptr(A));
            b.add(asmjit::a64::regs::w13, asmjit::a64::regs::w13, CY);
            b.mov(asmjit::a64::regs::w14, 64);
            b.cmp(asmjit::a64::regs::w13, asmjit::a64::regs::w14);
            b.b_lo(dmacDone);
            b.mov(asmjit::a64::regs::w13, 0);
            b.str(asmjit::a64::regs::w13, ptr(A));
            b.bind(dmacDone);
        }
        b.ldr(A, ptr(VIEW, SLOT(cycles)));
        b.ldr(B, ptr(A));
        b.add(B, B, CY);
        b.str(B, ptr(A));

        if (i + 1 < P.n) {                          // step()'s tail for a non-final instruction
            b.ldr(A, ptr(VIEW, SLOT(executed)));
            b.ldr(asmjit::a64::regs::w13, ptr(A));
            b.add(asmjit::a64::regs::w13, asmjit::a64::regs::w13, 1);
            b.str(asmjit::a64::regs::w13, ptr(A));
            b.ldr(D, ptr(VIEW, SLOT(taintCycle)));
            b.str(asmjit::a64::regs::x13, ptr(D));                // the uint32 value, zero-extended, into the uint64
            const Label diagStore = b.newLabel(), diagDone = b.newLabel();
            b.ldr(A, ptr(VIEW, SLOT(diagCounter)));
            b.ldr(asmjit::a64::regs::w13, ptr(A));
            b.add(asmjit::a64::regs::w13, asmjit::a64::regs::w13, 1);
            b.mov(asmjit::a64::regs::w14, 1000);
            b.cmp(asmjit::a64::regs::w13, asmjit::a64::regs::w14);
            b.b_ne(diagStore);
            b.mov(asmjit::a64::regs::w14, 0);
            b.str(asmjit::a64::regs::w14, ptr(A));
            b.mov(asmjit::a64::regs::x0, CTX);
            b.ldr(FN, ptr(VIEW, SLOT(diag1000)));
            b.blr(FN);
            b.b(diagDone);
            b.bind(diagStore);
            b.str(asmjit::a64::regs::w13, ptr(A));
            b.bind(diagDone);
        }
    }
    b.b(exitL);                                  // the block retired: never fall into the hand

    b.bind(hand);
    b.mov(asmjit::a64::regs::x0, CTX);
    b.mov(FN, asmjit::Imm(reinterpret_cast<uint64_t>(V.handoff ? V.handoff : &ms2kMcuJitThunk)));
    b.blr(FN);

    b.bind(exitL);
    b.ldr(VIEW, ptr(asmjit::a64::regs::sp, 16));
    b.ldp(CTX, asmjit::a64::regs::x30, ptr(asmjit::a64::regs::sp));
    b.add(asmjit::a64::regs::sp, asmjit::a64::regs::sp, 32);
    b.ret(asmjit::a64::regs::x30);
}

#else // x86-64

static void emitJit0(asmjit::x86::Builder& b)
{
    // entered with rsp = 8 (mod 16) per the C ABI; at the call the callee must see
    // the same, hence the +/- 8 around it.
    b.sub(asmjit::x86::rsp, 8);
    b.call(reinterpret_cast<const void*>(&ms2kMcuJitThunk));
    b.add(asmjit::x86::rsp, 8);
    b.ret();
}

// See the ARM64 block above for the fidelity rules. rbx = ctx, r12 = &view (both callee-saved);
// rax/rcx/rdx/rsi/rdi/r8/r9 scratch. Entry rsp = 8 (mod 16): two pushes leave it 8, the pad
// sub brings it to 0 exactly at each call site (the ABI wants 0 before the call pushes).
static void emitTier1(asmjit::x86::Builder& b, const JitView& V, const BlockPlan& P)
{
    using namespace asmjit::x86;
    using asmjit::Imm;
    using asmjit::Label;
    const Gp CTX = regs::rbx, VIEW = regs::r12;
    const Gp A = regs::rax, B = regs::rcx;
    const Gp FN = regs::r11;
    const Gp B32 = regs::ecx;

    const Label hand = b.newLabel(), exitL = b.newLabel();

    b.push(CTX);
    b.push(VIEW);
    b.sub(rsp, 8);
    b.mov(CTX, regs::rdi);
    b.mov(VIEW, asmjit::Imm(reinterpret_cast<uint64_t>(&V)));

    b.mov(A, ptr(VIEW, SLOT(clockStep)));
    b.cmp(dword_ptr(A), 1);
    b.jne(hand);
    if (!P.busOff) {
        const uint64_t gslot[4] = { SLOT(abwcr), SLOT(astcr), SLOT(wcrl), SLOT(wcrh) };
        const uint8_t  gval[4]  = { V.abwcrVal, V.astcrVal, V.wcrlVal, V.wcrhVal };
        for (int k = 0; k < 4; ++k) {
            b.mov(A, ptr(VIEW, gslot[k]));
            b.cmp(byte_ptr(A), Imm(gval[k]));
            b.jne(hand);
        }
    }
    b.mov(A, ptr(VIEW, SLOT(quietBoot)));
    b.cmp(byte_ptr(A), Imm(P.quiet));
    b.jne(hand);

    for (uint32_t i = 0; i < P.n; ++i) {
        const JitInsnDesc& d = P.d[i];
        const Imm CY(d.cycles);

        if (P.spTest) {
            b.mov(A, ptr(VIEW, SLOT(r7)));
            b.mov(B32, dword_ptr(A));
            b.and_(B32, 0x00FFFFFFu);
            b.sub(B32, 0x00FFFC00u);
            b.cmp(B32, 0x240u);
            b.jb(hand);
        }
        b.mov(A, ptr(VIEW, SLOT(insnIndex)));
        b.inc(qword_ptr(A));

        b.mov(A, ptr(VIEW, SLOT(pc)));
        b.mov(dword_ptr(A), Imm(d.pcFall));
        if (d.kind != 1) {
            b.mov(regs::rdi, ptr(VIEW, SLOT(flags)));
            b.mov(regs::esi, Imm(d.cc));
            b.mov(FN, ptr(VIEW, SLOT(ccTake)));
            b.call(FN);
            const Label fall = b.newLabel();
            b.test(eax, eax);
            b.jz(fall);
            b.mov(A, ptr(VIEW, SLOT(pc)));
            b.mov(dword_ptr(A), Imm(d.pcTaken));
            b.bind(fall);
        }
        {
            b.mov(A, ptr(VIEW, SLOT(lastExec)));
            b.mov(dword_ptr(A, V.leStart), Imm(d.pc));
            b.mov(B, ptr(VIEW, SLOT(pc)));
            b.mov(regs::edx, dword_ptr(B));
            b.mov(dword_ptr(A, V.lePc), regs::edx);
            b.mov(byte_ptr(A, V.leSize), Imm(d.size));
            b.mov(byte_ptr(A, V.lePrimary), Imm(d.opcode & 0xFFu));
        }

        const Label fail = b.newLabel(), ok = b.newLabel(), sciNo = b.newLabel();
        b.mov(A, ptr(VIEW, SLOT(peripheral)));
        b.cmp(qword_ptr(A), 0);
        b.jne(fail);
        b.mov(A, ptr(VIEW, SLOT(perBudget)));
        b.cmp(qword_ptr(A), CY);
        b.jbe(fail);                                // !(cycles < budget)
        b.mov(A, ptr(VIEW, SLOT(irqPending)));
        b.cmp(dword_ptr(A), 0);
        b.jne(fail);
        b.mov(A, ptr(VIEW, SLOT(sciScr)));
        b.movzx(B32, word_ptr(A));
        b.test(B32, 0x40u);
        b.jz(sciNo);
        b.test(B32, 0x10u);
        b.jz(sciNo);
        b.mov(A, ptr(VIEW, SLOT(sciSsr)));
        b.test(word_ptr(A), 0x40u);
        b.jnz(fail);
        b.bind(sciNo);
        b.mov(A, ptr(VIEW, SLOT(midiIdx)));
        b.mov(B, qword_ptr(A));
        b.mov(A, ptr(VIEW, SLOT(midiEnd)));
        b.cmp(B, qword_ptr(A));
        b.jb(fail);                                 // idx < size: sched in flight - do not batch
        b.mov(A, ptr(VIEW, SLOT(midiHostPending)));
        b.cmp(byte_ptr(A), 0);
        b.jne(fail);
        b.jmp(ok);

        b.bind(fail);
        b.mov(regs::rdi, CTX);
        b.mov(regs::esi, CY);
        b.mov(FN, ptr(VIEW, SLOT(commitTick)));
        b.call(FN);
        b.jmp(exitL);

        b.bind(ok);
        // pend += c; budget -= c; dmac pace; cycles += c - the body's exact order
        b.mov(A, ptr(VIEW, SLOT(perPend)));
        b.add(dword_ptr(A), CY);
        b.mov(A, ptr(VIEW, SLOT(perBudget)));
        b.sub(qword_ptr(A), CY);
        {
            const Label dmacDone = b.newLabel();
            b.mov(A, ptr(VIEW, SLOT(dmacFast)));
            b.cmp(byte_ptr(A), 0);
            b.je(dmacDone);
            b.mov(A, ptr(VIEW, SLOT(dmacAcc)));
            b.add(dword_ptr(A), CY);
            b.cmp(dword_ptr(A), 64);
            b.jb(dmacDone);
            b.mov(dword_ptr(A), 0);
            b.bind(dmacDone);
        }
        b.mov(A, ptr(VIEW, SLOT(cycles)));
        b.add(qword_ptr(A), CY);

        if (i + 1 < P.n) {
            b.mov(A, ptr(VIEW, SLOT(executed)));
            b.inc(dword_ptr(A));
            b.mov(B, ptr(VIEW, SLOT(executed)));
            b.mov(B32, dword_ptr(B));
            b.mov(A, ptr(VIEW, SLOT(taintCycle)));
            b.mov(qword_ptr(A), B);
            const Label diagDone = b.newLabel();
            b.mov(A, ptr(VIEW, SLOT(diagCounter)));
            b.inc(dword_ptr(A));
            b.cmp(dword_ptr(A), 1000);
            b.jne(diagDone);
            b.mov(dword_ptr(A), 0);
            b.mov(regs::rdi, CTX);
            b.mov(FN, ptr(VIEW, SLOT(diag1000)));
            b.call(FN);
            b.bind(diagDone);
        }
    }
    b.jmp(exitL);                                   // the block retired: never fall into the hand

    b.bind(hand);
    b.mov(regs::rdi, CTX);
    b.call(reinterpret_cast<const void*>(V.handoff ? V.handoff : &ms2kMcuJitThunk));

    b.bind(exitL);
    b.add(rsp, 8);
    b.pop(VIEW);
    b.pop(CTX);
    b.ret();
}

#endif // arch

McuJitFunc McuJit::compileBlock(void* ctx, uint32_t pc)
{
    MS2000::H8S2350Emulator* emu = static_cast<MS2000::H8S2350Emulator*>(ctx);
    JitView& V = m_impl->view;
    BlockPlan P;
    try {
        emu->jitFillView(V);
        if (V.tier1 && emu->jitTier1StaticOk()) {
            const bool dram = pc >= 0x400000u;
            const uint32_t page = dram ? (pc - 0x400000u) >> 8 : 0u;
            uint32_t pcCur = pc;
            while (P.n < kMaxInsns) {
                if (!emu->jitScan(pcCur, P.d[P.n])) break;
                // The block key carries only the ENTRY page's pageGen: a DRAM run must not
                // cross into a page whose writes could not invalidate this block.
                if (dram && ((P.d[P.n].pc - 0x400000u) >> 8) != page) break;
                if (P.n == 0) {
                    P.busOff = P.d[0].busOff != 0;
                    P.quiet  = V.quietBootVal;
                    P.spTest = !V.spReported || !*V.spReported;
                }
                ++P.n;
                if (P.d[P.n - 1].kind != 1) break;    // a branch, taken or not, ends the block
                pcCur = P.d[P.n - 1].pcFall;
            }
        }
    } catch (...) {                                   // decode-side allocation: JIT-0 shape
        P.n = 0;
    }

    ++m_statLen[P.n < 9 ? P.n : 8];
    if (const char* s = std::getenv("MS2K_JITSTRICT"); s && *s && *s != '0' && !m_strictShown) {
        m_strictShown = true;
        std::printf("[MCUJIT/TIER1] pc=%06X tier1=%d staticOk=%d n=%u why=%d\n",
                    pc, int(V.tier1), int(emu->jitTier1StaticOk()), P.n, emu->jitScanWhy());
    }

    try {
        asmjit::CodeHolder code;
        code.init(m_impl->rt.environment());
        {
#if defined(HAVE_ARM64)
            asmjit::a64::Builder b(&code);
            if (P.n) emitTier1(b, V, P); else emitJit0(b);
#else
            asmjit::x86::Builder b(&code);
            if (P.n) emitTier1(b, V, P); else emitJit0(b);
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
        e.fn = compileBlock(ctx, pc);
        if (!e.fn) { ++m_statFall; return false; }
    }
    e.fn(ctx);                              // one to eight instructions, via native code
    ++m_statExec;
    return true;
}

}} // namespace ms2k::mcujit

#undef SLOT
