/*****************************************************************************
 * dmachannel.cpp
 *****************************************************************************
 * See dmachannel.h. Not PAGED_CODE() anywhere in this file: PortCls's own
 * WaveCyclic buffer-copy path may call SystemAddress()/CopyTo()/CopyFrom()
 * at DISPATCH_LEVEL while servicing a stream, so nothing here can touch
 * paged memory or block.
 */

static char STR_MODULENAME[] = "stwrtxp DmaChannel: ";
#include "dmachannel.h"

NTSTATUS CHdaDmaChannel::Init(IN PADAPTERCOMMON AdapterCommon)
{
    if (!AdapterCommon)
        return STATUS_INVALID_PARAMETER;

    m_pAdapterCommon = AdapterCommon;
    m_pAdapterCommon->AddRef();

    m_EngineHandle = NULL;
    m_bEngineHandleValid = FALSE;
    m_pBufferMdl = NULL;
    m_pSystemAddress = NULL;
    m_AllocatedSize = 0;
    m_RequestedSize = 0;
    m_StreamId = 0;
    m_FifoSize = 0;

    return STATUS_SUCCESS;
}

CHdaDmaChannel::~CHdaDmaChannel()
{
    // FreeBuffer() is idempotent (checks m_pBufferMdl) - covers the case
    // where the stream is destroyed without an explicit FreeBuffer() call.
    FreeBuffer();

    if (m_pAdapterCommon)
    {
        m_pAdapterCommon->Release();
        m_pAdapterCommon = NULL;
    }
}

/*****************************************************************************
 * NonDelegatingQueryInterface
 *****************************************************************************
 */
STDMETHODIMP_(NTSTATUS) CHdaDmaChannel::NonDelegatingQueryInterface
(
    IN  REFIID  Interface,
    OUT PVOID * Object
)
{
    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = (PVOID)(PUNKNOWN)(PDMACHANNEL)this;
    }
    else if (IsEqualGUIDAligned(Interface, IID_IDmaChannel))
    {
        *Object = (PVOID)(PDMACHANNEL)this;
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
 * AllocateBuffer / FreeBuffer
 *****************************************************************************
 * The actual buffer is bus-driver-owned (see this class's header comment) -
 * PhysicalAddressConstraint is unused, there is nothing for us to honor it
 * against; the bus driver's own DMA-capable allocation already satisfies
 * whatever constraint the real hardware needs.
 */
STDMETHODIMP_(NTSTATUS) CHdaDmaChannel::AllocateBuffer
(
    IN      ULONG               BufferSize,
    IN OPTIONAL PPHYSICAL_ADDRESS   PhysicalAddressConstraint
)
{
    UNREFERENCED_PARAMETER(PhysicalAddressConstraint);

    if (!m_bEngineHandleValid)
    {
        DOUT(DBG_ERROR, ("AllocateBuffer: no DMA engine handle yet - "
            "SetFormat must run before PortCls requests the buffer"));
        return STATUS_DEVICE_NOT_READY;
    }

    // Re-allocating over an existing buffer (e.g. a format/size change)
    // isn't needed by this MVP - one buffer for the life of the stream.
    if (m_pBufferMdl)
        return STATUS_SUCCESS;

    PMDL    mdl = NULL;
    SIZE_T  allocatedSize = 0;

    NTSTATUS status = m_pAdapterCommon->AllocateDmaBuffer(m_EngineHandle,
        (SIZE_T)BufferSize, &mdl, &allocatedSize, &m_StreamId, &m_FifoSize);
    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("AllocateBuffer: AllocateDmaBuffer failed, status=%08X", status));
        return status;
    }

    PVOID systemAddress = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
    if (!systemAddress)
    {
        m_pAdapterCommon->FreeDmaBuffer(m_EngineHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_pBufferMdl        = mdl;
    m_pSystemAddress    = systemAddress;
    m_AllocatedSize     = allocatedSize;
    // Stage 5am: report the size the bus driver ACTUALLY gave us, not the
    // one PortCls asked for. HDA hardware wraps its link position register
    // at the real allocation length, so if AllocateDmaBuffer rounded up (it
    // works in 128-byte granules) while we kept telling PortCls the smaller
    // requested figure, GetPosition() would hand PortCls positions past the
    // end of the buffer it thinks it owns. Reporting the allocated size is
    // what makes the two wrap points agree.
    m_RequestedSize     = (ULONG)allocatedSize;

    // Stage 5az: the addresses are logged so that a buffer re-allocated
    // across a format change can be told apart from the one it replaced.
    // If the hardware ever turns out to be DMAing from a stale BDL, this
    // line and GetPosition()'s wrap point are the two places it shows.
    DOUT(DBG_PRINT, ("AllocateBuffer: requested %u, got %Iu bytes, stream ID %u, "
        "fifo %u, sys %p phys %I64X", BufferSize, allocatedSize, m_StreamId,
        m_FifoSize, systemAddress, MmGetPhysicalAddress(systemAddress).QuadPart));

    return STATUS_SUCCESS;
}

STDMETHODIMP_(void) CHdaDmaChannel::FreeBuffer(void)
{
    if (!m_pBufferMdl)
        return;

    if (m_bEngineHandleValid)
    {
        // Stage 5ar: this status used to be discarded, which hid half of
        // the DMA-engine leak. FreeDmaBuffer was failing for exactly the
        // same reason FreeDmaEngine was - the engine was paused rather
        // than reset - and nothing anywhere said so.
        NTSTATUS status = m_pAdapterCommon->FreeDmaBuffer(m_EngineHandle);
        if (!NT_SUCCESS(status))
        {
            DOUT(DBG_ERROR, ("FreeBuffer: FreeDmaBuffer(%p) FAILED, "
                "status=%08X - the engine was not in the reset state",
                m_EngineHandle, status));
        }
        else
        {
            DOUT(DBG_PRINT, ("FreeBuffer: freed buffer on engine %p",
                m_EngineHandle));
        }
    }

    // The MDL itself belongs to the bus driver (FreeDmaBuffer releases it,
    // per HDAUDIO_BUS_INTERFACE's contract for the AllocateDmaBuffer/
    // FreeDmaBuffer pair) - do not IoFreeMdl() it here.
    m_pBufferMdl        = NULL;
    m_pSystemAddress    = NULL;
    m_AllocatedSize     = 0;
    m_RequestedSize     = 0;
}

STDMETHODIMP_(ULONG) CHdaDmaChannel::TransferCount(void)
{
    return (ULONG)m_AllocatedSize;
}

STDMETHODIMP_(ULONG) CHdaDmaChannel::MaximumBufferSize(void)
{
    // Stage 5an: report the same figure the stream actually allocates, as
    // the samples' implementation does - msvad/basedma.cpp:294 returns the
    // very m_MaxDmaBufferSize that basewave.cpp:597 passes to
    // AllocateBuffer. The 1 MB previously returned here was an invented
    // ceiling unrelated to anything this driver ever allocated.
    return HDA_MAX_DMA_BUFFER_SIZE;
}

STDMETHODIMP_(ULONG) CHdaDmaChannel::AllocatedBufferSize(void)
{
    return (ULONG)m_AllocatedSize;
}

STDMETHODIMP_(ULONG) CHdaDmaChannel::BufferSize(void)
{
    return m_RequestedSize;
}

STDMETHODIMP_(void) CHdaDmaChannel::SetBufferSize(IN ULONG BufferSize)
{
    m_RequestedSize = BufferSize;
}

STDMETHODIMP_(PVOID) CHdaDmaChannel::SystemAddress(void)
{
    return m_pSystemAddress;
}

STDMETHODIMP_(PHYSICAL_ADDRESS) CHdaDmaChannel::PhysicalAddress(void)
{
    PHYSICAL_ADDRESS physAddr;
    physAddr.QuadPart = 0;

    if (m_pSystemAddress)
        physAddr = MmGetPhysicalAddress(m_pSystemAddress);

    return physAddr;
}

STDMETHODIMP_(PADAPTER_OBJECT) CHdaDmaChannel::GetAdapterObject(void)
{
    // No PDMA_ADAPTER of our own (see this class's header comment - the
    // codec function device owns no PCI resources, so there was never an
    // IoGetDmaAdapter to call). Real hardware DMA is driven entirely by the
    // bus driver against the MDL handed back from AllocateDmaBuffer.
    return NULL;
}

STDMETHODIMP_(void) CHdaDmaChannel::CopyTo
(
    IN OUT  PVOID   Destination,
    IN      PVOID   Source,
    IN      ULONG   ByteCount
)
{
    RtlCopyMemory(Destination, Source, ByteCount);
}

STDMETHODIMP_(void) CHdaDmaChannel::CopyFrom
(
    IN OUT  PVOID   Destination,
    IN      PVOID   Source,
    IN      ULONG   ByteCount
)
{
    RtlCopyMemory(Destination, Source, ByteCount);
}
