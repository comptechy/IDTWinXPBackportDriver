/*****************************************************************************
 * pindump.c
 *****************************************************************************
 * Standalone usermode diagnostic - NOT part of the shipped driver.
 *
 * Stage 5bo. Dumps, byte for byte, what our wave filter's pins ADVERTISE:
 * KSPROPERTY_PIN_CTYPES, and per pin DATAFLOW / COMMUNICATION / CINSTANCES /
 * DATARANGES, with every data range decoded (FormatSize, the three format
 * GUIDs by name, and the KSDATARANGE_AUDIO fields when present).
 *
 * Two questions it exists to answer:
 *
 *  1. The driver log shows sysaudio reading 360 bytes of DATARANGES from wave
 *     pin 0. 360 = sizeof(KSMULTIPLE_ITEM) + 4 * 88, i.e. FOUR data ranges -
 *     but BuildPcmDataRanges() builds exactly TWO (48000 and 44100). Something
 *     between our descriptor and the wire is adding ranges. Until we know what
 *     they are, we do not know what our own pin advertises.
 *
 *  2. Our DataRangeIntersection() returns STATUS_NOT_IMPLEMENTED and lets
 *     PortCls's default handler answer. That default only understands
 *     KSDATAFORMAT_SPECIFIER_WAVEFORMATEX. Every WDK audio sample that wants
 *     DirectSound (sb16\minwave.cpp:804, ac97\wavepciminiport.cpp:1025)
 *     implements the handler itself specifically to also answer
 *     KSDATAFORMAT_SPECIFIER_DSOUND. So this tool also fires a real
 *     KSPROPERTY_PIN_DATAINTERSECTION at pin 0 twice - once with a
 *     WAVEFORMATEX range, once with a DSOUND range - and reports both.
 *
 * Build it BOTH ways and run BOTH: the whole point is the 32/64 split.
 * A result without a stated bitness is not a result.
 */

#define INITGUID

#include <windows.h>
#include <mmsystem.h>
#include <setupapi.h>
/* winioctl.h must precede ks.h - see kstest.c for why. */
#include <winioctl.h>
#include <ks.h>
#include <ksmedia.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#if defined(_M_X64) || defined(_M_AMD64)
#define BITNESS L"x64 (64-bit)"
#define LOGNAME L"pindump64_log.txt"
#else
#define BITNESS L"x86 (32-bit)"
#define LOGNAME L"pindump32_log.txt"
#endif

static FILE *g_log = NULL;

static void LogF(const wchar_t *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfwprintf(stdout, fmt, ap);
    va_end(ap);

    if (g_log)
    {
        va_start(ap, fmt);
        vfwprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

/*
 * KS device handles are opened FILE_FLAG_OVERLAPPED, so every ioctl has to go
 * through OVERLAPPED and wait for its own completion. Unlike kstest.c's
 * helper this one hands back the byte count, which is the entire point here.
 */
static DWORD
KsSyncIoctl(
    HANDLE hObject,
    DWORD  IoControlCode,
    PVOID  InBuf,
    DWORD  InLen,
    PVOID  OutBuf,
    DWORD  OutLen,
    DWORD *BytesReturned
)
{
    OVERLAPPED ov;
    DWORD      bytes = 0;
    DWORD      err   = ERROR_SUCCESS;

    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == NULL)
        return GetLastError();

    if (!DeviceIoControl(hObject, IoControlCode, InBuf, InLen,
                         OutBuf, OutLen, &bytes, &ov))
    {
        err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            if (GetOverlappedResult(hObject, &ov, &bytes, TRUE))
                err = ERROR_SUCCESS;
            else
                err = GetLastError();
        }
    }

    CloseHandle(ov.hEvent);

    if (BytesReturned)
        *BytesReturned = bytes;

    return err;
}

static DWORD
GetPinProperty(
    HANDLE  hFilter,
    ULONG   PinId,
    ULONG   PropertyId,
    PVOID   OutBuf,
    DWORD   OutLen,
    DWORD  *BytesReturned
)
{
    KSP_PIN pin;

    ZeroMemory(&pin, sizeof(pin));
    pin.Property.Set   = KSPROPSETID_Pin;
    pin.Property.Id    = PropertyId;
    pin.Property.Flags = KSPROPERTY_TYPE_GET;
    pin.PinId          = PinId;
    pin.Reserved       = 0;

    return KsSyncIoctl(hFilter, IOCTL_KS_PROPERTY, &pin, sizeof(pin),
                       OutBuf, OutLen, BytesReturned);
}

/*
 * Name the format GUIDs we care about; anything else prints raw so an
 * unexpected one cannot hide behind "unknown".
 */
static const wchar_t *
GuidName(const GUID *g)
{
    static wchar_t raw[80];

    if (IsEqualGUID(g, &KSDATAFORMAT_TYPE_AUDIO))            return L"TYPE_AUDIO";
    if (IsEqualGUID(g, &KSDATAFORMAT_TYPE_WILDCARD))         return L"TYPE_WILDCARD";
    if (IsEqualGUID(g, &KSDATAFORMAT_SUBTYPE_PCM))           return L"SUBTYPE_PCM";
    if (IsEqualGUID(g, &KSDATAFORMAT_SUBTYPE_ANALOG))        return L"SUBTYPE_ANALOG";
    if (IsEqualGUID(g, &KSDATAFORMAT_SUBTYPE_WILDCARD))      return L"SUBTYPE_WILDCARD";
    if (IsEqualGUID(g, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))return L"SPECIFIER_WAVEFORMATEX";
    if (IsEqualGUID(g, &KSDATAFORMAT_SPECIFIER_DSOUND))      return L"SPECIFIER_DSOUND";
    if (IsEqualGUID(g, &KSDATAFORMAT_SPECIFIER_NONE))        return L"SPECIFIER_NONE";
    if (IsEqualGUID(g, &KSDATAFORMAT_SPECIFIER_WILDCARD))    return L"SPECIFIER_WILDCARD";

    _snwprintf(raw, 79,
        L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        (ULONG)g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    raw[79] = 0;
    return raw;
}

static const wchar_t *
DataFlowName(KSPIN_DATAFLOW f)
{
    switch (f)
    {
        case KSPIN_DATAFLOW_IN:  return L"IN (host -> device, render sink)";
        case KSPIN_DATAFLOW_OUT: return L"OUT (device -> host, capture/bridge)";
        default:                 return L"?";
    }
}

static const wchar_t *
CommName(KSPIN_COMMUNICATION c)
{
    switch (c)
    {
        case KSPIN_COMMUNICATION_NONE:   return L"NONE (bridge pin)";
        case KSPIN_COMMUNICATION_SINK:   return L"SINK";
        case KSPIN_COMMUNICATION_SOURCE: return L"SOURCE";
        case KSPIN_COMMUNICATION_BOTH:   return L"BOTH";
        case KSPIN_COMMUNICATION_BRIDGE: return L"BRIDGE";
        default:                         return L"?";
    }
}

/*
 * Walk a KSMULTIPLE_ITEM of KSDATARANGEs. Each entry's length is its own
 * FormatSize rounded up to FILE_QUAD_ALIGNMENT - that rounding is exactly the
 * arithmetic that says 360 bytes is four 88-byte ranges, so it is done here
 * explicitly rather than assumed.
 */
static void
DumpDataRanges(PKSMULTIPLE_ITEM mi, DWORD cbTotal)
{
    PUCHAR p   = (PUCHAR)(mi + 1);
    PUCHAR end = (PUCHAR)mi + cbTotal;
    ULONG  i;

    LogF(L"        KSMULTIPLE_ITEM: Size=%lu Count=%lu  (buffer held %lu bytes)\n",
         (ULONG)mi->Size, (ULONG)mi->Count, (ULONG)cbTotal);

    if (mi->Size != cbTotal)
        LogF(L"        NOTE: KSMULTIPLE_ITEM.Size disagrees with bytes returned.\n");

    for (i = 0; i < mi->Count; i++)
    {
        PKSDATARANGE dr = (PKSDATARANGE)p;
        ULONG        adv;

        if (p + sizeof(KSDATARANGE) > end)
        {
            LogF(L"        range %lu: TRUNCATED - only %ld bytes left\n",
                 i, (long)(end - p));
            break;
        }

        LogF(L"        range %lu of %lu  @+%ld  FormatSize=%lu Flags=%08lX "
             L"SampleSize=%lu Reserved=%lu\n",
             i + 1, (ULONG)mi->Count, (long)(p - (PUCHAR)mi),
             (ULONG)dr->FormatSize, (ULONG)dr->Flags,
             (ULONG)dr->SampleSize, (ULONG)dr->Reserved);
        LogF(L"            Major     = %s\n", GuidName(&dr->MajorFormat));
        LogF(L"            SubFormat = %s\n", GuidName(&dr->SubFormat));
        LogF(L"            Specifier = %s\n", GuidName(&dr->Specifier));

        if (dr->FormatSize >= sizeof(KSDATARANGE_AUDIO))
        {
            PKSDATARANGE_AUDIO a = (PKSDATARANGE_AUDIO)dr;
            LogF(L"            audio: MaxChannels=%lu Bits=%lu..%lu Freq=%lu..%lu\n",
                 (ULONG)a->MaximumChannels,
                 (ULONG)a->MinimumBitsPerSample,
                 (ULONG)a->MaximumBitsPerSample,
                 (ULONG)a->MinimumSampleFrequency,
                 (ULONG)a->MaximumSampleFrequency);
        }
        else
        {
            LogF(L"            (plain KSDATARANGE - no audio fields)\n");
        }

        adv = (dr->FormatSize + 7) & ~7UL;
        if (adv == 0)
        {
            LogF(L"            FormatSize is zero - cannot advance, stopping.\n");
            break;
        }
        p += adv;
    }
}

/*
 * Fire a real KSPROPERTY_PIN_DATAINTERSECTION at the pin with a single
 * caller-supplied range, and report what came back. This is the request
 * 64-bit DirectSound issues and 32-bit DirectSound never gets to.
 */
static void
TryIntersection(
    HANDLE          hFilter,
    ULONG           PinId,
    const wchar_t  *Label,
    const GUID     *Specifier,
    ULONG           Channels,
    ULONG           Bits,
    ULONG           Freq
)
{
    struct {
        KSP_PIN            Pin;
        KSMULTIPLE_ITEM    Item;
        KSDATARANGE_AUDIO  Range;
    } req;

    UCHAR out[512];
    DWORD bytes = 0;
    DWORD err;

    ZeroMemory(&req, sizeof(req));
    ZeroMemory(out, sizeof(out));

    req.Pin.Property.Set   = KSPROPSETID_Pin;
    req.Pin.Property.Id    = KSPROPERTY_PIN_DATAINTERSECTION;
    req.Pin.Property.Flags = KSPROPERTY_TYPE_GET;
    req.Pin.PinId          = PinId;

    req.Item.Size  = sizeof(KSMULTIPLE_ITEM) + sizeof(KSDATARANGE_AUDIO);
    req.Item.Count = 1;

    req.Range.DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    req.Range.DataRange.Flags      = 0;
    req.Range.DataRange.SampleSize = 0;
    req.Range.DataRange.Reserved   = 0;
    req.Range.DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    req.Range.DataRange.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
    req.Range.DataRange.Specifier   = *Specifier;

    req.Range.MaximumChannels       = Channels;
    req.Range.MinimumBitsPerSample  = Bits;
    req.Range.MaximumBitsPerSample  = Bits;
    req.Range.MinimumSampleFrequency = Freq;
    req.Range.MaximumSampleFrequency = Freq;

    LogF(L"    DATAINTERSECTION pin %lu, %s, %lu ch / %lu bit / %lu Hz "
         L"(in=%lu bytes)\n",
         PinId, Label, Channels, Bits, Freq, (ULONG)sizeof(req));

    err = KsSyncIoctl(hFilter, IOCTL_KS_PROPERTY, &req, sizeof(req),
                      out, sizeof(out), &bytes);

    if (err != ERROR_SUCCESS)
    {
        LogF(L"        FAILED, GetLastError=%lu, bytes=%lu\n", err, bytes);
        return;
    }

    LogF(L"        OK, %lu bytes returned\n", bytes);

    if (bytes >= sizeof(KSDATAFORMAT))
    {
        PKSDATAFORMAT df = (PKSDATAFORMAT)out;

        LogF(L"        KSDATAFORMAT: FormatSize=%lu Flags=%08lX SampleSize=%lu\n",
             (ULONG)df->FormatSize, (ULONG)df->Flags, (ULONG)df->SampleSize);
        LogF(L"            Major     = %s\n", GuidName(&df->MajorFormat));
        LogF(L"            SubFormat = %s\n", GuidName(&df->SubFormat));
        LogF(L"            Specifier = %s\n", GuidName(&df->Specifier));

        if (IsEqualGUID(&df->Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
            bytes >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX))
        {
            WAVEFORMATEX *wf = (WAVEFORMATEX *)(df + 1);
            LogF(L"            wfx: tag=%u ch=%u rate=%lu bits=%u "
                 L"blockAlign=%u avgBytes=%lu cbSize=%u\n",
                 wf->wFormatTag, wf->nChannels, wf->nSamplesPerSec,
                 wf->wBitsPerSample, wf->nBlockAlign,
                 wf->nAvgBytesPerSec, wf->cbSize);
        }
        else if (IsEqualGUID(&df->Specifier, &KSDATAFORMAT_SPECIFIER_DSOUND) &&
                 bytes >= sizeof(KSDATAFORMAT_DSOUND))
        {
            PKSDATAFORMAT_DSOUND ds = (PKSDATAFORMAT_DSOUND)out;
            LogF(L"            dsound: Flags=%08lX Control=%08lX "
                 L"tag=%u ch=%u rate=%lu bits=%u\n",
                 (ULONG)ds->BufferDesc.Flags,
                 (ULONG)ds->BufferDesc.Control,
                 ds->BufferDesc.WaveFormatEx.wFormatTag,
                 ds->BufferDesc.WaveFormatEx.nChannels,
                 ds->BufferDesc.WaveFormatEx.nSamplesPerSec,
                 ds->BufferDesc.WaveFormatEx.wBitsPerSample);
        }
    }
}

static void
DumpFilter(const wchar_t *DevicePath)
{
    HANDLE hFilter;
    DWORD  bytes = 0;
    DWORD  err;
    ULONG  cTypes = 0;
    ULONG  pin;

    LogF(L"    Opening \"%s\"\n", DevicePath);

    hFilter = CreateFileW(DevicePath, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                          FILE_FLAG_OVERLAPPED, NULL);

    if (hFilter == INVALID_HANDLE_VALUE)
    {
        LogF(L"    CreateFile FAILED, GetLastError=%lu\n", GetLastError());
        return;
    }

    LogF(L"    CreateFile OK\n");

    err = GetPinProperty(hFilter, 0, KSPROPERTY_PIN_CTYPES,
                         &cTypes, sizeof(cTypes), &bytes);
    if (err != ERROR_SUCCESS)
    {
        LogF(L"    KSPROPERTY_PIN_CTYPES FAILED, GetLastError=%lu\n", err);
        CloseHandle(hFilter);
        return;
    }

    LogF(L"    KSPROPERTY_PIN_CTYPES = %lu pin(s)\n", cTypes);

    for (pin = 0; pin < cTypes; pin++)
    {
        KSPIN_DATAFLOW      flow = (KSPIN_DATAFLOW)0;
        KSPIN_COMMUNICATION comm = (KSPIN_COMMUNICATION)0;
        KSPIN_CINSTANCES    inst;
        PKSMULTIPLE_ITEM    mi;
        DWORD               needed = 0;

        LogF(L"\n    ---- pin %lu ----\n", pin);

        ZeroMemory(&inst, sizeof(inst));

        if (GetPinProperty(hFilter, pin, KSPROPERTY_PIN_DATAFLOW,
                           &flow, sizeof(flow), &bytes) == ERROR_SUCCESS)
            LogF(L"        DATAFLOW      = %s\n", DataFlowName(flow));
        else
            LogF(L"        DATAFLOW      query failed, err=%lu\n", GetLastError());

        if (GetPinProperty(hFilter, pin, KSPROPERTY_PIN_COMMUNICATION,
                           &comm, sizeof(comm), &bytes) == ERROR_SUCCESS)
            LogF(L"        COMMUNICATION = %s\n", CommName(comm));
        else
            LogF(L"        COMMUNICATION query failed, err=%lu\n", GetLastError());

        if (GetPinProperty(hFilter, pin, KSPROPERTY_PIN_CINSTANCES,
                           &inst, sizeof(inst), &bytes) == ERROR_SUCCESS)
            LogF(L"        CINSTANCES    = current %lu, possible %lu\n",
                 (ULONG)inst.CurrentCount, (ULONG)inst.PossibleCount);
        else
            LogF(L"        CINSTANCES    query failed, err=%lu\n", GetLastError());

        /*
         * Size probe first, exactly as sysaudio does: ask with a zero-length
         * output buffer, take the required size from the byte count.
         */
        err = GetPinProperty(hFilter, pin, KSPROPERTY_PIN_DATARANGES,
                             NULL, 0, &needed);

        LogF(L"        DATARANGES size probe: err=%lu needed=%lu\n", err, needed);

        if (needed < sizeof(KSMULTIPLE_ITEM))
        {
            LogF(L"        (no data ranges reported)\n");
            continue;
        }

        mi = (PKSMULTIPLE_ITEM)malloc(needed);
        if (!mi)
        {
            LogF(L"        out of memory for %lu bytes\n", needed);
            continue;
        }

        ZeroMemory(mi, needed);
        bytes = 0;

        err = GetPinProperty(hFilter, pin, KSPROPERTY_PIN_DATARANGES,
                             mi, needed, &bytes);

        if (err != ERROR_SUCCESS)
        {
            LogF(L"        DATARANGES fetch FAILED, err=%lu, bytes=%lu\n",
                 err, bytes);
        }
        else
        {
            LogF(L"        DATARANGES fetch OK, %lu bytes\n", bytes);
            DumpDataRanges(mi, bytes ? bytes : needed);
        }

        free(mi);
    }

    /*
     * Intersections against pin 0 (the render sink). Two specifiers, three
     * rates: the two we advertise plus 22050, which is what DirectSound's own
     * default primary buffer format uses.
     */
    LogF(L"\n    ---- KSPROPERTY_PIN_DATAINTERSECTION probes on pin 0 ----\n");

    TryIntersection(hFilter, 0, L"SPECIFIER_WAVEFORMATEX",
                    &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX, 2, 16, 48000);
    TryIntersection(hFilter, 0, L"SPECIFIER_WAVEFORMATEX",
                    &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX, 2, 16, 44100);
    TryIntersection(hFilter, 0, L"SPECIFIER_WAVEFORMATEX",
                    &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX, 2, 8, 22050);
    TryIntersection(hFilter, 0, L"SPECIFIER_DSOUND",
                    &KSDATAFORMAT_SPECIFIER_DSOUND, 2, 16, 48000);
    TryIntersection(hFilter, 0, L"SPECIFIER_DSOUND",
                    &KSDATAFORMAT_SPECIFIER_DSOUND, 2, 16, 44100);
    TryIntersection(hFilter, 0, L"SPECIFIER_DSOUND",
                    &KSDATAFORMAT_SPECIFIER_DSOUND, 2, 8, 22050);

    CloseHandle(hFilter);
}

static void
EnumerateCategory(const GUID *Category, const wchar_t *Name)
{
    HDEVINFO                          hDevInfo;
    SP_DEVICE_INTERFACE_DATA          ifData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
    DWORD                             idx;
    DWORD                             needed;

    LogF(L"\n================ %s ================\n", Name);

    hDevInfo = SetupDiGetClassDevsW(Category, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        LogF(L"SetupDiGetClassDevs FAILED, GetLastError=%lu\n", GetLastError());
        return;
    }

    ZeroMemory(&ifData, sizeof(ifData));
    ifData.cbSize = sizeof(ifData);

    for (idx = 0;
         SetupDiEnumDeviceInterfaces(hDevInfo, NULL, (LPGUID)Category, idx, &ifData);
         idx++)
    {
        needed = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, NULL, 0, &needed, NULL);

        if (needed == 0)
        {
            LogF(L"[%lu] size query FAILED, GetLastError=%lu\n",
                 idx, GetLastError());
            continue;
        }

        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(needed);
        if (!detail)
            continue;

        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail,
                                             needed, NULL, NULL))
        {
            LogF(L"\n[%lu] %s\n", idx, detail->DevicePath);
            DumpFilter(detail->DevicePath);
        }
        else
        {
            LogF(L"[%lu] detail query FAILED, GetLastError=%lu\n",
                 idx, GetLastError());
        }

        free(detail);
        ZeroMemory(&ifData, sizeof(ifData));
        ifData.cbSize = sizeof(ifData);
    }

    if (idx == 0)
        LogF(L"    (no interfaces in this category)\n");

    SetupDiDestroyDeviceInfoList(hDevInfo);
}

int __cdecl wmain(int argc, wchar_t **argv)
{
    wchar_t path[MAX_PATH];

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    if (GetCurrentDirectoryW(MAX_PATH, path))
    {
        wchar_t file[MAX_PATH + 32];
        _snwprintf(file, MAX_PATH + 31, L"%s\\%s", path, LOGNAME);
        file[MAX_PATH + 31] = L'\0';
        g_log = _wfopen(file, L"w");
    }

    LogF(L"pindump - what our KS pins actually advertise\n");
    LogF(L"Stage 5bo. Decodes DATARANGES and probes DATAINTERSECTION.\n");
    LogF(L"build: %s\n", BITNESS);
    LogF(L"sizeof(KSDATARANGE)=%lu sizeof(KSDATARANGE_AUDIO)=%lu "
         L"sizeof(KSMULTIPLE_ITEM)=%lu sizeof(KSP_PIN)=%lu\n",
         (ULONG)sizeof(KSDATARANGE), (ULONG)sizeof(KSDATARANGE_AUDIO),
         (ULONG)sizeof(KSMULTIPLE_ITEM), (ULONG)sizeof(KSP_PIN));
    LogF(L"======================================================================\n");

    EnumerateCategory(&KSCATEGORY_RENDER,  L"KSCATEGORY_RENDER");
    EnumerateCategory(&KSCATEGORY_CAPTURE, L"KSCATEGORY_CAPTURE");

    LogF(L"\n======================================================================\n");
    LogF(L"done.\n");

    if (g_log)
    {
        fclose(g_log);
        g_log = NULL;
    }

    return 0;
}
