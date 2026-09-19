/*****************************************************************************
 * common.cpp
 *****************************************************************************
 * See common.h for design notes. This file implements the HDA controller
 * bring-up (MMIO map, reset, CORB/RIRB ring buffers), the synchronous verb
 * transport, and runtime widget-graph discovery.
 *
 * NOTE ON SCOPE: this is the Stage 4 skeleton. The controller-level
 * plumbing below (reset, CORB/RIRB, SendVerb, codec/widget discovery) is
 * written to be functionally complete against the public Intel HDA spec.
 * InitCodec()'s actual per-widget init sequence still needs the exact
 * verb list ported over from sigmatel.c's STAC92HD73XX path (Stage 1
 * reference) - marked with TODO below - since that part is codec-family-
 * specific quirk logic, not generic HDA transport.
 */

static char STR_MODULENAME[] = "stwrtxp Common: ";
#define DEFINE_DEBUG_VARS
#include "common.h"
#include <ntstrsafe.h>

/*****************************************************************************
 * QueryBusInterfaceCompletion
 *****************************************************************************
 * Completion routine for the IRP_MN_QUERY_INTERFACE sent to the lower
 * (bus-driver-owned) device object in AcquireBusInterface(). Must stay in a
 * non-paged segment: IoCallDriver may complete the IRP from a DPC, so this
 * can run at DISPATCH_LEVEL.
 */
static NTSTATUS QueryBusInterfaceCompletion
(
    IN  PDEVICE_OBJECT DeviceObject,
    IN  PIRP           Irp,
    IN  PVOID          Context
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// LogToFileF is deliberately NOT in the PAGE segment - see the IRQL guard
// inside it. Everything after it in this file is paged; the pragma is
// restored immediately below the function.
#pragma code_seg()

/*****************************************************************************
 * LogToFileF
 *****************************************************************************
 * Diagnostic-only (checked builds, DBG=1): appends one formatted line to
 * %SystemDrive%\stwrtxp_log.txt. Exists purely because there's no other
 * trace path available on this real XP x64 machine right now - no physical
 * serial/1394 debug cable, and current Sysinternals DebugView builds dropped
 * pre-Vista kernel-capture support.
 *
 * Zw file I/O is legal at PASSIVE_LEVEL only. Most DOUT call sites in this
 * driver run from PAGED_CODE paths and satisfy that, but Stage 5bg added
 * DOUT calls inside a PortCls event handler, which PortCls invokes at
 * DISPATCH_LEVEL - and that combination bugchecked. Rather than audit every
 * present and future call site, this function now checks the IRQL itself and
 * does nothing when it is raised; the DbgPrint half of DOUT is safe at any
 * IRQL and still runs. The function is also kept out of the PAGE segment so
 * that the call from raised IRQL cannot fault on the way in.
 *
 * Deliberately tolerant of its own failure (e.g. if called before the boot
 * volume is writable) - logging must never be what makes StartDevice fail.
 */
#if (DBG)
void LogToFileF(PCSTR Format, ...)
{
    char buffer[512];

    // Zw file I/O below is PASSIVE_LEVEL-only. See the header comment.
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;

    va_list args;
    va_start(args, Format);
    NTSTATUS formatStatus = RtlStringCbVPrintfA(buffer, sizeof(buffer), Format, args);
    va_end(args);

    if (formatStatus != STATUS_SUCCESS && formatStatus != STATUS_BUFFER_OVERFLOW)
        return;

    size_t len;
    if (!NT_SUCCESS(RtlStringCbLengthA(buffer, sizeof(buffer), &len)))
        return;

    if (len + 2 < sizeof(buffer))
    {
        buffer[len]     = '\r';
        buffer[len + 1] = '\n';
        buffer[len + 2] = '\0';
        len += 2;
    }

    UNICODE_STRING path;
    RtlInitUnicodeString(&path, L"\\??\\C:\\stwrtxp_log.txt");

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    NTSTATUS status = ZwCreateFile(&hFile, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb,
        NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL, 0);

    if (!NT_SUCCESS(status))
        return;

    LARGE_INTEGER offset;
    offset.HighPart = -1;
    offset.LowPart  = FILE_WRITE_TO_END_OF_FILE;

    ZwWriteFile(hFile, NULL, NULL, NULL, &iosb, buffer, (ULONG)len, &offset, NULL);
    ZwClose(hFile);
}
#endif

#pragma code_seg("PAGE")

NTSTATUS NewAdapterCommon
(
    OUT     PUNKNOWN *  Unknown,
    IN      REFCLSID,
    IN      PUNKNOWN    UnknownOuter    OPTIONAL,
    IN      POOL_TYPE   PoolType
)
{
    PAGED_CODE();

    if (!Unknown)
        return STATUS_INVALID_PARAMETER;

    CHdaAdapterCommon *pNewAdapterCommon =
        new(PoolType) CHdaAdapterCommon(UnknownOuter);

    if (!pNewAdapterCommon)
        return STATUS_INSUFFICIENT_RESOURCES;

    *Unknown = (PUNKNOWN)(PADAPTERCOMMON)pNewAdapterCommon;
    (*Unknown)->AddRef();

    return STATUS_SUCCESS;
}

CHdaAdapterCommon::~CHdaAdapterCommon()
{
    PAGED_CODE();

    // Stage 5ap: last chance to give back any DMA engine a stream failed
    // to release. Must run BEFORE InterfaceDereference below, while the
    // bus interface is still ours to call. Anything caught here is a bug
    // in the stream teardown path, so say so loudly - but reclaim it,
    // because the alternative is hardware that stays missing until the
    // machine is rebooted.
    if (m_bBusInterfaceAcquired)
    {
        for (ULONG i = 0; i < HDA_MAX_DMA_ENGINES; i++)
        {
            HANDLE leaked = (HANDLE)InterlockedExchangePointer(
                (PVOID volatile *)&m_EngineHandles[i], NULL);
            if (!leaked)
                continue;

            NTSTATUS status = m_BusInterface.FreeDmaBuffer(
                m_BusInterface.Context, leaked);
            NTSTATUS engineStatus = m_BusInterface.FreeDmaEngine(
                m_BusInterface.Context, leaked);

            DOUT(DBG_ERROR, ("~AdapterCommon: LEAKED DMA engine %p "
                "reclaimed at teardown (FreeDmaBuffer=%08X, "
                "FreeDmaEngine=%08X) - a stream did not release it",
                leaked, status, engineStatus));
        }
    }

    if (m_bBusInterfaceAcquired)
    {
        m_BusInterface.InterfaceDereference(m_BusInterface.Context);
        m_bBusInterfaceAcquired = FALSE;
    }
}

/*****************************************************************************
 * AcquireBusInterface
 *****************************************************************************
 * Real HD Audio/UAA architecture (confirmed on real hardware, see
 * HANDOFF.md): the codec function device we are (matched via
 * HDAUDIO\FUNC_01&VEN_111D&DEV_76C7...) is a logical child PDO of the bus
 * driver (HDAudBus.sys on XP, via the UAA package KB888111) and owns no PCI
 * resources of its own - the bus driver owns the controller's MMIO BAR and
 * IRQ. Verb transport and stream/DMA-engine management instead go through
 * HDAUDIO_BUS_INTERFACE, obtained here from the lower (bus-owned) device
 * object via a synchronous IRP_MN_QUERY_INTERFACE, exactly as any WDM
 * driver queries a private interface from the device below it in the stack.
 *
 * IoGetLowerDeviceObject is an ntifs.h routine, but ntifs.h itself can't be
 * included alongside ntddk.h/wdm.h (portcls.h's own include chain already
 * pulls one of those in, and the two redefine overlapping macros/types) -
 * it's still exported by ntoskrnl and safe to call on this target (present
 * since Windows Server 2003, confirmed against wnet/amd64 build), so just
 * declare its prototype directly instead of dragging in the whole header.
 */
extern "C" NTKERNELAPI PDEVICE_OBJECT IoGetLowerDeviceObject(IN PDEVICE_OBJECT DeviceObject);

NTSTATUS CHdaAdapterCommon::AcquireBusInterface(IN PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    PDEVICE_OBJECT lowerDeviceObject = IoGetLowerDeviceObject(DeviceObject);
    if (!lowerDeviceObject)
    {
        DOUT(DBG_ERROR, ("AcquireBusInterface: IoGetLowerDeviceObject failed!"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    PIRP irp = IoAllocateIrp(lowerDeviceObject->StackSize, FALSE);
    if (!irp)
    {
        ObDereferenceObject(lowerDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // IoAllocateIrp zero-fills the IRP (via IoInitializeIrp), which leaves
    // Tail.Overlay.Thread NULL. That field is not optional for an IRP that
    // will actually be completed: IoCompleteRequest's post-completion
    // bookkeeping (thread priority boost / APC-based completion) dereferences
    // it. Confirmed on real hardware (see HANDOFF.md, second BSOD,
    // MEMORY.DMP) - bugcheck 0xA with Arg1 (memory referenced) == 0x78,
    // exactly NULL plus a small field offset, faulting inside
    // HDAudBus!FxIrp::CompleteRequest while completing this exact IRP.
    irp->Tail.Overlay.Thread = PsGetCurrentThread();

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    // IoGetNextIrpStackLocation is a pure read (Tail.Overlay.CurrentStackLocation - 1,
    // no side effects - confirmed against the WDK 7600 wdm.h definition) and
    // IoSetCompletionRoutine resolves to that exact same slot internally. Zeroing the
    // stack location AFTER registering the completion routine (the previous order here)
    // clobbers the CompletionRoutine/Context/Control fields IoSetCompletionRoutine just
    // wrote, so HDAudBus's completion walk never stops at ours and fully completes the
    // IRP on its own - which is also what the earlier NULL Tail.Overlay.Thread bugcheck
    // was hitting during. This driver then called IoFreeIrp on an already-fully-completed
    // IRP, producing bugcheck 0x44 MULTIPLE_IRP_COMPLETE_REQUESTS on real hardware (see
    // HANDOFF.md). Fill/zero the stack location fully first, then set the completion
    // routine last so its write is the last one into the slot.
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    RtlZeroMemory(stack, sizeof(IO_STACK_LOCATION));
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = &GUID_HDAUDIO_BUS_INTERFACE;
    stack->Parameters.QueryInterface.Size = sizeof(m_BusInterface);
    // Per Microsoft's own HD Audio DDI docs ("Obtaining an HDAUDIO_BUS_INTERFACE
    // DDI Object"), the required Version value for GUID_HDAUDIO_BUS_INTERFACE is
    // the literal 0x0100, not a plain incrementing integer - confirmed as the
    // real-hardware root cause of AcquireBusInterface failing with
    // STATUS_INVALID_BUFFER_SIZE (0xC0000206) once the IRP double-completion bug
    // was fixed and the true failure status became observable (see HANDOFF.md).
    stack->Parameters.QueryInterface.Version = 0x0100;
    stack->Parameters.QueryInterface.Interface = (PINTERFACE)&m_BusInterface;
    stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = 0;

    IoSetCompletionRoutine(irp, QueryBusInterfaceCompletion, &event, TRUE, TRUE, TRUE);

    NTSTATUS status = IoCallDriver(lowerDeviceObject, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = irp->IoStatus.Status;
    }

    IoFreeIrp(irp);
    ObDereferenceObject(lowerDeviceObject);

    if (!NT_SUCCESS(status))
    {
        DOUT(DBG_ERROR, ("AcquireBusInterface: IRP_MN_QUERY_INTERFACE for "
            "GUID_HDAUDIO_BUS_INTERFACE failed, status=%08X", status));
        return status;
    }

    m_bBusInterfaceAcquired = TRUE;

    UCHAR fgStartNode = 0;
    m_BusInterface.GetResourceInformation(m_BusInterface.Context, &m_CodecAddress,
        &fgStartNode);
    m_bCodecFound = TRUE;

    DOUT(DBG_PRINT, ("AcquireBusInterface: got HDAUDIO_BUS_INTERFACE, codec "
        "address %u, function group start node %u", m_CodecAddress, fgStartNode));

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * Init
 *****************************************************************************
 */
STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::Init
(
    IN  PRESOURCELIST ResourceList,
    IN  PDEVICE_OBJECT DeviceObject
)
{
    PAGED_CODE();

    ASSERT(ResourceList);
    ASSERT(DeviceObject);

    UNREFERENCED_PARAMETER(ResourceList);

    m_pDeviceObject = DeviceObject;
    m_PowerState = PowerDeviceD0;

    // No PCI resources to map (see AcquireBusInterface's comment) -
    // ResourceList is whatever PnP assigned this codec function device,
    // which by design is nothing; ValidateResources() in adapter.cpp no
    // longer requires memory/interrupt resources for exactly this reason.
    NTSTATUS status = AcquireBusInterface(DeviceObject);
    if (!NT_SUCCESS(status))
        return status;

    // BringUpCodec() confirms the vendor/device ID - this must happen
    // before we trust anything the codec reports, including its widget
    // graph. No reset handshake here: the bus driver already brought the
    // codec out of reset and enumerated it (that's how it knew to create
    // the FUNC_01 PDO this INF matched in the first place).
    status = BringUpCodec();
    if (!NT_SUCCESS(status))
        return status;

    // Widget discovery must run before InitCodec(): the init verb sequence
    // below applies power-up/pin-enable verbs per-widget using the runtime-
    // discovered table (m_Widgets/m_AfgNid), not hardcoded NIDs.
    status = DiscoverWidgets();
    if (!NT_SUCCESS(status))
        return status;

    status = InitCodec();
    if (!NT_SUCCESS(status))
        return status;

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * HDA_VERB_SYNC_CONTEXT / VerbTransferCompletion
 *****************************************************************************
 * SendVerb() below is called dozens of times per StartDevice (once per verb,
 * across BringUpCodec/DiscoverWidgets/InitCodec) via
 * HDAUDIO_BUS_INTERFACE::TransferCodecVerbs, which is a raw callback-based
 * interface - unlike IoCallDriver/IRP completion, its header
 * (hdaudio.h/PTRANSFER_CODEC_VERBS) documents no explicit guarantee that a
 * non-success return means the completion callback will *never* fire. The
 * first-ever real-hardware run of this code (see HANDOFF.md, BSOD
 * 0x0A/IRQL_NOT_LESS_OR_EQUAL, Mini090826-01.dmp) crashed with a small-offset
 * atomic write at DIRQL - the classic signature of a completion callback
 * firing after its target memory was already reused for something else.
 * The original implementation put both the HDAUDIO_CODEC_TRANSFER and the
 * KEVENT on SendVerb's stack frame and handed their addresses to the bus
 * driver as async context; if that "no late callback" assumption is ever
 * wrong for any status this function treats as synchronous failure, the bus
 * driver would write into a stack frame that already returned. Heap-
 * allocating this context removes that hazard: worst case on an unexpected
 * late callback is now a leaked NonPagedPool block, never corruption.
 */
struct HDA_VERB_SYNC_CONTEXT
{
    KEVENT                  Event;
    HDAUDIO_CODEC_TRANSFER  Transfer;
};

/*****************************************************************************
 * VerbTransferCompletion
 *****************************************************************************
 * PHDAUDIO_TRANSFER_COMPLETE_CALLBACK for TransferCodecVerbs() (see SendVerb
 * below). Non-paged for the same reason as QueryBusInterfaceCompletion - the
 * bus driver may invoke this from a DPC once the verb round-trip completes.
 * Switch out of the "PAGE" segment (active since AcquireBusInterface above)
 * for just this function, then switch back.
 */
#pragma code_seg()
static VOID VerbTransferCompletion
(
    IN  HDAUDIO_CODEC_TRANSFER *Transfer,
    IN  PVOID                   Context
)
{
    UNREFERENCED_PARAMETER(Transfer);
    HDA_VERB_SYNC_CONTEXT *ctx = (HDA_VERB_SYNC_CONTEXT *)Context;
    KeSetEvent(&ctx->Event, IO_NO_INCREMENT, FALSE);
}
#pragma code_seg("PAGE")

/*****************************************************************************
 * SendVerb
 *****************************************************************************
 * Verb transport via HDAUDIO_BUS_INTERFACE::TransferCodecVerbs instead of a
 * directly-driven CORB/RIRB (see AcquireBusInterface's comment for why).
 * TransferCodecVerbs is callback-based - the bus driver may complete it
 * asynchronously (e.g. from a DPC) - so we synchronize with a KEVENT,
 * matching the same "send and block" shape the old CORB/RIRB polling loop
 * had from every caller's point of view. See HDA_VERB_SYNC_CONTEXT's comment
 * above for why this context is heap-allocated rather than on the stack.
 */
STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::SendVerb
(
    IN  UCHAR   CodecAddress,
    IN  UCHAR   Nid,
    IN  USHORT  Verb,
    IN  USHORT  Payload,
    OUT PULONG  Response
)
{
    if (!m_bBusInterfaceAcquired)
        return STATUS_DEVICE_NOT_READY;

    HDA_VERB_SYNC_CONTEXT *ctx = (HDA_VERB_SYNC_CONTEXT *)
        ExAllocatePoolWithTag(NonPagedPool, sizeof(HDA_VERB_SYNC_CONTEXT), 'brVH');
    if (!ctx)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(ctx, sizeof(*ctx));
    KeInitializeEvent(&ctx->Event, NotificationEvent, FALSE);

    // HDAUDIO_CODEC_COMMAND has no CodecAddress/Node member directly - those
    // live inside the Verb8/Verb16 sub-structs (both start with the same
    // Data/VerbId layout width difference, but CodecAddress/Node share the
    // same bit positions in both, so setting them once via either view
    // works for both verb-width cases below).
    ctx->Transfer.Output.Verb8.CodecAddress = CodecAddress;
    ctx->Transfer.Output.Verb8.Node = Nid;

    if (Verb <= 0xF)
    {
        // 4-bit verb + 16-bit payload form (rare; e.g. SET_STREAM_FORMAT
        // uses this encoding per the HDA spec).
        ctx->Transfer.Output.Verb16.VerbId = Verb;
        ctx->Transfer.Output.Verb16.Data = Payload;
    }
    else
    {
        ctx->Transfer.Output.Verb8.VerbId = Verb;
        ctx->Transfer.Output.Verb8.Data = Payload & 0xFF;
    }

    NTSTATUS status = m_BusInterface.TransferCodecVerbs(m_BusInterface.Context,
        1, &ctx->Transfer, VerbTransferCompletion, ctx);

    if (!NT_SUCCESS(status))
    {
        // Treated as "no completion callback will come" (mirrors the
        // IoCallDriver/IRP convention) - safe to free immediately.
        DOUT(DBG_WARNING, ("verb (addr=%u nid=%u verb=%X payload=%X) failed, "
            "status=%08X", CodecAddress, Nid, Verb, Payload, status));
        ExFreePool(ctx);
        return status;
    }

    // KeWaitForSingleObject only returns after VerbTransferCompletion has
    // run (KeSetEvent happens-before the wait's return), so ctx is
    // guaranteed quiescent here - safe to read and free.
    KeWaitForSingleObject(&ctx->Event, Executive, KernelMode, FALSE, NULL);

    BOOLEAN isValid = ctx->Transfer.Input.IsValid;
    ULONG   response = ctx->Transfer.Input.Response;
    ExFreePool(ctx);

    if (!isValid)
    {
        DOUT(DBG_WARNING, ("verb (addr=%u nid=%u verb=%X payload=%X) timed "
            "out/invalid response", CodecAddress, Nid, Verb, Payload));
        return STATUS_IO_TIMEOUT;
    }

    if (Response)
        *Response = response;

    DOUT(DBG_VERB, ("verb (addr=%u nid=%u verb=%X payload=%X) -> response %08X",
        CodecAddress, Nid, Verb, Payload, response));

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * DMA-engine wrappers (see shared.h's IHdaAdapterCommon declaration)
 *****************************************************************************
 * Thin pass-throughs to m_BusInterface, added for the WaveCyclic stream/DMA
 * rewrite (HANDOFF.md's WaveCyclic pivot): the codec function device has no
 * MMIO/IRQ of its own (see AcquireBusInterface's comment above), so all
 * stream DMA-engine and buffer management goes through these bus-driver
 * calls instead. Not PAGED_CODE() - callers include stream SetState(), which
 * PortCls may invoke at DISPATCH_LEVEL for WaveCyclic (mirrors why
 * VerbTransferCompletion above lives outside the "PAGE" segment).
 */
#pragma code_seg()

/*****************************************************************************
 * TrackEngine / UntrackEngine
 *****************************************************************************
 * Stage 5ap: bookkeeping for the outstanding-DMA-engine registry declared in
 * common.h. Deliberately lock-free - a single interlocked pointer swap per
 * slot - because these run on the same paths as the allocate/free wrappers
 * below, which are non-paged precisely so PortCls can reach them from
 * DISPATCH_LEVEL.
 *
 * Failing to find a slot is not fatal to the caller: the engine is still
 * allocated and usable, we have simply lost the safety net for it, so log
 * and carry on rather than failing an otherwise good allocation.
 */
void CHdaAdapterCommon::TrackEngine(IN HANDLE Handle)
{
    if (!Handle)
        return;

    for (ULONG i = 0; i < HDA_MAX_DMA_ENGINES; i++)
    {
        if (InterlockedCompareExchangePointer(
                (PVOID volatile *)&m_EngineHandles[i], Handle, NULL) == NULL)
            return;
    }

    DOUT(DBG_ERROR, ("TrackEngine: registry full, engine %p untracked", Handle));
}

void CHdaAdapterCommon::UntrackEngine(IN HANDLE Handle)
{
    if (!Handle)
        return;

    for (ULONG i = 0; i < HDA_MAX_DMA_ENGINES; i++)
    {
        if (InterlockedCompareExchangePointer(
                (PVOID volatile *)&m_EngineHandles[i], NULL, Handle) == Handle)
            return;
    }
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::AllocateRenderDmaEngine
(
    IN  PHDAUDIO_STREAM_FORMAT     StreamFormat,
    OUT PHANDLE                    Handle,
    OUT PHDAUDIO_CONVERTER_FORMAT  ConverterFormat
)
{
    if (!m_bBusInterfaceAcquired)
        return STATUS_DEVICE_NOT_READY;

    NTSTATUS status = m_BusInterface.AllocateRenderDmaEngine(
        m_BusInterface.Context, StreamFormat, FALSE, Handle, ConverterFormat);
    if (NT_SUCCESS(status))
        TrackEngine(*Handle);
    return status;
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::AllocateCaptureDmaEngine
(
    IN  PHDAUDIO_STREAM_FORMAT     StreamFormat,
    OUT PHANDLE                    Handle,
    OUT PHDAUDIO_CONVERTER_FORMAT  ConverterFormat
)
{
    if (!m_bBusInterfaceAcquired)
        return STATUS_DEVICE_NOT_READY;

    NTSTATUS status = m_BusInterface.AllocateCaptureDmaEngine(
        m_BusInterface.Context, m_CodecAddress, StreamFormat, Handle,
        ConverterFormat);
    if (NT_SUCCESS(status))
        TrackEngine(*Handle);
    return status;
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::AllocateDmaBuffer
(
    IN  HANDLE      Handle,
    IN  SIZE_T      RequestedBufferSize,
    OUT PMDL *      BufferMdl,
    OUT PSIZE_T     AllocatedBufferSize,
    OUT PUCHAR      StreamId,
    OUT PULONG      FifoSize
)
{
    if (!m_bBusInterfaceAcquired)
        return STATUS_DEVICE_NOT_READY;

    return m_BusInterface.AllocateDmaBuffer(m_BusInterface.Context, Handle,
        RequestedBufferSize, BufferMdl, AllocatedBufferSize, StreamId, FifoSize);
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::FreeDmaBuffer(IN HANDLE Handle)
{
    if (!m_bBusInterfaceAcquired)
        return STATUS_DEVICE_NOT_READY;

    return m_BusInterface.FreeDmaBuffer(m_BusInterface.Context, Handle);
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::FreeDmaEngine(IN HANDLE Handle)
{
    if (!m_bBusInterfaceAcquired)
        return STATUS_DEVICE_NOT_READY;

    NTSTATUS status = m_BusInterface.FreeDmaEngine(m_BusInterface.Context,
        Handle);
    if (NT_SUCCESS(status))
        UntrackEngine(Handle);
    return status;
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::SetDmaEngineState
(
    IN  HDAUDIO_STREAM_STATE   StreamState,
    IN  ULONG                  NumberOfHandles,
    IN  HANDLE *               Handles
)
{
    if (!m_bBusInterfaceAcquired)
        return STATUS_DEVICE_NOT_READY;

    return m_BusInterface.SetDmaEngineState(m_BusInterface.Context,
        StreamState, NumberOfHandles, Handles);
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::GetLinkPositionRegister
(
    IN  HANDLE   Handle,
    OUT PULONG * Position
)
{
    if (!m_bBusInterfaceAcquired)
        return STATUS_DEVICE_NOT_READY;

    return m_BusInterface.GetLinkPositionRegister(m_BusInterface.Context,
        Handle, Position);
}

#pragma code_seg("PAGE")

/*****************************************************************************
 * BringUpCodec
 *****************************************************************************
 * Vendor/device ID confirmation. Must run before DiscoverWidgets() - this
 * is our only sanity check that we're actually talking to the expected
 * codec before trusting anything it reports. No reset handshake here (see
 * Init()'s comment): the bus driver already reset and enumerated the codec.
 */
NTSTATUS CHdaAdapterCommon::BringUpCodec(void)
{
    PAGED_CODE();

    ULONG vendorDevice;
    NTSTATUS status = SendVerb(m_CodecAddress, HDA_NID_ROOT, HDA_VERB_GET_PARAMETER,
                       HDA_PARAM_VENDOR_ID, &vendorDevice);
    if (!NT_SUCCESS(status))
        return status;

    USHORT vendorId = (USHORT)(vendorDevice >> 16);
    USHORT deviceId = (USHORT)(vendorDevice & 0xFFFF);

    DOUT(DBG_PRINT, ("Found Codec Vendor ID %04X Device ID %04X", vendorId, deviceId));

    if (vendorId != 0x111D)
    {
        DOUT(DBG_ERROR, ("Unexpected vendor ID %04X (expected IDT 0x111D)", vendorId));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (deviceId != 0x76C7)
    {
        // Not fatal by itself (this driver could in principle run on a
        // sibling 92HD7x/89x device), but flag it since HANDOFF.md's
        // reference research (sigmatel.c) was done specifically against
        // 0x76C7 / 92HD89E2.
        DOUT(DBG_WARNING, ("Device ID %04X != expected 0x76C7 (92HD89E2) - "
            "codec init verb sequence below was derived for 76C7 specifically.",
            deviceId));
    }

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * InitOutputPin
 *****************************************************************************
 * Applies the "power up + route + enable" sequence to one output-capable
 * pin complex (Line-Out/Speaker/HP-Out), and to the DAC it connects to.
 *
 * This is a flattened, single-pass substitute for what sigmatel.c actually
 * does for the STAC92HD73XX model: that codec has no static per-model verb
 * table for pin routing (stac92hd73xx_core_init only sets the master volume
 * knob - see below) - real pin/path setup comes entirely from the generic
 * HDA auto-parser (generic.c's snd_hda_gen_init -> init_multi_out /
 * snd_hda_activate_path / set_pin_eapd), which builds DAC->mixer->pin
 * "paths" from the BIOS-programmed pin configs and powers/unmutes/routes
 * each widget on that path. Building the full generic path-graph is out of
 * scope for this backport (playback-only MVP, not DTS/EQ/mixer parity), so
 * this function reproduces just the observable effect for the simple
 * one-widget-deep case this board's output pins actually need:
 *   - power the pin and its connected DAC up to D0 (path_power_update)
 *   - select the DAC on the pin's input connection (activate_path's
 *     AC_VERB_SET_CONNECT_SEL for a multi[] path stage)
 *   - unmute the pin's own output amp, if it has one
 *   - enable the pin widget as an output (SET_PIN_WIDGET_CTRL), plus the
 *     headphone-amp bit for HP-Out pins specifically
 *   - turn on EAPD (set_pin_eapd) if PIN_CAP reports the pin supports it
 */
NTSTATUS CHdaAdapterCommon::InitOutputPin(IN PHDA_WIDGET PinWidget)
{
    PAGED_CODE();

    ULONG response;

    // Power up the pin widget itself.
    SendVerb(m_CodecAddress, PinWidget->Nid, HDA_VERB_SET_POWER_STATE,
              HDA_PWR_D0, &response);

    // Route the pin from its first connection (the DAC feeding it) and
    // power that DAC up too. Real path-building would pick a specific
    // connection index per-channel-assignment; for a single-DAC render
    // path index 0 is the correct/only choice.
    if (PinWidget->ConnCount > 0)
    {
        SendVerb(m_CodecAddress, PinWidget->Nid, HDA_VERB_SET_CONN_SELECT,
                  0, &response);

        UCHAR dacNid = PinWidget->ConnList[0];
        SendVerb(m_CodecAddress, dacNid, HDA_VERB_SET_POWER_STATE,
                  HDA_PWR_D0, &response);

        // Unmute the DAC's output amp, both channels, max gain. Payload
        // encoding per the HDA spec's 16-bit SET_AMP_GAIN_MUTE form:
        // bit15=output amp, bit13=left, bit12=right, bit7=mute, bits6:0=gain.
        SendVerb(m_CodecAddress, dacNid, HDA_VERB_SET_AMP_GAIN_MUTE,
                  0xB07F, &response);

        // Remember it for BindRenderConverters(): this is the widget that
        // has to be told the stream tag and sample format once a stream
        // actually runs. Powering and unmuting it here is not enough.
        RecordOutputDac(dacNid);
    }

    // Unmute the pin's own output amp, if it has one (harmless no-op verb
    // if this pin has no output amp stage).
    SendVerb(m_CodecAddress, PinWidget->Nid, HDA_VERB_SET_AMP_GAIN_MUTE,
              0xB07F, &response);

    ULONG device = HDA_PINCFG_DEVICE(PinWidget->PinConfig);

    ULONG pinCtl = HDA_PINCTL_OUT_ENABLE;
    if (device == HDA_PINCFG_DEVICE_HP_OUT)
        pinCtl |= HDA_PINCTL_HPHN_ENABLE;

    SendVerb(m_CodecAddress, PinWidget->Nid, HDA_VERB_SET_PIN_WIDGET_CTRL,
              (USHORT)pinCtl, &response);

    // set_pin_eapd() in generic.c: only touch EAPD if PIN_CAP says this pin
    // actually has an EAPD bit (most codecs' line/mic pins don't).
    ULONG pinCaps = 0;
    if (NT_SUCCESS(SendVerb(m_CodecAddress, PinWidget->Nid, HDA_VERB_GET_PARAMETER,
                             HDA_PARAM_PIN_CAP, &pinCaps)) &&
        (pinCaps & HDA_PINCAP_EAPD))
    {
        SendVerb(m_CodecAddress, PinWidget->Nid, HDA_VERB_SET_EAPD_BTL_ENABLE,
                  HDA_EAPD_BTL_ENABLE, &response);
    }

    DOUT(DBG_PRINT, ("Initialized output pin NID %u (device type %u, config %08X)",
        PinWidget->Nid, device, PinWidget->PinConfig));

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * InitCodec
 *****************************************************************************
 * Applies the STAC92HD73XX-family init sequence derived from sigmatel.c,
 * against the widget graph DiscoverWidgets() just populated (m_Widgets/
 * m_AfgNid) - never hardcoded NIDs, per this project's design (no HP quirk
 * exists for subsys 103c2acd in the Linux reference driver, so this board's
 * pin layout is BIOS/EEPROM-programmed and only knowable at runtime).
 *
 * Scope note: this covers playback bring-up only (this backport's stated
 * goal), so only output-capable pins (Line-Out/Speaker/HP-Out) are touched
 * here; input/recording pin setup is a separate future addition.
 */
STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::InitCodec(void)
{
    PAGED_CODE();

    if (m_AfgNid == 0)
    {
        DOUT(DBG_ERROR, ("InitCodec called with no AFG discovered!"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ULONG response;

    // Power up the Audio Function Group itself first (stac_init() ->
    // snd_hda_gen_init() implicitly relies on the AFG already being live;
    // we make it explicit here since our reset handshake alone doesn't
    // guarantee D0).
    SendVerb(m_CodecAddress, m_AfgNid, HDA_VERB_SET_POWER_STATE, HDA_PWR_D0, &response);

    UCHAR volKnobNid = 0;

    // Rebuilt from scratch on every InitCodec (it may run again after a
    // power transition), so stale NIDs can never accumulate.
    m_OutputDacCount = 0;

    for (ULONG i = 0; i < m_WidgetCount; i++)
    {
        PHDA_WIDGET w = &m_Widgets[i];

        if (w->Type == HDA_WIDGET_TYPE_VOLKNOB)
        {
            volKnobNid = w->Nid;
            continue;
        }

        if (w->Type != HDA_WIDGET_TYPE_PIN)
            continue;

        // Skip pins the board doesn't actually wire out (port connectivity
        // == "no physical connection"), matching how the generic auto-
        // parser ignores unconnected pins entirely.
        if (HDA_PINCFG_PORT_CONN(w->PinConfig) == HDA_PINCFG_PORTCONN_NONE)
            continue;

        ULONG device = HDA_PINCFG_DEVICE(w->PinConfig);
        BOOLEAN isOutput = (device == HDA_PINCFG_DEVICE_LINE_OUT ||
                            device == HDA_PINCFG_DEVICE_SPEAKER ||
                            device == HDA_PINCFG_DEVICE_HP_OUT);

        if (!isOutput)
            continue; // recording pins: out of scope here (see comment above)

        InitOutputPin(w);
    }

    // stac92hd73xx_core_init's one and only verb: set the master volume
    // knob to max with direct control (payload 0xFF = direct-control bit
    // set, 7-bit gain field maxed). Applied by widget Type instead of the
    // hardcoded NID 0x1f sigmatel.c uses, since this driver never assumes
    // fixed NIDs.
    if (volKnobNid != 0)
    {
        SendVerb(m_CodecAddress, volKnobNid, HDA_VERB_SET_VOLUME_KNOB, 0xFF, &response);
    }

    if (m_OutputDacCount == 0)
    {
        // Not fatal to init - enumeration and pin creation still work -
        // but nothing will ever be audible, so say so loudly.
        DOUT(DBG_ERROR, ("InitCodec: no output DACs recorded - render will be silent"));
    }
    else
    {
        for (ULONG d = 0; d < m_OutputDacCount; d++)
            DOUT(DBG_PRINT, ("InitCodec: render DAC[%u] = NID %u", d, m_OutputDacNids[d]));

        // Stage 5ay: now that the render DACs are known, read the amplifier
        // geometry the topology filter's volume node reports as its range.
        QueryOutputAmpCaps();

        // Stage 5bb: and which sample rates the converter can actually
        // produce, which decides what the wave filter is allowed to
        // advertise. Must follow the DAC scan for the same reason.
        QueryPcmCaps();
    }

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * RecordOutputDac
 *****************************************************************************
 * Adds one DAC node ID to the set BindRenderConverters() programs, ignoring
 * duplicates (every output pin on this board selects the same DAC) and
 * silently dropping anything past the array bound.
 */
void CHdaAdapterCommon::RecordOutputDac(IN UCHAR Nid)
{
    PAGED_CODE();

    for (ULONG i = 0; i < m_OutputDacCount; i++)
    {
        if (m_OutputDacNids[i] == Nid)
            return;
    }

    if (m_OutputDacCount < (sizeof(m_OutputDacNids) / sizeof(m_OutputDacNids[0])))
        m_OutputDacNids[m_OutputDacCount++] = Nid;
}

/*****************************************************************************
 * BindRenderConverters
 *****************************************************************************
 * The step that was missing entirely until Stage 5ao.
 *
 * AllocateRenderDmaEngine and AllocateDmaBuffer set up the CONTROLLER side:
 * a DMA engine, a buffer, and a stream tag the engine stamps onto every
 * frame it puts on the HD Audio link. Neither of them touches the CODEC.
 * A converter only picks a stream off the link once it has been told, by
 * verb, which tag to listen for and how to interpret the samples. Until
 * then the link carries our audio and every DAC ignores it - the driver
 * ran, the position register advanced, writes completed, and nothing came
 * out of the speakers.
 *
 * ConverterFormat is passed through verbatim from the HDAUDIO_CONVERTER_
 * FORMAT the bus driver filled in at AllocateRenderDmaEngine time; the bus
 * driver owns that encoding (hdaudio.h documents the bitfield) so we do not
 * recompute it.
 *
 * The 0x706 payload is (tag << 4) | first channel, per the HDA spec's
 * Converter Stream/Channel format. Channel is 0: a stereo converter takes
 * channels 0 and 1 starting from the one named here.
 */
/*****************************************************************************
 * QueryOutputAmpCaps
 *****************************************************************************
 * Stage 5ay (HANDOFF item n36). Reads the render DAC's output AMP_CAP and
 * turns it into the KS volume range the topology filter's KSNODETYPE_VOLUME
 * nodes advertise.
 *
 * HDA amplifiers are indexed in steps, not levels: step 0 is the amp's most
 * attenuated setting, step "Offset" is 0 dB, and one step is
 * (StepSize + 1) quarter-decibels. KS volume levels are signed 1/65536 dB,
 * and a quarter of a decibel is exactly 16384 of those, so the conversion
 * both ways is integer arithmetic with no rounding loss on the step size.
 *
 * Per the HDA spec a widget reports its own amp capabilities only if its
 * AUDIO_WIDGET_CAP carries the "amp parameter override" bit, and otherwise
 * inherits the AFG's. Rather than decode that bit and trust it, this asks
 * the widget and falls back to the AFG if the answer is empty - both kinds
 * of codec then work.
 *
 * If nothing usable comes back the nodes still exist and still remember what
 * they are set to; they simply never send a verb (m_bAmpCapsValid). Writing
 * gain steps into a widget that has no gain stage would be worse than a
 * slider that does nothing.
 */
const ULONG g_HdaPcmRateHz[HDA_PCM_RATE_COUNT] =
{
    8000, 11025, 16000, 22050, 32000, 44100,
    48000, 88200, 96000, 176400, 192000, 384000
};

/*****************************************************************************
 * QueryPcmCaps
 *****************************************************************************
 * Stage 5bb. Asks the codec which PCM rates and widths it can actually
 * produce, which this driver had never done - wavecyclicminiport.cpp
 * advertised a flat 8 kHz .. 48 kHz range with a TODO next to it saying
 * exactly that.
 *
 * That mattered more than a TODO suggests. Windows XP's system sounds are
 * 22.05 kHz files, so kmixer negotiated the pin down to 22050 Hz and every
 * one of those streams was silent, while kstest.exe - which asks for
 * 44.1 kHz - was audible on the same boot from the same driver. By Stage 5ba
 * the DMA buffer was proven to hold a full-scale tone during the silent
 * streams and the DMA engine was proven to be reading it, so the data was
 * never the problem; the rate was the only thing that differed.
 *
 * The bus driver cannot catch this for us. AllocateRenderDmaEngine only
 * checks that a rate is expressible in the stream descriptor's format
 * register - 22.05 kHz is simply 44.1 kHz with the divide-by-two field set,
 * so it encodes fine and returns success - and it knows nothing about what
 * the codec on the other end of the link will do with it.
 *
 * A widget answers 0x0A for itself only when AUDIO_WIDGET_CAP bit 4 (Format
 * Override) is set; otherwise the Audio Function Group's answer governs it.
 * NID 21 reports caps 0x000D0C05 on this codec, so bit 4 is clear and the
 * AFG is the authority - but read both and log both, because a wrong guess
 * about which one applies is exactly the kind of thing that costs a stage.
 */
void CHdaAdapterCommon::QueryPcmCaps(void)
{
    PAGED_CODE();

    m_SuppPcmRates      = 0;
    m_SuppStreamFormats = 0;

    ULONG afgRates = 0, afgFormats = 0, dacRates = 0, dacCaps = 0;

    SendVerb(m_CodecAddress, m_AfgNid, HDA_VERB_GET_PARAMETER,
             HDA_PARAM_SUPP_PCM_RATES, &afgRates);
    SendVerb(m_CodecAddress, m_AfgNid, HDA_VERB_GET_PARAMETER,
             HDA_PARAM_SUPP_STREAM_FORMATS, &afgFormats);

    ULONG rates = afgRates;

    if (m_OutputDacCount)
    {
        UCHAR dacNid = m_OutputDacNids[0];

        SendVerb(m_CodecAddress, dacNid, HDA_VERB_GET_PARAMETER,
                 HDA_PARAM_AUDIO_WIDGET_CAP, &dacCaps);
        SendVerb(m_CodecAddress, dacNid, HDA_VERB_GET_PARAMETER,
                 HDA_PARAM_SUPP_PCM_RATES, &dacRates);

        // Bit 4 of AUDIO_WIDGET_CAP is Format Override: set means the
        // widget's own 0x0A is what governs it.
        if ((dacCaps & 0x10) && dacRates)
            rates = dacRates;

        DOUT(DBG_PRINT, ("QueryPcmCaps: DAC NID %u caps=%08X (format override %s), "
            "own 0x0A=%08X", dacNid, dacCaps,
            (dacCaps & 0x10) ? "SET" : "clear", dacRates));
    }

    DOUT(DBG_PRINT, ("QueryPcmCaps: AFG NID %u 0x0A=%08X 0x0B=%08X -> effective "
        "rate mask %08X", m_AfgNid, afgRates, afgFormats, rates));

    if (rates == 0)
    {
        DOUT(DBG_WARNING, ("QueryPcmCaps: codec reported no PCM rates at all - "
            "nothing will be ruled out on its say-so"));
        return;
    }

    for (ULONG b = 0; b < HDA_PCM_RATE_COUNT; b++)
    {
        DOUT(DBG_PRINT, ("QueryPcmCaps:   %6u Hz %s", g_HdaPcmRateHz[b],
            (rates & (1UL << b)) ? "SUPPORTED" : "not supported"));
    }

    m_SuppPcmRates      = rates;
    m_SuppStreamFormats = afgFormats;
}

STDMETHODIMP_(ULONG) CHdaAdapterCommon::GetSupportedPcmRates(void)
{
    return m_SuppPcmRates;
}

void CHdaAdapterCommon::QueryOutputAmpCaps(void)
{
    PAGED_CODE();

    // Fallback geometry, used only for what the nodes report if the codec
    // has no output amp: a plain 0 dB .. -64 dB ramp in 1 dB steps.
    m_AmpOffset     = 0;
    m_AmpNumSteps   = 0;
    m_bAmpCapsValid = FALSE;
    m_VolumeMinimum = -64 * 0x10000;
    m_VolumeMaximum = 0;
    m_VolumeStep    = 0x10000;

    if (m_OutputDacCount == 0)
    {
        DOUT(DBG_WARNING, ("QueryOutputAmpCaps: no render DAC recorded - "
            "volume nodes will cache values only"));
        return;
    }

    UCHAR dacNid = m_OutputDacNids[0];
    ULONG caps   = 0;

    if (!NT_SUCCESS(SendVerb(m_CodecAddress, dacNid, HDA_VERB_GET_PARAMETER,
                             HDA_PARAM_OUTPUT_AMP_CAP, &caps)) || caps == 0)
    {
        caps = 0;
        SendVerb(m_CodecAddress, m_AfgNid, HDA_VERB_GET_PARAMETER,
                 HDA_PARAM_OUTPUT_AMP_CAP, &caps);
    }

    UCHAR numSteps = HDA_AMPCAP_NUMSTEPS(caps);
    UCHAR offset   = HDA_AMPCAP_OFFSET(caps);
    UCHAR stepSize = HDA_AMPCAP_STEPSIZE(caps);

    if (numSteps == 0)
    {
        DOUT(DBG_WARNING, ("QueryOutputAmpCaps: NID %u reports no output amp "
            "(AMP_CAP=%08X) - volume nodes will cache values only",
            dacNid, caps));
        return;
    }

    LONG stepLevel = ((LONG)stepSize + 1) * 16384;   // (n+1) * 0.25 dB

    m_AmpOffset     = offset;
    m_AmpNumSteps   = numSteps;
    m_VolumeStep    = stepLevel;
    m_VolumeMaximum = ((LONG)numSteps - (LONG)offset) * stepLevel;
    m_VolumeMinimum = -((LONG)offset) * stepLevel;
    m_bAmpCapsValid = TRUE;

    // Both stages start at the top of the range, which is where InitOutputPin
    // has already left the hardware: it asks for gain 0x7F, and a codec
    // clamps that to its own highest step.
    for (ULONG s = 0; s < HDA_GAIN_COUNT; s++)
    {
        m_Volume[s][0] = m_VolumeMaximum;
        m_Volume[s][1] = m_VolumeMaximum;
        m_Mute[s]      = FALSE;
    }

    DOUT(DBG_PRINT, ("QueryOutputAmpCaps: NID %u AMP_CAP=%08X offset=%u nsteps=%u "
        "stepsize=%u -> range %d..%d step %d (1/65536 dB)",
        dacNid, caps, offset, numSteps, stepSize,
        m_VolumeMinimum, m_VolumeMaximum, m_VolumeStep));
}

/*****************************************************************************
 * LevelToAmpStep
 *****************************************************************************
 * KS volume level (1/65536 dB) -> AMP_CAP step index. Rounds to nearest
 * rather than truncating: truncation would put the very top of the slider
 * one step below the maximum the node just told the caller about.
 */
UCHAR CHdaAdapterCommon::LevelToAmpStep(IN LONG Level)
{
    if (Level >= m_VolumeMaximum)
        return m_AmpNumSteps;
    if (Level <= m_VolumeMinimum || m_VolumeStep <= 0)
        return 0;

    LONG step = (Level - m_VolumeMinimum + (m_VolumeStep / 2)) / m_VolumeStep;

    if (step < 0)
        step = 0;
    if (step > (LONG)m_AmpNumSteps)
        step = (LONG)m_AmpNumSteps;

    return (UCHAR)step;
}

/*****************************************************************************
 * ProgramOutputAmp
 *****************************************************************************
 * Pushes the current wave + master state onto the render DACs' output amp.
 *
 * There is one hardware attenuator and two KS gain stages in front of it, so
 * the two levels are ADDED. That is not a fudge: cascaded gain stages
 * multiply, decibels are logarithmic, and both levels are already in dB - so
 * the sum is exactly what two real attenuators in series would do. Muting is
 * the same idea in the degenerate case: either stage muted mutes the path.
 *
 * PASSIVE_LEVEL - SendVerb waits on the RIRB.
 */
NTSTATUS CHdaAdapterCommon::ProgramOutputAmp(void)
{
    PAGED_CODE();

    if (!m_bAmpCapsValid || m_OutputDacCount == 0)
        return STATUS_SUCCESS;

    BOOLEAN mute = (BOOLEAN)(m_Mute[HDA_GAIN_WAVE] || m_Mute[HDA_GAIN_MASTER]);

    UCHAR steps[2];
    for (ULONG ch = 0; ch < 2; ch++)
    {
        LONG total = m_Volume[HDA_GAIN_WAVE][ch] + m_Volume[HDA_GAIN_MASTER][ch];

        if (total > m_VolumeMaximum)
            total = m_VolumeMaximum;
        if (total < m_VolumeMinimum)
            total = m_VolumeMinimum;

        steps[ch] = LevelToAmpStep(total);
    }

    NTSTATUS result = STATUS_SUCCESS;
    ULONG    response;

    for (ULONG i = 0; i < m_OutputDacCount; i++)
    {
        for (ULONG ch = 0; ch < 2; ch++)
        {
            USHORT payload = (USHORT)(HDA_AMP_SET_OUTPUT |
                (ch == 0 ? HDA_AMP_SET_LEFT : HDA_AMP_SET_RIGHT) |
                (steps[ch] & HDA_AMP_GAIN_MASK));

            if (mute)
                payload |= HDA_AMP_SET_MUTE;

            NTSTATUS status = SendVerb(m_CodecAddress, m_OutputDacNids[i],
                HDA_VERB_SET_AMP_GAIN_MUTE, payload, &response);

            if (!NT_SUCCESS(status))
            {
                DOUT(DBG_ERROR, ("ProgramOutputAmp: NID %u payload %04X failed, "
                    "status=%08X", m_OutputDacNids[i], payload, status));
                result = status;
            }
        }
    }

    DOUT(DBG_PRINT, ("ProgramOutputAmp: wave L=%d R=%d mute=%u, master L=%d R=%d "
        "mute=%u -> amp steps L=%u R=%u mute=%u",
        m_Volume[HDA_GAIN_WAVE][0],   m_Volume[HDA_GAIN_WAVE][1],   m_Mute[HDA_GAIN_WAVE],
        m_Volume[HDA_GAIN_MASTER][0], m_Volume[HDA_GAIN_MASTER][1], m_Mute[HDA_GAIN_MASTER],
        steps[0], steps[1], mute));

    return result;
}

/*****************************************************************************
 * GetVolumeRange / GetVolumeLevel / SetVolumeLevel / GetMute / SetMute
 *****************************************************************************
 * The IHdaAdapterCommon surface the topology filter's node property handlers
 * call (mintopo.cpp). Channel -1 is "all channels".
 */
STDMETHODIMP_(void) CHdaAdapterCommon::GetVolumeRange
(
    OUT PLONG   Minimum,
    OUT PLONG   Maximum,
    OUT PLONG   Step
)
{
    if (Minimum)
        *Minimum = m_VolumeMinimum;
    if (Maximum)
        *Maximum = m_VolumeMaximum;
    if (Step)
        *Step = m_VolumeStep;
}

STDMETHODIMP_(LONG) CHdaAdapterCommon::GetVolumeLevel
(
    IN  ULONG   Stage,
    IN  LONG    Channel
)
{
    if (Stage >= HDA_GAIN_COUNT)
        return 0;

    // Channel -1 asks for the value common to every channel; there is no
    // single right answer when they differ, and every caller that sends -1
    // is about to set them together anyway, so report the left one.
    if (Channel < 0 || Channel > 1)
        return m_Volume[Stage][0];

    return m_Volume[Stage][Channel];
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::SetVolumeLevel
(
    IN  ULONG   Stage,
    IN  LONG    Channel,
    IN  LONG    Level
)
{
    PAGED_CODE();

    if (Stage >= HDA_GAIN_COUNT)
        return STATUS_INVALID_PARAMETER;

    // kmixer sends 0x80000000 for "-infinity"; clamping is the whole
    // response to it.
    if (Level > m_VolumeMaximum)
        Level = m_VolumeMaximum;
    if (Level < m_VolumeMinimum)
        Level = m_VolumeMinimum;

    if (Channel < 0)
    {
        m_Volume[Stage][0] = Level;
        m_Volume[Stage][1] = Level;
    }
    else if (Channel <= 1)
    {
        m_Volume[Stage][Channel] = Level;
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Stage == HDA_GAIN_CAPTURE)
        return STATUS_SUCCESS;      // state only - no ADC path in this backport

    return ProgramOutputAmp();
}

STDMETHODIMP_(BOOLEAN) CHdaAdapterCommon::GetMute(IN ULONG Stage)
{
    if (Stage >= HDA_GAIN_COUNT)
        return FALSE;

    return m_Mute[Stage];
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::SetMute
(
    IN  ULONG   Stage,
    IN  BOOLEAN Mute
)
{
    PAGED_CODE();

    if (Stage >= HDA_GAIN_COUNT)
        return STATUS_INVALID_PARAMETER;

    m_Mute[Stage] = Mute;

    if (Stage == HDA_GAIN_CAPTURE)
        return STATUS_SUCCESS;

    return ProgramOutputAmp();
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::BindRenderConverters
(
    IN  USHORT  ConverterFormat,
    IN  UCHAR   StreamTag
)
{
    PAGED_CODE();

    if (m_OutputDacCount == 0)
    {
        DOUT(DBG_ERROR, ("BindRenderConverters: no output DAC known"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    NTSTATUS  result = STATUS_SUCCESS;
    ULONG     response;

    for (ULONG i = 0; i < m_OutputDacCount; i++)
    {
        UCHAR dacNid = m_OutputDacNids[i];

        if (StreamTag != 0)
        {
            // Format first: the spec requires the converter to know the
            // format before it is attached to a running stream.
            NTSTATUS status = SendVerb(m_CodecAddress, dacNid,
                HDA_VERB_SET_STREAM_FORMAT, ConverterFormat, &response);
            if (!NT_SUCCESS(status))
            {
                DOUT(DBG_ERROR, ("BindRenderConverters: NID %u SET_STREAM_FORMAT "
                    "%04X failed, status=%08X", dacNid, ConverterFormat, status));
                result = status;
                continue;
            }
        }

        USHORT payload = (USHORT)(((USHORT)StreamTag << 4) | 0);

        NTSTATUS status = SendVerb(m_CodecAddress, dacNid,
            HDA_VERB_SET_CHAN_STREAMID, payload, &response);
        if (!NT_SUCCESS(status))
        {
            DOUT(DBG_ERROR, ("BindRenderConverters: NID %u SET_CHAN_STREAMID "
                "%04X failed, status=%08X", dacNid, payload, status));
            result = status;
            continue;
        }

        DOUT(DBG_PRINT, ("BindRenderConverters: NID %u %s tag %u, format %04X",
            dacNid, StreamTag ? "bound to" : "unbound from",
            StreamTag, ConverterFormat));
    }

    return result;
}

/*****************************************************************************
 * AllocateCommonBuffer / FreeCommonBuffer
 *****************************************************************************
 * STUBBED pending the stream/DMA rewrite: this used to hand out a common
 * buffer from a PDMA_ADAPTER obtained via IoGetDmaAdapter, but that adapter
 * was only ever obtainable because we thought we owned the controller's PCI
 * resources directly (see AcquireBusInterface's comment - we don't). Real
 * HD Audio/UAA stream buffers must instead come from
 * HDAUDIO_BUS_INTERFACE::AllocateDmaBuffer (paired with AllocateRenderDma
 * Engine/AllocateCaptureDmaEngine and SetDmaEngineState), which is a bigger,
 * not-yet-done rewrite of wavepcistream.cpp/wavepciminiport.cpp - tracked in
 * HANDOFF.md. Until that lands, callers (wavepcistream.cpp) will fail to
 * allocate a stream buffer, which only matters once something actually
 * tries to open a wave stream - it does not block StartDevice/enumeration.
 */
STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::AllocateCommonBuffer
(
    IN  ULONG               Size,
    OUT PVOID *             VirtualAddress,
    OUT PPHYSICAL_ADDRESS   PhysicalAddress
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Size);

    if (!VirtualAddress || !PhysicalAddress)
        return STATUS_INVALID_PARAMETER;

    DOUT(DBG_ERROR, ("AllocateCommonBuffer: stubbed out pending the "
        "HDAUDIO_BUS_INTERFACE-based stream/DMA rewrite - see HANDOFF.md"));

    *VirtualAddress = NULL;
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(void) CHdaAdapterCommon::FreeCommonBuffer
(
    IN  ULONG               Size,
    IN  PVOID               VirtualAddress,
    IN  PHYSICAL_ADDRESS    PhysicalAddress
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(VirtualAddress);
    UNREFERENCED_PARAMETER(PhysicalAddress);
    // No-op - see AllocateCommonBuffer's comment; nothing was ever allocated.
}

/*****************************************************************************
 * DiscoverWidgets
 *****************************************************************************
 * Walks NID 0's subordinate node list to find the Audio Function Group,
 * then walks the AFG's subordinate nodes to build the flat widget table.
 * Deliberately does NOT hardcode NIDs anywhere - this is the "read pin
 * config from the codec at runtime instead of a static per-board table"
 * approach HANDOFF.md's sigmatel.c research concluded was necessary
 * (no HP quirk exists for subsys 103c2acd in the Linux reference driver).
 */
NTSTATUS CHdaAdapterCommon::DiscoverWidgets(void)
{
    PAGED_CODE();

    ULONG response;
    NTSTATUS status = SendVerb(m_CodecAddress, HDA_NID_ROOT, HDA_VERB_GET_PARAMETER,
                                HDA_PARAM_SUBORDINATE_NODE_COUNT, &response);
    if (!NT_SUCCESS(status))
        return status;

    UCHAR rootStartNid = (UCHAR)((response >> 16) & 0xFF);
    UCHAR rootNodeCount = (UCHAR)(response & 0xFF);

    m_WidgetCount = 0;
    m_AfgNid = 0;

    for (UCHAR fgNid = rootStartNid; fgNid < rootStartNid + rootNodeCount; fgNid++)
    {
        status = SendVerb(m_CodecAddress, fgNid, HDA_VERB_GET_PARAMETER,
                           HDA_PARAM_FUNCTION_GROUP_TYPE, &response);
        if (!NT_SUCCESS(status))
            continue;

        UCHAR fgType = (UCHAR)(response & 0xFF);
        if (fgType != 0x01) // 1 = Audio Function Group
            continue;

        m_AfgNid = fgNid;

        status = SendVerb(m_CodecAddress, fgNid, HDA_VERB_GET_PARAMETER,
                           HDA_PARAM_SUBORDINATE_NODE_COUNT, &response);
        if (!NT_SUCCESS(status))
            continue;

        UCHAR startNid = (UCHAR)((response >> 16) & 0xFF);
        UCHAR nodeCount = (UCHAR)(response & 0xFF);

        DiscoverWidgetsInGroup(startNid, nodeCount);
        break; // this codec only exposes one AFG
    }

    if (m_AfgNid == 0)
    {
        DOUT(DBG_ERROR, ("No Audio Function Group found on this codec!"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    DOUT(DBG_PRINT, ("Discovered %u widgets under AFG NID %u", m_WidgetCount, m_AfgNid));

    return STATUS_SUCCESS;
}

NTSTATUS CHdaAdapterCommon::DiscoverWidgetsInGroup(IN UCHAR StartNid, IN UCHAR NodeCount)
{
    PAGED_CODE();

    ULONG response;

    for (UCHAR nid = StartNid; nid < StartNid + NodeCount && m_WidgetCount < HDA_MAX_WIDGETS; nid++)
    {
        PHDA_WIDGET w = &m_Widgets[m_WidgetCount];
        RtlZeroMemory(w, sizeof(HDA_WIDGET));
        w->Nid = nid;

        if (!NT_SUCCESS(SendVerb(m_CodecAddress, nid, HDA_VERB_GET_PARAMETER,
                                  HDA_PARAM_AUDIO_WIDGET_CAP, &response)))
            continue;

        w->Caps = response;
        w->Type = (UCHAR)((response >> 20) & 0xF);

        if (w->Type == HDA_WIDGET_TYPE_PIN)
        {
            if (NT_SUCCESS(SendVerb(m_CodecAddress, nid, HDA_VERB_GET_CONFIG_DEFAULT,
                                     0, &response)))
            {
                w->PinConfig = response;
            }
        }

        // Connection list, if this widget type has one (mixers/selectors/
        // pins can source from other widgets; DACs/ADCs/power/beep do not).
        if (w->Type == HDA_WIDGET_TYPE_MIXER || w->Type == HDA_WIDGET_TYPE_SELECTOR ||
            w->Type == HDA_WIDGET_TYPE_PIN)
        {
            if (NT_SUCCESS(SendVerb(m_CodecAddress, nid, HDA_VERB_GET_PARAMETER,
                                     HDA_PARAM_CONN_LIST_LENGTH, &response)))
            {
                UCHAR connLen = (UCHAR)(response & 0x7F);
                BOOLEAN longForm = (response & 0x80) != 0;

                for (UCHAR i = 0; i < connLen && w->ConnCount < 32; )
                {
                    ULONG connResp;
                    if (!NT_SUCCESS(SendVerb(m_CodecAddress, nid,
                            HDA_VERB_GET_CONN_LIST_ENTRY, i, &connResp)))
                        break;

                    if (longForm)
                    {
                        // 2 entries of 16 bits each per response
                        w->ConnList[w->ConnCount++] = (UCHAR)(connResp & 0xFFFF);
                        if (w->ConnCount < 32 && (i + 1) < connLen)
                            w->ConnList[w->ConnCount++] = (UCHAR)((connResp >> 16) & 0xFFFF);
                        i += 2;
                    }
                    else
                    {
                        // 4 entries of 8 bits each per response
                        for (int b = 0; b < 4 && i < connLen && w->ConnCount < 32; b++, i++)
                            w->ConnList[w->ConnCount++] = (UCHAR)((connResp >> (b * 8)) & 0xFF);
                    }
                }
            }
        }

        m_WidgetCount++;
    }

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * IAdapterPowerManagement (stub for now)
 *****************************************************************************
 * TODO: proper D0/D3 transitions - re-run InitCodec()'s power-up sequence
 * on D0 entry, and (per stwrt64.sys's DoubleResetHandshake reasoning)
 * consider a full re-init on resume from D3Cold specifically, not just D3.
 */
STDMETHODIMP_(void) CHdaAdapterCommon::PowerChangeState(IN POWER_STATE NewState)
{
    PAGED_CODE();
    m_PowerState = NewState.DeviceState;
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::QueryPowerChangeState(IN POWER_STATE NewStateQuery)
{
    UNREFERENCED_PARAMETER(NewStateQuery);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::QueryDeviceCapabilities(IN PDEVICE_CAPABILITIES PowerDeviceCaps)
{
    UNREFERENCED_PARAMETER(PowerDeviceCaps);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CHdaAdapterCommon::NonDelegatingQueryInterface
(
    REFIID  Interface,
    PVOID * Object
)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID((PUNKNOWN)(PADAPTERCOMMON)this);
    }
    else if (IsEqualGUIDAligned(Interface, IID_IHdaAdapterCommon))
    {
        *Object = PVOID((PADAPTERCOMMON)this);
    }
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement))
    {
        *Object = PVOID(PADAPTERPOWERMANAGEMENT(this));
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }

    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}


/*****************************************************************************
 * PropertyHandler_ComponentId()
 *****************************************************************************
 * Handles KSPROPERTY_GENERAL_COMPONENTID on both the wave and the topology
 * filter - see the long note in shared.h for why this exists. sysaudio asks
 * this first, before it asks anything else, on every filter it enumerates.
 *
 * The three-way GET size check (big enough / zero / too small) is sb16's,
 * not msvad's: sb16 answers a zero-length request with STATUS_BUFFER_OVERFLOW
 * and the required size, which is what a caller doing a size probe expects.
 * Stage 5av's trace shows sysaudio skipping the probe and arriving with 72
 * bytes ready, but other callers do probe.
 */
NTSTATUS
PropertyHandler_ComponentId
(
    IN      PPCPROPERTY_REQUEST     PropertyRequest
)
{
    PAGED_CODE();

    if (!PropertyRequest)
    {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        if (PropertyRequest->ValueSize >= sizeof(KSCOMPONENTID))
        {
            PKSCOMPONENTID pComponentId = (PKSCOMPONENTID)PropertyRequest->Value;

            INIT_MMREG_MID(&pComponentId->Manufacturer, MM_MICROSOFT);
            pComponentId->Product   = PID_STWRTXP;
            pComponentId->Name      = NAME_STWRTXP;

            // Component is only used for extended capability reporting, which
            // this driver does not do. Zeroed rather than assigned GUID_NULL
            // to avoid depending on that symbol's storage being linked in.
            RtlZeroMemory(&pComponentId->Component, sizeof(GUID));

            pComponentId->Version   = STWRTXP_VERSION;
            pComponentId->Revision  = STWRTXP_REVISION;

            PropertyRequest->ValueSize = sizeof(KSCOMPONENTID);
            ntStatus = STATUS_SUCCESS;
        }
        else if (PropertyRequest->ValueSize == 0)
        {
            PropertyRequest->ValueSize = sizeof(KSCOMPONENTID);
            ntStatus = STATUS_BUFFER_OVERFLOW;
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

    DOUT(DBG_PRINT, ("PropertyHandler_ComponentId: verb=%08X size=%d -> %08X",
        PropertyRequest->Verb, PropertyRequest->ValueSize, ntStatus));

    return ntStatus;
}

/*****************************************************************************
 * PropertyHandler_PreferredStatus()
 *****************************************************************************
 * Handles KSPROPERTY_AUDIO_PREFERRED_STATUS - the XP-only notification that
 * this filter has been selected as (or dropped as) the system's preferred
 * audio device. See shared.h: this was the very last request in Stage 5av's
 * trace and the driver refused it with STATUS_PROPSET_NOT_FOUND.
 *
 * There is nothing to program on the codec for this; the correct behaviour is
 * to accept it. The payload is logged because it says which way the flag was
 * being set, which the Stage 5av hook could not see.
 */
NTSTATUS
PropertyHandler_PreferredStatus
(
    IN      PPCPROPERTY_REQUEST     PropertyRequest
)
{
    PAGED_CODE();

    if (!PropertyRequest)
    {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
    {
        if (PropertyRequest->ValueSize >= sizeof(KSAUDIO_PREFERRED_STATUS))
        {
            PKSAUDIO_PREFERRED_STATUS pPreferred =
                (PKSAUDIO_PREFERRED_STATUS)PropertyRequest->Value;

            DOUT(DBG_PRINT, ("PropertyHandler_PreferredStatus: Enable=%d DeviceType=%d Flags=%08X",
                pPreferred->Enable, pPreferred->DeviceType, pPreferred->Flags));

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
                KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_SET;

            PropertyRequest->ValueSize = sizeof(ULONG);
            ntStatus = STATUS_SUCCESS;
        }
        else
        {
            PropertyRequest->ValueSize = 0;
            ntStatus = STATUS_BUFFER_TOO_SMALL;
        }
    }

    DOUT(DBG_PRINT, ("PropertyHandler_PreferredStatus: verb=%08X size=%d -> %08X",
        PropertyRequest->Verb, PropertyRequest->ValueSize, ntStatus));

    return ntStatus;
}
