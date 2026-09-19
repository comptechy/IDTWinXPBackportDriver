/*
 * audiodiag.c - mixer / waveOut / DirectSound diagnostic probe for XP x64.
 *
 * Stage 5bk. Supersedes dstest.c.
 *
 * dstest proved DirectSound is completely healthy on this driver: every call
 * returned DS_OK, CreateSoundBuffer succeeded at every rate from 8000 to
 * 96000, and the play cursor advanced. So the DSERR_BADFORMAT theory is dead
 * and DirectSound is not where Windows Media Player stops.
 *
 * Two things in that output redirect attention one layer sideways, to the
 * mixer API:
 *
 *   1. waveOutGetDevCaps reported channels=65535 (0xFFFF) and pid=0xFFFF,
 *      while the pin advertises MaximumChannels = 2. Whether 0xFFFF is
 *      wdmaud's "unknown" filler or a real misread, it is worth pinning down.
 *
 *   2. WMP's very first act in the driver log is mixer-line setup - MUTE,
 *      node NAME, VOLUME on two nodes, three ENABLE_EVENTs - and this project
 *      has a known, unexplained mixer-layer anomaly (item n42): the Windows
 *      volume slider traverses only about 0.54 dB of the range the driver
 *      reports. The driver's own numbers are sane - BASICSUPPORT range
 *      -6242304..0 step 49152 is 0 dB to -95.25 dB in 0.75 dB steps, since KS
 *      volume units are 1/65536 dB - so something ABOVE the driver is
 *      misreading them. That was filed as cosmetic on assumption, not on
 *      evidence.
 *
 * WMP's error names the sound *device*, and the mixer is the first thing it
 * touches. So this program dumps the entire mixer topology as the mixer API
 * presents it - every destination, every source, every control, every control
 * value and every range - and then exercises waveOut directly (the path VLC
 * uses and which works) as a control. The DirectSound section is kept but
 * trimmed, since its answer is already known.
 *
 * This is a measurement, not a claim. It may well show a perfectly healthy
 * mixer, in which case the mixer is eliminated the same way DirectSound just
 * was.
 *
 * Fixes the one real bug in dstest.c: the log file was opened by relative
 * name, so it landed in whatever the working directory happened to be and the
 * user could not find it. The log path is now derived from the executable's
 * own module path, with a temp-directory fallback, and is printed both at the
 * start and at the end of the run.
 *
 * Build: audiodiag_build.cmd. WNET/amd64, static CRT, subsystem 5.02.
 */

#define _WIN32_WINNT 0x0502
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>

static FILE *g_log = NULL;
static char  g_logPath[MAX_PATH * 2] = {0};

static void L(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    fputs(buf, stdout);
    fflush(stdout);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
}

/* Open the log beside the .exe, not beside the working directory.
 *
 * Stage 5bq: the log is named after the EXE, not hardcoded, because the
 * 32-bit and 64-bit builds now live in the same folder and a fixed name
 * meant the second run silently destroyed the first one's evidence. So
 * audiodiag.exe writes audiodiag_log.txt and audiodiag32.exe writes
 * audiodiag32_log.txt, and the two can be run in either order. */
static void OpenLog(void)
{
    char mod[MAX_PATH];
    char stem[MAX_PATH];
    DWORD n;
    char *p;
    char *dot;

    stem[0] = 0;

    n = GetModuleFileNameA(NULL, mod, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        mod[MAX_PATH - 1] = 0;
        p = strrchr(mod, '\\');
        if (p)
        {
            /* Remember the bare exe name without its extension. */
            strncpy(stem, p + 1, MAX_PATH - 1);
            stem[MAX_PATH - 1] = 0;
            dot = strrchr(stem, '.');
            if (dot) *dot = 0;
            if (stem[0] == 0) strcpy(stem, "audiodiag");

            *(p + 1) = 0;
            _snprintf(g_logPath, sizeof(g_logPath) - 1, "%s%s_log.txt", mod, stem);
            g_logPath[sizeof(g_logPath) - 1] = 0;
            g_log = fopen(g_logPath, "w");
            if (g_log) return;
        }
    }

    if (stem[0] == 0) strcpy(stem, "audiodiag");

    /* Fallback: the temp directory, which is always writable. */
    if (GetTempPathA(MAX_PATH, mod) > 0)
    {
        _snprintf(g_logPath, sizeof(g_logPath) - 1, "%s%s_log.txt", mod, stem);
        g_logPath[sizeof(g_logPath) - 1] = 0;
        g_log = fopen(g_logPath, "w");
        if (g_log) return;
    }

    /* Last resort: relative, same as the old behaviour. */
    _snprintf(g_logPath, sizeof(g_logPath) - 1,
              "%s_log.txt (relative to working directory)", stem);
    g_logPath[sizeof(g_logPath) - 1] = 0;
    {
        char rel[MAX_PATH];
        _snprintf(rel, sizeof(rel) - 1, "%s_log.txt", stem);
        rel[sizeof(rel) - 1] = 0;
        g_log = fopen(rel, "w");
    }
}

/* ------------------------------------------------------------ MMRESULT ---- */
static const char *MmName(MMRESULT mr)
{
    switch (mr)
    {
    case MMSYSERR_NOERROR:      return "MMSYSERR_NOERROR";
    case MMSYSERR_ERROR:        return "MMSYSERR_ERROR";
    case MMSYSERR_BADDEVICEID:  return "MMSYSERR_BADDEVICEID";
    case MMSYSERR_NOTENABLED:   return "MMSYSERR_NOTENABLED";
    case MMSYSERR_ALLOCATED:    return "MMSYSERR_ALLOCATED  <-- device already in use";
    case MMSYSERR_INVALHANDLE:  return "MMSYSERR_INVALHANDLE";
    case MMSYSERR_NODRIVER:     return "MMSYSERR_NODRIVER";
    case MMSYSERR_NOMEM:        return "MMSYSERR_NOMEM";
    case MMSYSERR_NOTSUPPORTED: return "MMSYSERR_NOTSUPPORTED";
    case MMSYSERR_BADERRNUM:    return "MMSYSERR_BADERRNUM";
    case MMSYSERR_INVALFLAG:    return "MMSYSERR_INVALFLAG";
    case MMSYSERR_INVALPARAM:   return "MMSYSERR_INVALPARAM";
    case MMSYSERR_HANDLEBUSY:   return "MMSYSERR_HANDLEBUSY";
    case MMSYSERR_INVALIDALIAS: return "MMSYSERR_INVALIDALIAS";
    case MMSYSERR_BADDB:        return "MMSYSERR_BADDB";
    case MMSYSERR_KEYNOTFOUND:  return "MMSYSERR_KEYNOTFOUND";
    case MMSYSERR_READERROR:    return "MMSYSERR_READERROR";
    case MMSYSERR_WRITEERROR:   return "MMSYSERR_WRITEERROR";
    case MMSYSERR_DELETEERROR:  return "MMSYSERR_DELETEERROR";
    case MMSYSERR_VALNOTFOUND:  return "MMSYSERR_VALNOTFOUND";
    case MMSYSERR_NODRIVERCB:   return "MMSYSERR_NODRIVERCB";
    case WAVERR_BADFORMAT:      return "WAVERR_BADFORMAT  <-- format refused";
    case WAVERR_STILLPLAYING:   return "WAVERR_STILLPLAYING";
    case WAVERR_UNPREPARED:     return "WAVERR_UNPREPARED";
    case WAVERR_SYNC:           return "WAVERR_SYNC";
    case MIXERR_INVALLINE:      return "MIXERR_INVALLINE  <-- no such mixer line";
    case MIXERR_INVALCONTROL:   return "MIXERR_INVALCONTROL";
    case MIXERR_INVALVALUE:     return "MIXERR_INVALVALUE";
    default:                    return "(unrecognised)";
    }
}

#define MM(x) L("        -> %u  %s\n", (unsigned)(x), MmName(x))

/* --------------------------------------------------- mixer type decoding -- */
static const char *ComponentName(DWORD t)
{
    switch (t)
    {
    case MIXERLINE_COMPONENTTYPE_DST_UNDEFINED:   return "DST_UNDEFINED";
    case MIXERLINE_COMPONENTTYPE_DST_DIGITAL:     return "DST_DIGITAL";
    case MIXERLINE_COMPONENTTYPE_DST_LINE:        return "DST_LINE";
    case MIXERLINE_COMPONENTTYPE_DST_MONITOR:     return "DST_MONITOR";
    case MIXERLINE_COMPONENTTYPE_DST_SPEAKERS:    return "DST_SPEAKERS  <-- what a player looks for";
    case MIXERLINE_COMPONENTTYPE_DST_HEADPHONES:  return "DST_HEADPHONES";
    case MIXERLINE_COMPONENTTYPE_DST_TELEPHONE:   return "DST_TELEPHONE";
    case MIXERLINE_COMPONENTTYPE_DST_WAVEIN:      return "DST_WAVEIN";
    case MIXERLINE_COMPONENTTYPE_DST_VOICEIN:     return "DST_VOICEIN";
    case MIXERLINE_COMPONENTTYPE_SRC_UNDEFINED:   return "SRC_UNDEFINED";
    case MIXERLINE_COMPONENTTYPE_SRC_DIGITAL:     return "SRC_DIGITAL";
    case MIXERLINE_COMPONENTTYPE_SRC_LINE:        return "SRC_LINE";
    case MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE:  return "SRC_MICROPHONE";
    case MIXERLINE_COMPONENTTYPE_SRC_SYNTHESIZER: return "SRC_SYNTHESIZER";
    case MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC: return "SRC_COMPACTDISC";
    case MIXERLINE_COMPONENTTYPE_SRC_TELEPHONE:   return "SRC_TELEPHONE";
    case MIXERLINE_COMPONENTTYPE_SRC_PCSPEAKER:   return "SRC_PCSPEAKER";
    case MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT:     return "SRC_WAVEOUT  <-- what a player looks for";
    case MIXERLINE_COMPONENTTYPE_SRC_AUXILIARY:   return "SRC_AUXILIARY";
    case MIXERLINE_COMPONENTTYPE_SRC_ANALOG:      return "SRC_ANALOG";
    default:                                      return "(unrecognised)";
    }
}

static const char *ControlTypeName(DWORD t)
{
    switch (t)
    {
    case MIXERCONTROL_CONTROLTYPE_CUSTOM:         return "CUSTOM";
    case MIXERCONTROL_CONTROLTYPE_BOOLEANMETER:   return "BOOLEANMETER";
    case MIXERCONTROL_CONTROLTYPE_SIGNEDMETER:    return "SIGNEDMETER";
    case MIXERCONTROL_CONTROLTYPE_PEAKMETER:      return "PEAKMETER";
    case MIXERCONTROL_CONTROLTYPE_UNSIGNEDMETER:  return "UNSIGNEDMETER";
    case MIXERCONTROL_CONTROLTYPE_BOOLEAN:        return "BOOLEAN";
    case MIXERCONTROL_CONTROLTYPE_ONOFF:          return "ONOFF";
    case MIXERCONTROL_CONTROLTYPE_MUTE:           return "MUTE";
    case MIXERCONTROL_CONTROLTYPE_MONO:           return "MONO";
    case MIXERCONTROL_CONTROLTYPE_LOUDNESS:       return "LOUDNESS";
    case MIXERCONTROL_CONTROLTYPE_STEREOENH:      return "STEREOENH";
    case MIXERCONTROL_CONTROLTYPE_BUTTON:         return "BUTTON";
    case MIXERCONTROL_CONTROLTYPE_DECIBELS:       return "DECIBELS";
    case MIXERCONTROL_CONTROLTYPE_SIGNED:         return "SIGNED";
    case MIXERCONTROL_CONTROLTYPE_UNSIGNED:       return "UNSIGNED";
    case MIXERCONTROL_CONTROLTYPE_PERCENT:        return "PERCENT";
    case MIXERCONTROL_CONTROLTYPE_SLIDER:         return "SLIDER";
    case MIXERCONTROL_CONTROLTYPE_PAN:            return "PAN";
    case MIXERCONTROL_CONTROLTYPE_QSOUNDPAN:      return "QSOUNDPAN";
    case MIXERCONTROL_CONTROLTYPE_FADER:          return "FADER";
    case MIXERCONTROL_CONTROLTYPE_VOLUME:         return "VOLUME  <-- what a player looks for";
    case MIXERCONTROL_CONTROLTYPE_BASS:           return "BASS";
    case MIXERCONTROL_CONTROLTYPE_TREBLE:         return "TREBLE";
    case MIXERCONTROL_CONTROLTYPE_EQUALIZER:      return "EQUALIZER";
    case MIXERCONTROL_CONTROLTYPE_SINGLESELECT:   return "SINGLESELECT";
    case MIXERCONTROL_CONTROLTYPE_MUX:            return "MUX";
    case MIXERCONTROL_CONTROLTYPE_MULTIPLESELECT: return "MULTIPLESELECT";
    case MIXERCONTROL_CONTROLTYPE_MIXER:          return "MIXER";
    case MIXERCONTROL_CONTROLTYPE_MICROTIME:      return "MICROTIME";
    case MIXERCONTROL_CONTROLTYPE_MILLITIME:      return "MILLITIME";
    default:                                      return "(unrecognised)";
    }
}

/* Dump every control on one mixer line, including its range and live value.
 * The range is the whole point: item n42 says the slider only traverses about
 * 0.54 dB of the driver's 95.25 dB, so either dwMinimum/dwMaximum or cSteps
 * as the mixer API reports them should look wrong here. */
static void DumpLineControls(HMIXER hmx, MIXERLINE *ml, const char *indent)
{
    MIXERLINECONTROLS mlc;
    MIXERCONTROL      ctrls[40];
    MMRESULT          mr;
    DWORD             i;

    if (ml->cControls == 0)
    {
        L("%scControls = 0  (this line exposes no controls at all)\n", indent);
        return;
    }

    ZeroMemory(&mlc, sizeof(mlc));
    ZeroMemory(ctrls, sizeof(ctrls));
    mlc.cbStruct  = sizeof(mlc);
    mlc.dwLineID  = ml->dwLineID;
    mlc.cControls = (ml->cControls > 40) ? 40 : ml->cControls;
    mlc.cbmxctrl  = sizeof(MIXERCONTROL);
    mlc.pamxctrl  = ctrls;

    mr = mixerGetLineControls((HMIXEROBJ)hmx, &mlc, MIXER_GETLINECONTROLSF_ALL);
    L("%smixerGetLineControls(ALL) for %u control(s)\n", indent, ml->cControls);
    if (mr != MMSYSERR_NOERROR) { L("%s", indent); MM(mr); return; }

    for (i = 0; i < mlc.cControls; i++)
    {
        MIXERCONTROL *c = &ctrls[i];
        L("%s  control[%u] id=%u type=%08X %s\n",
          indent, i, c->dwControlID, c->dwControlType,
          ControlTypeName(c->dwControlType));
        L("%s    name=\"%s\" short=\"%s\"\n", indent, c->szName, c->szShortName);
        L("%s    fdwControl=%08X%s%s%s cMultipleItems=%u\n",
          indent, c->fdwControl,
          (c->fdwControl & MIXERCONTROL_CONTROLF_UNIFORM)  ? " UNIFORM"  : "",
          (c->fdwControl & MIXERCONTROL_CONTROLF_MULTIPLE) ? " MULTIPLE" : "",
          (c->fdwControl & MIXERCONTROL_CONTROLF_DISABLED) ? " DISABLED <-- greyed out" : "",
          c->cMultipleItems);
        L("%s    Bounds  min=%d (%08X)  max=%d (%08X)\n",
          indent,
          (int)c->Bounds.lMinimum, (unsigned)c->Bounds.lMinimum,
          (int)c->Bounds.lMaximum, (unsigned)c->Bounds.lMaximum);
        L("%s    Metrics cSteps=%u (%08X)\n",
          indent, c->Metrics.cSteps, (unsigned)c->Metrics.cSteps);

        /* A mixer VOLUME control is defined to run 0..65535. Anything else is
         * a genuine defect in the layer that built this control. */
        if (c->dwControlType == MIXERCONTROL_CONTROLTYPE_VOLUME)
        {
            if (c->Bounds.lMinimum != 0 || c->Bounds.lMaximum != 65535)
                L("%s    *** VOLUME bounds are not 0..65535 - the mixer API "
                  "defines them as 0..65535 ***\n", indent);
            else
                L("%s    (VOLUME bounds are the canonical 0..65535)\n", indent);
        }

        /* Read the live value. */
        {
            MIXERCONTROLDETAILS mcd;
            MIXERCONTROLDETAILS_UNSIGNED u[8];
            DWORD ch = (c->fdwControl & MIXERCONTROL_CONTROLF_UNIFORM)
                     ? 1 : ml->cChannels;
            if (ch == 0) ch = 1;
            if (ch > 8)  ch = 8;

            ZeroMemory(&mcd, sizeof(mcd));
            ZeroMemory(u, sizeof(u));
            mcd.cbStruct       = sizeof(mcd);
            mcd.dwControlID    = c->dwControlID;
            mcd.cChannels      = ch;
            mcd.cMultipleItems = 0;
            mcd.cbDetails      = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
            mcd.paDetails      = u;

            mr = mixerGetControlDetails((HMIXEROBJ)hmx, &mcd,
                                        MIXER_GETCONTROLDETAILSF_VALUE);
            if (mr == MMSYSERR_NOERROR)
            {
                DWORD k;
                L("%s    value(s):", indent);
                for (k = 0; k < ch; k++) L(" %u", u[k].dwValue);
                L("\n");
            }
            else
            {
                L("%s    mixerGetControlDetails failed", indent);
                L("\n%s    ", indent); MM(mr);
            }
        }
    }
}

/* Stage 5bq -------------------------------------------------------------
 * SweepVolumeControl - the measurement item n42 has always needed.
 *
 * Every previous look at the volume path was read-only, so we knew the
 * driver receives a range of about 0.54 dB across a full slider drag but not
 * WHY. The mapping is what matters: for a given mixer value 0..65535, what
 * KS level in 1/65536 dB does wdmaud hand the driver? This walks the control
 * down its documented domain and reads it back each time. Match the sets
 * against PropertyHandler_Volume lines in C:\stwrtxp_log.txt and the mapping
 * falls out directly - no inference required.
 *
 * Read-back matters as much as the set. If wdmaud accepts 65535 and reports
 * back 65535 while the driver only ever sees a 0.54 dB spread, the fault is
 * wdmaud's dB conversion. If the read-back is quantised to a handful of
 * distinct values, the control's cSteps is the fault. Those are different
 * bugs with different fixes.
 *
 * The original value is restored at the end, so running this cannot leave
 * the machine silent.
 */
static void SweepVolumeControl(HMIXER hmx, MIXERLINE *ml, const char *what)
{
    static const DWORD kSweep[] = {
        65535, 49152, 32768, 16384, 8192, 4096, 1024, 256, 64, 0
    };

    MIXERLINECONTROLS   mlc;
    MIXERCONTROL        ctrls[40];
    MIXERCONTROLDETAILS mcd;
    MIXERCONTROLDETAILS_UNSIGNED u[8];
    MIXERCONTROL       *vol = NULL;
    MMRESULT            mr;
    DWORD               i, k, ch, original[8];

    L("\n  ==== volume sweep: %s ====\n", what);

    if (ml->cControls == 0)
    {
        L("    line exposes no controls - nothing to sweep\n");
        return;
    }

    ZeroMemory(&mlc, sizeof(mlc));
    ZeroMemory(ctrls, sizeof(ctrls));
    mlc.cbStruct  = sizeof(mlc);
    mlc.dwLineID  = ml->dwLineID;
    mlc.cControls = (ml->cControls > 40) ? 40 : ml->cControls;
    mlc.cbmxctrl  = sizeof(MIXERCONTROL);
    mlc.pamxctrl  = ctrls;

    mr = mixerGetLineControls((HMIXEROBJ)hmx, &mlc, MIXER_GETLINECONTROLSF_ALL);
    if (mr != MMSYSERR_NOERROR) { L("    "); MM(mr); return; }

    for (i = 0; i < mlc.cControls; i++)
    {
        if (ctrls[i].dwControlType == MIXERCONTROL_CONTROLTYPE_VOLUME)
        {
            vol = &ctrls[i];
            break;
        }
    }

    if (!vol)
    {
        L("    no VOLUME control on this line - nothing to sweep\n");
        return;
    }

    L("    control id=%u \"%s\" bounds %d..%d cSteps=%u%s\n",
      vol->dwControlID, vol->szName,
      (int)vol->Bounds.lMinimum, (int)vol->Bounds.lMaximum,
      vol->Metrics.cSteps,
      (vol->fdwControl & MIXERCONTROL_CONTROLF_UNIFORM) ? " UNIFORM" : "");

    ch = (vol->fdwControl & MIXERCONTROL_CONTROLF_UNIFORM) ? 1 : ml->cChannels;
    if (ch == 0) ch = 1;
    if (ch > 8)  ch = 8;

    /* Remember where the user had it. */
    ZeroMemory(&mcd, sizeof(mcd));
    ZeroMemory(u, sizeof(u));
    mcd.cbStruct    = sizeof(mcd);
    mcd.dwControlID = vol->dwControlID;
    mcd.cChannels   = ch;
    mcd.cbDetails   = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
    mcd.paDetails   = u;
    mr = mixerGetControlDetails((HMIXEROBJ)hmx, &mcd, MIXER_GETCONTROLDETAILSF_VALUE);
    if (mr != MMSYSERR_NOERROR)
    {
        L("    cannot read the current value - not sweeping, "
          "because it could not be put back\n    ");
        MM(mr);
        return;
    }
    for (k = 0; k < ch; k++) original[k] = u[k].dwValue;

    L("    original value(s):");
    for (k = 0; k < ch; k++) L(" %u", original[k]);
    L("   (restored at the end)\n");
    L("      set ->  read back\n");

    for (i = 0; i < sizeof(kSweep) / sizeof(kSweep[0]); i++)
    {
        ZeroMemory(&mcd, sizeof(mcd));
        ZeroMemory(u, sizeof(u));
        mcd.cbStruct    = sizeof(mcd);
        mcd.dwControlID = vol->dwControlID;
        mcd.cChannels   = ch;
        mcd.cbDetails   = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
        mcd.paDetails   = u;
        for (k = 0; k < ch; k++) u[k].dwValue = kSweep[i];

        mr = mixerSetControlDetails((HMIXEROBJ)hmx, &mcd,
                                    MIXER_SETCONTROLDETAILSF_VALUE);
        if (mr != MMSYSERR_NOERROR)
        {
            L("    %6u ->  set FAILED  ", kSweep[i]);
            MM(mr);
            continue;
        }

        ZeroMemory(&mcd, sizeof(mcd));
        ZeroMemory(u, sizeof(u));
        mcd.cbStruct    = sizeof(mcd);
        mcd.dwControlID = vol->dwControlID;
        mcd.cChannels   = ch;
        mcd.cbDetails   = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
        mcd.paDetails   = u;
        mr = mixerGetControlDetails((HMIXEROBJ)hmx, &mcd,
                                    MIXER_GETCONTROLDETAILSF_VALUE);
        if (mr != MMSYSERR_NOERROR)
        {
            L("    %6u ->  read FAILED  ", kSweep[i]);
            MM(mr);
            continue;
        }

        L("    %6u -> ", kSweep[i]);
        for (k = 0; k < ch; k++) L(" %6u", u[k].dwValue);
        if (u[0].dwValue != kSweep[i])
            L("   <-- NOT what was written");
        L("\n");
    }

    /* Put it back. */
    ZeroMemory(&mcd, sizeof(mcd));
    ZeroMemory(u, sizeof(u));
    mcd.cbStruct    = sizeof(mcd);
    mcd.dwControlID = vol->dwControlID;
    mcd.cChannels   = ch;
    mcd.cbDetails   = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
    mcd.paDetails   = u;
    for (k = 0; k < ch; k++) u[k].dwValue = original[k];
    mr = mixerSetControlDetails((HMIXEROBJ)hmx, &mcd,
                                MIXER_SETCONTROLDETAILSF_VALUE);
    L("    restored to");
    for (k = 0; k < ch; k++) L(" %u", original[k]);
    L("  ");
    MM(mr);
}

static void DumpOneLine(HMIXER hmx, MIXERLINE *ml, const char *indent)
{
    L("%sdwLineID=%08X dwDestination=%u dwSource=%u\n",
      indent, ml->dwLineID, ml->dwDestination, ml->dwSource);
    L("%scomponentType=%08X %s\n",
      indent, ml->dwComponentType, ComponentName(ml->dwComponentType));
    L("%scChannels=%u cConnections=%u cControls=%u\n",
      indent, ml->cChannels, ml->cConnections, ml->cControls);
    L("%sfdwLine=%08X%s%s%s\n", indent, ml->fdwLine,
      (ml->fdwLine & MIXERLINE_LINEF_ACTIVE)      ? " ACTIVE"      : " (inactive)",
      (ml->fdwLine & MIXERLINE_LINEF_DISCONNECTED)? " DISCONNECTED <-- reported unplugged" : "",
      (ml->fdwLine & MIXERLINE_LINEF_SOURCE)      ? " SOURCE"      : "");
    L("%sname=\"%s\" short=\"%s\"\n", indent, ml->szName, ml->szShortName);
    L("%starget: type=%u dev=%u \"%s\" (mid=%04X pid=%04X)\n",
      indent, ml->Target.dwType, ml->Target.dwDeviceID, ml->Target.szPname,
      ml->Target.wMid, ml->Target.wPid);

    if (ml->cChannels == 0)
        L("%s*** cChannels is ZERO - a line with no channels cannot carry "
          "a volume control ***\n", indent);

    DumpLineControls(hmx, ml, indent);
}

static void ProbeMixer(void)
{
    UINT n, d;
    UINT dev;

    L("================ mixer API (what WMP touches FIRST) ================\n");
    n = mixerGetNumDevs();
    L("mixerGetNumDevs = %u\n", n);
    if (n == 0)
    {
        L("*** No mixer devices. A player that opens the mixer to build its\n");
        L("*** volume control would fail right here.\n\n");
        return;
    }

    for (dev = 0; dev < n; dev++)
    {
        MIXERCAPSA mc;
        HMIXER     hmx = NULL;
        MMRESULT   mr;

        ZeroMemory(&mc, sizeof(mc));
        mr = mixerGetDevCapsA(dev, &mc, sizeof(mc));
        L("\n---- mixer device %u ----\n", dev);
        L("  mixerGetDevCaps\n");
        MM(mr);
        if (mr != MMSYSERR_NOERROR) continue;

        L("  name=\"%s\"\n", mc.szPname);
        L("  mid=%04X pid=%04X driverVersion=%08X fdwSupport=%08X\n",
          mc.wMid, mc.wPid, mc.vDriverVersion, mc.fdwSupport);
        L("  cDestinations=%u\n", mc.cDestinations);
        if (mc.cDestinations == 0)
            L("  *** cDestinations is ZERO - the mixer has no output lines ***\n");

        L("  mixerOpen\n");
        mr = mixerOpen(&hmx, dev, 0, 0, MIXER_OBJECTF_MIXER);
        MM(mr);
        if (mr != MMSYSERR_NOERROR || !hmx) continue;

        for (d = 0; d < mc.cDestinations; d++)
        {
            MIXERLINE ml;
            UINT s;

            ZeroMemory(&ml, sizeof(ml));
            ml.cbStruct      = sizeof(ml);
            ml.dwDestination = d;
            mr = mixerGetLineInfo((HMIXEROBJ)hmx, &ml,
                                  MIXER_GETLINEINFOF_DESTINATION);
            L("\n  == destination %u ==\n", d);
            L("    mixerGetLineInfo(DESTINATION)\n");
            L("    "); MM(mr);
            if (mr != MMSYSERR_NOERROR) continue;

            DumpOneLine(hmx, &ml, "    ");

            for (s = 0; s < ml.cConnections; s++)
            {
                MIXERLINE sl;
                ZeroMemory(&sl, sizeof(sl));
                sl.cbStruct      = sizeof(sl);
                sl.dwDestination = d;
                sl.dwSource      = s;
                mr = mixerGetLineInfo((HMIXEROBJ)hmx, &sl,
                                      MIXER_GETLINEINFOF_SOURCE);
                L("\n    -- source %u of destination %u --\n", s, d);
                L("      mixerGetLineInfo(SOURCE)\n");
                L("      "); MM(mr);
                if (mr != MMSYSERR_NOERROR) continue;
                DumpOneLine(hmx, &sl, "      ");
            }
        }

        /* The specific lookup a media player does: find the speakers
         * destination, then the wave-out source feeding it. This is the exact
         * pair WMP needs in order to own a volume control. */
        L("\n  == the lookup a player actually performs ==\n");
        {
            MIXERLINE ml;
            ZeroMemory(&ml, sizeof(ml));
            ml.cbStruct = sizeof(ml);
            ml.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
            mr = mixerGetLineInfo((HMIXEROBJ)hmx, &ml,
                                  MIXER_GETLINEINFOF_COMPONENTTYPE);
            L("    mixerGetLineInfo(COMPONENTTYPE, DST_SPEAKERS)\n");
            L("    "); MM(mr);
            if (mr == MMSYSERR_NOERROR)
                L("    -> \"%s\" lineID=%08X cControls=%u cConnections=%u\n",
                  ml.szName, ml.dwLineID, ml.cControls, ml.cConnections);

            ZeroMemory(&ml, sizeof(ml));
            ml.cbStruct = sizeof(ml);
            ml.dwComponentType = MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT;
            mr = mixerGetLineInfo((HMIXEROBJ)hmx, &ml,
                                  MIXER_GETLINEINFOF_COMPONENTTYPE);
            L("    mixerGetLineInfo(COMPONENTTYPE, SRC_WAVEOUT)\n");
            L("    "); MM(mr);
            if (mr == MMSYSERR_NOERROR)
                L("    -> \"%s\" lineID=%08X cControls=%u cChannels=%u\n",
                  ml.szName, ml.dwLineID, ml.cControls, ml.cChannels);
        }

        /* Stage 5bq: the sweep. Run it on both lines a slider can drive -
         * the speakers destination (what the tray slider moves) and the
         * wave-out source (what a player's own slider moves). They are
         * different KS nodes in our topology, so they can fail differently. */
        {
            MIXERLINE ml;

            ZeroMemory(&ml, sizeof(ml));
            ml.cbStruct = sizeof(ml);
            ml.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
            if (mixerGetLineInfo((HMIXEROBJ)hmx, &ml,
                                 MIXER_GETLINEINFOF_COMPONENTTYPE)
                == MMSYSERR_NOERROR)
                SweepVolumeControl(hmx, &ml, "DST_SPEAKERS (the master slider)");
            else
                L("\n  ==== volume sweep: DST_SPEAKERS ====\n"
                  "    line not found - cannot sweep\n");

            ZeroMemory(&ml, sizeof(ml));
            ml.cbStruct = sizeof(ml);
            ml.dwComponentType = MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT;
            if (mixerGetLineInfo((HMIXEROBJ)hmx, &ml,
                                 MIXER_GETLINEINFOF_COMPONENTTYPE)
                == MMSYSERR_NOERROR)
                SweepVolumeControl(hmx, &ml, "SRC_WAVEOUT (the wave slider)");
            else
                L("\n  ==== volume sweep: SRC_WAVEOUT ====\n"
                  "    line not found - cannot sweep\n");
        }

        mixerClose(hmx);
    }
    L("\n");
}

/* --------------------------------------------------------------- waveOut -- */
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

static const struct { DWORD bit; const char *name; } kWaveSupport[] = {
    { WAVECAPS_PITCH,        "PITCH"        },
    { WAVECAPS_PLAYBACKRATE, "PLAYBACKRATE" },
    { WAVECAPS_VOLUME,       "VOLUME"       },
    { WAVECAPS_LRVOLUME,     "LRVOLUME"     },
    { WAVECAPS_SYNC,         "SYNC"         },
    { WAVECAPS_SAMPLEACCURATE,"SAMPLEACCURATE" },
};

static void ProbeWaveOutCaps(void)
{
    UINT n, i;
    size_t f;
    DWORD vol = 0;
    MMRESULT mr;

    L("================ waveOut caps (the path that already works) ========\n");
    n = waveOutGetNumDevs();
    L("waveOutGetNumDevs = %u\n", n);

    for (i = 0; i < n; i++)
    {
        WAVEOUTCAPSA c;
        ZeroMemory(&c, sizeof(c));
        mr = waveOutGetDevCapsA(i, &c, sizeof(c));
        L("  dev %u: waveOutGetDevCaps\n", i);
        L("  "); MM(mr);
        if (mr != MMSYSERR_NOERROR) continue;
        L("    name=\"%s\"\n", c.szPname);
        L("    mid=%04X pid=%04X driverVersion=%08X\n",
          c.wMid, c.wPid, c.vDriverVersion);
        L("    wChannels=%u%s\n", c.wChannels,
          (c.wChannels == 0xFFFF)
            ? "   <-- 0xFFFF. The pin advertises MaximumChannels=2."
            : "");
        L("    dwSupport=%08X\n", c.dwSupport);
        for (f = 0; f < sizeof(kWaveSupport)/sizeof(kWaveSupport[0]); f++)
            if (c.dwSupport & kWaveSupport[f].bit)
                L("      + %s\n", kWaveSupport[f].name);
        L("    dwFormats=%08X\n", c.dwFormats);
        for (f = 0; f < sizeof(kWaveFormats)/sizeof(kWaveFormats[0]); f++)
            if (c.dwFormats & kWaveFormats[f].bit)
                L("      + %s\n", kWaveFormats[f].name);
    }

    mr = waveOutGetVolume((HWAVEOUT)IntToPtr(0), &vol);
    L("  waveOutGetVolume(dev 0)\n");
    L("  "); MM(mr);
    if (mr == MMSYSERR_NOERROR)
        L("    volume = %08X  (left=%u right=%u of 65535)\n",
          vol, vol & 0xFFFF, (vol >> 16) & 0xFFFF);
    L("\n");
}

/* Integer triangle wave. Deliberately avoids the CRT math library so the
 * build stays a single cl command. */
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

/* Open waveOut and actually play. This is the VLC-equivalent path. It is the
 * control for the DirectSound test: if this is audible and DirectSound is not,
 * or vice versa, that difference is the finding. */
static void ProbeWaveOutPlay(DWORD rate, DWORD toneHz)
{
    WAVEFORMATEX wfx;
    HWAVEOUT     hwo = NULL;
    WAVEHDR      hdr;
    MMRESULT     mr;
    DWORD        bytes = rate * 4;          /* one second, 16-bit stereo */
    char        *buf;

    L("  --- waveOut play test: %u Hz, %u Hz tone ---\n", rate, toneHz);

    buf = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (!buf) { L("    HeapAlloc failed\n"); return; }
    FillTriangle(buf, bytes, rate, toneHz);

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = rate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = rate * 4;

    /* Ask first, exactly as a careful player does. */
    mr = waveOutOpen(NULL, WAVE_MAPPER, &wfx, 0, 0, WAVE_FORMAT_QUERY);
    L("    waveOutOpen(WAVE_FORMAT_QUERY)\n");
    L("    "); MM(mr);

    mr = waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    L("    waveOutOpen(WAVE_MAPPER)\n");
    L("    "); MM(mr);
    if (mr != MMSYSERR_NOERROR || !hwo)
    {
        HeapFree(GetProcessHeap(), 0, buf);
        return;
    }

    ZeroMemory(&hdr, sizeof(hdr));
    hdr.lpData         = buf;
    hdr.dwBufferLength = bytes;

    mr = waveOutPrepareHeader(hwo, &hdr, sizeof(hdr));
    L("    waveOutPrepareHeader\n");
    L("    "); MM(mr);

    mr = waveOutWrite(hwo, &hdr, sizeof(hdr));
    L("    waveOutWrite  (listen: ~1 s)\n");
    L("    "); MM(mr);

    if (mr == MMSYSERR_NOERROR)
    {
        MMTIME mt;
        int guard = 0;
        while (!(hdr.dwFlags & WHDR_DONE) && guard < 100) { Sleep(50); guard++; }
        ZeroMemory(&mt, sizeof(mt));
        mt.wType = TIME_BYTES;
        if (waveOutGetPosition(hwo, &mt, sizeof(mt)) == MMSYSERR_NOERROR)
            L("    final position: %u bytes of %u%s\n",
              mt.u.cb, bytes,
              (mt.u.cb == 0) ? "   <-- position never moved" : "   (advanced)");
        L("    WHDR_DONE = %s\n", (hdr.dwFlags & WHDR_DONE) ? "yes" : "NO (timed out)");
    }

    waveOutUnprepareHeader(hwo, &hdr, sizeof(hdr));
    waveOutClose(hwo);
    HeapFree(GetProcessHeap(), 0, buf);
}

/* ---------------------------------------------------------- DirectSound --- */
static const struct { DWORD bit; const char *name; } kDsCaps[] = {
    { DSCAPS_PRIMARYMONO,    "PRIMARYMONO"    },
    { DSCAPS_PRIMARYSTEREO,  "PRIMARYSTEREO"  },
    { DSCAPS_PRIMARY8BIT,    "PRIMARY8BIT"    },
    { DSCAPS_PRIMARY16BIT,   "PRIMARY16BIT"   },
    { DSCAPS_CONTINUOUSRATE, "CONTINUOUSRATE" },
    { DSCAPS_EMULDRIVER,     "EMULDRIVER (emulating over waveOut)" },
    { DSCAPS_CERTIFIED,      "CERTIFIED"      },
    { DSCAPS_SECONDARYMONO,  "SECONDARYMONO"  },
    { DSCAPS_SECONDARYSTEREO,"SECONDARYSTEREO"},
    { DSCAPS_SECONDARY8BIT,  "SECONDARY8BIT"  },
    { DSCAPS_SECONDARY16BIT, "SECONDARY16BIT" },
};

/* Superseded by ProbeDirectSoundFull below; kept for reference. */
#if 0
static void ProbeDirectSoundBrief(void)
{
    LPDIRECTSOUND pDS = NULL;
    DSCAPS  caps;
    HRESULT hr;
    size_t  i;

    L("================ DirectSound (already known healthy; recheck) ======\n");
    hr = DirectSoundCreate(NULL, &pDS, NULL);
    L("  DirectSoundCreate(NULL)\n        -> %08X\n", (ULONG)hr);
    if (FAILED(hr) || !pDS) { L("  failed\n\n"); return; }

    ZeroMemory(&caps, sizeof(caps));
    caps.dwSize = sizeof(caps);
    hr = IDirectSound_GetCaps(pDS, &caps);
    L("  GetCaps\n        -> %08X\n", (ULONG)hr);
    if (SUCCEEDED(hr))
    {
        L("    dwFlags = %08X\n", caps.dwFlags);
        for (i = 0; i < sizeof(kDsCaps)/sizeof(kDsCaps[0]); i++)
            if (caps.dwFlags & kDsCaps[i].bit)
                L("      + %s\n", kDsCaps[i].name);
        L("    secondary rate window   = %u .. %u\n",
          caps.dwMinSecondarySampleRate, caps.dwMaxSecondarySampleRate);
        L("    dwMaxHwMixingAllBuffers = %u\n", caps.dwMaxHwMixingAllBuffers);
        L("    dwFreeHwMixingAllBuffers= %u%s\n",
          caps.dwFreeHwMixingAllBuffers,
          (caps.dwMaxHwMixingAllBuffers > 0 && caps.dwFreeHwMixingAllBuffers == 0)
            ? "   <-- max>0 but free==0: the single pin instance is taken"
            : "");
    }

    IDirectSound_Release(pDS);
    L("\n");
}
#endif

/* ------------------------------------------- Stage 5bs: dsound plumbing --- */
/*
 * n49: 32-bit DirectSoundCreate fails with DSERR_INVALIDPARAM, and the driver
 * log proves that no ioctl is ever sent at the moment it gives up. Both
 * bitnesses run byte-identical through eleven property requests (ending with
 * CHANNEL_CONFIG and STEREO_SPEAKER_GEOMETRY, both succeeding) and then the
 * 64-bit one issues KSPROPERTY_PIN_DATAINTERSECTION and creates its pin while
 * the 32-bit one issues nothing further at all. So dsound.dll rejects this
 * device on a value it read in USER MODE.
 *
 * Everything above this comment probes APIs that work in both bitnesses. This
 * section probes the layer underneath DirectSound: the waveOut driver messages
 * dsound itself uses to find and describe a device before it touches KS.
 *
 * DSDRIVERDESC is the interesting one. It is the only structure in this path
 * that contains POINTERS, so it is meaningfully shorter in a 32-bit process
 * than in a 64-bit one. If the thunk in SysWOW64\wdmaud.drv mishandles it,
 * 32-bit dsound reads a garbage device description and rejects the device
 * without ever sending a request - which is exactly n49's signature.
 *
 * Nothing here writes anything. Every call is a query.
 */

#ifndef DRV_RESERVED
#define DRV_RESERVED 0x0800
#endif
#define MSG_DEVICEINTERFACE_A  (DRV_RESERVED + 12)
#define MSG_DEVICEINTERFACE_B  (DRV_RESERVED + 13)
#define MSG_DSOUNDIFACE        (DRV_RESERVED + 20)
#define MSG_DSOUNDDESC         (DRV_RESERVED + 21)

/* Declared locally rather than pulled from mmddk.h, which is not in the WDK's
 * inc\api. The layout is the documented one; the point of printing it is that
 * sizeof() differs between the two builds and both numbers go in the log. */
typedef struct _MY_DSDRIVERDESC {
    DWORD   dwFlags;
    CHAR    szDesc[256];
    CHAR    szDrvname[256];
    DWORD   dnDevNode;
    WORD    wVxdId;
    WORD    wReserved;
    ULONG   ulDeviceNum;
    DWORD   dwHeapType;
    LPVOID  pvDirectDrawHeap;
    DWORD   dwMemStartAddress;
    DWORD   dwMemEndAddress;
    DWORD   dwMemAllocExtra;
    LPVOID  pvReserved1;
    LPVOID  pvReserved2;
} MY_DSDRIVERDESC;

static void DumpPrintable(const char *label, const char *s, size_t max)
{
    size_t i;
    L("    %s = \"", label);
    for (i = 0; i < max && s[i]; i++)
        L("%c", (s[i] >= 32 && s[i] < 127) ? s[i] : '?');
    L("\"\n");
}

static void ProbeDsoundPlumbing(void)
{
    UINT     devId = 0;
    DWORD    cb = 0;
    MMRESULT mr;
    UINT     sizeMsg = 0, pathMsg = 0;

    L("========= Stage 5bs: the plumbing dsound uses BEFORE it calls KS =====\n");
    L("(all queries, nothing is written)\n");
    L("  sizeof(DSDRIVERDESC) in THIS build = %u bytes\n",
      (unsigned)sizeof(MY_DSDRIVERDESC));
    L("  sizeof(void*) = %u\n\n", (unsigned)sizeof(void *));

    /* Which of the two adjacent messages is the SIZE query is settled here by
     * asking rather than by trusting a remembered constant. Whichever returns
     * NOERROR with a plausible byte count is the size one. */
    for (;;)
    {
        cb = 0;
        mr = waveOutMessage((HWAVEOUT)(UINT_PTR)devId, MSG_DEVICEINTERFACE_A,
                            (DWORD_PTR)&cb, sizeof(cb));
        L("  waveOutMessage(%04X) as size query -> %u, cb=%u\n",
          MSG_DEVICEINTERFACE_A, mr, cb);
        if (mr == MMSYSERR_NOERROR && cb > 0 && cb < 2048)
        {
            sizeMsg = MSG_DEVICEINTERFACE_A;
            pathMsg = MSG_DEVICEINTERFACE_B;
            break;
        }

        cb = 0;
        mr = waveOutMessage((HWAVEOUT)(UINT_PTR)devId, MSG_DEVICEINTERFACE_B,
                            (DWORD_PTR)&cb, sizeof(cb));
        L("  waveOutMessage(%04X) as size query -> %u, cb=%u\n",
          MSG_DEVICEINTERFACE_B, mr, cb);
        if (mr == MMSYSERR_NOERROR && cb > 0 && cb < 2048)
        {
            sizeMsg = MSG_DEVICEINTERFACE_B;
            pathMsg = MSG_DEVICEINTERFACE_A;
        }
        break;
    }

    if (sizeMsg == 0)
    {
        L("  neither message answered as a size query.\n");
        L("  *** dsound cannot locate this device's KS interface path. ***\n\n");
    }
    else
    {
        WCHAR wpath[1024];
        char  apath[1024];
        int   n;

        L("  -> size message is %04X, path message is %04X, path is %u bytes\n",
          sizeMsg, pathMsg, cb);

        ZeroMemory(wpath, sizeof(wpath));
        if (cb > (DWORD)sizeof(wpath) - 2) cb = (DWORD)sizeof(wpath) - 2;

        mr = waveOutMessage((HWAVEOUT)(UINT_PTR)devId, pathMsg,
                            (DWORD_PTR)wpath, cb);
        L("  waveOutMessage(%04X) fetch path -> %u\n", pathMsg, mr);

        if (mr == MMSYSERR_NOERROR)
        {
            HANDLE h;

            n = WideCharToMultiByte(CP_ACP, 0, wpath, -1, apath,
                                    sizeof(apath) - 1, NULL, NULL);
            apath[(n > 0 && n < (int)sizeof(apath)) ? n : 0] = 0;
            L("    path = \"%s\"\n", apath);
            L("    length = %u wide chars\n", (unsigned)(n > 0 ? n - 1 : 0));

            /* This is literally dsound's next move. If it fails here in one
             * bitness and not the other, n49 is solved. */
            h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                            FILE_FLAG_OVERLAPPED, NULL);
            if (h == INVALID_HANDLE_VALUE)
                L("    CreateFile on it -> FAILED, GetLastError=%u\n",
                  GetLastError());
            else
            {
                L("    CreateFile on it -> OK (handle opened)\n");
                CloseHandle(h);
            }
        }
        L("\n");
    }

    /* The pointer-bearing structure. */
    {
        MY_DSDRIVERDESC dd;
        ZeroMemory(&dd, sizeof(dd));
        mr = waveOutMessage((HWAVEOUT)(UINT_PTR)devId, MSG_DSOUNDDESC,
                            (DWORD_PTR)&dd, sizeof(dd));
        L("  waveOutMessage(%04X) DSOUNDDESC -> %u%s\n", MSG_DSOUNDDESC, mr,
          (mr == MMSYSERR_NOTSUPPORTED) ? "  (NOTSUPPORTED)" : "");
        if (mr == MMSYSERR_NOERROR)
        {
            L("    dwFlags      = %08X\n", dd.dwFlags);
            DumpPrintable("szDesc   ", dd.szDesc, 256);
            DumpPrintable("szDrvname", dd.szDrvname, 256);
            L("    dnDevNode    = %08X\n", dd.dnDevNode);
            L("    ulDeviceNum  = %u\n", dd.ulDeviceNum);
            L("    dwHeapType   = %u\n", dd.dwHeapType);
        }
    }

    {
        void *iface = NULL;
        mr = waveOutMessage((HWAVEOUT)(UINT_PTR)devId, MSG_DSOUNDIFACE,
                            (DWORD_PTR)&iface, sizeof(iface));
        L("  waveOutMessage(%04X) DSOUNDIFACE -> %u%s\n", MSG_DSOUNDIFACE, mr,
          (mr == MMSYSERR_NOTSUPPORTED)
            ? "  (NOTSUPPORTED - normal for a WDM driver)" : "");
    }

    L("\n");
}

/* ------------------------------------- Stage 5bs: full DirectSound probe --- */

#define MAX_DS_DEVS 8
static GUID  g_dsGuid[MAX_DS_DEVS];
static BOOL  g_dsHasGuid[MAX_DS_DEVS];
static char  g_dsDesc[MAX_DS_DEVS][256];
static int   g_dsCount = 0;

static BOOL CALLBACK DsEnumCb(LPGUID g, LPCSTR desc, LPCSTR mod, LPVOID ctx)
{
    (void)ctx;
    if (g_dsCount >= MAX_DS_DEVS) return FALSE;
    if (g) { g_dsGuid[g_dsCount] = *g; g_dsHasGuid[g_dsCount] = TRUE; }
    else   { ZeroMemory(&g_dsGuid[g_dsCount], sizeof(GUID));
             g_dsHasGuid[g_dsCount] = FALSE; }
    lstrcpynA(g_dsDesc[g_dsCount], desc ? desc : "(null)", 255);
    L("  [%d] guid=%s desc=\"%s\" module=\"%s\"\n", g_dsCount,
      g ? "yes" : "NULL (default device)", g_dsDesc[g_dsCount],
      mod ? mod : "");
    if (g)
        L("      {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
          g->Data1, g->Data2, g->Data3,
          g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
          g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    g_dsCount++;
    return TRUE;
}

/* Play a real tone through DirectSound. This is the branch that matters after
 * Stage 5br: the master volume control went from a UNIFORM one-channel control
 * to a genuine two-channel one, which is a value dsound reads while deciding
 * whether it can drive this device at all. If DirectSound now initialises,
 * this plays and WMP should work. */
static void PlayDirectSoundTone(LPDIRECTSOUND pDS)
{
    WAVEFORMATEX     wfx;
    DSBUFFERDESC     bd;
    LPDIRECTSOUNDBUFFER pBuf = NULL;
    HRESULT          hr;
    void            *p1 = NULL, *p2 = NULL;
    DWORD            b1 = 0, b2 = 0;
    DWORD            bytes = 48000 * 4;   /* 1 s, 48 kHz, 16-bit stereo */

    hr = IDirectSound_SetCooperativeLevel(pDS, GetDesktopWindow(),
                                          DSSCL_PRIORITY);
    L("    SetCooperativeLevel(PRIORITY) -> %08X\n", (ULONG)hr);
    if (FAILED(hr)) return;

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = 48000 * 4;

    ZeroMemory(&bd, sizeof(bd));
    bd.dwSize        = sizeof(bd);
    bd.dwFlags       = DSBCAPS_GLOBALFOCUS;
    bd.dwBufferBytes = bytes;
    bd.lpwfxFormat   = &wfx;

    hr = IDirectSound_CreateSoundBuffer(pDS, &bd, &pBuf, NULL);
    L("    CreateSoundBuffer(1 s, 48k stereo) -> %08X\n", (ULONG)hr);
    if (FAILED(hr) || !pBuf) return;

    hr = IDirectSoundBuffer_Lock(pBuf, 0, bytes, &p1, &b1, &p2, &b2, 0);
    L("    Lock -> %08X (%u bytes)\n", (ULONG)hr, b1);
    if (SUCCEEDED(hr))
    {
        FillTriangle(p1, b1, 48000, 440);
        IDirectSoundBuffer_Unlock(pBuf, p1, b1, p2, b2);
        hr = IDirectSoundBuffer_Play(pBuf, 0, 0, 0);
        L("    Play -> %08X   (LISTEN: 440 Hz for one second)\n", (ULONG)hr);
        Sleep(1200);
        IDirectSoundBuffer_Stop(pBuf);
    }
    IDirectSoundBuffer_Release(pBuf);
}

static void ProbeDirectSoundFull(void)
{
    LPDIRECTSOUND pDS = NULL;
    HRESULT hr;
    int i;

    L("================ Stage 5bs: DirectSound, every entry point =========\n");

    g_dsCount = 0;
    L("  DirectSoundEnumerate:\n");
    hr = DirectSoundEnumerateA(DsEnumCb, NULL);
    L("  -> %08X, %d device(s)\n\n", (ULONG)hr, g_dsCount);

    SetLastError(0);
    hr = DirectSoundCreate(NULL, &pDS, NULL);
    L("  DirectSoundCreate(NULL)  -> %08X   GetLastError=%u\n",
      (ULONG)hr, GetLastError());

    if (FAILED(hr) || !pDS)
    {
        /* Try each enumerated GUID explicitly - the default-device lookup and
         * the explicit-GUID path are different code inside dsound.dll. */
        for (i = 0; i < g_dsCount; i++)
        {
            if (!g_dsHasGuid[i]) continue;
            pDS = NULL;
            SetLastError(0);
            hr = DirectSoundCreate(&g_dsGuid[i], &pDS, NULL);
            L("  DirectSoundCreate(dev %d \"%s\") -> %08X   GetLastError=%u\n",
              i, g_dsDesc[i], (ULONG)hr, GetLastError());
            if (SUCCEEDED(hr) && pDS) break;
            pDS = NULL;
        }
    }

    if (!pDS)
    {
        L("\n  *** DirectSound could not be created by any route. ***\n");
        L("  The driver log will show whether ANY request reached the driver\n");
        L("  after the STEREO_SPEAKER_GEOMETRY set. If it shows none, the\n");
        L("  rejection is still entirely inside dsound.dll, and the plumbing\n");
        L("  block above is where the bad value came from.\n\n");
        return;
    }

    {
        DSCAPS caps;
        size_t k;
        ZeroMemory(&caps, sizeof(caps));
        caps.dwSize = sizeof(caps);
        hr = IDirectSound_GetCaps(pDS, &caps);
        L("  GetCaps -> %08X\n", (ULONG)hr);
        if (SUCCEEDED(hr))
        {
            L("    dwFlags = %08X\n", caps.dwFlags);
            for (k = 0; k < sizeof(kDsCaps)/sizeof(kDsCaps[0]); k++)
                if (caps.dwFlags & kDsCaps[k].bit)
                    L("      + %s\n", kDsCaps[k].name);
            L("    secondary rate window   = %u .. %u\n",
              caps.dwMinSecondarySampleRate, caps.dwMaxSecondarySampleRate);
            L("    dwMaxHwMixingAllBuffers = %u\n", caps.dwMaxHwMixingAllBuffers);
            L("    dwFreeHwMixingAllBuffers= %u\n", caps.dwFreeHwMixingAllBuffers);
        }
    }

    L("  ---- actually play something through DirectSound ----\n");
    PlayDirectSoundTone(pDS);

    IDirectSound_Release(pDS);
    L("\n");
}

int __cdecl main(void)
{
    HRESULT hr;

    OpenLog();

    L("audiodiag - mixer / waveOut / DirectSound probe for stwrtxp\n");
    L("Stage 5bs. n49 / WMP: 32-bit DirectSoundCreate fails with\n");
    L("DSERR_INVALIDPARAM and sends no ioctl, so dsound rejects this device\n");
    L("in user mode. This build probes the plumbing underneath DirectSound\n");
    L("(the waveOut driver messages dsound uses to find and describe the\n");
    L("device) and then every DirectSound entry point in turn. Run BOTH\n");
    L("bitnesses and diff them - the first line that differs is the bug.\n");
#ifdef _WIN64
    L("build: x64 (64-bit).  NOTE: WMP itself is a 32-bit process.\n");
#else
    L("build: x86 (32-bit).  This matches WMP's own bitness.\n");
#endif
    L("log file: %s\n", g_logPath);
    L("====================================================================\n\n");

    hr = CoInitialize(NULL);
    L("CoInitialize -> %08X\n\n", (ULONG)hr);

    ProbeMixer();
    ProbeWaveOutCaps();

    L("================ waveOut playback (the VLC-equivalent path) ========\n");
    L("(listen for TWO tones: 440 Hz at 48 kHz, then 220 Hz at 44.1 kHz)\n");
    ProbeWaveOutPlay(48000, 440);
    Sleep(300);
    ProbeWaveOutPlay(44100, 220);
    L("\n");

    ProbeDsoundPlumbing();
    ProbeDirectSoundFull();

    CoUninitialize();

    L("====================================================================\n");
    L("done.\n");
    L("log written to: %s\n", g_logPath);
    if (g_log) fclose(g_log);
    return 0;
}
