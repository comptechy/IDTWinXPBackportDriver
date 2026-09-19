## Credits and acknowledgements

Do keep in mind that basically ALL OF THIS was made and verified, and everything, by Claude Code. As I don't know shit about coding, ESPECIALLY when it comes to drivers, like, how would I even do this?! With that being said, since AI was what ultimately made this, please keep that in mind when you use this driver. The driver is confirmed to be working on the machine I made it for, I had Claude set this up so it can help anyone else who has this problem. So yes, it's vibe-coded. This'll be the one and only time you'll see a vibe coded thing like this from me. This was the one and ONLY time I would be using AI to help me with something, and I only resorted to this because this driver literally would not work otherwise since the original stuff was only made with WDDM in mind, and not XPDM. Claude likely already says this somewhere, but I'm just repeating it in a shorter, better way to understand, since Claude, like all AIs, loves going into detail lmao, it was very unlikely that something like this would exist otherwise, so that's why this even exists to begin with. I do hope that this driver can help you in some form though! By the way, there's two ways to get the driver itself, the driver's located in the repo's `driver` folder. But, you'd likely not want to download the source code just for the driver itself, which is why I've provided a release for the files, so you wouldn't have to download stuff you likely wouldn't need. If the driver ever gets updated by me due to another bug, more releases will be made.


---

# stwrtxp — IDT 92HD89E2 HD Audio driver for 64-bit Windows XP

A from-scratch audio driver that gives the **IDT 92HD89E2** HD Audio codec working
sound under **Windows XP Professional x64 Edition**, where no such driver has ever
existed.

Developed and confirmed working on an **HP Pavilion p6-2133w**.

```
Hardware ID:  HDAUDIO\FUNC_01&VEN_111D&DEV_76C7&SUBSYS_103C2ACD&REV_1001
```

---

## The problem this solves

IDT shipped this codec with a driver called `stwrt64.sys`. That driver is **WaveRT**,
a driver model Microsoft introduced with Windows Vista. Windows XP does not have
WaveRT — its `portcls.sys` has no `IMiniportWaveRT` interface to bind to. So the
vendor driver cannot load on XP no matter what you do to the INF, and there is no
XP-era driver for this codec to fall back on.

The usual answers do not work either:

| Attempt | Why it fails |
|---|---|
| Install the Vista/Win7 x64 driver | WaveRT miniport; XP's PortCls can't bind it |
| Install the 32-bit XP driver | Different codec generation, and x64 XP won't load a 32-bit driver |
| Generic "High Definition Audio Device" | XP has no inbox HD Audio bus driver at all |

This project re-implements the driver against the model XP *does* have: an **XPDM
WaveCyclic PortCls miniport**, written against the Windows Driver Kit 7600 and
targeting the Server 2003 x64 kernel ABI that XP x64 shares.

The result is a genuinely new driver. No vendor code is reused or redistributed.

---

## Status

**Working**, on the target machine, on the release build in this repository:

- Windows Media Player, VLC, games, and other media players
- Windows system sounds
- 32-bit **and** 64-bit DirectSound
- The volume control, the mute, and the volume mixer
- Control Panel → Sounds and Audio Devices sees the device
- 48 kHz and 44.1 kHz, 16-bit, stereo

**Not implemented:**

- **Recording / capture.** This is a playback-only backport. The capture pins,
  the ADC node and the capture DMA path all exist — enough that KS enumeration is
  well-formed and a capture stream can be created — but the codec-side setup that
  would make them carry audio is deliberately out of scope, so nothing was ever
  recorded with this and nothing is expected to be. See the comment at
  `src/wavecyclicstream.cpp:541`.
- **Sample rates other than 48000 and 44100 Hz.** This is deliberate — it is what
  the codec's own supported-rate bitmap (`0x000E05E0`) actually reports, and
  advertising more would be dishonest. Everything else is resampled by XP's kernel
  mixer, which is normal.
- **Jack detection / headphone auto-switching.** Output goes to the configured
  pin complex.
- **S/PDIF, multichannel, HDMI audio.**

**Known cosmetic quirks** (neither affects playback):

- `waveOutGetDevCaps` reports `wChannels = 65535`. This is a kmixer/sysaudio
  artefact, measured identically from both 32-bit and 64-bit callers. No caller
  acts on it.
- The 64-bit `Drivers32` registry key ends up with duplicate `wave`/`wave1`/`wave2`,
  `midi1`/`midi2`, `mixer1`/`mixer2` entries.

---

## Will this work on *my* machine?

**If you have the same codec, probably — but nobody has tested it.** The INF matches
two hardware IDs:

```
HDAUDIO\FUNC_01&VEN_111D&DEV_76C7&SUBSYS_103C2ACD   ← the exact target machine
HDAUDIO\FUNC_01&VEN_111D&DEV_76C7                   ← any board with this codec
```

The second, subsystem-less match is a deliberate fallback, because some HP BIOS
revisions report different `SUBSYS` values on otherwise identical boards. So the
driver will *install* on any machine whose codec is an IDT 92HD89E2.

Whether it *works* there depends on how that board wires the codec's pin complexes,
which this driver does not auto-discover — the pin and node layout was determined
for one specific board. Expect a working install with no sound as the most likely
failure mode on different hardware. The checked build (see below) writes a log that
will tell you exactly where it stops.

To check your hardware ID: Device Manager → the audio device → Properties →
Details → Hardware Ids.

---

## Requirements

1. **Windows XP Professional x64 Edition.** This driver is x64 only. It will not
   work on 32-bit XP.

2. **Microsoft's UAA bus driver, KB888111.** This is not optional and it must be
   installed *first*. Windows XP ships with no HD Audio bus driver, so without
   KB888111 the `HDAUDIO\FUNC_01` device this INF matches against does not exist
   on the system at all — Device Manager will show an unknown PCI device and the
   INF will match nothing.

   Make sure you get the **x64** package.

3. **The driver is unsigned.** XP x64 does not block unsigned kernel drivers (that
   arrived with Vista x64), but it will show a warning during install. You have to
   click through it.

---

## Install

1. Install KB888111 (x64) first, if you have not already. Reboot.
2. Device Manager → the IDT audio device → **Update Driver**
3. Browse to **`driver/release/`** and let it install `stwrtxp.inf`.
4. Click through the unsigned-driver warning.
5. **REBOOT.**

### Step 5 is not optional

The INF registers `ksthunk.sys` as an upper filter on the MEDIA device class, and
that is what makes 32-bit audio work. PnP reads a class `UpperFilters` value when it
**builds** a device stack. Updating a driver through Device Manager restarts the
device but does not reliably rebuild the stack with a newly added filter — so until
a cold boot the thunk is registered but not attached.

The symptom of that state is *exactly* the symptom the thunk was added to fix:
64-bit audio works, Windows sounds work, and Windows Media Player says
**"There was a problem with your sound device."**

This cost a full debugging round trip during development. If 32-bit audio is broken
immediately after installing, **reboot before investigating anything else.**

---

## Troubleshooting

### "There was a problem with your sound device" in Windows Media Player

Reboot. See above. If you have already rebooted, run `fixthunk.cmd` from the driver
folder — it prints the current state of the `ksthunk` registration without changing
anything unless something is actually missing.

### The device has a yellow mark and Device Manager code 39

Code 39 on every device in the MEDIA class means an upper filter was registered but
its driver file could not load — i.e. `ksthunk.sys` is missing from your install.

Run **`unfixthunk.cmd`** (written into the folder by `fixthunk.cmd`) and reboot.
That removes the registration and gets you back to a bootable audio stack, minus
32-bit sound.

To restore the file itself:

```
expand "%SystemRoot%\Driver Cache\amd64\driver.cab" -F:ksthunk.sys "%SystemRoot%\system32\drivers"
```

### Device installs and shows "working properly", but there is no sound

Install the **checked** build from `driver/checked/` instead. It writes
`C:\stwrtxp_log.txt`, which traces device start, format negotiation, stream
creation and DMA setup. That log is the fastest route to finding where a different
board diverges.

Switch back to the release build afterwards; the checked build appends to that log
forever.

### Control Panel says "no audio device"

That symptom is specifically about the legacy wave-mapper registration
(`AssociatedFilters` / `Drivers\SubClasses` in the INF), not about the driver
failing. If you have edited the INF, that is the first thing to check.

---

## Uninstall

Device Manager → the device → Uninstall, then reboot. If you also want to undo the
`ksthunk` class-filter registration, run `unfixthunk.cmd` first.

---

## What's in this repository

```
driver/
  release/       The build to install. No logging, no diagnostics.
    stwrtxp.sys      25,088 bytes   MD5 637eaa9056f05f077fa7caea9617c18a
    stwrtxp.inf      11,556 bytes   MD5 27305dadcfe018be3f377730b8f610d5
    stwrtxp.pdb                     symbols, for reading a crash dump
    fixthunk.cmd      8,712 bytes   MD5 f987c29a96079161a19d3797bbab03cc
    README.txt                      build-specific release notes
  checked/       Same driver, diagnostics compiled in. Writes C:\stwrtxp_log.txt.
    stwrtxp.sys      65,536 bytes   MD5 22889b8dcb5034e75ce14dc87a8dcfd5
    ... (INF and fixthunk.cmd are byte-identical to release/)

src/             Driver source. WDK 7600, C++, builds with zero warnings.
tools/           User-mode diagnostic programs written during development.
docs/
  DEVELOPMENT-LOG.md   The full engineering record. See below.
```

**A note on the MD5s above:** use them to tell two *files* apart, never as a
fingerprint of what went into a binary. WDK 7600 stamps a build timestamp into the
PE header, so rebuilding byte-identical source produces a different hash every time.
Three different 65,536-byte checked builds came out of one late stage of this
project, two of them from identical source.

Likewise, do not identify a build by its size. Three consecutive release builds all
came out at exactly 25,088 bytes — that figure is a section-padding artefact.

---

## How it works

A short tour, for anyone reading the source or attempting the same thing for a
different codec.

**Driver model.** XPDM WaveCyclic PortCls miniport. The driver provides two
subdevices — a **Wave** filter (`IMiniportWaveCyclic`, `IMiniportWaveCyclicStream`)
and a **Topology** filter (`IMiniportTopology`) — registered with
`PcRegisterSubdevice` from `adapter.cpp`. PortCls supplies the KS plumbing above
them. WaveCyclic means the driver owns a cyclic DMA buffer that PortCls copies
into; WaveRT, which the vendor driver used, instead hands the buffer to the client
and does not exist on XP.

**Hardware access** goes entirely through the `HDAUDIO_BUS_INTERFACE` function
pointers that KB888111's bus driver hands down — codec verbs, stream allocation,
DMA engine setup. There is no raw MMIO poking in the driver except one
`READ_REGISTER_ULONG` of the stream position register.

**`RelaxPdoSecureOpen` is load-bearing.** The bus-owned `HDAUDIO\FUNC_01` PDO comes
up with `FILE_DEVICE_SECURE_OPEN` set and a *populated but empty* DACL — a deny-all.
KS device interfaces are registered on the PDO, so a user-mode `CreateFile` of the
filter path resolves there and is checked against that empty DACL, failing with
`ERROR_ACCESS_DENIED` before any IRP reaches the driver. Clearing that flag is the
single thing that makes the device openable. Do not remove it.

The INF also sets a permissive SDDL on the device's hardware key via a
`[DDInstall.HW]` `AddReg` — the only INF mechanism that actually sets a device's PDO
security descriptor. Two earlier attempts put the same SDDL in interface and
software keys and had zero effect.

> **Historical warning for anyone editing this:** an earlier revision of the driver
> fixed the DACL at runtime with `ObOpenObjectByPointer` + `ZwSetSecurityObject`.
> That is a **reproducible bluescreen** and it has been deleted from the source. If
> a DACL is ever implicated again, fix it declaratively in the INF. Never rewrite
> security at runtime.

**The 32-bit fix (`ksthunk`).** `ksthunk.sys` is XP x64's "Kernel Streaming WOW64
Thunk Service", an upper filter on the MEDIA class that translates kernel-streaming
requests from 32-bit processes into the 64-bit layout the kernel expects. Windows
registers it from `wdmaudio.inf`'s `[ClassInstall32.NT]` section — the **class**
installer, which a device INF's `Include=`/`Needs=` cannot reach. On the target
machine that registration was simply absent.

Without it, 64-bit DirectSound works and anything going through the kernel mixer
works, but 32-bit DirectSound dies at the pin create. The reason is structure
layout: `KSPROPERTY` (24), `KSP_PIN` (32), `KSNODEPROPERTY` (32), `KSDATARANGE`
(64), `KSDATARANGE_AUDIO` (88) and `KSMULTIPLE_ITEM` (8) are all identical in both
bitnesses — which is why traces looked byte-for-byte the same right up to the point
where 32-bit fell silent — but **`KSPIN_CONNECT` is 64 bytes on x86 and 72 on x64.**

The INF adds `ksthunk` to the class `UpperFilters` with
`FLG_ADDREG_TYPE_MULTI_SZ | FLG_ADDREG_APPEND` (`0x00010002`), copied verbatim from
Microsoft's own wording. APPEND does not duplicate a string already present, which
makes it safe alongside any other audio INF on the machine. The INF does **not**
copy `ksthunk.sys` — it ships with the OS.

---

## Building from source

You need the **Windows Driver Kit 7600.16385.1**. Later WDKs cannot target XP.

**WDK 7600's `build.exe` cannot build from a path containing spaces** — nmake fails
with `U1087`. Create a junction first:

```
cmd /c mklink /J "C:\stwrtxp_src" "C:\path\to\this\repo\src"
```

Then, from a plain `cmd`:

```
C:\WinDDK\7600.16385.1\bin\setenv.bat C:\WinDDK\7600.16385.1 fre x64 WNET no_oacr
cd /d C:\stwrtxp_src
build -cZ -w
```

Output lands in `objfre_wnet_amd64\amd64\stwrtxp.sys`. Swap `fre` for `chk` to get
the diagnostic build; that one writes `C:\stwrtxp_log.txt`.

Two things that will bite you:

- **`WNET` is the correct target, not `WXP`.** WDK 7600's `wxp` target is 32-bit
  only, and XP x64 shares the Server 2003 (`wnet`) NTAMD64 kernel ABI.
- **Spell include paths with backslashes.** MSVC in this WDK tracks included headers
  by path *string*, so a `/IC:/WinDDK/...` with forward slashes double-processes
  `DEFINE_GUID` headers and you get C2374 redefinition errors.

The build is clean. The "1 Warning" in BUILD's summary line is the environment
banner ("x64 Native compiling isn't supported"), not a compiler warning.

---

## Diagnostic tools

`tools/` holds the user-mode programs written to debug this, with their build
scripts. They are not needed to use the driver, but they are useful if you are
porting it.

| Tool | What it does |
|---|---|
| `kstest` | Opens the KS filter directly and pushes a stream through it, bypassing DirectSound and the mixer entirely. The bottom of the stack. |
| `pindump` | Enumerates pins, data ranges and node topology from the driver. |
| `audiodiag` | Walks the whole audio stack — devices, formats, the registry state — and reports what each layer sees. |
| `dstest` | DirectSound playback test. Builds 32-bit and 64-bit, which is how the `ksthunk` bug was isolated. |
| `wmpdiag` | Inspects the Windows Media Player side specifically. |
| `szprobe` | Prints kernel structure sizes per bitness — the tool that found the `KSPIN_CONNECT` 64-vs-72 discrepancy. |
| `regdump.cmd` | Dumps the relevant registry state. |

Build them with the `.cmd` scripts alongside each. The recipe is WDK 7600's
compiler, statically linked, targeting subsystem 5.01 (x86) or 5.02 (x64).

---

## The development log

`docs/DEVELOPMENT-LOG.md` is the complete engineering record kept while this was
built — roughly 700 KB of it. It is the working handoff document, not polished
documentation, and it is included because it is the only place that explains *why*
the driver is the way it is.

It is worth reading if you are attempting something similar, mostly for the dead
ends. It records, among other things: a bluescreen caused by rewriting device
security at runtime; a day lost to a stale copy of the INF; a diagnostic build's
result being wrongly generalised to the release build; and a "regression" at the
very end that turned out to be a missing reboot.

Paths in it have been genericised, and it references some directories that cannot
be redistributed (see below), so a few cross-references dangle.

**Source comments refer to it as `HANDOFF.md`, which was its filename during
development.** Those references have deliberately been left alone so that the source
in `src/` stays byte-identical to the maintained copy — a stale duplicate of a file
is precisely the kind of thing that cost a day of debugging here once already. When
a comment says "see HANDOFF.md Stage 5af", it means `docs/DEVELOPMENT-LOG.md`.

---

## What is deliberately *not* in this repository

- **The original IDT/Vista driver package**, and the disassembly and decompilation
  of `stwrt64.sys` produced while working out what the hardware wanted. That is
  proprietary vendor code and analysis derived from it. None of it is in the
  shipped driver — the source here was written against the WDK, the HD Audio
  specification and the codec's own responses.
- **Linux kernel HD-audio sources** (`sound/pci/hda/`) consulted as a reference for
  codec behaviour. Those are GPL-2.0 and mixing them into this tree would change
  its licensing; get them from the kernel tree if you want them.
- **Captured driver logs, VM images, crash dumps**, and a product key. Development
  debris, some of it personal, none of it useful to you.

---

## License

MIT — see [LICENSE](LICENSE).

One thing to be aware of: parts of this driver's structure derive from the
**WDK 7600 `ac97` and `msvad` sample drivers**, which Microsoft ships as sample code
under its own terms. The INF's wave-mapper registration in particular is modelled
closely on `msvad.inf`. This is the normal and intended way to write a PortCls
driver, but it means the MIT grant covers the original work here, not Microsoft's
sample code that it was built on. The LICENSE file says so explicitly.

---



Built against:

- Microsoft Windows Driver Kit 7600.16385.1, and its `ac97` / `msvad` / `toaster`
  sample drivers
- The Intel High Definition Audio specification
- Microsoft KB888111 (UAA bus driver for HD Audio)
- The Linux kernel's `sound/pci/hda` drivers, consulted as a behavioural reference
  for the SigmaTel/IDT codec family
