import io

P = r"kstest.c"
s = io.open(P, encoding="utf-8", newline="").read()


def sub(old, new, what):
    global s
    n = s.count(old)
    assert n == 1, "%s: expected 1 occurrence, got %d" % (what, n)
    s = s.replace(old, new)
    print("patched: %s" % what)


# ---------------------------------------------------------------- 1. streaming
STREAM_CODE = r'''/*****************************************************************************
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
TestOpenBareInterface('''

sub("static void\nTestOpenBareInterface(", STREAM_CODE, "insert streaming code")

# ------------------------------------------------- 2. bare-interface verdict
sub(
    """    WCHAR  bare[1024];
    LPWSTR lastSlash;
    HANDLE hObj;
""",
    """    WCHAR  bare[1024];
    LPWSTR lastSlash;
    HANDLE hObj;
    DWORD  bareErr;
""",
    "bare-interface locals",
)

sub(
    """        wprintf(L"    Bare-interface CreateFile(0) FAILED, GetLastError=%lu\\n", GetLastError());
        wprintf(L"    ==> Denial happens on the base device object itself, before\\n");
        wprintf(L"        any reference-string/filter-factory name is even involved.\\n");
""",
    """        bareErr = GetLastError();
        wprintf(L"    Bare-interface CreateFile(0) FAILED, GetLastError=%lu\\n", bareErr);

        if (bareErr == ERROR_ACCESS_DENIED)
        {
            wprintf(L"    ==> ACCESS_DENIED on the base device object itself, before any\\n");
            wprintf(L"        reference-string/filter-factory name is even involved.\\n");
        }
        else
        {
            wprintf(L"    ==> NOT a denial. ERROR_FILE_NOT_FOUND (2) here is the EXPECTED\\n");
            wprintf(L"        result: PortCls registers its filter factories under the\\n");
            wprintf(L"        \\"\\\\wave\\"/\\"\\\\topology\\" reference strings, so the bare interface\\n");
            wprintf(L"        path names no openable object. Nothing is wrong.\\n");
        }
""",
    "bare-interface verdict text",
)

# --------------------------------------------------- 3. TestOpenAndPin plumbing
sub(
    """TestOpenAndPin(
    LPCWSTR DevicePath,
    ULONG   PinId
)""",
    """TestOpenAndPin(
    LPCWSTR DevicePath,
    ULONG   PinId,
    BOOL    Render
)""",
    "TestOpenAndPin signature",
)

sub(
    """        wprintf(L"    KsCreatePin SUCCEEDED - pin handle opened.\\n");
        CloseHandle(hPin);""",
    """        wprintf(L"    KsCreatePin SUCCEEDED - pin handle opened.\\n");

        if (Render)
        {
            StreamToneToPin(hPin);
        }
        else
        {
            wprintf(L"    (capture pin - no tone streamed)\\n");
        }

        CloseHandle(hPin);""",
    "TestOpenAndPin streaming call",
)

# -------------------------------------------------- 4. EnumerateAndTest plumbing
sub(
    """EnumerateAndTest(
    const GUID *Category,
    LPCWSTR     CategoryName,
    ULONG       PinId
)""",
    """EnumerateAndTest(
    const GUID *Category,
    LPCWSTR     CategoryName,
    ULONG       PinId,
    BOOL        Render
)""",
    "EnumerateAndTest signature",
)

sub(
    "            TestOpenAndPin(detail->DevicePath, PinId);",
    "            TestOpenAndPin(detail->DevicePath, PinId, Render);",
    "EnumerateAndTest call site",
)

# ------------------------------------------------------------------- 5. main
sub(
    """    EnumerateAndTest(&KSCATEGORY_RENDER, L"KSCATEGORY_RENDER (PinId=0, render)", 0);
    EnumerateAndTest(&KSCATEGORY_CAPTURE, L"KSCATEGORY_CAPTURE (PinId=1, capture)", 1);""",
    """    EnumerateAndTest(&KSCATEGORY_RENDER, L"KSCATEGORY_RENDER (PinId=0, render)", 0, TRUE);
    EnumerateAndTest(&KSCATEGORY_CAPTURE, L"KSCATEGORY_CAPTURE (PinId=1, capture)", 1, FALSE);""",
    "main call sites",
)

sub(
    """    wprintf(L"directly via CreateFile + KsCreatePin, bypassing sysaudio entirely.\\n");""",
    """    wprintf(L"directly via CreateFile + KsCreatePin, bypassing sysaudio entirely.\\n");
    wprintf(L"If the render pin opens, a 3-second 440 Hz tone is streamed to it.\\n");
    wprintf(L"TURN THE VOLUME UP AND LISTEN.\\n");""",
    "main banner",
)

io.open(P, "w", encoding="utf-8", newline="").write(s)
print("kstest.c written, %d bytes" % len(s.encode("utf-8")))
