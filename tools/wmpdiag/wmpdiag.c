/*
 * wmpdiag - Stage 5bl probe for the stwrtxp XPDM backport.
 *
 * Mixer, waveOut and DirectSound have all been measured healthy and AUDIBLE.
 * Only Windows Media Player still fails, with "there is a problem with your
 * sound device".  That string is the surfaced form of DirectShow's
 * VFW_E_NO_AUDIO_HARDWARE (0x80040256), so this probe goes at the DirectShow
 * layer and at WMP's own persisted state.
 *
 * Four parts, ordered so a crash in a later one does not cost the earlier ones:
 *   1. CoCreateInstance of the renderer filters   (no hand-declared vtables)
 *   2. WMP's own registry state                   (no COM at all)
 *   3. DirectSound buffers with the DirectShow renderer's REAL flag set,
 *      plus 24/32-bit and WAVEFORMATEXTENSIBLE    (no hand-declared vtables)
 *   4. IGraphBuilder::RenderFile + IMediaControl::Run  (hand-declared vtables)
 *
 * The WDK ships strmiids.lib/quartz.lib for wnet but NOT dshow.h or strmif.h,
 * so every GUID and every vtable in part 4 is declared by hand below.  The
 * slot order is load-bearing: a wrong slot calls a wrong function pointer.
 *
 * Build: see wmpdiag_build.cmd (builds both x64 and x86 - WMP is 32-bit).
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define CINTERFACE
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* logging - path derived from the module, never relative.  See the    */
/* Stage 5bj lesson: a relative fopen() lands wherever CWD happens to  */
/* be and the user cannot find it.                                     */
/* ------------------------------------------------------------------ */

static FILE *g_log = NULL;
static char  g_logPath[MAX_PATH] = "";

static void L(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

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
            _snprintf(g_logPath, sizeof(g_logPath) - 1, "%swmpdiag_log.txt", mod);
            g_logPath[sizeof(g_logPath) - 1] = 0;
            g_log = fopen(g_logPath, "w");
            if (g_log) return;
        }
    }
    if (GetTempPathA(MAX_PATH, mod) > 0) {
        _snprintf(g_logPath, sizeof(g_logPath) - 1, "%swmpdiag_log.txt", mod);
        g_logPath[sizeof(g_logPath) - 1] = 0;
        g_log = fopen(g_logPath, "w");
        if (g_log) return;
    }
    strcpy(g_logPath, "wmpdiag_log.txt");
    g_log = fopen(g_logPath, "w");
}

/* ------------------------------------------------------------------ */
/* HRESULT decoding                                                    */
/* ------------------------------------------------------------------ */

#define VFW_E_INVALIDMEDIATYPE    0x80040200L
#define VFW_E_NO_ACCEPTABLE_TYPES 0x80040207L
#define VFW_E_NOT_CONNECTED       0x80040209L
#define VFW_E_NOT_FOUND           0x80040216L
#define VFW_E_CANNOT_CONNECT      0x80040217L
#define VFW_E_CANNOT_RENDER       0x80040218L
#define VFW_S_PARTIAL_RENDER      0x00040242L
#define VFW_S_DUPLICATE_NAME      0x0004022DL
#define VFW_E_TYPE_NOT_ACCEPTED   0x8004022AL
#define VFW_E_NO_AUDIO_HARDWARE   0x80040256L
#define VFW_S_AUDIO_NOT_RENDERED  0x00040257L
#define VFW_S_VIDEO_NOT_RENDERED  0x00040258L
#define VFW_E_UNSUPPORTED_AUDIO   0x8004025CL
#define VFW_E_UNSUPPORTED_VIDEO   0x8004025DL
#define VFW_E_NOT_IN_GRAPH        0x8004025FL
#define VFW_E_UNSUPPORTED_STREAM  0x80040265L

static const char *HrName(HRESULT hr)
{
    switch ((unsigned long)hr) {
    case 0x00000000: return "S_OK";
    case 0x00000001: return "S_FALSE";
    case 0x80004001: return "E_NOTIMPL / DSERR_UNSUPPORTED";
    case 0x80004002: return "E_NOINTERFACE";
    case 0x80004003: return "E_POINTER";
    case 0x80004005: return "E_FAIL";
    case 0x8007000E: return "E_OUTOFMEMORY";
    case 0x80070057: return "E_INVALIDARG / DSERR_INVALIDPARAM";
    case 0x80040154: return "REGDB_E_CLASSNOTREG  *** the filter is not registered ***";
    case 0x80040155: return "REGDB_E_IIDNOTREG";
    case 0x80080005: return "CO_E_SERVER_EXEC_FAILURE";
    case 0x800401F0: return "CO_E_NOTINITIALIZED";

    case VFW_E_INVALIDMEDIATYPE:    return "VFW_E_INVALIDMEDIATYPE";
    case VFW_E_NO_ACCEPTABLE_TYPES: return "VFW_E_NO_ACCEPTABLE_TYPES";
    case VFW_E_NOT_CONNECTED:       return "VFW_E_NOT_CONNECTED";
    case VFW_E_NOT_FOUND:           return "VFW_E_NOT_FOUND";
    case VFW_E_CANNOT_CONNECT:      return "VFW_E_CANNOT_CONNECT";
    case VFW_E_CANNOT_RENDER:       return "VFW_E_CANNOT_RENDER";
    case VFW_S_PARTIAL_RENDER:      return "VFW_S_PARTIAL_RENDER (some streams unrendered)";
    case VFW_S_DUPLICATE_NAME:      return "VFW_S_DUPLICATE_NAME (benign)";
    case VFW_E_TYPE_NOT_ACCEPTED:   return "VFW_E_TYPE_NOT_ACCEPTED";
    case VFW_E_NO_AUDIO_HARDWARE:
        return "VFW_E_NO_AUDIO_HARDWARE  *** THIS IS WMP'S ERROR ***";
    case VFW_S_AUDIO_NOT_RENDERED:
        return "VFW_S_AUDIO_NOT_RENDERED  *** graph built but AUDIO WAS DROPPED ***";
    case VFW_S_VIDEO_NOT_RENDERED:  return "VFW_S_VIDEO_NOT_RENDERED (expected, no video)";
    case VFW_E_UNSUPPORTED_AUDIO:   return "VFW_E_UNSUPPORTED_AUDIO";
    case VFW_E_UNSUPPORTED_VIDEO:   return "VFW_E_UNSUPPORTED_VIDEO";
    case VFW_E_NOT_IN_GRAPH:        return "VFW_E_NOT_IN_GRAPH";
    case VFW_E_UNSUPPORTED_STREAM:  return "VFW_E_UNSUPPORTED_STREAM";

    /* MAKE_DSHRESULT(n) == 0x88780000 | n.  E_INVALIDARG doubles as
       DSERR_INVALIDPARAM and E_NOTIMPL as DSERR_UNSUPPORTED, so those two
       are folded into the generic cases above rather than duplicated here
       (a duplicate case label is a hard compile error). */
    case 0x8878000A: return "DSERR_ALLOCATED  *** the pin instance is already taken ***";
    case 0x8878001E: return "DSERR_CONTROLUNAVAIL  *** a CTRL* flag was refused ***";
    case 0x88780032: return "DSERR_INVALIDCALL";
    case 0x88780046: return "DSERR_PRIOLEVELNEEDED";
    case 0x88780064: return "DSERR_BADFORMAT  *** the format was refused ***";
    case 0x88780078: return "DSERR_NODRIVER";
    case 0x88780082: return "DSERR_ALREADYINITIALIZED";
    case 0x88780096: return "DSERR_BUFFERLOST";
    case 0x887800A0: return "DSERR_OTHERAPPHASPRIO";
    case 0x887800AA: return "DSERR_UNINITIALIZED";
    case 0x80040110: return "CLASS_E_NOAGGREGATION";
    default: return "(unrecognised)";
    }
}

#define HR(x) L("        -> %08lX  %s\n", (unsigned long)(x), HrName(x))

/* ------------------------------------------------------------------ */
/* GUIDs - hand-declared, since dshow.h/strmif.h are absent from the   */
/* WDK.  Values transcribed from the DirectShow documentation.         */
/* ------------------------------------------------------------------ */

static const GUID cCLSID_FilterGraph =
    {0xE436EBB3,0x524F,0x11CE,{0x9F,0x53,0x00,0x20,0xAF,0x0B,0xA7,0x70}};
static const GUID cCLSID_DSoundRender =
    {0x79376820,0x07D0,0x11CF,{0xA2,0x4D,0x00,0x20,0xAF,0xD7,0x97,0x67}};
static const GUID cCLSID_AudioRender =
    {0xCD8743A1,0x3736,0x11D0,{0x9E,0x69,0x00,0xC0,0x4F,0xD7,0xC1,0x5B}};

static const GUID cIID_IBaseFilter =
    {0x56A86895,0x0AD4,0x11CE,{0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};
static const GUID cIID_IGraphBuilder =
    {0x56A868A9,0x0AD4,0x11CE,{0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};
static const GUID cIID_IMediaControl =
    {0x56A868B1,0x0AD4,0x11CE,{0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};
static const GUID cIID_IBasicAudio =
    {0x56A868B3,0x0AD4,0x11CE,{0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* KSDATAFORMAT_SUBTYPE_PCM, for WAVEFORMATEXTENSIBLE */
static const GUID cSUBTYPE_PCM =
    {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

/* ------------------------------------------------------------------ */
/* Part 1 - can the renderer filters even be created?                  */
/* ------------------------------------------------------------------ */

static void TryFilter(const GUID *clsid, const char *name)
{
    IUnknown *pUnk = NULL;
    void     *pBF  = NULL;
    HRESULT   hr;

    L("  CoCreateInstance(%s)\n", name);
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUnknown, (void **)&pUnk);
    HR(hr);
    if (FAILED(hr) || !pUnk) {
        L("    *** could not create %s ***\n", name);
        return;
    }

    L("    QueryInterface(IID_IBaseFilter)\n");
    hr = pUnk->lpVtbl->QueryInterface(pUnk, &cIID_IBaseFilter, &pBF);
    HR(hr);
    if (SUCCEEDED(hr) && pBF)
        ((IUnknown *)pBF)->lpVtbl->Release((IUnknown *)pBF);

    pUnk->lpVtbl->Release(pUnk);
}

static void ProbeFilters(void)
{
    L("\n============ part 1: DirectShow renderer filters ==================\n");
    L("(no hand-declared vtables here - only IUnknown, so this part is safe)\n\n");
    TryFilter(&cCLSID_DSoundRender, "CLSID_DSoundRender  (WMP's audio renderer)");
    L("\n");
    TryFilter(&cCLSID_AudioRender,  "CLSID_AudioRender   (the waveOut renderer)");
    L("\n");
    TryFilter(&cCLSID_FilterGraph,  "CLSID_FilterGraph");
}

/* ------------------------------------------------------------------ */
/* Part 2 - WMP's own persisted state                                  */
/* ------------------------------------------------------------------ */

static void DumpValues(HKEY hKey, const char *indent)
{
    char  name[512];
    BYTE  data[1024];
    DWORD i, cbName, cbData, type;
    LONG  rc;

    for (i = 0; ; i++) {
        cbName = sizeof(name);
        cbData = sizeof(data);
        rc = RegEnumValueA(hKey, i, name, &cbName, NULL, &type, data, &cbData);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) {
            L("%s  (RegEnumValue -> %ld)\n", indent, rc);
            break;
        }
        if (cbName == 0) strcpy(name, "(default)");

        switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ:
            data[sizeof(data) - 1] = 0;
            L("%s  %-34s = \"%s\"\n", indent, name, (char *)data);
            break;
        case REG_DWORD:
            L("%s  %-34s = %lu (0x%08lX)\n", indent, name,
              *(DWORD *)data, *(DWORD *)data);
            break;
        default:
            L("%s  %-34s = <type %lu, %lu bytes>\n", indent, name,
              type, cbData);
            break;
        }
    }
}

static void DumpKey(HKEY root, const char *rootName, const char *path, REGSAM extra)
{
    HKEY  hKey = NULL;
    LONG  rc;
    char  sub[512];
    DWORD i, cb;

    L("  [%s\\%s]%s\n", rootName, path,
      (extra == KEY_WOW64_32KEY) ? "   (32-bit view - WMP is a 32-bit app)" : "");

    rc = RegOpenKeyExA(root, path, 0, KEY_READ | extra, &hKey);
    if (rc != ERROR_SUCCESS) {
        L("    (RegOpenKeyEx -> %ld  %s)\n", rc,
          (rc == ERROR_FILE_NOT_FOUND) ? "key does not exist" : "");
        return;
    }

    DumpValues(hKey, "  ");

    for (i = 0; ; i++) {
        cb = sizeof(sub);
        if (RegEnumKeyExA(hKey, i, sub, &cb, NULL, NULL, NULL, NULL)
            != ERROR_SUCCESS) break;
        L("    <subkey> %s\n", sub);
    }

    RegCloseKey(hKey);
}

static void ProbeWmpState(void)
{
    L("\n============ part 2: WMP's own persisted state ====================\n");
    L("A stale audio-device selection in WMP's own preferences would produce\n");
    L("exactly this symptom while every system API stayed healthy.\n\n");

    DumpKey(HKEY_CURRENT_USER, "HKCU",
            "Software\\Microsoft\\MediaPlayer\\Preferences", 0);
    L("\n");
    DumpKey(HKEY_CURRENT_USER, "HKCU",
            "Software\\Microsoft\\MediaPlayer\\Preferences\\VideoSettings", 0);
    L("\n");
    DumpKey(HKEY_LOCAL_MACHINE, "HKLM",
            "SOFTWARE\\Microsoft\\MediaPlayer", 0);
    L("\n");
    DumpKey(HKEY_LOCAL_MACHINE, "HKLM",
            "SOFTWARE\\Microsoft\\MediaPlayer", KEY_WOW64_32KEY);
    L("\n");
    DumpKey(HKEY_LOCAL_MACHINE, "HKLM",
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Setup\\WindowsMediaPlayer", 0);
    L("\n");
    L("  -- the system's preferred wave device, as multimedia sees it --\n");
    DumpKey(HKEY_CURRENT_USER, "HKCU", "Software\\Microsoft\\Multimedia\\Sound Mapper", 0);
    L("\n");
    DumpKey(HKEY_LOCAL_MACHINE, "HKLM",
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", 0);
}

/* ------------------------------------------------------------------ */
/* Part 3 - DirectSound with the DirectShow renderer's REAL flag set   */
/* ------------------------------------------------------------------ */

/* Declared by hand rather than relying on mmreg.h's guard. */
typedef struct {
    WAVEFORMATEX Format;
    union { WORD wValidBitsPerSample; WORD wSamplesPerBlock; WORD wReserved; } Samples;
    DWORD dwChannelMask;
    GUID  SubFormat;
} MYWFXEXT;

/* mmsystem.h does not pull in mmreg.h in the WDK, so define it here. */
#ifndef WAVE_FORMAT_EXTENSIBLE
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#endif

#ifndef DSBCAPS_STICKYFOCUS
#define DSBCAPS_STICKYFOCUS 0x00004000
#endif

/* The flag set the DirectShow DirectSound Renderer actually uses. */
#define DSHOW_RENDERER_FLAGS  (DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN |         \
                               DSBCAPS_CTRLFREQUENCY | DSBCAPS_GLOBALFOCUS |  \
                               DSBCAPS_STICKYFOCUS | DSBCAPS_GETCURRENTPOSITION2)

static LPDIRECTSOUNDBUFFER MakeBuf(LPDIRECTSOUND pDS, WAVEFORMATEX *wfx,
                                   DWORD flags, const char *what)
{
    DSBUFFERDESC        d;
    LPDIRECTSOUNDBUFFER pBuf = NULL;
    HRESULT             hr;

    ZeroMemory(&d, sizeof(d));
    d.dwSize        = sizeof(DSBUFFERDESC);
    d.dwFlags       = flags;
    d.dwBufferBytes = wfx->nAvgBytesPerSec;
    d.lpwfxFormat   = wfx;

    L("    %-58s flags=%08lX\n", what, (unsigned long)flags);
    hr = IDirectSound_CreateSoundBuffer(pDS, &d, &pBuf, NULL);
    HR(hr);
    return SUCCEEDED(hr) ? pBuf : NULL;
}

static void FillWfx(WAVEFORMATEX *w, DWORD rate, WORD bits, WORD ch)
{
    ZeroMemory(w, sizeof(*w));
    w->wFormatTag      = WAVE_FORMAT_PCM;
    w->nChannels       = ch;
    w->nSamplesPerSec  = rate;
    w->wBitsPerSample  = bits;
    w->nBlockAlign     = (WORD)(ch * bits / 8);
    w->nAvgBytesPerSec = rate * w->nBlockAlign;
    w->cbSize          = 0;
}

static void ProbeDsRendererShaped(void)
{
    LPDIRECTSOUND       pDS = NULL;
    LPDIRECTSOUNDBUFFER pBuf;
    WAVEFORMATEX        wfx;
    MYWFXEXT            ext;
    HRESULT             hr;
    LONG                v;
    DWORD               freq;
    int                 i;

    static const struct { DWORD f; const char *n; } kOne[] = {
        { DSBCAPS_CTRLVOLUME,            "CTRLVOLUME alone" },
        { DSBCAPS_CTRLPAN,               "CTRLPAN alone" },
        { DSBCAPS_CTRLFREQUENCY,         "CTRLFREQUENCY alone" },
        { DSBCAPS_CTRLPOSITIONNOTIFY,    "CTRLPOSITIONNOTIFY alone" },
        { DSBCAPS_STICKYFOCUS,           "STICKYFOCUS alone" },
        { DSBCAPS_GETCURRENTPOSITION2,   "GETCURRENTPOSITION2 alone (the 5bj baseline)" }
    };

    L("\n============ part 3: DirectSound, renderer-shaped ==================\n");
    L("Stage 5bj created every buffer with only GETCURRENTPOSITION2|GLOBALFOCUS\n");
    L("and only at 8 and 16 bit.  This part closes both gaps.\n\n");

    hr = DirectSoundCreate(NULL, &pDS, NULL);
    L("  DirectSoundCreate(NULL)\n");
    HR(hr);
    if (FAILED(hr) || !pDS) return;

    hr = IDirectSound_SetCooperativeLevel(pDS, GetDesktopWindow(), DSSCL_PRIORITY);
    L("  SetCooperativeLevel(DSSCL_PRIORITY)\n");
    HR(hr);

    L("\n  -- the full DirectShow renderer flag set --\n");
    FillWfx(&wfx, 48000, 16, 2);
    pBuf = MakeBuf(pDS, &wfx, DSHOW_RENDERER_FLAGS, "48000 Hz 16 bit 2 ch, renderer flags");
    if (pBuf) {
        L("      -- controls on that buffer, which is what the renderer drives --\n");
        hr = IDirectSoundBuffer_SetVolume(pBuf, -1000);
        L("      SetVolume(-1000)\n"); HR(hr);
        hr = IDirectSoundBuffer_GetVolume(pBuf, &v);
        L("      GetVolume -> %ld\n", v); HR(hr);
        hr = IDirectSoundBuffer_SetPan(pBuf, -2000);
        L("      SetPan(-2000)\n"); HR(hr);
        hr = IDirectSoundBuffer_SetFrequency(pBuf, 44100);
        L("      SetFrequency(44100)\n"); HR(hr);
        hr = IDirectSoundBuffer_GetFrequency(pBuf, &freq);
        L("      GetFrequency -> %lu\n", (unsigned long)freq); HR(hr);
        hr = IDirectSoundBuffer_SetFrequency(pBuf, 96000);
        L("      SetFrequency(96000)\n"); HR(hr);
        hr = IDirectSoundBuffer_SetVolume(pBuf, 0);
        L("      SetVolume(0) restore\n"); HR(hr);
        IDirectSoundBuffer_Release(pBuf);
    }

    FillWfx(&wfx, 44100, 16, 2);
    pBuf = MakeBuf(pDS, &wfx, DSHOW_RENDERER_FLAGS, "44100 Hz 16 bit 2 ch, renderer flags");
    if (pBuf) IDirectSoundBuffer_Release(pBuf);

    L("\n  -- each control flag on its own, to isolate a refusal --\n");
    for (i = 0; i < (int)(sizeof(kOne) / sizeof(kOne[0])); i++) {
        FillWfx(&wfx, 48000, 16, 2);
        pBuf = MakeBuf(pDS, &wfx, kOne[i].f | DSBCAPS_GLOBALFOCUS, kOne[i].n);
        if (pBuf) IDirectSoundBuffer_Release(pBuf);
    }

    L("\n  -- bit depths our pin does NOT publish (it publishes 16..16 only) --\n");
    L("  WMP 11 Options > Devices > Speakers > Properties has \"Use 24-bit audio\".\n");
    FillWfx(&wfx, 48000, 24, 2);
    pBuf = MakeBuf(pDS, &wfx, DSHOW_RENDERER_FLAGS, "48000 Hz 24 bit 2 ch, renderer flags");
    if (pBuf) IDirectSoundBuffer_Release(pBuf);
    FillWfx(&wfx, 44100, 24, 2);
    pBuf = MakeBuf(pDS, &wfx, DSHOW_RENDERER_FLAGS, "44100 Hz 24 bit 2 ch, renderer flags");
    if (pBuf) IDirectSoundBuffer_Release(pBuf);
    FillWfx(&wfx, 48000, 32, 2);
    pBuf = MakeBuf(pDS, &wfx, DSHOW_RENDERER_FLAGS, "48000 Hz 32 bit 2 ch, renderer flags");
    if (pBuf) IDirectSoundBuffer_Release(pBuf);

    L("\n  -- WAVEFORMATEXTENSIBLE, a distinct code path from WAVE_FORMAT_PCM --\n");
    ZeroMemory(&ext, sizeof(ext));
    ext.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    ext.Format.nChannels       = 2;
    ext.Format.nSamplesPerSec  = 48000;
    ext.Format.wBitsPerSample  = 16;
    ext.Format.nBlockAlign     = 4;
    ext.Format.nAvgBytesPerSec = 48000 * 4;
    ext.Format.cbSize          = 22;
    ext.Samples.wValidBitsPerSample = 16;
    ext.dwChannelMask          = 0x3;   /* FRONT_LEFT | FRONT_RIGHT */
    ext.SubFormat              = cSUBTYPE_PCM;
    pBuf = MakeBuf(pDS, (WAVEFORMATEX *)&ext, DSHOW_RENDERER_FLAGS,
                   "48000 Hz 16 bit 2 ch EXTENSIBLE, renderer flags");
    if (pBuf) IDirectSoundBuffer_Release(pBuf);

    L("\n  -- a second simultaneous buffer (our pin's instance count is 1) --\n");
    FillWfx(&wfx, 48000, 16, 2);
    {
        LPDIRECTSOUNDBUFFER a, b;
        a = MakeBuf(pDS, &wfx, DSHOW_RENDERER_FLAGS, "buffer A");
        b = MakeBuf(pDS, &wfx, DSHOW_RENDERER_FLAGS, "buffer B while A is open");
        if (b) IDirectSoundBuffer_Release(b);
        if (a) IDirectSoundBuffer_Release(a);
    }

    IDirectSound_Release(pDS);
}

/* ------------------------------------------------------------------ */
/* Part 4 - the graph.  Hand-declared vtables; slot order is           */
/* load-bearing.  IGraphBuilder = IUnknown(0-2) + IFilterGraph(3-10) + */
/* IGraphBuilder(11-17), so RenderFile is slot 13.                     */
/* ------------------------------------------------------------------ */

typedef struct IGraphBuilderX IGraphBuilderX;
typedef struct IGraphBuilderXVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IGraphBuilderX *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IGraphBuilderX *);
    ULONG   (STDMETHODCALLTYPE *Release)(IGraphBuilderX *);
    /* IFilterGraph */
    HRESULT (STDMETHODCALLTYPE *AddFilter)(IGraphBuilderX *, void *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *RemoveFilter)(IGraphBuilderX *, void *);
    HRESULT (STDMETHODCALLTYPE *EnumFilters)(IGraphBuilderX *, void **);
    HRESULT (STDMETHODCALLTYPE *FindFilterByName)(IGraphBuilderX *, LPCWSTR, void **);
    HRESULT (STDMETHODCALLTYPE *ConnectDirect)(IGraphBuilderX *, void *, void *, const void *);
    HRESULT (STDMETHODCALLTYPE *Reconnect)(IGraphBuilderX *, void *);
    HRESULT (STDMETHODCALLTYPE *Disconnect)(IGraphBuilderX *, void *);
    HRESULT (STDMETHODCALLTYPE *SetDefaultSyncSource)(IGraphBuilderX *);
    /* IGraphBuilder */
    HRESULT (STDMETHODCALLTYPE *Connect)(IGraphBuilderX *, void *, void *);
    HRESULT (STDMETHODCALLTYPE *Render)(IGraphBuilderX *, void *);
    HRESULT (STDMETHODCALLTYPE *RenderFile)(IGraphBuilderX *, LPCWSTR, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *AddSourceFilter)(IGraphBuilderX *, LPCWSTR, LPCWSTR, void **);
    HRESULT (STDMETHODCALLTYPE *SetLogFile)(IGraphBuilderX *, DWORD_PTR);
    HRESULT (STDMETHODCALLTYPE *Abort)(IGraphBuilderX *);
    HRESULT (STDMETHODCALLTYPE *ShouldOperationContinue)(IGraphBuilderX *);
} IGraphBuilderXVtbl;
struct IGraphBuilderX { IGraphBuilderXVtbl *lpVtbl; };

/* IMediaControl = IDispatch(0-6) + Run 7, Pause 8, Stop 9, GetState 10 */
typedef struct IMediaControlX IMediaControlX;
typedef struct IMediaControlXVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IMediaControlX *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IMediaControlX *);
    ULONG   (STDMETHODCALLTYPE *Release)(IMediaControlX *);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfoCount)(IMediaControlX *, UINT *);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfo)(IMediaControlX *, UINT, LCID, void **);
    HRESULT (STDMETHODCALLTYPE *GetIDsOfNames)(IMediaControlX *, const GUID *, LPOLESTR *,
                                               UINT, LCID, DISPID *);
    HRESULT (STDMETHODCALLTYPE *Invoke)(IMediaControlX *, DISPID, const GUID *, LCID,
                                        WORD, void *, VARIANT *, void *, UINT *);
    HRESULT (STDMETHODCALLTYPE *Run)(IMediaControlX *);
    HRESULT (STDMETHODCALLTYPE *Pause)(IMediaControlX *);
    HRESULT (STDMETHODCALLTYPE *Stop)(IMediaControlX *);
    HRESULT (STDMETHODCALLTYPE *GetState)(IMediaControlX *, LONG, LONG *);
} IMediaControlXVtbl;
struct IMediaControlX { IMediaControlXVtbl *lpVtbl; };

/* IBasicAudio = IDispatch(0-6) + put_Volume 7, get_Volume 8, put_Balance 9, get_Balance 10 */
typedef struct IBasicAudioX IBasicAudioX;
typedef struct IBasicAudioXVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IBasicAudioX *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IBasicAudioX *);
    ULONG   (STDMETHODCALLTYPE *Release)(IBasicAudioX *);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfoCount)(IBasicAudioX *, UINT *);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfo)(IBasicAudioX *, UINT, LCID, void **);
    HRESULT (STDMETHODCALLTYPE *GetIDsOfNames)(IBasicAudioX *, const GUID *, LPOLESTR *,
                                               UINT, LCID, DISPID *);
    HRESULT (STDMETHODCALLTYPE *Invoke)(IBasicAudioX *, DISPID, const GUID *, LCID,
                                        WORD, void *, VARIANT *, void *, UINT *);
    HRESULT (STDMETHODCALLTYPE *put_Volume)(IBasicAudioX *, LONG);
    HRESULT (STDMETHODCALLTYPE *get_Volume)(IBasicAudioX *, LONG *);
    HRESULT (STDMETHODCALLTYPE *put_Balance)(IBasicAudioX *, LONG);
    HRESULT (STDMETHODCALLTYPE *get_Balance)(IBasicAudioX *, LONG *);
} IBasicAudioXVtbl;
struct IBasicAudioX { IBasicAudioXVtbl *lpVtbl; };

static const char *PickMediaFile(char *buf, size_t cb)
{
    static const char *kCand[] = {
        "\\Media\\Windows XP Startup.wav",
        "\\Media\\ding.wav",
        "\\Media\\notify.wav",
        "\\Media\\chimes.wav",
        "\\Media\\tada.wav"
    };
    char win[MAX_PATH];
    UINT n;
    int  i;

    n = GetWindowsDirectoryA(win, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return NULL;

    for (i = 0; i < (int)(sizeof(kCand) / sizeof(kCand[0])); i++) {
        _snprintf(buf, cb - 1, "%s%s", win, kCand[i]);
        buf[cb - 1] = 0;
        if (GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES)
            return buf;
    }
    return NULL;
}

static void ProbeGraph(const char *path)
{
    IGraphBuilderX *pG  = NULL;
    IMediaControlX *pMC = NULL;
    IBasicAudioX   *pBA = NULL;
    WCHAR           wpath[MAX_PATH];
    HRESULT         hr;
    LONG            state = 0, vol = 0;

    L("\n============ part 4: the DirectShow graph ==========================\n");
    L("This is what WMP actually does.  RenderFile returning\n");
    L("VFW_E_NO_AUDIO_HARDWARE (80040256) is WMP's exact error.\n\n");
    L("  media file: %s\n\n", path);

    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);

    L("  CoCreateInstance(CLSID_FilterGraph, IID_IGraphBuilder)\n");
    hr = CoCreateInstance(&cCLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
                          &cIID_IGraphBuilder, (void **)&pG);
    HR(hr);
    if (FAILED(hr) || !pG) return;

    L("  IGraphBuilder::RenderFile   <-- the decisive call\n");
    hr = pG->lpVtbl->RenderFile(pG, wpath, NULL);
    HR(hr);

    if (hr == VFW_E_NO_AUDIO_HARDWARE) {
        L("\n  *** FOUND IT.  The graph cannot build an audio renderer.       ***\n");
        L("  *** This is precisely the error WMP reports as \"there is a      ***\n");
        L("  *** problem with your sound device\".  The failure is in the     ***\n");
        L("  *** DirectShow layer, not in the driver, mixer, waveOut or      ***\n");
        L("  *** DirectSound - all four of which measured healthy.           ***\n\n");
    } else if (hr == VFW_S_AUDIO_NOT_RENDERED) {
        L("\n  *** The graph built but DROPPED THE AUDIO STREAM.              ***\n");
        L("  *** Same conclusion: the audio renderer could not connect.      ***\n\n");
    } else if (SUCCEEDED(hr)) {
        L("\n  The graph built successfully.\n\n");
    }

    L("  QueryInterface(IID_IBasicAudio)  (present only if audio got rendered)\n");
    hr = pG->lpVtbl->QueryInterface(pG, &cIID_IBasicAudio, (void **)&pBA);
    HR(hr);
    if (SUCCEEDED(hr) && pBA) {
        hr = pBA->lpVtbl->get_Volume(pBA, &vol);
        L("    get_Volume -> %ld  (0 = full, -10000 = silence)\n", vol);
        HR(hr);
        pBA->lpVtbl->put_Volume(pBA, 0);
        pBA->lpVtbl->Release(pBA);
    }

    L("\n  QueryInterface(IID_IMediaControl)\n");
    hr = pG->lpVtbl->QueryInterface(pG, &cIID_IMediaControl, (void **)&pMC);
    HR(hr);
    if (SUCCEEDED(hr) && pMC) {
        L("  IMediaControl::Run   (LISTEN - a system sound should play)\n");
        hr = pMC->lpVtbl->Run(pMC);
        HR(hr);

        Sleep(2500);

        hr = pMC->lpVtbl->GetState(pMC, 1000, &state);
        L("  IMediaControl::GetState -> state %ld  (0=Stopped 1=Paused 2=Running)\n", state);
        HR(hr);

        hr = pMC->lpVtbl->Stop(pMC);
        L("  IMediaControl::Stop\n");
        HR(hr);
        pMC->lpVtbl->Release(pMC);
    }

    pG->lpVtbl->Release(pG);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    char    path[MAX_PATH];
    const char *media;
    HRESULT hr;

    OpenLog();

    L("wmpdiag - Stage 5bl DirectShow / WMP probe for stwrtxp\n");
    L("Mixer, waveOut and DirectSound are all measured healthy AND audible.\n");
    L("WMP alone fails.  This probe goes at the only layer left.\n");
#ifdef _WIN64
    L("build: x64 (64-bit).  NOTE: WMP itself is a 32-bit process.\n");
#else
    L("build: x86 (32-bit).  This matches WMP's own bitness.\n");
#endif
    L("log file: %s\n", g_logPath);
    L("====================================================================\n\n");

    hr = CoInitialize(NULL);
    L("CoInitialize -> %08lX  %s\n", (unsigned long)hr, HrName(hr));

    ProbeFilters();
    ProbeWmpState();
    ProbeDsRendererShaped();

    if (argc > 1) {
        strncpy(path, argv[1], MAX_PATH - 1);
        path[MAX_PATH - 1] = 0;
        media = path;
    } else {
        media = PickMediaFile(path, sizeof(path));
    }

    if (media)
        ProbeGraph(media);
    else
        L("\n============ part 4: SKIPPED ======================================\n"
          "  No media file found under %%WINDIR%%\\Media and none given on the\n"
          "  command line.  Re-run as:  wmpdiag.exe \"C:\\path\\to\\file.wav\"\n");

    CoUninitialize();

    L("\n====================================================================\n");
    L("done.\n");
    L("log written to: %s\n", g_logPath);

    if (g_log) fclose(g_log);
    return 0;
}
