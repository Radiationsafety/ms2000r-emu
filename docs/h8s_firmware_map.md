# H8S/2350 firmware map (MS2000R v1.07 flash.bin) - measured from within the emulator

Tags as in `ipl_protocol.md`: **VERIFIED** = measured through the emulator or `tools/h8sdis`
on the owner's own dump; **INFERRED** = from the Renesas manual or the service manual.
No listing content here: `tools/h8sdis` writes the disassembly and the coverage tables to the
gitignored `dumps/h8s/` folder.

## 1. Address map as the emulator sees it

**VERIFIED** (`h8s2350_memory_map.h`, `flash_rom.h`, `readByteBus`/`writeByte`):

| range | content |
|---|---|
| `0x000000-0x0003FF` | exception vectors (VBR=0 after reset): longword at `vec*4`, 24-bit masked (`irqTryService`) |
| `0x000810` | the IPL reset entry longword (see `ipl_protocol.md` section 1) |
| `0x000000-0x0FFFFF` | the 1 MB flash (MBM29LV800, CS0); writes go through `FlashROM::busWrite`, persisted as `ms2000_flash_state.bin/.sectors`, never into `flash.bin` |
| `0x100000-0x17FFFF` | external CPU SRAM (the firmware's copied vector table lives here - `vector_table_tracker`) |
| `0x400000-0x47FFFF` | DRAM; code pages here are decode-cached and page-invalidated (`dramCodeWrite`) |
| `0xF80000` window, `0xFFF400-0xFFFBFF` | on-chip RAM |
| `0xFFFE00-0xFFFFFF` | I/O registers (the extracted list: `ms2k_fw_ports_map.h`) |

## 2. What has been executed: the site census (RE-3)

**VERIFIED**, one cold boot + 10 s (`MS2K_OPCOV` -> `tools/h8sdis --opcov`, 2026-10-20):

* 10528 distinct instruction sites executed; all of them are inside the flash image (0 DRAM
  code sites in this window - the copy of code into DRAM did not run during boot+10 s idle).
* The tree's own decoder decodes 10086 of them (95.8%) to a mnemonic the executor uses.
* 439 of the 442 undecoded sites are TWO primary opcodes: `0x10` (300 sites) and `0x11`
  (139 sites). Everything else is noise by comparison. These are the decoder's next arms by
  census order, and until they decode, they are the exclusion list for the MCU JIT's Tier-1.
* A whole-ROM linear sweep (476374 sites) leaves 17813 undecoded - the data areas included,
  so it is a floor, not a measure: what matters is coverage of executed sites.

## 3. Shape of the executed code

**VERIFIED** (same run, `MS2K_OPHIST=1` site histogram, commit `967cc18`): no poll loop
dominates - the hottest site is 1.13% of executed instructions, the top twenty add to 15.6%,
spread over ~10500 sites. The interpreter's ~45 ns per instruction is the work itself, which is
why the speed plan (JIT-0/1: a translating JIT over the same decode cache) targets per-instruction
overhead broadly, not one loop.

**INFERRED** (manual HM Rev 3.00 Appendix A.1): executed-opcode census above the decode gap:
`0x6A mov.b @(d:16,ERn),Rx7`-family (1076 sites), `0x01 mov.w` group (912), `0x7A mov.b` group
(802), `0x0B bset` group (491), `0x1A sub.w` group (444).

## 4. Instruments

| instrument | gives |
|---|---|
| `MS2K_OPCOV=<file>` | every executed site with its encoding (append, exit-time) - the input of the coverage report |
| `MS2K_OPHIST=1` | opcode and per-site ([PCHIST]) histograms at exit |
| `tools/h8sdis` | `vectors.txt`, `dis_<range>.s`, `coverage.txt` from the ROM + an OPCOV file, through `decode<StaticByteSource>` |
| `getDisassembly()` | live emulator-window disassembly (RE-3: was the fixed-16-bit stub) |

## 5. Ownership

`flash.bin` is a dump of *your* unit. The repository carries no firmware bytes and no listings
of them; everything measured lands in the gitignored `dumps/`.
