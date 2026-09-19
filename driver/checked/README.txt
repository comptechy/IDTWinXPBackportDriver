stwrtxp - CHECKED (debug) build
===============================

IDT 92HD89E2 HD Audio driver for 64-bit Windows XP, on the HP Pavilion
p6-2133w.

THIS IS THE DIAGNOSTIC BUILD. It is functionally identical to the release
build in ..\release\, but it writes a trace to

    C:\stwrtxp_log.txt

on every load, appending rather than truncating. Install this one only when
something needs investigating. For ordinary use install ..\release\
instead - it cannot write a log at all, because the free binary does not even
import ZwCreateFile.

PREREQUISITE: Microsoft's UAA bus driver, KB888111, must already be
installed. Windows XP has no inbox HD Audio bus driver, so without it the
HDAUDIO\FUNC_01 device this INF matches against does not exist at all.


What is in here
---------------
  stwrtxp.sys    65,536 bytes   checked x64 build, logging enabled
                                MD5 22889b8dcb5034e75ce14dc87a8dcfd5
  stwrtxp.inf    11,556 bytes   the maintained copy - see below
  stwrtxp.pdb   502,784 bytes   symbols
  fixthunk.cmd    8,712 bytes   NOT needed for a normal install; see the
                                release README for what it is for


Reading the log
---------------
It appends across reboots, so one file holds many sessions. Lines reading

    DriverEntry:

mark a session boundary - the last one is the current boot.

Entries carry the calling process's PID. Filter by PID when you are following
one application; do NOT read adjacent lines as related, because several
processes stream through this driver at once and their lines interleave.

The log is verbose but no longer exhaustive. The trace apparatus built during
the bring-up - the IRP_MJ_CREATE and IRP_MJ_DEVICE_CONTROL dispatch hooks,
the KS property decoder and hex dump, the device-security dumps, the DMA
buffer scan - was removed from the source in the item-p cleanup, so what
remains is the tracing that lives inside the functional code itself - and
the verbosity went from DBG_ALL back to DBG_DEFAULT, so this build reports
warnings and errors rather than everything. That is why it shrank from
77,312 bytes to 65,536.

If you need the full trace, raise ulDebugOut in the source from DBG_DEFAULT
to DBG_ALL and rebuild. That alone restores every DOUT in DriverEntry,
AddDevice, StartDevice, the topology handlers, stream creation, format
validation and the DMA channel - enough to show whether a caller reaches the
driver at all and what it asked for. It was done once, in Stage 5bx, to
chase what turned out to be a missing reboot; the log was never needed.

A NOTE ON THE MD5. It identifies this FILE, not this SOURCE. WDK 7600 stamps
a build timestamp into the PE header, so rebuilding identical source gives a
different hash every time - the Stage 5bx round trip produced three distinct
65,536-byte checked binaries from source that differed only in one constant.
Use the MD5 to tell two files apart, never to infer what is in one.

If you need the full trace back: the apparatus is about 400 lines of
SEH-wrapped kernel code that was expensive to get right, so it was not simply
discarded. It is preserved in ..\..\src\*.bak-5bw, which are kept for exactly
this purpose. Raising ulDebugOut in ..\..\src\debug.h from DBG_DEFAULT back to
DBG_ALL restores the verbosity on its own, without needing any of that.


The INF
-------
stwrtxp.inf is MAINTAINED HERE. ..\release\stwrtxp.inf is a copy of
it, and the two must stay byte-identical - installing a stale copy of this
file is what cost a day in Stage 5bv. Both are currently
MD5 27305dadcfe018be3f377730b8f610d5.

Its important job, beyond the ordinary device install, is registering
ksthunk.sys as an upper filter on the MEDIA device class, which is what makes
32-bit audio work. The release README explains that in full.


Source and build
----------------
Built from ..\..\src\ through a junction, because WDK 7600 cannot build from a
path containing spaces:

  cmd /c mklink /J "C:\stwrtxp_src" "C:\path\to\stwrtxp\src"
  setenv C:\WinDDK\7600.16385.1 chk x64 WNET no_oacr
  build -cZ -w

Output lands in objchk_wnet_amd64\amd64\stwrtxp.sys. Swap "chk" for "fre" to
get the release build.

Compiled with zero warnings.
