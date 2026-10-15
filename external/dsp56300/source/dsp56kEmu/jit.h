#pragma once

#include <memory>
#include <cstdio>

#include "types.h"

#include <set>
#include <vector>
#include <unordered_map>

#include "debuggerinterface.h"

#include "jitblockchain.h"
#include "jitcacheentry.h"
#include "jitconfig.h"
#include "jitdspmode.h"
#include "jitruntimedata.h"
#include "jittrampoline.h"

namespace asmjit
{
	inline namespace ASMJIT_ABI_NAMESPACE
	{
		class JitRuntime;
		class CodeHolder;
	}
}

namespace dsp56k
{
	struct DspRegs;
	struct JitBlockInfo;
	class DSP;
	class JitBlock;
	class JitProfilingSupport;
	struct JitBlockEmitter;

	class Jit final
	{
	public:
		explicit Jit(DSP& _dsp);
		~Jit();

		DSP& dsp() { return m_dsp; }

		// 2026-10-12 MS2000 memory-appetite hunt: per-second status line counters
		size_t getChainCount() const;
		size_t getCurrentBlockCount() const;
		size_t getCurrentCodeSize() const;
		uint64_t getRecreateCount() const;

		// 2026-10-12 MS2000 recreate-pingpong fix: an entry pc that repeatedly triggers destroy+re-emit
		// rotations (measured: ~75k recreate/s at the boot-ROM entry P:0xFF0000, feeding an asmjit pool
		// that never shrinks -> 4+ GB commit, host dies) becomes a forced block boundary in
		// JitBlock::getInfo: no future block may cover it, so the rotation cannot reform. The hotness
		// map grows only with distinct recreated pcs (bounded by P size); once warm it only does lookups.
		void markRecreateEntry(const TWord _pc)
		{
			if (m_forcedSplits.find(_pc) != m_forcedSplits.end())
				return;		// already a forced boundary
			const auto count = ++m_recreateHotness[_pc];
			if (count == kHotEntryThreshold)
			{
				m_forcedSplits.insert(_pc);
				std::fprintf(stderr, "[JIT] hot entry: pc 0x%06X recreated %u times - forced block boundary added\n", _pc, count);
			}
		}
		const std::set<TWord>& getForcedSplits() const { return m_forcedSplits; }

		// 2026-10-12 recreate-pingpong hunt: the current chain's raw table value at _pc, for the
		// dispatch-path instruments (compare against what m_jitEntries returned at dispatch time).
		const void* debugCurrentChainEntry(const TWord _pc) const
		{
			if (!m_currentChain)
				return nullptr;
			const auto& funcs = m_currentChain->getFuncs();
			if (_pc >= funcs.size())
				return nullptr;
			return *reinterpret_cast<void* const*>(&funcs[_pc]);
		}

		void exec(const TWord _pc)
		{
			m_currentChain->exec(_pc);
		}

		static Jit* toJitPtr(DspRegs* _regs);
		void notifyProgramMemWrite(const TWord _offset);

		void run(TWord _pc) noexcept;
		void runCheckPMemWrite(TWord _pc) noexcept;
		void runCheckPMemWriteAndModeChange(TWord _pc) noexcept;
		void runCheckModeChange(TWord _pc) noexcept;
		void runCheckLoopEnd(TWord _pc) noexcept;

		const JitConfig& getConfig() const { return m_config; }
		JitConfig getConfig(TWord _pc) const;
		void setConfig(const JitConfig& _config) { m_config = _config; }
		void resetHW();
		const std::map<TWord, TWord>& getLoops() const { return m_loops; }
		const std::set< TWord>& getLoopEnds() const { return m_loopEnds; }

		static TJitFunc updateRunFunc(const JitCacheEntry& e);

		auto* getRuntime() { return m_rt; }
		auto& getRuntimeData() { return m_runtimeData; }
		const auto& getVolatileP()  { return m_volatileP; }
		auto* getProfilingSupport() const { return m_profiling.get(); }

		bool isVolatileP(const TWord _pc) const
		{
			return m_volatileP.find(_pc) != m_volatileP.end();
		}

		void create(TWord _pc, bool _execute);
		void recreate(TWord _pc);

		void addLoop(const JitBlockInfo& _info);
		void addLoop(TWord _begin, TWord _end);
		void removeLoop(const JitBlockInfo& _info);
		void removeLoop(TWord _begin);

		void destroy(TWord _pc);
		void destroyToRecreate(TWord _pc);

		void checkModeChange() noexcept;

		void onDebuggerAttached(DebuggerInterface& _debugger) const;

		void destroyAllBlocks();

		JitBlockEmitter* acquireEmitter(JitConfig&& _config);
		JitBlockEmitter* acquireEmitter(TWord _pc);
		void releaseEmitter(JitBlockEmitter* _emitter);

		JitBlockRuntimeData* acquireBlockRuntimeData();
		void releaseBlockRuntimeData(JitBlockRuntimeData* _b);

		void onFuncsResized(const JitBlockChain& _chain) const;

		JitTrampoline& getTrampoline() { return m_trampoline; }

	private:
		void checkPMemWrite() noexcept;
		void checkLoopEnd() noexcept;
		void moveLoopEnd(TWord _begin, TWord _end);

		DSP& m_dsp;
		JitTrampoline m_trampoline;

		asmjit::ASMJIT_ABI_NAMESPACE::JitRuntime* m_rt = nullptr;

		struct DspModeHash
		{
		    std::size_t operator () (const JitDspMode& m) const	{ return m.get(); }
		};

		std::unordered_map<JitDspMode, std::unique_ptr<JitBlockChain>, DspModeHash> m_chains;
		JitBlockChain* m_currentChain = nullptr;

		// 2026-10-12 MS2000 recreate-pingpong fix: see markRecreateEntry / getForcedSplits
		static constexpr uint32_t kHotEntryThreshold = 16;
		std::unordered_map<TWord, uint32_t> m_recreateHotness;
		std::set<TWord> m_forcedSplits;

		std::vector<TJitFunc> m_jitFuncs;
		std::set<TWord> m_volatileP;
		std::map<TWord, TWord> m_loops;
		std::set<TWord> m_loopEnds;

		std::unique_ptr<JitProfilingSupport> m_profiling;

		std::vector<JitBlockEmitter*> m_emitters;
		std::vector<JitBlockRuntimeData*> m_blockRuntimeDatas;

		JitConfig m_config;

		size_t m_maxUsedPAddress = 0;

		// the following data is accessed by JIT code at runtime, it NEEDS to be put last into this struct to be
		// able to use ARM relative addressing, see member ordering in dsp.h
		JitRuntimeData m_runtimeData;
	};
}
