/*****************************************************************************
 * mintopo.cpp
 *****************************************************************************
 * See mintopo.h. Static 4-pin, 7-node topology filter descriptor:
 *
 *   PIN_TOPO_WAVEOUT_DEST  <- the wave filter's PIN_WAVEOUT_BRIDGE
 *   PIN_TOPO_WAVEIN_SOURCE -> the wave filter's PIN_WAVEIN_BRIDGE
 *   PIN_TOPO_LINEOUT_DEST     physical speaker/line-out jack
 *   PIN_TOPO_MIC_SOURCE       physical mic/line-in jack
 *
 * with each bridge pin joined to its physical jack pin through a chain of
 * nodes. The pin and node indices live in shared.h's HdaTopoPin /
 * HdaTopoNode enums, which adapter.cpp's PcRegisterPhysicalConnection calls
 * also use.
 *
 * Stage 5au: this filter used to have zero nodes and two direct pin-to-pin
 * connections - PCFILTER_NODE on both sides. That bugchecked sysaudio.sys
 * (0x7E, c0000005, NULL deref at sysaudio+0x1d2b4) the first time it was
 * walked, which was as soon as Stage 5at gave the wave filter real bridge
 * pins for sysaudio to follow into here. sysaudio resolves each connection
 * endpoint to a node object; PCFILTER_NODE on both sides resolves to nothing
 * and it dereferences the NULL. Note that not one of the seventeen
 * connections in msvad\simple\toptable.h is pin-to-pin - every single one
 * passes through a node. That constraint still holds below.
 *
 * Stage 5ay (HANDOFF item n36): the two nodes Stage 5au added were bare
 * property-less KSNODETYPE_SUM pass-throughs, chosen as the minimum shape
 * that made each half of this filter traversable while adding no property
 * surface to get wrong. That was the right call for a crash fix and the
 * wrong shape to ship. wdmaud builds the Windows mixer device out of THIS
 * filter, and a topology whose only nodes are property-less summers offers
 * it nothing to expose: the volume sliders come up greyed, and - since XP's
 * kmixer takes the per-stream gain it applies from that same mixer line -
 * that is also the leading explanation for why system playback was silent
 * while kstest, which writes straight to the pin and never goes through
 * kmixer, was audible on the same boot.
 *
 * So the render path now carries a real volume and mute node for the wave
 * line, and another pair for the master, either side of the summer; capture
 * carries a volume node. All of them are backed by the codec's own output
 * amplifier through IHdaAdapterCommon (see common.cpp's ProgramOutputAmp) -
 * not cached in software the way msvad does it - except the capture node,
 * which has no ADC path in this backport to drive and is state-only.
 */

static char STR_MODULENAME[] = "stwrtxp Topology: ";
#include "mintopo.h"

//
// VT_I4 / VT_BOOL are the VARIANT type tags a KSPROPERTY_DESCRIPTION carries
// in its PropTypeSet.Id. ks.h happens to declare them in an unnamed enum, so
// these guards only bite if that ever changes.
//
#ifndef VT_I4
#define VT_I4   3
#endif
#ifndef VT_BOOL
#define VT_BOOL 11
#endif

#pragma code_seg("PAGE")

//
// Every pin on this filter is a bridge pin, so they all advertise the same
// plain analog KSDATARANGE - not a KSDATARANGE_AUDIO. Nobody opens a bridge
// pin as a stream, so there is no PCM range to negotiate here; the wave
// filter's own bridge pins advertise an identical range, which is what the
// two ends of a physical connection are supposed to agree on. Verbatim in
// shape from msvad\simple\toptable.h's PinDataRangesBridge.
//
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

static PCPIN_DESCRIPTOR MiniportPins[] =
{
    // PIN_TOPO_WAVEOUT_DEST: render bridge (system-side, connects up to the
    // wave filter's PIN_WAVEOUT_BRIDGE - see adapter.cpp's PcRegisterPhysicalConnection
    // calls). KsPinDescriptor field order/count here is taken verbatim from
    // the real KSPIN_DESCRIPTOR layout (ks.h) - confirmed against the WDK
    // 7600 msvad\simple sample's toptable.h, which uses this same static-
    // aggregate-initializer style (11 fields: I/faceCount, Interfaces,
    // MediumCount, Mediums, DataRangeCount, DataRanges, DataFlow,
    // Communication, Category, Name, Reserved).
    {
        0, 0, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // PIN_TOPO_WAVEIN_SOURCE: capture bridge (connects up to the wave
    // filter's PIN_WAVEIN_BRIDGE).
    {
        0, 0, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // PIN_TOPO_LINEOUT_DEST: physical speaker/line-out jack. THIS category is what sysaudio's
    // topology walk actually keys on to recognize a render endpoint exists
    // at all - a generic KSCATEGORY_AUDIO pin here (the original bug) is
    // invisible to it, which is why no "audio specific settings"/mixer line
    // ever appeared even though the codec itself was fully initialized and
    // unmuted. Matches msvad\simple\toptable.h's KSPIN_TOPO_LINEOUT_DEST.
    {
        0, 0, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER,
            NULL,
            0
        }
    },
    // PIN_TOPO_MIC_SOURCE: physical mic/line-in jack. Same reasoning, for capture.
    // Matches msvad\simple\toptable.h's KSPIN_TOPO_MIC_SOURCE.
    {
        0, 0, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(PinDataRangePointersBridge),
            PinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE,
            NULL,
            0
        }
    }
};

/*****************************************************************************
 * Node property handlers
 *****************************************************************************
 * PCPFNPROPERTY_HANDLER is a plain function pointer, so these are file-static
 * functions that recover the miniport object by casting
 * PropertyRequest->MajorTarget back - which is exactly the pointer
 * CreateMiniportTopologyHda handed PortCls, and is the pattern both msvad and
 * sb16 use. PropertyRequest->Node says which node is being addressed.
 */

static NTSTATUS PropertyHandler_Volume(IN PPCPROPERTY_REQUEST PropertyRequest);
static NTSTATUS PropertyHandler_Mute(IN PPCPROPERTY_REQUEST PropertyRequest);
static NTSTATUS PropertyHandler_CpuResources(IN PPCPROPERTY_REQUEST PropertyRequest);
static NTSTATUS EventHandler_ControlChange(IN PPCEVENT_REQUEST EventRequest);

//
// Stage 5bg. Both of the last two Windows Media Player logs contain twelve
// IOCTL_KS_ENABLE_EVENT requests refused with STATUS_PROPSET_NOT_FOUND
// (C0000230), because until now this driver published no event sets at all.
//
// Stage 5bd and 5be both dismissed those refusals on the grounds that no WDK
// sample implements event support. That was wrong: sb16\tables.h publishes
// exactly this set, and sb16\mintopo.cpp implements the handler. The earlier
// search had been narrowed to msvad and ac97, which do not.
//
// KSEVENT_CONTROL_CHANGE is how a driver tells the mixer that a control moved
// behind its back - a hardware volume wheel, say. This backport never fires
// one: the codec's unsolicited-response path is not wired up, so nothing here
// can notice a control changing except the SET that changed it. Publishing
// the set therefore only makes ENABLE succeed rather than fail; the events
// themselves never arrive, exactly as if the hardware never changed. That is
// the honest state of it and it is strictly better than a refusal, because a
// refusal is a fact about the driver that a client can misread as a fact
// about the graph.
//
static PCEVENT_ITEM NodeControlChangeEvent[] =
{
    {
        &KSEVENTSETID_AudioControlChange,
        KSEVENT_CONTROL_CHANGE,
        KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT,
        EventHandler_ControlChange
    }
};

//
// KSPROPERTY_AUDIO_CPU_RESOURCES is on every control node deliberately.
// kmixer asks it of each node it finds and a node that refuses the question
// is one kmixer may decline to build a line for; answering
// KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU is also simply true here, since the
// gain really is applied by the codec's amplifier and costs the CPU nothing.
//
static PCPROPERTY_ITEM PropertiesVolume[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_Volume
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_CPU_RESOURCES,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_CpuResources
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationVolume, PropertiesVolume,
                                     NodeControlChangeEvent);

static PCPROPERTY_ITEM PropertiesMute[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_MUTE,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_Mute
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_CPU_RESOURCES,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_CpuResources
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationMute, PropertiesMute,
                                     NodeControlChangeEvent);

//
// The node table. Order must match shared.h's HdaTopoNode exactly.
//
// The Name GUIDs are the well-known KSAUDFNAME_* ones, which wdmaudio.inf
// registers under HKLM\SYSTEM\CurrentControlSet\Control\MediaCategories on
// every XP install - that is where the mixer line gets its displayed name
// from, and it is why msvad's nodes carry them rather than NULL. The two
// summers stay nameless: nothing displays a summer.
//
static PCNODE_DESCRIPTOR TopologyNodes[] =
{
    // NODE_TOPO_WAVEOUT_VOLUME
    {
        0,                          // Flags
        &AutomationVolume,          // AutomationTable
        &KSNODETYPE_VOLUME,         // Type
        &KSAUDFNAME_WAVE_VOLUME     // Name
    },
    // NODE_TOPO_WAVEOUT_MUTE
    {
        0,
        &AutomationMute,
        &KSNODETYPE_MUTE,
        &KSAUDFNAME_WAVE_MUTE
    },
    // NODE_TOPO_LINEOUT_MIX - the Stage 5au summer, unchanged.
    {
        0,
        NULL,
        &KSNODETYPE_SUM,
        NULL
    },
    // NODE_TOPO_LINEOUT_VOLUME - the master slider.
    {
        0,
        &AutomationVolume,
        &KSNODETYPE_VOLUME,
        &KSAUDFNAME_MASTER_VOLUME
    },
    // NODE_TOPO_LINEOUT_MUTE
    {
        0,
        &AutomationMute,
        &KSNODETYPE_MUTE,
        &KSAUDFNAME_MASTER_MUTE
    },
    // NODE_TOPO_MIC_VOLUME
    {
        0,
        &AutomationVolume,
        &KSNODETYPE_VOLUME,
        &KSAUDFNAME_MIC_VOLUME
    },
    // NODE_TOPO_WAVEIN_MIX - the Stage 5au capture summer, unchanged.
    {
        0,
        NULL,
        &KSNODETYPE_SUM,
        NULL
    }
};

// Pin 1 of a node is its input and pin 0 its output, which is why every
// connection into a node below ends at 1 and every connection out of one
// starts at 0. PCFILTER_NODE means "this filter's own pin", and it appears
// on exactly one side of each connection - never both (Stage 5au).
static PCCONNECTION_DESCRIPTOR MiniportConnections[] =
{
    //  FromNode                    FromPin                 ToNode                      ToPin
    // render: wave bridge -> wave volume -> wave mute -> sum -> master volume -> master mute -> speaker jack
    { PCFILTER_NODE,                PIN_TOPO_WAVEOUT_DEST,  NODE_TOPO_WAVEOUT_VOLUME,   1                       },
    { NODE_TOPO_WAVEOUT_VOLUME,     0,                      NODE_TOPO_WAVEOUT_MUTE,     1                       },
    { NODE_TOPO_WAVEOUT_MUTE,       0,                      NODE_TOPO_LINEOUT_MIX,      1                       },
    { NODE_TOPO_LINEOUT_MIX,        0,                      NODE_TOPO_LINEOUT_VOLUME,   1                       },
    { NODE_TOPO_LINEOUT_VOLUME,     0,                      NODE_TOPO_LINEOUT_MUTE,     1                       },
    { NODE_TOPO_LINEOUT_MUTE,       0,                      PCFILTER_NODE,              PIN_TOPO_LINEOUT_DEST   },
    // capture: mic jack -> mic volume -> sum -> wave capture bridge
    { PCFILTER_NODE,                PIN_TOPO_MIC_SOURCE,    NODE_TOPO_MIC_VOLUME,       1                       },
    { NODE_TOPO_MIC_VOLUME,         0,                      NODE_TOPO_WAVEIN_MIX,       1                       },
    { NODE_TOPO_WAVEIN_MIX,         0,                      PCFILTER_NODE,              PIN_TOPO_WAVEIN_SOURCE  }
};

static PCFILTER_DESCRIPTOR MiniportFilterDescriptor =
{
    0,                                      // Version
    &AutomationFilter,                      // AutomationTable (shared.h)
    sizeof(PCPIN_DESCRIPTOR),               // PinSize
    SIZEOF_ARRAY(MiniportPins),             // PinCount
    MiniportPins,                           // Pins
    sizeof(PCNODE_DESCRIPTOR),              // NodeSize
    SIZEOF_ARRAY(TopologyNodes),            // NodeCount
    TopologyNodes,                          // Nodes
    SIZEOF_ARRAY(MiniportConnections),      // ConnectionCount
    MiniportConnections,                    // Connections
    0,                                      // CategoryCount - PortCls supplies the defaults
    NULL                                    // Categories
};

/*****************************************************************************
 * NodeToGainStage
 *****************************************************************************
 * Which IHdaAdapterCommon gain stage a node's property request belongs to.
 * Two KS stages share one hardware attenuator; common.cpp's ProgramOutputAmp
 * sums them.
 */
static ULONG NodeToGainStage(IN ULONG Node)
{
    switch (Node)
    {
        case NODE_TOPO_WAVEOUT_VOLUME:
        case NODE_TOPO_WAVEOUT_MUTE:
            return HDA_GAIN_WAVE;

        case NODE_TOPO_LINEOUT_VOLUME:
        case NODE_TOPO_LINEOUT_MUTE:
            return HDA_GAIN_MASTER;

        default:
            return HDA_GAIN_CAPTURE;
    }
}

/*****************************************************************************
 * NodeChannelCount
 *****************************************************************************
 * How many channels a volume node actually has. This is not cosmetic: a
 * BASICSUPPORT reply that carries the MULTICHANNEL flag is declaring "one
 * stepping range per channel", so the member count IS the channel count as
 * far as wdmaud is concerned. Answer 1 for a stereo node and wdmaud builds a
 * one-channel, UNIFORM mixer control and thereafter only ever addresses
 * channel 0 - which leaves the right speaker at 0 dB forever. That was the
 * Stage 5br bug: the slider moved, the left amp followed it, and the right
 * amp never budged, so the volume audibly did nothing.
 */
static ULONG NodeChannelCount(IN ULONG Node)
{
    switch (Node)
    {
        case NODE_TOPO_MIC_VOLUME:
            return 1;           // the capture line really is mono

        default:
            return 2;           // both render gain stages are stereo
    }
}

/*****************************************************************************
 * BasicSupportStepped
 *****************************************************************************
 * The KSPROPERTY_TYPE_BASICSUPPORT answer for a LONG-valued property with a
 * stepped range - i.e. a volume node. This is the reply that actually decides
 * whether the Windows slider is live: it is where the caller learns the
 * property is settable and what range and granularity to draw. Shape is
 * msvad's PropertyHandler_BasicSupportVolume (basetopo.cpp).
 *
 * Three answers, by buffer size: the full description with its range, the
 * KSPROPERTY_DESCRIPTION alone (a caller sizing its buffer, which learns the
 * full size from DescriptionSize and comes back with a bigger one), or - for
 * a caller that only wants to know the verbs - the access flags as a bare
 * ULONG.
 *
 * The middle case gates on sizeof(KSPROPERTY_DESCRIPTION), NOT on description
 * + members header. A caller doing the documented two-step sizing probe asks
 * with exactly sizeof(KSPROPERTY_DESCRIPTION) bytes; gating any higher drops
 * it into the bare-ULONG branch, so it never sees DescriptionSize and never
 * comes back for the range. See Stage 5bn.
 */
static NTSTATUS BasicSupportStepped
(
    IN  PPCPROPERTY_REQUEST PropertyRequest,
    IN  ULONG               AccessFlags,
    IN  LONG                Minimum,
    IN  LONG                Maximum,
    IN  LONG                Step,
    IN  ULONG               Channels
)
{
    PAGED_CODE();

    if (Channels < 1)
        Channels = 1;

    const ULONG cbFull = sizeof(KSPROPERTY_DESCRIPTION) +
                         sizeof(KSPROPERTY_MEMBERSHEADER) +
                         Channels * sizeof(KSPROPERTY_STEPPING_LONG);

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        PKSPROPERTY_DESCRIPTION desc =
            (PKSPROPERTY_DESCRIPTION)PropertyRequest->Value;

        desc->AccessFlags       = AccessFlags;
        desc->DescriptionSize   = cbFull;
        desc->PropTypeSet.Set   = KSPROPTYPESETID_General;
        desc->PropTypeSet.Id    = VT_I4;
        desc->PropTypeSet.Flags = 0;
        desc->MembersListCount  = 1;
        desc->Reserved          = 0;

        if (PropertyRequest->ValueSize >= cbFull)
        {
            PKSPROPERTY_MEMBERSHEADER members =
                (PKSPROPERTY_MEMBERSHEADER)(desc + 1);

            members->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
            members->MembersSize  = sizeof(KSPROPERTY_STEPPING_LONG);
            members->MembersCount = Channels;
            members->Flags        = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL;

            PKSPROPERTY_STEPPING_LONG range =
                (PKSPROPERTY_STEPPING_LONG)(members + 1);

            // MULTICHANNEL means one range per channel, and every channel of
            // this attenuator has the same range - but the count has to be
            // right or the caller stops at channel 0.
            for (ULONG i = 0; i < Channels; i++)
            {
                range[i].Bounds.SignedMinimum = Minimum;
                range[i].Bounds.SignedMaximum = Maximum;
                range[i].SteppingDelta        = (ULONG)Step;
                range[i].Reserved             = 0;
            }

            PropertyRequest->ValueSize = cbFull;
        }
        else
        {
            PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        }

        return STATUS_SUCCESS;
    }

    if (PropertyRequest->ValueSize >= sizeof(ULONG))
    {
        *(PULONG)PropertyRequest->Value = AccessFlags;
        PropertyRequest->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    PropertyRequest->ValueSize = 0;
    return STATUS_BUFFER_TOO_SMALL;
}

/*****************************************************************************
 * BasicSupportBoolean
 *****************************************************************************
 * The same, for a BOOL-valued property with no range - a mute node. A
 * KSPROPERTY_DESCRIPTION with MembersListCount 0 is the documented way to say
 * "settable, no enumerable member list".
 */
static NTSTATUS BasicSupportBoolean
(
    IN  PPCPROPERTY_REQUEST PropertyRequest,
    IN  ULONG               AccessFlags
)
{
    PAGED_CODE();

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        PKSPROPERTY_DESCRIPTION desc =
            (PKSPROPERTY_DESCRIPTION)PropertyRequest->Value;

        desc->AccessFlags       = AccessFlags;
        desc->DescriptionSize   = sizeof(KSPROPERTY_DESCRIPTION);
        desc->PropTypeSet.Set   = KSPROPTYPESETID_General;
        desc->PropTypeSet.Id    = VT_BOOL;
        desc->PropTypeSet.Flags = 0;
        desc->MembersListCount  = 0;
        desc->Reserved          = 0;

        PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        return STATUS_SUCCESS;
    }

    if (PropertyRequest->ValueSize >= sizeof(ULONG))
    {
        *(PULONG)PropertyRequest->Value = AccessFlags;
        PropertyRequest->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    PropertyRequest->ValueSize = 0;
    return STATUS_BUFFER_TOO_SMALL;
}

/*****************************************************************************
 * MiniportFromRequest
 *****************************************************************************
 * MajorTarget is the PUNKNOWN CreateMiniportTopologyHda handed PortCls, which
 * it built as (PUNKNOWN)(PMINIPORTTOPOLOGY)pTopology - so this cast is that
 * one run backwards, and the PMINIPORTTOPOLOGY step in the middle is not
 * optional on a class with two base interfaces.
 */
static CMiniportTopologyHda * MiniportFromRequest
(
    IN  PPCPROPERTY_REQUEST PropertyRequest
)
{
    return (CMiniportTopologyHda *)
        (PMINIPORTTOPOLOGY)PropertyRequest->MajorTarget;
}

/*****************************************************************************
 * PropertyHandler_Volume
 *****************************************************************************
 * KSPROPERTY_AUDIO_VOLUMELEVEL on any of the three volume nodes.
 *
 * GET and SET arrive as a KSNODEPROPERTY_AUDIO_CHANNEL, and PortCls points
 * Instance at the Channel field that follows the node property header. -1
 * means "every channel".
 */
static NTSTATUS PropertyHandler_Volume(IN PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();

    if (!PropertyRequest || !PropertyRequest->MajorTarget)
        return STATUS_INVALID_PARAMETER;

    CMiniportTopologyHda *that = MiniportFromRequest(PropertyRequest);
    PADAPTERCOMMON adapter = that ? that->AdapterCommon() : NULL;

    if (!adapter)
        return STATUS_DEVICE_NOT_READY;

    ULONG stage = NodeToGainStage(PropertyRequest->Node);

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        LONG minimum = 0;
        LONG maximum = 0;
        LONG step    = 0x10000;

        adapter->GetVolumeRange(&minimum, &maximum, &step);

        // asked vs answered, because they are not the same thing and the
        // Stage 5bn bug was invisible while only "answered" was logged.
        ULONG asked = PropertyRequest->ValueSize;

        ULONG channels = NodeChannelCount(PropertyRequest->Node);

        NTSTATUS basicStatus = BasicSupportStepped(PropertyRequest,
            KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET,
            minimum, maximum, step, channels);

        DOUT(DBG_PRINT, ("PropertyHandler_Volume: node=%u BASICSUPPORT range %d..%d "
            "step %d channels=%u asked=%d answered=%d -> %08X", PropertyRequest->Node,
            minimum, maximum, step, channels, asked, PropertyRequest->ValueSize,
            basicStatus));

        return basicStatus;
    }

    if (PropertyRequest->InstanceSize < sizeof(LONG))
    {
        DOUT(DBG_WARNING, ("PropertyHandler_Volume: node=%u instance size %d too "
            "small for a channel", PropertyRequest->Node, PropertyRequest->InstanceSize));
        return STATUS_INVALID_PARAMETER;
    }

    LONG     channel  = *(PLONG)PropertyRequest->Instance;
    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    LONG     level    = 0;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        if (PropertyRequest->ValueSize >= sizeof(LONG))
        {
            level = adapter->GetVolumeLevel(stage, channel);
            *(PLONG)PropertyRequest->Value = level;
            PropertyRequest->ValueSize = sizeof(LONG);
            ntStatus = STATUS_SUCCESS;
        }
        else
        {
            PropertyRequest->ValueSize = 0;
            ntStatus = STATUS_BUFFER_TOO_SMALL;
        }
    }
    else if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
    {
        if (PropertyRequest->ValueSize >= sizeof(LONG))
        {
            level = *(PLONG)PropertyRequest->Value;
            ntStatus = adapter->SetVolumeLevel(stage, channel, level);
        }
        else
        {
            PropertyRequest->ValueSize = 0;
            ntStatus = STATUS_BUFFER_TOO_SMALL;
        }
    }

    DOUT(DBG_PRINT, ("PropertyHandler_Volume: node=%u stage=%u ch=%d verb=%08X "
        "level=%d -> %08X", PropertyRequest->Node, stage, channel,
        PropertyRequest->Verb, level, ntStatus));

    return ntStatus;
}

/*****************************************************************************
 * PropertyHandler_Mute
 *****************************************************************************
 * KSPROPERTY_AUDIO_MUTE. Carries a channel the same way volume does; the
 * codec's mute bit is per-channel but there is no sane UI for half a mute, so
 * both channels move together and the channel is only read for validation.
 */
static NTSTATUS PropertyHandler_Mute(IN PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();

    if (!PropertyRequest || !PropertyRequest->MajorTarget)
        return STATUS_INVALID_PARAMETER;

    CMiniportTopologyHda *that = MiniportFromRequest(PropertyRequest);
    PADAPTERCOMMON adapter = that ? that->AdapterCommon() : NULL;

    if (!adapter)
        return STATUS_DEVICE_NOT_READY;

    ULONG stage = NodeToGainStage(PropertyRequest->Node);

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        NTSTATUS basicStatus = BasicSupportBoolean(PropertyRequest,
            KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET);

        DOUT(DBG_PRINT, ("PropertyHandler_Mute: node=%u BASICSUPPORT size=%d -> %08X",
            PropertyRequest->Node, PropertyRequest->ValueSize, basicStatus));

        return basicStatus;
    }

    if (PropertyRequest->InstanceSize < sizeof(LONG))
    {
        DOUT(DBG_WARNING, ("PropertyHandler_Mute: node=%u instance size %d too "
            "small for a channel", PropertyRequest->Node, PropertyRequest->InstanceSize));
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    LONG     value    = 0;

    // A KS mute value is a BOOL, which is four bytes - not a BOOLEAN.
    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        if (PropertyRequest->ValueSize >= sizeof(LONG))
        {
            value = adapter->GetMute(stage) ? 1 : 0;
            *(PLONG)PropertyRequest->Value = value;
            PropertyRequest->ValueSize = sizeof(LONG);
            ntStatus = STATUS_SUCCESS;
        }
        else
        {
            PropertyRequest->ValueSize = 0;
            ntStatus = STATUS_BUFFER_TOO_SMALL;
        }
    }
    else if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
    {
        if (PropertyRequest->ValueSize >= sizeof(LONG))
        {
            value = *(PLONG)PropertyRequest->Value;
            ntStatus = adapter->SetMute(stage, (BOOLEAN)(value != 0));
        }
        else
        {
            PropertyRequest->ValueSize = 0;
            ntStatus = STATUS_BUFFER_TOO_SMALL;
        }
    }

    DOUT(DBG_PRINT, ("PropertyHandler_Mute: node=%u stage=%u verb=%08X value=%d -> %08X",
        PropertyRequest->Node, stage, PropertyRequest->Verb, value, ntStatus));

    return ntStatus;
}

// EventHandler_ControlChange must live OUTSIDE the PAGE segment. PortCls
// invokes miniport event handlers at DISPATCH_LEVEL (it holds its event-list
// lock across the call), so paged code here is not merely an assert failure -
// a page fault at DISPATCH_LEVEL bugchecks. The WDK 7600 sb16 sample does the
// same thing: its mintopo.cpp:1438 drops out of code_seg("PAGE") for
// EventHandler/ServiceEvent and restores it at :1501.
#pragma code_seg()

/*****************************************************************************
 * EventHandler_ControlChange
 *****************************************************************************
 * KSEVENTSETID_AudioControlChange / KSEVENT_CONTROL_CHANGE on any node
 * carrying AutomationVolume or AutomationMute. Structure taken from the WDK
 * 7600 sb16 sample's CMiniportTopologySB16::EventHandler.
 *
 * sb16 validates EventRequest->Node against its single event-bearing node and
 * refuses anything else. That check is unnecessary here: PortCls only routes
 * an event request to a node whose automation table names the event item, so
 * by the time this runs the node is already one of the five that do.
 *
 * See the note on NodeControlChangeEvent above for why nothing ever generates
 * one of these events.
 *
 * Runs at DISPATCH_LEVEL, so there is deliberately no PAGED_CODE() here. The
 * first revision of this function had one; it asserted on the very first
 * IOCTL_KS_ENABLE_EVENT and bugchecked 0x3B (STATUS_BREAKPOINT) during
 * driver install. Everything touched below is non-paged, and LogToFileF now
 * returns immediately above PASSIVE_LEVEL, so these DOUT lines reach
 * DbgPrint only and never the on-disk log.
 */
static NTSTATUS EventHandler_ControlChange(IN PPCEVENT_REQUEST EventRequest)
{
    if (!EventRequest)
        return STATUS_INVALID_PARAMETER;

    CMiniportTopologyHda * that =
        (CMiniportTopologyHda *)(PMINIPORTTOPOLOGY)EventRequest->MajorTarget;

    if (!that)
        return STATUS_INVALID_PARAMETER;

    NTSTATUS ntStatus = STATUS_SUCCESS;

    switch (EventRequest->Verb)
    {
        case PCEVENT_VERB_SUPPORT:
            // "Do you support this event?" - answering STATUS_SUCCESS is the
            // whole of it; there is no value to fill in.
            DOUT(DBG_PRINT, ("EventHandler_ControlChange: node=%u BASICSUPPORT",
                EventRequest->Node));
            break;

        case PCEVENT_VERB_ADD:
            if (EventRequest->EventEntry && that->PortEvents())
            {
                that->PortEvents()->AddEventToEventList(EventRequest->EventEntry);
                DOUT(DBG_PRINT, ("EventHandler_ControlChange: node=%u ADD",
                    EventRequest->Node));
            }
            else
            {
                DOUT(DBG_WARNING, ("EventHandler_ControlChange: node=%u ADD refused, entry=%p portevents=%p",
                    EventRequest->Node, EventRequest->EventEntry, that->PortEvents()));
                ntStatus = STATUS_UNSUCCESSFUL;
            }
            break;

        case PCEVENT_VERB_REMOVE:
            // PortCls owns the event list and unlinks the entry itself. As
            // sb16 notes, there is nothing for the miniport to do here beyond
            // stopping generation - and this one never generates any.
            DOUT(DBG_PRINT, ("EventHandler_ControlChange: node=%u REMOVE",
                EventRequest->Node));
            break;

        default:
            DOUT(DBG_WARNING, ("EventHandler_ControlChange: node=%u unknown verb %u",
                EventRequest->Node, EventRequest->Verb));
            ntStatus = STATUS_INVALID_PARAMETER;
            break;
    }

    return ntStatus;
}

#pragma code_seg("PAGE")

/*****************************************************************************
 * PropertyHandler_CpuResources
 *****************************************************************************
 * KSPROPERTY_AUDIO_CPU_RESOURCES: does this node cost host CPU time? It does
 * not - the gain is applied by the codec's own amplifier - so the answer is
 * KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU. GET only.
 */
static NTSTATUS PropertyHandler_CpuResources(IN PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();

    if (!PropertyRequest)
        return STATUS_INVALID_PARAMETER;

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        if (PropertyRequest->ValueSize >= sizeof(ULONG))
        {
            *(PULONG)PropertyRequest->Value = KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU;
            PropertyRequest->ValueSize = sizeof(ULONG);
            ntStatus = STATUS_SUCCESS;
        }
        else
        {
            PropertyRequest->ValueSize = 0;
            ntStatus = STATUS_BUFFER_TOO_SMALL;
        }
    }
    else if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        if (PropertyRequest->ValueSize >= sizeof(ULONG))
        {
            *(PULONG)PropertyRequest->Value =
                KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;
            PropertyRequest->ValueSize = sizeof(ULONG);
            ntStatus = STATUS_SUCCESS;
        }
        else
        {
            PropertyRequest->ValueSize = 0;
            ntStatus = STATUS_BUFFER_TOO_SMALL;
        }
    }

    DOUT(DBG_PRINT, ("PropertyHandler_CpuResources: node=%u verb=%08X size=%d -> %08X",
        PropertyRequest->Node, PropertyRequest->Verb,
        PropertyRequest->ValueSize, ntStatus));

    return ntStatus;
}

NTSTATUS CreateMiniportTopologyHda
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

    CMiniportTopologyHda *pTopology =
        new(PoolType) CMiniportTopologyHda(UnknownOuter);

    if (!pTopology)
        return STATUS_INSUFFICIENT_RESOURCES;

    *Unknown = (PUNKNOWN)(PMINIPORTTOPOLOGY)pTopology;
    (*Unknown)->AddRef();

    return STATUS_SUCCESS;
}

CMiniportTopologyHda::~CMiniportTopologyHda()
{
    PAGED_CODE();
    if (m_pAdapterCommon)
        m_pAdapterCommon->Release();

    // Stage 5bg, matching sb16's destructor.
    if (m_pPortEvents)
    {
        m_pPortEvents->Release();
        m_pPortEvents = NULL;
    }
}

/*****************************************************************************
 * CMiniportTopologyHda::NonDelegatingQueryInterface
 *****************************************************************************
 * Required body for the DECLARE_STD_UNKNOWN() QI this class inherits -
 * modeled on the WDK 7600 ac97\driver sample's CAC97MiniportTopology
 * equivalent (mintopo.cpp).
 */
STDMETHODIMP_(NTSTATUS) CMiniportTopologyHda::NonDelegatingQueryInterface
(
    IN      REFIID  Interface,
    OUT     PVOID * Object
)
{
    PAGED_CODE();
    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = (PVOID)(PUNKNOWN)(PMINIPORTTOPOLOGY)this;
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = (PVOID)(PMINIPORT)this;
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
    {
        *Object = (PVOID)(PMINIPORTTOPOLOGY)this;
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }

    ((PUNKNOWN)(*Object))->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopologyHda::Init
(
    IN  PUNKNOWN        UnknownAdapter,
    IN  PRESOURCELIST   ResourceList,
    IN  PPORTTOPOLOGY   Port
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourceList);

    if (!UnknownAdapter)
        return STATUS_INVALID_PARAMETER;

    NTSTATUS status = UnknownAdapter->QueryInterface(IID_IHdaAdapterCommon,
        (PVOID *)&m_pAdapterCommon);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("Init: QueryInterface(IID_IHdaAdapterCommon) failed, status=%08X", status));
        return status;
    }

    // Stage 5bg: needed to register KSEVENT_CONTROL_CHANGE entries. Failure
    // is not fatal - the event handler refuses PCEVENT_VERB_ADD without it,
    // which is no worse than the STATUS_PROPSET_NOT_FOUND clients got before
    // the event set was published at all. Port may be NULL in principle;
    // guard rather than assume.
    if (Port)
    {
        NTSTATUS eventStatus = Port->QueryInterface(IID_IPortEvents,
            (PVOID *)&m_pPortEvents);
        if (!NT_SUCCESS(eventStatus))
        {
            m_pPortEvents = NULL;
            DOUT(DBG_WARNING, ("Init: QueryInterface(IID_IPortEvents) failed, status=%08X - control-change events unavailable", eventStatus));
        }
        else
        {
            DOUT(DBG_PRINT, ("Init: IPortEvents acquired, %p", m_pPortEvents));
        }
    }

    DOUT(DBG_PRINT, ("Init: topology miniport initialized successfully"));

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopologyHda::GetDescription
(
    OUT     PPCFILTER_DESCRIPTOR *  OutFilterDescriptor
)
{
    PAGED_CODE();
    if (!OutFilterDescriptor)
        return STATUS_INVALID_PARAMETER;
    *OutFilterDescriptor = &MiniportFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopologyHda::DataRangeIntersection
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

    // Not implemented for this minimal topology filter - PortCls falls back
    // to its own default intersection handler when this returns
    // STATUS_NOT_IMPLEMENTED (standard pattern, matches ac97 sample).
    *ResultantFormatLength = 0;
    return STATUS_NOT_IMPLEMENTED;
}
