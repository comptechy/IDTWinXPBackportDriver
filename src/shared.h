/*****************************************************************************
 * shared.h
 *****************************************************************************
 * Common includes, GUIDs, and the IAdapterCommon interface shared by every
 * miniport in this driver. Modeled on the WDK 7600 ac97\driver sample's
 * shared.h (IAC97AdapterCommon pattern), adapted for an HD Audio codec
 * instead of an AC'97 codec.
 */

#ifndef _SHARED_H_
#define _SHARED_H_

#define PC_IMPLEMENTATION 1

// Get the NTDDK headers instead of the WDM headers that portcls.h wants to
// include (matches the WDK 7600 ac97\driver sample's shared.h) - needed for
// MmGetPhysicalAddress and friends used by dmachannel.cpp.
#define WIN9X_COMPAT_SPINLOCK

#ifdef __cplusplus
extern "C" {
#endif
    #include <ntddk.h>
#ifdef __cplusplus
} // extern "C"
#endif

#include <portcls.h>
#include <stdunk.h>
#include <ksdebug.h>
#include <hdaudio.h>

#include "hdaregs.h"
#include "hdaverbs.h"
#include "debug.h"

//
// Pin and node IDs for the two KS filters this driver exposes. Each value is
// the entry's index in the corresponding descriptor array, so these enums and
// those arrays must be kept in step:
//
//   HdaWavePin  <-> MiniportWavePins       (wavecyclicminiport.cpp)
//   HdaWaveNode <-> MiniportWaveNodes      (wavecyclicminiport.cpp)
//   HdaTopoPin  <-> MiniportPins           (mintopo.cpp)
//   HdaTopoNode <-> TopologyNodes          (mintopo.cpp)
//
// Scope stays deliberately small for the MVP: one render path (front stereo
// out) and one capture path (mic/line in), matching the "basic playback,
// ideally + recording" scope from HANDOFF.md - NOT full DTS/EQ/mixer parity,
// so no surround/SPDIF/aux pins and no volume/mute nodes yet.
//
// Stage 5at: PIN_WAVEOUT_BRIDGE and PIN_WAVEIN_BRIDGE used to be 0 and 1 -
// that is, aliases for the two STREAMING pins. adapter.cpp registers the
// filter-to-filter physical connection on them, so the driver was telling
// sysaudio that its topology hangs off two KSPIN_COMMUNICATION_SINK pins.
// A physical connection must terminate on a bridge pin
// (KSPIN_COMMUNICATION_NONE) instead, so sysaudio could not walk from any
// wave pin to the topology filter's speaker connector and never built a
// waveOut device: Control Panel's "no audio device", even though kstest.exe -
// which opens our KS pins directly and bypasses sysaudio - already got clean
// audio out. The bridge pins now exist for real, at indices 2 and 3.
//
// The streaming pins keep indices 0 and 1 on purpose, so kstest.exe's
// hardcoded PinId=0 (render) / PinId=1 (capture) stay valid.
//
typedef enum
{
    PIN_WAVE_RENDER_SINK = 0, // host -> render DMA engine   (streaming, SINK)
    PIN_WAVE_CAPTURE_SINK,    // capture DMA engine -> host  (streaming, SINK)
    PIN_WAVEOUT_BRIDGE,       // wave -> topology  (bridge, DATAFLOW_OUT)
    PIN_WAVEIN_BRIDGE,        // topology -> wave  (bridge, DATAFLOW_IN)
    PIN_WAVE_TOP_ELEMENT      // count / sentinel
} HdaWavePin;

//
// The two converter nodes on the wave filter. They sit between each
// streaming pin and its bridge pin, which is what makes each half of the
// filter a path sysaudio can traverse rather than a pair of loose pins.
//
typedef enum
{
    NODE_WAVE_DAC = 0,        // PIN_WAVE_RENDER_SINK -> PIN_WAVEOUT_BRIDGE
    NODE_WAVE_ADC,            // PIN_WAVEIN_BRIDGE    -> PIN_WAVE_CAPTURE_SINK
    NODE_WAVE_TOP_ELEMENT     // count / sentinel
} HdaWaveNode;

//
// Topology filter pins. Indices unchanged from before Stage 5at - these were
// already correct - but they now have names instead of bare literals at the
// PcRegisterPhysicalConnection and PCCONNECTION_DESCRIPTOR call sites.
//
typedef enum
{
    PIN_TOPO_WAVEOUT_DEST = 0, // from the wave render bridge  (DATAFLOW_IN)
    PIN_TOPO_WAVEIN_SOURCE,    // to the wave capture bridge   (DATAFLOW_OUT)
    PIN_TOPO_LINEOUT_DEST,     // physical speaker/line-out jack
    PIN_TOPO_MIC_SOURCE,       // physical mic/line-in jack
    PIN_TOPO_TOP_ELEMENT       // count / sentinel
} HdaTopoPin;

//
// Topology filter nodes. Stage 5au: these exist because a KS filter must not
// wire one of its own pins straight to another of its own pins. Every
// PCCONNECTION_DESCRIPTOR endpoint that is not PCFILTER_NODE names a node,
// and sysaudio's graph walk resolves each connection to a node object; a
// connection with PCFILTER_NODE on BOTH sides resolves to nothing, which is
// what bugchecked sysaudio.sys with a NULL dereference the first time Stage
// 5at's real bridge pins let it walk into this filter at all. Every one of
// msvad\simple\toptable.h's seventeen connections passes through a node for
// the same reason.
//
// Stage 5ay (HANDOFF item n36): Stage 5au deliberately used two bare,
// property-less KSNODETYPE_SUM pass-throughs - the minimum shape that made
// each half of this filter traversable without adding any property-handler
// surface to get wrong, which is what a crash fix should do. That is no
// longer enough. wdmaud builds the Windows mixer device out of THIS filter,
// and a topology whose only nodes are property-less summers gives it no
// volume and no mute to expose: hence dead sliders, and - because XP's
// kmixer sources the per-stream gain it applies from that same mixer line -
// very likely the silence too. The shape below is msvad\simple\toptable.h's,
// which is known to produce a working XP mixer UI:
//
//   render:  WAVEOUT pin -> VOLUME -> MUTE -> SUM -> VOLUME -> MUTE -> LINEOUT pin
//   capture: MIC pin     -> VOLUME -> SUM -> WAVEIN pin
//
// The summers are kept exactly where Stage 5au put them, so the graph shape
// that stopped the bugcheck is preserved and no connection is ever
// pin-to-pin.
//
typedef enum
{
    NODE_TOPO_WAVEOUT_VOLUME = 0, // wave line gain    (KSNODETYPE_VOLUME)
    NODE_TOPO_WAVEOUT_MUTE,       // wave line mute    (KSNODETYPE_MUTE)
    NODE_TOPO_LINEOUT_MIX,        // render summer     (KSNODETYPE_SUM)
    NODE_TOPO_LINEOUT_VOLUME,     // master gain       (KSNODETYPE_VOLUME)
    NODE_TOPO_LINEOUT_MUTE,       // master mute       (KSNODETYPE_MUTE)
    NODE_TOPO_MIC_VOLUME,         // capture gain      (KSNODETYPE_VOLUME)
    NODE_TOPO_WAVEIN_MIX,         // capture summer    (KSNODETYPE_SUM)
    NODE_TOPO_TOP_ELEMENT         // count / sentinel
} HdaTopoNode;

//
// The gain stages IHdaAdapterCommon keeps state for, one per volume/mute
// node pair above. There is only ONE hardware attenuator behind all of this
// - the render DACs' output amplifier - so the wave and master stages are
// summed before being programmed, which is what two cascaded gain stages
// physically do. Working in 1/65536 dB (the KS volume unit) makes that an
// integer addition. The capture stage is state-only: this backport does not
// implement recording, so there is no ADC amp to drive, but the node has to
// exist for the mixer's recording line to be well formed.
//
typedef enum
{
    HDA_GAIN_WAVE = 0,
    HDA_GAIN_MASTER,
    HDA_GAIN_CAPTURE,
    HDA_GAIN_COUNT
} HdaGainStage;

//
// Forward decl of the adapter common object's interface pointer type, used
// throughout the miniports before common.h is necessarily included.
//
struct IHdaAdapterCommon;
typedef IHdaAdapterCommon *PADAPTERCOMMON;

//
// Creation entry point for the adapter common object (implemented in
// common.cpp, called from adapter.cpp's StartDevice).
//
NTSTATUS NewAdapterCommon
(
    OUT     PUNKNOWN *  Unknown,
    IN      REFCLSID,
    IN      PUNKNOWN    UnknownOuter    OPTIONAL,
    IN      POOL_TYPE   PoolType
);

//
// One resolved widget in the codec's node graph, as walked at StartDevice
// time. This is intentionally a flat runtime-discovered table (NOT a
// hardcoded per-board NID map) - see HANDOFF.md's "Findings from reading
// sigmatel.c": this board (subsys 103c2acd) has no hardcoded HP quirk in
// the Linux driver, so pin/widget layout must come from the codec's
// BIOS/EEPROM-programmed config, read at runtime via GET_CONFIG_DEFAULT,
// exactly like the generic HDA auto-parser does.
//
typedef struct _HDA_WIDGET
{
    UCHAR   Nid;                // this widget's node ID
    UCHAR   Type;                // HDA_WIDGET_TYPE_* (from AUDIO_WIDGET_CAP)
    ULONG   Caps;                // raw AUDIO_WIDGET_CAP dword
    ULONG   PinConfig;           // raw GET_CONFIG_DEFAULT dword, valid only if Type == PIN
    UCHAR   ConnList[32];        // connection list (source NIDs), if any
    UCHAR   ConnCount;
} HDA_WIDGET, *PHDA_WIDGET;

#define HDA_MAX_WIDGETS   64   // generous upper bound; real AFGs for this codec family have well under this

// Stage 5ap: size of CHdaAdapterCommon's outstanding-DMA-engine registry.
// The HD Audio spec caps a controller at 15 input + 15 output + 15
// bidirectional stream descriptors; this controller has 4 + 4. 48 slots
// can therefore never overflow, and costs 384 bytes once per device.
#define HDA_MAX_DMA_ENGINES 48

//
// IHdaAdapterCommon
//
// The common object shared by the wave and topology miniports. Owns the
// HDA controller MMIO mapping, the CORB/RIRB verb-transport ring buffers,
// codec enumeration/vendor-ID confirmation, and the runtime-discovered
// widget graph. This is the direct architectural analogue of stwrt64.sys's
// CController (verb transport) + CHDACodec (widget graph / path building)
// classes recovered via Ghidra in Stage 3, but reimplemented from scratch
// for the WaveCyclic/WavePci port-class model instead of WaveRT.
//
DECLARE_INTERFACE_(IHdaAdapterCommon, IUnknown)
{
    STDMETHOD_(NTSTATUS, Init)
    (   THIS_
        IN      PRESOURCELIST   ResourceList,
        IN      PDEVICE_OBJECT  DeviceObject
    )   PURE;

    //
    // Sends one verb and waits (via the RIRB) for its response. Mirrors
    // CController::TransferCodecVerb's retry/timeout design recovered from
    // stwrt64.sys: bounded attempt count, per-attempt timeout, and distinct
    // "HW reported failure" vs "timed out" status codes so callers/logging
    // can tell the two apart.
    //
    STDMETHOD_(NTSTATUS, SendVerb)
    (   THIS_
        IN      UCHAR   CodecAddress,
        IN      UCHAR   Nid,
        IN      USHORT  Verb,
        IN      USHORT  Payload,
        OUT     PULONG  Response
    )   PURE;

    //
    // Returns the codec address (0-15) the 92HD89E2 responded on, as found
    // by scanning STATESTS at init. Only one codec is expected on this
    // board; if more than one bit is set we log it and use the lowest.
    //
    STDMETHOD_(UCHAR, GetCodecAddress)
    (   THIS_
        void
    )   PURE;

    //
    // HDAUDIO_BUS_INTERFACE DMA-engine wrappers, used by the WaveCyclic
    // stream/DMA-channel classes (wavecyclicstream.cpp/dmachannel.cpp)
    // instead of touching m_BusInterface directly (private to
    // CHdaAdapterCommon - see common.h). See HANDOFF.md's WaveCyclic pivot
    // writeup for why: the real bus-interface DDI is a single-handle,
    // bus-driver-owned-buffer model (AllocateRenderDmaEngine/
    // AllocateCaptureDmaEngine -> opaque HANDLE, AllocateDmaBuffer -> an
    // MDL the bus driver allocates and manages), which is a WaveCyclic fit,
    // not WavePci's miniport-owned scatter/gather model.
    //
    STDMETHOD_(NTSTATUS, AllocateRenderDmaEngine)
    (   THIS_
        IN      PHDAUDIO_STREAM_FORMAT     StreamFormat,
        OUT     PHANDLE                    Handle,
        OUT     PHDAUDIO_CONVERTER_FORMAT  ConverterFormat
    )   PURE;

    STDMETHOD_(NTSTATUS, AllocateCaptureDmaEngine)
    (   THIS_
        IN      PHDAUDIO_STREAM_FORMAT     StreamFormat,
        OUT     PHANDLE                    Handle,
        OUT     PHDAUDIO_CONVERTER_FORMAT  ConverterFormat
    )   PURE;

    //
    // Binds the codec's output converter(s) to a render stream: sends
    // SET_CONVERTER_FORMAT (verb 0x2) with the ConverterFormat word the
    // bus driver handed back from AllocateRenderDmaEngine, then
    // SET_CHAN_STREAMID (verb 0x706) with the stream tag AllocateDmaBuffer
    // assigned. Without both of these the DAC never listens to our stream
    // tag and never knows the sample format, so the controller DMAs
    // happily onto the link and the codec emits silence - which is exactly
    // what happened up to Stage 5an (see HANDOFF.md Stage 5ao).
    //
    // StreamTag 0 is the documented "not in use" tag and unbinds instead;
    // ConverterFormat is ignored in that case.
    //
    // PASSIVE_LEVEL only - it sends verbs and waits on the RIRB.
    //
    STDMETHOD_(NTSTATUS, BindRenderConverters)
    (   THIS_
        IN      USHORT  ConverterFormat,
        IN      UCHAR   StreamTag
    )   PURE;

    // Signature matches HDAUDIO_BUS_INTERFACE's real PALLOCATE_DMA_BUFFER/
    // PGET_LINK_POSITION_REGISTER typedefs (hdaudio.h) exactly - note
    // SIZE_T buffer sizes, ULONG (not USHORT) FifoSize, and that
    // GetLinkPositionRegister hands back a POINTER TO the live position
    // register (read it with READ_REGISTER_ULONG each time a fresh position
    // is needed) rather than a value snapshotted at call time.
    STDMETHOD_(NTSTATUS, AllocateDmaBuffer)
    (   THIS_
        IN      HANDLE      Handle,
        IN      SIZE_T      RequestedBufferSize,
        OUT     PMDL *      BufferMdl,
        OUT     PSIZE_T     AllocatedBufferSize,
        OUT     PUCHAR      StreamId,
        OUT     PULONG      FifoSize
    )   PURE;

    STDMETHOD_(NTSTATUS, FreeDmaBuffer)
    (   THIS_
        IN      HANDLE      Handle
    )   PURE;

    STDMETHOD_(NTSTATUS, FreeDmaEngine)
    (   THIS_
        IN      HANDLE      Handle
    )   PURE;

    STDMETHOD_(NTSTATUS, SetDmaEngineState)
    (   THIS_
        IN      HDAUDIO_STREAM_STATE    StreamState,
        IN      ULONG                   NumberOfHandles,
        IN      HANDLE *                Handles
    )   PURE;

    STDMETHOD_(NTSTATUS, GetLinkPositionRegister)
    (   THIS_
        IN      HANDLE      Handle,
        OUT     PULONG *    Position
    )   PURE;

    //
    // Widget graph access, populated once at Init time by walking the AFG's
    // node list and issuing GET_PARAMETER / GET_CONNECTION_LIST_ENTRY /
    // GET_CONFIG_DEFAULT for each node - see codec.cpp's DiscoverWidgets().
    //
    STDMETHOD_(ULONG, GetWidgetCount)
    (   THIS_
        void
    )   PURE;

    STDMETHOD_(PHDA_WIDGET, GetWidget)
    (   THIS_
        IN      ULONG   Index
    )   PURE;

    //
    // Runs the codec-family init verb sequence (double-reset handshake +
    // power-up + the per-widget init verbs derived from sigmatel.c's
    // STAC92HD73XX path). Called once from StartDevice after DiscoverWidgets.
    //
    STDMETHOD_(NTSTATUS, InitCodec)
    (   THIS_
        void
    )   PURE;

    //
    // Shared cache-coherent common-buffer allocation, reusing the same
    // PDMA_ADAPTER the adapter-common object already obtained via
    // IoGetDmaAdapter for the CORB/RIRB buffers (see common.cpp's
    // SetupCorbRirb), instead of every wave stream calling IoGetDmaAdapter
    // itself. Used by wavepcistream.cpp to allocate each stream's BDL.
    //
    STDMETHOD_(NTSTATUS, AllocateCommonBuffer)
    (   THIS_
        IN      ULONG               Size,
        OUT     PVOID *             VirtualAddress,
        OUT     PPHYSICAL_ADDRESS   PhysicalAddress
    )   PURE;

    //
    // Stage 5ay: the topology filter's KSNODETYPE_VOLUME / KSNODETYPE_MUTE
    // nodes, backed by the render DACs' output amplifier (SET_AMP_GAIN_MUTE,
    // the same verb InitOutputPin already uses to unmute at full gain).
    //
    // Levels are KS volume levels: signed 1/65536 dB, so 0 is unity and
    // negative is attenuation. The range comes from the codec's own AMP_CAP
    // at init time rather than from a made-up constant - see
    // QueryOutputAmpCaps in common.cpp. Channel -1 means "all channels";
    // 0 and 1 are left and right.
    //
    // PASSIVE_LEVEL only for the setters: they send verbs and wait on the
    // RIRB. The getters read cached state and are safe anywhere.
    //
    // Stage 5bb. The codec's PARAMETER 0x0A bitmap, read once at InitCodec
    // and cached. Zero means the codec did not answer, in which case the
    // caller must not treat any rate as ruled out. See QueryPcmCaps().
    STDMETHOD_(ULONG, GetSupportedPcmRates)(THIS) PURE;

    STDMETHOD_(void, GetVolumeRange)
    (   THIS_
        OUT     PLONG   Minimum,
        OUT     PLONG   Maximum,
        OUT     PLONG   Step
    )   PURE;

    STDMETHOD_(LONG, GetVolumeLevel)
    (   THIS_
        IN      ULONG   Stage,
        IN      LONG    Channel
    )   PURE;

    STDMETHOD_(NTSTATUS, SetVolumeLevel)
    (   THIS_
        IN      ULONG   Stage,
        IN      LONG    Channel,
        IN      LONG    Level
    )   PURE;

    STDMETHOD_(BOOLEAN, GetMute)
    (   THIS_
        IN      ULONG   Stage
    )   PURE;

    STDMETHOD_(NTSTATUS, SetMute)
    (   THIS_
        IN      ULONG   Stage,
        IN      BOOLEAN Mute
    )   PURE;

    STDMETHOD_(void, FreeCommonBuffer)
    (   THIS_
        IN      ULONG               Size,
        IN      PVOID               VirtualAddress,
        IN      PHYSICAL_ADDRESS    PhysicalAddress
    )   PURE;
};

//
// {2E71D296-2038-4E51-8C77-3B0A1E7F6F9E} - private to this driver, generated
// for this project (not a well-known PortCls GUID).
//
DEFINE_GUID(IID_IHdaAdapterCommon,
0x2e71d296, 0x2038, 0x4e51, 0x8c, 0x77, 0x3b, 0x0a, 0x1e, 0x7f, 0x6f, 0x9e);

/*****************************************************************************
 * Filter-level property surface
 *****************************************************************************
 * Stage 5aw. Stage 5av's IOCTL_KS_PROPERTY trace showed sysaudio's kernel
 * worker (PID 4) walking both filters completely and correctly - all four
 * pins on each, every data range, and KSPROPERTY_PIN_PHYSICALCONNECTION
 * answering on exactly the four bridge pins (topology 0/1, wave 2/3) and
 * returning STATUS_NOT_FOUND on the other four, which is right - and then
 * quietly declining to build a waveOut device.
 *
 * Of the 118 enumeration requests only one kind was refused, and it was the
 * very FIRST question asked of each filter:
 *
 *   KSPROP #1:  Topology filter  General(1464EDA5) id=0 in=24 out=72 -> C0000230
 *   KSPROP #61: Wave filter      General(1464EDA5) id=0 in=24 out=72 -> C0000230
 *
 * That is KSPROPERTY_GENERAL_COMPONENTID, and out=72 is sizeof(KSCOMPONENTID)
 * exactly - sysaudio arrives with a correctly pre-sized buffer, so it is not
 * probing, it expects an answer. C0000230 is STATUS_PROPSET_NOT_FOUND: both
 * PCFILTER_DESCRIPTORs carried AutomationTable = NULL, so this driver
 * published no filter-level property set at all and PortCls could only say
 * "no such set". Every WDK audio sample answers this one - msvad\simple on
 * its wave filter (wavtable.h), sb16 on BOTH filters from a single shared
 * table (common.h) - and sb16, being a real hardware driver, is the closer
 * model, so its shape is what is used here.
 *
 * The second (and only other) refusal in the whole log is the LAST thing that
 * happens before the trace ends:
 *
 *   KSPROP #119/#120: PID=480 Wave filter Audio(45FFAAA0) id=40 SET
 *                     in=24 out=16 -> C0000230
 *
 * id 40 is KSPROPERTY_AUDIO_PREFERRED_STATUS and out=16 is
 * sizeof(KSAUDIO_PREFERRED_STATUS) exactly - a user-mode service telling this
 * filter it has been picked as the preferred device. Note this property only
 * exists between NTDDI_WINXP and NTDDI_VISTA, so no WDK 7600 sample
 * implements it and, more to the point, the Vista-era stwrt64.sys this driver
 * is backported from could never have implemented it either. It is exactly
 * the class of thing a Vista-to-XP backport has to add. Accepting the SET is
 * the correct response; there is nothing for the hardware to do about it.
 */

//
// {CF1D2B2E-DB97-4DA2-827E-0250DF31F153} and
// {17E1EBEF-A7BA-4DF6-B3AB-3635DA98781B} - generated for this project, not
// well-known GUIDs. The name GUID is looked up under
// HKLM\SYSTEM\CurrentControlSet\Control\MediaCategories, which stwrtxp.inf
// now populates - sb16's INF does the same for its NAME_MSSB16.
//
DEFINE_GUID(PID_STWRTXP,
0xcf1d2b2e, 0xdb97, 0x4da2, 0x82, 0x7e, 0x02, 0x50, 0xdf, 0x31, 0xf1, 0x53);

DEFINE_GUID(NAME_STWRTXP,
0x17e1ebef, 0xa7ba, 0x4df6, 0xb3, 0xab, 0x36, 0x35, 0xda, 0x98, 0x78, 0x1b);

#define STWRTXP_VERSION     0x1
#define STWRTXP_REVISION    0x0

NTSTATUS PropertyHandler_ComponentId
(
    IN      PPCPROPERTY_REQUEST PropertyRequest
);

NTSTATUS PropertyHandler_PreferredStatus
(
    IN      PPCPROPERTY_REQUEST PropertyRequest
);

//
// One table shared by both filter descriptors, as in sb16's common.h. It is
// static, so each translation unit that includes this header gets its own
// copy; only mintopo.cpp and wavecyclicminiport.cpp actually reference
// AutomationFilter, and a few hundred unused bytes elsewhere is the price of
// keeping the definition in one place.
//
static PCPROPERTY_ITEM PropertiesFilter[] =
{
    {
        &KSPROPSETID_General,
        KSPROPERTY_GENERAL_COMPONENTID,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_ComponentId
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_PREFERRED_STATUS,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_PreferredStatus
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationFilter, PropertiesFilter);

#endif  // _SHARED_H_
