/*
 * dstest.c - DirectSound / waveOut diagnostic probe for Windows XP x64.
 *
 * Stage 5bj. Every previous stage asked "did the driver answer this property
 * correctly?" and the answer was always yes. Windows Media Player opens our
 * wave filter, completes a KSPROPERTY_AUDIO_CHANNEL_CONFIG set and a
 * KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY set - both returning
 * STATUS_SUCCESS - and then stops without ever asking for a pin data
 * intersection or creating a pin. Nothing fails inside the driver. So the
 * decision to give up is being made in user mode, above us, and the driver
 * log physically cannot show it.
 *
 * This program walks the exact DirectSound initialisation sequence that WMP's
 * DirectSound renderer filter uses, and prints the HRESULT of every single
 * step. The point is to convert WMP's opaque "There is a problem with your
 * sound device" into a specific error code at a specific call.
 *
 * The two fields I most want to see are DSCAPS.dwMinSecondarySampleRate and
 * dwMaxSecondarySampleRate. Those are derived by sysaudio from the data
 * ranges our pin advertises. We advertise only 44100 and 48000, because that
 * is genuinely all the codec's rate bitmap (000E05E0) reports below 88200.
 * If DirectSound has concluded from that that it cannot create a buffer at
 * some rate WMP wants, CreateSoundBuffer will fail here with DSERR_BADFORMAT
 * and we will see exactly which rates are refused - while VLC keeps working
 * because it uses waveOut, where kmixer resamples unconditionally.
 *
 * That is a hypothesis, not a diagnosis. The value of this program does not
 * depend on it being right: it reports where DirectSound actually stops.
 *
 * Build: see dstest_build.cmd. Targets WNET/amd64 (Server 2003 x64 == XP x64)
 * and links the static CRT, so the .exe needs nothing installed on the target.
 *
 * Output goes to stdout AND to dstest_log.txt beside the executable.
 */

#define _WIN32_WINNT 0x0502
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
#include <string.h>

static FILE *g_log = NULL;

static void L(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    fputs(buf, stdout);
    fflush(stdout);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
}

static char  g_logPath[MAX_PATH] = "";

/* Module-derived, never relative: a relative fopen() lands wherever CWD
 * happens to be and the log becomes unfindable.  Stage 5bj lesson. */
static void OpenLog(void)
{
    char mod[MAX_PATH];
    DWORD n;
    char *p;

    n = GetModuleFileNameA(NULL, mod, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        mod[MAX_PATH - 1] = 0;
        p = strrchr(mod, '\\');
        if (p) {
            *(p + 1) = 0;
            _snprintf(g_logPath, sizeof(g_logPath) - 1, "%sdstest_log.txt", mod);
            g_logPath[sizeof(g_logPath) - 1] = 0;
            g_log = fopen(g_logPath, "w");
            if (g_log) return;
        }
    }
    if (GetTempPathA(MAX_PATH, mod) > 0) {
        _snprintf(g_logPath, sizeof(g_logPath) - 1, "%sdstest_log.txt", mod);
        g_logPath[sizeof(g_logPath) - 1] = 0;
        g_log = fopen(g_logPath, "w");
        if (g_log) return;
    }
    strcpy(g_logPath, "dstest_log.txt");
    g_log = fopen(g_logPath, "w");
}

/* Decode the HRESULTs DirectSound actually returns. Anything unrecognised is
 * printed raw rather than guessed at. */
static const char *HrName(HRESULT hr)
{
    switch ((ULONG)hr)
    {
    case (ULONG)DS_OK:                     return "DS_OK";
    case (ULONG)DS_NO_VIRTUALIZATION:      return "DS_NO_VIRTUALIZATION";
    case (ULONG)DSERR_ALLOCATED:           return "DSERR_ALLOCATED (resource already in use)";
    case (ULONG)DSERR_CONTROLUNAVAIL:      return "DSERR_CONTROLUNAVAIL";
    case (ULONG)DSERR_INVALIDPARAM:        return "DSERR_INVALIDPARAM";
    case (ULONG)DSERR_INVALIDCALL:         return "DSERR_INVALIDCALL";
    case (ULONG)DSERR_GENERIC:             return "DSERR_GENERIC";
    case (ULONG)DSERR_PRIOLEVELNEEDED:     return "DSERR_PRIOLEVELNEEDED";
    case (ULONG)DSERR_OUTOFMEMORY:         return "DSERR_OUTOFMEMORY";
    case (ULONG)DSERR_BADFORMAT:           return "DSERR_BADFORMAT  <-- format refused";
    case (ULONG)DSERR_UNSUPPORTED:         return "DSERR_UNSUPPORTED";
    case (ULONG)DSERR_NODRIVER:            return "DSERR_NODRIVER   <-- no driver found";
    case (ULONG)DSERR_ALREADYINITIALIZED:  return "DSERR_ALREADYINITIALIZED";
    case (ULONG)DSERR_NOAGGREGATION:       return "DSERR_NOAGGREGATION";
    case (ULONG)DSERR_BUFFERLOST:          return "DSERR_BUFFERLOST";
    case (ULONG)DSERR_OTHERAPPHASPRIO:     return "DSERR_OTHERAPPHASPRIO";
    case (ULONG)DSERR_UNINITIALIZED:       return "DSERR_UNINITIALIZED";
    case (ULONG)DSERR_NOINTERFACE:         return "DSERR_NOINTERFACE";
    case (ULONG)DSERR_ACCESSDENIED:        return "DSERR_ACCESSDENIED";
    case (ULONG)DSERR_BUFFERTOOSMALL:      return "DSERR_BUFFERTOOSMALL";
    case (ULONG)DSERR_DS8_REQUIRED:        return "DSERR_DS8_REQUIRED";
    case (ULONG)DSERR_SENDLOOP:            return "DSERR_SENDLOOP";
    case (ULONG)DSERR_BADSENDBUFFERGUID:   return "DSERR_BADSENDBUFFERGUID";
    case (ULONG)DSERR_OBJECTNOTFOUND:      return "DSERR_OBJECTNOTFOUND";
    default:                               return "(unrecognised)";
    }
}

#define HR(x) L("        -> %08X  %s\n", (ULONG)(x), HrName(x))

/* ---------------------------------------------------------------- waveOut --
 * This is the path that already works (system sounds, VLC). It is here as the
 * control: whatever DirectSound reports has to be read against what waveOut
 * reports for the same device. dwFormats in particular is a bitmask of the
 * standard formats the wave driver claims, which wdmaud derives from our pin.
 */
static const struct { DWORD bit; const char *name; } kWaveFormats[] = {
    { WAVE_FORMAT_1M08, "11.025k  8bit mono"   },
    { WAVE_FORMAT_1S08, "11.025k  8bit stereo" },
    { WAVE_FORMAT_1M16, "11.025k 16bit mono"   },
    { WAVE_FORMAT_1S16, "11.025k 16bit stereo" },
    { WAVE_FORMAT_2M08, "22.05k   8bit mono"   },
    { WAVE_FORMAT_2S08, "22.05k   8bit stereo" },
    { WAVE_FORMAT_2M16, "22.05k  16bit mono"   },
    { WAVE_FORMAT_2S16, "22.05k  16bit stereo" },
    { WAVE_FORMAT_4M08, "44.1k    8bit mono"   },
    { WAVE_FORMAT_4S08, "44.1k    8bit stereo" },
    { WAVE_FORMAT_4M16, "44.1k   16bit mono"   },
    { WAVE_FORMAT_4S16, "44.1k   16bit stereo" },
    { WAVE_FORMAT_48M08,"48k      8bit mono"   },
    { WAVE_FORMAT_48S08,"48k      8bit stereo" },
    { WAVE_FORMAT_48M16,"48k     16bit mono"   },
    { WAVE_FORMAT_48S16,"48k     16bit stereo" },
    { WAVE_FORMAT_96M08,"96k      8bit mono"   },
    { WAVE_FORMAT_96S08,"96k      8bit stereo" },
    { WAVE_FORMAT_96M16,"96k     16bit mono"   },
    { WAVE_FORMAT_96S16,"96k     16bit stereo" },
};

static void ProbeWaveOut(void)
{
    UINT n, i;
    size_t f;

    L("================ waveOut (the path that already works) ================\n");
    n = waveOutGetNumDevs();
    L("waveOutGetNumDevs = %u\n", n);

    for (i = 0; i < n; i++)
    {
        WAVEOUTCAPSA c;
        MMRESULT mr = waveOutGetDevCapsA(i, &c, sizeof(c));
        if (mr != MMSYSERR_NOERROR) { L("  dev %u: waveOutGetDevCaps failed, mmr=%u\n", i, mr); continue; }
        L("  dev %u: \"%s\"\n", i, c.szPname);
        L("          mid=%04X pid=%04X channels=%u support=%08X formats=%08X\n",
          c.wMid, c.wPid, c.wChannels, c.dwSupport, c.dwFormats);
        for (f = 0; f < sizeof(kWaveFormats)/sizeof(kWaveFormats[0]); f++)
            if (c.dwFormats & kWaveFormats[f].bit)
                L("            + %s\n", kWaveFormats[f].name);
        if (c.dwFormats == 0)
            L("            (dwFormats is ZERO - the wave driver advertises no standard format)\n");
    }
    L("\n");
}

/* ------------------------------------------------------- DirectSound enum -- */
static int g_devCount = 0;
static GUID g_devGuid[8];

static BOOL CALLBACK DsEnumCb(LPGUID guid, LPCSTR desc, LPCSTR mod, LPVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    L("  [%d] %s\n", g_devCount, desc ? desc : "(null)");
    L("      module = %s\n", mod && *mod ? mod : "(none)");
    if (guid)
    {
        L("      guid   = {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
          guid->Data1, guid->Data2, guid->Data3,
          guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
          guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
        if (g_devCount < 8) g_devGuid[g_devCount] = *guid;
    }
    else
    {
        L("      guid   = NULL (primary / default device entry)\n");
        if (g_devCount < 8) ZeroMemory(&g_devGuid[g_devCount], sizeof(GUID));
    }
    g_devCount++;
    return TRUE;
}

/* --------------------------------------------------------------- DSCAPS --- */
static const struct { DWORD bit; const char *name; } kDsCaps[] = {
    { DSCAPS_PRIMARYMONO,      "PRIMARYMONO"      },
    { DSCAPS_PRIMARYSTEREO,    "PRIMARYSTEREO"    },
    { DSCAPS_PRIMARY8BIT,      "PRIMARY8BIT"      },
    { DSCAPS_PRIMARY16BIT,     "PRIMARY16BIT"     },
    { DSCAPS_CONTINUOUSRATE,   "CONTINUOUSRATE"   },
    { DSCAPS_EMULDRIVER,       "EMULDRIVER (DirectSound is emulating over waveOut!)" },
    { DSCAPS_CERTIFIED,        "CERTIFIED"        },
    { DSCAPS_SECONDARYMONO,    "SECONDARYMONO"    },
    { DSCAPS_SECONDARYSTEREO,  "SECONDARYSTEREO"  },
    { DSCAPS_SECONDARY8BIT,    "SECONDARY8BIT"    },
    { DSCAPS_SECONDARY16BIT,   "SECONDARY16BIT"   },
};

static void DumpCaps(LPDIRECTSOUND pDS)
{
    DSCAPS caps;
    HRESULT hr;
    size_t i;

    ZeroMemory(&caps, sizeof(caps));
    caps.dwSize = sizeof(caps);
    L("    IDirectSound::GetCaps\n");
    hr = IDirectSound_GetCaps(pDS, &caps);
    HR(hr);
    if (FAILED(hr)) return;

    L("      dwFlags                    = %08X\n", caps.dwFlags);
    for (i = 0; i < sizeof(kDsCaps)/sizeof(kDsCaps[0]); i++)
        if (caps.dwFlags & kDsCaps[i].bit)
            L("        + %s\n", kDsCaps[i].name);

    /* The two fields this whole program exists to read. */
    L("      dwMinSecondarySampleRate   = %u   <== derived from our data ranges\n",
      caps.dwMinSecondarySampleRate);
    L("      dwMaxSecondarySampleRate   = %u   <== derived from our data ranges\n",
      caps.dwMaxSecondarySampleRate);

    L("      dwPrimaryBuffers           = %u\n", caps.dwPrimaryBuffers);
    L("      dwMaxHwMixingAllBuffers    = %u\n", caps.dwMaxHwMixingAllBuffers);
    L("      dwMaxHwMixingStaticBuffers = %u\n", caps.dwMaxHwMixingStaticBuffers);
    L("      dwMaxHwMixingStreamingBufs = %u\n", caps.dwMaxHwMixingStreamingBuffers);
    L("      dwFreeHwMixingAllBuffers   = %u\n", caps.dwFreeHwMixingAllBuffers);
    L("      dwMaxHw3DAllBuffers        = %u\n", caps.dwMaxHw3DAllBuffers);
    L("      dwTotalHwMemBytes          = %u\n", caps.dwTotalHwMemBytes);
    L("      dwFreeHwMemBytes           = %u\n", caps.dwFreeHwMemBytes);
    L("      dwUnlockTransferRateHwBufs = %u\n", caps.dwUnlockTransferRateHwBuffers);
    L("      dwPlayCpuOverheadSwBuffers = %u\n", caps.dwPlayCpuOverheadSwBuffers);
}

/* Integer triangle wave - deliberately avoids linking the CRT's math library,
 * which keeps the build to one command. Audibly a buzz rather than a tone;
 * that is fine, the question is whether anything comes out at all. */
static void FillTriangle(void *p, DWORD bytes, DWORD rate, DWORD freq)
{
    short *s = (short *)p;
    DWORD frames = bytes / 4;
    DWORD period = (freq && rate) ? (rate / freq) : 100;
    DWORD i;
    if (period < 4) period = 4;
    for (i = 0; i < frames; i++)
    {
        DWORD ph = i % period;
        LONG v;
        if (ph < period / 2) v = (LONG)((ph * 24000) / (period / 2)) - 12000;
        else                 v = 12000 - (LONG)(((ph - period / 2) * 24000) / (period / 2));
        s[2 * i]     = (short)v;
        s[2 * i + 1] = (short)v;
    }
}

/* Try to create - and if asked, actually play - a secondary buffer at one
 * format. Returns TRUE if creation succeeded. */
static BOOL TrySecondary(LPDIRECTSOUND pDS, DWORD rate, WORD bits, WORD ch,
                         BOOL play, DWORD toneHz)
{
    WAVEFORMATEX wfx;
    DSBUFFERDESC dsbd;
    LPDIRECTSOUNDBUFFER pBuf = NULL;
    HRESULT hr;
    void *p1 = NULL, *p2 = NULL;
    DWORD b1 = 0, b2 = 0;

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = ch;
    wfx.nSamplesPerSec  = rate;
    wfx.wBitsPerSample  = bits;
    wfx.nBlockAlign     = (WORD)(ch * bits / 8);
    wfx.nAvgBytesPerSec = rate * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    ZeroMemory(&dsbd, sizeof(dsbd));
    dsbd.dwSize        = sizeof(dsbd);
    dsbd.dwFlags       = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
    dsbd.dwBufferBytes = wfx.nAvgBytesPerSec;          /* one second */
    dsbd.lpwfxFormat   = &wfx;

    L("    CreateSoundBuffer  %6u Hz  %2u bit  %u ch\n", rate, bits, ch);
    hr = IDirectSound_CreateSoundBuffer(pDS, &dsbd, &pBuf, NULL);
    HR(hr);
    if (FAILED(hr) || !pBuf) return FALSE;

    if (play && bits == 16 && ch == 2)
    {
        hr = IDirectSoundBuffer_Lock(pBuf, 0, dsbd.dwBufferBytes,
                                     &p1, &b1, &p2, &b2, 0);
        L("      Lock\n");
        HR(hr);
        if (SUCCEEDED(hr))
        {
            if (p1) FillTriangle(p1, b1, rate, toneHz);
            if (p2) FillTriangle(p2, b2, rate, toneHz);
            IDirectSoundBuffer_Unlock(pBuf, p1, b1, p2, b2);

            L("      SetVolume(0) + Play  (%u Hz buzz, ~1.2 s)\n", toneHz);
            IDirectSoundBuffer_SetVolume(pBuf, DSBVOLUME_MAX);
            hr = IDirectSoundBuffer_Play(pBuf, 0, 0, DSBPLAY_LOOPING);
            HR(hr);
            if (SUCCEEDED(hr))
            {
                DWORD pp = 0, wp = 0;
                Sleep(1200);
                IDirectSoundBuffer_GetCurrentPosition(pBuf, &pp, &wp);
                L("      after 1.2 s: play cursor = %u, write cursor = %u%s\n",
                  pp, wp,
                  (pp == 0) ? "   <-- cursor never moved, DMA is not running"
                            : "   (cursor advanced, so the stream really ran)");
                IDirectSoundBuffer_Stop(pBuf);
            }
            Sleep(250);
        }
    }

    IDirectSoundBuffer_Release(pBuf);
    return TRUE;
}

static void ProbeOneDevice(LPGUID guid, const char *label)
{
    LPDIRECTSOUND pDS = NULL;
    LPDIRECTSOUNDBUFFER pPrim = NULL;
    DSBUFFERDESC dsbd;
    WAVEFORMATEX wfx;
    HRESULT hr;
    HWND hwnd;
    DWORD spk = 0;
    BOOL isNull = TRUE;
    int i;

    static const DWORD kRates[] = { 48000, 44100, 32000, 22050, 16000, 11025, 8000, 96000 };

    L("======================================================================\n");
    L("  DirectSoundCreate  (%s)\n", label);
    L("======================================================================\n");

    if (guid)
    {
        const unsigned char *b = (const unsigned char *)guid;
        size_t k;
        for (k = 0; k < sizeof(GUID); k++) if (b[k]) { isNull = FALSE; break; }
    }

    SetLastError(0);
    hr = DirectSoundCreate(isNull ? NULL : guid, &pDS, NULL);
    HR(hr);
    if (FAILED(hr) || !pDS)
    {
        DWORD le = GetLastError();
        LPDIRECTSOUND8 pDS8 = NULL;
        HRESULT hr8;

        /* 0x80070057 is HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER). If dsound
         * is just wrapping a failed ioctl, last error is still 87 here. */
        L("  GetLastError after the failure = %lu (0x%08lX)\n",
          (unsigned long)le, (unsigned long)le);
        if (le == ERROR_INVALID_PARAMETER)
            L("    == ERROR_INVALID_PARAMETER - consistent with a wrapped "
              "ioctl failure.\n");
        else if (le == 0)
            L("    == 0 - dsound synthesised this HRESULT, it is not a "
              "wrapped Win32 error.\n");

        /* DS8 is a different init path inside dsound.dll. */
        SetLastError(0);
        hr8 = DirectSoundCreate8(isNull ? NULL : guid, &pDS8, NULL);
        L("  DirectSoundCreate8 (different init path in dsound.dll)\n");
        HR(hr8);
        if (SUCCEEDED(hr8) && pDS8)
        {
            L("    *** DS8 SUCCEEDED where DS failed - the fault is in one "
              "specific init path, not in DirectSound as a whole. ***\n");
            IDirectSound8_Release(pDS8);
        }
        else
        {
            L("    DS8 failed too, last error = %lu\n",
              (unsigned long)GetLastError());
        }

        L("  DirectSoundCreate failed - nothing further can be probed here.\n\n");
        return;
    }

    DumpCaps(pDS);

    /* WMP's renderer sets a cooperative level before doing anything else.
     * DSSCL_PRIORITY is what it uses, because it wants to set the primary
     * buffer format. */
    hwnd = GetConsoleWindow();
    if (!hwnd) hwnd = GetDesktopWindow();
    L("    SetCooperativeLevel(hwnd=%p, DSSCL_PRIORITY)\n", (void *)hwnd);
    hr = IDirectSound_SetCooperativeLevel(pDS, hwnd, DSSCL_PRIORITY);
    HR(hr);

    L("    GetSpeakerConfig\n");
    hr = IDirectSound_GetSpeakerConfig(pDS, &spk);
    HR(hr);
    if (SUCCEEDED(hr))
        L("      config = %08X  (geometry=%u, speakers=%u)\n",
          spk, (ULONG)DSSPEAKER_GEOMETRY(spk), (ULONG)DSSPEAKER_CONFIG(spk));

    /* The primary buffer. This is the object WMP manipulates first, and the
     * one whose SetFormat drives the KSPROPERTY_AUDIO_CHANNEL_CONFIG and
     * STEREO_SPEAKER_GEOMETRY sets we already see arriving in the driver log. */
    ZeroMemory(&dsbd, sizeof(dsbd));
    dsbd.dwSize  = sizeof(dsbd);
    dsbd.dwFlags = DSBCAPS_PRIMARYBUFFER;
    L("    CreateSoundBuffer  PRIMARY\n");
    hr = IDirectSound_CreateSoundBuffer(pDS, &dsbd, &pPrim, NULL);
    HR(hr);

    if (SUCCEEDED(hr) && pPrim)
    {
        DSBCAPS bc;
        ZeroMemory(&bc, sizeof(bc));
        bc.dwSize = sizeof(bc);
        if (SUCCEEDED(IDirectSoundBuffer_GetCaps(pPrim, &bc)))
            L("      primary caps: flags=%08X bytes=%u\n", bc.dwFlags, bc.dwBufferBytes);

        ZeroMemory(&wfx, sizeof(wfx));
        if (SUCCEEDED(IDirectSoundBuffer_GetFormat(pPrim, &wfx, sizeof(wfx), NULL)))
            L("      primary current format: %u Hz, %u bit, %u ch\n",
              wfx.nSamplesPerSec, wfx.wBitsPerSample, wfx.nChannels);

        /* Now set it, both ways round, exactly as a renderer would. */
        for (i = 0; i < 2; i++)
        {
            DWORD r = (i == 0) ? 48000 : 44100;
            ZeroMemory(&wfx, sizeof(wfx));
            wfx.wFormatTag      = WAVE_FORMAT_PCM;
            wfx.nChannels       = 2;
            wfx.nSamplesPerSec  = r;
            wfx.wBitsPerSample  = 16;
            wfx.nBlockAlign     = 4;
            wfx.nAvgBytesPerSec = r * 4;
            L("      SetFormat(primary, %u Hz 16 bit 2 ch)\n", r);
            hr = IDirectSoundBuffer_SetFormat(pPrim, &wfx);
            HR(hr);
        }

        IDirectSoundBuffer_Release(pPrim);
        pPrim = NULL;
    }

    /* Secondary buffers across the standard rate ladder. Creation only - no
     * audio yet - so the pass/fail pattern is visible in one block. */
    L("\n    --- secondary buffer creation, 16 bit stereo, all standard rates ---\n");
    for (i = 0; i < (int)(sizeof(kRates)/sizeof(kRates[0])); i++)
        TrySecondary(pDS, kRates[i], 16, 2, FALSE, 0);

    L("\n    --- secondary buffer creation, other widths at 44100 ---\n");
    TrySecondary(pDS, 44100, 8,  1, FALSE, 0);
    TrySecondary(pDS, 44100, 8,  2, FALSE, 0);
    TrySecondary(pDS, 44100, 16, 1, FALSE, 0);

    /* Finally, actually play. Two rates, two pitches, so the number of
     * distinct buzzes heard identifies which rates really stream. */
    L("\n    --- audible test: 48000 Hz then 44100 Hz ---\n");
    L("    (listen for TWO buzzes, the second lower in pitch than the first)\n");
    TrySecondary(pDS, 48000, 16, 2, TRUE, 440);
    TrySecondary(pDS, 44100, 16, 2, TRUE, 220);

    IDirectSound_Release(pDS);
    L("\n");
}

int __cdecl main(void)
{
    HRESULT hr;
    int i;

    OpenLog();

    L("dstest - DirectSound / waveOut probe for the stwrtxp XPDM driver\n");
    L("Stage 5bj. Reports where DirectSound stops, since the driver log cannot.\n");
#ifdef _WIN64
    L("build: x64 (64-bit).  NOTE: WMP itself is a 32-bit process.\n");
#else
    L("build: x86 (32-bit).  This matches WMP's own bitness.\n");
#endif
    L("log file: %s\n", g_logPath);
    L("======================================================================\n\n");

    hr = CoInitialize(NULL);
    L("CoInitialize\n");
    HR(hr);
    L("\n");

    ProbeWaveOut();

    L("================ DirectSoundEnumerate ================\n");
    g_devCount = 0;
    hr = DirectSoundEnumerateA(DsEnumCb, NULL);
    L("  DirectSoundEnumerate returned\n");
    HR(hr);
    L("  device count = %d\n", g_devCount);
    if (g_devCount == 0)
        L("  *** DirectSound sees NO devices at all. That alone explains WMP. ***\n");
    L("\n");

    /* The default device is what WMP uses unless told otherwise. */
    ProbeOneDevice(NULL, "NULL == default device");

    /* Then every enumerated device that is not the default entry, in case the
     * default resolves somewhere unexpected. */
    for (i = 0; i < g_devCount && i < 8; i++)
    {
        const unsigned char *b = (const unsigned char *)&g_devGuid[i];
        size_t k;
        BOOL isNull = TRUE;
        char label[64];
        for (k = 0; k < sizeof(GUID); k++) if (b[k]) { isNull = FALSE; break; }
        if (isNull) continue;
        _snprintf(label, sizeof(label) - 1, "enumerated device [%d]", i);
        label[sizeof(label) - 1] = 0;
        ProbeOneDevice(&g_devGuid[i], label);
    }

    CoUninitialize();

    L("======================================================================\n");
    L("done.\n");
    L("log written to: %s\n", g_logPath);
    if (g_log) fclose(g_log);
    return 0;
}
