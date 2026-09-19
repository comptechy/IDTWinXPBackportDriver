/*****************************************************************************
 * debug.h
 *****************************************************************************
 * Debug print helper, adapted from the WDK 7600 ac97\driver sample's
 * debug.h (that file is generic port-class boilerplate, not AC'97-specific,
 * so it's reused near-verbatim with our own module name/level set).
 */

#ifndef _DEBUG_H_
#define _DEBUG_H_

const int DBG_NONE     = 0x00000000;
const int DBG_PRINT    = 0x00000001; // function entries / trace
const int DBG_WARNING  = 0x00000002;
const int DBG_ERROR    = 0x00000004;
const int DBG_VERB     = 0x00000010; // codec verb transport tracing
const int DBG_POWER    = 0x00000020;
const int DBG_STREAM   = 0x00000040; // wave stream / DMA tracing
const int DBG_TOPOLOGY = 0x00000080; // topology build tracing

const int DBG_ALL      = 0xFFFFFFFF;
const int DBG_DEFAULT  = 0x00000006;  // warnings + errors only

#ifdef DEFINE_DEBUG_VARS
#if (DBG)
// Warnings and errors only. This was DBG_ALL for the whole diagnostic
// campaign - no live kernel debugger is available on XP (DebugView dropped
// pre-Vista kernel-capture support), so LogToFileF below was the only trace
// path and it had to capture everything. The device now starts, streams and
// serves 32-bit callers, so the trace is no longer earning its cost; raise
// this back to DBG_ALL by hand if a checked build ever needs to be verbose
// again. A free build compiles all of it out either way.
unsigned long ulDebugOut = DBG_DEFAULT;
#endif
#else
#if (DBG)
extern unsigned long ulDebugOut;
#endif
#endif

#if defined(__cplusplus)
extern "C" {
#endif

#if (DBG)
// Appends one already-formatted line to %SystemDrive%\stwrtxp_log.txt so the
// DOUT trace survives to disk with no debugger attached - see LogToFileF's
// definition in common.cpp. Same (Format, ...) signature as DbgPrint so it
// can be invoked the same way DOUT already invokes DbgPrint (`LogToFileF
// strings` where strings is the DOUT caller's parenthesized argument tuple).
void LogToFileF(PCSTR Format, ...);

#define DOUT(lvl, strings)          \
    if ((lvl) & ulDebugOut)         \
    {                               \
        DbgPrint(STR_MODULENAME);   \
        DbgPrint strings;           \
        DbgPrint("\n");             \
        LogToFileF strings;         \
    }
#define BREAK() DbgBreakPoint()
#else
#define DOUT(lvl, strings)
#define BREAK()
#endif

#if defined(__cplusplus)
}
#endif

#endif // _DEBUG_H_
