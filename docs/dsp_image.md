# The DSP56362's program: how it gets into the chip, and how to take it out

Status tags follow `ipl_protocol.md`: **VERIFIED** = measured in this emulator with the
instrument named; **INFERRED** = read out of a manual or the board, not re-measured here.

## 1. There is no DSP program file

**VERIFIED** (boot sequence, `dsp56362_emulator.cpp`): the only DSP ROM ever read from disk is
Motorola's 56362 boot ROM (`boot-362.bin`, 192 words x 4 LE, latched OMR mode 5, PC=$FF0000).
Korg's program is not in any file in `MS2000_Full_Firmware/` either: it lives *inside* the H8S's
`flash.bin` as a byte payload, and after power-on the H8S firmware streams it to the DSP over
SCI0 in SPI-clock-synchronous mode (SCMR0=0xFA, SS = port PF1 - `h8s2350_emulator.cpp`,
`sci0SpiCharacterDone()`), a byte at a time, per SCI0 character.

## 2. Framing

**VERIFIED** (RE-1 tap, `MS2K_DSPCAP`):

* `spiByte()` assembles words per `HCSR.HM`: 24-bit mode = 3 bytes/word, MSB first,
  `m_inWord = (m_inWord << 8) | mosi` (`dsp56362_emulator.cpp`).
* A word is handed to the SHI RX FIFO on the third byte; with `HFIFO=1` it is 10-deep,
  and words arriving into a full FIFO are dropped with HROE (the tap's `flags` bit0 records
  exactly that: a recorded-but-not-accepted word is a real loss on the wire too).
* The reply direction is latched at byte 0 of each word from the SHI's TX register; the tap
  records staleness (bit1) the same way the VOC-DEATH-3 `D0LAT` probe does.
* 10 s of a cold boot streams 133774 accepted words, overruns 0.

**INFERRED** (Motorola UM DSP56300FM/8.1, SHI bootloader chapter, mirrored by the boot ROM in
`tools/dsp_bootrom_probe.cpp`): command words select the destination memory and length; the
boot ROM itself, not the wire, performs the writes. So the tap is a faithful *record* of the
transfer, but the authoritative image is what lands in the DSP's memory.

Facts **VERIFIED** in this tree before RE-1/RE-2: stage-1 lands at `P:$000000-$000007` (10 words,
flash.bin offset 0x03BCDE, `tools/dsp_bootrom_probe.cpp`); wavedata command `$70` fills
`Y:$490000-$4BFFFF` (`BUG119` block comment); the program-ID query `D01064` and its reply window
(VOC-DEATH-3).

## 3. Taking the image out: MS2K_DSPSNAP

**VERIFIED** (this build, Oct 20 2026): with `MS2K_DSPSNAP=<dir>` the emulator waits until

1. >= 20 000 words have flowed (the upload is ~130 K words; power-on line silence is not a
   completed upload), and
2. the SHI word counter has not moved for 50 ms of MCU time, and
3. two FNV-1a hashes of `P:[0..$10000)` and `Y:[$490000..$4C0000)`, taken >= 50 ms apart, agree,

then writes to `<dir>`: `p_prog.raw` / `x_data.raw` / `y_wave.raw` (24-bit words, 3 bytes each,
big endian - directly the input format of the library's `dsp56kDisassemble`), `p_prog.s` (the
library's own `Memory::saveAssembly` listing), and `manifest.txt` (cycle stamps, counters, the
two hashes, the ranges). On the MS2000R v1.07 dump this completes at MCU t=2.583 s.

The hashing range and the quiet/flow thresholds are tuned for this machine's boot; they are
measurement, not hardware. `P:[0..$10000)` covers the program (the boot ROM's own 192 words live
at $FF0000 and are not Korg's); `Y:[$490000..$4C0000)` is the KM416V1204 DRAM as the Port A
windows alias it (`BUG119`): `Y:$400000..$7FFFFF` is ONE 1 M-word physical array, mapped four
times over. **STATED APPROXIMATION** (unchanged from BUG119): the part stores 16 bits (D8-D23);
the emulator stores and returns all 24, so `y_wave.raw`'s low byte is 00 where the hardware's
resistor networks would leave it floating.

## 4. Reading it

```
MS2K_DSPCAP=dumps/dsp/shi.bin MS2K_DSPSNAP=dumps/dsp \
  ms2000_emulator --headless --quiet-boot --duration 10
build/.../dsp56kDisassemble -in dumps/dsp/p_prog.raw -out dumps/dsp/p_prog_dis.txt -pc 0
```

The sweep decodes 4635 words of the loaded program's word range with 2 undecoded trailing words
(2026-10-20 measurement).

## 5. Ownership of the bytes

The dump is whatever is in *your* unit's flash. The `dumps/` tree is gitignored and the repo
never carries DSP program content, listings of it, or wavedata - only the instruments above.
