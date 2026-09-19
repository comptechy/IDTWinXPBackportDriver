/*****************************************************************************
 * dmachannel.h
 *****************************************************************************
 * CHdaDmaChannel: the IDmaChannel implementation WaveCyclic's NewStream()
 * hands back to PortCls (see wavecyclicminiport.cpp). This exists because
 * WaveCyclic (unlike WavePci, which the previous version of this driver
 * targeted) requires a real DMA-channel object exposing a single contiguous
 * buffer's system/physical address - it has no "return NULL, I manage my
 * own scatter/gather" escape hatch the way IMiniportWavePci::NewStream does.
 *
 * The real buffer, though, is not ours to allocate: the codec function
 * device owns no PCI resources of its own (see common.cpp's
 * AcquireBusInterface comment), so the actual audio buffer must come from
 * HDAUDIO_BUS_INTERFACE::AllocateDmaBuffer, which hands back a bus-driver-
 * owned MDL for a DMA engine already reserved via AllocateRenderDmaEngine/
 * AllocateCaptureDmaEngine (see wavecyclicstream.cpp's SetFormat). This
 * class is therefore a thin adapter: AllocateBuffer()/FreeBuffer() forward
 * to those bus-interface calls (via IHdaAdapterCommon - common.h/shared.h),
 * and SystemAddress()/PhysicalAddress() expose the resulting MDL's mapping,
 * so PortCls's own WaveCyclic buffer-copy logic reads/writes the exact same
 * memory the hardware DMA engine is programmed against.
 */

#ifndef _DMACHANNEL_H_
#define _DMACHANNEL_H_

#include "shared.h"

// Size of the cyclic DMA buffer this driver allocates per stream.
//
// Stage 5az - THIS NUMBER WAS THE SILENCE. It was 0x16000, copied from
// msvad's DMA_BUFFER_SIZE (msvad/msvad.h:61), and msvad is a virtual
// driver whose "hardware" never plays a sample, so the one property of
// that figure that matters here was never exercised where it came from.
//
// PortCls's WaveCyclic pin fills the cyclic buffer BEHIND the play
// cursor. On each service call it reads IDmaChannel::GetPosition(), works
// out how many bytes the hardware consumed since the last call, and
// copies exactly that many bytes into exactly the region they were
// consumed from. Audio handed to the pin now is therefore not heard until
// the cursor comes round again - the latency of a WaveCyclic stream is
// one full buffer lap, and the lap is the buffer's own duration.
//
// At 0x16000 the lap is 1.02 s at 22.05 kHz and 511 ms at 44.1 kHz, both
// longer than the system sounds this driver was being asked to play. Every
// one of them reached RUN -> PAUSE before the cursor wrapped back to its
// own data, so the hardware played the zeros the buffer was created with
// and nothing else. The Stage 5ay DMA scan caught it exactly: every silent
// session left a buffer full of real audio (peak |sample| up to 22679),
// and in every single one the bytes written came to slightly LESS than one
// lap - 94%, 86%, 84%, 45%, 38%, never 100%. kstest was audible throughout
// for the same reason read the other way: its tone runs for seconds, so it
// survived the first lap and everything after it was heard.
//
// 0x4000 is what the WDK 7600 samples that drive real hardware use, and it
// puts the latency back in the normal WaveCyclic range: 85 ms at 48 kHz,
// 93 ms at 44.1 kHz, 186 ms at 22.05 kHz. It stays an exact multiple of
// the 128-byte granule HDA works in (0x4000 / 128 = 128) and of PAGE_SIZE,
// and at a 10 ms notification interval it is still 8 to 18 service calls
// deep, so there is ample margin against DPC jitter.
//
// It must stay CONSTANT across re-allocations. PortCls reads BufferSize()
// once when the pin is created and caches it, while SetFormat() tears the
// buffer down and builds it again later (PortCls re-negotiates the format
// after NewStream returns - see the log), so a size computed from the
// negotiated format could hand PortCls a buffer smaller than the one it
// still believes it owns. MaximumBufferSize() reports this same value, as
// the samples' implementation does.
const ULONG HDA_MAX_DMA_BUFFER_SIZE = 0x4000;

class CHdaDmaChannel : public IDmaChannel, public CUnknown
{
private:
    PADAPTERCOMMON  m_pAdapterCommon;

    // Set by CMiniportWaveCyclicStreamHda::SetFormat once it has allocated
    // the DMA engine handle for this stream's chosen format (engine
    // allocation needs the format up front - see hdaudio.h's
    // AllocateRenderDmaEngine/AllocateCaptureDmaEngine - so it cannot happen
    // any earlier than SetFormat).
    //
    // Stage 5an: the rest of that sentence used to read "...which always
    // precedes PortCls calling AllocateBuffer() on the WaveCyclic
    // pin-creation sequence". That was simply false, and it cost a
    // bugcheck. PortCls NEVER calls IDmaChannel::AllocateBuffer - the
    // miniport allocates its own cyclic buffer, exactly as the WDK 7600
    // samples do (sb16/minwave.cpp:227 and :261 from the miniport's Init,
    // msvad/basewave.cpp:597 from the stream's Init). Waiting for a call
    // that never came left m_RequestedSize at zero for the life of the
    // stream; see CMiniportWaveCyclicStreamHda::Init, which now allocates.
    HANDLE          m_EngineHandle;
    BOOLEAN         m_bEngineHandleValid;

    PMDL            m_pBufferMdl;       // bus-driver-owned, from AllocateDmaBuffer
    PVOID           m_pSystemAddress;   // MmGetSystemAddressForMdlSafe(m_pBufferMdl)
    SIZE_T          m_AllocatedSize;    // actual size the bus driver granted
    ULONG           m_RequestedSize;    // BufferSize()/SetBufferSize() logical value
    UCHAR           m_StreamId;
    ULONG           m_FifoSize;

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CHdaDmaChannel);
    ~CHdaDmaChannel();

    NTSTATUS Init(IN PADAPTERCOMMON AdapterCommon);

    // Called by CMiniportWaveCyclicStreamHda once it has a live engine
    // handle (see this class's header comment above).
    // Stage 5ap: a NULL handle INVALIDATES this channel rather than
    // marking a NULL handle valid. CMiniportWaveCyclicStreamHda calls
    // SetEngineHandle(NULL) the instant it frees the engine, so that a
    // later FreeBuffer() - from this class's own destructor, say -
    // cannot hand a dead engine handle back to hdaudbus.
    void SetEngineHandle(IN HANDLE Handle)
    {
        m_EngineHandle = Handle;
        m_bEngineHandleValid = (Handle != NULL);
    }

    UCHAR GetStreamId(void) { return m_StreamId; }

    IMP_IDmaChannel;
};

#endif // _DMACHANNEL_H_
