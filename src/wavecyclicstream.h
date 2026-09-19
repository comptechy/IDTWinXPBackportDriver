/*****************************************************************************
 * wavecyclicstream.h
 *****************************************************************************
 * One WaveCyclic stream (one direction: render or capture). Owns one HDA
 * DMA-engine HANDLE (from HDAUDIO_BUS_INTERFACE::AllocateRenderDmaEngine/
 * AllocateCaptureDmaEngine, via IHdaAdapterCommon) and the CHdaDmaChannel
 * object PortCls uses to move audio into/out of that engine's bus-driver-
 * owned buffer.
 *
 * The base HDAUDIO_BUS_INTERFACE DDI exposes no per-stream completion
 * interrupt (GetLinkPositionRegister only hands back a pointer to a live
 * position register to poll on demand; RegisterEventCallback is for
 * unsolicited codec responses, not stream completion) - so, mirroring the
 * classic WDK 7600 toneclick sample's software-timer-driven servicing
 * pattern (used whenever there's no real hardware interrupt to synchronize
 * against), each stream drives its own KTIMER+KDPC pair while running,
 * calling IPortWaveCyclic::Notify() at the cadence SetNotificationFreq()
 * requested. PortCls then calls back into GetPosition()/the port's own
 * buffer-copy logic from DPC level in response to that Notify().
 */

#ifndef _WAVECYCLICSTREAM_H_
#define _WAVECYCLICSTREAM_H_

#include "shared.h"

class CMiniportWaveCyclicHda;
class CHdaDmaChannel;

class CMiniportWaveCyclicStreamHda : public IMiniportWaveCyclicStream, public CUnknown
{
private:
    CMiniportWaveCyclicHda *   m_pMiniport;
    PADAPTERCOMMON              m_pAdapterCommon;
    CHdaDmaChannel *            m_pDmaChannel;      // AddRef'd, released in destructor
    BOOLEAN                     m_bCapture;

    HANDLE                      m_EngineHandle;
    BOOLEAN                     m_bEngineHandleValid;

    HDAUDIO_STREAM_FORMAT       m_StreamFormat;
    HDAUDIO_CONVERTER_FORMAT    m_ConverterFormat;
    ULONG                       m_SampleRate;
    ULONG                       m_Channels;
    ULONG                       m_BitsPerSample;

    ULONG                       m_NotificationInterval; // ms, from SetNotificationFreq
    KSSTATE                     m_CurrentState;

    PSERVICEGROUP               m_pServiceGroup;

    // Position register, per GetLinkPositionRegister's real semantics: a
    // pointer TO a live/mapped register, re-read directly each time fresh
    // position data is needed (see shared.h's IHdaAdapterCommon comment) -
    // not re-queried through the bus interface on every GetPosition() call.
    PULONG                      m_pPositionRegister;
    BOOLEAN                     m_bPositionRegisterValid;

    // Software servicing timer - see this class's header comment above for
    // why there's no real hardware interrupt to drive Notify() from.
    KTIMER                      m_Timer;
    KDPC                        m_Dpc;
    BOOLEAN                     m_bTimerStarted;

    static VOID TimerDpcRoutine
    (
        IN  PKDPC   Dpc,
        IN  PVOID   DeferredContext,
        IN  PVOID   SystemArgument1,
        IN  PVOID   SystemArgument2
    );

    NTSTATUS StartEngine(void);
    void StopEngine(void);

    // Stage 5ap: hand this stream's HDA DMA engine back to the bus
    // driver, buffer first and engine second, and invalidate every
    // cached copy of the handle. PASSIVE_LEVEL only.
    void ReleaseEngine(void);

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveCyclicStreamHda);
    ~CMiniportWaveCyclicStreamHda();

    NTSTATUS Init
    (
        IN  CMiniportWaveCyclicHda *   Miniport,
        IN  PADAPTERCOMMON              AdapterCommon,
        IN  BOOLEAN                     Capture,
        IN  CHdaDmaChannel *            DmaChannel,
        IN  PKSDATAFORMAT               DataFormat,
        OUT PSERVICEGROUP *             OutServiceGroup
    );

    IMP_IMiniportWaveCyclicStream;
};

#endif // _WAVECYCLICSTREAM_H_
