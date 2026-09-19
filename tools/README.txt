Diagnostic tools
================

Six user-mode programs written during the stwrtxp bring-up, plus two odds and
ends. NONE of them is part of the driver, and none may ever appear in the
INF's CopyFiles - they are ordinary .exe files that happen to have been built
with the WDK.

They are kept as SOURCE only. The compiled binaries were deleted from
..\package\ and from here in the item-p cleanup, because every question they
were built to answer is now closed. What is expensive to reconstruct is not
the C - it is the build recipe (WNET, the static CRT, and the right PE
subsystem stamp) and, in wmpdiag's case, the interface vtables. Hence this
directory.


  dstest\      DirectSound probe. Creates a device, a secondary buffer, and
               reports every failure with GetLastError. Answered "is this a
               DirectSound problem or a driver problem?"

  audiodiag\   The broadest of the tools. Enumerates waveOut devices, mixer
               lines and controls, and - via ProbeDsoundPlumbing, added in
               Stage 5bs - walks the DirectSound plumbing underneath.

  wmpdiag\     Drives the Windows Media Player filter graph directly through
               hand-declared IGraphBuilder / IMediaControl / IBasicAudio
               vtables. THE MOST EXPENSIVE ONE TO RECONSTRUCT: those vtable
               slot orders were validated against a real quartz.dll, and that
               validation is not repeatable for free. Do not delete.
               (A sample of this tool's output was kept alongside it during
               development; it is not in the public repository because it was
               captured on a development machine and contained local paths.)

  kstest\      Opens the driver's KS pin directly and plays a tone, bypassing
               sysaudio and DirectSound entirely. This was the first thing in
               the project that ever made a sound, and for many stages it was
               the only proof the driver could stream at all.
               patch_kstest.py is the helper that edited it in place during
               the campaign.

  pindump\     Dumps every KS pin's advertised data ranges and probes
               DATAINTERSECTION. Built for both bitnesses on purpose - the
               whole question it answered was a 32/64 split.

  szprobe\     Prints sizeof() for the KS structures in both bitnesses. It is
               what established that KSPROPERTY, KSP_PIN, KSNODEPROPERTY,
               KSDATARANGE and KSDATARANGE_AUDIO are identical on x86 and
               x64 while KSPIN_CONNECT is 64 bytes against 72 - the size
               difference that made the missing ksthunk filter show up
               exactly at the pin create and nowhere earlier.

  msvad_abtest\  msvadtest.inf, for installing Microsoft's MSVAD sample
               driver alongside ours as an A/B control.

  regdump.cmd  Dumps the audio-related registry subtrees from the target
               machine. Run on the XP box; writes regdump\ next to itself.


Building
--------
Two routes are in use here, and they are not interchangeable.

1. A cl command line. dstest, audiodiag and wmpdiag use this. The
   *_build*.cmd scripts are self-contained and commented, and work from this
   directory as it stands, spaces in the path and all:

     dstest\dstest_build.cmd
     audiodiag\audiodiag_build.cmd
     audiodiag\audiodiag_build32.cmd

   All three verified 19 September 2026 from this directory; they reproduce
   audiodiag.exe at 83,456 bytes, audiodiag32.exe at 76,288 and dstest.exe at
   72,192, byte-for-byte the sizes of the binaries that were deleted.

   The recipe, if you need to write a new one:

     setenv C:\WinDDK\7600.16385.1 fre {x86|x64} WNET no_oacr
     cl ... /MT /GS- /I"%DDK%\inc\api" /I"%DDK%\inc\crt"
        /link /SUBSYSTEM:CONSOLE,{5.01|5.02} /MACHINE:{X86|AMD64}
              /NODEFAULTLIB:libc.lib
              /LIBPATH:"%DDK%\lib\wnet\{i386|amd64}"
              /LIBPATH:"%DDK%\lib\crt\{i386|amd64}"

   WNET is the Server 2003 SP2 environment, which is the kernel and CRT
   vintage XP x64 actually runs. /MT links the WDK's static CRT so the .exe
   needs no Visual C++ redistributable on the target. /GS- avoids needing
   BufferOverflowU.lib. The explicit /SUBSYSTEM version matters: the default
   is too new and the XP loader rejects it - 5.02 for x64, 5.01 for x86.

2. The WDK `build` system: a `sources` file, invoked as `build -cZ -w`.
   kstest, pindump and szprobe use this.

   `build` CANNOT RUN FROM A PATH CONTAINING SPACES. It fails with

     U1087: cannot have : and :: dependents for same target

   which is what kstest\buildfre_wnet_x86 failed on when it was run in place
   here; the x64 build of the same tool succeeded only because it was run
   from C:\stwrtxp_kstest. Make a junction first, exactly as the driver
   itself does:

     mklink /J C:\pindump_src "<this dir>\pindump"

   Outputs land in objfre_wnet_{x86\i386,amd64\amd64}\ inside the junction.


The forward-slash include trap
------------------------------
The terse bld32_*.cmd / bld32.cmd / bld64.cmd scripts are the one-line
variants used during the campaign. They are preserved for their exact flag
sets, so they have been left byte-for-byte as they were actually run, with
one exception: they used to cd into a session scratchpad that no longer
exists, and now cd to their own directory.

Be aware before reusing them that they spell their include paths with
forward slashes - `/IC:/WinDDK/7600.16385.1\inc\api`. That reaches the same
directory as the backslash spelling already on %INCLUDE%, but THIS COMPILER
TRACKS ALREADY-INCLUDED HEADERS BY PATH STRING, so it does not recognise the
two spellings as the same file. Any header carrying DEFINE_GUID blocks then
gets processed twice and every GUID in it becomes a C2374 redefinition.
Spell include paths with backslashes. The longer *_build*.cmd scripts
already do, which is why those are the ones that still build.

kstest additionally does `#define INITGUID` and then includes both
<setupapi.h> and <winioctl.h>. winioctl.h keeps its DEFINE_GUID block
OUTSIDE its include guard, deliberately, so that a translation unit can
re-include it after defining INITGUID - which means being included twice
defines those GUIDs twice regardless of the path spelling. It also wants
accctrl.h, which lives in inc\ddk and is not on the default user-mode
include path. So bld32_kstest.cmd needs `/I"%DDK%\inc\ddk"` and one of the
two includes dropped. Use kstest\sources with `build` instead; that route
works, and is how the surviving binaries were produced.


Related
-------
The toolchain proper - Ghidra, the JDK, the extracted XP binaries and the
installers - lives in the repository-level ..\..\tools\ directory, which has
its own README.
