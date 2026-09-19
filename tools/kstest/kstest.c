/*****************************************************************************
 * kstest.c
 *****************************************************************************
 * Standalone usermode diagnostic tool - NOT part of the shipped driver.
 *
 * Enumerates KSCATEGORY_RENDER and KSCATEGORY_CAPTURE device interfaces
 * directly via SetupDi*, opens each one with CreateFile, and attempts
 * KsCreatePin() on it. This tests our wave miniport's raw KS pin-open path
 * (wavecyclicminiport.cpp's NewStream) completely independent of both
 * sysaudio's wave-mapper discovery and any third-party GUI tool (KS Studio
 * turned out to be broken/unreliable on the target machine - see
 * HANDOFF.md Stage 5l/5m) - if a pin opens here, NewStream must have run.
 */

#define INITGUID

#include <windows.h>
#include <mmsystem.h>
#include <setupapi.h>
// winioctl.h must precede ks.h: ks.h builds IOCTL_KS_PROPERTY/IOCTL_KS_WRITE_STREAM
// out of CTL_CODE/METHOD_NEITHER/FILE_DEVICE_KS, which live here.
#include <winioctl.h>
#include <ks.h>
#include <ksmedia.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

// GUID_DEVINTERFACE_DISK - hardcoded here (rather than pulling in winioctl.h/
// ntddstor.h) so this control test has no extra header dependencies. Used to
// answer "is CreateFile access denied on THIS machine/account for ANY device
// interface, or only for our audio device?" - see HANDOFF.md Stage 5n/5o.
static const GUID GUID_DEVINTERFACE_DISK_CTL =
    { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };

typedef struct _PIN_CONNECT_FORMAT
{
    KSPIN_CONNECT              Connect;
    KSDATAFORMAT_WAVEFORMATEX  Format;
} PIN_CONNECT_FORMAT, *PPIN_CONNECT_FORMAT;

static BOOL
EnablePrivilege(
    LPCWSTR PrivilegeName
)
{
    HANDLE           hToken;
    TOKEN_PRIVILEGES tp;
    BOOL             ok;

    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        wprintf(L"    OpenProcessToken FAILED, GetLastError=%lu\n", GetLastError());
        return FALSE;
    }

    if (!LookupPrivilegeValueW(NULL, PrivilegeName, &tp.Privileges[0].Luid))
    {
        wprintf(L"    LookupPrivilegeValue(%s) FAILED, GetLastError=%lu\n",
            PrivilegeName, GetLastError());
        CloseHandle(hToken);
        return FALSE;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    if (!ok || GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        wprintf(L"    AdjustTokenPrivileges(%s) FAILED/NOT_ALL_ASSIGNED, GetLastError=%lu\n",
            PrivilegeName, GetLastError());
        ok = FALSE;
    }
    else
    {
        wprintf(L"    AdjustTokenPrivileges(%s) OK - privilege enabled.\n", PrivilegeName);
    }

    CloseHandle(hToken);
    return ok;
}

// Forcibly take ownership of the device object at DevicePath and install a
// wide-open DACL on it, bypassing whatever DACL currently denies us access.
// This is the same trick takeown.exe/icacls use on locked-down files: an
// account with SeTakeOwnershipPrivilege can seize ownership of ANY securable
// object regardless of its current DACL, and an object's owner is always
// implicitly granted READ_CONTROL/WRITE_DAC - so once we own it, we can set
// whatever DACL we like. Used here because two separate INF-level "Security"
// AddReg attempts (interface-level, then device-level) both had zero effect
// on this device's CreateFile access, indicating PortCls's own device-object
// creation doesn't consult those registry overrides at all - see HANDOFF.md
// Stage 5n-5r.
// Diagnostic that runs BEFORE any take-ownership attempt: a CreateFile with
// dwDesiredAccess=0 requests no access rights at all, so under standard NT
// object-manager semantics it ALWAYS succeeds regardless of the object's
// DACL (MSDN: "a dwDesiredAccess of zero allows the application to query
// device attributes without accessing the device"). If even THIS is denied,
// that's conclusive proof the ACCESS_DENIED is not a normal security check
// at all (nothing standard could produce it), and must instead be coming
// from somewhere in KS/PortCls's own IRP_MJ_CREATE dispatch explicitly
// rejecting the open for a driver-logic reason. If the zero-access open
// DOES succeed, we escalate to READ_CONTROL and, if that also succeeds,
// dump the actual owner/DACL as SDDL so we have ground truth instead of
// continued guessing. See HANDOFF.md Stage 5u.
static void
DiagnoseSecurityDenial(
    LPCWSTR DevicePath
)
{
    HANDLE               hObj;
    PSECURITY_DESCRIPTOR pSd = NULL;
    DWORD                result;
    LPWSTR               sddl = NULL;

    wprintf(L"    Diagnostic: CreateFile with dwDesiredAccess=0 (no rights requested)...\n");
    hObj = CreateFileW(DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hObj == INVALID_HANDLE_VALUE)
    {
        wprintf(L"    CreateFile(0) FAILED, GetLastError=%lu\n", GetLastError());
        wprintf(L"    ==> This is NOT a standard security-descriptor check (a\n");
        wprintf(L"        zero-access open always succeeds against any DACL). The\n");
        wprintf(L"        ACCESS_DENIED must be coming from driver/PortCls logic.\n");
        return;
    }
    wprintf(L"    CreateFile(0) OK - zero-access open succeeds.\n");
    CloseHandle(hObj);

    wprintf(L"    Diagnostic: CreateFile with READ_CONTROL only...\n");
    hObj = CreateFileW(DevicePath, READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hObj == INVALID_HANDLE_VALUE)
    {
        wprintf(L"    CreateFile(READ_CONTROL) FAILED, GetLastError=%lu\n", GetLastError());
        wprintf(L"    ==> Zero-access succeeds but READ_CONTROL is denied - this IS\n");
        wprintf(L"        being evaluated against a real DACL, just an unusually\n");
        wprintf(L"        restrictive one (denies even Administrators READ_CONTROL).\n");
        return;
    }
    wprintf(L"    CreateFile(READ_CONTROL) OK.\n");

    result = GetSecurityInfo(hObj, SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        NULL, NULL, NULL, NULL, &pSd);
    if (result != ERROR_SUCCESS)
    {
        wprintf(L"    GetSecurityInfo FAILED, error=%lu\n", result);
        CloseHandle(hObj);
        return;
    }

    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(pSd, SDDL_REVISION_1,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &sddl, NULL))
    {
        wprintf(L"    Actual security descriptor: %s\n", sddl);
        LocalFree(sddl);
    }
    else
    {
        wprintf(L"    ConvertSecurityDescriptorToStringSecurityDescriptor FAILED, GetLastError=%lu\n",
            GetLastError());
    }

    LocalFree(pSd);
    CloseHandle(hObj);
}

static void
TakeOwnershipAndOpenUp(
    LPCWSTR DevicePath
)
{
    BYTE                 adminSidBuf[SECURITY_MAX_SID_SIZE];
    PSID                 adminSid = (PSID)adminSidBuf;
    DWORD                adminSidSize = sizeof(adminSidBuf);
    BYTE                 everyoneSidBuf[SECURITY_MAX_SID_SIZE];
    PSID                 everyoneSid = (PSID)everyoneSidBuf;
    DWORD                everyoneSidSize = sizeof(everyoneSidBuf);
    EXPLICIT_ACCESS_W    ea;
    PACL                 newDacl;
    DWORD                result;
    HANDLE               hObj;

    wprintf(L"    Attempting to take ownership + reset DACL...\n");

    EnablePrivilege(L"SeTakeOwnershipPrivilege");
    EnablePrivilege(L"SeSecurityPrivilege");
    EnablePrivilege(L"SeRestorePrivilege");

    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, adminSid, &adminSidSize))
    {
        wprintf(L"    CreateWellKnownSid(Administrators) FAILED, GetLastError=%lu\n", GetLastError());
        return;
    }

    // SetNamedSecurityInfoW can't parse a \\?\...#{GUID}\ref device-interface
    // path (fails with ERROR_BAD_PATHNAME) - go through a HANDLE instead.
    // Opening with ONLY WRITE_OWNER requested (no GENERIC_READ/WRITE) succeeds
    // even though the DACL denies everything, because SeTakeOwnershipPrivilege
    // makes the security reference monitor grant WRITE_OWNER unconditionally.
    hObj = CreateFileW(DevicePath, WRITE_OWNER, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hObj == INVALID_HANDLE_VALUE)
    {
        wprintf(L"    CreateFile(WRITE_OWNER) FAILED, GetLastError=%lu\n", GetLastError());
        return;
    }

    result = SetSecurityInfo(hObj, SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION, adminSid, NULL, NULL, NULL);
    CloseHandle(hObj);

    if (result != ERROR_SUCCESS)
    {
        wprintf(L"    SetSecurityInfo (take ownership) FAILED, error=%lu\n", result);
        return;
    }
    wprintf(L"    Ownership taken by BUILTIN\\Administrators.\n");

    if (!CreateWellKnownSid(WinWorldSid, NULL, everyoneSid, &everyoneSidSize))
    {
        wprintf(L"    CreateWellKnownSid(Everyone) FAILED, GetLastError=%lu\n", GetLastError());
        return;
    }

    ZeroMemory(&ea, sizeof(ea));
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = NO_INHERITANCE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName    = (LPWSTR)everyoneSid;

    newDacl = NULL;
    result = SetEntriesInAclW(1, &ea, NULL, &newDacl);
    if (result != ERROR_SUCCESS || newDacl == NULL)
    {
        wprintf(L"    SetEntriesInAcl FAILED, error=%lu\n", result);
        return;
    }

    // Now that we own the object, WRITE_DAC is implicitly granted to us
    // regardless of the (still-restrictive) DACL, so this open succeeds too.
    hObj = CreateFileW(DevicePath, WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hObj == INVALID_HANDLE_VALUE)
    {
        wprintf(L"    CreateFile(WRITE_DAC) FAILED, GetLastError=%lu\n", GetLastError());
        LocalFree(newDacl);
        return;
    }

    result = SetSecurityInfo(hObj, SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION, NULL, NULL, newDacl, NULL);
    CloseHandle(hObj);

    if (result != ERROR_SUCCESS)
    {
        wprintf(L"    SetSecurityInfo (set DACL) FAILED, error=%lu\n", result);
    }
    else
    {
        wprintf(L"    New DACL installed: Everyone/GENERIC_ALL.\n");
    }

    LocalFree(newDacl);
}

// Diagnostic (see HANDOFF.md Stage 5ab): DumpDeviceSecurity proved our
// driver's own device object has a fully permissive DACL (Everyone/System/
// Administrators/Restricted Code all granted broad access), yet even a
// zero-access open of the FULL device-interface path (base symbolic link +
// "\wave"/"\topology" reference-string suffix) is denied, and never reaches
// our IRP_MJ_CREATE hook. This test discriminates between two remaining
// explanations: (a) the base device object itself is somehow unreachable/
// denied despite its own permissive SD (e.g. a stale/mismatched device-
// interface registration), or (b) the base object IS reachable fine, and
// the denial is specific to KS's handling of a named sub-object ("\wave"/
// "\topology") - e.g. a per-filter-factory security descriptor set by KS/
// PortCls when PcRegisterSubdevice created that named object, independent
// of the device object's own DACL we already dumped. Strip the trailing
// "\<reference-string>" off DevicePath (everything after the last '\') and
// try opening THAT bare path directly.
/*****************************************************************************
 * Stage 5al: raw KS tone streaming
 *****************************************************************************
 * Now that KsCreatePin() succeeds, the next question is whether the driver's
 * DMA/BDL/codec path actually produces sound. This writes real PCM straight
 * down the open pin with IOCTL_KS_WRITE_STREAM, bypassing sysaudio, kmixer and
 * wdmaud entirely. Audible tone => the whole kernel-side render path works and
 * the remaining "no audio device" symptom is purely graph/topology plumbing
 * above us. Silence but clean completions => codec routing/unmute. Writes that
 * never complete => DMA position or the servicing timer is not advancing.
 */

#define TONE_SAMPLE_RATE   44100
#define TONE_CHANNELS      2
#define TONE_FREQ_HZ       440
#define TONE_SECONDS       3
#define TONE_BUFFERS       8

/* 16-point sine, linearly interpolated via a 32-bit phase accumulator - keeps
   this integer-only, so no float math and nothing for /W4 /WX to complain
   about in a conversion. */
static const short g_SineTable[16] =
{
         0,  12539,  23170,  30273,  32767,  30273,  23170,  12539,
         0, -12539, -23170, -30273, -32767, -30273, -23170, -12539
};

/*
 * KS handles are asynchronous, so even a logically synchronous property call
 * has to go through OVERLAPPED and wait for its own completion.
 */
static DWORD
KsSyncIoctl(
    HANDLE hObject,
    DWORD  IoControlCode,
    PVOID  InBuf,
    DWORD  InLen,
    PVOID  OutBuf,
    DWORD  OutLen
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
    return err;
}

static DWORD
SetPinState(
    HANDLE  hPin,
    KSSTATE State
)
{
    KSPROPERTY prop;
    KSSTATE    state = State;

    ZeroMemory(&prop, sizeof(prop));
    prop.Set   = KSPROPSETID_Connection;
    prop.Id    = KSPROPERTY_CONNECTION_STATE;
    prop.Flags = KSPROPERTY_TYPE_SET;

    return KsSyncIoctl(hPin, IOCTL_KS_PROPERTY,
        &prop, sizeof(prop), &state, sizeof(state));
}

static void
StreamToneToPin(
    HANDLE hPin
)
{
    const DWORD frameBytes  = TONE_CHANNELS * 2;
    const DWORD totalFrames = TONE_SAMPLE_RATE * TONE_SECONDS;
    const DWORD bufFrames   = totalFrames / TONE_BUFFERS;
    const DWORD bufBytes    = bufFrames * frameBytes;

    KSSTREAM_HEADER hdr[TONE_BUFFERS];
    OVERLAPPED      ov[TONE_BUFFERS];
    PBYTE           data[TONE_BUFFERS];
    HANDLE          waits[TONE_BUFFERS];
    unsigned int    phase = 0;
    unsigned int    inc;
    DWORD           i, j, err, bytes;
    int             submitted = 0;

    ZeroMemory(hdr,   sizeof(hdr));
    ZeroMemory(ov,    sizeof(ov));
    ZeroMemory(data,  sizeof(data));
    ZeroMemory(waits, sizeof(waits));

    inc = (unsigned int)((((unsigned __int64)TONE_FREQ_HZ) << 32) / TONE_SAMPLE_RATE);

    wprintf(L"    Streaming a %lu Hz tone for %lu s (%lu buffers x %lu bytes)...\n",
        (ULONG)TONE_FREQ_HZ, (ULONG)TONE_SECONDS, (ULONG)TONE_BUFFERS, bufBytes);

    for (i = 0; i < TONE_BUFFERS; i++)
    {
        short *smp;

        data[i]      = (PBYTE)VirtualAlloc(NULL, bufBytes, MEM_COMMIT, PAGE_READWRITE);
        ov[i].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

        if (data[i] == NULL || ov[i].hEvent == NULL)
        {
            wprintf(L"    Allocation failed for buffer %lu\n", i);
            goto cleanup;
        }

        smp = (short *)data[i];
        for (j = 0; j < bufFrames; j++)
        {
            unsigned int idx  = phase >> 28;
            unsigned int frac = (phase >> 12) & 0xFFFF;
            int          a    = g_SineTable[idx];
            int          b    = g_SineTable[(idx + 1) & 15];
            short        v    = (short)((a + (((b - a) * (int)frac) >> 16)) / 4);

            smp[j * 2]     = v;
            smp[j * 2 + 1] = v;
            phase += inc;
        }

        hdr[i].Size                         = sizeof(KSSTREAM_HEADER);
        hdr[i].PresentationTime.Numerator   = 1;
        hdr[i].PresentationTime.Denominator = 1;
        hdr[i].Data                         = data[i];
        hdr[i].FrameExtent                  = bufBytes;
        hdr[i].DataUsed                     = bufBytes;
    }

    /* STOP -> ACQUIRE -> PAUSE, queue every buffer, then RUN. */
    err = SetPinState(hPin, KSSTATE_ACQUIRE);
    wprintf(L"    KSSTATE_ACQUIRE -> %s (%lu)\n", err ? L"FAILED" : L"ok", err);
    if (err != ERROR_SUCCESS)
        goto cleanup;

    err = SetPinState(hPin, KSSTATE_PAUSE);
    wprintf(L"    KSSTATE_PAUSE   -> %s (%lu)\n", err ? L"FAILED" : L"ok", err);
    if (err != ERROR_SUCCESS)
        goto stopit;

    for (i = 0; i < TONE_BUFFERS; i++)
    {
        bytes = 0;
        if (!DeviceIoControl(hPin, IOCTL_KS_WRITE_STREAM, NULL, 0,
                             &hdr[i], hdr[i].Size, &bytes, &ov[i]))
        {
            err = GetLastError();
            if (err != ERROR_IO_PENDING)
            {
                wprintf(L"    IOCTL_KS_WRITE_STREAM buffer %lu FAILED, error=%lu\n", i, err);
                break;
            }
        }
        waits[submitted++] = ov[i].hEvent;
    }

    wprintf(L"    %d of %lu buffers queued.\n", submitted, (ULONG)TONE_BUFFERS);
    if (submitted == 0)
        goto stopit;

    err = SetPinState(hPin, KSSTATE_RUN);
    wprintf(L"    KSSTATE_RUN     -> %s (%lu)\n", err ? L"FAILED" : L"ok", err);
    if (err != ERROR_SUCCESS)
        goto stopit;

    wprintf(L"    >>> LISTEN NOW - waiting for the queued buffers to drain <<<\n");

    err = WaitForMultipleObjects((DWORD)submitted, waits, TRUE,
                                 (TONE_SECONDS + 5) * 1000);
    if (err == WAIT_TIMEOUT)
    {
        wprintf(L"    TIMED OUT waiting for completion. The driver ACCEPTED the writes\n");
        wprintf(L"    but never completed them - DMA position or the servicing timer\n");
        wprintf(L"    is not advancing (look at GetPosition/TimerDpcRoutine).\n");
    }
    else if (err == WAIT_FAILED)
    {
        wprintf(L"    WaitForMultipleObjects FAILED, error=%lu\n", GetLastError());
    }
    else
    {
        wprintf(L"    All %d buffers completed - the driver consumed the data.\n", submitted);
        wprintf(L"    If you heard nothing, the render path ran but the codec is\n");
        wprintf(L"    muted/misrouted; if you heard the tone, the driver WORKS.\n");
    }

stopit:
    SetPinState(hPin, KSSTATE_PAUSE);
    SetPinState(hPin, KSSTATE_ACQUIRE);
    SetPinState(hPin, KSSTATE_STOP);

cleanup:
    /* Any still-pending write IRP points at these buffers, so cancel and let
       the cancellations land BEFORE freeing anything. */
    CancelIo(hPin);
    if (submitted > 0)
        WaitForMultipleObjects((DWORD)submitted, waits, TRUE, 3000);

    for (i = 0; i < TONE_BUFFERS; i++)
    {
        if (ov[i].hEvent != NULL)
            CloseHandle(ov[i].hEvent);
        if (data[i] != NULL)
            VirtualFree(data[i], 0, MEM_RELEASE);
    }
}

static void
TestOpenBareInterface(
    LPCWSTR DevicePath
)
{
    WCHAR  bare[1024];
    LPWSTR lastSlash;
    HANDLE hObj;
    DWORD  bareErr;

    wcsncpy(bare, DevicePath, ARRAYSIZE(bare) - 1);
    bare[ARRAYSIZE(bare) - 1] = L'\0';

    lastSlash = wcsrchr(bare, L'\\');
    if (lastSlash == NULL || lastSlash == bare)
    {
        wprintf(L"    (no reference-string suffix to strip - skipping bare-interface test)\n");
        return;
    }
    *lastSlash = L'\0';

    wprintf(L"    Bare-interface diagnostic: CreateFile(0) on \"%s\" (no reference-string suffix)...\n", bare);
    hObj = CreateFileW(bare, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hObj == INVALID_HANDLE_VALUE)
    {
        bareErr = GetLastError();
        wprintf(L"    Bare-interface CreateFile(0) FAILED, GetLastError=%lu\n", bareErr);

        if (bareErr == ERROR_ACCESS_DENIED)
        {
            wprintf(L"    ==> ACCESS_DENIED on the base device object itself, before any\n");
            wprintf(L"        reference-string/filter-factory name is even involved.\n");
        }
        else
        {
            wprintf(L"    ==> NOT a denial. ERROR_FILE_NOT_FOUND (2) here is the EXPECTED\n");
            wprintf(L"        result: PortCls registers its filter factories under the\n");
            wprintf(L"        \"\\wave\"/\"\\topology\" reference strings, so the bare interface\n");
            wprintf(L"        path names no openable object. Nothing is wrong.\n");
        }
    }
    else
    {
        wprintf(L"    Bare-interface CreateFile(0) OK.\n");
        wprintf(L"    ==> Base device object is reachable and open-able; the denial\n");
        wprintf(L"        is specific to the \"\\<reference-string>\" named sub-object\n");
        wprintf(L"        (KS filter-factory-level check, not the device object's own DACL).\n");
        CloseHandle(hObj);
    }
}

static void
TestOpenAndPin(
    LPCWSTR DevicePath,
    ULONG   PinId,
    BOOL    Render
)
{
    HANDLE hFilter;
    HANDLE hPin;
    DWORD  result;
    PIN_CONNECT_FORMAT pcf;

    TestOpenBareInterface(DevicePath);

    wprintf(L"    Opening filter via CreateFile...\n");

    hFilter = CreateFileW(DevicePath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (hFilter == INVALID_HANDLE_VALUE)
    {
        DWORD firstError = GetLastError();
        wprintf(L"    CreateFile FAILED, GetLastError=%lu\n", firstError);

        if (firstError == ERROR_ACCESS_DENIED)
        {
            DiagnoseSecurityDenial(DevicePath);

            TakeOwnershipAndOpenUp(DevicePath);

            wprintf(L"    Retrying CreateFile...\n");
            hFilter = CreateFileW(DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

            if (hFilter == INVALID_HANDLE_VALUE)
            {
                wprintf(L"    Retry CreateFile FAILED, GetLastError=%lu\n", GetLastError());
                return;
            }
            wprintf(L"    Retry CreateFile OK\n");
        }
        else
        {
            return;
        }
    }
    else
    {
        wprintf(L"    CreateFile OK\n");
    }

    ZeroMemory(&pcf, sizeof(pcf));

    pcf.Connect.Interface.Set      = KSINTERFACESETID_Standard;
    pcf.Connect.Interface.Id       = KSINTERFACE_STANDARD_STREAMING;
    pcf.Connect.Interface.Flags    = 0;
    pcf.Connect.Medium.Set         = KSMEDIUMSETID_Standard;
    pcf.Connect.Medium.Id          = KSMEDIUM_TYPE_ANYINSTANCE;
    pcf.Connect.Medium.Flags       = 0;
    pcf.Connect.PinId              = PinId;
    pcf.Connect.PinToHandle        = NULL;
    pcf.Connect.Priority.PriorityClass    = KSPRIORITY_NORMAL;
    pcf.Connect.Priority.PrioritySubClass = 1;

    pcf.Format.DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    pcf.Format.DataFormat.Flags      = 0;
    pcf.Format.DataFormat.SampleSize = 4;
    pcf.Format.DataFormat.Reserved   = 0;
    pcf.Format.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    pcf.Format.DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
    pcf.Format.DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    pcf.Format.WaveFormatEx.wFormatTag      = WAVE_FORMAT_PCM;
    pcf.Format.WaveFormatEx.nChannels       = 2;
    pcf.Format.WaveFormatEx.nSamplesPerSec  = 44100;
    pcf.Format.WaveFormatEx.nAvgBytesPerSec = 44100 * 4;
    pcf.Format.WaveFormatEx.nBlockAlign     = 4;
    pcf.Format.WaveFormatEx.wBitsPerSample  = 16;
    pcf.Format.WaveFormatEx.cbSize          = 0;

    wprintf(L"    Calling KsCreatePin(PinId=%lu, 44100/16/stereo PCM)...\n", PinId);

    hPin = NULL;
    result = KsCreatePin(hFilter, &pcf.Connect, GENERIC_WRITE, &hPin);

    if (result == ERROR_SUCCESS)
    {
        wprintf(L"    KsCreatePin SUCCEEDED - pin handle opened.\n");

        if (Render)
        {
            StreamToneToPin(hPin);
        }
        else
        {
            wprintf(L"    (capture pin - no tone streamed)\n");
        }

        CloseHandle(hPin);
    }
    else
    {
        wprintf(L"    KsCreatePin FAILED, error=%lu (0x%08lX)\n", result, result);
    }

    CloseHandle(hFilter);
}

static void
EnumerateAndTest(
    const GUID *Category,
    LPCWSTR     CategoryName,
    ULONG       PinId,
    BOOL        Render
)
{
    HDEVINFO hDevInfo;
    SP_DEVICE_INTERFACE_DATA ifData;
    DWORD idx;
    DWORD found = 0;

    wprintf(L"\n=== Enumerating %s ===\n", CategoryName);

    hDevInfo = SetupDiGetClassDevsW(Category, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        wprintf(L"SetupDiGetClassDevs FAILED, GetLastError=%lu\n", GetLastError());
        return;
    }

    ifData.cbSize = sizeof(ifData);

    for (idx = 0;
         SetupDiEnumDeviceInterfaces(hDevInfo, NULL, Category, idx, &ifData);
         idx++)
    {
        DWORD needed = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
        SP_DEVINFO_DATA devInfoData;

        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, NULL, 0, &needed, NULL);
        if (needed == 0)
        {
            wprintf(L"[%lu] SetupDiGetDeviceInterfaceDetail (size query) FAILED, GetLastError=%lu\n",
                idx, GetLastError());
            continue;
        }

        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(needed);
        if (!detail)
        {
            wprintf(L"[%lu] out of memory\n", idx);
            continue;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        devInfoData.cbSize = sizeof(devInfoData);

        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, needed,
                NULL, &devInfoData))
        {
            found++;
            wprintf(L"\n[%lu] %s\n", idx, detail->DevicePath);
            TestOpenAndPin(detail->DevicePath, PinId, Render);
        }
        else
        {
            wprintf(L"[%lu] SetupDiGetDeviceInterfaceDetail FAILED, GetLastError=%lu\n",
                idx, GetLastError());
        }

        free(detail);
    }

    if (found == 0)
    {
        wprintf(L"(no device interfaces found for this category)\n");
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
}

static void
ControlTestDisk(void)
{
    HDEVINFO hDevInfo;
    SP_DEVICE_INTERFACE_DATA ifData;
    DWORD needed = 0;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
    HANDLE hDisk;

    wprintf(L"\n=== Control test: GUID_DEVINTERFACE_DISK (unrelated to audio/KS) ===\n");

    hDevInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK_CTL, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        wprintf(L"SetupDiGetClassDevs FAILED, GetLastError=%lu\n", GetLastError());
        return;
    }

    ifData.cbSize = sizeof(ifData);

    if (!SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_DISK_CTL, 0, &ifData))
    {
        wprintf(L"(no disk device interfaces found - control test skipped)\n");
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return;
    }

    SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, NULL, 0, &needed, NULL);
    detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(needed);
    if (!detail)
    {
        wprintf(L"out of memory\n");
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return;
    }
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, needed, NULL, NULL))
    {
        wprintf(L"SetupDiGetDeviceInterfaceDetail FAILED, GetLastError=%lu\n", GetLastError());
        free(detail);
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return;
    }

    wprintf(L"%s\n", detail->DevicePath);
    wprintf(L"    Opening via CreateFile (GENERIC_READ, share read+write)...\n");

    hDisk = CreateFileW(detail->DevicePath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (hDisk == INVALID_HANDLE_VALUE)
    {
        wprintf(L"    CreateFile FAILED, GetLastError=%lu\n", GetLastError());
    }
    else
    {
        wprintf(L"    CreateFile OK - this machine/account CAN open device interfaces in general.\n");
        CloseHandle(hDisk);
    }

    free(detail);
    SetupDiDestroyDeviceInfoList(hDevInfo);
}

// A/B control test: is ERROR_ACCESS_DENIED specific to OUR filter, or does
// EVERY KS audio filter on this machine deny CreateFile the same way? XP
// always registers at least one other real KS audio filter under
// KSCATEGORY_AUDIO even with no sound hardware present at all - the software
// wavetable MIDI synth (swmidi.sys, "Microsoft GS Wavetable SW Synth"). If
// that filter is ALSO denied, the problem is machine-wide (some KS/PortCls
// policy or environment issue unrelated to our driver). If it opens fine,
// the problem is isolated to something specific about our filter's own
// registration/descriptor. Plain CreateFile only (no KsCreatePin, no
// take-ownership dance) - just need the open result for each. See
// HANDOFF.md Stage 5v.
static void
ControlTestOtherAudioFilters(void)
{
    HDEVINFO hDevInfo;
    SP_DEVICE_INTERFACE_DATA ifData;
    DWORD idx;
    DWORD found = 0;

    wprintf(L"\n=== Control test: ALL KSCATEGORY_AUDIO filters on this machine ===\n");

    hDevInfo = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        wprintf(L"SetupDiGetClassDevs FAILED, GetLastError=%lu\n", GetLastError());
        return;
    }

    ifData.cbSize = sizeof(ifData);

    for (idx = 0;
         SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &KSCATEGORY_AUDIO, idx, &ifData);
         idx++)
    {
        DWORD needed = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
        HANDLE hObj;

        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, NULL, 0, &needed, NULL);
        if (needed == 0)
        {
            continue;
        }

        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(needed);
        if (!detail)
        {
            continue;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, needed, NULL, NULL))
        {
            found++;
            wprintf(L"\n[%lu] %s\n", idx, detail->DevicePath);

            hObj = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (hObj == INVALID_HANDLE_VALUE)
            {
                wprintf(L"    CreateFile FAILED, GetLastError=%lu\n", GetLastError());
            }
            else
            {
                wprintf(L"    CreateFile OK\n");
                CloseHandle(hObj);
            }
        }

        free(detail);
    }

    if (found == 0)
    {
        wprintf(L"(no KSCATEGORY_AUDIO device interfaces found at all)\n");
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
}

int __cdecl
main(void)
{
    wprintf(L"kstest - raw KS pin-open diagnostic\n");
    wprintf(L"Testing our wave filter's render pin (PinId=0) and capture pin (PinId=1)\n");
    wprintf(L"directly via CreateFile + KsCreatePin, bypassing sysaudio entirely.\n");
    wprintf(L"If the render pin opens, a 3-second 440 Hz tone is streamed to it.\n");
    wprintf(L"TURN THE VOLUME UP AND LISTEN.\n");

    EnumerateAndTest(&KSCATEGORY_RENDER, L"KSCATEGORY_RENDER (PinId=0, render)", 0, TRUE);
    EnumerateAndTest(&KSCATEGORY_CAPTURE, L"KSCATEGORY_CAPTURE (PinId=1, capture)", 1, FALSE);

    ControlTestDisk();
    ControlTestOtherAudioFilters();

    wprintf(L"\nDone.\n");

    return 0;
}
