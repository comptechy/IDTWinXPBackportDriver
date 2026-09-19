/*****************************************************************************
 * hdaregs.h
 *****************************************************************************
 * Intel High Definition Audio controller MMIO register layout, and HDA verb
 * encoding helpers. Register offsets are from the public Intel HD Audio
 * specification (rev 1.0a) - this is a standardized controller interface,
 * not IDT/codec-specific, so it's safe to define from the spec directly
 * rather than from stwrt64.sys disassembly.
 *
 * The controller itself (the "bus" side, GCTL/CORB/RIRB/stream registers)
 * is common to any HDA implementation. What's codec-specific (and what we
 * derived from sigmatel.c / stwrt64.sys reverse-engineering) is the verb
 * sequence sent *through* this transport - see hdaverbs.h and the codec
 * init table in codec.cpp.
 */

#ifndef _HDAREGS_H_
#define _HDAREGS_H_

#pragma pack(push, 1)

//
// HDA controller MMIO register offsets (from the single memory BAR the
// PCI HDA controller exposes - unlike AC97's I/O-port pair, HDA is a
// single MMIO region; ValidateResources() in adapter.cpp should expect
// 1 memory resource + 1 IRQ, 0 ports, 0 DMA channels - PortCls does the
// scatter-gather DMA itself via the BDL, there's no legacy ISA-style DMA
// channel to reserve).
//
#define HDA_REG_GCAP            0x00  // Global Capabilities (RO, 16-bit)
#define HDA_REG_VMIN             0x02  // Minor Version (RO, 8-bit)
#define HDA_REG_VMAJ             0x03  // Major Version (RO, 8-bit)
#define HDA_REG_OUTPAY           0x04  // Output Payload Capability (RO, 16-bit)
#define HDA_REG_INPAY            0x06  // Input Payload Capability (RO, 16-bit)
#define HDA_REG_GCTL             0x08  // Global Control (RW, 32-bit)
#define HDA_REG_WAKEEN           0x0C  // Wake Enable (RW, 16-bit)
#define HDA_REG_STATESTS         0x0E  // State Change Status (RWC, 16-bit) - bit N set = codec addr N present
#define HDA_REG_GSTS             0x10  // Global Status (RWC, 16-bit)
#define HDA_REG_INTCTL           0x20  // Interrupt Control (RW, 32-bit)
#define HDA_REG_INTSTS           0x24  // Interrupt Status (RWC, 32-bit)
#define HDA_REG_WALCLK           0x30  // Wall Clock Counter (RO, 32-bit)
#define HDA_REG_SSYNC            0x38  // Stream Synchronization (RW, 32-bit)
#define HDA_REG_CORBLBASE        0x40  // CORB Lower Base Address (RW, 32-bit)
#define HDA_REG_CORBUBASE        0x44  // CORB Upper Base Address (RW, 32-bit)
#define HDA_REG_CORBWP           0x48  // CORB Write Pointer (RW, 16-bit)
#define HDA_REG_CORBRP           0x4A  // CORB Read Pointer (RW, 16-bit)
#define HDA_REG_CORBCTL          0x4C  // CORB Control (RW, 8-bit)
#define HDA_REG_CORBSTS          0x4D  // CORB Status (RWC, 8-bit)
#define HDA_REG_CORBSIZE         0x4E  // CORB Size (RO/RW, 8-bit)
#define HDA_REG_RIRBLBASE        0x50  // RIRB Lower Base Address (RW, 32-bit)
#define HDA_REG_RIRBUBASE        0x54  // RIRB Upper Base Address (RW, 32-bit)
#define HDA_REG_RIRBWP           0x58  // RIRB Write Pointer (RW, 16-bit) - write 1 to bit15 to reset
#define HDA_REG_RINTCNT          0x5A  // Response Interrupt Count (RW, 16-bit)
#define HDA_REG_RIRBCTL          0x5C  // RIRB Control (RW, 8-bit)
#define HDA_REG_RIRBSTS          0x5D  // RIRB Status (RWC, 8-bit)
#define HDA_REG_RIRBSIZE         0x5E  // RIRB Size (RO/RW, 8-bit)
#define HDA_REG_IC               0x60  // Immediate Command (RW, 32-bit) - single-verb path, unused (we use CORB/RIRB)
#define HDA_REG_IR               0x64  // Immediate Response (RO, 32-bit)
#define HDA_REG_IRS              0x68  // Immediate Command Status (RW, 16-bit)
#define HDA_REG_DPLBASE          0x70  // DMA Position Lower Base (RW, 32-bit)
#define HDA_REG_DPUBASE          0x74  // DMA Position Upper Base (RW, 32-bit)

// Per-stream descriptor registers start at 0x80, 0x20 bytes apart.
// Stream index 0..N: input streams first, then output streams (counts from
// GCAP: bits [11:8]=ISS, [15:12]=OSS, [3:0]=BSS - read at init to know layout).
#define HDA_STREAM_REGBASE       0x80
#define HDA_STREAM_REGSIZE       0x20
#define HDA_SD_CTL(n)            (HDA_STREAM_REGBASE + (n) * HDA_STREAM_REGSIZE + 0x00)  // 24-bit (3 bytes)
#define HDA_SD_STS(n)            (HDA_STREAM_REGBASE + (n) * HDA_STREAM_REGSIZE + 0x03)  // 8-bit
#define HDA_SD_LPIB(n)           (HDA_STREAM_REGBASE + (n) * HDA_STREAM_REGSIZE + 0x04)  // 32-bit, link position in buffer
#define HDA_SD_CBL(n)            (HDA_STREAM_REGBASE + (n) * HDA_STREAM_REGSIZE + 0x08)  // 32-bit, cyclic buffer length
#define HDA_SD_LVI(n)            (HDA_STREAM_REGBASE + (n) * HDA_STREAM_REGSIZE + 0x0C)  // 16-bit, last valid BDL index
#define HDA_SD_FIFOW(n)          (HDA_STREAM_REGBASE + (n) * HDA_STREAM_REGSIZE + 0x0E)  // 16-bit, FIFO watermark (input only)
#define HDA_SD_FIFOSIZE(n)       (HDA_STREAM_REGBASE + (n) * HDA_STREAM_REGSIZE + 0x10)  // 16-bit
#define HDA_SD_FORMAT(n)         (HDA_STREAM_REGBASE + (n) * HDA_STREAM_REGSIZE + 0x12)  // 16-bit, stream format
#define HDA_SD_BDLPL(n)          (HDA_STREAM_REGBASE + (n) * HDA_STREAM_REGSIZE + 0x18)  // 32-bit, BDL lower base
#define HDA_SD_BDLPU(n)          (HDA_STREAM_REGBASE + (n) * HDA_STREAM_REGSIZE + 0x1C)  // 32-bit, BDL upper base

// GCTL bits
#define HDA_GCTL_CRST            0x00000001  // Controller Reset (write 0 then 1 to reset, poll until it reads back 1)
#define HDA_GCTL_UNSOL           0x00000100  // Accept Unsolicited Response Enable

// SDnCTL bits (lower byte)
#define HDA_SDCTL_SRST           0x00000001  // Stream Reset
#define HDA_SDCTL_RUN            0x00000002  // Stream Run
#define HDA_SDCTL_IOCE           0x00000004  // Interrupt On Completion Enable
#define HDA_SDCTL_FEIE           0x00000008  // FIFO Error Interrupt Enable
#define HDA_SDCTL_DEIE           0x00000010  // Descriptor Error Interrupt Enable
// upper bits of SDnCTL (byte 2, shifted <<16 when writing as 24-bit field): STRIPE, TP, DIR, STRM tag
#define HDA_SDCTL_DIR_OUTPUT     0x00800000  // 1 = output (render), 0 = input (capture) - only meaningful on bidirectional engines

// INTCTL/INTSTS bits - both registers share the same layout: bit 31 is the
// global enable/status bit, bit 30 is the controller (non-stream, e.g.
// CORB/RIRB) enable/status bit, and bits [29:0] are one enable/status bit
// per stream descriptor index (HDA_SD_STS(n)'s owning stream).
#define HDA_INTCTL_GIE           0x80000000  // Global Interrupt Enable
#define HDA_INTCTL_CIE           0x40000000  // Controller Interrupt Enable (CORB/RIRB etc.)
#define HDA_INTCTL_SIE(n)        (1UL << (n))  // Stream n Interrupt Enable
#define HDA_INTSTS_GIS           0x80000000  // Global Interrupt Status
#define HDA_INTSTS_CIS           0x40000000  // Controller Interrupt Status
#define HDA_INTSTS_SIS(n)        (1UL << (n))  // Stream n Interrupt Status

// SDnSTS bits (8-bit, RWC - write 1 to clear)
#define HDA_SDSTS_BCIS           0x04  // Buffer Completion Interrupt Status
#define HDA_SDSTS_FIFOE          0x08  // FIFO Error
#define HDA_SDSTS_DESE           0x10  // Descriptor Error

// CORB/RIRB control bits
#define HDA_CORBCTL_RUN          0x02        // CORB DMA Engine enable
#define HDA_CORBCTL_MEIE         0x01        // CORB Memory Error Interrupt Enable
#define HDA_RIRBCTL_RUN          0x02        // RIRB DMA Engine enable
#define HDA_RIRBCTL_RINTCTL      0x01        // Response Interrupt Control
#define HDA_RIRBCTL_RIRBOIC      0x04        // Response Overrun Interrupt Control

// CORB/RIRB size register: request "size 2" (256 entries) if supported (bit 6 of the
// capability nibble), else fall back based on what HW reports; we read back after
// writing to confirm what the HW actually granted, as the spec requires.
#define HDA_CORBSIZE_ENT_256     0x02
#define HDA_RIRBSIZE_ENT_256     0x02

//
// Verb encoding (standard HDA 32-bit "long form" verb, used over CORB):
//   bits [31:28] = codec address (0-15)
//   bits [27:20] = node ID (NID)
//   bits [19:8]  = verb (12-bit form) or [19:16]=4-bit verb + [15:0]=payload for
//                  the "12-bit verb + 8-bit payload" short form - see below.
// HDA verbs come in two encodings depending on the verb's ID range:
//   - 12-bit verb + 8-bit payload (most Set/Get parameter, connection select, etc.)
//   - 4-bit verb + 16-bit payload (only used by a few verbs, e.g. Set Processing
//     Coefficient); not needed for basic playback/init, so only the common form
//     is provided here.
//
#define HDA_MAKE_VERB(codec, nid, verb12, payload8) \
    ( ((ULONG)(codec) << 28) | ((ULONG)(nid) << 20) | ((ULONG)(verb12) << 8) | ((ULONG)(payload8) & 0xFF) )

#pragma pack(pop)

#endif // _HDAREGS_H_
