/*****************************************************************************
 * common.h
 *****************************************************************************
 * CHdaAdapterCommon: the shared adapter object that owns the HDA controller
 * MMIO mapping, CORB/RIRB verb transport, and the runtime-discovered codec
 * widget graph. Implements IHdaAdapterCommon (shared.h).
 *
 * Architectural analogue of stwrt64.sys's CController (verb transport, see
 * Stage 3 Ghidra findings on FUN_00019b30) and CHDACodec (widget graph /
 * path building, FUN_0003ff84), reimplemented from scratch against the
 * WaveCyclic/WavePci port-class model instead of WaveRT.
 */

#ifndef _COMMON_H_
#define _COMMON_H_

#include "shared.h"
#include <hdaudio.h>

class CHdaAdapterCommon : public IHdaAdapterCommon,
                           public IAdapterPowerManagement,
                           public CUnknown
{
private:
    PDEVICE_OBJECT      m_pDeviceObject;

    // Real HD Audio/UAA architecture: the PCI HDA controller's MMIO BAR and
    // IRQ belong to the bus driver (HDAudBus.sys), not to this codec function
    // device - confirmed on real hardware via ValidateResources logging
    // "expected at least 1 memory resource, got 0". Verb transport and
    // stream/DMA-engine management instead go through this interface,
    // obtained from the lower (bus-owned) device object via
    // IRP_MN_QUERY_INTERFACE - see AcquireBusInterface() in common.cpp.
    HDAUDIO_BUS_INTERFACE m_BusInterface;
    BOOLEAN             m_bBusInterfaceAcquired;

    UCHAR               m_CodecAddress;     // reported by the bus via GetResourceInformation
    BOOLEAN             m_bCodecFound;

    ULONG               m_NumInputStreams;
    ULONG               m_NumOutputStreams;
    ULONG               m_NumBidiStreams;

    HDA_WIDGET          m_Widgets[HDA_MAX_WIDGETS];
    ULONG               m_WidgetCount;
    UCHAR               m_AfgNid;           // Audio Function Group node ID

    // Distinct DAC node IDs feeding the output pins InitOutputPin() wired
    // up, recorded there and replayed by BindRenderConverters(). On this
    // board all five output pins select the same DAC at connection index
    // 0, so this is normally a one-element set - but the array costs
    // nothing and keeps the code honest on hardware that splits them.
    // Capped well below the widget count: a codec with more than eight
    // distinct output DACs is not something this backport targets.
    UCHAR               m_OutputDacNids[8];
    ULONG               m_OutputDacCount;

    // Stage 5ay: state behind the topology filter's volume and mute nodes.
    // Cached rather than read back from the codec on every GET because a
    // GET must not depend on a RIRB round trip, and because the wave and
    // master stages have to be summed before either can be programmed -
    // there is one hardware attenuator, not two (see shared.h's
    // HdaGainStage). Levels are signed 1/65536 dB.
    LONG                m_Volume[HDA_GAIN_COUNT][2];    // [stage][channel]
    BOOLEAN             m_Mute[HDA_GAIN_COUNT];

    // Decoded from the render DAC's AMP_CAP at init (QueryOutputAmpCaps).
    // m_bAmpCapsValid is FALSE when the codec reports no usable output amp,
    // in which case the nodes still work as a cache but no verb is sent -
    // a dead slider is better than a slider that silently writes garbage
    // gain steps into a widget that has none.
    LONG                m_VolumeMinimum;
    LONG                m_VolumeMaximum;
    LONG                m_VolumeStep;
    UCHAR               m_AmpOffset;        // AMP_CAP step index meaning 0 dB
    UCHAR               m_AmpNumSteps;      // highest valid step index
    BOOLEAN             m_bAmpCapsValid;

    // Stage 5bb: PARAMETER 0x0A as read at InitCodec, or 0 if the codec did
    // not answer. The wave miniport uses it to decide which sample rates it
    // is honest to advertise.
    ULONG               m_SuppPcmRates;
    ULONG               m_SuppStreamFormats;

    // Stage 5ap: every DMA engine this driver has taken from hdaudbus
    // and not yet given back. HD Audio controllers have very few (this
    // one has four render and four capture), hdaudbus keeps them out on
    // loan until they are freed or the bus interface is dereferenced,
    // and a driver that loses track of one has no way to ask for it
    // back. ~CHdaAdapterCommon sweeps whatever is still here, so a leak
    // that escapes the stream teardown path costs a device restart
    // instead of a reboot - which is exactly what Stage 5ap's root cause
    // cost before it was found. Slots are claimed and released with
    // interlocked pointer swaps: no lock, no IRQL constraint.
    HANDLE volatile     m_EngineHandles[HDA_MAX_DMA_ENGINES];

    DEVICE_POWER_STATE  m_PowerState;

    //
    // Internal helpers (see common.cpp)
    //
    NTSTATUS AcquireBusInterface(IN PDEVICE_OBJECT DeviceObject);
    NTSTATUS DiscoverWidgets(void);
    NTSTATUS DiscoverWidgetsInGroup(IN UCHAR StartNid, IN UCHAR NodeCount);
    NTSTATUS BringUpCodec(void);
    NTSTATUS InitOutputPin(IN PHDA_WIDGET PinWidget);
    void RecordOutputDac(IN UCHAR Nid);
    void QueryOutputAmpCaps(void);
    void QueryPcmCaps(void);           // Stage 5bb
    NTSTATUS ProgramOutputAmp(void);
    UCHAR LevelToAmpStep(IN LONG Level);
    void TrackEngine(IN HANDLE Handle);
    void UntrackEngine(IN HANDLE Handle);

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CHdaAdapterCommon);
    ~CHdaAdapterCommon();

    IMP_IAdapterPowerManagement;

    STDMETHODIMP_(NTSTATUS) Init
    (
        IN  PRESOURCELIST ResourceList,
        IN  PDEVICE_OBJECT DeviceObject
    );

    STDMETHODIMP_(NTSTATUS) SendVerb
    (
        IN  UCHAR   CodecAddress,
        IN  UCHAR   Nid,
        IN  USHORT  Verb,
        IN  USHORT  Payload,
        OUT PULONG  Response
    );

    STDMETHODIMP_(UCHAR) GetCodecAddress(void)
    {
        return m_CodecAddress;
    };

    // HDAUDIO_BUS_INTERFACE DMA-engine wrappers (see shared.h's
    // IHdaAdapterCommon declaration for why) - thin pass-throughs to
    // m_BusInterface, implemented in common.cpp.
    STDMETHODIMP_(NTSTATUS) AllocateRenderDmaEngine
    (
        IN  PHDAUDIO_STREAM_FORMAT     StreamFormat,
        OUT PHANDLE                    Handle,
        OUT PHDAUDIO_CONVERTER_FORMAT  ConverterFormat
    );

    STDMETHODIMP_(NTSTATUS) AllocateCaptureDmaEngine
    (
        IN  PHDAUDIO_STREAM_FORMAT     StreamFormat,
        OUT PHANDLE                    Handle,
        OUT PHDAUDIO_CONVERTER_FORMAT  ConverterFormat
    );

    STDMETHODIMP_(NTSTATUS) AllocateDmaBuffer
    (
        IN  HANDLE      Handle,
        IN  SIZE_T      RequestedBufferSize,
        OUT PMDL *      BufferMdl,
        OUT PSIZE_T     AllocatedBufferSize,
        OUT PUCHAR      StreamId,
        OUT PULONG      FifoSize
    );

    STDMETHODIMP_(NTSTATUS) FreeDmaBuffer(IN HANDLE Handle);
    STDMETHODIMP_(NTSTATUS) FreeDmaEngine(IN HANDLE Handle);

    STDMETHODIMP_(NTSTATUS) SetDmaEngineState
    (
        IN  HDAUDIO_STREAM_STATE   StreamState,
        IN  ULONG                  NumberOfHandles,
        IN  HANDLE *               Handles
    );

    STDMETHODIMP_(NTSTATUS) GetLinkPositionRegister
    (
        IN  HANDLE   Handle,
        OUT PULONG * Position
    );

    STDMETHODIMP_(ULONG) GetWidgetCount(void)
    {
        return m_WidgetCount;
    };

    STDMETHODIMP_(PHDA_WIDGET) GetWidget(IN ULONG Index)
    {
        if (Index >= m_WidgetCount)
            return NULL;
        return &m_Widgets[Index];
    };

    STDMETHODIMP_(NTSTATUS) InitCodec(void);

    STDMETHODIMP_(NTSTATUS) BindRenderConverters
    (
        IN  USHORT  ConverterFormat,
        IN  UCHAR   StreamTag
    );

    STDMETHODIMP_(NTSTATUS) AllocateCommonBuffer
    (
        IN  ULONG               Size,
        OUT PVOID *             VirtualAddress,
        OUT PPHYSICAL_ADDRESS   PhysicalAddress
    );

    STDMETHODIMP_(void) FreeCommonBuffer
    (
        IN  ULONG               Size,
        IN  PVOID               VirtualAddress,
        IN  PHYSICAL_ADDRESS    PhysicalAddress
    );

    // Stage 5ay volume/mute surface - see shared.h.
    STDMETHODIMP_(ULONG) GetSupportedPcmRates(void);   // Stage 5bb

    STDMETHODIMP_(void) GetVolumeRange
    (
        OUT PLONG   Minimum,
        OUT PLONG   Maximum,
        OUT PLONG   Step
    );

    STDMETHODIMP_(LONG) GetVolumeLevel
    (
        IN  ULONG   Stage,
        IN  LONG    Channel
    );

    STDMETHODIMP_(NTSTATUS) SetVolumeLevel
    (
        IN  ULONG   Stage,
        IN  LONG    Channel,
        IN  LONG    Level
    );

    STDMETHODIMP_(BOOLEAN) GetMute(IN ULONG Stage);

    STDMETHODIMP_(NTSTATUS) SetMute
    (
        IN  ULONG   Stage,
        IN  BOOLEAN Mute
    );

    friend NTSTATUS NewAdapterCommon
    (
        OUT PUNKNOWN *OutUnknown,
        IN  REFCLSID,
        IN  PUNKNOWN UnknownOuter,
        IN  POOL_TYPE PoolType
    );
};

#endif  // _COMMON_H_
