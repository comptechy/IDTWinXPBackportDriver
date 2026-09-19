/*****************************************************************************
 * wavecyclicminiport.h
 *****************************************************************************
 * WaveCyclic wave miniport for the HDA controller, replacing the earlier
 * WavePci attempt (see the retired wavepciminiport.h/.cpp). The real HD
 * Audio bus driver's DDI (HDAUDIO_BUS_INTERFACE, see common.h/shared.h) is a
 * single-handle, bus-driver-owned-single-buffer model - AllocateRenderDma
 * Engine/AllocateCaptureDmaEngine hand back one opaque HANDLE per stream,
 * and AllocateDmaBuffer hands back one MDL the bus driver itself manages -
 * which is a WaveCyclic fit (one contiguous ring buffer per stream), not
 * WavePci's miniport-owned scatter/gather BDL model. See HANDOFF.md's
 * WaveCyclic pivot writeup for the full rationale.
 *
 * Modeled on the WDK 7600 ac97\driver sample's wavecyclicminiport pattern,
 * but with a real IDmaChannel implementation (CHdaDmaChannel, dmachannel.h)
 * instead of a common-buffer-backed one, since the actual buffer memory is
 * bus-driver-owned rather than allocated via IoGetDmaAdapter here.
 */

#ifndef _WAVECYCLICMINIPORT_H_
#define _WAVECYCLICMINIPORT_H_

#include "shared.h"

NTSTATUS CreateMiniportWaveCyclicHda
(
    OUT     PUNKNOWN *  Unknown,
    IN      REFCLSID    ClassId,
    IN      PUNKNOWN    UnknownOuter    OPTIONAL,
    IN      POOL_TYPE   PoolType
);

class CMiniportWaveCyclicStreamHda;

class CMiniportWaveCyclicHda : public IMiniportWaveCyclic,
                                public IPowerNotify,
                                public CUnknown
{
private:
    PADAPTERCOMMON      m_pAdapterCommon;
    PPORTWAVECYCLIC     m_pPort;
    DEVICE_POWER_STATE  m_PowerState;

    // Stage 5be: the speaker mask last set through
    // KSPROPERTY_AUDIO_CHANNEL_CONFIG on NODE_WAVE_DAC. State only - the
    // codec is programmed from the stream format, not from this - which is
    // also what msvad\pcmex does with its own m_ChannelConfig. Defaulted in
    // Init() to KSAUDIO_SPEAKER_STEREO.
    KSAUDIO_CHANNEL_CONFIG  m_ChannelConfig;

    // Stage 5bf: the speaker geometry last set through
    // KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY on the same node - the other
    // half of what DirectSound's IDirectSound::SetSpeakerConfig decomposes
    // into. A LONG angle in degrees, or KSAUDIO_STEREO_SPEAKER_GEOMETRY_
    // HEADPHONE (-1). Nothing in this driver acts on it; there is no 3D
    // panning here to steer. Defaulted in Init() to _WIDE (20), which is
    // what dsound uses for a plain stereo device.
    LONG                    m_SpeakerGeometry;

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveCyclicHda);
    ~CMiniportWaveCyclicHda();

    // NOTE: Init/NewStream/Service/GetDescription/DataRangeIntersection are
    // declared by the IMP_IMiniportWaveCyclic macro below, matching the real
    // IMiniportWaveCyclic signatures (portcls.h) - unlike IMiniportWavePci's
    // Init, this one has NO ServiceGroup out-param (each stream owns its
    // own, allocated in NewStream), and NewStream both hands back a real
    // PDMACHANNEL (CHdaDmaChannel, not NULL) and a PSERVICEGROUP.
    IMP_IMiniportWaveCyclic;

    STDMETHODIMP_(void) PowerChangeNotify
    (
        IN  POWER_STATE  NewState
    );

    PPORTWAVECYCLIC GetPort(void) { return m_pPort; }

    // The CHANNEL_CONFIG handler in wavecyclicminiport.cpp is a plain
    // function (that is the PCPFNPROPERTY_HANDLER signature) and recovers
    // this object from PropertyRequest->MajorTarget, so it needs a way in -
    // the same accessor pattern as CMiniportTopologyHda::AdapterCommon().
    KSAUDIO_CHANNEL_CONFIG * ChannelConfig(void) { return &m_ChannelConfig; }
    LONG * SpeakerGeometry(void) { return &m_SpeakerGeometry; }

    // Stage 5bc: the gate that actually stops an unplayable rate.
    // Called from CMiniportWaveCyclicStreamHda::SetFormat before it
    // touches the DMA engine - see the implementation's comment in
    // wavecyclicminiport.cpp for why nothing above us does this.
    NTSTATUS ValidateFormat(IN PKSDATAFORMAT DataFormat);

    friend class CMiniportWaveCyclicStreamHda;
};

#endif // _WAVECYCLICMINIPORT_H_
