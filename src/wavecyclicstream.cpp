/*****************************************************************************
 * wavecyclicstream.cpp
 *****************************************************************************
 * See wavecyclicstream.h.
 */

static char STR_MODULENAME[] = "stwrtxp WaveCyclicStream: ";
#include "wavecyclicstream.h"
#include "wavecyclicminiport.h"
#include "dmachannel.h"

#pragma code_seg("PAGE")

CMiniportWaveCyclicStreamHda::~CMiniportWaveCyclicStreamHda()
{
    PAGED_CODE();

    // Stage 5ap: trace destruction unconditionally. Nothing in this
    // object's teardown path logged anything a reader could rely on -
    // StopEngine()'s only DOUT sits inside its "engine still valid"
    // branch - so the logs could not answer whether PortCls was
    // destroying streams at all, which is the first thing you need to
    // know when hardware resources go missing.
    DOUT(DBG_PRINT, ("~Stream: destroying (capture %u, engine %p)",
        m_bCapture, m_EngineHandle));

    StopEngine();

    // Stage 5am: StopEngine()'s KeCancelTimer() only stops FUTURE
    // expirations. If the timer had already expired, its DPC is either
    // sitting in some processor's DPC queue or actively running on another
    // processor, and KeCancelTimer neither dequeues it nor waits for it.
    // StopEngine() now calls KeRemoveQueueDpc() for the queued case; this
    // flush covers the already-running case. Without both, TimerDpcRoutine
    // dereferences m_pMiniport / m_pServiceGroup on this object after the
    // pool block below has been freed - the same use-after-free shape as
    // the Stage 5ak 0x3B, but driven by a timer instead of a refcount.
    // Legal here because this destructor is PASSIVE_LEVEL (PAGED_CODE
    // above); KeFlushQueuedDpcs requires PASSIVE_LEVEL.
    KeFlushQueuedDpcs();

    ReleaseEngine();

    if (m_pDmaChannel)
    {
        m_pDmaChannel->Release();
        m_pDmaChannel = NULL;
    }

    if (m_pServiceGroup)
    {
        m_pServiceGroup->Release();
        m_pServiceGroup = NULL;
    }

    if (m_pAdapterCommon)
    {
        m_pAdapterCommon->Release();
        m_pAdapterCommon = NULL;
    }
}

/*****************************************************************************
 * CMiniportWaveCyclicStreamHda::NonDelegatingQueryInterface
 *****************************************************************************
 */
STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStreamHda::NonDelegatingQueryInterface
(
    IN  REFIID  Interface,
    OUT PVOID * Object
)
{
    PAGED_CODE();
    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = (PVOID)(PUNKNOWN)(PMINIPORTWAVECYCLICSTREAM)this;
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveCyclicStream))
    {
        *Object = (PVOID)(PMINIPORTWAVECYCLICSTREAM)this;
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }

    ((PUNKNOWN)(*Object))->AddRef();
    return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveCyclicStreamHda::Init
(
    IN  CMiniportWaveCyclicHda *   Miniport,
    IN  PADAPTERCOMMON              AdapterCommon,
    IN  BOOLEAN                     Capture,
    IN  CHdaDmaChannel *            DmaChannel,
    IN  PKSDATAFORMAT               DataFormat,
    OUT PSERVICEGROUP *             OutServiceGroup
)
{
    PAGED_CODE();

    if (!Miniport || !AdapterCommon || !DmaChannel || !OutServiceGroup)
        return STATUS_INVALID_PARAMETER;

    m_pMiniport = Miniport;
    m_pAdapterCommon = AdapterCommon;
    m_pAdapterCommon->AddRef();
    m_pDmaChannel = DmaChannel;
    // Stage 5ak: take our OWN reference on the DMA channel.
    //
    // The previous comment here claimed the caller's reference was
    // "transferred to us". It is not. NewStream() takes exactly one
    // AddRef() on the channel and then hands that single reference to
    // PortCls through its *DmaChannel OUT parameter - that reference
    // belongs to the port driver, not to this stream. Without the AddRef
    // below, ~CMiniportWaveCyclicStreamHda's m_pDmaChannel->Release()
    // drops PortCls's reference, the channel is destructed and freed
    // while PortCls still holds the pointer, and PortCls's own later
    // Release() calls through the freed object's vtable pointer.
    // Confirmed as bugcheck 0x3B in the 2026-09-08 dump: a 96-byte
    // 'wNcP' block, refcount 1, vptr already rewound to
    // CUnknown::'vftable' by ~CUnknown(). Compare m_pServiceGroup below,
    // which gets this right: it keeps its own reference and AddRef()s a
    // second one for its OUT parameter.
    m_pDmaChannel->AddRef();
    m_bCapture = Capture;

    m_EngineHandle = NULL;
    m_bEngineHandleValid = FALSE;
    RtlZeroMemory(&m_StreamFormat, sizeof(m_StreamFormat));
    RtlZeroMemory(&m_ConverterFormat, sizeof(m_ConverterFormat));

    // Conservative defaults (16-bit stereo 44.1kHz) until SetFormat() runs;
    // SetNotificationFreq() may legally be called before SetFormat() by
    // PortCls, and needs something reasonable to compute FrameSize from.
    m_SampleRate = 44100;
    m_Channels = 2;
    m_BitsPerSample = 16;

    m_NotificationInterval = 0;
    m_CurrentState = KSSTATE_STOP;

    m_pPositionRegister = NULL;
    m_bPositionRegisterValid = FALSE;

    m_bTimerStarted = FALSE;
    KeInitializeTimer(&m_Timer);
    KeInitializeDpc(&m_Dpc, TimerDpcRoutine, (PVOID)this);

    NTSTATUS status = PcNewServiceGroup(&m_pServiceGroup, NULL);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("Init: PcNewServiceGroup failed, status=%08X", status));
        m_pAdapterCommon->Release();
        m_pAdapterCommon = NULL;
        return status;
    }

    if (!DataFormat)
    {
        // WaveCyclic's NewStream contract always supplies the negotiated
        // format, and without one there is no DMA engine and so no buffer.
        DOUT(DBG_ERROR, ("Init: no DataFormat supplied"));
        return STATUS_INVALID_PARAMETER;
    }

    // Stage 5an: this used to warn and continue on failure, on the theory
    // that "PortCls should call SetFormat again before RUN". It does not,
    // and a stream with no DMA engine cannot allocate a buffer - which is
    // how a half-built stream reached PortCls in the first place. Fail.
    status = SetFormat(DataFormat);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("Init: SetFormat failed, status=%08X", status));
        return status;
    }

    // Stage 5an: allocate this stream's cyclic DMA buffer HERE.
    //
    // PortCls does not do it. IDmaChannel::AllocateBuffer is a method the
    // miniport calls on its own channel, exactly as the WDK 7600 samples
    // do - sb16/minwave.cpp:227 and :261 from the miniport's Init, and
    // msvad/basewave.cpp:597 from the stream's Init, which is the shape
    // copied here. This driver assumed the opposite and waited for a call
    // that never came, so IDmaChannel::BufferSize() returned 0 for the
    // life of every stream. The first IOCTL_KS_WRITE_STREAM then bugchecked
    // PortCls inside its own cyclic-buffer arithmetic with
    // STATUS_INTEGER_DIVIDE_BY_ZERO - 0x3B / c0000094 at portcls+0x19a2,
    // 'div eax,r13d' with r13d holding that zero buffer size (confirmed in
    // the 2026-09-08 16:10 dump).
    //
    // Note the ordering dependency: SetFormat above is what allocates the
    // HDA engine handle and hands it to the channel via SetEngineHandle(),
    // and CHdaDmaChannel::AllocateBuffer refuses to run without it.
    status = m_pDmaChannel->AllocateBuffer(HDA_MAX_DMA_BUFFER_SIZE, NULL);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("Init: AllocateBuffer failed, status=%08X", status));
        return status;
    }

    // Both early returns above leave cleanup to ~CMiniportWaveCyclicStreamHda:
    // NewStream() releases this stream on Init failure, and the destructor
    // releases m_pDmaChannel, m_pServiceGroup and m_pAdapterCommon.

    *OutServiceGroup = m_pServiceGroup;
    m_pServiceGroup->AddRef();

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * SetFormat
 *****************************************************************************
 * Allocates (or re-allocates) this stream's DMA engine for the negotiated
 * format, then hands the resulting engine HANDLE to the CHdaDmaChannel so
 * its later AllocateBuffer() call (PortCls's own buffer-creation step,
 * which always follows SetFormat in the WaveCyclic pin-creation sequence)
 * targets the right engine.
 */
STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStreamHda::SetFormat
(
    IN  PKSDATAFORMAT   DataFormat
)
{
    PAGED_CODE();

    if (!DataFormat)
        return STATUS_INVALID_PARAMETER;

    // Stage 5bc: bound the cast before making it. Everything below
    // reads a WAVEFORMATEX out of this buffer, including the trace.
    if (DataFormat->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX))
    {
        DOUT(DBG_ERROR, ("SetFormat: FormatSize %u is too small for a "
            "KSDATAFORMAT_WAVEFORMATEX", DataFormat->FormatSize));
        return STATUS_INVALID_PARAMETER;
    }

    PKSDATAFORMAT_WAVEFORMATEX pFormatEx = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;
    PWAVEFORMATEX pWfx = &pFormatEx->WaveFormatEx;

    // Stage 5az: log the request BEFORE anything can reject it. The old
    // trace only printed a format once it had already been accepted, so
    // the fifteen consecutive "AllocateRenderDmaEngine failed,
    // status=C000000D" sessions in the Stage 5ay log name no format at
    // all and there is no way to tell which one the codec would not take.
    // Stage 5bc moved it above the new rate guard for the same reason.
    DOUT(DBG_STREAM, ("SetFormat: requested %u Hz, %u ch, %u bit (capture %u)",
        pWfx->nSamplesPerSec, pWfx->nChannels, pWfx->wBitsPerSample,
        m_bCapture ? 1 : 0));

    // Stage 5bc: refuse a rate the codec cannot produce, and refuse it
    // HERE - before hadBuffer, before the saved-format bookkeeping and
    // above all before ReleaseEngine(). A rejected format change must
    // cost the caller its SetFormat and nothing else; a stream that was
    // already running keeps its engine, its buffer and its format, and
    // the Stage 5az rollback path below never even has to be reached.
    //
    // This is what stops 22050 Hz. Nothing above the miniport was ever
    // going to: PortCls hands a KSPROPERTY_CONNECTION_DATAFORMAT set
    // straight through without re-checking the pin's data ranges, and
    // the bus driver's AllocateRenderDmaEngine cheerfully computes a
    // 0x4111 converter format for a DAC that has no 22.05 kHz divisor.
    NTSTATUS validation = m_pMiniport->ValidateFormat(DataFormat);

    if (!NT_SUCCESS(validation))
    {
        DOUT(DBG_STREAM, ("SetFormat: refused %u Hz, %u ch, %u bit - the "
            "stream keeps the format it already had, status=%08X",
            pWfx->nSamplesPerSec, pWfx->nChannels, pWfx->wBitsPerSample,
            validation));
        return validation;
    }

    // Release any previously-allocated engine before allocating a new one
    // (SetFormat can legally be called more than once on a stream).
    //
    // Stage 5ap: this had the same inverted teardown order as the
    // destructor and leaked an engine on every format change; go
    // through ReleaseEngine(), which frees the buffer first. Remember
    // whether a buffer existed, because ReleaseEngine() takes it with
    // the engine and a re-format has to build a new one.
    BOOLEAN hadBuffer = (BOOLEAN)(m_pDmaChannel->AllocatedBufferSize() != 0);

    // Stage 5az: remember the format being left behind. ReleaseEngine()
    // below tears down a perfectly good engine before we find out whether
    // the replacement is one the codec will accept, and when it is not,
    // every later call on the pin - SetState, GetPosition, the DMA scan -
    // runs against a stream with no engine and a zero-length buffer, which
    // is exactly the shape of those fifteen dead sessions. SetFormat is
    // allowed to fail; it is not allowed to destroy the format that was
    // already working.
    HDAUDIO_STREAM_FORMAT   savedFormat  = m_StreamFormat;
    ULONG                   savedRate    = m_SampleRate;
    ULONG                   savedChans   = m_Channels;
    ULONG                   savedBits    = m_BitsPerSample;
    BOOLEAN                 hadEngine    = m_bEngineHandleValid;

    ReleaseEngine();

    m_SampleRate = pWfx->nSamplesPerSec;
    m_Channels = pWfx->nChannels;
    m_BitsPerSample = pWfx->wBitsPerSample;

    m_StreamFormat.SampleRate = m_SampleRate;
    m_StreamFormat.ValidBitsPerSample = (USHORT)m_BitsPerSample;
    m_StreamFormat.ContainerSize = (USHORT)m_BitsPerSample;
    m_StreamFormat.NumberOfChannels = (USHORT)m_Channels;

    HANDLE handle = NULL;
    NTSTATUS status;

    if (m_bCapture)
        status = m_pAdapterCommon->AllocateCaptureDmaEngine(&m_StreamFormat,
            &handle, &m_ConverterFormat);
    else
        status = m_pAdapterCommon->AllocateRenderDmaEngine(&m_StreamFormat,
            &handle, &m_ConverterFormat);

    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("SetFormat: Allocate%sDmaEngine failed, status=%08X",
            m_bCapture ? "Capture" : "Render", status));

        // Stage 5az: put the working format back, so a rejected format
        // change costs the caller its SetFormat and nothing else.
        if (hadEngine)
        {
            m_StreamFormat  = savedFormat;
            m_SampleRate    = savedRate;
            m_Channels      = savedChans;
            m_BitsPerSample = savedBits;

            HANDLE   restored = NULL;
            NTSTATUS restoreStatus;

            if (m_bCapture)
                restoreStatus = m_pAdapterCommon->AllocateCaptureDmaEngine(
                    &m_StreamFormat, &restored, &m_ConverterFormat);
            else
                restoreStatus = m_pAdapterCommon->AllocateRenderDmaEngine(
                    &m_StreamFormat, &restored, &m_ConverterFormat);

            if (NT_SUCCESS(restoreStatus) && restored)
            {
                m_EngineHandle = restored;
                m_bEngineHandleValid = TRUE;
                m_pDmaChannel->SetEngineHandle(restored);

                if (hadBuffer)
                    m_pDmaChannel->AllocateBuffer(HDA_MAX_DMA_BUFFER_SIZE, NULL);

                DOUT(DBG_PRINT, ("SetFormat: rolled back to %u Hz, %u ch, "
                    "%u bit - the pin stays usable", m_SampleRate, m_Channels,
                    m_BitsPerSample));
            }
            else
            {
                DOUT(DBG_ERROR, ("SetFormat: rollback failed too, status=%08X "
                    "- this pin is now dead", restoreStatus));
            }
        }

        return status;
    }

    m_EngineHandle = handle;
    m_bEngineHandleValid = TRUE;
    m_pDmaChannel->SetEngineHandle(handle);

    // Stage 5ap: restore the cyclic buffer ReleaseEngine() just took.
    // Without this a second SetFormat() would drop IDmaChannel::
    // BufferSize() back to zero and PortCls would divide by it - the
    // Stage 5an bugcheck, re-armed. On the first call through Init()
    // hadBuffer is FALSE and Init()'s own AllocateBuffer() does the work.
    if (hadBuffer)
    {
        NTSTATUS bufStatus =
            m_pDmaChannel->AllocateBuffer(HDA_MAX_DMA_BUFFER_SIZE, NULL);
        if (!NT_SUCCESS(bufStatus))
        {
            DOUT(DBG_ERROR, ("SetFormat: re-AllocateBuffer failed, status=%08X",
                bufStatus));
            return bufStatus;
        }
    }

    DOUT(DBG_STREAM, ("SetFormat: %u Hz, %u ch, %u bit -> engine handle %p",
        m_SampleRate, m_Channels, m_BitsPerSample, handle));

    return STATUS_SUCCESS;
}

STDMETHODIMP_(ULONG) CMiniportWaveCyclicStreamHda::SetNotificationFreq
(
    IN  ULONG   Interval,
    OUT PULONG  FrameSize
)
{
    PAGED_CODE();

    m_NotificationInterval = Interval;

    if (FrameSize)
    {
        ULONG bytesPerSample = (m_BitsPerSample + 7) / 8;
        ULONG bytesPerFrame = bytesPerSample * m_Channels;
        ULONG bytesPerSecond = bytesPerFrame * m_SampleRate;

        // Interval is in milliseconds, per the WaveCyclic contract.
        *FrameSize = (ULONG)(((ULONGLONG)bytesPerSecond * Interval) / 1000);

        // Round to a whole sample frame so the ring buffer never splits one.
        if (bytesPerFrame)
            *FrameSize -= (*FrameSize % bytesPerFrame);
    }

    return Interval;
}

/*****************************************************************************
 * ReleaseEngine
 *****************************************************************************
 * Stage 5ap: give this stream's HDA DMA engine back to the bus driver, in
 * the order hdaudio.h's contract requires - BUFFER FIRST, ENGINE SECOND.
 *
 * This is the fix for the "AllocateRenderDmaEngine failed, status=C000009A"
 * (STATUS_INSUFFICIENT_RESOURCES / Win32 1450) wall that stopped every pin
 * from opening. The destructor used to call FreeDmaEngine() and only
 * afterwards release m_pDmaChannel, whose own destructor then called
 * FreeDmaBuffer() on the engine we had just freed. Freeing an engine that
 * still owns a DMA buffer does not work - and the only thing that said so
 * was the NTSTATUS both call sites discarded, even though hdaudio.h marks
 * PFREE_DMA_ENGINE __checkReturn. The engine was simply gone.
 *
 * The evidence is unambiguous in the Stage 5an log: eight streams took
 * engine handles ...7000 through ...7007 and stream tags 1,2,3,4 on each
 * side, marching upward and never once reusing a slot, until the
 * controller's four render and four capture engines were all consumed.
 * Every pin ever created cost one engine permanently. hdaudbus only
 * reclaims them when the bus interface is finally dereferenced, so the
 * shortage outlived the driver: the next load's very first pin failed.
 *
 * PASSIVE_LEVEL only - called from the destructor and from SetFormat().
 */
void CMiniportWaveCyclicStreamHda::ReleaseEngine(void)
{
    PAGED_CODE();

    if (!m_bEngineHandleValid)
        return;

    // Stage 5ar: RESET the engine before freeing anything.
    //
    // hdaudio.h:143 declares
    //     ResetState = 0, StopState = 1, PauseState = 1, RunState = 2
    // - StopState and PauseState are THE SAME VALUE. StopEngine()'s
    // SetDmaEngineState(StopState) therefore only pauses the stream; it
    // does not reset it. hdaudbus refuses both FreeDmaBuffer and
    // FreeDmaEngine on an engine that is not in the reset state, which is
    // why Stage 5ap's ordering fix on its own still left every single
    // FreeDmaEngine returning STATUS_INVALID_DEVICE_REQUEST (C0000010)
    // and every engine leaked. Worse, the destructor's StopEngine() call
    // actively undid the reset that SetState(KSSTATE_STOP) had just done,
    // so even a cleanly stopped stream arrived here paused.
    if (m_pAdapterCommon)
    {
        HANDLE resetHandles[1] = { m_EngineHandle };
        NTSTATUS resetStatus =
            m_pAdapterCommon->SetDmaEngineState(ResetState, 1, resetHandles);
        if (!NT_SUCCESS(resetStatus))
        {
            DOUT(DBG_ERROR, ("ReleaseEngine: SetDmaEngineState(Reset) on %p "
                "FAILED, status=%08X - the frees below will be refused",
                m_EngineHandle, resetStatus));
        }
    }

    // Buffer next. FreeBuffer() is idempotent and a no-op on a stream that
    // never got as far as allocating one.
    if (m_pDmaChannel)
        m_pDmaChannel->FreeBuffer();

    if (m_pAdapterCommon)
    {
        NTSTATUS status = m_pAdapterCommon->FreeDmaEngine(m_EngineHandle);
        if (NT_SUCCESS(status))
        {
            DOUT(DBG_PRINT, ("ReleaseEngine: freed engine %p", m_EngineHandle));
        }
        else
        {
            DOUT(DBG_ERROR, ("ReleaseEngine: FreeDmaEngine(%p) FAILED, "
                "status=%08X - the controller has lost this engine until the "
                "bus interface is dereferenced", m_EngineHandle, status));
        }
    }

    m_EngineHandle = NULL;
    m_bEngineHandleValid = FALSE;
    m_bPositionRegisterValid = FALSE;
    m_pPositionRegister = NULL;

    // Stop the channel handing the dead handle back to hdaudbus later.
    if (m_pDmaChannel)
        m_pDmaChannel->SetEngineHandle(NULL);
}

/*****************************************************************************
 * StartEngine / StopEngine
 *****************************************************************************
 */
NTSTATUS CMiniportWaveCyclicStreamHda::StartEngine(void)
{
    PAGED_CODE();

    if (!m_bEngineHandleValid)
        return STATUS_DEVICE_NOT_READY;

    // Stage 5ao: attach the codec's DAC to this stream BEFORE the DMA
    // engine starts pushing frames at it.
    //
    // Up to Stage 5an nothing ever did this, and it is the whole reason
    // the driver was silent: AllocateRenderDmaEngine/AllocateDmaBuffer
    // build the controller side and assign a stream tag, but the codec
    // side is the function driver's job. A converter that has not been
    // sent SET_CONVERTER_FORMAT and SET_CHAN_STREAMID ignores the link
    // traffic completely - no error, no timeout, just silence.
    //
    // Done here rather than in Init() so that only streams that actually
    // run touch the codec. Several processes probe this filter by
    // creating a pin and closing it again without ever reaching RUN
    // (visible all over stwrtxp_log.txt); binding at Init would have them
    // fighting over the one shared DAC.
    //
    // Capture is deliberately not bound - this backport is playback-only,
    // and the ADC-side equivalent needs the recording pin setup that
    // InitCodec() explicitly leaves out of scope.
    if (!m_bCapture)
    {
        NTSTATUS bindStatus = m_pAdapterCommon->BindRenderConverters(
            m_ConverterFormat.ConverterFormat, m_pDmaChannel->GetStreamId());
        if (!NT_SUCCESS(bindStatus))
        {
            DOUT(DBG_ERROR, ("StartEngine: BindRenderConverters failed, "
                "status=%08X - starting DMA anyway, output will be silent",
                bindStatus));
        }
    }

    HANDLE handles[1] = { m_EngineHandle };
    NTSTATUS status = m_pAdapterCommon->SetDmaEngineState(RunState, 1, handles);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("StartEngine: SetDmaEngineState(Run) failed, status=%08X",
            status));
        return status;
    }

    DOUT(DBG_PRINT, ("StartEngine: running, stream tag %u, converter format %04X, "
        "notification interval %u ms", m_pDmaChannel->GetStreamId(),
        m_ConverterFormat.ConverterFormat, m_NotificationInterval));

    if (!m_bTimerStarted && m_NotificationInterval > 0)
    {
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -(LONGLONG)m_NotificationInterval * 10000; // ms -> 100ns, relative
        KeSetTimerEx(&m_Timer, dueTime, m_NotificationInterval, &m_Dpc);
        m_bTimerStarted = TRUE;
    }

    return STATUS_SUCCESS;
}

void CMiniportWaveCyclicStreamHda::StopEngine(void)
{
    PAGED_CODE();

    if (m_bTimerStarted)
    {
        KeCancelTimer(&m_Timer);
        // KeCancelTimer returns FALSE - and does nothing else - when the
        // timer has already expired and queued its DPC. Pull that DPC back
        // out explicitly; the destructor's KeFlushQueuedDpcs() covers the
        // remaining case where it is already running on another processor.
        KeRemoveQueueDpc(&m_Dpc);
        m_bTimerStarted = FALSE;
    }

    if (m_bEngineHandleValid && m_pAdapterCommon)
    {
        // NB (Stage 5ar): hdaudio.h aliases StopState and PauseState to
        // the same value (1), so this PAUSES the DMA engine - it does not
        // reset it, and an unreset engine can have neither its buffer nor
        // itself freed. The reset lives in ReleaseEngine() and in
        // SetState(KSSTATE_STOP).
        HANDLE handles[1] = { m_EngineHandle };
        NTSTATUS stopStatus =
            m_pAdapterCommon->SetDmaEngineState(StopState, 1, handles);
        if (!NT_SUCCESS(stopStatus))
        {
            DOUT(DBG_ERROR, ("StopEngine: SetDmaEngineState(Stop) FAILED, "
                "status=%08X", stopStatus));
        }

        // Stage 5ao: release the DAC again (stream tag 0 = "not in use"),
        // so the next stream to run can claim it. Order matters: the DMA
        // engine is stopped first, so the converter is never detached from
        // a stream that is still feeding it.
        if (!m_bCapture)
            m_pAdapterCommon->BindRenderConverters(0, 0);

        DOUT(DBG_PRINT, ("StopEngine: stopped"));
    }
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStreamHda::SetState
(
    IN  KSSTATE State
)
{
    PAGED_CODE();

    NTSTATUS status = STATUS_SUCCESS;

    // Stage 5ao: SetState, StartEngine, StopEngine and the servicing DPC
    // had no tracing at all, so the Stage 5an log went silent after
    // "NewStream: success" and could not answer whether the stream ever
    // reached RUN. It can now.
    static const CHAR * const stateNames[] =
        { "STOP", "ACQUIRE", "PAUSE", "RUN" };

    DOUT(DBG_PRINT, ("SetState: %s -> %s (capture %u)",
        (m_CurrentState <= KSSTATE_RUN) ? stateNames[m_CurrentState] : "?",
        (State <= KSSTATE_RUN) ? stateNames[State] : "?",
        m_bCapture));

    if (State == m_CurrentState)
        return STATUS_SUCCESS;

    switch (State)
    {
    case KSSTATE_RUN:
        status = StartEngine();
        break;

    case KSSTATE_PAUSE:
        // Hardware stop (StopState doubles as HDA's pause/suspend state -
        // see hdaudio.h's HDAUDIO_STREAM_STATE comment) without tearing
        // down the engine handle - RUN can resume directly from here.
        StopEngine();
        break;

    case KSSTATE_ACQUIRE:
        break;

    case KSSTATE_STOP:
        StopEngine();
        if (m_bEngineHandleValid)
        {
            HANDLE handles[1] = { m_EngineHandle };
            m_pAdapterCommon->SetDmaEngineState(ResetState, 1, handles);
        }
        m_bPositionRegisterValid = FALSE;
        break;
    }

    m_CurrentState = State;
    return status;
}

// ---------------------------------------------------------------------------
// Stage 5am: everything from here to the end of the file must be NON-PAGED.
//
// PortCls calls GetPosition(), NormalizePhysicalPosition() and Silence() at
// IRQL DISPATCH_LEVEL while servicing a running WaveCyclic stream, and
// TimerDpcRoutine() is itself a DPC. Touching pageable code at DISPATCH_LEVEL
// bugchecks (0xD1 / 0x50 / 0xA) whenever that page happens to have been
// trimmed. Note that none of these three carry PAGED_CODE() - the author knew
// they run raised; only the file-wide code_seg("PAGE") above put them in the
// pageable section. Both WDK 7600 WaveCyclic samples bracket exactly these
// routines out of their PAGE segment: sb16/minwave.cpp resets at line 1669,
// just before GetPosition/NormalizePhysicalPosition/Silence, and
// msvad/basewave.cpp resets at 671 (GetPosition, NormalizePhysicalPosition)
// and again at 1008 (Silence). SetFormat/SetState/SetNotificationFreq stay
// paged above - those are PASSIVE_LEVEL only and do carry PAGED_CODE().
// ---------------------------------------------------------------------------
#pragma code_seg()

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStreamHda::GetPosition
(
    OUT PULONG  Position
)
{
    if (!Position)
        return STATUS_INVALID_PARAMETER;

    if (!m_bEngineHandleValid)
    {
        *Position = 0;
        return STATUS_SUCCESS;
    }

    if (!m_bPositionRegisterValid)
    {
        NTSTATUS status = m_pAdapterCommon->GetLinkPositionRegister(
            m_EngineHandle, &m_pPositionRegister);
        if (!NT_SUCCESS(status) || !m_pPositionRegister)
        {
            *Position = 0;
            return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;
        }
        m_bPositionRegisterValid = TRUE;
    }

    // Per HDAUDIO_BUS_INTERFACE's real semantics, this is a pointer to a
    // live/mapped register - re-read it directly rather than re-querying
    // the bus interface each call (see shared.h's IHdaAdapterCommon comment).
    ULONG pos = READ_REGISTER_ULONG(m_pPositionRegister);

    // Stage 5am: the link position register counts within the buffer the BUS
    // DRIVER allocated, and AllocateDmaBuffer is free to round that UP from
    // what PortCls asked for. PortCls indexes its cyclic buffer by whatever
    // IDmaChannel::BufferSize() reports, so a raw LPIB value can legitimately
    // land past the end of PortCls's view - whereupon PortCls's servicing
    // logic computes a nonsense byte count from the delta against its last
    // known position and hands that to CopyTo(). Keep the reported position
    // inside the window PortCls believes in. CHdaDmaChannel::AllocateBuffer
    // now also reports the ALLOCATED size rather than the requested one, so
    // in the normal case this modulo is a no-op - it is the belt to that
    // fix's braces, and it also covers PortCls calling SetBufferSize() to
    // shrink its view after the fact.
    if (m_pDmaChannel)
    {
        ULONG bufferSize = m_pDmaChannel->BufferSize();
        if (bufferSize && pos >= bufferSize)
            pos %= bufferSize;
    }

    *Position = pos;

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStreamHda::NormalizePhysicalPosition
(
    IN OUT  PLONGLONG   PhysicalPosition
)
{
    if (!PhysicalPosition)
        return STATUS_INVALID_PARAMETER;

    ULONG bytesPerSample = (m_BitsPerSample + 7) / 8;
    ULONG bytesPerFrame = bytesPerSample * m_Channels;
    ULONGLONG bytesPerSecond = (ULONGLONG)bytesPerFrame * m_SampleRate;

    if (bytesPerSecond == 0)
    {
        *PhysicalPosition = 0;
        return STATUS_SUCCESS;
    }

    // Bytes -> 100-nanosecond units.
    *PhysicalPosition = (*PhysicalPosition * 10000000i64) / (LONGLONG)bytesPerSecond;

    return STATUS_SUCCESS;
}

STDMETHODIMP_(void) CMiniportWaveCyclicStreamHda::Silence
(
    IN  PVOID   Buffer,
    IN  ULONG   ByteCount
)
{
    // Only 8/16-bit PCM is offered by this driver's data range (see
    // wavecyclicminiport.cpp's PinDataRangePcm) - 8-bit PCM's silence level
    // is 0x80 (unsigned), everything else here is signed and silence is 0.
    UCHAR fill = (m_BitsPerSample == 8) ? 0x80 : 0x00;
    RtlFillMemory(Buffer, ByteCount, fill);
}

/*****************************************************************************
 * TimerDpcRoutine
 *****************************************************************************
 * Software substitute for a hardware stream-completion interrupt (see this
 * class's header comment) - fires at the SetNotificationFreq()-requested
 * cadence while RUN, and simply hands the stream's ServiceGroup to
 * IPortWaveCyclic::Notify(), exactly like the WavePci ISR handed its
 * ServiceGroup to IPortWavePci::Notify() from real hardware-interrupt
 * context. PortCls itself then invokes buffer servicing at DPC level.
 */
VOID CMiniportWaveCyclicStreamHda::TimerDpcRoutine
(
    IN  PKDPC   Dpc,
    IN  PVOID   DeferredContext,
    IN  PVOID   SystemArgument1,
    IN  PVOID   SystemArgument2
)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    CMiniportWaveCyclicStreamHda *that = (CMiniportWaveCyclicStreamHda *)DeferredContext;
    if (!that || !that->m_pMiniport || !that->m_pServiceGroup)
        return;

    PPORTWAVECYCLIC port = that->m_pMiniport->GetPort();
    if (port)
        port->Notify(that->m_pServiceGroup);
}
