/*****************************************************************************
 * wavecyclicminiport.cpp
 *****************************************************************************
 * See wavecyclicminiport.h. The filter descriptor below is a four-pin,
 * two-node graph on the WDK 7600 msvad\simple pattern: a streaming sink pin
 * and a bridge pin per direction, wired through a converter node. Up to
 * Stage 5at it was two loose streaming pins with no nodes and no connections
 * (carried over from the retired wavepciminiport.cpp), which is what kept
 * sysaudio from ever building a waveOut device - see the long comment on
 * MiniportWavePins and the HdaWavePin enum in shared.h.
 */

static char STR_MODULENAME[] = "stwrtxp WaveCyclic: ";
#include "wavecyclicminiport.h"
#include "wavecyclicstream.h"
#include "dmachannel.h"

#pragma code_seg("PAGE")

// The PCM format range this filter advertises. Stage 5bb narrows the two
// frequency fields at Init() - see NarrowPcmRangeToCodec() below - so the
// values written here are only the pre-negotiation defaults.
//
// They used to be the whole story, and that was a real defect: 8 kHz .. 48 kHz
// was asserted without ever asking the codec, and the TODO that used to sit in
// this comment said so. Windows XP's system sounds are 22.05 kHz files, kmixer
// duly negotiated the pin down to 22050 Hz, and every one of those streams was
// silent while kstest.exe's 44.1 kHz stream was audible on the same boot. By
// Stage 5ba the buffer was proven to contain a full-scale tone during the
// silent streams and the DMA engine was proven to be reading it, so what the
// pin claimed it could play was the last thing left that was untrue.
static KSDATARANGE_AUDIO PinDataRangePcm =
{
    {
        sizeof(KSDATARANGE_AUDIO),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    2, 16, 16, 8000, 48000
};

// Stage 5bi. PinDataRangePcm above is now only a TEMPLATE - it holds the
// fields that do not vary by rate (max channels, bits per sample) and is no
// longer pointed at by any pin. What the pins actually advertise is one
// discrete KSDATARANGE_AUDIO per supported rate, with MinimumSampleFrequency
// == MaximumSampleFrequency, built at Init() by BuildPcmDataRanges() from the
// codec's own rate bitmap. See that function for why a span was wrong.
//
// The arrays are sized for the whole standard HDA rate table so the count is a
// compile-time constant here; BuildPcmDataRanges() patches each streaming pin's
// DataRangesCount down to the number it actually filled, exactly as ac97's
// BuildDataRangeInformation() does at wavepciminiport.cpp:783-789.
static KSDATARANGE_AUDIO PinDataRangesPcm[HDA_PCM_RATE_COUNT];

static PKSDATARANGE PinDataRangesOut[HDA_PCM_RATE_COUNT];
static PKSDATARANGE PinDataRangesIn[HDA_PCM_RATE_COUNT];

// How many entries of the three arrays above are live. Zero until Init().
static ULONG PinDataRangesPcmCount = 0;

// The rates this driver is willing to advertise, intersected with whatever the
// codec's bitmap allows. Deliberately just the 44.1 / 48 kHz pair: those are
// the only two rates this project has ever played on this hardware, the only
// two the stream path's converter-format and buffer maths have been exercised
// at, and the two an HDA codec is in practice guaranteed to have. The codec
// also claims 88.2, 96 and 192 kHz; adding one is a one-line change here, but
// it would be advertising a path nothing has tested, so it stays out until
// something needs it.
static const ULONG PinAdvertisedRateHz[] = { 48000, 44100 };

// Stage 5at: the bridge pins' data range. A bridge pin carries no digital
// stream format - it is the analog side of the filter, the hand-off point to
// the topology filter - so it advertises a plain KSDATARANGE (NOT a
// KSDATARANGE_AUDIO) of TYPE_AUDIO / SUBTYPE_ANALOG / SPECIFIER_NONE, exactly
// as the WDK 7600 msvad\simple sample's wavtable.h does.
static KSDATARANGE PinDataRangesBridge[] =
{
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static PKSDATARANGE PinDataRangePointersBridge[] = { &PinDataRangesBridge[0] };

// Stage 5at: this filter used to expose ONLY the two streaming sink pins, with
// no nodes and no connections, and adapter.cpp registered its physical
// connection to the topology filter on those very pins. That is not a graph
// sysaudio can traverse. A physical connection has to terminate on a bridge
// pin - KSPIN_COMMUNICATION_NONE, dataflow pointing out of this filter on the
// render side and into it on the capture side - so with no bridge pins at all
// sysaudio saw a wave filter with two dead-end sink pins, could not reach the
// topology filter's KSNODETYPE_SPEAKER connector from either of them, and
// therefore never built a waveOut/waveIn device. That is the whole of the
// "no audio device" symptom in Control Panel, and it is also why kstest.exe -
// which creates our KS pins directly and never involves sysaudio - has been
// getting clean audio out of the same driver the whole time.
//
// Shape follows msvad\simple\wavtable.h: streaming sink -> converter node ->
// bridge pin, per direction. The streaming pins keep indices 0 and 1 so
// kstest.exe's hardcoded PinIds still work; the bridge pins are appended.
static PCPIN_DESCRIPTOR MiniportWavePins[] =
{
    // PIN_WAVE_RENDER_SINK: host -> this pin -> render DMA engine.
    {
        1, 1, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(PinDataRangesOut), PinDataRangesOut,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // PIN_WAVE_CAPTURE_SINK: capture DMA engine -> this pin -> host.
    {
        1, 1, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(PinDataRangesIn), PinDataRangesIn,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // PIN_WAVEOUT_BRIDGE: NODE_WAVE_DAC -> this pin -> the topology filter's
    // PIN_TOPO_WAVEOUT_DEST. Instance counts are zero: nobody opens a bridge
    // pin as a stream.
    {
        0, 0, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge), PinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // PIN_WAVEIN_BRIDGE: the topology filter's PIN_TOPO_WAVEIN_SOURCE -> this
    // pin -> NODE_WAVE_ADC. Mirror of the render bridge.
    {
        0, 0, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge), PinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    }
};

// Stage 5be: KSPROPERTY_AUDIO_CHANNEL_CONFIG on the DAC node.
//
// Windows Media Player's last act before refusing to play with "there is a
// problem with your sound device" is a SET of this property, aimed at the
// WAVE filter (not the topology filter), and until now it was refused with
// STATUS_NOT_FOUND because both nodes below carried a NULL automation table
// and so had no property surface at all. WMP then gave up without ever
// creating a pin - no NewStream, no SetFormat, nothing.
//
// The WDK 7600 msvad\pcmex sample is the one sample that implements this
// property, and it puts it in exactly this place: PropertiesDAC on
// KSNODE_WAVE_DAC of the wave miniport, GET | SET, handled out of a cached
// KSAUDIO_CHANNEL_CONFIG member. This mirrors that.
//
// Stage 5bf, resolved: the "node 8" in WMP's request is a SYSAUDIO VIRTUAL
// NODE ID, and virtual 8 really is this DAC node. The Stage 5be log proves
// it directly - the widened trace pairs each hook line with the handler that
// ran, and the pairs read:
//
//   virtual 6 -> topology node 0     virtual 2 -> topology node 4
//   virtual 5 -> topology node 1     virtual 1 -> topology node 5
//   virtual 4 -> topology node 2     virtual 0 -> topology node 6
//   virtual 3 -> topology node 3     virtual 8 -> WAVE node 0 (this one)
//
// i.e. sysaudio enumerates the composite graph's nodes in reverse walk order:
// the topology filter's seven mirrored onto 6..0, then the wave filter's two
// onto 8 (DAC) and 7 (ADC). Nothing is untranslated and nothing is leaking;
// the id was simply not ours to recognise. The Stage 5be refusal was the
// plain fact that this node published no properties at all.
// Stage 5bf: two more properties on the same node, both of which the Stage
// 5be log caught WMP asking for and being refused.
//
// KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY is the terminal one - a SET of it
// is the last thing WMP does before it gives up, and together with the
// CHANNEL_CONFIG set immediately before it, the pair is exactly what
// IDirectSound::SetSpeakerConfig decomposes into. Half a SetSpeakerConfig
// succeeding is still a failed SetSpeakerConfig, which is a DirectSound
// error, which is "there is a problem with your sound device".
//
// KSPROPERTY_AUDIO_CPU_RESOURCES is not terminal - WMP asked twice, was
// refused twice and carried on - but it costs one handler and it is the same
// question the topology nodes have answered since Stage 5ay. The answer here
// is the same and for the same reason: the conversion is the codec's work,
// not the host's.
static NTSTATUS PropertyHandler_ChannelConfig(IN PPCPROPERTY_REQUEST PropertyRequest);
static NTSTATUS PropertyHandler_SpeakerGeometry(IN PPCPROPERTY_REQUEST PropertyRequest);
static NTSTATUS PropertyHandler_CpuResourcesDac(IN PPCPROPERTY_REQUEST PropertyRequest);

static PCPROPERTY_ITEM PropertiesDAC[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_CHANNEL_CONFIG,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET,
        PropertyHandler_ChannelConfig
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET,
        PropertyHandler_SpeakerGeometry
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_CPU_RESOURCES,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_CpuResourcesDac
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationDAC, PropertiesDAC);

// The converter nodes between each streaming pin and its bridge pin. The ADC
// node carries no automation table - there is no capture control surface in
// this backport - but both still have to exist, because a KS topology path is
// walked node by node and a bare pin-to-pin hop gives sysaudio nothing to
// attribute the conversion to.
static PCNODE_DESCRIPTOR MiniportWaveNodes[] =
{
    // NODE_WAVE_DAC
    { 0, &AutomationDAC, &KSNODETYPE_DAC, NULL },
    // NODE_WAVE_ADC
    { 0, NULL, &KSNODETYPE_ADC, NULL }
};

// KS node pin convention (the one msvad follows throughout): pin 1 of a node
// is its input, pin 0 its output.
static PCCONNECTION_DESCRIPTOR MiniportWaveConnections[] =
{
    //  FromNode        FromPin                ToNode          ToPin
    { PCFILTER_NODE,    PIN_WAVE_RENDER_SINK,  NODE_WAVE_DAC,  1                     },
    { NODE_WAVE_DAC,    0,                     PCFILTER_NODE,  PIN_WAVEOUT_BRIDGE    },
    { PCFILTER_NODE,    PIN_WAVEIN_BRIDGE,     NODE_WAVE_ADC,  1                     },
    { NODE_WAVE_ADC,    0,                     PCFILTER_NODE,  PIN_WAVE_CAPTURE_SINK }
};

static PCFILTER_DESCRIPTOR MiniportWaveFilterDescriptor =
{
    0,                                      // Version
    &AutomationFilter,                      // AutomationTable (shared.h)
    sizeof(PCPIN_DESCRIPTOR),               // PinSize
    SIZEOF_ARRAY(MiniportWavePins),         // PinCount
    MiniportWavePins,                       // Pins
    sizeof(PCNODE_DESCRIPTOR),              // NodeSize
    SIZEOF_ARRAY(MiniportWaveNodes),        // NodeCount
    MiniportWaveNodes,                      // Nodes
    SIZEOF_ARRAY(MiniportWaveConnections),  // ConnectionCount
    MiniportWaveConnections,                // Connections
    0,                                      // CategoryCount - PortCls defaults
    NULL                                    // Categories      (audio/render/capture)
};

/*****************************************************************************
 * PropertyHandler_ChannelConfig
 *****************************************************************************
 * KSPROPERTY_AUDIO_CHANNEL_CONFIG on NODE_WAVE_DAC. See the comment above
 * PropertiesDAC for why this exists and what it is and is not known to fix.
 *
 * Value is a KSAUDIO_CHANNEL_CONFIG - a single LONG speaker-position mask,
 * four bytes, which is exactly the out=4 seen in the failing WMP request.
 * The validation below is msvad's ValidatePropertyParams (kshelper.cpp:156)
 * inlined: a zero ValueSize is a size probe and gets the size back with
 * STATUS_BUFFER_OVERFLOW, anything short gets STATUS_BUFFER_TOO_SMALL with
 * ValueSize cleared.
 *
 * On SET, only masks this filter can actually carry are accepted. The pin
 * advertises MaximumChannels = 2 (PinDataRangePcm above), so mono and stereo
 * are honoured and everything else is refused with STATUS_NOT_SUPPORTED -
 * the same gating msvad\pcmex applies against its own m_MaxChannelsPcm.
 * Refusing rather than silently accepting matters here for the same reason
 * it mattered in Stage 5bc: this miniport is the only thing in the stack
 * that checks, so a lie told here is a lie nothing downstream will catch.
 */
/*****************************************************************************
 * ValidateValueSize
 *****************************************************************************
 * msvad's ValidatePropertyParams (kshelper.cpp:156) reduced to the part these
 * handlers need: a zero ValueSize is a size probe and gets the size back with
 * STATUS_BUFFER_OVERFLOW, anything short gets STATUS_BUFFER_TOO_SMALL with
 * ValueSize cleared, and STATUS_SUCCESS means Value is safe to dereference
 * for cbSize bytes.
 */
static NTSTATUS ValidateValueSize
(
    IN  PPCPROPERTY_REQUEST PropertyRequest,
    IN  ULONG               cbSize
)
{
    PAGED_CODE();

    if (0 == PropertyRequest->ValueSize)
    {
        PropertyRequest->ValueSize = cbSize;
        return STATUS_BUFFER_OVERFLOW;
    }

    if (PropertyRequest->ValueSize < cbSize || !PropertyRequest->Value)
    {
        PropertyRequest->ValueSize = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS PropertyHandler_ChannelConfig(IN PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();

    if (!PropertyRequest || !PropertyRequest->MajorTarget)
        return STATUS_INVALID_PARAMETER;

    CMiniportWaveCyclicHda *that = (CMiniportWaveCyclicHda *)
        (PMINIPORTWAVECYCLIC)PropertyRequest->MajorTarget;

    PKSAUDIO_CHANNEL_CONFIG cached = that->ChannelConfig();

    NTSTATUS sizeStatus = ValidateValueSize(PropertyRequest,
        sizeof(KSAUDIO_CHANNEL_CONFIG));
    if (!NT_SUCCESS(sizeStatus) || STATUS_BUFFER_OVERFLOW == sizeStatus)
    {
        DOUT(DBG_PRINT, ("PropertyHandler_ChannelConfig: node=%u size check -> %08X",
            PropertyRequest->Node, sizeStatus));
        return sizeStatus;
    }

    PKSAUDIO_CHANNEL_CONFIG value = (PKSAUDIO_CHANNEL_CONFIG)PropertyRequest->Value;
    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        *value = *cached;
        PropertyRequest->ValueSize = sizeof(KSAUDIO_CHANNEL_CONFIG);
        ntStatus = STATUS_SUCCESS;
    }
    else if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
    {
        switch (value->ActiveSpeakerPositions)
        {
            case KSAUDIO_SPEAKER_MONO:
                ntStatus = STATUS_SUCCESS;
                break;

            case KSAUDIO_SPEAKER_STEREO:
                ntStatus = (PinDataRangePcm.MaximumChannels >= 2)
                    ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
                break;

            default:
                ntStatus = STATUS_NOT_SUPPORTED;
                break;
        }

        if (NT_SUCCESS(ntStatus))
        {
            *cached = *value;
        }
    }

    DOUT(DBG_PRINT, ("PropertyHandler_ChannelConfig: node=%u verb=%08X mask=%08X "
        "-> %08X", PropertyRequest->Node, PropertyRequest->Verb,
        value->ActiveSpeakerPositions, ntStatus));

    return ntStatus;
}

/*****************************************************************************
 * PropertyHandler_SpeakerGeometry
 *****************************************************************************
 * KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY on NODE_WAVE_DAC. The value is a
 * LONG: the angle in degrees subtended by the speaker pair, or
 * KSAUDIO_STEREO_SPEAKER_GEOMETRY_HEADPHONE (-1). DirectSound reaches this
 * through IDirectSound::SetSpeakerConfig, which is why it arrives paired with
 * the CHANNEL_CONFIG set immediately before it.
 *
 * Purely cached. This backport does no 3D panning, so there is no hardware
 * meaning to give the angle - and unlike a sample rate (Stage 5bc), a
 * geometry this driver ignores cannot make anything inaudible, so accepting
 * it costs nothing. Values outside the documented range are still refused
 * rather than silently swallowed, and the refusal names the value so the
 * trace says what was asked for.
 */
static NTSTATUS PropertyHandler_SpeakerGeometry(IN PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();

    if (!PropertyRequest || !PropertyRequest->MajorTarget)
        return STATUS_INVALID_PARAMETER;

    CMiniportWaveCyclicHda *that = (CMiniportWaveCyclicHda *)
        (PMINIPORTWAVECYCLIC)PropertyRequest->MajorTarget;

    LONG *cached = that->SpeakerGeometry();

    NTSTATUS sizeStatus = ValidateValueSize(PropertyRequest, sizeof(LONG));
    if (!NT_SUCCESS(sizeStatus) || STATUS_BUFFER_OVERFLOW == sizeStatus)
    {
        DOUT(DBG_PRINT, ("PropertyHandler_SpeakerGeometry: node=%u size check -> %08X",
            PropertyRequest->Node, sizeStatus));
        return sizeStatus;
    }

    LONG *value = (LONG *)PropertyRequest->Value;
    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        *value = *cached;
        PropertyRequest->ValueSize = sizeof(LONG);
        ntStatus = STATUS_SUCCESS;
    }
    else if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
    {
        if (KSAUDIO_STEREO_SPEAKER_GEOMETRY_HEADPHONE == *value ||
            (*value >= KSAUDIO_STEREO_SPEAKER_GEOMETRY_MIN &&
             *value <= KSAUDIO_STEREO_SPEAKER_GEOMETRY_MAX))
        {
            *cached = *value;
            ntStatus = STATUS_SUCCESS;
        }
        else
        {
            ntStatus = STATUS_INVALID_PARAMETER;
        }
    }

    DOUT(DBG_PRINT, ("PropertyHandler_SpeakerGeometry: node=%u verb=%08X value=%d "
        "-> %08X", PropertyRequest->Node, PropertyRequest->Verb, *value, ntStatus));

    return ntStatus;
}

/*****************************************************************************
 * PropertyHandler_CpuResourcesDac
 *****************************************************************************
 * KSPROPERTY_AUDIO_CPU_RESOURCES on NODE_WAVE_DAC. Same question and same
 * answer as mintopo.cpp's PropertyHandler_CpuResources on the topology nodes
 * - the conversion is done by the codec, not by the host CPU - but the two
 * cannot share an implementation, because each is file-static in the
 * translation unit whose automation tables reference it.
 */
static NTSTATUS PropertyHandler_CpuResourcesDac(IN PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();

    if (!PropertyRequest)
        return STATUS_INVALID_PARAMETER;

    NTSTATUS sizeStatus = ValidateValueSize(PropertyRequest, sizeof(ULONG));
    if (!NT_SUCCESS(sizeStatus) || STATUS_BUFFER_OVERFLOW == sizeStatus)
        return sizeStatus;

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        *(PULONG)PropertyRequest->Value = KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU;
        PropertyRequest->ValueSize = sizeof(ULONG);
        ntStatus = STATUS_SUCCESS;
    }
    else if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        *(PULONG)PropertyRequest->Value =
            KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;
        PropertyRequest->ValueSize = sizeof(ULONG);
        ntStatus = STATUS_SUCCESS;
    }

    DOUT(DBG_PRINT, ("PropertyHandler_CpuResourcesDac: node=%u verb=%08X -> %08X",
        PropertyRequest->Node, PropertyRequest->Verb, ntStatus));

    return ntStatus;
}

NTSTATUS CreateMiniportWaveCyclicHda
(
    OUT     PUNKNOWN *  Unknown,
    IN      REFCLSID,
    IN      PUNKNOWN    UnknownOuter,
    IN      POOL_TYPE   PoolType
)
{
    PAGED_CODE();

    if (!Unknown)
        return STATUS_INVALID_PARAMETER;

    CMiniportWaveCyclicHda *pWave = new(PoolType) CMiniportWaveCyclicHda(UnknownOuter);
    if (!pWave)
        return STATUS_INSUFFICIENT_RESOURCES;

    *Unknown = (PUNKNOWN)(PMINIPORTWAVECYCLIC)pWave;
    (*Unknown)->AddRef();

    return STATUS_SUCCESS;
}

CMiniportWaveCyclicHda::~CMiniportWaveCyclicHda()
{
    PAGED_CODE();

    if (m_pAdapterCommon)
    {
        m_pAdapterCommon->Release();
        m_pAdapterCommon = NULL;
    }
}

/*****************************************************************************
 * CMiniportWaveCyclicHda::NonDelegatingQueryInterface
 *****************************************************************************
 */
STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicHda::NonDelegatingQueryInterface
(
    IN  REFIID  Interface,
    OUT PVOID * Object
)
{
    PAGED_CODE();
    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = (PVOID)(PUNKNOWN)(PMINIPORTWAVECYCLIC)this;
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = (PVOID)(PMINIPORT)this;
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveCyclic))
    {
        *Object = (PVOID)(PMINIPORTWAVECYCLIC)this;
    }
    else if (IsEqualGUIDAligned(Interface, IID_IPowerNotify))
    {
        *Object = (PVOID)(PPOWERNOTIFY)this;
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }

    ((PUNKNOWN)(*Object))->AddRef();
    return STATUS_SUCCESS;
}

/*****************************************************************************
 * BuildPcmDataRanges
 *****************************************************************************
 * Stage 5bi. Fills the streaming pins' data-range lists with one DISCRETE
 * KSDATARANGE_AUDIO per rate the codec confirms, replacing Stage 5bb's single
 * contiguous 44100..48000 span.
 *
 * Stage 5bb was right that the old 8 kHz .. 48 kHz claim was a defect - it was
 * asserted without ever asking the codec, XP's 22.05 kHz system sounds were
 * duly negotiated onto a pin that could not play them, and every one of those
 * streams was silent while a 44.1 kHz stream was audible on the same boot.
 * Narrowing it fixed that. What Stage 5bb got wrong was the shape, and it said
 * so out loud: "a contiguous range loses nothing here". That reasoning was
 * about kmixer, and about kmixer it is correct. But the range list is also
 * what sysaudio caches when it enumerates the graph at boot, and what
 * DirectSound consults before it creates a buffer, and a span is not the shape
 * either of them is given by any other driver:
 *
 *   ac97's BuildDataRangeInformation (wavepciminiport.cpp:739, and identically
 *   in rtminiport.cpp) walks dwWaveSampleRates = {48000, 44100, 32000, 22050,
 *   16000, 11025, 8000}, programs each rate into the hardware to find out
 *   whether it takes, and emits one range per surviving rate with
 *   MinimumSampleFrequency == MaximumSampleFrequency. On a non-VSR AC'97 codec
 *   exactly one range survives, 48000..48000, and that is a working
 *   configuration - so a narrow rate SET is demonstrably fine. It is the span
 *   that is unlike the reference.
 *
 * The span was also a claim this driver did not honour. It said every rate
 * from 44100 to 48000 inclusive was acceptable; ValidateFormat accepted 44100
 * and 48000 and refused everything between. Nothing in the stack is obliged to
 * only ask about the two endpoints. Driving both the advertisement and the
 * validator off PinDataRangesPcm - which is what ValidateFormat now does -
 * means the two cannot disagree again.
 *
 * This is not known to be what stops Windows Media Player. WMP opens the wave
 * filter, completes a DirectSound speaker-config negotiation in which every
 * request this driver answers succeeds, and then stops without ever asking for
 * a data intersection or creating a pin - so whatever it rejects, it rejects
 * on information it already holds, and the cached range list is the largest
 * piece of that information which this driver states differently from the
 * reference. That is the reason to try it, and it is a reason, not a
 * diagnosis. The mismatch above is worth fixing either way.
 */
static void BuildPcmDataRanges(IN PADAPTERCOMMON AdapterCommon)
{
    PAGED_CODE();

    ULONG rates = AdapterCommon ? AdapterCommon->GetSupportedPcmRates() : 0;

    // A codec that did not answer is treated as claiming the pair, exactly as
    // Stage 5bb did: the measurement that 44.1 kHz plays and 22.05 kHz does not
    // stands on its own, without the bitmap's help.
    BOOLEAN trustMask = (BOOLEAN)(rates != 0);

    PinDataRangesPcmCount = 0;

    for (ULONG i = 0; i < SIZEOF_ARRAY(PinAdvertisedRateHz); i++)
    {
        ULONG rate = PinAdvertisedRateHz[i];

        if (trustMask)
        {
            BOOLEAN listed = FALSE;

            for (ULONG b = 0; b < HDA_PCM_RATE_COUNT; b++)
            {
                if ((rates & (1UL << b)) && g_HdaPcmRateHz[b] == rate)
                {
                    listed = TRUE;
                    break;
                }
            }

            if (!listed)
            {
                DOUT(DBG_PRINT, ("BuildPcmDataRanges: skipping %u Hz - not in "
                    "the codec's rate bitmap %08X", rate, rates));
                continue;
            }
        }

        // Copy the invariant fields from the template, then pin the frequency
        // to this one rate.
        PinDataRangesPcm[PinDataRangesPcmCount] = PinDataRangePcm;
        PinDataRangesPcm[PinDataRangesPcmCount].MinimumSampleFrequency = rate;
        PinDataRangesPcm[PinDataRangesPcmCount].MaximumSampleFrequency = rate;

        // ac97 sets SampleSize to the frame size rather than leaving it zero
        // (wavepciminiport.cpp:750). For 16-bit stereo that is 4 bytes.
        PinDataRangesPcm[PinDataRangesPcmCount].DataRange.SampleSize =
            PinDataRangePcm.MaximumChannels *
            (PinDataRangePcm.MaximumBitsPerSample / 8);

        PinDataRangesOut[PinDataRangesPcmCount] =
            (PKSDATARANGE)&PinDataRangesPcm[PinDataRangesPcmCount];
        PinDataRangesIn[PinDataRangesPcmCount] =
            (PKSDATARANGE)&PinDataRangesPcm[PinDataRangesPcmCount];

        PinDataRangesPcmCount++;
    }

    if (PinDataRangesPcmCount == 0)
    {
        // The codec answered and listed neither rate. Advertise the pair
        // anyway rather than a pin with no formats at all, which sysaudio
        // cannot build a graph from; ValidateFormat's own bitmap check is
        // still there to refuse a rate the hardware really will not take.
        DOUT(DBG_WARNING, ("BuildPcmDataRanges: codec bitmap %08X lists neither "
            "44.1 nor 48 kHz - advertising the pair regardless", rates));

        for (ULONG i = 0; i < SIZEOF_ARRAY(PinAdvertisedRateHz); i++)
        {
            ULONG rate = PinAdvertisedRateHz[i];

            PinDataRangesPcm[i] = PinDataRangePcm;
            PinDataRangesPcm[i].MinimumSampleFrequency = rate;
            PinDataRangesPcm[i].MaximumSampleFrequency = rate;
            PinDataRangesPcm[i].DataRange.SampleSize =
                PinDataRangePcm.MaximumChannels *
                (PinDataRangePcm.MaximumBitsPerSample / 8);

            PinDataRangesOut[i] = (PKSDATARANGE)&PinDataRangesPcm[i];
            PinDataRangesIn[i]  = (PKSDATARANGE)&PinDataRangesPcm[i];
        }

        PinDataRangesPcmCount = SIZEOF_ARRAY(PinAdvertisedRateHz);
    }

    // Patch the streaming pins' counts down from the compile-time array size
    // to what was actually filled. Matching on the DataRanges pointer rather
    // than on a pin index is ac97's idiom (wavepciminiport.cpp:783-789) and it
    // survives the pin order changing. The two bridge pins are left alone:
    // they carry PinDataRangePointersBridge and no PCM rate at all.
    for (ULONG p = 0; p < SIZEOF_ARRAY(MiniportWavePins); p++)
    {
        if (MiniportWavePins[p].KsPinDescriptor.DataRanges == PinDataRangesOut ||
            MiniportWavePins[p].KsPinDescriptor.DataRanges == PinDataRangesIn)
        {
            MiniportWavePins[p].KsPinDescriptor.DataRangesCount =
                PinDataRangesPcmCount;
        }
    }

    for (ULONG i = 0; i < PinDataRangesPcmCount; i++)
    {
        DOUT(DBG_PRINT, ("BuildPcmDataRanges: range %u of %u - %u Hz exactly, "
            "%u..%u bit, max %u ch, frame %u bytes",
            i + 1, PinDataRangesPcmCount,
            PinDataRangesPcm[i].MinimumSampleFrequency,
            PinDataRangesPcm[i].MinimumBitsPerSample,
            PinDataRangesPcm[i].MaximumBitsPerSample,
            PinDataRangesPcm[i].MaximumChannels,
            PinDataRangesPcm[i].DataRange.SampleSize));
    }

    DOUT(DBG_PRINT, ("BuildPcmDataRanges: rate mask %08X -> %u discrete range(s) "
        "(was one contiguous 44100..48000 span)", rates, PinDataRangesPcmCount));
}

/*****************************************************************************
 * CMiniportWaveCyclicHda::ValidateFormat
 *****************************************************************************
 * Stage 5bc. Refuses a format the codec cannot play.
 *
 * Stage 5bb narrowed the advertised range to 44100..48000 Hz and that part
 * worked: the log shows NarrowPcmRangeToCodec running before GetDescription(),
 * and the KSPROPERTY_PIN_DATAINTERSECTION that sysaudio issues at pin-create
 * time duly came back with 48000 Hz. Then, on the very next breath, a
 * KSPROPERTY_CONNECTION_DATAFORMAT set arrived asking for 22050 Hz and every
 * layer of the stack waved it through:
 *
 *   - PortCls does not re-check a dynamic format change against the pin's
 *     data ranges. It hands the KSDATAFORMAT straight to the stream's
 *     SetFormat. (The WDK's own msvad sample validates in NewStream only,
 *     and its stream SetFormat says so in a comment - "MSVAD does not
 *     validate the format" - which is fine for a driver with no hardware
 *     behind it and not fine for one with a real DAC.)
 *   - HDAUDIO_BUS_INTERFACE::AllocateRenderDmaEngine does not check either.
 *     It only asks whether the rate is expressible in the stream descriptor's
 *     format register, which 22050 is; it returns success and computes
 *     ConverterFormat 0x4111 itself. It never sees the codec's capabilities.
 *
 * So the data range describes the pin and enforces nothing, and this function
 * is the only place in the whole path where the codec's real answer can stop
 * a rate it cannot produce. The bitmap it consults is the one the hardware
 * gave us: 0x000E05E0 on this codec, bit 3 clear, 22.05 kHz genuinely absent.
 *
 * Refusing costs the caller its format change and nothing else - SetFormat
 * returns before releasing the engine, so a stream that was already playing
 * keeps playing - and it is what makes kmixer resample instead of handing us
 * a rate the DAC will silently drop on the floor.
 */
NTSTATUS CMiniportWaveCyclicHda::ValidateFormat
(
    IN  PKSDATAFORMAT   DataFormat
)
{
    PAGED_CODE();

    if (!DataFormat)
        return STATUS_INVALID_PARAMETER;

    if (DataFormat->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX))
    {
        DOUT(DBG_WARNING, ("ValidateFormat: rejecting - FormatSize %u is too "
            "small for a KSDATAFORMAT_WAVEFORMATEX",
            DataFormat->FormatSize));
        return STATUS_INVALID_PARAMETER;
    }

    PWAVEFORMATEX pWfx = &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx;

    ULONG rate     = pWfx->nSamplesPerSec;
    ULONG channels = pWfx->nChannels;
    ULONG bits     = pWfx->wBitsPerSample;

    if (channels < 1 ||
        channels > PinDataRangePcm.MaximumChannels ||
        bits < PinDataRangePcm.MinimumBitsPerSample ||
        bits > PinDataRangePcm.MaximumBitsPerSample)
    {
        DOUT(DBG_WARNING, ("ValidateFormat: rejecting %u Hz, %u ch, %u bit - "
            "outside the advertised %u..%u bit, max %u ch",
            rate, channels, bits,
            PinDataRangePcm.MinimumBitsPerSample,
            PinDataRangePcm.MaximumBitsPerSample,
            PinDataRangePcm.MaximumChannels));
        return STATUS_INVALID_PARAMETER;
    }

    // Stage 5bi: ask the advertised range list itself rather than a pair of
    // endpoints. The list is now discrete - one entry per rate, built by
    // BuildPcmDataRanges from the codec's bitmap - so this test accepts
    // exactly the set the pin advertises and nothing else. Before 5bi the
    // advertisement was a 44100..48000 span while this function accepted only
    // the two endpoints, which meant the driver was refusing rates it had
    // itself claimed to support.
    BOOLEAN advertised = FALSE;

    for (ULONG i = 0; i < PinDataRangesPcmCount; i++)
    {
        if (PinDataRangesPcm[i].MinimumSampleFrequency == rate)
        {
            advertised = TRUE;
            break;
        }
    }

    if (!advertised)
    {
        DOUT(DBG_WARNING, ("ValidateFormat: rejecting %u Hz - not one of the %u "
            "rate(s) this pin advertises", rate, PinDataRangesPcmCount));
        return STATUS_INVALID_PARAMETER;
    }

    // The range list is built from the bitmap, so this is belt and braces -
    // but it is the check that has hardware behind it, and it costs nothing.
    ULONG rates = m_pAdapterCommon ? m_pAdapterCommon->GetSupportedPcmRates() : 0;

    if (rates != 0)
    {
        BOOLEAN supported = FALSE;

        for (ULONG b = 0; b < HDA_PCM_RATE_COUNT; b++)
        {
            if ((rates & (1UL << b)) && g_HdaPcmRateHz[b] == rate)
            {
                supported = TRUE;
                break;
            }
        }

        if (!supported)
        {
            DOUT(DBG_WARNING, ("ValidateFormat: rejecting %u Hz - the codec's "
                "rate bitmap %08X does not list it", rate, rates));
            return STATUS_INVALID_PARAMETER;
        }
    }

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicHda::Init
(
    IN  PUNKNOWN        UnknownAdapter,
    IN  PRESOURCELIST   ResourceList,
    IN  PPORTWAVECYCLIC Port
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourceList);

    if (!UnknownAdapter || !Port)
        return STATUS_INVALID_PARAMETER;

    m_pPort = Port;
    m_PowerState = PowerDeviceD0;

    // Stage 5be/5bf: this filter is a stereo pair at the default dsound
    // geometry until something sets otherwise.
    m_ChannelConfig.ActiveSpeakerPositions = KSAUDIO_SPEAKER_STEREO;
    m_SpeakerGeometry = KSAUDIO_STEREO_SPEAKER_GEOMETRY_WIDE;

    NTSTATUS status = UnknownAdapter->QueryInterface(IID_IHdaAdapterCommon,
        (PVOID *)&m_pAdapterCommon);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("Init: QueryInterface(IID_IHdaAdapterCommon) failed, status=%08X", status));
        return status;
    }

    // Stage 5bb: before GetDescription() can hand the pin descriptors to
    // PortCls, cut the advertised sample-rate range down to what the codec
    // says it can do. InitCodec() has already run by this point, so the
    // capability read is cached and this costs nothing.
    BuildPcmDataRanges(m_pAdapterCommon);

    // Real HD Audio/UAA architecture (see common.cpp's AcquireBusInterface
    // comment): this codec function device owns no PCI resources of its
    // own, so there is no controller IRQ/MMIO here for us to touch directly.
    // All stream DMA and servicing goes through HDAUDIO_BUS_INTERFACE (via
    // IHdaAdapterCommon), driven per-stream from wavecyclicstream.cpp - see
    // that file's software-timer-based Notify() servicing, since the base
    // bus-interface DDI exposes no per-stream completion interrupt.

    DOUT(DBG_PRINT, ("Init: wave miniport initialized successfully"));

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicHda::GetDescription
(
    OUT     PPCFILTER_DESCRIPTOR *  OutFilterDescriptor
)
{
    PAGED_CODE();
    if (!OutFilterDescriptor)
        return STATUS_INVALID_PARAMETER;
    *OutFilterDescriptor = &MiniportWaveFilterDescriptor;

    // Stage 5bc: this is the moment PortCls copies the data ranges
    // into the subdevice descriptor, so it is the moment worth
    // logging - it proves whether BuildPcmDataRanges got there
    // first, which is a question the Stage 5bb log could only be
    // made to answer by inference. Stage 5bi: print the count and
    // every rate, because there is no longer a single span to print,
    // and a count of zero here would mean the pins went out with no
    // PCM formats at all.
    DOUT(DBG_PRINT, ("GetDescription: publishing %u discrete PCM range(s), "
        "%u..%u bit, max %u ch", PinDataRangesPcmCount,
        PinDataRangePcm.MinimumBitsPerSample,
        PinDataRangePcm.MaximumBitsPerSample,
        PinDataRangePcm.MaximumChannels));

    for (ULONG i = 0; i < PinDataRangesPcmCount; i++)
    {
        DOUT(DBG_PRINT, ("GetDescription:   range %u - %u Hz",
            i + 1, PinDataRangesPcm[i].MinimumSampleFrequency));
    }

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicHda::DataRangeIntersection
(
    IN      ULONG           PinId,
    IN      PKSDATARANGE    DataRange,
    IN      PKSDATARANGE    MatchingDataRange,
    IN      ULONG           OutputBufferLength,
    OUT     PVOID           ResultantFormat     OPTIONAL,
    OUT     PULONG          ResultantFormatLength
)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);

    *ResultantFormatLength = 0;
    return STATUS_NOT_IMPLEMENTED; // PortCls falls back to its default handler
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicHda::NewStream
(
    OUT     PMINIPORTWAVECYCLICSTREAM * Stream,
    IN      PUNKNOWN                    OuterUnknown    OPTIONAL,
    IN      POOL_TYPE                   PoolType,
    IN      ULONG                       Pin,
    IN      BOOLEAN                     Capture,
    IN      PKSDATAFORMAT               DataFormat,
    OUT     PDMACHANNEL *               DmaChannel,
    OUT     PSERVICEGROUP *             ServiceGroup
)
{
    PAGED_CODE();

    DOUT(DBG_PRINT, ("NewStream: entry, Pin=%u, Capture=%u", Pin, Capture));

    if (!Stream || !DmaChannel || !ServiceGroup)
        return STATUS_INVALID_PARAMETER;

    // WaveCyclic (unlike WavePci) requires a real IDmaChannel object back
    // from NewStream - see dmachannel.h's header comment. Build it first so
    // the stream object can hand it its engine handle once SetFormat runs.
    CHdaDmaChannel *pDmaChannel = new(PoolType) CHdaDmaChannel(NULL);
    if (!pDmaChannel)
        return STATUS_INSUFFICIENT_RESOURCES;

    pDmaChannel->AddRef();

    NTSTATUS status = pDmaChannel->Init(m_pAdapterCommon);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("NewStream: dma channel Init failed, status=%08X", status));
        pDmaChannel->Release();
        return status;
    }

    CMiniportWaveCyclicStreamHda *pStream =
        new(PoolType) CMiniportWaveCyclicStreamHda(OuterUnknown);
    if (!pStream)
    {
        pDmaChannel->Release();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pStream->AddRef();

    status = pStream->Init(this, m_pAdapterCommon, Capture, pDmaChannel,
        DataFormat, ServiceGroup);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("NewStream: stream Init failed, status=%08X", status));
        pStream->Release();
        pDmaChannel->Release();
        return status;
    }

    *Stream = (PMINIPORTWAVECYCLICSTREAM)pStream;
    *DmaChannel = (PDMACHANNEL)pDmaChannel;

    // *Stream now holds the reference taken by pStream->AddRef() above, and
    // *DmaChannel holds the reference taken by pDmaChannel->AddRef() above -
    // both are handed to PortCls, matching NewStream's OUT-param contract
    // (caller receives one reference each, no extra Release() needed here).

    DOUT(DBG_PRINT, ("NewStream: success, Pin=%u, Capture=%u", Pin, Capture));

    return STATUS_SUCCESS;
}

STDMETHODIMP_(void) CMiniportWaveCyclicHda::PowerChangeNotify
(
    IN  POWER_STATE  NewState
)
{
    PAGED_CODE();
    m_PowerState = NewState.DeviceState;
    // TODO: on D0 return from D3Cold, re-run IHdaAdapterCommon::InitCodec()
    // (mirrors stwrt64.sys's DoubleResetHandshake D3Cold-detection reasoning
    // documented in HANDOFF.md / common.cpp).
}
