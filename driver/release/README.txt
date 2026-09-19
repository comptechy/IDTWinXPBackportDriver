stwrtxp - RELEASE (free) build
==============================

IDT 92HD89E2 HD Audio driver for 64-bit Windows XP, on the HP Pavilion
p6-2133w. This is the "keep this one" build: the same driver as the checked
build in ..\checked\, with every diagnostic compiled out. Install this if you
just want working audio and don't want the machine writing a log file.

PREREQUISITE: Microsoft's UAA bus driver, KB888111, must already be
installed. Windows XP has no inbox HD Audio bus driver, so without it the
HDAUDIO\FUNC_01 device this INF matches against does not exist at all.


What is in here
---------------
  stwrtxp.sys    25,088 bytes   free (retail) x64 build, no logging
                                MD5 637eaa9056f05f077fa7caea9617c18a
  stwrtxp.inf    11,556 bytes   identical to the INF in ..\checked\
  stwrtxp.pdb   404,480 bytes   symbols - not needed to install, keep it in
                                case the driver ever bugchecks and a dump
                                needs to be read
  fixthunk.cmd    8,712 bytes   NOT needed for a normal install. See
                                "The 32-bit fix" below.

Note on the size: this .sys is 25,088 bytes, which is the third release build
in a row to come out at exactly that size. They are all different files - the
figure is a section-padding artefact and is no use at all for telling builds
apart. Check the MD5 instead. This build is
637eaa9056f05f077fa7caea9617c18a; its immediate predecessor was
0672d29c73a37fa190a07d7c3643c33c and the one before that
94a2f1327db8fd2aa42667494dabe595.

The MD5 identifies this FILE, not this SOURCE. WDK 7600 stamps a build
timestamp into the PE header, so rebuilding byte-identical source yields a
different hash every time. Use the hash to tell two files apart; never read
it as a fingerprint of what went into one.


Install
-------
  Device Manager -> the IDT audio device -> Update Driver -> browse to this
  folder. If the checked build is currently installed, this replaces it.

  THEN REBOOT. This is not optional and it is not superstition. The INF
  registers ksthunk.sys as an upper filter on the MEDIA device class (see
  "The 32-bit fix" below), and PnP reads a class UpperFilters value when it
  BUILDS a device stack. Updating a driver through Device Manager restarts
  the device but does not necessarily rebuild the stack with a newly added
  filter, so until a cold boot the thunk is registered but not attached -
  and the symptom of that is precisely the symptom it was added to fix:
  64-bit audio works, system sounds work, and Windows Media Player reports
  "There was a problem with your sound device."

  This cost a round trip in Stage 5bx. If 32-bit audio is broken right after
  an install, reboot before investigating anything else.

  After installing, C:\stwrtxp_log.txt stops growing. You can delete that
  file; nothing will recreate it.


The 32-bit fix
--------------
This INF registers ksthunk.sys as an upper filter on the MEDIA device class.
That is what makes 32-bit audio work - Windows Media Player, and any other
32-bit program that uses hardware-accelerated DirectSound.

ksthunk.sys is part of Windows XP x64 itself; its service is named, verbatim,
"Kernel Streaming WOW64 Thunk Service". It translates kernel-streaming
requests coming from 32-bit processes into the 64-bit layout the kernel
expects. Windows normally registers it from wdmaudio.inf's class-installer
section, which a device INF cannot inherit - and on this machine the
registration was missing, so 64-bit audio worked and 32-bit audio did not.
Adding it back is the last fix this project needed.

The INF does NOT copy ksthunk.sys. The file ships with the operating system
and lives at %SystemRoot%\system32\drivers\ksthunk.sys. It is present on
every XP x64 install, including ones with no sound hardware at all.

fixthunk.cmd does the same thing by hand and prints what it finds. You do not
need to run it for a normal install - the INF covers it. It is here for two
reasons: to check the current state without changing anything, and because
it writes an unfixthunk.cmd that undoes the registration. If the sound device
ever comes up with a yellow mark and Device Manager code 39, that means
ksthunk.sys could not load; run unfixthunk.cmd and reboot.


What changed in this build
--------------------------
Functionally, nothing. This is the first build made after the diagnostic
apparatus was removed from the SOURCE rather than merely compiled out of the
free build, so it is the first release whose contents can be stated from the
source rather than from the preprocessor.

Removed outright, from both builds:

  * The IRP_MJ_CREATE and IRP_MJ_DEVICE_CONTROL diagnostic dispatch hooks,
    and every helper they pulled in. PortCls's own dispatch table is left
    untouched.
  * DumpDeviceSecurity, the device-object DACL dump in AddDevice and
    StartDevice.
  * The StopEngine DMA-buffer scan.
  * The test-tone path in full: g_HdaTestTone, the sine table, the
    substitution branch in CopyTo, and the "TestTone" registry override that
    switched it on.

The test-tone removal is the one worth spelling out, because the previous
release README described it inaccurately. It said the tone was gated behind
#if (DBG). The registry READ was. The flag, the table and the branch in
CopyTo were not, so the previous free build physically contained a
689 Hz sine table and a code path that substitutes it for the caller's
audio - a byte scan of that binary confirms the table is in it. It could not
actually fire, because with the registry read compiled out the flag stayed
zero and the branch was dead; the old README's claim that no tone could be
injected was therefore true in effect. But a shipped driver has no business
containing a path that replaces the user's audio with a tone, dead or not,
and it is now gone from the source, so it is in neither build.

Still compiled out of this build, as before, by #if (DBG):

  * LogToFileF and every DOUT trace call. The free binary does not import
    ZwCreateFile or ZwWriteFile, so it physically cannot write a log.

The debug verbosity in the source also went back from DBG_ALL to
DBG_DEFAULT, so even a checked build is now quiet by default. That only
affects ..\checked\.

For scale: the checked build went from 77,312 bytes to 65,536. The free
build is unchanged in size, since almost all of what was removed was already
being compiled out of it.


What was deliberately KEPT
--------------------------
  * RelaxPdoSecureOpen - this is not a diagnostic. It clears
    FILE_DEVICE_SECURE_OPEN on the PDO and is what makes the device
    openable at all. Removing it breaks everything.
  * Every functional fix in the project: the topology handlers, the
    volume/mute nodes, the buffer-size change, the 44.1/48 kHz rate work,
    ValidateFormat, the DAC properties, event support, the
    BasicSupportStepped per-channel fix that made the volume slider audible,
    and the ksthunk registration above.


Known state
-----------
Confirmed working ON THIS BUILD, on the target machine, after a reboot:
           Windows Media Player, system sounds, VLC, games, 32-bit and
           64-bit DirectSound, the volume control and mixer, Control Panel
           sees the device.

           The "on this build" matters. A revision of this file briefly
           listed WMP as working here on the strength of a 13 Sep result
           that was actually obtained on the CHECKED build - the proof of
           that fix is a driver log, and only the checked build can write
           one. The free build is functionally identical by source
           inspection: the only #if (DBG) code in the project is the
           logging itself, no ASSERT has side effects, and every hardware
           access goes through the HD Audio bus interface or
           READ_REGISTER_ULONG, so nothing is exposed to /O2. All true, and
           all beside the point - "identical by inspection" is not
           "tested". It has now been tested here directly, which is what
           the first line of this section reports.

Cosmetic, not fixed: waveOutGetDevCaps reports wChannels = 65535. This is a
           sysaudio/kmixer artefact, was measured from both bitnesses, and
           affects nothing - no caller acts on it.


Source
------
Built from the canonical source in ..\..\src\. Build through a junction, not the
real path (WDK 7600 cannot build from a path containing spaces):

  cmd /c mklink /J "C:\stwrtxp_src" "C:\path\to\stwrtxp\src"

Free x64 WNET build, WDK 7600.16385.1:

  setenv C:\WinDDK\7600.16385.1 fre x64 WNET no_oacr
  build -cZ -w

Output lands in objfre_wnet_amd64\amd64\stwrtxp.sys. Swap "fre" for "chk" to
get the diagnostic build instead - that one writes C:\stwrtxp_log.txt and is
what ..\checked\ contains; this folder deliberately does not.

The wnet (Server 2003) target is correct and wxp is not: WDK 7600's wxp
target is 32-bit only, and XP x64 shares the Server 2003 NTAMD64 kernel ABI.

Compiled with zero warnings.

stwrtxp.inf is maintained in ..\checked\ and copied here. If you edit one,
copy it to the other - the two must stay identical, and an earlier stale
copy is exactly what cost a day in Stage 5bv.
