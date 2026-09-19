/*****************************************************************************
 * mintopo.h
 *****************************************************************************
 * The topology miniport. Per HANDOFF.md's scope ("basic playback, ideally +
 * recording", explicitly NOT full DTS/EQ/mixer parity), this filter carries
 * the physical-connector pin categories (KSNODETYPE_SPEAKER etc.) the KS
 * filter graph expects, the bridge pins that join it to the wave filter, and
 * - since Stage 5ay - the volume/mute nodes wdmaud builds the Windows mixer
 * device out of. It does not model the codec's internal mixer/selector
 * widgets as KS nodes; the node graph below is a fixed shape, not a
 * projection of the widget graph.
 *
 * Modeled on the WDK 7600 ac97\driver sample's mintopo.h/CAC97MiniportTopology
 * shape (IMiniportTopology + CUnknown, PcNewMiniport-constructed, Init()
 * builds a PCFILTER_DESCRIPTOR), with the node/connection table taken from
 * msvad\simple\toptable.h - see shared.h's HdaTopoNode.
 */

#ifndef _MINTOPO_H_
#define _MINTOPO_H_

#include "shared.h"

NTSTATUS CreateMiniportTopologyHda
(
    OUT     PUNKNOWN *  Unknown,
    IN      REFCLSID    ClassId,
    IN      PUNKNOWN    UnknownOuter    OPTIONAL,
    IN      POOL_TYPE   PoolType
);

class CMiniportTopologyHda : public IMiniportTopology, public CUnknown
{
private:
    PADAPTERCOMMON  m_pAdapterCommon;

    // Stage 5bg: the port's IPortEvents, used to register the event entries
    // that KSEVENTSETID_AudioControlChange / KSEVENT_CONTROL_CHANGE requests
    // arrive with. Optional - Init() carries on if the port does not offer
    // the interface, and the event handler simply refuses in that case.
    // Modeled on the WDK 7600 sb16 sample (mintopo.h's PortEvents member,
    // acquired in Init and released in the destructor).
    PPORTEVENTS     m_pPortEvents;

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopologyHda);
    ~CMiniportTopologyHda();

    // Stage 5ay: the node property handlers in mintopo.cpp are plain
    // functions (that is the PCPFNPROPERTY_HANDLER signature), and they
    // recover this object by casting PropertyRequest->MajorTarget back -
    // the standard msvad/sb16 pattern. They need the adapter to reach the
    // codec's amplifier, hence this accessor. May be NULL if Init() has
    // not run.
    PADAPTERCOMMON AdapterCommon(void)
    {
        return m_pAdapterCommon;
    }

    // Same reason as AdapterCommon above: EventHandler_ControlChange is a
    // plain function (that is the PCPFNEVENT_HANDLER signature) and recovers
    // this object by casting EventRequest->MajorTarget back. May be NULL.
    PPORTEVENTS PortEvents(void)
    {
        return m_pPortEvents;
    }

    // Init is declared by the IMP_IMiniportTopology macro below (matching
    // the real IMiniportTopology::Init signature) - do not redeclare it
    // here, see the analogous note on IMiniportWavePci in wavepciminiport.h.
    IMP_IMiniportTopology;
};

#endif // _MINTOPO_H_
