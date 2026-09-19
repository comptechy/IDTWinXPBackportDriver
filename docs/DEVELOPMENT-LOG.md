# Development log

This is the working engineering record kept while the driver was written,
reproduced here because it is the only place that explains **why** the driver
is built the way it is. It was written as a handoff document for whoever
picked the work up next, not as documentation, so it is long, in rough
chronological order, and argues with itself in places. That is deliberate:
several of the entries record things that turned out to be **wrong**, and the
correction is usually more useful than the conclusion.

Read it if you are porting a different HD Audio codec to XP, debugging this
one, or want to know what a dead end looked like before it was recognised as
one. Do not read it as a specification — the README is the specification.

Two caveats:

* **Some paths referenced here are not in this repository.** The working tree
  also held the original proprietary IDT driver, its disassembly, Linux HDA
  sources used for reference, VM images, crash dumps and captured logs. None
  of those can be redistributed, so references to `Original Driver/`,
  `reference/`, `logs/`, `VMs/`, `tools/xpbin/` and `*.DMP` are dangling.
* **Stages are numbered in the order they happened, not in order of
  correctness.** Later entries supersede earlier ones. Where an entry has been
  superseded it normally says so at the top; where it does not, trust the
  higher stage letter.

Local filesystem paths have been replaced with `C:\path\to\stwrtxp`.

---

# HANDOFF — IDT HD Audio Driver Backport to Windows XP x64

Read this file first when resuming this project. It is the single source of continuity between sessions — update it at the end of every stage, and mid-stage if a session might end before the stage is done.

**>>> SESSION HANDOFF (2026-09-08, latest session): THE RENDER PATH IS DONE — the driver plays clean, repeatable audio on the real HP Pavilion p6-2133w under 64-bit Windows XP and no longer leaks hardware. Fourteen stages in one session. 5aj killed the ACCESS_DENIED wall; 5ak fixed the `0x3B` use-after-free; 5am fixed three defects found by auditing everything `KSSTATE_RUN` newly reaches; 5an found why the first tone test bugchecked (**PortCls never calls `IDmaChannel::AllocateBuffer`**); 5ao found why the surviving build was silent (**the codec's converter was never bound to the stream**); 5ap fixed the DMA-engine free *order*; 5aq is the milestone — **the 440 Hz tone was audible**; 5ar traced the still-live engine leak to **`hdaudio.h` aliasing `StopState` and `PauseState` to the same value (1)**; 5as is the proof it worked; 5at gave the wave filter the bridge pins it never had — **and bluescreened the machine**; 5au read that dump and routed both topology paths through `KSNODETYPE_SUM` nodes — **confirmed, no more bluescreen**; 5av added an `IOCTL_KS_PROPERTY` trace; 5aw read it, killed the cross-filter-hop theory for good, and gave both filters the automation table they never had. **5ax read the Stage 5aw log: both new handlers fire and succeed, a user-mode service now marks this device the system default for BOTH playback and recording — and sysaudio's 118-request enumeration is otherwise byte-for-byte IDENTICAL to the old driver's. The property fix was real but non-gating; the fault is above PortCls, where the trace cannot see.** The one remaining divergence from every WDM audio driver in the WDK is that this INF registers `SubClasses = "wave"` and never `mixer` — the exact layer XP's "no audio device" message comes from. **5ax's mixer-subclass fix WORKED and 5ay's volume nodes WORK: Control Panel lists the device and the sliders are live on hardware (n36 done).** 5az then shrank the cyclic buffer from `0x16000` to `0x4000` because PortCls fills a WaveCyclic buffer BEHIND the play cursor, making stream latency one full buffer lap. **5ba tested it: the mechanism is confirmed and it was not enough.** What the log then shows is the thing nobody had measured: **the audio arriving in the buffer peaks at 103 and 23 out of 32767, i.e. -50 and -63 dBFS, against the -12 dBFS of the kstest tone that has always been audible.** Everything else is provably healthy — the link position is the genuine LPIB (it holds across a pause), and every output verb is right at load (DAC and all five pins at D0, amps unmuted at max gain 127, pin OUT enable, EAPD on). 5ba's substitute test tone then came back **SILENT** — a full-scale sine (peak 16384, RMS 11585) sat in the buffer across five sessions and nothing came out, while kstest was audible on the same build — **so amplitude is NOT the fault and that theory is withdrawn.** **5bb reads the discriminator the same log hands over: every audible stream this project has ever produced was 44.1 kHz and every silent one was 22.05 kHz, with no counterexample.** The cause is that `wavecyclicminiport.cpp` advertised a flat 8-48 kHz range **without ever querying the codec's `PARAMETER 0x0A` supported-rates bitmap** (there is no such query in any log in this project), so kmixer negotiated XP's 22.05 kHz system sounds straight through — and nothing below catches it, because `AllocateRenderDmaEngine` returns success for 22050 and computes `ConverterFormat = 0x4111` itself without ever seeing the codec's caps. 5bb adds `QueryPcmCaps()` (reads and decodes `0x0A`/`0x0B` from both the AFG and the DAC, honouring the Format Override bit) and `NarrowPcmRangeToCodec()`, which clamps the advertised range to **44100..48000** so kmixer resamples. **TESTED (Stage 5bc): the codec's answer CONFIRMS the diagnosis — `0x0A = 0x000E05E0`, bit 3 clear, 22.05 kHz genuinely unsupported — but the clamp did not hold: 22050 Hz still reached `SetFormat` in every session.** The narrowed range *was* honoured where it applies (the pin data intersection returned 48000 Hz), but the 22050 arrives afterwards as a `KSPROPERTY_CONNECTION_DATAFORMAT` set, and **nothing in the stack checks a dynamic format change against the pin's data ranges** — not PortCls (the WDK's own `msvad` says so in a comment), not the bus driver. **Stage 5bc puts the gate where it has to be: a new `CMiniportWaveCyclicHda::ValidateFormat` consulted at the top of `SetFormat`, above `ReleaseEngine()`, so a refused format costs the caller its `SetFormat` and nothing else.** Built clean; `stwrtxp.sys` is now **69120 bytes**. **TESTED (Stage 5bc RESULT): IT WORKS — Windows audio plays through this driver.** Fourteen sessions asked for 22050 Hz, all fourteen were refused, all fourteen kept the 48000 Hz pin format and played (converter format `0011`, peaks up to 18925), and **kmixer resamples on refusal exactly as predicted**, so the driver never has to. **The silence is over.** What remains: **n47**. **n46 is answered and VLC was never this driver's fault** — its own debug log shows the active output module is `mmdevice`/**WASAPI**, a Vista+ API that cannot exist on XP, failing `IAudioClient::Initialize` with `E_INVALIDARG` (`0x80070057`) once per buffer and never once attempting DirectSound or WaveOut, so no create from VLC ever reached this driver (restart VLC after changing the output module, or run `vlc.exe --aout=directsound`; the cached aout is why the WaveOut attempt looked identical). That also **retracts the PID-1752 identification** in the Stage 5bc log — that session was something else, and the "44100 succeeds and reallocates the DMA buffer mid-stream" suspicion goes with it, unexercised and unsupported. **Windows Media Player is the live question, and Stage 5be has the first real answer**: the WMP log proves **WMP does reach this driver** (`MJ_CREATE ... Name="\Wave" returned 00000000`), and the last thing it does before giving up is a **SET of `KSPROPERTY_AUDIO_CHANNEL_CONFIG` on the wave filter, refused `C0000225`** — after which it never creates a pin at all (no `NewStream`, no `SetFormat`). The refusal is structural: both wave nodes carried a NULL automation table, so every node property aimed at that filter returned `STATUS_NOT_FOUND`. `msvad\pcmex` implements this property on exactly that node of exactly that filter, so **5be does the same** — `AutomationDAC` on `NODE_WAVE_DAC`, GET|SET, mono/stereo accepted and everything wider refused. Caveat stated up front: **the failing request names node 8 and this filter has nodes 0 and 1** (2 wave + 7 topology = virtual 0..8), so the id looks like an untranslated sysaudio virtual node and the theory that publishing the property supplies the missing mapping is unproven — which is why the same build raises `DC_LOG_FULL_LIMIT` 6000 -> 40000 so the retest log shows successes as well as failures. `stwrtxp.sys` was 70656 bytes for that round. **TESTED (5bf): the `CHANNEL_CONFIG` fix WORKED and node 8 is solved — it is a sysaudio VIRTUAL node id, and the widened trace proves virtual 8 is this wave filter's DAC node** (sysaudio numbers the composite graph in reverse: topology 0..6 -> virtual 6..0, wave DAC -> 8, ADC -> 7). WMP advanced exactly one property and stopped on the next: **the terminal failure is now `KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY` (Audio id 34) SET on that same node**, and together with the `CHANNEL_CONFIG` set immediately before it that pair is **`IDirectSound::SetSpeakerConfig` decomposed** — half of it succeeding is still a failed DirectSound call, which is the "problem with your sound device" message. 5bf implements it (cached `LONG`, `_HEADPHONE` or 5..180 accepted) plus `CPU_RESOURCES` on the same node; `stwrtxp.sys` is now **71680 bytes**, staged, awaiting reinstall. **Correction carried by 5bf: `Audio id=33` is `CPU_RESOURCES`, not `PEAKMETER`** — `PEAKMETER` is id 37 and appears in no log at all, so the 5bd/5be "no sample implements PEAKMETER" reasoning was aimed at the wrong property. Also proved in passing: **the streaming path is healthy on the same boot** — another process negotiated 48000 then 44100, bound converter `4011` and played real content (peak 755), so WMP's problem is not the pin, the format negotiation, the DMA engine or the converter. **TESTED (5bg): every property Stage 5bf added WORKS** — `CPU_RESOURCES`, `CHANNEL_CONFIG` (mask 3) and `STEREO_SPEAKER_GEOMETRY` (value 20) all return `00000000` on the wave DAC node, and **there is no longer a single failing request anywhere on the wave filter** — yet WMP still refuses. **The session now stops after a SUCCESS, not a failure**, so for the first time the terminal event is invisible from inside the driver. **The blocking unknown is no longer a property but process identity:** two processes matter in the log and the driver records only PIDs — one completes the whole speaker-config negotiation and vanishes without creating a pin, the other creates a pin and plays ~93 ms of real audio, and the two readings select different next fixes. **Stage 5bg therefore logs the process IMAGE NAME on every `MJ_CREATE` (`PsGetProcessImageFileName`, resolved by name at runtime) — read that first in the next log.** It also decodes the `ENABLE_EVENT` IOCTLs and publishes `KSEVENTSETID_AudioControlChange` on the volume/mute nodes, **correcting a second too-narrow search: `sb16` DOES implement event support** (`tables.h:383`, `mintopo.cpp:1444`) where msvad and ac97 do not — though since those refusals happened *before* the stopping point this is gap-closing, not a predicted fix. Confirmed correct and closed: the two summer nodes keep `NULL` automation tables, matching `ac97\driver\mintopo.cpp:1178`/`:1233` on real hardware. `stwrtxp.sys` is now **75264 bytes**, staged, awaiting reinstall. Then n42 (the slider, cosmetic again and finally measurable) and the item-p cleanup plus a `fre` build. n40 is WITHDRAWN — that bug does not exist. **STAGE 5bp RESULT (latest): THE DIVERGENCE IS FOUND AND IT IS NOT IN THIS DRIVER.** The same-boot, same-build, same-API, 64-vs-32 DirectSound comparison finally gave a like-for-like trace. Both bitnesses run **byte-identical** through the whole init sequence — same property sets, same ids, same buffer sizes, same payloads, same statuses, in the same order — ending with `CPU_RESOURCES` on node 8, then `CHANNEL_CONFIG` SET (`03 00 00 00`, stereo) and `STEREO_SPEAKER_GEOMETRY` SET (`14 00 00 00`, wide), both succeeding. Then the 64-bit run issues `KSPROPERTY_PIN_DATAINTERSECTION` at `#758` (`in=216 out=0` -> `80000005`, re-asks `out=82` -> success, gets back a well-formed 48000 Hz 16-bit stereo `KSDATAFORMAT_WAVEFORMATEX`) and creates its pin — and **the 32-bit run issues nothing further at all.** PID 2876 has zero `set=Pin` requests of any id in the entire log. The 216-byte request contains no pointers (`KSP_PIN` 32 + `KSMULTIPLE_ITEM` 8 + 2 x `KSDATARANGE_AUDIO` 88, all identical in both bitnesses per szprobe), so had 32-bit dsound issued it, it would have been byte-for-byte the same. **The decision to give up therefore happens entirely in user mode inside `SysWOW64\dsound.dll`, before any ioctl is sent.** Two more readings die with it: `GetLastError = 234` is **ERROR_MORE_DATA left over from a normal, correctly re-asked `Topology id=3` name-size probe**, not evidence of a failure — so dsound **synthesised** `DSERR_INVALIDPARAM` rather than wrapping an ioctl error (theory twelve, dead); and the status census inverts the obvious guess — the **working** 64-bit run collects **four** `C000000D`s (all `CONNECTION_DATAFORMAT` SET with `PinId=0xFFFFFFFF`, correctly refused) while the failing 32-bit run collects **zero**. `wChannels=65535` is now dead from both sides in one log. Also confirmed on the wire: the 5bn `BasicSupportStepped` fix decodes correctly (`AccessFlags=0x203`, `DescriptionSize=72`, `VT_I4`, min `0xFFA0C000` = -95.25 dB, step 0.75 dB), and the `kstest32` `KSPIN_CONNECT` 146-vs-154 size bug is almost certainly a **separate** bug, since dstest32 never reaches a pin create at all. **THE ONE REMAINING LEAD (n49):** no `KSPROPERTY_PIN_DATARANGES` (`Pin id=3`) query appears anywhere in the log from any PID, so dsound sources the ranges it needs from somewhere that never touches our dispatch table — `sysaudio.sys`'s cached graph (`Sysaudio` `CBE3FAA0` / `SysaudioDev` `0C4F9C81`, answered by sysaudio, invisible to us), or a registry cache. **Cheapest next test, no build required: diff `Drivers32` against its `Wow6432Node` twin on the XP box** — a 32-bit process reads a different key, and the 64-bit one is already known to carry triplicate `wave`/`wave1`/`wave2` entries. Do NOT re-run the 5bp driver-log comparison; it has given everything it can. **STAGE 5bq (staged, awaiting one run): the volume slider is a REAL bug, not a cosmetic one.** The user reported independently that moving the Windows volume slider does nothing audible — that is **n42**, and the Stage 5ay measurement explains it: a full drag delivers levels spanning only `0`..`-35230` in 1/65536 dB units, **0.54 dB out of 95.25 dB**, which at 0.75 dB per codec step moves the attenuator by zero or one of its 127 steps. The driver side was re-audited this stage and is **correct** — AMP_CAP decode, `(n+1) x 0.25 dB` step maths, and the published `-6242304..0 step 49152` all check out, and 5bp confirmed those exact bytes on the wire. So something above us collapses the range, and the one thing never measured is the **mapping**: for mixer value V in 0..65535, what KS level does wdmaud hand us? Every prior look was read-only. `audiodiag` now has `SweepVolumeControl`, which writes ten known values down both the `DST_SPEAKERS` and `SRC_WAVEOUT` lines, reads back each one, and restores the user's original setting at the end; matched against `PropertyHandler_Volume` SET lines the mapping falls out by position, no inference. Three possible shapes, three different fixes — see the table in `## Stage 5bq`. Built both bitnesses (**78,336** / **72,192**) because 5bp proved this stack diverges by bitness and **XP x64's volume control is a 32-bit process**; if the sweep also diverges, n42 and n49 are ONE bug. Also fixed: `OpenLog` hardcoded its filename, so the two builds would have destroyed each other's evidence. **n49 folded into the same trip** as `package/regdump.cmd` (read-only, exports eight keys): `Drivers32` 64-bit vs `Wow6432Node` is the experiment, and **`HKLM\SYSTEM\CurrentControlSet\Control\MediaResources` is the dark horse** — it records which KS filter backs each device for wave, mixer, midi AND DirectSound, and being under `SYSTEM` it is NOT redirected, so if it is wrong it is wrong for both bitnesses. **Cost: zero rebuilds, zero reinstalls, zero reboots** — `package/stwrtxp.sys` is unchanged at 77,312 bytes, the checked 5bp build already installed. Everything added is user-mode. **STAGE 5bs: scope is now n49/WMP alone — the user has said so explicitly, and n42 is verified closed on hardware. WMP and 32-bit DirectSound are proven to be the SAME bug (identical trace, identical stopping point after `set=Audio id=34`), so fixing n49 fixes WMP. The divergence is located to the instruction: eleven property requests byte-identical in both bitnesses, then 64-bit issues `Pin id=4 DATAINTERSECTION` and streams while 32-bit sends nothing further at all — the rejection happens entirely inside `SysWOW64\dsound.dll`. Cleared this window: our data ranges (`MaximumChannels=2`, counts patched correctly, and nobody ever queries `Pin id=3`), `wChannels=65535` (a sysaudio/kmixer artefact, dead from both sides), `dwFreeHwMixingAllBuffers=0` (correct — GetCaps ran after 64-bit dsound took the single instance), and n50 (summer nodes legitimately have NULL automation; 64-bit tolerates the same `C0000225`). Stage 5bs probe built and staged in `package/` — **user-mode only, no reinstall or reboot** — probing the waveOut driver messages dsound uses before it touches KS. `DSDRIVERDESC` is the prime suspect: it contains pointers, so it is **556 bytes in 32-bit and 576 in 64-bit**, a real WOW64 thunking surface in `SysWOW64\wdmaud.drv`. **RESULT: the probe ran and the hypothesis was wrong - WMP still fails.** Five more theories died: DSDRIVERDESC thunking (`DSOUNDDESC -> NOTSUPPORTED` in BOTH bitnesses, so the struct is never exchanged), device-interface path discovery (identical path, `CreateFile` OK in both), DirectSound enumeration (both enumerate our device), the narrow rate window / missing 8-bit (the codec bitmap `0x000E05E0` genuinely lacks 22.05 kHz and 8-bit, so our advertisement is honest - do NOT widen it), `Wow6432Node` registry redirection (`MediaResources` lives under `SYSTEM`, which is not redirected), and pin exhaustion (`MaxGlobalInstanceCount=1`, but the ledger balances with peak concurrency 1). The two probe logs are byte-identical except three lines. `wmplayer.exe` (PID 2064) is now captured in the SAME log as a working 64-bit DirectSound and its 39-line trace stops at exactly the same place. The 22 rejections of 22050 Hz belong to sndvol32 (16) and explorer (6) - zero from any player. Logs preserved in `logs/stage5bs/`. **STAGE 5bt: the emulation workaround WORKS - with Hardware acceleration set to None in Control Panel, WMP plays.** That exonerates `SysWOW64\dsound.dll` entirely and confines the fault to 32-bit dsound's HARDWARE-accelerated KS path. Two facts finish the localisation. First, `dwMaxHwMixingAllBuffers=1` with `dwFreeHwMixingAllBuffers=0` proves the pin create happens INSIDE `DirectSoundCreate`, so the working 64-bit trace and the failing 32-bit trace are the same API call and the divergence is ONE step wide: after `SpeakerGeometry` SET succeeds, 64-bit issues `Pin id=4 DATAINTERSECTION` and streams, 32-bit sends nothing ever again. Second, the pin-enumeration properties appear on our wire in NEITHER bitness, so dsound learns the pin layout from **sysaudio's cached graph** - and it builds its 216-byte intersection request from exactly the two rates we advertise, proving that dependency. `GetLastError=234` is NOT a clue (it was already killed as theory twelve in 5bp: leftover `ERROR_MORE_DATA` from a normal `Topology id=3` size probe; dsound SYNTHESISES `DSERR_INVALIDPARAM`). Also cleared this stage: pin categories (all `&KSCATEGORY_AUDIO`, same as msvad and ac97), `MaxGlobalInstanceCount=1` (ac97 ships 1 too), and the lowercase `\wave` opens (test tools' own `CreateFile`, not dsound). **Nothing further can be learned from driver logging - every wire observable is identical.** The remaining route needs no hardware round trip: **disassemble `SysWOW64\dsound.dll`** (Ghidra and the JDK are already in `tools/`) and read the predicate it evaluates between the speaker-geometry set and the intersection ioctl. Logs preserved in `logs/stage5bt/`. **Running tally: nineteen theories dead.** **STAGE 5br: n42 IS SOLVED, and the premise it was filed under was wrong.** The Stage 5bq sweep ran. wdmaud's dB conversion is **exact** — `20*log10(V/65535)` to within 0.001 dB over the full 0..-95.25 dB range — so the Stage 5ay belief that "a full drag spans only 0.54 dB" was a mis-measurement and must be discarded. The real bug was one word wide: `BasicSupportStepped` declared `KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL` (which means "one stepping range per channel") while supplying `MembersCount = 1` for a **stereo** node. wdmaud read that as a one-channel node, built a UNIFORM mixer control, and thereafter only ever set `ch=0` — so `ProgramOutputAmp` shows `master L=<slider> R=0` and `amp steps L=<n> R=127` on every single write. **The right speaker sat at 0 dB forever**, which is why the slider was inaudible. Fixed: new `NodeChannelCount` (1 for the genuinely mono `NODE_TOPO_MIC_VOLUME`, 2 otherwise), `BasicSupportStepped` now takes a channel count and writes one range per channel, stereo `DescriptionSize` 72 -> 88. Checked build is clean and **staged in `package/` at 77,312 bytes** (same size as 5bp by section-padding coincidence; verified new by the `channels=%u` string at offset 68786). **INSTALLED AND VERIFIED ON HARDWARE:** `channels=2 asked=88 answered=88`, 90 SETs on each of ch=0 and ch=1 (channel 1 previously got zero), `amp steps L=124 R=124` tracking together, 47 distinct steps across ~35 dB of measured travel, and a clean log (no warnings, no errors; the only failures are the eleven deliberate 22050 Hz rejections). **n42 is CLOSED.** **n49: two more theories dead (thirteen and fourteen).** `Drivers32` is healthy in BOTH views (the 32-bit one is tidier than the 64-bit one), and `MediaResources` is clean (`Speaker Configuration = 0x00140004` = stereo + wide, matching what we program) — so n49 is **not** a registry or redirection problem. New fact: `audiodiag32.exe` **did** successfully create a pin (`MJ_CREATE #40`, `status=00000000`), so 32-bit pin creation works fine and the `kstest32` `KSPIN_CONNECT` size discrepancy cannot explain n49. Loose thread: 64-bit `GetCaps` says `dwMaxHwMixingAllBuffers=1` but `dwFreeHwMixingAllBuffers=0` — check whether the miniport releases its pin instance on close. Evidence preserved in `Backported Driver/logs/stage5bq/` (driver log, both audiodiag logs, the whole regdump) — do not delete it in the item-p cleanup. **STAGE 5bu (latest) - THE MISSING PIECE IS NOT IN THIS DRIVER. This machine is missing `UpperFilters = ksthunk` on the MEDIA device class, and a stock XP x64 installation has it.** `ksthunk.sys` is XP x64's upper filter driver whose service is named, verbatim, "Kernel Streaming WOW64 Thunk Service" - it translates kernel-streaming requests from **32-bit** processes into the 64-bit layout the kernel expects, and Microsoft attaches it to every MEDIA-class device from `wdmaudio.inf`'s `[ClassInstall32.NT]` section. That section is the **class** installer; it is not reachable from `WDMAUDIO.Registration`, which is all our INF pulls in, so no device INF supplies it. **Control experiment, run locally with zero user cost:** the clean XP x64 VM already in this project (`VMs/XPx64/`) has *no audio hardware at all* - no `sysaudio`, `kmixer`, `wdmaud`, `portcls` service and none of those files on disk - yet its `SYSTEM` hive, read straight out of the VMDK with `scratchpad/hive.py`, carries `UpperFilters = REG_MULTI_SZ{ ksthunk }` and a full `Services\ksthunk` key. The class gets registered at OS setup for the five `wave.inf` legacy pseudo-devices, so **every** XP x64 machine has this from day one. The p6-2133w's `logs/stage5bq/regdump/mediaclass.reg` has all six other standard class values, the same five `wave.inf` subkeys - **and no UpperFilters.** The dump is trustworthy here: `reg export` writes `REG_MULTI_SZ` as `hex(7):`. **This corrects Stage 5y**, which asked the user to check that exact value hunting a leftover *hostile* OEM filter, got "no UpperFilters/LowerFilters anywhere", and filed it as *RULED OUT*. The observation was right and the reading was backwards - the absence IS the defect. **It fits every symptom**: 64-bit DirectSound streams (native, no thunk needed), 32-bit DirectSound and WMP fail, and everything that works - system sounds, `sndvol32`, and WMP under Stage 5bt's Hardware acceleration = None - is kernel-mode `wdmaud`/`kmixer` with no 32-bit structure on the KS wire. It explains the thing 5bp found most confusing too: `KSPROPERTY`/`KSP_PIN`/`KSDATARANGE_AUDIO` are **identical sizes in both bitnesses** (szprobe, 5bs), so 32-bit property calls sail through an unthunked stack untouched - eleven of them byte-identical, exactly as logged - while the first structure that actually differs, `KSPIN_CONNECT` (**64 bytes x86, 72 x64**), is reached at the **pin create**, which is precisely and only where 32-bit stops. And the failure is invisible to us because dsound talks to **`sysaudio.sys`**, itself a MEDIA-class device missing the same filter, so the request dies before sysaudio proxies anything down to our dispatch table - which is where nineteen theories went looking. **TEST: `package/fixthunk.cmd`, no rebuild, no reinstall, one reboot.** It reports all three states first, refuses to register the filter unless `ksthunk.sys` is actually on disk (an upper filter that cannot load stops every device in its class with code 39), tries `expand` from the Driver Cache if it is missing, and writes `unfixthunk.cmd`. **Hardware acceleration must go back to Full or the test proves nothing.** If it works, the permanent fix is declarative in the INF - `HKLM,...Class\{4d36e96c-...},"UpperFilters",0x00010002,"ksthunk"` plus an `AddService` - and **not before** it is proved on hardware. Static analysis of `SysWOW64\dsound.dll` was also completed and **withdraws two Stage 5bt claims**: the two dsound builds are **one source drop** (both 2007-02-18, same PDB, same imports, same GUID set), so any divergence is architectural rather than a code lineage difference; and 32-bit dsound has **no `DATAINTERSECTION` call site at all**, so that 216-byte request is **sysaudio's**, not dsound's. What stands: `FUN_73e8722c` is `SetSpeakerConfig` and returns `DSERR_CONTROLUNAVAIL` in **both** bitnesses, so the speaker-config step is cleanly eliminated as the divergence; and one open sub-lead - dsound's pin matcher `FUN_73e850df` requires `KSPROPERTY_PIN_INTERFACES` to return a matching `{KSINTERFACESETID_Standard, ...}` entry while all four of our `PCPIN_DESCRIPTOR`s declare `InterfacesCount = 0`; msvad and ac97 do the same so PortCls presumably substitutes it, but that is unverified. **The Stage 5bt ask to copy `dsound.dll` off the XP machine is obsolete** - 7-Zip reads the VMDK->MBR->NTFS chain directly, everything is in `tools/xpbin/`, and any future "what does a stock XP x64 have here?" question can now be answered locally in seconds. **STAGE 5bv (latest) - CONFIRMED ON HARDWARE. WINDOWS MEDIA PLAYER PLAYS. n49 IS CLOSED AND THE FUNCTIONAL WORK IS DONE.** The user ran `package/fixthunk.cmd`, rebooted, put Hardware acceleration back to **Full**, and WMP played. The proof is at the exact instruction the 5bu theory named: `wmplayer.exe` (PID 3044, 32-bit) now issues `KSPROPERTY_PIN_DATAINTERSECTION` (`set=Pin id=4 in=216`), gets the normal `80000005` size probe, re-asks `out=82`, receives a well-formed 48000 Hz / 2 ch / 16 bit `KSDATAFORMAT_WAVEFORMATEX` - **six times** - then creates six pins on `KSNAME_Pin` (all `status=00000000`), binds converter format `0011`, and RUNs for 637 lines of advancing `AUDIO_POSITION` polls before a clean teardown. **No 32-bit process had ever reached that instruction in this project's history.** The log is otherwise clean: 1310 `STATUS_PENDING`, 588 successes, 58 normal size probes, and every failure is either our own deliberate 22050/100 Hz `ValidateFormat` refusals (14) or something the working 64-bit path also collects (`C0000225` x10 = n50, `C0000034` x6 = sysaudio probing pins with no physical connection). Zero warnings, zero errors, no bugcheck. **Why nineteen theories missed it:** every one asked what this driver was doing differently for 32-bit callers, and the answer was *nothing* - the driver was never asked. The request died in `sysaudio.sys`'s stack, itself a MEDIA-class device missing the same filter, before sysaudio proxied anything down to our dispatch table. **The fix is now declarative in `package/stwrtxp.inf`** - `StwrtXP.Thunk.AddReg` (`UpperFilters`, `0x00010002` = MULTI_SZ|APPEND, which does not duplicate an existing string) plus `AddService = ksthunk,,StwrtXP.Thunk.Service` mirroring `wdmaudio.inf`'s `ksthunk_Service_Inst` including `LoadOrderGroup = PNP Filter`. `ksthunk.sys` is deliberately NOT copied - it is an OS file present on every XP x64 install. **Two INF discoveries:** `Backported Driver/inf/stwrtxp.inf` was **fifteen lines stale** - it never got the Stage 5ax `SubClasses = "wave,midi,mixer"` fix and still said `"wave"`, so installing from `inf/` would have reproduced a solved bug; `package/` was the live copy and both are now identical. And **`.cmd` files delivered to the XP machine must be CRLF** - this repo writes LF-only, and XP-era `cmd.exe` seeks `goto` targets by byte offset. **Release build cut:** `fre`, **25,088 bytes**, MD5 `0672d29c73a37fa190a07d7c3643c33c` - same size as the 9 September build by section-padding coincidence but a different file, verified free of the trace apparatus by byte scan. `package-release/` now holds it with the new INF, a rewritten README and a copy of `fixthunk.cmd` for inspection/recovery only. **All that remains is item p (cleanup) and delivery.**

**STAGE 5bx (latest) - THE PROJECT IS COMPLETE AND CONFIRMED ON THE BUILD THAT SHIPS.** Windows Media Player now plays on the **free** build, on the target machine, installed from the INF. The Stage 5bw “regression” was not a regression: the user installed `package-release/` and did not reboot. PnP reads a class `UpperFilters` value when it **builds** a device stack, so a driver updated through Device Manager can run with the `ksthunk` registration perfect but the filter **not attached** - and the symptom of that is exactly the symptom ksthunk was added to cure (64-bit audio fine, system sounds fine, WMP says “There was a problem with your sound device”). A reboot fixed it. **Record correction:** the Stage 5bv proof is a *driver log*, and only the checked build can write one, so WMP had only ever been confirmed on `chk`; the release README's claim that it worked on `fre` was carried across on the strength of a source audit, not a test. The audit was right (the entire project contains **4** `#if (DBG)` blocks, all of them the logging itself; **6** `ASSERT`s, none with a call inside; and no raw MMIO - every hardware access goes through the HD Audio bus interface or `READ_REGISTER_ULONG`, so nothing is exposed to `/O2`) but “identical by inspection” is not “tested”, and it is now tested. **`fixthunk.cmd` fixed:** it conflated “UpperFilters exists” with “ksthunk is registered” and asked for manual help when the answer was “nothing to do”; it now detects `ksthunk` inside the `REG_MULTI_SZ` and says so, pointing at the reboot. Validated by running it against a machine in the identical state - new branch taken, zero writes. **`MD5` is a FILE identity, not a SOURCE identity:** WDK 7600 stamps a build timestamp into the PE header, so identical source rebuilds to a different hash every time (this stage produced three distinct 65,536-byte `chk` binaries). Final artefacts: `fre` **25,088** MD5 `637eaa9056f05f077fa7caea9617c18a` (unchanged - this is the confirmed one), `chk` **65,536** MD5 `22889b8dcb5034e75ce14dc87a8dcfd5`, `stwrtxp.inf` **11,556** MD5 `27305dadcfe018be3f377730b8f610d5`, `fixthunk.cmd` **8,712** MD5 `f987c29a96079161a19d3797bbab03cc`, `IDT Audio Backported Driver (Release).zip` **137,475**. Both builds zero-warning. <<<**

**(SUPERSEDED by the banner above - kept because its defect finding stands) STAGE 5bw: ITEM p IS DONE. THE PROJECT IS COMPLETE.** The cleanup pass removed every diagnostic from the source (both dispatch hooks, `DumpDeviceSecurity`, the `StopEngine` DMA scan, the whole Stage 5ba tone apparatus) and dialled `ulDebugOut` back to `DBG_DEFAULT`. **It also found a real defect:** the previous release build *physically contained* the 689 Hz sine table, because `g_HdaTestTone`, `g_ToneTable` and the `CopyTo` branch were never `#if (DBG)`-gated - only the registry read was. It could not fire (the flag stayed zero), but it shipped, and it is now gone from the source. New builds, both zero-warning: `chk` **65,536** (was 77,312) MD5 `9ffb38f221b9c2b3c815441a95d021cd`, `fre` **25,088** MD5 `637eaa9056f05f077fa7caea9617c18a` - the third release in a row at 25,088 bytes and all three different files, so **use the MD5, never the size**. `Backported Driver/inf/` is deleted (the Stage 5bv stale trap; the INF is now maintained in `package/` and copied to `package-release/`), the diagnostic binaries are out of `package/`, all tool sources are consolidated in `Backported Driver/tools/`, and the stale `(Near Release).zip` - which held the pre-ksthunk INF *and* the tone-carrying `.sys` - is replaced by **`IDT Audio Backported Driver (Release).zip`**. Only optional work remains: an end-to-end INF install test, and cosmetic `Drivers32` tidying. <<<**

**(SUPERSEDED by the banner immediately above — kept for history) SESSION HANDOFF (2026-09-08, later session): the Stage 5ai bluescreen is FIXED BY REMOVAL. `FixPdoSecurity` — the `ObOpenObjectByPointer`/`ZwSetSecurityObject` runtime DACL rewrite that caused the `SYSTEM_SERVICE_EXCEPTION` — has been deleted from `adapter.cpp`, so the current build is safe to install on the real machine again. It was replaced by two much safer attacks on the same Stage 5af root cause, both shipping in the same build: (1) `RelaxPdoSecureOpen()`, which clears `FILE_DEVICE_SECURE_OPEN` on the PDO — a single `ULONG` field write, no APIs, no handles, cannot bugcheck; and (2) the INF's new `[StwrtXP.Install.NTamd64.HW]` section carrying a `Security` SDDL, which is the only documented INF mechanism for setting a PDO's security descriptor and had never been tried (Stages 5q/5r used the interface key and the software key instead — neither is ever read back as a device-object SD, which is why both retests showed nothing). Built clean (`chk`, 9 files, 1 pre-existing warning) and staged together at `Backported Driver/package/`. **Never reinstate `FixPdoSecurity`.** Next action is a real-hardware retest — see Next Steps item n27 for what to collect and the three-way fingerprint table for reading the result; the two fixes leave distinguishable traces, so this is not a confounded experiment. The Stage 5ai bugcheck data was never collected and is no longer needed. <<<**

## Goal

Get working audio (playback at minimum, ideally recording + basic mixer controls) on the target machine under 64-bit Windows XP, by reverse-engineering the closed-source Vista-era `stwrt64.sys` (no source code available) and reimplementing its codec-control logic as a new XPDM-model (WaveCyclic/WavePci) miniport driver, packaged with its own XP-compatible INF.

## Target Hardware

- Machine: HP Pavilion p6-2133w
- Target OS: Windows XP, x64 edition
- Audio codec: **CONFIRMED** via Device Manager Hardware ID on the real p6-2133w:
  `HDAUDIO\FUNC_01&VEN_111D&DEV_76C7&SUBSYS_103C2ACD&REV_1001`
  (exact match for one of the entries already in `STWRT64.INF`.)
  - `VEN_111D` = IDT (Integrated Device Technology, later Tempo Semiconductor / part of the old Sigmatel lineage).
  - `DEV_76C7` = **IDT 92HD89E2** codec. Confirmed via Linux kernel's `sound/pci/hda/patch_sigmatel.c` (ALSA `CONFIG_SND_HDA_CODEC_SIGMATEL`), which maps HDA codec vendor/device ID `111d76c7` to codec name "92HD89E2".
  - **This is a major reference discovery**: `patch_sigmatel.c` is open-source (GPL) Linux driver code that already fully implements verb sequences, pin-complex/power-up logic, and quirks for this exact codec. HD Audio's verb-level protocol is standardized regardless of OS, so this Linux source is a much more reliable reference for Stage 3/4 than disassembling the closed `stwrt64.sys` binary. Plan: treat `patch_sigmatel.c` (and the generic HDA "auto parser" it uses, `hda_generic.c`/`hda_auto_parser.c` in the same ALSA tree) as the **primary reference** for codec init/verb logic; use Ghidra disassembly of `stwrt64.sys` only as a secondary check for HP/IDT-specific quirks (e.g. HP's specific pin configuration, mute LED/EAPD behavior) not visible from the generic Linux auto-parser path.

## Key Facts Established

- **This is not a WDDM-vs-XPDM (graphics) issue** — WDDM is a graphics driver model. The relevant mismatch is audio **port-class model**: the original driver uses **WaveRT** (introduced in Vista), while XP's audio stack only supports the older **WaveCyclic** / **WavePci** port classes.
- The original package (`Original Driver/`) is IDT's HD Audio driver v6.10.6486.0 (2013), for Vista/7/8+.
- `Original Driver/WDM/stwrt64.sys` is a **PE32+ (64-bit only)** binary — confirmed via file header ("PE32+ executable for MS Windows 6.00 (native), x86-64"). No 32-bit build exists anywhere in the package.
- `Original Driver/WDM/STWRT64.INF` only has an `[IDT.NTamd64]` install section (no NTx86 section) — this driver was never even offered for 32-bit Windows.
- INF confirms it as `"IDT High Definition Audio WaveRT Driver"` explicitly.
- **There is no source code anywhere in the package** — only compiled `.sys`/`.dll`/`.exe` binaries plus data/config files (`Presets.bin`, `C-A1.INI`...`C-F3.INI`, `stwrt64.ini`, `DTS_TOWER.INI/.XML`, `EQ*.INI`). These INI/bin files may contain codec preset/verb data directly, which could reduce how much raw disassembly is needed for verb tables specifically.
- **Windows XP has no inbox HD Audio bus driver.** Vista added `HDAudBus.sys` inbox; on XP, HD Audio enumeration requires Microsoft's UAA (Universal Audio Architecture) bus driver package (KB888111) installed as a prerequisite layer *before* any codec function driver (including our new one) can bind to anything.
- The last WDK/DDK release with confirmed XP-era `NTAMD64` kernel driver build support is **WDK 7600.16385.1** ("WDK 7.1.0"). Newer WDK/EWDK releases drop XP targeting — do not assume they'll work for this project.
- `Backported Driver/` is currently empty — organizational home for all new driver source/INF output, per user's request to keep the original and backported files separated.

## Current Stage

**>>> MOST RECENT STATE (read this first; ignore the stale paragraphs below): see "Stage 5bb" and Next Steps items n36/p near the end of this file. Short version: **Stage 5ax's mixer-subclass registration WORKED and was not enough.** With `Drivers,SubClasses = "wave,midi,mixer"` in place, Control Panel lists the device, Windows lets you play sounds, and the log shows the first non-kstest `KSNAME_Pin` creates in this project's history (PIDs 472 and 1752, two complete playback sessions, 497 `IOCTL_KS_WRITE_STREAM`). But **system playback is silent while kstest is audible on the same boot**, and the volume sliders are greyed out. Diffing the silent system run against the audible kstest run found them identical at every observable level — md5-identical codec verb stream, same stream lifecycle, same write-IOCTL shape — which leaves **buffer content** as the only unobserved variable, i.e. kmixer is most likely delivering silence because the mixer device it built has no volume control to source a gain from. **Stage 5ay is the fix for that and for the dead sliders in one change:** the topology filter now carries real `KSNODETYPE_VOLUME` / `KSNODETYPE_MUTE` nodes (7 nodes, 9 connections, msvad shape) backed by the codec's own output amplifier through new `IHdaAdapterCommon` gain methods, and a temporary `StopEngine` DMA-buffer scan settles the silence question outright either way. Built clean (`chk`, 9 files, 0 errors); `stwrtxp.sys` is now **60928 bytes** (was 51200). **TESTED (Stage 5az): the volume nodes WORK — sliders live, Control Panel sees the device, n36 done — but the DMA scan came back showing the buffer FULL OF LOUD AUDIO (`peak |sample| = 22679`), which disproves the kmixer-silence diagnosis outright.** Stage 5az solves it from the numbers the scan returned: PortCls fills a WaveCyclic buffer **behind** the play cursor, so a stream's latency is one whole buffer lap; `HDA_MAX_DMA_BUFFER_SIZE` was `0x16000` — copied from the purely virtual msvad sample — which makes the lap 1.02 s at 22.05 kHz and 511 ms at 44.1 kHz, and across five independent playback sessions **every one wrote less than one lap before `RUN -> PAUSE`** (94%, 86%, 84%, 45%, 38% of the buffer, never 100%), so the hardware played the initial zeros and never reached the audio. kstest was audible throughout only because a continuous tone outlives the first lap. The buffer is now `0x4000` (85 ms at 48 kHz, 93 ms at 44.1 kHz, 186 ms at 22.05 kHz), `SetFormat` logs the format it was asked for before anything can reject it, and a rejected format now rolls back instead of leaving a dead pin. Built clean; `stwrtxp.sys` is **62976 bytes**. **TESTED (Stage 5ba): the buffer now fills end to end over ~23 laps, and the sound is still inaudible — because the audio arriving in it is 40-50 dB too quiet (peak 103 and 23 of 32767, against kstest's -12 dBFS).** Everything else checks out: the link position is the real LPIB (it holds across a pause, so the DMA engine genuinely runs), and every codec output verb is correct at load. **TESTED (Stage 5bb): the substitute test tone was SILENT — peak 16384, RMS 11585 in the buffer, five sessions, no sound — so amplitude is not the fault and that theory is withdrawn.** The real discriminator is **sample rate**: every audible stream ever produced here was 44.1 kHz and every silent one 22.05 kHz. The driver advertised 8-48 kHz without ever querying the codec's supported-rates bitmap, so kmixer sent XP's 22.05 kHz system sounds straight through. Stage 5bb queries `PARAMETER 0x0A` and clamps the advertised range to 44100..48000. **TESTED: the codec confirmed it — `0x0A = 0x000E05E0`, bit 3 clear, 22.05 kHz genuinely unsupported — and the clamp still did not stop 22050 reaching `SetFormat`, because a `KSPROPERTY_CONNECTION_DATAFORMAT` set is not validated against the pin's data ranges by PortCls or by anything below it.** Stage 5bc adds `CMiniportWaveCyclicHda::ValidateFormat` and calls it at the top of `SetFormat`, before the engine is released, so an unplayable rate is refused without disturbing a stream that is already working. `stwrtxp.sys` is **69120 bytes**. **TESTED AND WORKING — this is the build that broke the silence.** Fourteen refusals of 22050 Hz, fourteen streams kept at 48000 Hz, audible output on real hardware. The only application still quiet is VLC, which the log shows reaching the driver and getting everything it asked for — that is n46 and it starts in user space. The security investigation (Stages 5n-5aj), the `0x3B` use-after-free (5ak), the divide-by-zero (5an), the converter binding (5ao), the DMA-engine leak (5ap/5ar/5as), the sysaudio bugcheck (5au), the cross-filter physical connection (5aw), the mixer subclass (5ax) and the topology volume/mute nodes (5ay) are all FINISHED and must not be re-litigated. **n40 is withdrawn — the `ReleaseEngine`/`AllocateBuffer` ordering bug does not exist; that reading confused DbgPrint print order with call order.** **Never reinstate `FixPdoSecurity`** — it is a confirmed, reproducible bluescreen and has been deleted from the source. <<<**

**(SUPERSEDED by the Stage 5ay block immediately above — kept for history) EARLIER STATE: see "Stage 5aw" and "Stage 5ax" and Next Steps items n39/n36/p near the end of this file. Short version: **the render path is finished and proven** — the driver plays clean, correctly pitched, glitch-free audio on real hardware, repeatably, and returns every DMA engine it borrows. The sysaudio bugcheck is fixed and confirmed (5au). What is left is that Windows still shows no audio device. **Stage 5ax read the Stage 5aw log and it is conclusive in both directions.** Positive: `PropertyHandler_ComponentId` returns `00000000` on both filters, `PropertyHandler_PreferredStatus` is reached and logs `Enable=1` for DeviceType 1 and 2 (`PLAYBACK_DEFAULT` and `RECORD_DEFAULT`), and the process doing that is **user-mode PID 480**, which found `\Wave` on its own — so the device interface is discoverable and the system considers it the preferred device. Negative: diffing sysaudio's PID-4 enumeration between the old and new driver gives **118 lines each, 2 lines different, and those 2 are exactly the two `COMPONENTID` statuses.** sysaudio asks the same questions in the same order and stops in the same place. `KSPROPERTY_GENERAL_COMPONENTID` was a real gap, is correctly closed, and was never the gate. **Every size the trace returns decodes exactly right on both filters, so the fault is not in anything PortCls exposes — it is above PortCls.** Stage 5ax also withdraws a Stage 5aw claim: pin creates from PID 1704 happen under the OLD driver too (that log had been truncated mid-load), so PID 1704 is kstest and pin creation was never a success signal. The one surviving divergence from `msvad.inf` is `Drivers,SubClasses = "wave"` where every WDM audio driver says `"wave,midi,mixer"`, with no `Drivers\mixer\wdmaud.drv` registration at all — and XP's "no audio device" message comes off the mixer device. **The INF is fixed (8375 bytes); `stwrtxp.sys` is UNCHANGED at Stage 5aw's 51200 bytes because no code changed.** Because it is registry-only it can be tested with `package/quicktest-5ax.reg` plus a reboot instead of a reinstall — that is n39. Then n36 (volume/mute nodes driving the codec's amp verbs), which may be required together with the mixer registration, and item p (dial `ulDebugOut` back from `DBG_ALL`, remove BOTH diagnostic hooks, ship a `fre` build). The security investigation (Stages 5n-5aj), the `0x3B` use-after-free (5ak), the divide-by-zero (5an), the converter binding (5ao), the DMA-engine leak (5ap/5ar/5as), the sysaudio bugcheck (5au) and the cross-filter physical connection (5aw) are all FINISHED and must not be re-litigated. <<<**

**(SUPERSEDED by the Stage 5ak block immediately above — kept for history) EARLIER STATE: see "Stage 5aj" and Next Steps item n27 near the end of this file. Short version: the Stage 5ai bluescreen is resolved by deleting the code that caused it, rather than by debugging it — setting another driver's PDO security descriptor from a function driver is not a supported operation, and `SYSTEM_SERVICE_EXCEPTION` is exactly what an unsupported `ZwSetSecurityObject` on a device-object handle looks like. The Stage 5af root-cause finding is unchanged and still the leading explanation: the bus-owned PDO carries `FILE_DEVICE_SECURE_OPEN` plus a *populated* zero-ACE deny-all DACL, and our KS device interfaces resolve to that PDO, so every `CreateFile` of the filter path is denied before an IRP is ever built. (That finding was re-verified this session — `DumpDeviceSecurity()` distinguishes absent, NULL and empty DACLs on separate code paths, so "DACL has 0 ACE(s)" really does mean deny-all, not a misread diagnostic. Do not re-litigate it.) What was wrong was only the *fix*. Two safer replacements now ship together: a runtime `FILE_DEVICE_SECURE_OPEN` clear, and the documented `[DDInstall.HW]` `Security` INF entry. Built and staged; awaiting a real-hardware retest. <<<**

**STALE, superseded by Stage 5ac/5ad above: Short version: the `DumpDeviceSecurity` dump (Stage 5ab) came back — and it's a DEAD END, not a lead: our device object's DACL is fully permissive (`Everyone`/`S-1-5-18` LocalSystem/`Administrators`/`S-1-5-12` Restricted Code all granted broad access, e.g. Administrators has `0x001F01FF`, effectively full control), yet `kstest.exe` (running as Administrator) still gets `ACCESS_DENIED` on everything, including zero-access opens. **Also notable: only ONE device object (`dev#0`) was found on our driver's device list at all** — meaning `PcRegisterSubdevice` for Topology and Wave does NOT create separate `DEVICE_OBJECT`s linked into our `DriverObject->DeviceObject` chain; both subdevices are multiplexed through the single FDO via named sub-opens (matches the `MJ_CREATE` log's `Name="\Topology"`/`Name="\Wave"` `FileObject->FileName` values exactly). This reframes the mystery again: with the device object's own SD proven irrelevant, the remaining live theory is that KS's own per-filter-factory security (a check tied to the *named sub-object* "\Topology"/"\Wave", separate from and in addition to the device object's DACL) is what's denying access — not the Object Manager's device-object-level `SeAccessCheck` after all. Added a new user-mode diagnostic to `kstest.c`/`kstest.exe`: before opening the full filter path, it now also tries a zero-access `CreateFile` on the **bare** device-interface path with the trailing `"\wave"`/`"\topology"` reference-string suffix stripped off — if that succeeds, it proves the base device object is reachable/open-able and the denial is specific to the named sub-object; if it also fails, the base object is unreachable for some other reason despite its permissive SD. Rebuilt clean (`fre`, via the `C:\stwrtxp_kstest` junction — building from the real project path under `C:\path\to\stwrtxp` fails with WDK's `nmake` error `U1087` on paths containing spaces, same class of issue that motivated the driver's own `C:\stwrtxp_src` junction), sent to the user — **not yet retested on real hardware.** No driver rebuild was needed for this round (`stwrtxp.sys` unchanged). See "Stage 5ac" for the full writeup. <<<**

**Stage 5, WaveCyclic rewrite (STALE — superseded by Stage 5n onward, kept for history): two INF-only fixes retested and both ruled out; pivoted to a raw-KS-level test (KsStudio) to localize whether the fault is in our filter/topology or in `sysaudio`'s discovery layer — awaiting that result before the next code change.** The wave miniport has been converted from `IMiniportWavePci` to `IMiniportWaveCyclic` (see "Stage 5f" below for full rationale and design). First real-hardware test of this build installed cleanly (Device Manager "working properly") but produced no audio and no log — root-caused to the shipped build being a `fre` (free/retail) build, which compiles all `DOUT`/log-to-file diagnostic code out entirely; see "Stage 5h" below. Switched to a `chk` (checked) build to get real diagnostics — **always use `chk` for on-hardware diagnostic sessions on this project going forward**, `fre` only once things actually work end-to-end. The resulting log showed codec/widget init fully succeeding but nothing beyond it, and the user reported no "audio specific settings" appear in Windows at all. Root-caused (via direct comparison to the WDK's own `msvad\simple` sample) to `mintopo.cpp`'s topology filter never declaring a real physical-jack pin category (`KSNODETYPE_SPEAKER`/`KSNODETYPE_MICROPHONE`) — without one, sysaudio never recognizes a render/capture endpoint exists at all, even though the KS filter itself builds without error. **Fix applied** (new speaker/mic pins + connections in `mintopo.cpp`) **and diagnostic instrumentation added** to `wavecyclicminiport.cpp`/`mintopo.cpp`/`adapter.cpp` (previously zero `DOUT` calls in the topology/wave-miniport layers — this was a real blind spot). Rebuilt clean (`chk`, zero errors). **Retested twice — root cause now found.** Round 1 (Stage 5i): confirmed `StartDevice`, both miniports' `Init()`, and both `PcRegisterSubdevice`/`PcRegisterPhysicalConnection` calls all succeed with zero errors, but `NewStream` still never called; user reported Device Manager says "working properly" while Control Panel's Audio tab says "no audio device" — PnP/KS layer fully healthy, legacy multimedia layer sees nothing. That combination pointed at INF-level registration rather than the KS pin/topology graph. Round 2 (Stage 5j): diffed `stwrtxp.inf` against the real WDK `msvad.inf` sample and found it — **our INF never set up `AssociatedFilters`/`Drivers\wave\wdmaud.drv` registration at all**, so `wdmaud.drv` (the legacy filter that backs waveOut/waveIn and the Sounds and Audio Devices applet) never attached to the device, and our `AddInterface` lines had no reference string binding each KS category to a specific named subdevice. **Fixed** (pure INF change, no `.sys` rebuild needed) — see "Stage 5j" below. **Not yet retested on real hardware — this is the immediate next concrete action**, and given how cleanly it explains every symptom across both rounds, this is the strongest lead so far.

**Stage 5, real-hardware track (historical): CODE 10 IS RESOLVED.** The driver now installs cleanly on the real p6-2133w with no crashes and no Device Manager errors: `AcquireBusInterface` succeeds, the codec (Vendor 111D, Device 76C7) is found, all 35 widgets are discovered under the AFG, and all 5 output pins (NIDs 10, 13, 15, 16, 17) initialize successfully. See "Stage 5e: Code 10 resolved — codec fully initializes on real hardware" below for the full log walkthrough. **There is still no audible sound** — this is expected, not a bug: the log's last line explicitly states stream servicing is stubbed pending the DMA rewrite. Stages 1-3 are complete (codec identified, tooling installed, `stwrt64.sys` reverse-engineered — see recaps below). Stage 4's original driver skeleton (described in "Stage 4 progress" below) was written against an **incorrect architectural assumption** (that this codec function device directly owns the HDA controller's MMIO BAR/IRQ, AC'97-style) — this was empirically disproven on the real p6-2133w and has now been fixed by a significant rewrite. **See "Stage 5: CRITICAL — Code 10 root cause found, HDAUDIO_BUS_INTERFACE rewrite" below before trusting anything in "Stage 4 progress" about `common.cpp`'s MMIO/CORB-RIRB verb transport or `adapter.cpp`'s `ValidateResources` — both sections describe the now-superseded design.** See "Stage 5b", "Stage 5c", "Stage 5d" below for the three crash investigations (all fixed). **Next concrete action: implement the real DMA/stream engine** (`wavepcistream.cpp`/`wavepciminiport.cpp`) against `HDAUDIO_BUS_INTERFACE`'s `AllocateRenderDmaEngine`/`AllocateCaptureDmaEngine`/`AllocateDmaBuffer`/`SetDmaEngineState`/`GetLinkPositionRegister`/`GetWallClockRegister`/`RegisterEventCallback` — this is the only remaining blocker to actual audio output.

### Stage 5e: Code 10 resolved — codec fully initializes on real hardware

Real-hardware retest after the `Version = 0x0100` fix (Stage 5d). Result: driver installed properly, no crash, no Code 10. Full `stwrtxp_log.txt` walkthrough (note: this file is append-mode across driver loads — lines 1-3 are the stale pre-fix run from Stage 5d, real current run starts at line 4):

- Line 4: `AcquireBusInterface` succeeds — codec address 0, function group start node 1.
- Lines 5-6: codec vendor/device ID verb returns `111D76C7` — confirms we're talking to the right chip (IDT 92HD89E2) over the real bus interface.
- Lines 7-92: full verb-based widget enumeration (pin configs, output/input amp caps, connection lists) across NIDs 0-44 — this is `DiscoverWidgets` walking the AFG.
- Line 93: `Discovered 35 widgets under AFG NID 1`.
- Lines 94-139: per-pin init sequence for the 5 output pins (NIDs 10, 13, 15, 16, 17) — power state set to D0, EAPD/pin-widget-control enabled, connection select set, gain/mute verbs applied. Each pin logs `Initialized output pin NID <n> (device type <t>, config <cfg>)` with no error codes anywhere in this block.
- Line 140: `Init: no owned interrupt/MMIO resource - skipping ISR registration. Stream servicing is stubbed pending the HDAUDIO_BUS_INTERFACE-based DMA rewrite - see HANDOFF.md` — this is a deliberate, expected log line from the current code (not an error), confirming `StartDevice`/`Init` completed successfully end-to-end.

**Conclusion**: the entire PnP start / bus-interface acquisition / codec bring-up / widget discovery / pin init pipeline is now verified working on real hardware. No further BSODs, no further Code 10. The reason there's still no audible sound is exactly what line 140 says: the WavePci stream/DMA layer (`NewStream`, stream descriptor register programming, actual `IMiniportWavePciStream` buffer servicing) has not been implemented yet — it was stubbed out during Stage 4/5 while the bus-interface/codec-init work was the priority. That implementation is the next task; it does not require any further Code-10/crash debugging, just new feature code against the now-confirmed-working `HDAUDIO_BUS_INTERFACE`.

**Not yet done**: implement DMA engine allocation and stream start/stop/format-set logic in `wavepcistream.cpp`/`wavepciminiport.cpp` using the bus interface's DMA-related function pointers, then retest for actual audio output on the real machine.

### Stage 5: two-track testing plan

**Important distinction, clarified with the user**: a VM cannot be the real test target for this driver. VMware's emulated HDA controller exposes its own virtual codec (not the IDT 92HD89E2), so verb-level correctness and actual audio output can only be validated on the **real HP Pavilion p6-2133w**. The VM is a cheap first-pass sanity gate only — catching driver load failures/bugchecks/INF binding problems before ever touching the real machine, and giving a WinDbg-friendly kernel-debug target (serial named-pipe transport, no cable needed) for the class of bugs that don't need real HDA hardware to reproduce (init/IRP-handling/PnP-level crashes).

Two tracks going forward:
1. **VM sanity pass** (`VMs/XPx64/XPx64.vmx`, VMware Workstation) — install XP x64 fresh in the VM (in progress, see "VM setup" below), then copy the built driver + INF over and confirm it loads without a bugcheck and without device-manager code 10/39, using the VM's fake HDA/codec as the PnP target. WinDbg attaches via the VM's serial pipe (`\\.\pipe\com_1`, configured as a named-pipe server) — no physical debug cable needed for this track.
2. **Real-hardware pass** (the actual target) — **the user already has Windows XP x64 installed on the p6-2133w itself**, so real testing is just: copy the built `stwrtxp.sys` + INF onto a USB drive, move it to the real machine, and install there. This is the only track that can confirm actual codec verb correctness / real sound output. **Open item**: kernel debugging *on the real machine* (as opposed to the VM) will need a physical transport — Windows XP has no KDNET/network debug transport (that's a Vista+-era addition), so it requires either a serial null-modem cable or a 1394/FireWire link to a separate debug host. Not needed for a first driver-install attempt (can eyeball Device Manager status + try to make sound come out first), but flag this as a follow-up if/when a real-hardware bugcheck needs live kernel debugging.

### Stage 5: VM setup

Created `VMs/XPx64/XPx64.vmx` (VMware Workstation, already installed on this machine) + a 40GB growable SCSI (`lsilogic`) disk. Config highlights: `guestOS = "winxppro-64"`, 1 vCPU / 2GB RAM, CD-ROM mapped to `tools/installers/Microsoft Windows XP x64.iso`, NAT networking (`e1000` NIC — chosen for guaranteed inbox XP driver support during install), virtual sound device set to `hdaudio` (VMware's emulated Intel HDA controller — closest available stand-in for basic PnP/load testing, though its codec differs from the real 92HD89E2, see above), and a serial port pre-wired for WinDbg (`serial0` as a named-pipe server on `\\.\pipe\com_1`).

One config hiccup hit and fixed: an initial `ehci.present = "TRUE"` line caused VM start to fail ("Cannot assign available PCI slot to 'ehci'... no more PCI slots available") — removed it (not needed; `usb.present = "TRUE"` alone already provides a UHCI controller) and the VM powered on cleanly on retry.

Product key for the ISO was kept in a local file outside the repository — **never commit a product key or an OS image to a shared or public location.**

**Status as of this update**: VM is powered on and sitting at the XP x64 installer (booted from the mounted ISO) — the actual OS installation is an interactive GUI process the user needs to click through themselves (partitioning, product key entry, initial boot config); not yet confirmed complete.

### Stage 5: VM sanity pass abandoned — VMware won't expose the emulated HDA controller to this guest

The OS install completed and the user set up the rest of the VM environment themselves: VMware Tools (legacy-guest ISO, since this Workstation version doesn't bundle one for XP-class guests — user supplied `tools/installers/winPreVista.iso`, mounted as the CD image; audio component deliberately deselected during Tools install so it wouldn't claim the HDA device before our driver could), a shared folder (`Backported Driver/` mapped read/write to a guest `Z:` drive), and WinDbg kernel debugging over the pre-wired serial named pipe (`\\.\pipe\com_1`) — required a corrected `bootcfg /debug ON /port COM1 /baud 115200 /id 1` (this XP build's `bootcfg` needs an explicit `/id` boot-entry number, which the first attempt omitted).

**Incident along the way**: a `vmrun stop soft` command (issued to hot-swap the mounted CD image) executed with a delay and powered the VM off mid-way through the user's interactive VMware Tools install, corrupting that install. The user redid the OS install, VMware Tools, and shared-folder setup entirely themselves afterward, and took their own snapshot as a safeguard. **Standing behavioral correction from this**: do not issue any VM power-state command (start/stop/reset) once the user has indicated they're doing something manually inside the VM — give manual instructions instead, and treat any already-in-flight power command as still able to cause harm if the user says to stop, not just re-check it once and assume it's fine.

**The actual blocker**: with `sound.virtualDev = "hdaudio"` set and VMware itself reporting the virtual sound card as connected, the emulated HD Audio controller **never appeared anywhere in the guest** — not under Sound/audio device classes, not as an unrecognized/unknown device even with hidden devices shown, and not in `msinfo32`. The VM's own log (`vmware.log`) confirms `HDAudio: HDAudioConnectDisconnect: primary backend: wave, fallback backend: dummy` — i.e. VMware's host-side audio backend initializes fine — but that's inconclusive about whether the guest is actually shown the PCI function via the virtual BIOS/ACPI tables. Tried the theory that VMware Workstation gates `hdaudio` emulation to guest OS types it considers "modern" (Vista+), using the fact that XP x64 and Windows Server 2003 x64 share the identical NT 5.2 kernel/HAL to relabel the guest as `winnetstandard-64` without changing anything real about the installed OS:
- A direct `.vmx` text edit of `guestOS` got silently reverted by VMware itself before the next power-on (its internal per-VM metadata evidently overrides a raw text edit).
- Redone via the Workstation GUI (**Settings → Options → General → Guest operating system** dropdown) instead, which *did* stick (confirmed still `winnetstandard-64` in the `.vmx` after a reboot) — but the HDA controller still never showed up in the guest.

**Decision (with user): stop debugging VM audio visibility and move directly to the real-hardware track.** The VM was only ever meant as a cheap first-pass sanity check (driver loads/binds without bugchecking) — not codec validation, which only the real hardware can provide anyway. Given how much time this specific VMware quirk has consumed (Tools ISO gap, the accidental-shutdown incident, two failed guest-relabel attempts) for a check that was never going to validate real codec behavior regardless, it's not worth continuing to chase. The VM (with its WinDbg pipe already working) remains available if a future non-codec-specific bug (e.g. a PnP/init crash reproducible without real HDA hardware) needs a cheap repro target — just not for anything sound-hardware-visibility-dependent.

### Stage 5: CRITICAL — Code 10 root cause found, HDAUDIO_BUS_INTERFACE rewrite

**First real-hardware install attempt (per the "real-hardware track" Next Step from the previous update) failed with Device Manager Code 10.** Diagnosed using the existing `LogToFileF`/`DOUT` file-logging mechanism (see `debug.h` — no live kernel debugger available on this real XP x64 machine; `ulDebugOut` was already turned up to `DBG_ALL` for exactly this kind of diagnosis, see debug.h's comment). The log (`C:\stwrtxp_log.txt` on the real machine) showed exactly one line:

```
ValidateResources: expected at least 1 memory resource, got 0
```

**Root cause**: the entire original Stage 4 design (`common.cpp`'s direct MMIO mapping + CORB/RIRB verb transport + controller reset, `adapter.cpp`'s `ValidateResources` requiring ≥1 memory resource + ≥1 interrupt) was modeled on the wrong mental picture of HD Audio, carried over uncritically from the AC'97 sample. **Under the real HD Audio/UAA architecture, the PCI HDA controller's MMIO BAR and IRQ belong to the bus driver (`HDAudBus.sys`, from the UAA package), not to the codec.** The bus driver enumerates each codec as a **child PDO** (`HDAUDIO\FUNC_01\...` — exactly the device node our INF binds to) that is a purely logical device and legitimately receives **zero PCI resources of its own**. This is a hard architectural fact confirmed empirically on the real hardware, not a bug in resource assignment — `ValidateResources` was correctly reporting what PnP handed it; the driver's expectation was simply wrong.

**Fix**: replaced the codec function driver's verb-transport/controller-management layer with the real WDM mechanism for a bus-child device: **`HDAUDIO_BUS_INTERFACE`** (from the WDK's `<hdaudio.h>`, GUID `GUID_HDAUDIO_BUS_INTERFACE`), obtained via a synchronous `IRP_MN_QUERY_INTERFACE` sent to the lower (bus-owned) device object.

**Files changed**:
- **`common.h`** — `CHdaAdapterCommon` no longer holds any MMIO/CORB/RIRB state (`m_pMmioBase`, `m_pDmaAdapter`, CORB/RIRB ring fields, verb-retry/timeout fields all removed). New state: `HDAUDIO_BUS_INTERFACE m_BusInterface` + `m_bBusInterfaceAcquired`. New private helper `AcquireBusInterface(PDEVICE_OBJECT)` replaces the old `MapControllerResources`/`ResetController`/`FindCodec`/`SetupCorbRirb`/`DoubleResetHandshake` quintet. `GetMmioBase()` now **always returns NULL** (kept as a stub method — its callers are in the not-yet-rewritten stream/DMA code, see below — with a comment pointing at this section).
- **`common.cpp`**:
  - New `AcquireBusInterface()`: `IoGetLowerDeviceObject()` to get the bus-owned lower device object, builds and sends a synchronous `IRP_MN_QUERY_INTERFACE` IRP (`IoAllocateIrp` + a non-paged completion routine signaling a stack `KEVENT`, since `IoCallDriver` may complete from a DPC) requesting `GUID_HDAUDIO_BUS_INTERFACE`, then calls the interface's `GetResourceInformation()` to learn `m_CodecAddress`. `IoGetLowerDeviceObject` is an `ntifs.h` routine that can't be `#include`d alongside the existing `ntddk.h`/`wdm.h`-based include chain (macro/type conflicts) — declared its prototype directly instead (`extern "C" NTKERNELAPI PDEVICE_OBJECT IoGetLowerDeviceObject(PDEVICE_OBJECT);`) since it's still exported by `ntoskrnl` and safe to call (present since Windows Server 2003, confirmed linking clean against `wnet`/amd64).
  - `SendVerb()` rewritten to build an `HDAUDIO_CODEC_TRANSFER` and call `m_BusInterface.TransferCodecVerbs(...)`, synchronized the same way (non-paged completion callback + stack `KEVENT`) instead of the old CORB-write/RIRB-poll loop. **Bug caught by the first build attempt, now fixed**: `HDAUDIO_CODEC_COMMAND` has no direct `CodecAddress`/`Node` members — those fields only exist inside its `Verb8`/`Verb16` nested sub-structs (`transfer.Output.Verb8.CodecAddress`/`.Verb8.Node`, not `transfer.Output.CodecAddress`/`.Node`) — this compiled clean only after correcting to the real struct layout in `<hdaudio.h>`.
  - `BringUpCodec()` (still does the vendor/device-ID confirmation) no longer calls a reset handshake first — the bus driver already reset/enumerated the codec before our PDO could even exist, so there's nothing left for us to reset.
  - `AllocateCommonBuffer`/`FreeCommonBuffer` stubbed to `STATUS_NOT_IMPLEMENTED`/no-op — these backed the old CORB/RIRB and BDL common-buffer allocation; a real replacement (via `m_BusInterface`'s `AllocateDmaBuffer`) is deferred to the stream/DMA rewrite below.
  - `DiscoverWidgets`/`DiscoverWidgetsInGroup`/`InitCodec`/`InitOutputPin` needed **no changes** — confirmed by inspection that they only ever call through `SendVerb`, so they keep working transparently against the new bus-interface-backed transport.
- **`adapter.cpp`** — `ValidateResources` no longer requires any memory or interrupt resource; it now just checks `ResourceList` is non-NULL and accepts whatever PnP assigned (including empty), consistent with the codec PDO's real (typically zero) resource shape.
- **`wavepciminiport.cpp`** — `CMiniportWavePciHda::Init()` previously unconditionally created a `PINTERRUPTSYNC` against `ResourceList`'s first interrupt and unconditionally dereferenced `m_pMmioBase` (now always NULL) to touch the `HDA_REG_INTCTL` register — both would have either failed `Init()` (cascading into `StartDevice()` failure, a different but still-broken symptom) or NULL-deref-crashed. Fixed by guarding that entire block (`PcNewInterruptSync`/ISR registration/`Connect`/the `INTCTL` register touch) behind `if (ResourceList->NumberOfInterrupts() > 0 && m_pMmioBase)`, skipping it gracefully (still setting `*ServiceGroup = NULL` so `Init()` succeeds) when absent — which is the expected case for this codec PDO.
- **`guids.h`/`guids.cpp`** — `GUID_HDAUDIO_BUS_INTERFACE` is `DEFINE_GUID`'d in `<hdaudio.h>`, which only allocates real storage in the one translation unit that includes it with `INITGUID` defined first (same mechanism already used for `IID_IHdaAdapterCommon`). Added `#include <hdaudio.h>` to `guids.h` (included by `guids.cpp` after `#define INITGUID`) — without this, the link failed with `LNK2001: unresolved external symbol GUID_HDAUDIO_BUS_INTERFACE` (caught on rebuild, now fixed).

**Rebuilt successfully** (`build -cZ`, checked/`chk` this time rather than free/`fre`, same `X:\`-subst-drive/`wnet`/amd64 workaround documented below in "Stage 4: first successful build") after fixing all three real compile/link errors above (`IoGetLowerDeviceObject` undeclared, `HDAUDIO_CODEC_COMMAND` field-name mismatch, `GUID_HDAUDIO_BUS_INTERFACE` unresolved external) — **clean, zero errors**, `stwrtxp.sys` produced in `objchk_wnet_amd64\amd64\`.

**What is now stubbed/deferred, not yet done** (does not block clearing Code 10, but does block actual audio playback): real stream/DMA-engine management against `m_BusInterface`'s `AllocateRenderDmaEngine`/`AllocateCaptureDmaEngine`/`AllocateDmaBuffer`/`FreeDmaBuffer`/`SetDmaEngineState`/`GetLinkPositionRegister`/`GetWallClockRegister`/`RegisterEventCallback`. `wavepcistream.cpp`'s existing BDL-based stream code (see "Stage 4: BDL construction implemented" and "Stage 4: BDL slot recycling in Service()" below) was written against the old direct-MMIO stream-descriptor-register model (`SDnCTL`/`SDnBDLPL`/`SDnLPIB`/etc via `GetMmioBase()`, now always NULL) and has **not** been ported to the bus-interface model yet — it will silently fail once a stream is actually opened (via the already-stubbed `AllocateCommonBuffer` returning `STATUS_NOT_IMPLEMENTED`), but that's expected and does not affect basic device enumeration.

**Not yet done**: re-test on the real p6-2133w to confirm `ValidateResources` no longer rejects the device and the codec actually enumerates without Code 10 (the fix is build-verified only, not yet hardware-verified).

**Next Steps (current, supersedes the stale "real-hardware track" steps in the old Next Steps list below)**:
1. Copy the rebuilt `stwrtxp.sys` (+ existing `Backported Driver/inf/stwrtxp.inf` — unchanged, still matches the same hardware ID) to the real machine via USB, reinstall via Device Manager → Update Driver → Have Disk.
2. Check `stwrtxp_log.txt` on that machine and Device Manager's status to confirm Code 10 is gone and `AcquireBusInterface`/`BringUpCodec`/`DiscoverWidgets`/`InitCodec` all succeed (or capture whatever new failure point emerges — this is the first time this exact code path runs against real hardware).
3. Once enumeration is clean, dial `ulDebugOut` back down from `DBG_ALL` to `DBG_DEFAULT` in `debug.h` (flagged since the previous segment, still not due until this succeeds).
4. Design and implement the real stream/DMA rewrite (`wavepcistream.cpp`/`wavepciminiport.cpp`'s `NewStream` and stream-descriptor-register code) against `HDAUDIO_BUS_INTERFACE`'s DMA-engine functions — required before actual playback/recording works, not required for Code 10/enumeration.
5. Minor cleanup, not urgent: `common.h`'s `m_NumInputStreams`/`m_NumOutputStreams`/`m_NumBidiStreams` fields are now dead (declared, never set, never read) since the old `ResetController()` that used to set them is gone; the `HDA_MAKE_VERB` macro in `hdaregs.h` may also now be unused — sweep both once the stream/DMA rewrite's scope is settled (may end up reused there instead of truly dead).

### Stage 5b: real-hardware BSOD after the Code-10 fix

**Symptom**: after copying the Code-10-fixed rebuild to the real p6-2133w and reinstalling, the very first real-hardware run bugchecked: **`0x0A IRQL_NOT_LESS_OR_EQUAL`**, params `{0x78, 0xC, 0x1, 0xFFFFF80001030F46}` (memory referenced `0x78`, IRQL 12 (DIRQL range), write operation, faulting instruction `nt+0x30f46` — an `xchg reg,[mem]`, i.e. an atomic write). User supplied the minidump: `Mini090826-01.dmp` (project root).

**Symbol dead end — do not retry this**: tried analyzing the dump with the WDK 7600-bundled classic `cdb.exe` (`C:\WinDDK\7600.16385.1\Debuggers\cdb.exe`) against `srv*C:\symbols*https://msdl.microsoft.com/download/symbols`. The public Microsoft symbol server **no longer serves `ntoskrnl.pdb` for this OS build** (Windows Server 2003 SP2 x64 kernel, image timestamp `5d275458` — long past EOL): `C:\symbols` filled up with cached `.sys` executables for dozens of other loaded drivers, but **zero `.pdb` files**, and `!analyze -v` explicitly reported `"Kernel symbols are WRONG"` / `BUCKET_ID: WRONG_SYMBOLS`. The `nt+0x30f46` attribution and the dump's `STACK_TEXT` are both unreliable/uninterpretable as a result (the dump is also only a *Mini* Kernel Dump — registers + raw stack words only, no real call-stack reconstruction possible even with correct symbols). **If this recurs, don't waste time retrying the public symbol server for this OS build** — instead get a **full/kernel memory dump** next time (Control Panel → System → Advanced → Startup and Recovery → set "Write debugging information" to at least "Kernel memory dump"), and lean on this project's own `.pdb` (built alongside `stwrtxp.sys`) to check whether `stwrtxp.sys` is actually on the stack — that doesn't require Microsoft's symbols at all.

**Root-cause hypothesis and fix applied** (not yet confirmed on real hardware): with symbol-based analysis dead, fell back to direct code review of every code path exercised by `StartDevice` on this first real run (the only code that had never executed against real hardware before). Ruled out:
- `wavepciminiport.cpp`'s interrupt-sync/ISR registration in `Init()` — structurally unreachable (`m_pMmioBase` is always `NULL` under the new architecture, so the guard `NumberOfInterrupts() > 0 && m_pMmioBase` can never be true) and every other use of `m_pInterruptSync` (destructor, `PowerChangeNotify`) is already NULL-checked.

Prime suspect: **`CHdaAdapterCommon::SendVerb`** (`common.cpp`) — called dozens of times per `StartDevice` (once per HDA verb, across `BringUpCodec`/`DiscoverWidgets`/`InitCodec`) via `HDAUDIO_BUS_INTERFACE::TransferCodecVerbs`. The original implementation put the `HDAUDIO_CODEC_TRANSFER` struct *and* the synchronizing `KEVENT` on `SendVerb`'s **stack frame**, and handed their addresses to the bus driver as the async completion context — safe only if a non-success return from `TransferCodecVerbs` is a hard guarantee that the completion callback will *never* fire later (mirroring `IoCallDriver`/IRP semantics exactly). Nothing in the DDK's `hdaudio.h` (`PTRANSFER_CODEC_VERBS`'s typedef has no doc comment on this) actually guarantees that. If it's ever violated, the real bus driver (`HDAudBus.sys` via the UAA package) writes into a stack frame that has already returned — corrupting whatever now occupies that stack memory. That fits the observed signature well: a small-offset atomic write, at DIRQL, in code that isn't ours (i.e. corruption manifesting later, inside unrelated kernel code, once the stale stack memory got reused for some other kernel object with a spinlock/interlocked field at a similar offset) — and it's far more likely to be hit during the dozens of `SendVerb` calls in widget discovery than during `AcquireBusInterface`'s single, standard IRP-based query (IRP completion timing *is* a hard OS guarantee, so that one was left as-is).

**Fix applied**: `SendVerb` now heap-allocates (`ExAllocatePoolWithTag(NonPagedPool, ...)`) a combined `HDA_VERB_SYNC_CONTEXT { KEVENT Event; HDAUDIO_CODEC_TRANSFER Transfer; }` per call instead of using stack locals, and only frees it once `KeWaitForSingleObject` has actually returned (which happens-after the completion callback runs, so freeing then is always safe) or immediately on a synchronous failure return (same assumption as before, but now a wrong assumption leaks a pool block instead of corrupting a reused stack frame). `VerbTransferCompletion` moved out of the `#pragma code_seg("PAGE")` region (must stay non-paged — the bus driver may invoke it from a DPC). Rebuilt clean (checked x64, `build -cZ`, 0 warnings in `build.wrn`).

**Not yet done**: this is a hypothesis-driven hardening fix, not a confirmed root cause (no trustworthy stack trace was obtainable). **Next real-hardware test is the actual verification** — if the BSOD recurs identically, this wasn't it; capture a fresh dump (ideally a full/kernel dump per the note above) and also grab `stwrtxp_log.txt` from the machine (shows how far `AcquireBusInterface`/`BringUpCodec`/`DiscoverWidgets`/`InitCodec` got before the crash) to narrow further.

### Stage 5c: second BSOD (full dump) — real root cause found: missing `Irp->Tail.Overlay.Thread`

**The SendVerb hardening fix above did not prevent a recurrence.** The user retested the `SendVerb`-hardened rebuild on the real p6-2133w and got the same bugcheck class again (`0x0A IRQL_NOT_LESS_OR_EQUAL`), this time producing a **full/kernel memory dump** (`MEMORY.DMP`, ~556 MB, default filename for a Complete/Kernel dump — confirms the user changed the "Write debugging information" setting per the Stage 5b note) instead of a minidump, and — notably — **`stwrtxp_log.txt` was not written at all this run**. That absence turned out to be a non-mystery, not a new clue: the crash (see below) happens *before* the first possible `DOUT` call on this path (`AcquireBusInterface`'s own success log fires only after the crash point), so no log entry was ever going to exist regardless of root cause.

**This time symbol-based analysis actually worked**, because it's a full dump (not a mini dump — x64 stack unwinding uses unwind metadata embedded in each module's own PE image, `.pdata`/`.xdata`, which doesn't require a `.pdb` at all) and because `cdb.exe`'s symbol path included this project's own build output directory (`Backported Driver/src/objchk_wnet_amd64/amd64/`, containing `stwrtxp.pdb` from the exact crashed build) alongside the public symbol server. Result: **`HDAudBus.sys`'s own public symbols were actually present** in the local `C:\symbols` cache this time (`hdaudbus.pdb`) — only `nt`'s private/type-carrying symbols are still unavailable for this OS build (same confirmed-dead-end as Stage 5b) — so the stack trace's module/function attribution for `stwrtxp!...` and `HDAudBus!...` frames is genuine and trustworthy; only the `nt!<nearest-export>+offset` labels are approximate.

Bugcheck: `0x0A`, `{0x78, 0xC, 0x1, <nt address>}` — memory referenced `0x78` (i.e. NULL plus a small field offset), IRQL 0xC, write operation. Real (symbol-verified) call stack, innermost first:

```
nt!<mis-symbolized, unresolved>          <- actual crash site, inside nt
HDAudBus!FxIrp::CompleteRequest+0x12
HDAudBus!FxPkgPdo::PnpQueryInterface+0x37
HDAudBus!FxCoreGlobalDispatch+0x38
stwrtxp!CHdaAdapterCommon::AcquireBusInterface+0x1d6
stwrtxp!CHdaAdapterCommon::Init+0x106
stwrtxp!StartDevice+0x123
portcls!PnpStartDevice+0x18d
...
```

**Root cause, confirmed by exact-offset match**: `AcquireBusInterface()` (`common.cpp`) builds its own `IRP_MN_QUERY_INTERFACE` IRP via `IoAllocateIrp()` and sends it synchronously to the bus-owned lower device object (the codec's PDO, owned by `HDAudBus.sys`/KMDF — hence `FxPkgPdo`/`FxIrp` in the stack). `IoAllocateIrp` zero-fills the new IRP (via `IoInitializeIrp`), which leaves `Irp->Tail.Overlay.Thread` **`NULL`** — this driver never set it. That field is not optional for an IRP that will actually be completed: `IoCompleteRequest`'s post-completion bookkeeping (thread priority boost / APC-based completion delivery) dereferences `Irp->Tail.Overlay.Thread` at a small field offset. With `Thread == NULL`, that dereference is exactly `NULL + <small offset>` — matching the bugcheck's `Arg1 == 0x78` precisely, and matching the crash occurring inside `HDAudBus!FxIrp::CompleteRequest`'s call down into `nt` while completing exactly this IRP. This is a well-known WDM gotcha: any IRP built by hand with `IoAllocateIrp` (as opposed to one handed down by the I/O manager, or built via a helper like `IoBuildDeviceIoControlRequest` that does this for you) must have `Tail.Overlay.Thread` set manually before `IoCallDriver`, or `IoCompleteRequest` can crash exactly like this.

This is a **different, more concrete root cause** than the Stage 5b `SendVerb` hypothesis — that fix (heap-allocating the verb-transfer sync context) is still correct defensive hardening and was kept, but it did not address this bug, which is specifically in `AcquireBusInterface`'s hand-built IRP, not in `SendVerb`'s `TransferCodecVerbs` callback path.

**Fix applied**: added one line in `AcquireBusInterface()` (`common.cpp`, right before `IoSetCompletionRoutine`):
```cpp
irp->Tail.Overlay.Thread = PsGetCurrentThread();
```
Rebuilt clean (checked x64, `build -cZ`): "8 files compiled - 7 Warnings" / "1 executable built" — same warning count as every prior clean build (the 7 are the known benign `subst`-drive duplicate-path notices, not real issues).

**Not yet done**: rebuild has not yet been retested on the real p6-2133w. This is the next concrete action — copy the new `stwrtxp.sys` over, retest, and confirm `stwrtxp_log.txt` now shows `AcquireBusInterface: got HDAUDIO_BUS_INTERFACE...` succeeding (the first log line in this path that could never have appeared before this fix, given the crash always happened upstream of it).

**Debugging methodology note for future sessions**: `cdb.exe -y` accepts a semicolon-separated symbol path — always include this project's own build output directory (`.../src/objchk_wnet_amd64/amd64/`, wherever the crashed build's matching `.pdb` lives) alongside `srv*C:\symbols*https://msdl.microsoft.com/download/symbols`. This resolves our own driver's frames *and* any other module whose `.pdb` happens to be cached locally (like `HDAudBus.sys` was here) independent of whether `nt`'s symbols are available. A full/kernel dump's stack trace is trustworthy for module attribution even without correct `nt` symbols, because x64 stack unwinding relies on each module's own embedded `.pdata`/`.xdata`, not the `.pdb` — only *names* for the specific unresolved module (here, `nt`) are untrustworthy, not the overall call chain or which other modules are on it.

### Stage 5d: third BSOD (0x44 MULTIPLE_IRP_COMPLETE_REQUESTS) — real root cause found: completion routine clobbered by RtlZeroMemory

**The `Tail.Overlay.Thread` fix above did not fully fix things — it fixed that specific NULL deref, but the user's next real-hardware retest produced a *different* bugcheck**: **`0x44 MULTIPLE_IRP_COMPLETE_REQUESTS`**, `{fffffadf449c28d0, 0x1c19, 0, 0}` (Arg1 = IRP address; Arg2 currently unexplained/not decoded — `nt` symbols remain unavailable for this OS build, see below, so no authoritative meaning for `0x1c19` was established). Bugcheck 0x44's meaning per its standard documentation: "a driver has requested that an IRP be completed (`IoCompleteRequest()`), but the packet has already been completed" — i.e. two separate completions of the same IRP.

**Dump-staleness pitfall hit and resolved (process note, not a code bug)**: the first `MEMORY.DMP` analyzed for this crash was actually **stale** — leftover from the already-fixed second (0xA) crash, not the new 0x44 one. Full/kernel dumps (~500+ MB) can take a long time to finish writing to disk, and the user had reported the BSOD verbally before the dump finished writing. Detected via two signals: (1) the bugcheck code/args exactly matched the already-diagnosed second crash, and (2) `stwrtxp.sys` loaded in the dump with **"(no symbols)"** — a build/PDB timestamp mismatch against the current local build, itself a reliable tell that a dump predates the current binary. **Lesson for future sessions**: after being told about a BSOD, if analysis of `MEMORY.DMP` produces a bugcheck identical to one already fixed, or `stwrtxp.sys` loads with no symbols, treat it as a stale/incomplete dump write and ask the user to confirm the dump has finished writing (rename the old one aside, e.g. to `oldMEMORY.DMP`) before trusting the analysis. Once the user confirmed the fresh dump, re-analysis showed `stwrtxp.sys` loading with **matching private PDB symbols** — confirmed correct data — and:

```
BugCheck 44, {fffffadf449c28d0, 1c19, 0, 0}
Probably caused by : stwrtxp.sys ( stwrtxp!CHdaAdapterCommon::AcquireBusInterface+22b )

stwrtxp!CHdaAdapterCommon::AcquireBusInterface+0x22b
stwrtxp!CHdaAdapterCommon::Init+0x106
stwrtxp!StartDevice+0x123
portcls!PnpStartDevice+0x18d
portcls!EnqueuedIoWorkItemCallback+0x23
```

Disassembly (via `.lines` + `u`) pinpointed the fault-attributed address as the return address immediately following our own `call IoFreeIrp` — i.e. the double-completion is detected during/around freeing our hand-built IRP, not during the bus driver's own completion of it.

**Root cause, found by reading `AcquireBusInterface`'s exact call order in the disassembly and cross-checking it against the authoritative WDK 7600 `wdm.h` definitions of `IoGetNextIrpStackLocation`/`IoSetCompletionRoutine`**: `IoGetNextIrpStackLocation(Irp)` is a **pure, non-mutating** inline read — `return Irp->Tail.Overlay.CurrentStackLocation - 1;`, no side effects — and `IoSetCompletionRoutine` resolves to that exact same slot internally. The previous code in `AcquireBusInterface` did, in order:
1. `IoSetCompletionRoutine(irp, QueryBusInterfaceCompletion, &event, TRUE, TRUE, TRUE);` — writes `CompletionRoutine`/`Context`/`Control` into the next stack location.
2. `PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);` — fetches a pointer to that **same** slot.
3. `RtlZeroMemory(stack, sizeof(IO_STACK_LOCATION));` — **zeroes the entire slot, wiping out the `CompletionRoutine`/`Context`/`Control` fields `IoSetCompletionRoutine` had just written in step 1.**

Net effect: our completion routine registration never actually reaches `HDAudBus.sys` — `stack->Control` ends up 0 (no `SL_INVOKE_ON_SUCCESS`/`_ON_ERROR`/`_ON_CANCEL` bits) and `stack->CompletionRoutine` is `NULL`. When the bus driver completes the IRP via `IofCompleteRequest`, there's nothing registered at our stack location to intercept it and return `STATUS_MORE_PROCESSING_REQUIRED`, so the completion walk runs straight through to full completion on its own. **This also retroactively explains the Stage 5c crash**: the NULL `Tail.Overlay.Thread` deref happened during that same uninterrupted full-completion walk (not during some completion-routine-mediated path) — fixing `Tail.Overlay.Thread` removed that NULL deref, but the IRP was still fully completing itself before our code got a chance to observe it via the (silently-broken) completion callback. Our code then goes on to call `IoFreeIrp(irp)` on an IRP that the real I/O manager had already fully completed on its own — which is exactly `MULTIPLE_IRP_COMPLETE_REQUESTS`.

Cross-checked against a known-good, shipped reference implementation of this exact "self-built top-level IRP + stack `KEVENT` + synchronous send" idiom, `ClassSendDeviceIoControlSynchronous`/`ClassSendIrpSynchronous`/`ClassSignalCompletion` in `C:\WinDDK\7600.16385.1\src\storage\class\classpnp\class.c` — structurally near-identical to our pattern overall, which confirmed the general idiom (heap/stack IRP + `Tail.Overlay.Thread` + event-based completion + `IoFreeIrp`) is sound; the bug was specifically the field-clobbering order, not the idiom itself.

**Fix applied** (`common.cpp`, `AcquireBusInterface`): reordered so the stack location is fetched, zeroed, and fully filled in (`MajorFunction`/`MinorFunction`/`Parameters.QueryInterface.*`/`irp->IoStatus.*`) **first**, and `IoSetCompletionRoutine` is called **last**, immediately before `IoCallDriver` — so its write into the slot is the final one and can't be clobbered. Rebuilt clean (checked x64, `build -cZ`): "8 files compiled - 7 Warnings" / "1 executable built" — same benign warning count as every prior clean build.

**Retested on real hardware — no crash this time**, but Device Manager still showed **Code 10**, with `stwrtxp_log.txt` now (for the first time) surviving long enough to show a real, non-crashing failure:

```
ValidateResources: expected at least 1 memory resource, got 0
AcquireBusInterface: IRP_MN_QUERY_INTERFACE for GUID_HDAUDIO_BUS_INTERFACE failed, status=C0000206
StartDevice: adapter common Init failed, status=C0000206
```

(Line 1 is a stale leftover from a much earlier pre-HDAUDIO_BUS_INTERFACE-rewrite run — `stwrtxp_log.txt` is opened in append mode across driver load attempts, not truncated each time; `adapter.cpp`'s current `ValidateResources` no longer contains that log statement at all, so it cannot have been produced by this build. Lines 2-3 are the real, current failure.)

**This also retroactively explains why the 0xA/0x44 crashes never let us see a failure status before now**: those crashes happened *during* `IofCompleteRequest`'s internal bookkeeping / `IoFreeIrp`, both squarely *after* `IoCallDriver` returns but *before* the `if (!NT_SUCCESS(status))` check could ever run — so it's entirely possible `AcquireBusInterface` was already failing with this exact status in every prior run too; the IRP-management bugs and this status bug are independent and were simply stacked on top of each other, with the crash always masking the status check underneath.

`STATUS_INVALID_BUFFER_SIZE` (`0xC0000206`) from `HDAudBus.sys` on an `IRP_MN_QUERY_INTERFACE` for `GUID_HDAUDIO_BUS_INTERFACE` pointed at a version/size mismatch between what we request and what this specific bus driver (confirmed via `AskUserQuestion` with the user: this is the **official Microsoft KB888111 UAA package**, a ~2005/2006-era driver, not a Vista/7 port) actually implements. Researched via `WebSearch`/`WebFetch` against Microsoft's own current HD Audio DDI documentation (`learn.microsoft.com/.../audio/high-definition-audio-ddi` and `.../obtaining-an-hdaudio-bus-interface-ddi-object`) rather than guessing at struct layouts:
- Confirmed the **struct itself is not the problem**: per Microsoft's own docs, "The version of the HD Audio bus driver that runs on Windows Server 2003 and Windows XP supports three variants of the HD Audio DDI: A DDI that is defined by the `HDAUDIO_BUS_INTERFACE` structure. **This DDI is identical to the HD Audio DDI in Windows Vista.**" So `sizeof(HDAUDIO_BUS_INTERFACE)` from the WDK 7600 header (144 bytes / 14 function pointers, confirmed via disassembly: `Size = 0x90`) is correct as-is for this exact KB888111-era bus driver too — no older/smaller struct exists to guess at.
- **Found the actual bug**: Microsoft's documented sample parameter table for this exact IOCTL states plainly: `USHORT Version` must be **`0x0100`**, not a plain incrementing integer. Our code had `stack->Parameters.QueryInterface.Version = 1;` — a plausible-looking but wrong guess (no `HDAUDIO_BUS_INTERFACE_VERSION`-style named constant exists in the WDK 7600 header to catch this by inspection alone).

**Fix applied** (`common.cpp`, `AcquireBusInterface`): changed `Version = 1` to `Version = 0x0100`, matching Microsoft's documented sample exactly. Rebuilt clean (checked x64, `build -cZ`): "8 files compiled - 7 Warnings" / "1 executable built".

**Not yet done**: rebuild has not yet been retested on the real p6-2133w. This is the next concrete action — copy the new `stwrtxp.sys` over, retest, and confirm Code 10 is finally gone, with `stwrtxp_log.txt` showing `AcquireBusInterface`/`BringUpCodec`/`DiscoverWidgets`/`InitCodec` all succeeding. If this specific status clears but a new failure appears, note that `stwrtxp_log.txt`'s append-only behavior (see above) means old lines from earlier runs will still be present in the file — always check the last few lines / most recent block against this rebuild's timestamp, not just the first match found.

### Stage 4 progress: driver skeleton written

All new source lives under `Backported Driver/src/` (build file `sources`) and `Backported Driver/inf/` (packaging). Files, in dependency order:

> **Paths below are as of Stage 4 and one of them has moved.** `Backported Driver/inf/` was deleted in Stage 5bw as a redundant third copy; `stwrtxp.inf` is now maintained in `Backported Driver/package/` and copied to `package-release/`. Everything else in this section is still where it says.

- **`hdaregs.h`** — HDA controller MMIO register offsets/bits (GCTL/CORB/RIRB/per-stream `HDA_SD_*(n)` macros) and the verb-encoding macro (`HDA_MAKE_VERB`). Pure spec-derived, no codec-specific content.
- **`hdaverbs.h`** — HDA verb IDs, `GET_PARAMETER` parameter IDs, widget-type/pin-control/power-state constants. Also spec-derived; deliberately has no hardcoded NIDs (see Stage 1 finding: this board has no hardcoded HP quirk, pin config must be read at runtime).
- **`debug.h`** — `DOUT`/`BREAK` debug-print macros, adapted from the `ac97\driver` sample.
- **`shared.h`** — common includes, the `HdaWavePin` bridge-pin enum, the `HDA_WIDGET` struct (flat runtime-discovered widget table entry), and the `IHdaAdapterCommon` COM interface (`Init`, `SendVerb`, `GetCodecAddress`, `GetMmioBase`, `GetWidgetCount`/`GetWidget`, `InitCodec`) with its private GUID.
- **`common.h`/`common.cpp`** — `CHdaAdapterCommon`, the shared adapter object. **Functionally complete for controller bring-up and verb transport**: MMIO mapping, `GCTL`/`CRST` reset handshake, `STATESTS` codec scan, CORB/RIRB common-buffer allocation and ring setup, `SendVerb` (CORB write + RIRB poll, with the retry-count/per-attempt-timeout design mirrored from `stwrt64.sys`'s `CController::TransferCodecVerb`), `DoubleResetHandshake` (mirrors the recovered double-`FUNCTION_RESET` D3Cold-detection pattern), and `DiscoverWidgets`/`DiscoverWidgetsInGroup` (walks `SUBORDINATE_NODE_COUNT`/`FUNCTION_GROUP_TYPE`/`AUDIO_WIDGET_CAP`/`GET_CONFIG_DEFAULT`/`GET_CONNECTION_LIST_ENTRY` into the flat `m_Widgets[]` table — no hardcoded NIDs). **`InitCodec()` now applies the actual per-widget init sequence** (see "Stage 4: InitCodec() ported from sigmatel.c" below) — `Init()`'s call order was changed to `BringUpCodec()` (double-reset + vendor/device-ID check, split out of the old `InitCodec()`) → `DiscoverWidgets()` → `InitCodec()`, since the new verb sequence needs the widget table populated first.
- **`adapter.h`/`adapter.cpp`** — `DriverEntry` (`PcInitializeAdapterDriver`), `ValidateResources` (expects ≥1 memory resource + ≥1 interrupt, 0 legacy DMA channels — HDA uses one MMIO BAR, unlike AC97's two I/O port ranges), and `StartDevice` (creates `CHdaAdapterCommon`, calls its `Init`, then creates+`Init`s+registers the topology subdevice, then the wave subdevice, then wires `PcRegisterPhysicalConnection` between the wave filter's bridge pins and the topology filter's two pins).
- **`mintopo.h`/`mintopo.cpp`** — `CMiniportTopologyHda`. Deliberately minimal: a static 2-pin (render bridge / capture bridge), 0-node topology filter descriptor — no mixer/volume nodes are exposed, consistent with the project's explicit non-goal of full mixer/EQ parity. `DataRangeIntersection` returns `STATUS_NOT_IMPLEMENTED` to let PortCls use its default handler (standard pattern).
- **`wavepciminiport.h`/`wavepciminiport.cpp`** — `CMiniportWavePciHda` (`IMiniportWavePci` + `IPowerNotify`). `GetDescription` returns a static 2-pin (render/capture) filter descriptor with a single conservative 16-bit/8-48kHz PCM data range (TODO: tighten once per-widget `SUPP_PCM_RATES`/`SUPP_STREAM_FORMATS` are read and enforced). `NewStream` currently hardcodes stream descriptor index 0 for render / 1 for capture — **TODO: derive the input descriptor's starting index from the controller's actual output-stream count** (`GCAP`, already read into `common.cpp`'s `m_NumOutputStreams`) instead of assuming exactly one output descriptor exists ahead of it, before enabling simultaneous playback+record on other controllers.
- **`wavepcistream.h`/`wavepcistream.cpp`** — `CMiniportWaveStreamHda` (`IMiniportWavePciStream`). **BDL construction is now implemented** (see "BDL construction implemented" below) — `SetFormat` programs `SDnFMT` (verified for the common 48kHz-family 16-bit case; other rates' multiplier/divisor encoding is marked TODO-verify-against-spec-table). `SetState` sets the stream tag into `SDnCTL`, pulls initial mappings, programs the BDL registers, and toggles `RUN`. `GetPosition` reads `SDnLPIB` directly (real hardware position, no interpolation needed).
- **`guids.h`/`guids.cpp`** — `guids.cpp` is the one translation unit compiled with `INITGUID` defined, so `IID_IHdaAdapterCommon` (declared in `shared.h`) actually gets storage; every other `.cpp` includes `shared.h` without `INITGUID` and links against that storage.
- **`sources`** — WDK build file; targets `wnet`/amd64 per the already-documented build-target gotcha (see Stage 2 recap). Links `portcls.lib`/`stdunk.lib`/`libcntpr.lib`/`ks.lib`.
- **`Backported Driver/inf/stwrtxp.inf`** — `[StwrtXP.Mfg.NTamd64]` install section matching `HDAUDIO\FUNC_01&VEN_111D&DEV_76C7&SUBSYS_103C2ACD` (plus a fallback match without the `SUBSYS` qualifier), documents the KB888111 UAA bus driver prerequisite in a header comment, standard KS/WDMAUDIO service+interface registration.

**What's NOT done yet (the real remaining Stage 4/5 work, in priority order):**
1. ~~Port the actual STAC92HD73XX init-verb sequence from `sigmatel.c` into `common.cpp`'s `InitCodec()`~~ **DONE** — see "Stage 4: InitCodec() ported from sigmatel.c" below.
2. ~~Implement real BDL construction in `wavepcistream.cpp`~~ **DONE** — see "Stage 4: BDL construction implemented (and a signature-mismatch bug fixed along the way)" below.
3. ~~No ISR/DPC/`ServiceGroup`-signaling infrastructure exists anywhere in the driver yet~~ **DONE** — see "Stage 4: ISR/DPC wired up" below.
4. ~~`Service()` doesn't recycle BDL slots hardware has already consumed~~ **DONE** — see "Stage 4: BDL slot recycling in Service()" below.
5. Fix the hardcoded single-stream-descriptor assumption in `wavepciminiport.cpp`'s `NewStream` once multi-descriptor allocation matters.
6. ~~No build has been attempted yet~~ **DONE — first build now succeeds cleanly, zero errors.** See "Stage 4: first successful build" below.

### Stage 4: first successful build

Attempted (and completed) the project's first real compile+link against WDK 7600's `wnet`/amd64 free build environment. Three real bug categories surfaced, all now fixed — confirming the "PortCls structs were written from memory, not verified" risk flagged in item 6 above was justified:

1. **`KSDATARANGE_AUDIO` initializer overcounts** (`mintopo.cpp`'s `PinDataRangeOut`, `wavepciminiport.cpp`'s `PinDataRangePcm`): both supplied 6 trailing scalar values where the real struct (`ksmedia.h`) only has 5 fields after the nested `DataRange` member (`MaximumChannels, MinimumBitsPerSample, MaximumBitsPerSample, MinimumSampleFrequency, MaximumSampleFrequency`) — a duplicate leading value in each. Fixed by dropping the duplicate.
2. **`KSPIN_DESCRIPTOR`/`PCPIN_DESCRIPTOR` field-order/count mismatch** in both files' pin-descriptor arrays (`MiniportPins[]` / `MiniportWavePins[]`): the real `KSPIN_DESCRIPTOR` (`ks.h`) has exactly 11 fields (`InterfacesCount, Interfaces, MediumsCount, Mediums, DataRangesCount, DataRanges, DataFlow, Communication, Category, Name`, then a `Reserved`/`ConstrainedDataRanges` union slot); ours had 13 miscounted values including two non-existent identifiers (`KSAUDFNAME_LINE_OUT`/`_LINE_IN`) that don't exist anywhere in the WDK. Rewrote both arrays field-for-field against the real header, cross-checked against the WDK 7600 `msvad\simple` sample's `toptable.h`/`wavtable.h` (same static-aggregate-initializer style as our code), using `NULL` for `Name` instead of the invented constants.
3. **3× `LNK2001: unresolved external symbol ... NonDelegatingQueryInterface`** for `CMiniportTopologyHda`, `CMiniportWavePciHda`, `CMiniportWaveStreamHda`: all three use `DECLARE_STD_UNKNOWN()`, which declares but doesn't define this method — only `CHdaAdapterCommon` (in `common.cpp`) had ever gotten a body. Added a full implementation to each class (in `mintopo.cpp`, `wavepciminiport.cpp`, `wavepcistream.cpp` respectively), modeled directly on the WDK 7600 `ac97\driver` sample's equivalents, each handling `IID_IUnknown` plus whatever its own interface(s) imply (`IID_IMiniport`/`IID_IMiniportTopology` for topology; `IID_IMiniport`/`IID_IMiniportWavePci`/`IID_IPowerNotify` for the wave miniport; `IID_IMiniportWavePciStream` only for the stream — `IID_IServiceSink`/`IID_IDrmAudioStream` deliberately omitted, out of scope).

All fixes were derived by grepping the actual WDK 7600 headers (`ks.h`, `portcls.h`, `ksmedia.h`) and sample sources directly, never from memory.

**Build-tooling discovery**: in this session, the `Bash` tool's `cmd /c '...'` chained invocation (`setenv.bat && cd /d X:\ && build -cZ`) silently produced no output at all (not even an error) — root cause not fully diagnosed, suspected Git-Bash/cmd.exe quoting interaction. **Switching to the `PowerShell` tool for the identical command string works correctly** and should be used for all WDK build invocations in this project going forward:
```
cmd /c '"C:\WinDDK\7600.16385.1\bin\setenv.bat" C:\WinDDK\7600.16385.1 fre x64 WNET && cd /d X:\ && build -cZ' 2>&1 | Out-String
```
(run via the `X:` `subst`-drive workaround already documented — see the spaces-in-path note wherever it's recorded in this file's history/git log, since the build fails with `error U1087` from the real path's spaces otherwise.) Two harmless stderr lines appear every run before the real `BUILD:` output: `WARNING: x64 Native compiling isn't supported. Using cross compilers.` and a PowerShell `$env:PATH`-quirk warning about a nonexistent OpenSSH directory — neither indicates a real problem.

**Result**: `build -cZ` now reports **"8 files compiled - 7 Warnings" / "1 executable built"** with **zero errors**. The 7 warnings were checked and are **not real compiler warnings** — `buildfre_wnet_amd64.wrn` only contains benign "file X and ..\X exist" duplicate-path notices, an artifact of building from the `X:` `subst`-mapped drive vs. the real underlying path; no actual warning text needing a fix was found. `stwrtxp.sys` (16,384 bytes) plus matching `.pdb` symbol files now exist in `Backported Driver/src/objfre_wnet_amd64/amd64/` — **this is the first time this driver has ever produced a real binary.**

### Stage 4: BDL construction implemented (and a signature-mismatch bug fixed along the way)

Implemented `wavepcistream.cpp`'s BDL (Buffer Descriptor List) construction — the task that was previously item 2 above. While doing this, found and fixed a **pre-existing bug, not new scope**: the previous `wavepcistream.h`/`.cpp` and `wavepciminiport.h`/`.cpp` were written against an incorrect, invented mental model of the real WDK `IMiniportWavePciStream`/`IMiniportWavePci`/`IPortWavePciStream` interfaces (confirmed by grepping `C:\WinDDK\7600.16385.1\inc\ddk\portcls.h` directly, lines ~1946-2130):
- There is **no `MappingComplete` or `Silence` method** in the real `IMiniportWavePciStream` interface — those were invented. The real contract is **pull-based**: the miniport calls `IPortWavePciStream::GetMapping(Tag, &PhysicalAddress, &VirtualAddress, &ByteCount, &Flags)` itself to obtain each new physically-scattered buffer segment, and `ReleaseMapping(Tag)` once done with it.
- `RevokeMappings` had the wrong signature (was `(Tag, Mdl, MappedSize, OUT MappedBytes)` returning `void`; the real one is `(IN PVOID FirstTag, IN PVOID LastTag, OUT PULONG MappingsRevoked)` returning `NTSTATUS`).
- `GetPosition` had the wrong signature (`OUT PULONG Position`; real is `OUT PULONGLONG Position`).
- `IMiniportWavePci::Init` was missing its required `OUT PSERVICEGROUP *ServiceGroup` 4th parameter; `NewStream` was missing its required `IN PPORTWAVEPCISTREAM PortStream` parameter (comes right after `PoolType`); the miniport-level `Service(void)` method was entirely missing. `wavepciminiport.h` also had a hand-written, wrong-signature `Init` declaration that would have conflicted with the correct one already implied by the `IMP_IMiniportWavePci` macro.

All of the above are now fixed, cross-checked line-by-line against `portcls.h`'s `IMP_IMiniportWavePciStream`/`IMP_IMiniportWavePci` macros (see lines 1992-2130 of that header) rather than from memory.

New architecture, modeled on the WDK 7600 `ac97\driver` sample's `CMiniportWaveICHStream::GetNewMappings`/`ReleaseUsedMappings` (adapted from AC97's own BDL format to the real HDA spec's `{ULONGLONG Address; ULONG Length; ULONG IntOnCompletion;}` entry layout):
- **`AllocateBdl()`** — lazily allocates the 32-entry common (cache-coherent) BDL buffer on the first pulled mapping, via a new shared `IHdaAdapterCommon::AllocateCommonBuffer`/`FreeCommonBuffer` pair (`shared.h`/`common.h`/`common.cpp`) that reuses the adapter-common object's already-obtained `PDMA_ADAPTER`, instead of each stream calling `IoGetDmaAdapter` itself (the old destructor already had a TODO flagging this as undesirable once multiple streams exist concurrently). Programs `SDnBDLPL`/`SDnBDLPU` once here since the BDL's physical base address is fixed for the stream's lifetime.
- **`GetNewMappings()`** — pulls mappings from `m_pPortStream->GetMapping()` in a loop (releasing `m_MapLock` around the call into the port, matching the AC97 sample's documented reason: `GetMapping`/`ReleaseMapping` must not be called with a spinlock held), filling BDL slots `[m_BdlEntryCount, ...)`, setting each entry's `IntOnCompletion` bit when the port's returned `Flags` is nonzero. Reprograms `SDnLVI`/`SDnCBL` afterward (only while stopped — see below). *(Since superseded — see "Stage 4: BDL slot recycling in Service()" below: this no longer uses a rotating `m_BdlHead`, and now refuses to grow the ring at all once the stream is running.)*
- **`RevokeMappings(FirstTag, LastTag, &MappingsRevoked)`** — since tags are handed out in strictly increasing order and never reordered, `[FirstTag..LastTag]` is always a contiguous suffix run; finds and truncates it, reprograms registers if stopped.
- **`MappingAvailable()`**/**`Service()`** — at this stage both just called `GetNewMappings()` to top up. *(Since superseded — see "Stage 4: BDL slot recycling in Service()" below, which implements the mid-stream ring-recycling-while-`RUN=1` gap flagged in the paragraph this replaces.)*
- Destructor now releases any outstanding mappings, frees the BDL via `FreeCommonBuffer`, and releases the stream's `PSERVICEGROUP`.

### Stage 4: `InitCodec()` ported from sigmatel.c

Read `Backported Driver/reference/linux-hda/sigmatel.c` and `generic.c` directly rather than assuming a static per-model verb table exists. Key finding: **`stac92hd73xx_core_init[]` (the only STAC92HD73XX-specific verb array in `sigmatel.c`) is a single verb** — `{0x1f, AC_VERB_SET_VOLUME_KNOB_CONTROL, 0xff}` (master volume knob to max, direct control). All the actual pin/DAC power-up, routing, amp-unmute, and EAPD-enable work happens in `stac_init()` calling the shared `snd_hda_gen_init()` (in `generic.c`), which builds and activates DAC→mixer→pin "paths" from the BIOS-programmed pin configs via the generic HDA auto-parser (`snd_hda_gen_parse_auto_config` → `init_multi_out`/`snd_hda_activate_path`/`set_pin_eapd`). Confirmed (again) that `92HD73XX`'s `probe_stac92hd73xx()` has no GPIO/EAPD-mask quirk for our specific board — EAPD here is the modern per-pin `PIN_CAP`-gated kind (`set_pin_eapd()` in `generic.c`), not GPIO-based.

Building the full generic path-graph (multi-depth paths, aamix, digital I/O, jack-detect automute) is out of scope for this MVP (playback-only, not mixer/EQ/DTS parity), so `common.cpp`'s new `InitCodec()`/`InitOutputPin()` reproduce just the observable effect for this board's expected single-DAC-deep output pins, driven entirely off the runtime-discovered `m_Widgets[]` table (no hardcoded NIDs):
- Power the AFG to D0.
- For every discovered pin widget whose `GET_CONFIG_DEFAULT` port-connectivity isn't "no connect" and whose default device is Line-Out/Speaker/HP-Out: power the pin + its first connection (the DAC) to D0, `SET_CONNECT_SEL` the pin to that DAC, unmute the DAC's and pin's output amps (`SET_AMP_GAIN_MUTE`, payload `0xB07F` = both channels unmuted/max gain), enable the pin as an output (`SET_PIN_WIDGET_CTRL`, plus the headphone-amp bit specifically for HP-Out pins), and enable EAPD only if that pin's `PIN_CAP` reports `AC_PINCAP_EAPD` (bit 16).
- Find the Volume Knob widget by `Type` (not a hardcoded NID like `sigmatel.c`'s `0x1f`) and apply the one real STAC92HD73XX-specific verb: `SET_VOLUME_KNOB` = `0xFF`.
- Recording/input pins are explicitly skipped for now (out of scope until recording is tackled).

New constants added to `hdaverbs.h`: `HDA_VERB_SET_VOLUME_KNOB` (0x7F0), `HDA_PINCAP_EAPD` (bit 16 of `PIN_CAP`), and `HDA_PINCFG_*` pin-config decode macros (`PORT_CONN`/`DEVICE` field extraction + device-type constants), mirroring the Linux `AC_DEFCFG_*` macros. `common.h`/`common.cpp` gained `BringUpCodec()` (split out of the old `InitCodec()` — double-reset handshake + vendor/device-ID check, now called before `DiscoverWidgets()`) and `InitOutputPin()` (per-pin helper called from the new `InitCodec()`).

**Not yet verified against real hardware or even a build** — this is derived from reading the reference source and the public HDA spec, not yet build-tested (WDK build still pending, see item 4 above) or hardware-tested (VM still not set up, see Next Steps).

### Stage 4: ISR/DPC wired up

Implemented the ISR/DPC-signaling gap flagged as item 3 above. Confirmed against `portcls.h` and the WDK 7600 `ac97\driver` sample (`wavepciminiport.cpp`, `CMiniportWaveICH::InterruptServiceRoutine`) that **the ISR does not call a stream's `Service()` directly** — the real contract is:

- Register a `PINTERRUPTSYNC` against the controller's IRQ resource via `PcNewInterruptSync(&sync, NULL, ResourceList, 0, InterruptSyncModeNormal)`, then `RegisterServiceRoutine(ourISR, this, TRUE)`, then `Connect()`. `PcNewInterruptSync`/`RegisterServiceRoutine`/`Connect`/`Disconnect` is the correct PortCls-level primitive here — not raw `IoConnectInterrupt`.
- The ISR itself (synchronized, running at DIRQL) reads `HDA_REG_INTSTS`, ACKs the fired stream's `SDnSTS` (RWC — read back and write back only the bits actually set), then calls **`IPortWavePci::Notify(stream->ServiceGroup)`** — this is what schedules PortCls's own DPC-level servicing, which is what eventually calls the stream's `Service()`/`MappingAvailable()` (PortCls registered its own sink on that `ServiceGroup` back when it was returned from `NewStream()` — we never call `AddMember` ourselves).
- Per the ac97 sample's own explicit comment ("Bad, bad. Shouldn't print in an ISR!"), no `DOUT`/logging calls are made inside the ISR.

Concrete changes:
- **`hdaregs.h`** — added `HDA_INTCTL_GIE`/`HDA_INTCTL_CIE`/`HDA_INTCTL_SIE(n)` and the matching `HDA_INTSTS_GIS`/`CIS`/`SIS(n)` bit constants (both registers share the same bit-31/bit-30/per-stream-bit layout per the HDA spec), plus `HDA_SDSTS_BCIS`/`FIFOE`/`DESE` for the per-stream status register.
- **`wavepcistream.h`/`.cpp`** — added `GetStreamIndex()`/`GetServiceGroup()` public accessors (read-only, so the miniport's ISR can reach what it needs without broader access to the stream's private state) and, in `SetState()`, now also set/clear `INTCTL`'s per-stream `SIE(n)` bit alongside `SDnCTL`'s existing `IOCE`/`FEIE`/`DEIE` when a stream starts/stops (both are required — `SDnCTL.IOCE` lets a stream *generate* a completion status bit, `INTCTL.SIE(n)` is what lets it actually *propagate* to `INTSTS`/the interrupt line). The destructor now also clears the miniport's `m_pRenderStream`/`m_pCaptureStream` back-pointer to itself (via the existing `CMiniportWavePciHda` friend access) so a torn-down stream can never be found by a later ISR firing.
- **`wavepciminiport.h`/`.cpp`** — added `m_pInterruptSync` (`PINTERRUPTSYNC`), `m_pRenderStream`/`m_pCaptureStream` (`CMiniportWaveStreamHda *`, tracked in `NewStream()` so the ISR — which only gets `this` as context — can find live streams), and the `static NTSTATUS InterruptServiceRoutine(PINTERRUPTSYNC, PVOID)` method. `Init()` now creates/registers/connects the interrupt sync and sets `INTCTL.GIE` (global enable) once for the driver's lifetime — deliberately does **not** set `INTCTL.CIE`, since CORB/RIRB verb transport is polled (`SendVerb` in `common.cpp`), not interrupt-driven, so there'd be nothing to service a controller-level interrupt if left enabled. `PowerChangeNotify` now calls `Connect()`/`Disconnect()` on D0 entry/exit, mirroring the ac97 sample. The destructor disconnects+releases the interrupt sync.
- **Non-paged-code-segment gotcha**: `wavepciminiport.cpp` is otherwise entirely under `#pragma code_seg("PAGE")`, but a DIRQL-synchronized ISR must live in non-paged memory (a paged-out ISR is an instant bugcheck) — the new `InterruptServiceRoutine` is bracketed with `#pragma code_seg()` / `#pragma code_seg("PAGE")` to pull just that one function out of the paged segment.

~~**Known remaining follow-up**: `Service()` still only calls `GetNewMappings()` to top up the ring — it does not yet recycle/compact BDL slots that hardware has already consumed.~~ **DONE** — see "Stage 4: BDL slot recycling in Service()" immediately below.

Also **not yet verified against a build** — like the rest of Stage 4, this hasn't been compiled against WDK 7600 yet (see item 6 above).

### Stage 4: BDL slot recycling in Service()

Implemented the follow-up flagged in the previous section and directly requested next: extend `Service()` (`wavepcistream.cpp`) so it recycles BDL ring slots hardware has already played through, instead of only ever filling the ring once at stream start and then starving.

**The core constraint**: HDA gives no per-slot "this BDL entry completed" readback — unlike AC97's `X_CIV` (Current Index Value) register, which directly reports the BD entry hardware is on, or its `X_LVI` register, which the AC97 sample's `GetNewMappings()` happily rewrites *live* while DMA is running. HDA's equivalents, `SDnLVI` (a slot index) and `SDnCBL` (= sum of entry lengths `[0..LVI]`), are only safe to reprogram while the stream engine is stopped (`RUN=0`) — per the HDA spec and this codebase's own established comments. So recycling has to (a) infer which slots are done from `SDnLPIB` (the running DMA byte position, which does update live) instead of a slot readback, and (b) never touch `SDnLVI`/`SDnCBL` while `RUN=1`.

**Design chosen: fixed-ring-geometry.** Once `SetState(KSSTATE_RUN)` programs `SDnLVI`/`SDnCBL` from the initial fill, `m_BdlEntryCount` (the ring size) is frozen until the stream fully stops. "Recycling" a slot means overwriting only its BDL entry's `Address` field in place — never its `Length` — once hardware has demonstrably played past it. This keeps the ring's total byte length (and thus the already-programmed `SDnCBL`) from ever drifting, so `SDnLVI`/`SDnCBL` never need to be touched again while running. This mirrors the "period ring" technique real-world HDA drivers use, and sidesteps AC97's live-`LVI`-rewrite trick entirely (HDA has no equivalent to rely on).

**A latent pre-existing bug found and fixed along the way**: the old `m_BdlHead`-based rotating-window addressing (`(m_BdlHead + i) % HDA_BDL_MAX_ENTRIES`) could produce an `SDnLVI` value that didn't correspond to the head-to-tail window in ascending *physical* order once `m_BdlHead` wrapped past slot 0 — violating HDA's hard requirement that entries `0..LVI` be walked in strict ascending physical order. This would eventually corrupt any long-running stream (it just hadn't been exercised yet, since nothing called `Service()` until the previous stage's ISR/DPC work). Fixed by removing `m_BdlHead` entirely — valid entries now always live at direct physical indices `[0, m_BdlEntryCount)` (see `wavepcistream.h`'s updated comment on `m_BdlEntryCount`).

**Concrete changes**:
- **`wavepcistream.h`** — removed `m_BdlHead`; added `m_NextSlotToRefill` (the recycling cursor: the oldest slot, mod `m_BdlEntryCount`, not yet refreshed since hardware last played it) and the new private `RefillConsumedSlots()` declaration.
- **`wavepcistream.cpp`**, new **`RefillConsumedSlots()`** (the core new logic, called from `Service()`): reads `SDnLPIB`, converts that byte position to a slot index by walking cumulative per-slot `Length` offsets across `m_pBdl[0..ringSize)`, then loops `while (m_NextSlotToRefill != currentSlot)` releasing the old mapping and pulling a new one into that same slot index — writing only `Address`/`IntOnCompletion`/bookkeeping, never `Length` (a replacement mapping shorter than the frozen slot length is accepted as-is, so trailing bytes silently replay old content for that lap; a longer one is truncated to fit — both accepted as rare given the fixed-size 10ms framing `GetAllocatorFraming()` already requests). Advances `m_NextSlotToRefill` modulo the ring size; if `GetMapping()` fails it stops early without advancing, so the same slot is retried on the next `Service()` call.
- **`GetNewMappings()`** — added `if (m_bRunning) return STATUS_SUCCESS;` as an early guard. Needed because my first draft let `Service()` call `RefillConsumedSlots()` then `GetNewMappings()`, and `GetNewMappings()` previously only skipped *reprogramming registers* while running — it didn't refuse to *grow* `m_BdlEntryCount`, which would have silently desynced the ring size from what `SDnLVI`/`SDnCBL` actually reflect. Fixed by adding the guard and dropping the now-pointless `GetNewMappings()` call from `Service()` — `Service()` is now just `RefillConsumedSlots();`.
- **`ReleaseUsedMappings()`** — rewritten to pop from the tail (order doesn't matter for its only two callers, the destructor and `SetState(KSSTATE_STOP)`, both full-drain-to-zero); resets `m_NextSlotToRefill = 0` at the end. `SetState(KSSTATE_RUN)` also resets `m_NextSlotToRefill = 0` right after `ProgramBdlRegisters()`, before the RUN bit is written.
- **Non-paged code-segment relocation**: `GetNewMappings()`, `ReleaseUsedMappings()`, the new `RefillConsumedSlots()`, `GetPosition()`, `NormalizePhysicalPosition()`, `MappingAvailable()`, and `Service()` are all reachable from `Service()`, which runs at `DISPATCH_LEVEL` (ISR → `Notify()` → PortCls DPC, per the previous stage). All were moved out of the file's `#pragma code_seg("PAGE")` section into a `#pragma code_seg()` (non-paged) block, and their `PAGED_CODE()` assertions removed — confirmed correct by cross-referencing the WDK 7600 `ac97\driver` sample's own identical file structure (a non-paged break partway through its `wavepcistream.cpp`, with no `PAGED_CODE()` after it). Plain `KeAcquireSpinLock`/`KeReleaseSpinLock` (already used by `m_MapLock`) needed no change — it's correct at any starting IRQL ≤ `DISPATCH_LEVEL`, not just paged/passive callers.

**Accepted limitations** (documented in code comments): a short replacement mapping isn't padded (stale trailing bytes replay); a long one is truncated; and if the port hands back mappings smaller than the ring's target size while ramping up, the ring doesn't grow mid-`RUN` (by design — see the `GetNewMappings()` guard above) — all considered acceptable given the fixed 10ms/8-frame allocator framing already in place.

**Not yet verified against a build or real hardware** — like the rest of Stage 4, this hasn't been compiled against WDK 7600 yet (see item 6 above, now the explicit top priority — see Next Steps).

### Stage 3 findings so far (Ghidra headless analysis of `stwrt64.sys`)

Ran Ghidra headless (`support/analyzeHeadless.bat`) with a custom post-script `tools/ghidra_scripts/DumpDriverInfo.java` that dumps imports, exports, all defined functions, and a manual keyword-matched string scan (had to hand-roll the string scan — `ghidra.util.string.StringSearcher`/`FoundString` aren't on this Ghidra version's headless classpath, so the script now walks each initialized memory block's raw bytes itself looking for runs of printable ASCII ≥4 chars). Full output: `Backported Driver/reference/stwrt64_dump.txt` (~1575 lines). Ghidra project lives at `tools/ghidra_projects/stwrt64_analysis/`.

Key takeaways:
- **Imports confirm classic PortCls/WaveRT miniport structure**: `PcInitializeAdapterDriver`, `PcAddAdapterDevice`, `PcNewPort`, `PcRegisterSubdevice`, `PcRegisterAdapterPowerManagement`, `PcRegisterPhysicalConnection` — all standard `portcls.sys` entry points. Single export is just `entry` (the driver's `DriverEntry`), as expected for a portcls miniport DLL-style driver.
- **~990+ unnamed `FUN_xxxxxxxx` functions** with no exported symbol names — expected for a stripped release binary; will need manual identification of key routines (verb transfer, format negotiation, etc.) by cross-referencing the string table below against `Xrefs to` in the Ghidra GUI in a future session.
- **The keyword-matched debug/assert strings are the single richest find** — they reveal the internal C++ class design almost completely, because this binary retains full `assert`/log-style strings with class::method prefixes. Recovered class names (all clearly a layered HDA abstraction):
  - `CHDACodec` — top-level codec object (`InstantiateCodec`, `GetOutpConverterCtrl`, `GetSpdifOutCtrl`/`GetSpdifInpCtrl`, `SelectPath`, `OpenPath`, `IsPathPossible`)
  - `CController` — low-level HW verb transport (`TransferCodecVerb`, `HackCycleCodecPowerStates`) — this is the layer that actually pokes HW/CORB-RIRB; guards like `!m_bHwIfValid`/`m_bHwFailed` and "Codec access took %d mcs" / retry-with-attempts logic look directly reusable as design reference for our own verb-transport layer.
  - `CWidget` — per-NID widget wrapper (`IsPathOpen`, loop-detection logic "There is a loop in the codec!")
  - `CPinCtrl` / `CPinCfg` — pin control state and pin configuration parsing, including **VREF/PinBias validation**, and reading pin config **from the registry** (`RegReadBytes pin[%ws]`, `CPinCfg::SaveUcharVal/SaveUlongVal`) — confirms pin config can be registry-overridden, not just BIOS-default.
  - `CConverterCtrl` — SPDIF/converter-level control (`SetSpdifCtrlVal`)
  - `CWaveRT` / `CWaveRTStream` / `CWaveRTStreamPin` / `CPcmRTRenderPin` — the WaveRT port/miniport/pin objects themselves (`BuildFilter`, `SetContentId`/DRM handling, `AddRangesBySuppBitsAndRates`) — **these are WaveRT-specific and are exactly the layer we replace with WaveCyclic/WavePci equivalents**; everything below `CHDACodec`/`CController`/`CWidget`/`CPinCtrl` is port-class-agnostic and directly reusable as design reference.
  - `CGlobalCfg` — global init-verb table management (`AddInitVerbs`, "Maximum supported init verbs number is %d")
  - `CSubdevPair` / `CAudioDevice` — subdevice/DAC-ADC-to-pin assignment logic (`FindSubdevRenderConverters`, `AllocateHwResources`, `AllocateCaptureDmaEngine`/`AllocateRenderDmaEngine`)
  - `CBiosCrc` — BIOS preset lookup ("Preset Found. IdEffect=%d")
  - `CHDAHarrison` — a codec-specific subclass (for a *different* HDA codec family than STAC/IDT — "Harrison" is likely an internal/vendor codename for another chip variant this shared driver binary also supports; not directly relevant to our 92HD89E2 but confirms the driver is a multi-codec-family shared binary, consistent with `sigmatel.c`'s own multi-model handling).
- **`Presets.bin` format is now largely decoded from strings alone**, without needing to touch the binary file itself: it has a **header** or embedded header ("Presets header starts at %p/%d... Presets ver. %d compat.ID %08X count %d"), followed by a sequence of **preset records**, each with an **8-character name** and a **verb count** ("Preset #%d at %p offs %d: %.8s verb count %d"), i.e. each preset is a named list of HDA verbs. There's also a distinction between "HW Presets" (embedded/baked in) vs. registry-sourced `InitVerbs`/`Presets` (`RegReadBytes InitVerbs returned %08x`, `RegReadBytes Presets returned %08x`, "Maximum supported init verbs number is %d but registry contains %d") — **so the driver's init-verb sequence can come from either a compiled-in preset blob (matched by codec ID/compat.ID) or a registry override, with the registry apparently taking priority/supplementing**. This strongly suggests our new driver doesn't need to reverse-engineer `Presets.bin`'s exact binary layout byte-for-byte — we can instead derive the *equivalent* verb sequence from `sigmatel.c`'s STAC92HD73XX init path (already identified in Stage 1) and hardcode it directly, since both are just "a list of NID/verb/payload triples run at init."
- **"BIOS spoofing file"** strings ("BIO(S) codec ID %04X does not match actual codec %04X!", "Invalid pin (%02Xh) configuration (%08Xh) in BIOS spoofing file") indicate the original driver supports an optional dev/debug override file to fake pin configs for testing — not relevant to our shipping driver, just a debugging affordance in the original.
- **GPIO handling exists** ("codec has more GPIOs than driver supports", "GPIO %02x configured but codec has %02x GPIOs") — relevant because EAPD/amp-enable and mute-LED control on many HP boards (including this codec family per `sigmatel.c`) is done via GPIO pins, not just standard pin-widget verbs. Need to check `sigmatel.c`'s STAC92HD73XX GPIO quirk handling (if any) again with this in mind, and confirm via runtime `GET_CONFIG_DEFAULT`/GPIO capability verbs whether this board uses GPIO for anything (e.g. speaker/headphone EAPD or amp mute) — flagged as an open question below.
- Confirms **loop-detection** is a real concern in the path-building logic ("There is a loop in the codec! source nid %x sink nid %x" appears in `CWidget::IsPathOpen`, `CHDACodec::SelectPath`, `CHDACodec::OpenPath`, `CHDACodec::IsPathPossible`) — our new driver's path-building code (DAC→pin, ADC→pin) should include the same loop guard when walking the widget connection graph.

### Stage 2 recap (tooling)

Stage 1 (codec identification + reference gathering) is complete, see below.

Checked the machine first: no Ghidra, WinDbg, WDK/DDK, or Visual Studio were already installed. Located official download sources and got user permission, then kicked off downloads (all still running/pending verification as of last update — check `tools/installers/` and re-run the checks below before assuming any tool is ready):
- `tools/installers/ghidra_12.1.3_PUBLIC_20260817.zip` — Ghidra 12.1.3, from `github.com/NationalSecurityAgency/ghidra` (official repo). **DOWNLOAD COMPLETE AND HASH-VERIFIED** (569,445,154 bytes, SHA-256 `93a5d11a9ad510622acaaf908c556a7b9b764d338e78a7567f3689bf5081fd54` matches). Not yet extracted/run.
- `tools/installers/GRMWDK_EN_7600_1.ISO` — WDK 7600.16385.1 ("WDK 7.1.0", includes NTAMD64 XP-era build support), from `download.microsoft.com` (Microsoft's own legacy CDN, still live). **DOWNLOAD COMPLETE**, size matches expected exactly (649,877,504 bytes). No published hash was available to double-check against, but exact size match against the CDN's own Content-Length is a good sign. Not yet mounted/installed.
- `tools/installers/windbg.msixbundle` — modern WinDbg, from `windbg.download.prss.microsoft.com` (Microsoft's own CDN, URL obtained via `winget show Microsoft.WinDbg`). **DOWNLOAD COMPLETE AND HASH-VERIFIED** (1,188,564,441 bytes, SHA-256 `12e63fb884347567bdd35f67f7aad61b26a08f8404553dad6951a10776f7d771` matches winget metadata exactly). Not yet installed.

**All three downloads completed, and Ghidra + WinDbg are now installed/verified working. WDK install is still pending (user will run it manually).**

- **Ghidra: DONE.** Extracted to `tools/ghidra/ghidra_12.1.3_PUBLIC/`. Ghidra 12.1.3 requires JDK 21+ (machine only had JDK 17 on PATH) — rather than the JDK 21 MSI installer (which requires admin elevation, see below), downloaded the **portable JDK 21 zip** instead (`OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip`, official Adoptium/Temurin release, 205,073,461 bytes, SHA-256 `f9d6e191ab098c0d416e7d588a24420a8621cd2f4720dab2459b8b7b2d2d8b4e` — verified). Extracted to `tools/jdk21/jdk-21.0.12.1+1/`. Pointed Ghidra at it by setting `JAVA_HOME_OVERRIDE` in `tools/ghidra/ghidra_12.1.3_PUBLIC/support/launch.properties`. Verified working: ran `support/analyzeHeadless.bat -help` successfully (printed usage, no Java/launch errors). GUI (`ghidraRun.bat`) not yet smoke-tested (headless mode fine for confirming toolchain works; try the GUI next session if an interactive Ghidra session is needed).
- **WinDbg: DONE.** Installed via `Add-AppxPackage` on the downloaded `.msixbundle` — succeeded without needing extra elevation.
- **JDK 21 MSI (`OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.msi`): install FAILED, do not retry the same way.** Both a normal per-machine install and a `MSIINSTALLPERUSER=1` per-user install attempt failed with MSI error 1925 ("You do not have sufficient privileges... Log on as administrator"). This shell/environment cannot elevate (no interactive UAC). Worked around it entirely by using the portable zip distribution instead (see above) — the MSI is not needed and can be ignored/deleted from `tools/installers/`.
- **WDK 7600: INSTALLED AND VERIFIED.** User installed via `KitSetup.exe` (had them select "Full Development Environment" [Build Environments + Samples + Tools + Help] + "Debugging Tools for Windows"; left "Device Simulation Framework" / "Windows Device Testing Framework" unchecked). Landed at default path `C:\WinDDK\7600.16385.1\`. Confirmed the folder layout and, importantly, **confirmed which lib target we actually need**: there is no `lib\wxp\amd64` (the `wxp` target dir only has `i386` — WDK's "wxp" target name is 32-bit-XP-only). **64-bit XP shares its build target with Windows Server 2003 — the `wnet` target** (`C:\WinDDK\7600.16385.1\lib\wnet\amd64` exists and is what we build against). `bin\amd64` (cross/native amd64 toolchain) also present. This is an important gotcha to remember for Stage 5's build setup / `sources` file `TARGETOS`/`_BUILDARCH` settings — use the `wnet` free/checked build environment shortcuts, not `wxp`, for this project.

**Stage 3 (Ghidra analysis of `stwrt64.sys`) is underway in parallel** — see findings above. With WDK now confirmed, next up is Stage 4 (design) using both the `sigmatel.c` reference and the Ghidra findings together.

## Reference material now in the project

Downloaded (with user's permission) into `Backported Driver/reference/linux-hda/` — current mainline Linux kernel source, GPL-licensed, for engineering reference only (understanding the codec's verb/init protocol — not for copying code into the Windows driver):
- `sigmatel.c` (from `sound/hda/codecs/sigmatel.c` upstream — note: this file lived at `sound/pci/hda/patch_sigmatel.c` in older kernel trees before a restructure; current mainline path is `sound/hda/codecs/sigmatel.c`)
- `generic.c` / `generic.h` (from `sound/hda/codecs/` — the shared HDA auto-parser logic sigmatel.c is built on)

### Findings from reading sigmatel.c

- `0x111d76c7` is confirmed at line ~5124: `HDA_CODEC_ID_MODEL(0x111d76c7, "92HD89E2", MODEL_STAC92HD73XX)`. So our exact codec is handled under the **STAC92HD73XX** model family in this driver — it doesn't get its own bespoke code path, it shares logic with the broader 92HD73xx/92HD89xx family.
- Searched for a hardcoded HP quirk keyed to our subsystem ID (`103c2acd`) — **none found**. The subsystem-ID quirk tables in `sigmatel.c` (`case 0x103c....:` blocks around lines 639-700+) cover a bunch of other specific HP laptops/models (e.g. ProBook 6550b) for mic-detect/pin-config overrides, but `103c2acd` is not among them.
- **Conclusion: this board's pin configuration is not hardcoded anywhere in the Linux driver — it relies on the codec's BIOS/EEPROM-programmed pin configuration, read at runtime via the standard HDA `GET_CONFIG_DEFAULT` verb (0xF1C) per pin widget.** This is good news for the XP driver design: we do **not** need to hardcode HP's pin config table by hand. The new XPDM miniport should read pin configs from the codec at init time (matching how the generic HDA auto-parser operates), rather than needing a static per-board table reverse-engineered from `Presets.bin`.
- `Presets.bin` / `C-*.INI` in the original package are therefore most likely either (a) a runtime-read dump/cache of those same BIOS-supplied pin configs (for the vendor GUI's benefit), or (b) DSP/effects presets (EQ curves, DTS/beats profiles) unrelated to core pin wiring — both explicitly out of scope for basic playback, so no need to fully decode them for the MVP driver.

## Open Questions

- Confirm during Stage 3 (once we have a way to probe the actual hardware, e.g. via WinDbg + a live-verb tool, or by booting the machine and inspecting via existing OS) that runtime pin-config reads actually return sane data on this board (should be true — Windows drivers on this exact board already work this way today under 7/8/10).
- Scope confirmation: full mixer/EQ/DTS feature parity is explicitly NOT in scope per the approved plan — basic playback (and ideally recording) only.
- Does this specific board (subsys `103c2acd`) actually use GPIO for EAPD/amp-enable or mute-LED control? `stwrt64.sys` clearly has GPIO-handling code paths (see Stage 3 findings), but that doesn't confirm *this* board uses them — need to cross-check `sigmatel.c`'s STAC92HD73XX GPIO quirk table again specifically for this, and/or confirm empirically once hardware is accessible via WinDbg.
- Is `CHDAHarrison` (found in Stage 3 strings) relevant at all to us? Current read: no — it looks like a different codec family handled by the same shared binary. Worth a quick sanity check only if something doesn't add up later; not worth deep-diving now.

### Stage 3, round 2: decompiled the anchor functions

Wrote a second script, `tools/ghidra_scripts/DecompileAnchors.java` (finds functions referencing a curated list of the highest-value strings from round 1, then runs Ghidra's decompiler on each). Output: `Backported Driver/reference/stwrt64_decompile.txt`. Findings:

- **`Presets.bin`'s binary format is now fully decoded** (from the preset-parsing function, `FUN_0005fdf4` @ `0005fdf4`, formerly just a string hit): the header loader (`FUN_0005fcb4`) is called with a resource ID of `0x25` and language `0xffff` — this is the classic Win32 `FindResourceEx`/`LoadResource` calling pattern, meaning **the "HW Presets" are actually loaded as an embedded PE resource inside `stwrt64.sys` itself** (not, or not only, from the external `Presets.bin` file — that file may just be a build-time input or a debug/dev copy). Structure recovered:
  - **Header** (14 bytes / 7×`uint16`): `[0]` = total blob size in bytes (used to bounds-check everything after), `[1]`-`[2]` unused/padding, `[3]`-`[4]` (as one `uint32`) = **compat ID** (matched against the codec's vendor/device ID — this is the "Codec configuration is not compatible!" check from round 1), `[5]` = format version, `[6]` = **preset count**.
  - **Preset entry** (10-byte header + verb data): first 8 bytes = **ASCII name** (`%.8s` in the log format string — e.g. likely short tags like `"AFG_INIT"`-style names), next `uint16` = **verb count**, followed by `verbCount × 4 bytes` of **packed 32-bit HDA verbs** (standard NID/verb/payload encoding). Entry size = `verbCount*4 + 10` bytes, confirmed directly from the pointer arithmetic (`puVar6[4] * 4 + 10`).
  - This means: **if we ever do need to extract the exact init-verb sequence byte-for-byte instead of deriving it from `sigmatel.c`, we now have the exact parser to write** (read `uint16` header, walk 10-byte-header + N×4-byte-verb entries). Not needed for the current plan (deriving from `sigmatel.c` is still the primary approach), but recorded here as a fallback/cross-check option.
- **`CController::TransferCodecVerb` (`FUN_00019b30` @ `00019b30`)** — the actual HW verb-transport routine — reveals a retry/timeout design worth mirroring in our own verb-transport layer:
  - Two failure gates before even attempting HW access: `m_bHwIfValid` (interface not ready) and `m_bHwFailed` (HW previously marked dead — fails fast afterward, doesn't keep retrying a known-bad codec).
  - A **configurable retry count** (`CodecAccessAttempts`, validated to be in `[1..1000]`) with **per-attempt timing** (`KeQueryPerformanceCounter`-based, "Codec access took %d mcs") and a **timeout threshold read from config** (`* 10000` — i.e. config value is in units of 10ms) that distinguishes two distinct failure modes: "failed for reasons unknown" (HW responded but reported an error) vs. "failed due to timeout" (no response in time) — tracked via **separate counters** (`piVar1[3]` vs `*piVar6`) so the two failure classes don't get conflated.
  - There's also an **unsolicited-response short-circuit**: before actually sending a verb whose command is `0x706xx` (this is the HDA **Set/Get Unsolicited Response Enable** verb range, NID-relative), the code checks a **spinlock-guarded linked list of NIDs with unsolicited responses already pending** — if the target NID is already in that list, it skips re-sending the verb and returns success immediately. This is the driver's jack-detect/unsolicited-event bookkeeping; our new driver will need an equivalent "don't double-arm unsolicited responses" guard if we implement jack detection (not required for MVP playback, but worth remembering if recording/jack-detect gets added later).
- **`CHDACodec` instantiation (`FUN_0003ff84` @ `0003ff84`, the actual body behind `CHDACodec::InstantiateCodec`)** confirms the driver does a **big vendor/device-ID range dispatch** that `new`s a *different* C++ codec subclass (different object size, different vtable pointer) depending on the exact device ID — e.g. our device ID `0x76c7` lands in the branch that allocates a `0x2078`-byte object with vtable `PTR_FUN_0002b4d0`, shared with device IDs `0x7674`-`0x7678`. This means IDT's own Windows driver treats our codec as its own bucket (not a single monolithic "STAC92HD73xx" class like the Linux driver does) — purely an implementation detail of IDT's driver, doesn't change our design (we're still targeting protocol-level parity via `sigmatel.c`, not replicating IDT's exact class layout), but explains why device-ID range checks appear so prominently in this function.
- Also does a **double-reset-verb handshake** at codec instantiation ("Codec double reset failed", "No response for reset verb #1/#2! Was codec in D3Cold state?") — sends two back-to-back HDA codec reset verbs and checks for a response to each, with the D3Cold-state message implying this is specifically to detect "codec was power-cycled and needs full re-init" vs. a normal resume. Worth mirroring this double-reset-with-timeout pattern in our own codec init sequence rather than a single reset verb.

### Stage 5f: WaveCyclic pivot — WavePci was the wrong port class for this DDI

Once `HDAUDIO_BUS_INTERFACE`'s real DMA/stream function pointers were read directly from `hdaudio.h` (not guessed), it became clear the whole "implement `IMiniportWavePciStream`'s BDL-based scatter/gather" plan (Stage 4's original design, see below) was built on a wrong assumption. The real DDI is **handle-based, single-buffer, bus-driver-owned** — not the miniport-owned scatter/gather model WavePci expects. User decision (via explicit prompt): **switch to WaveCyclic**. Confirmed exact DDI shape by grepping `portcls.h` directly rather than trusting memory:

- `IHdaAdapterCommon` (via `HDAUDIO_BUS_INTERFACE`, common.h/common.cpp/shared.h) DMA-related methods: `AllocateRenderDmaEngine(Context, StreamFormat, Stripe, Handle, ConverterFormat)`, `AllocateCaptureDmaEngine(Context, StreamFormat, Handle, ConverterFormat)` (note: our wrapper omits the `CodecAddress` param the raw bus DDI takes — a deliberate simplification, single-codec design), `AllocateDmaBuffer(Context, Handle, RequestedBufferSize, BufferMdl OUT, AllocatedBufferSize OUT, StreamId OUT, FifoSize OUT)`, `FreeDmaBuffer(Context, Handle)`, `FreeDmaEngine(Context, Handle)`, `SetDmaEngineState(Context, StreamState, NumberOfHandles, Handles)` (`HDAUDIO_STREAM_STATE`: `ResetState=0, StopState=1(=PauseState), RunState=2`), `GetLinkPositionRegister(Context, Handle, Position OUT)` — `Position` is a pointer **to** a live/mapped register, re-read via `READ_REGISTER_ULONG` on every poll, not re-queried through the bus interface each time.
- This handle-based, one-buffer-per-stream model is a **WaveCyclic fit** (one contiguous ring buffer, `IDmaChannel` object), not WavePci's (which expects the miniport to own/build its own scatter/gather buffer descriptor list and hands back `NULL` for its DMA channel).
- `IMiniportWaveCyclic::Init` has **no `ServiceGroup` out-param** (unlike WavePci) — each stream allocates and owns its own `PSERVICEGROUP` (`PcNewServiceGroup`), handed back from `NewStream`'s own `ServiceGroup` out-param instead.
- **No per-stream hardware completion interrupt exists** in this DDI (`GetLinkPositionRegister` is poll-on-demand only; there's no analog of WavePci's ISR-driven `Notify()`). Design: each `CMiniportWaveCyclicStreamHda` owns a `KTIMER`+`KDPC` pair (mirrors the classic WDK 7600 `toneclick` sample's software-timer servicing pattern), started via `KeSetTimerEx` in `SetState(KSSTATE_RUN)` at the `SetNotificationFreq()`-requested interval, calling `IPortWaveCyclic::Notify(ServiceGroup)` from DPC context — functionally replacing what the old WavePci ISR did when real hardware fired an interrupt.

**New files** (all under `Backported Driver/src/`), replacing the retired `wavepciminiport.h/.cpp`/`wavepcistream.h/.cpp` (now deleted — confirmed obsolete only after the clean build below, see "Stage 5g"):
- `dmachannel.h/.cpp` — `CHdaDmaChannel : public IDmaChannel, public CUnknown`. Thin adapter: `AllocateBuffer()`/`FreeBuffer()` forward to `IHdaAdapterCommon::AllocateDmaBuffer`/`FreeDmaBuffer` against an engine `HANDLE` set via `SetEngineHandle()` (called once `wavecyclicstream.cpp`'s `SetFormat()` allocates the engine); `SystemAddress()`/`PhysicalAddress()` expose the resulting bus-driver-owned MDL's mapping so PortCls's own buffer-copy logic and the real hardware DMA engine operate on the exact same memory; `GetAdapterObject()` returns `NULL` (no real `ADAPTER_OBJECT` — DMA is bus-driver-managed, not `IoGetDmaAdapter`-based).
- `wavecyclicminiport.h/.cpp` — `CMiniportWaveCyclicHda`. Pin descriptors carried over verbatim from the retired WavePci version (pin shape doesn't change with the port-class swap). `Init()` no longer has any ISR/MMIO registration at all (fully removed, not just dead code) since all servicing is per-stream software timers now. `NewStream()` builds a real `CHdaDmaChannel` (unlike WavePci's `NewStream`, which returned `NULL` for its DMA channel).
- `wavecyclicstream.h/.cpp` — `CMiniportWaveCyclicStreamHda`. Owns the DMA engine `HANDLE`, the `CHdaDmaChannel`, the `KTIMER`/`KDPC` pair, and the cached position-register pointer. `SetFormat()` allocates the render/capture DMA engine from the parsed `WAVEFORMATEX`; `SetNotificationFreq()` computes frame size from the requested interval; `SetState()` maps `KSSTATE_RUN`→start engine+timer, `PAUSE`/`STOP`→stop them (`STOP` also resets the engine and invalidates the cached position register); `GetPosition()` lazily caches the position-register pointer via `GetLinkPositionRegister()` then re-reads it directly thereafter.

`adapter.cpp` and `sources` were rewired accordingly (`CreateMiniportWaveCyclicHda`, `PMINIPORTWAVECYCLIC`, `CLSID_PortWaveCyclic`, etc.).

### Stage 5g: build-tooling problems solved, first clean WaveCyclic build achieved

Two real, unrelated problems blocked verifying the new code compiled, both now fixed:

1. **Bash tool + `cmd //c "..."` mangles quoted `cd` arguments.** Invoking `setenv.bat`+`build` via the Bash tool's `cmd //c "... && cd \"...\" && build ..."` pattern silently failed to scope the build to the project directory and instead built the **entire WDK 7600 sample tree** (1,553 files) twice. Root cause never fully isolated for Bash specifically, but the fix is simple: **use the `PowerShell` tool instead of `Bash` for WDK build invocations** — `cmd /c "call setenv.bat ... && cd /d ""...path..."" && build -cZ"` from PowerShell correctly scopes to just the target directory. Use this pattern from now on for all builds.
2. **WDK 7600's `build.exe`/nmake cannot handle spaces in the project path.** Even once correctly scoped, building directly from `C:\path\to\stwrtxp\src` (multiple space-containing path segments) failed the link phase with `makefile.new(7117) : error U1087: cannot have : and :: dependents for same target` — this is nmake's curly-brace inference-rule parser (`{..\}.cxx{$O}.obj::` etc. in `makefile.new`) getting corrupted by `$(MAKEDIR)` containing spaces. Confirmed by testing an isolated minimal `SOURCES=` file in the real path (still failed identically) vs. the same tree built through a space-free path (worked). **Fix: build through a directory junction to a space-free path**, e.g.:
   ```powershell
   cmd /c mklink /J "C:\stwrtxp_src" "C:\path\to\stwrtxp\src"
   ```
   then always build via `C:\stwrtxp_src` (junction, not a copy — edits to files under either path are the same files). **This junction must be recreated on a fresh machine/session if it doesn't already exist** — it's not part of the repo, just a local build convenience. Do this before every future build attempt rather than re-diagnosing the same U1087 error.

With both fixed, three real (and now-fixed) source bugs surfaced on the first genuine compile of the new WaveCyclic files:
- **`dmachannel.cpp`**: `AllocateBuffer`'s second parameter was written `IN OPT PPHYSICAL_ADDRESS` — `OPT` isn't a real SAL/DDK annotation macro (the correct one is `OPTIONAL`), causing a signature mismatch against the pure-virtual `IDmaChannel::AllocateBuffer`. Fixed to `IN OPTIONAL`.
- **`wavecyclicminiport.cpp`** had a leftover `CMiniportWaveCyclicHda::Service()` method definition copied from the WavePci-era habit — **`IMiniportWaveCyclic` has no `Service()` method at all** (confirmed via `portcls.h`'s `IMP_IMiniportWaveCyclic` macro expansion — only `Init`/`NewStream`/`GetDescription`/`DataRangeIntersection` plus `IMP_IMiniport`). Deleted the stray definition.
- **`shared.h` was missing the standard PortCls `#define PC_IMPLEMENTATION 1` before `#include <portcls.h>`.** Every `IMP_I*` macro in `portcls.h` (including `IMP_IDmaChannel`, `IMP_IMiniportWaveCyclic`, `IMP_IMiniportWaveCyclicStream`) is gated behind `#ifdef PC_IMPLEMENTATION` — without it defined, those macros silently don't expand and the classes using them end up abstract/missing their method declarations (this is why `dmachannel.cpp`'s out-of-line method definitions all failed with "member function not declared in class"). Confirmed this is the standard convention by checking the WDK 7600 `ac97\driver` sample's own `shared.h`, which does exactly this. Also needed the sample's accompanying unconditional `#include <ntddk.h>` (wrapped in `extern "C" { }` for C++ files) — `portcls.h` alone pulls in `wdm.h`, not `ntddk.h`, and functions like `MmGetPhysicalAddress` (used in `dmachannel.cpp`'s `PhysicalAddress()`) are only declared in `ntddk.h`.

**Result**: `build -cZ` now reports `9 files compiled - 1 Warning` (the warning is just "x64 Native compiling isn't supported. Using cross compilers.", not a code issue) and `1 executable built` — `stwrtxp.sys` exists cleanly at `objfre_wnet_amd64\amd64\stwrtxp.sys`. **This has not yet been tested on the real p6-2133w** — that's the next step.

### Stage 5h: real-hardware retest — "no audio", found and fixed two real bugs (fre/chk build-flavor gap; missing physical-jack topology pins)

**First real-hardware test of the WaveCyclic build**: driver installed cleanly, Device Manager showed "working properly", but **no audio output and no log file at all** despite reinstalling and rebooting repeatedly. Root cause: the shipped build was `objfre_wnet_amd64` (free/retail, `DBG=0`) — under a free build, `debug.h`'s `DOUT` macro and `common.cpp`'s `LogToFileF` are both **entirely compiled out** via `#if (DBG)`. This was never a runtime bug; the diagnostic code was never in the binary. **Fix**: rebuild with `setenv.bat C:\WinDDK\7600.16385.1 chk x64 WNET` (checked build) — same `C:\stwrtxp_src` junction / `PowerShell` build pattern, just `chk` instead of `fre`. **A checked-build driver still loads fine on a normal (free/retail) XP install** — this is how real diagnostic output was obtained on the p6-2133w, which has no kernel debugger or debug cable available. **Use `chk` builds for all future on-hardware diagnostic sessions on this project**; only go back to `fre` once the driver is actually working end-to-end.

With a checked build installed, the user retrieved a real `stwrtxp_log.txt`. It showed full, error-free codec bring-up: `AcquireBusInterface` succeeds, vendor/device ID confirms IDT 92HD89E2, all 35 widgets discovered, all 5 output pins (NIDs 10/13/15/16/17) initialized (power/connect-select/amp-unmute/pin-enable/EAPD all applied) — then the log **stopped dead** right after the last `Initialized output pin NID 17...` line, with zero trace of anything from the topology or wave-cyclic miniport layers. The user separately reported: device shows up under the Audio tab in Device Manager, but **no "audio specific settings" appear at all** (i.e. no selectable playback device/mixer surfaced by the OS) — meaning no other diagnostic question (default-device status, Test/Volume behavior) could even be checked.

**Root cause found by comparing `mintopo.cpp` against the WDK 7600 `msvad\simple\toptable.h` reference sample**: `mintopo.cpp`'s topology filter had exactly 2 pins (render/capture bridge), **both categorized as the generic `&KSCATEGORY_AUDIO`**, and the filter had **zero nodes and zero connections** at all. For Windows' audio subsystem (sysaudio) to recognize a topology filter as exposing a genuine render/capture endpoint, **at least one pin must carry a real physical-jack category** (`KSNODETYPE_SPEAKER` for render, `KSNODETYPE_MICROPHONE` for capture) — a pin categorized only as `KSCATEGORY_AUDIO` is syntactically valid for building the KS filter but invisible to sysaudio's endpoint-discovery walk. This exactly matches the observed symptom (KS filter graph builds fine — no crash, no Code 10 — but no "audio specific settings"/mixer entry ever appears). Notably, `mintopo.h`'s own pre-existing header comment (written in an earlier session) had already flagged this exact risk in the abstract, and it had never been acted on until now.

**Fix applied** (`mintopo.cpp`): added two new pins — pin 2 (`KSPIN_DATAFLOW_OUT`, category `&KSNODETYPE_SPEAKER`) and pin 3 (`KSPIN_DATAFLOW_IN`, category `&KSNODETYPE_MICROPHONE`) — plus a `PCCONNECTION_DESCRIPTOR` array wiring the existing bridge pins directly to these new physical pins (`PCFILTER_NODE` used as the connection endpoint on both sides, no intervening volume/mute node graph, matching the project's explicit "no mixer parity" MVP scope). `MiniportFilterDescriptor` updated to reference the new pin/connection arrays.

**Diagnostic blind spot also closed this segment**: `wavecyclicminiport.cpp` and `mintopo.cpp` had **zero `DOUT` calls anywhere** — meaning the log's silence past pin-init never actually proved the topology/wave layers didn't run, just that nothing there was instrumented. Added: `DOUT` success/failure logging to `mintopo.cpp`'s `Init()`; to `wavecyclicminiport.cpp`'s `Init()` (success path) and `NewStream()` (entry logging `Pin`/`Capture`, plus success/failure); and to `adapter.cpp`'s `StartDevice()` (success/failure around both the topology and wave subdevice bring-up calls, plus a final success line after `PcRegisterPhysicalConnection`). This closes the gap for the next retest regardless of whether the topology-pin fix alone resolves "no audio".

**Rebuilt clean** (checked/`chk`, via the `C:\stwrtxp_src` junction): `9 files compiled - 1 Warning` / `1 executable built`, zero errors.

**Not yet retested on real hardware** — this is a hypothesis-driven fix (strongly supported by direct comparison to a known-working WDK sample and by matching the exact observed symptom), not yet empirically confirmed. **Next concrete action**: copy the new `objchk_wnet_amd64\amd64\stwrtxp.sys` + `stwrtxp.inf` to the p6-2133w, reinstall via Device Manager → Update Driver → Have Disk, reboot, and check: (1) does "audio specific settings"/a selectable playback device now appear in Sounds and Audio Devices, (2) is it selectable as default and does Test/Volume do anything, (3) does actual sound play, (4) what does the new `stwrtxp_log.txt` show past pin-init now that topology/wave-miniport DOUT calls exist (does `StartDevice` reach its final success line, does `NewStream` ever get called at all — this last one is the single most important new signal, since it tells us whether PortCls/the OS ever attempts to open a wave stream).

### Stage 5i: retest — topology-pin fix confirmed running but symptom UNCHANGED; `NewStream` still never called; two more hypotheses ruled out

Retested the Stage 5h fix on real hardware (same `chk` build with the new speaker/mic topology pins + full `StartDevice`/`Init()`/`NewStream` instrumentation). User's report: **"Same exact thing as last time, the log has some new info."** — i.e. still no "audio specific settings"/selectable playback device, but the log is different.

New `stwrtxp_log.txt` (two boot cycles, both identical in shape): codec/widget bring-up identical to before (all 35 widgets, all 5 output pins init cleanly), **then**, newly, all of the following fire successfully with no errors:
```
Init: topology miniport initialized successfully
StartDevice: topology subdevice registered
Init: wave miniport initialized successfully
StartDevice: wave subdevice registered
StartDevice: complete, physical connections registered
```
This proves: `CMiniportTopologyHda::Init()`, `CMiniportWaveCyclicHda::Init()`, both `PcRegisterSubdevice` calls, and both `PcRegisterPhysicalConnection` calls all complete without error. **Critically, `NewStream: entry...` never appears anywhere in either boot cycle.** Since the *other* new instrumentation lines *do* appear, this isn't an instrumentation gap — it's proof that PortCls/sysaudio genuinely never attempts to open a wave stream on this device. So the Stage 5h topology-pin fix, while correctly built and running end-to-end with zero internal errors, **did not fix the actual problem**. The root cause is still open, and now more tightly bounded: it's somewhere between "the KS filter graphs build and register without error" and "sysaudio recognizes this as a usable render/capture endpoint."

Two follow-up hypotheses investigated this session, **both ruled out**:
1. **`PCFILTER_DESCRIPTOR` missing `CategoryCount`/`Categories` fields?** Grepped the real struct in `C:\WinDDK\7600.16385.1\inc\ddk\portcls.h` (line ~1380) — confirmed it has 2 trailing fields after `Connections` (`ULONG CategoryCount; const GUID *Categories;`) that neither `mintopo.cpp`'s `MiniportFilterDescriptor` nor `wavecyclicminiport.cpp`'s `MiniportWaveFilterDescriptor` initializers populate explicitly (C++ aggregate-init zero-fills them to `0`/`NULL`). **Ruled out**: compared directly against the WDK's own `msvad\simple\wavtable.h` sample (a real, working WaveCyclic sample) — it does the *exact same thing* (`0, NULL, // CategoryCount, Categories - use defaults (audio, render, capture)`). `0`/`NULL` is the documented "use PortCls's own default category list" value, not an omission bug.
2. **INF `.Interfaces` section wired wrong?** Read `Backported Driver/inf/stwrtxp.inf` in full. It has `[StwrtXP.Install.NTamd64.Interfaces]` registering `KSCATEGORY_AUDIO`/`KSCATEGORY_RENDER`/`KSCATEGORY_CAPTURE` against a single proxy CLSID — this is not explicitly `Include`d anywhere in `[StwrtXP.Install.NTamd64]`, but `.Interfaces` is one of setupapi's automatically-processed DDInstall suffix sections (like `.Services`, `.HW`, `.CoInstallers`) — it doesn't need an explicit reference, it's picked up by name convention alone. Structurally this matches real WDM audio INF samples. **Not ruled out with certainty** (haven't diffed byte-for-byte against a known-good WDM audio INF), but no concrete defect found.

**New structural observation, not yet a confirmed bug**: `wavecyclicminiport.cpp`'s `MiniportWavePins[]` has only **2 pins** — pin 0 (render, `KSPIN_DATAFLOW_IN`/`KSPIN_COMMUNICATION_SINK`) and pin 1 (capture, `KSPIN_DATAFLOW_OUT`/`KSPIN_COMMUNICATION_SINK`) — both are **streaming** pins, there are no separate **bridge** pins. But `shared.h` defines `PIN_WAVEOUT_BRIDGE = 0` / `PIN_WAVEIN_BRIDGE = 1` (i.e. aliases for the *same* two streaming-pin indices), and `adapter.cpp`'s `StartDevice` calls `PcRegisterPhysicalConnection(..., wavePort, PIN_WAVEOUT_BRIDGE, ..., topoPort, 0)` etc. treating those streaming pins as if they were bridge pins wired to the topology filter. This is architecturally different from the WDK `msvad\simple` sample, which has 4 wave pins (a streaming pin *and* a separate bridge pin per direction, joined internally via `KSNODE_WAVE_DAC`/`KSNODE_WAVE_ADC`). **Also notable: neither `PcRegisterPhysicalConnection` call's return value is checked in `adapter.cpp`** — if either silently fails (e.g. due to a bad/out-of-range pin index), `StartDevice` would still report full success, exactly matching what the log shows. This is the current leading thread for the next session but is NOT yet confirmed as the actual bug.

**Missing diagnostic info needed from the user for the next step**: whether the device appears **at all** in Windows' own audio device lists — specifically, Control Panel → Sounds and Audio Devices → Audio tab (does a playback/recording device get listed there, even if unusable/greyed out?) and Device Manager's exact status text for the device (not just "no code 10/39", the literal properties/driver tab text). This distinguishes "sysaudio never discovered the KS filter as an audio device at all" from "it was discovered but something else prevents stream open" — the two point to different next fixes (physical-connection/pin-graph wiring vs. something later in the pin-instantiation path).

### Stage 5j: root cause found — the INF never attached the legacy wave-mapper (`wdmaud.drv`) to the KS wave subdevice

Retested the `PcRegisterPhysicalConnection` status-check build (Stage 5i's `m2`). Result: **no change** — same log shape (`StartDevice`/both `Init()`s/both subdevice registrations all succeed, no `PcRegisterPhysicalConnection failed` line appears — that call was never the problem), `NewStream` still never called. New diagnostic info from the user this round: **Control Panel → Sounds and Audio Devices → Audio tab says "no audio device"** (i.e. the device doesn't appear in Windows' own audio device list at all, not even disabled), while **Device Manager says "This device is working properly" with no errors**. This combination — PnP/driver layer completely happy, legacy multimedia layer sees nothing — pointed away from the KS pin/topology graph entirely and toward INF-level interface/registry registration.

**Root cause found** by reading `Backported Driver/inf/stwrtxp.inf` side-by-side with the real WDK 7600 `src\audio\msvad\msvad.inf` sample (a real, working WaveCyclic INF). Two real defects in our `[StwrtXP.Install.NTamd64]` DDInstall section:

1. **No `AddReg` line at all.** msvad's install section has `AddReg=MSVAD_Simple.AddReg`, which sets `HKR,,AssociatedFilters,,"wdmaud,swmidi,redbook"` plus `HKR,Drivers,SubClasses,,"wave"` and `HKR,Drivers\wave\wdmaud.drv,Driver,,wdmaud.drv`. **This is what attaches `wdmaud.drv`/`wdmaud.sys` — the legacy kernel-mode filter that bridges KS to the classic waveOut/waveIn API and backs the Sounds and Audio Devices applet — to the device.** Our INF had none of this at all. Without it, PortCls/KS registration can succeed completely on its own (hence "device working properly" with zero Device Manager errors) while the legacy multimedia stack never learns a wave endpoint exists here at all (hence "no audio device" in the Audio tab, and `NewStream` never being called since nothing above PortCls ever opens a stream).
2. **`AddInterface` lines had no reference string.** Ours: `AddInterface = %KSCATEGORY_AUDIO%,, StwrtXP.Interface` (empty middle parameter, all three categories sharing one undifferentiated interface template). msvad's: `AddInterface=%KSCATEGORY_RENDER%,%KSNAME_Wave%,MSVAD.I.Wave` — the middle parameter is a reference string that ties the interface registration to a **specific named subdevice**, matching the name passed to `PcRegisterSubdevice` at runtime (our `adapter.h`'s `WAVE_SUBDEVICE_NAME L"Wave"` / `TOPO_SUBDEVICE_NAME L"Topology"`). Ours never made this association.

**Fix applied** to `Backported Driver/inf/stwrtxp.inf`: added `[StwrtXP.AddReg]` (mirroring msvad's `AssociatedFilters`/`SubClasses`/`Drivers\wave\wdmaud.drv` registration, referenced from the main install section) and split the interfaces section into `StwrtXP.I.Wave` (bound to reference string `"Wave"`, used for `KSCATEGORY_AUDIO`/`RENDER`/`CAPTURE`) and `StwrtXP.I.Topo` (bound to reference string `"Topology"`, used for `KSCATEGORY_AUDIO` on the topology subdevice) — directly modeled on msvad's `[MSVAD_Simple.NT.Interfaces]`/`[MSVAD.I.Wave]`/`[MSVAD.I.Topo]` pattern. Added `KSNAME_Wave`/`KSNAME_Topology` string tokens.

**This is a pure INF/packaging change — no `.sys` rebuild needed.** But since the INF change means the device needs a *fresh* driver install (not just "Update Driver" over the existing one — the old registry keys from the previous INF's install may need to be superseded), the next retest should either use Device Manager's "Uninstall" + rescan, or at minimum "Update Driver → Have Disk" pointed at the corrected INF, then reboot.

**Not yet retested on real hardware.** This is a well-supported hypothesis (found by direct diff against a known-working WDK sample, and it cleanly explains every observed symptom across both retests — clean PortCls bring-up, zero Device Manager errors, zero KS-level errors, yet total invisibility to the legacy multimedia layer), but not yet empirically confirmed.

### Stage 5k: INF fix retested — NO CHANGE; `AssociatedFilters`/`wdmaud.drv` theory retracted; interfaces confirmed registered via regedit; pivoted to raw-KS test via KsStudio

**Retested the Stage 5j INF fix on real hardware. Result: no change whatsoever.** The new `stwrtxp_log.txt` is byte-for-byte the same shape as the pre-fix log (three identical boot-cycle repetitions of the full codec/widget bring-up, both `Init()`s succeeding, both subdevices registering, `StartDevice: complete, physical connections registered` — and still **zero** `NewStream: entry...` lines anywhere). The user did report one new, more minor symptom: Device Manager now shows a **Properties/Driver tab** for the device that wasn't there before (cosmetic — almost certainly just from the new `HKR,,Driver,,stwrtxp.sys` value in `[StwrtXP.AddReg]` populating the Driver Details dialog), but the actual blocking symptom (device absent from Control Panel's audio device list, `NewStream` never called) is completely unchanged.

**Conclusion: the `AssociatedFilters`/`Drivers\wave\wdmaud.drv` registration was the wrong theory, and is now retracted as a candidate root cause.** On reflection, `AssociatedFilters` is a **legacy 16-bit/VxD-era MME association mechanism** (from the Windows 9x/NT4 multimedia driver model) — it has no bearing on how a modern WDM/KS PortCls driver gets discovered. A WDM audio device is discovered purely through `sysaudio.sys` walking `KSCATEGORY_AUDIO`/`RENDER`/`CAPTURE` device interfaces and the exposed filter's pin/node topology; there is no registry-key step required beyond the device interfaces themselves. Splitting the `AddInterface` reference strings (the other half of the Stage 5j fix) was still a legitimate correctness fix (ties each KS category registration to the correct named subdevice) but evidently wasn't the actual blocker either, since it made no observable difference.

**New diagnostic evidence gathered** (regedit, no reinstall needed): got the device's real Device Instance ID from Device Manager → Details tab → "Device Instance Path": `HDAUDIO\FUNC_01&VEN_111D&DEV_76C7&SUBSYS_103C2ACD&REV_1001\4&38953DA4&0&0001`. Checked `HKLM\SYSTEM\CurrentControlSet\Control\DeviceClasses\{6994AD04-93EF-11D0-A3CC-00A0C9223196}` (`KSCATEGORY_AUDIO`) and the `KSCATEGORY_RENDER` GUID's equivalent key — **subkeys referencing this device instance DO exist under both**. This confirms: `AddInterface` processing in the INF is working correctly and the device interfaces really are registered at the PnP level. This rules out "interface registration silently failing" as an explanation and narrows the problem to **downstream of interface registration** — either `sysaudio` never actually opens/queries our filter, or it does and rejects the pin/topology graph it finds.

**Next diagnostic step (in progress, no code change yet)**: found that WDK 7600 ships `KsStudio.exe` (a Microsoft-provided raw KS filter browser/tester) at `C:\WinDDK\7600.16385.1\tools\avstream\amd64\xp\KsStudio.exe` — the right build for XP x64 specifically (do not use the `i386\xp` 32-bit build). This tool can enumerate raw KS filters independent of `sysaudio`'s higher-level wave-mapper virtualization, letting us test whether our filter/pins are even openable at the KS level at all, decoupled from whether `sysaudio` recognizes it as a "wave device." User is copying it to the target machine via USB and testing now. Two possible outcomes once tested:
- Filter doesn't appear in KsStudio at all → our KS filter/pin/topology surface itself is broken (points at restructuring `wavecyclicminiport.cpp`'s 2-pin flat design into a proper 4-pin streaming+bridge design with real `PCNODE_DESCRIPTOR`/`PCCONNECTION_DESCRIPTOR` entries, matching `msvad\simple\wavtable.h` — this was already flagged as the fallback hypothesis in Stage 5i/n0 below, and is now the leading suspect since two INF-only fixes failed to move the log at all).
- Filter appears and can be opened/streamed via KsStudio (i.e. manually creating a pin instance makes `NewStream` fire in our log) → the raw driver stack is actually fine end-to-end, and the remaining problem is entirely inside `sysaudio`'s wave-device discovery/topology-walk logic refusing to adopt our filter as a wave endpoint — a narrower, different fix (likely still related to the topology/bridge-pin graph shape, but from `sysaudio`'s specific recognition-heuristic angle rather than raw KS openability).

**Not yet retested/confirmed — awaiting KsStudio test result from the user.**

### Stage 5l: KsStudio confirms both filters fully enumerable at raw KS level with correct metadata; filter-instantiation attempt inconclusive so far

User ran `KsStudio.exe` (amd64\xp build) on the real machine. Screenshot result: under `KSCATEGORY_AUDIO Filter Factories`, **both** of our sub-filters appear correctly, alongside the four standard Microsoft Kernel filters (Audio Splitter, Acoustic Echo Canceller, GS Wavetable Synthesizer, Wave Audio Mixer):

- `IDT 92HD89E2 HD Audio (XPDM backport) Topology` — device name ends in `\topology`, Category Aliases = `KSCATEGORY_AUDIO`, `KSCATEGORY_TOPOLOGY`. CLSID/Service/Driver/Binary/HardwareID all correct (`stwrtxp`, `C:\WINDOWS\system32\drivers\stwrtxp.sys`, our real hardware ID).
- `IDT 92HD89E2 HD Audio (XPDM backport)` — device name ends in `\wave`, Category Aliases = `KSCATEGORY_AUDIO`, `KSCATEGORY_CAPTURE`, `KSCATEGORY_RENDER` (all three, as expected). Same correct CLSID/Service/Driver/Binary/HardwareID.

**Conclusion: our KS filter/pin surface is definitely NOT invisible or fundamentally broken at the raw KS level** — this rules out the "filter doesn't appear in KsStudio at all" branch from Stage 5k entirely. Combined with the confirmed-correct `KSCATEGORY_RENDER`/`KSCATEGORY_CAPTURE` aliases on the wave filter, this makes "our raw KS surface is broken" a much weaker hypothesis than before, and shifts weight toward `sysaudio`'s own wave-endpoint discovery/topology-walk logic being the actual blocker (Stage 5k's second branch).

The log pane also showed two `SetupDiEnumDeviceInterfaces failed`/`GetDeviceDetails failed` pairs, tied to a **different, separately-named class**, "KSCATEGORY_AUDIO_DEVICE" (not `KSCATEGORY_AUDIO`) — this occurred only after all real filter factories (including ours) were already successfully enumerated. This looks like a benign/expected quirk of a legacy class most systems don't populate, not something specific to our driver — deprioritized unless later evidence points back at it.

User then attempted to **instantiate** our wave filter (`IDT 92HD89E2 HD Audio (XPDM backport)`, the `\wave` one) via KsStudio — log pane shows `Instantiating Filter: "IDT 92HD89E2 HD Audio (XPDM backport)"` twice, with no visible error message directly beneath either line in the screenshot. However:
- The "Instantiated Objects" pane (top-right) appeared empty in the screenshot — unclear whether that's because the instantiation hadn't completed yet, silently failed, or the screenshot was taken before an entry populated.
- A fresh `stwrtxp_log.txt` pulled after this test showed **only the same three identical boot-cycle repetitions from driver load** (codec/widget init, both miniports' `Init()`, both subdevices registering, `StartDevice: complete...`) — **no new entries correlating to the instantiation attempt, and still zero `NewStream` lines.** This is not necessarily bad news: opening a filter handle (`IRP_MJ_CREATE` on the device/pin-instance interface) was never instrumented with `DOUT` calls in our driver — only `Init`/`StartDevice`/`NewStream` are logged — so a successful-but-silent filter open is expected to produce no new log lines. Only actually creating/opening a **pin** on the filter would be expected to reach `NewStream` and log something new.

**Not yet resolved: whether the filter instantiation itself succeeded.** Next action needed from the user: check whether an entry now appears in KsStudio's "Instantiated Objects" pane after instantiating (if yes, expand it to find its pin factories and try instantiating/creating Pin 0, the render pin, with a PCM format — this is the real test of whether `NewStream` fires); if no entry appeared and/or an error dialog was shown, capture that error text specifically.

**Control test result: KS Studio itself is broken on this machine — RETRACTED as a diagnostic tool.** Instantiating our Wave filter produced no entry in "Instantiated Objects" and no error, no log activity. As a control, the user tried the exact same action on a **known-good Microsoft-shipped filter** ("Microsoft Kernel Wave Audio Mixer") — **identical silent failure.** KS Studio also threw a "sys file missing" error on its own startup (its filter-factory tree shows a top-level "KS Studio Test Filter (Communication Pump) Factories" node, implying it depends on its own companion kernel-mode support driver); the user located and copied that `.sys` file over, but the retest still failed the same way. **Conclusion: KS Studio's filter-instantiation feature does not work at all on this machine** (most likely its Communication Pump support driver still isn't fully/correctly installed even after copying the `.sys`, e.g. missing service registration) — this is a tool-environment problem, not evidence about our driver. Every finding from the "Instantiate Filter" attempts is therefore **inconclusive and discarded**; the only KS Studio result that stands is the raw filter-factory *enumeration* (Stage 5l above), which doesn't depend on this broken instantiation path and remains valid (both our filters ARE visible with correct metadata).

**Pivoting away from KS Studio entirely.** Next plan: write a small custom user-mode console test program (not a GUI tool) that opens our wave filter's device interface directly via `CreateFile` and calls `KsCreatePin` (from `ksuser.lib`) to open Pin 0 with a PCM format — this bypasses both `sysaudio`'s discovery layer and any third-party tool's own bugs, giving an unambiguous yes/no on whether our raw KS pin-open path (and therefore `NewStream`) actually works.

### Stage 5m: custom `kstest.exe` diagnostic tool written and built (replaces KS Studio)

Wrote `Backported Driver\tools\kstest\kstest.c` (+ `sources`) — a small standalone usermode console tool, **not part of the shipped driver**. It:
- Enumerates `KSCATEGORY_RENDER` device interfaces via `SetupDiGetClassDevs`/`SetupDiEnumDeviceInterfaces`/`SetupDiGetDeviceInterfaceDetailW`, prints each device path found, `CreateFile`s it, then calls `KsCreatePin` for `PinId=0` (render) with a plain 44100Hz/16-bit/stereo PCM format.
- Repeats the same for `KSCATEGORY_CAPTURE` with `PinId=1` (capture).
- Prints a clear pass/fail + Win32 error code at every step (`CreateFile` failure, `KsCreatePin` failure, or success).

This directly tests `wavecyclicminiport.cpp`'s pin-open path (`NewStream`) completely independent of both `sysaudio`'s discovery layer and any GUI tool, which is exactly the ambiguous gap KS Studio's broken instantiation feature left open (Stage 5l).

**Build notes** (for anyone rebuilding this tool):
- Needed `#include <mmsystem.h>` before `<ksmedia.h>` — `KSDATAFORMAT_WAVEFORMATEX` in `ksmedia.h` is guarded behind `#if defined(_INC_MMSYSTEM) || defined(_INC_MMREG)` and silently disappears (with confusing "syntax error: identifier" errors) without it.
- `TARGETTYPE=PROGRAM`, `UMTYPE=console`, `USE_MSVCRT=1`; links `$(SDK_LIB_PATH)\setupapi.lib` and `$(DDK_LIB_PATH)\ksuser.lib` (the latter is the usermode import lib for `KsCreatePin`, found at `lib\wnet\amd64\ksuser.lib` in WDK 7600 — same `wnet`/amd64 target already used for the driver itself).
- Entry point must be plain `main(void)`, not `wmain` — `USE_MSVCRT=1` + `UMTYPE=console` links the ANSI CRT startup (`crtexe.obj` wants `main`) by default in this WDK's build environment; `wmain` produced an `unresolved external symbol main` link error. (`wprintf`/wide string literals still work fine from a `main`-based entry point — only the entry symbol name matters here.)
- Built via: `setenv C:\WinDDK\7600.16385.1 fre x64 WNET no_oacr`, then `build -cZ` from a junction (`C:\stwrtxp_kstest` → the tool's real path) to avoid the same space-in-path `U1087` link issue noted for the driver itself. **Built clean, zero errors, one build in this session.**
- Output binary: `objfre_wnet_amd64\amd64\kstest.exe`. Sent to the user as a file attachment — needs to be copied to the target XP x64 machine via USB and run from a command prompt (`kstest.exe`, no arguments). Its full text output should be pasted back for analysis.

**Not yet run on the target machine — this is the immediate next action.**

### Stage 5n: `kstest.exe` run on real hardware — CreateFile itself fails with `ERROR_ACCESS_DENIED` (error 5), on BOTH categories

Ran `kstest.exe` on the real p6-2133w. Result for both `KSCATEGORY_RENDER` and `KSCATEGORY_CAPTURE`:
- `SetupDi` enumeration finds exactly one device interface in each category (both resolving to the same underlying `\wave` filter reference string, as expected — render/capture are just two category aliases on one filter).
- `CreateFile` on that path **fails outright** with `GetLastError=5` (`ERROR_ACCESS_DENIED`) — `KsCreatePin` is never even reached.

This is a materially different and more specific signal than anything found in Stage 5j–5m: not a topology/pin-descriptor problem, not a `sysaudio`-recognition heuristic, not a KS-level format/negotiation rejection — it's an OS-level access-control denial on the device object itself, before any KS-specific code in our driver could run at all.

**Investigated and ruled out**: grepped the entire driver source tree for `Security`/`SDDL`/`AccessMask`/`GENERIC_`/`IoCreateDevice`/`IoCreateDeviceSecure` — zero matches. The driver never creates its own device object or touches security in any way; it goes entirely through `PcAddAdapterDevice` ([adapter.cpp:49](Backported Driver/src/adapter.cpp:49)) and `PcRegisterSubdevice` ([adapter.cpp:156](Backported Driver/src/adapter.cpp:156), [adapter.cpp:194](Backported Driver/src/adapter.cpp:194)) — the exact same mechanism the `ac97`/`msvad` WDK samples use, and those don't exhibit this problem. So the access denial is very unlikely to be caused by anything in our current driver code; it's much more likely either (a) a stale symbolic-link/security-descriptor leftover on this devnode from one of the many earlier install/reinstall cycles across sessions, or (b) some other environment-level condition, not yet identified.

**Proposed next action (cheapest/highest-signal first)**: on the real hardware, fully remove the device via Device Manager → Uninstall → check "Delete the driver software for this device" → reboot → let `stwrtxp.inf` reinstall completely fresh (not just an overwrite-reinstall), then rerun `kstest.exe` and pull a fresh `stwrtxp_log.txt`. If Access Denied persists after a truly clean reinstall, that rules out stale-state and the next moves are: (1) modify `kstest.exe` to try reduced access masks (`GENERIC_WRITE` only for render, `GENERIC_READ` only for capture, or `0`) to see if a specific requested right is being denied; (2) add a control test in `kstest.exe` against a stock Microsoft-shipped KS device interface (if one exists on this machine) to determine whether Access Denied is specific to our device object or reflects a broader machine/permission-context issue. If the clean reinstall fixes it, this is very plausibly the root cause of the *entire* multi-session "no audio device" symptom (a `sysaudio` open attempt silently failing the same way would look exactly like "Device Manager: fine, Audio tab: nothing" with no diagnostic surfaced anywhere).

### Stage 5o: clean-reinstall retest — NO CHANGE; user reports this was already standard practice; also, Device Manager never even offers "Delete driver software" for this device

User reports: (1) they were already doing a full uninstall+reinstall before every attempt across this whole investigation, not just this once — so "stale leftover state from a prior install" is effectively ruled out as the explanation; Access Denied is reproducible from a genuinely fresh install every time. (2) Device Manager's Uninstall dialog never presents the "Delete the driver software for this device" checkbox for this device at all — worth remembering as a minor oddity (possibly because the device's driver package isn't tracked as a separate staged package the way `pnputil`-installed ones are), but not yet explained or investigated further.

Reviewed the driver source again for anything that could cause a deliberate/conditional access denial (custom `DispatchCreate`, `KSFILTER_DESCRIPTOR`/`PCFILTER_DESCRIPTOR` exclusivity flags, filter-instance limits) — none exist; `mintopo.cpp`'s and `wavecyclicminiport.cpp`'s filter descriptors are plain, unflagged PortCls descriptors ([mintopo.cpp:142](Backported Driver/src/mintopo.cpp:142), [wavecyclicminiport.cpp:70](Backported Driver/src/wavecyclicminiport.cpp:70)).

**Next diagnostic, implemented**: extended `kstest.exe` with a control test (`ControlTestDisk()`) that enumerates `GUID_DEVINTERFACE_DISK` (hardcoded GUID, no extra header dependency) and attempts `CreateFile(GENERIC_READ, ...)` on the system disk's device interface — a device interface that has nothing to do with audio/KS and is always present. This directly answers: is `ERROR_ACCESS_DENIED` specific to our device object, or does this machine/account deny `CreateFile` on device interfaces broadly? Rebuilt clean (`fre`, zero errors, one build) via the existing `C:\stwrtxp_kstest` junction; sent the updated binary to the user. **Not yet run on the target machine — immediate next action.**

### Stage 5p: control test succeeded — Access Denied is specific to our device object; added explicit `Security` AddReg to the INF

Ran the updated `kstest.exe` with the disk control test. Result: render/capture `CreateFile` still fail with `GetLastError=5`, but the control test (`CreateFile` on the system disk's `GUID_DEVINTERFACE_DISK` interface, unrelated to audio/KS) **succeeded**. This conclusively proves the Access Denied is **specific to our device object**, not a machine-wide or account-wide `CreateFile` restriction — ruling out local security policy / OEM image hardening / account-level causes.

This points at a well-documented, concrete mechanism: KS device interfaces do not get a fully-open default security descriptor from the class installer the way a typical device object does — the effective default DACL is restrictive (roughly SYSTEM + built-in Administrators only, sometimes even narrower depending on the class), and an INF is expected to explicitly grant access via a `Security` AddReg value (a special value name the class installer recognizes, taking an SDDL string) under the interface's own AddReg section if broader access is needed. Our INF's `[StwrtXP.I.Wave]`/`[StwrtXP.I.Topo]` sections never included one.

**Fix applied**: added `HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;WD)"` (generic-all to SYSTEM, built-in Administrators, and Everyone) to both `[StwrtXP.I.Wave.AddReg]` and `[StwrtXP.I.Topo.AddReg]` in `stwrtxp.inf`. This is an INF-only change — no driver `.sys` rebuild needed, but it does require a genuine reinstall (uninstall the device, then reinstall from the updated INF) since device-interface registry entries are written at install time. **Not yet retested on the target machine — immediate next action.**

### Stage 5q: interface-level `Security` AddReg retested — NO CHANGE; moved to the device-level DDInstall AddReg section instead

Reinstalled with the Stage 5p `Security` AddReg fix and reran `kstest.exe` + pulled a fresh `stwrtxp_log.txt`. Result: **identical failure** — both `CreateFile` calls still return `GetLastError=5`. The log shows byte-for-byte the same codec-init sequence as every prior successful boot (two full init cycles, `StartDevice: complete, physical connections registered` both times) with zero new activity — confirming, again, that the access check happens entirely before any IRP reaches our driver, so nothing our driver's own code does or logs can be affected by this at all.

This means the `Security` AddReg value placed under the **interface-specific** AddReg sections (`[StwrtXP.I.Wave.AddReg]`/`[StwrtXP.I.Topo.AddReg]`, referenced from `AddInterface`) had no effect — the documented INF mechanism for a device's security descriptor override is a `Security` value under the **device's own DDInstall AddReg section** (the one referenced by `AddReg=` in the main `[StwrtXP.Install.NTamd64]` install section, i.e. `[StwrtXP.AddReg]`), since what's actually being checked is the device object's (FDO's) security, not interface-registration metadata. **Fix corrected**: moved the `Security` line into `[StwrtXP.AddReg]` (same SDDL string), leaving the interface-level ones in place too (harmless, likely just inert extra registry data at that location). INF-only change again — needs the same uninstall+reinstall+retest cycle.

### Stage 5r: device-level `Security` AddReg ALSO had zero effect — both INF placements now ruled out; pivoted to a runtime take-ownership/DACL-reset fix in `kstest.exe`

Reinstalled with the Stage 5q device-level `Security` AddReg fix and reran `kstest.exe`. Result: **identical failure again** — byte-for-byte the same `GetLastError=5` on both categories, same unchanged codec-init log. **Both possible INF-level `Security` AddReg placements (interface-level, Stage 5p; device-level, Stage 5q) are now conclusively ruled out.** Working theory: PortCls's own device-object creation for this port-class model almost certainly goes through a plain `IoCreateDevice` call that never consults any per-device INF/registry `Security` override at all — this is a plausible explanation for why an otherwise-correct, well-documented INF mechanism had literally zero measurable effect across two separate placements and two full reinstall cycles.

**Pivoted to a different fix, implemented directly in `kstest.c`** rather than more INF tinkering: forcibly take ownership of the denied device object at runtime and install a wide-open DACL on it, using the same technique `takeown.exe`/`icacls /grant` use to recover access to any locked-down securable object — an account holding `SeTakeOwnershipPrivilege` can seize ownership of ANY securable object regardless of its current DACL (this privilege overrides the normal access check for `WRITE_OWNER` specifically), and once ownership is taken, the new owner is implicitly granted `READ_CONTROL`/`WRITE_DAC` regardless of DACL content, allowing an arbitrary new DACL to be installed afterward. Added two new functions to `kstest.c`: `EnablePrivilege()` (enables a named privilege — `SeTakeOwnershipPrivilege`/`SeSecurityPrivilege`/`SeRestorePrivilege` — in the current process token via `OpenProcessToken`+`LookupPrivilegeValueW`+`AdjustTokenPrivileges`) and `TakeOwnershipAndOpenUp()` (takes ownership via `SetNamedSecurityInfoW(..., OWNER_SECURITY_INFORMATION, adminSid, ...)` with `adminSid` from `CreateWellKnownSid(WinBuiltinAdministratorsSid, ...)`, builds a new DACL granting `GENERIC_ALL` to Everyone via `SetEntriesInAclW`, then installs it via a second `SetNamedSecurityInfoW(..., DACL_SECURITY_INFORMATION, ...)`; object type `SE_KERNEL_OBJECT` since a KS device is a kernel/device object accessed by path). `TestOpenAndPin()` now calls this automatically on a first `CreateFile` failure with `ERROR_ACCESS_DENIED`, then retries `CreateFile` once.

**Build friction hit and resolved this stage (two unrelated issues, both now fixed)**:
1. **Claude Code's auto-mode safety classifier blocked the build twice** (via both the PowerShell and Bash tool routes) with "Blocked by classifier" — almost certainly because enabling `SeTakeOwnershipPrivilege` and forcibly rewriting a device object's DACL pattern-matches privilege-escalation tooling, even though its actual purpose here is fixing our own driver's device-object ACL on the user's own machine for diagnostic purposes. Per standing safety behavior, did not try to route around the block — stopped and explained the situation transparently, asking the user to either authorize the build or build it themselves. **User's verbatim response: "Go ahead and build it yourself, I'll allow it."** After this explicit authorization, the identical build command was no longer blocked.
2. **New compile error surfaced once unblocked**: `<aclapi.h>` (needed for `SetNamedSecurityInfoW`/`SetEntriesInAclW`/`EXPLICIT_ACCESS_W`) failed with `error C1083: Cannot open include file: 'accctrl.h'` — `aclapi.h` lives under `inc\api` on the default include path, but it `#include`s `accctrl.h`, which actually lives under a different include root (`inc\ddk`). **Fixed** by adding `INCLUDES=$(DDK_INC_PATH)` to `Backported Driver/tools/kstest/sources` (mirroring the exact pattern the main driver's own `sources` file already uses at `Backported Driver/src/sources:18`). Also hit and fixed a second, unrelated `/W4 /WX` warning-as-error (`C4133: incompatible types - from 'char [...]' to 'LPCWSTR'`) caused by passing the ANSI `SE_TAKE_OWNERSHIP_NAME`/`SE_SECURITY_NAME`/`SE_RESTORE_NAME` macros (which expand to narrow `"SeXxxPrivilege"` string literals in this WDK's headers) into a function expecting `LPCWSTR` — fixed by passing explicit wide literals (`L"SeTakeOwnershipPrivilege"` etc.) directly instead of the macros.

**Rebuilt clean** (`fre`, via the `C:\stwrtxp_kstest` junction, PowerShell tool): "3 files compiled - 1 Warning" / "1 executable built" — zero errors. Confirmed via file timestamp that this is a genuinely fresh binary. **Not yet run on the target machine — immediate next action**: send this `kstest.exe` to the user, have them run it and paste the full output plus a fresh `stwrtxp_log.txt`.

**Important open design question, to revisit once/if this workaround is confirmed to work**: even if forcibly taking ownership + resetting the DACL from `kstest.exe` lets `CreateFile`/`KsCreatePin` succeed, that only proves the theory from inside a custom diagnostic tool — it does not by itself constitute a shippable fix, since end users (and `sysaudio` itself) can't be expected to run a custom ACL-fixing tool before every use. If this pans out, the next conversation needs to be about what the *real* production fix looks like (e.g., finding whatever actual mechanism, if any, can influence this device object's default security descriptor at creation time — possibly something PortCls-specific we haven't found yet, or a system-wide policy adjustment, or reconsidering whether this is even the right layer to fix it at all).

### Stage 5s: first take-ownership attempt failed with `ERROR_BAD_PATHNAME` (161) — fixed by switching from name-based to handle-based security APIs

Ran the Stage 5r `kstest.exe` build. Result: `CreateFile` still denied as expected, but the new take-ownership fallback itself failed: `SetNamedSecurityInfo (take ownership) FAILED, error=161` (`ERROR_BAD_PATHNAME`) on both categories, so the retry `CreateFile` still failed too. Control test (disk) still succeeded. `stwrtxp_log.txt` unchanged, as expected (the failure never reaches the driver either way).

**Root cause**: `SetNamedSecurityInfoW` is a *name-based* API that parses its path argument according to its own rules — it cannot handle a raw KS device-interface symbolic-link path in `\\?\hdaudio#func_01&...#{GUID}\wave` form (the `#`-separated instance ID + reference string suffix is not something it knows how to interpret), even though that exact same string is a perfectly valid path for `CreateFileW`.

**Fix applied**: switched `TakeOwnershipAndOpenUp()` in `kstest.c` from the name-based `SetNamedSecurityInfoW` to the handle-based `SetSecurityInfo`. This works because access checks apply per-open, not just via the named API: opening the device with `CreateFileW(..., WRITE_OWNER, ...)` (no `GENERIC_READ`/`GENERIC_WRITE`) succeeds even though the DACL denies everything, because `SeTakeOwnershipPrivilege` makes the security reference monitor grant `WRITE_OWNER` unconditionally, independent of the DACL. Once ownership is taken via that handle, a second `CreateFileW(..., WRITE_DAC, ...)` also succeeds (new owners are implicitly granted `WRITE_DAC`/`READ_CONTROL` regardless of DACL content), and that handle is used to install the new wide-open DACL. Rebuilt clean (`fre`, zero errors, "3 files compiled - 1 Warning").

### Stage 5t: even a `WRITE_OWNER`-only open is denied — this is probably NOT an object-manager DACL check at all

Retested the Stage 5s handle-based build. Result: **`CreateFile(WRITE_OWNER) FAILED, GetLastError=5`** — i.e. even opening with *only* `WRITE_OWNER` requested (no data-access rights at all) was denied, on both categories. Control test (disk) still succeeded; `stwrtxp_log.txt` still unchanged.

**This is a significant new signal, not just another failed attempt.** If this really were a plain object-manager DACL check on the device object's security descriptor, an open requesting only `WRITE_OWNER` with `SeTakeOwnershipPrivilege` enabled in the caller's token is supposed to succeed *unconditionally*, regardless of what the DACL says — that privilege bypass is a hard guarantee of the Windows security reference monitor, not a heuristic. Getting `ERROR_ACCESS_DENIED` here anyway means one of two things:
1. The privilege isn't actually getting enabled in our process token (e.g. `AdjustTokenPrivileges` silently not granting it, or a policy restriction on this account) — `kstest.c`'s `EnablePrivilege()` previously only logged *failures*, so a silent success looked identical to output as a normal run; **fixed** by adding an explicit success log line (`"AdjustTokenPrivileges(%s) OK - privilege enabled."`) so the next run will show definitively whether all three privileges actually got enabled.
2. **More likely, given how clean-cut this result is**: the `ERROR_ACCESS_DENIED` on this device was never an object-manager/DACL-level security check to begin with — it's the driver stack's own `IRP_MJ_CREATE` handling (something in KS's default filter-create dispatch, invoked via `PcRegisterSubdevice`/`PcNewPort`'s exposed create path) explicitly returning `STATUS_ACCESS_DENIED` for an unrelated, driver-logic reason, before the object manager's security-descriptor check is even reached. This would also retroactively explain why **both** INF-level `Security` AddReg fixes (Stage 5p/5q) had zero effect — they were never going to matter if the denial isn't a DACL evaluation at all.

Rebuilt clean (`fre`, zero errors) with the added privilege-enable success logging. **Not yet retested on the target machine — immediate next action**: this run's output will disambiguate the two possibilities above — if all three `AdjustTokenPrivileges(...) OK` lines appear yet `WRITE_OWNER` is still denied, that conclusively rules out (1) and confirms (2), meaning the entire take-ownership/ACL approach is a dead end and the real investigation needs to shift to **why KS/PortCls's own create-dispatch path is rejecting this open** — a very different, driver/KS-logic question rather than a security-descriptor one.

### Stage 5u: retest confirms all three privileges enable successfully, yet `WRITE_OWNER` is STILL denied — DACL theory conclusively dead; added zero-access/READ_CONTROL diagnostic to get ground truth

Retested the Stage 5t build. Result: all three lines appear —
```
AdjustTokenPrivileges(SeTakeOwnershipPrivilege) OK - privilege enabled.
AdjustTokenPrivileges(SeSecurityPrivilege) OK - privilege enabled.
AdjustTokenPrivileges(SeRestorePrivilege) OK - privilege enabled.
CreateFile(WRITE_OWNER) FAILED, GetLastError=5
```
Per the decision tree recorded in Stage 5t, this is decisive: possibility (1) (privilege silently not enabled) is ruled out, confirming possibility (2) — **the `ERROR_ACCESS_DENIED` on our device is not an object-manager DACL/security-descriptor check at all.** `SeTakeOwnershipPrivilege` making the security reference monitor grant `WRITE_OWNER` unconditionally, regardless of DACL content, is a hard OS guarantee; nothing standard can produce this result except a denial happening somewhere other than the normal security check. This also fully retroactively explains why both INF `Security` AddReg attempts (Stage 5p/5q/5r) and the whole take-ownership/DACL-reset approach (Stage 5r/5s/5t) were always going to be dead ends — none of that machinery was ever going to matter if the check being applied isn't a DACL evaluation to begin with.

The entire object-manager-security theory is now abandoned as a line of investigation. The leading theory is that this `STATUS_ACCESS_DENIED` (mapped to Win32 `ERROR_ACCESS_DENIED`) is coming from somewhere in KS's/PortCls's own `IRP_MJ_CREATE`-dispatch handling for opening a filter — invoked generically by KS's create-dispatch machinery before any of our own miniport code ever runs (consistent with the log file gaining zero new lines across every one of these `kstest.exe` runs: `CMiniportWaveCyclicHda::NewStream`/`Init` and all our `DOUT` instrumentation would only fire *after* a successful `CreateFile` + `KsCreatePin`, and neither ever happens) — for a driver-logic reason unrelated to security at all.

Before diving into `PCFILTER_DESCRIPTOR`/pin-table archaeology any further on pure guesswork, added a diagnostic step to `kstest.c` to get **ground truth instead of more theorizing**, since we still don't actually know what specific mechanism is producing this:
- **Zero-access `CreateFile(DevicePath, 0, ...)`**: per MSDN, a `dwDesiredAccess` of `0` requests no access rights at all, so under standard NT object-manager semantics this call is supposed to **always succeed** regardless of the object's DACL (it's the officially documented way to "query device attributes without accessing the device"). If even this fails, it's airtight proof the denial isn't a security check of any kind — nothing standard could produce that result — and confirms the KS/PortCls-create-dispatch theory outright.
- If the zero-access open succeeds, escalate to a **`READ_CONTROL`-only** open, and if that also succeeds, call `GetSecurityInfo` + `ConvertSecurityDescriptorToStringSecurityDescriptorW` to print the device object's actual owner + DACL as an SDDL string — giving real data on what the DACL actually contains, rather than continuing to guess from INF source text alone.

New function `DiagnoseSecurityDenial()` added to `kstest.c` (runs automatically, before the existing take-ownership attempt, whenever the initial `GENERIC_READ|GENERIC_WRITE` open hits `ERROR_ACCESS_DENIED`). Needed `#include <sddl.h>` for the SDDL conversion API; no other build-tooling changes needed since `advapi32.lib` was already linked. Rebuilt clean (`fre`, zero errors, only the pre-existing harmless `warning.h` forced-include line in the build log — not a real code warning).

**Not yet retested on the target machine — immediate next action.** This run's output will tell us definitively whether this is a security check at all, and if so, exactly what the DACL contains.

### Stage 5v: `CreateFile(0)` (zero access rights) is ALSO denied — airtight proof this is not a security check at all; added an A/B control test against every other KS audio filter on the machine

Retested the Stage 5u build. Result: `CreateFile(0) FAILED, GetLastError=5` on both categories (control disk test still succeeds). This is the single most decisive result of this entire investigation: a zero-access-rights `CreateFile` is documented Windows behavior to always succeed against any object regardless of its DACL (nothing is being requested, so there's nothing for `SeAccessCheck` to evaluate). Getting `ACCESS_DENIED` here is airtight proof that **no object-manager security descriptor is involved in this denial at all** — it must be generated by driver-side logic (most likely somewhere in KS's/PortCls's own generic filter-open dispatch, since the failure happens before any of our own miniport code runs - the log file gains zero new lines across every one of these runs, confirming `NewStream`/pin-level code is never reached).

This fully retires the entire object-manager-security investigation thread (Stage 5p through 5u): the two INF `Security` AddReg attempts and the full take-ownership/DACL-reset `kstest.exe` machinery were never going to matter, because the check being hit isn't a DACL evaluation to begin with.

With pure driver-logic now the only remaining explanation, and with `mintopo.cpp`/`wavecyclicminiport.cpp`'s filter descriptors reviewed once already with nothing obviously wrong found, the next move was to get an empirical A/B signal rather than keep reading descriptor tables on guesswork: added `ControlTestOtherAudioFilters()` to `kstest.c`, which enumerates **every** `KSCATEGORY_AUDIO` device interface on the machine (not filtered to render/capture, and not filtered to our own hardware ID) and does a plain `CreateFile(GENERIC_READ|GENERIC_WRITE)` on each, printing the result. Windows XP always registers at least one other real KS audio filter under this category even with no sound hardware present — the software wavetable MIDI synth ("Microsoft GS Wavetable SW Synth", `swmidi.sys`) — so this test tells us in one run whether **every** KS audio filter on this machine is equally denied (pointing to a machine-wide KS/PortCls policy or environment problem, unrelated to anything in our own driver) or whether the denial is specific to our own filter's registration/descriptor (pointing back at `mintopo.cpp`/`wavecyclicminiport.cpp`, or a comparison against a known-good sample like `msvad\simple`). Rebuilt clean (`fre`, zero errors).

**Not yet retested on the target machine — immediate next action.**

### Stage 5w: A/B control test against every KSCATEGORY_AUDIO filter came back inconclusive (wrong comparison targets) — pivoted to installing Microsoft's own msvad\simple sample driver as a genuine known-good comparison

Retested the Stage 5v build. Result: the two `hdaudio#...` entries (our own topology + wave filters) fail with `ERROR_ACCESS_DENIED` (5) as always. Four additional `root#system#0000#{KSCATEGORY_AUDIO}\...` entries were found and also failed, but with a **different** error: `GetLastError=1` (`ERROR_INVALID_FUNCTION`), not 5. No `swmidi`/wavetable-synth entry appeared in the enumeration at all — this machine apparently doesn't have that software synth actively registered as a real filter. The four `root#system#0000` entries are almost certainly legacy stub/pseudo-filter registrations (redbook/DRM/property-only placeholder categories Windows registers even with no backing driver instance actually running) rather than genuine, actively-loaded KS filters — `ERROR_INVALID_FUNCTION` is consistent with "this isn't a real openable filter this way" rather than "access denied to a real one". **This control test is therefore inconclusive** — it did not give us a genuine known-good, actively-running KS audio filter to compare against.

**Decision (with user sign-off)**: build and temporarily install Microsoft's own unmodified WDK 7600 `msvad\simple` sample driver (`vadsimpl.sys` — a purely virtual/software audio device, no real hardware I/O) on the real target machine, to get a genuine, real, actively-loaded comparison filter. If `kstest.exe` (which already enumerates every `KSCATEGORY_AUDIO` filter automatically) also gets `ERROR_ACCESS_DENIED` against msvad's filter, that's conclusive proof of a machine-wide KS/PortCls/policy problem, unrelated to anything in our own driver's code. If msvad's filter opens fine, that conclusively isolates the problem to something specific about our own driver's filter registration/descriptor.

Rebuilt `msvad\simple` fresh and clean (`fre`, WNET amd64, zero real errors — 15 files compiled) directly from its own unmodified WDK source tree (`C:\WinDDK\7600.16385.1\src\audio\msvad\simple`), producing `vadsimpl.sys`. Wrote a new minimal standalone INF, `Backported Driver/tools/msvad_abtest/msvadtest.inf`, modeled directly on `msvad.inf`'s own `[MSVAD_Simple.NT]` section (same `AddReg`/`AddInterface`/service pattern already used successfully for our own `stwrtxp.inf`), trimmed to just the "simple" variant and using a root-enumerated hardware ID (`*MSVADSimpleTest`) so it installs as a virtual/software device via Device Manager's "Add Legacy Hardware" wizard — no real hardware match needed. Sent both files to the user with install instructions (same signature-enforcement handling as our own driver).

**Not yet installed/tested on the target machine — immediate next action.**

### Stage 5x: msvad\simple opens FINE on the real machine (CreateFile succeeds, KsCreatePin succeeds on its render pin) — root cause found: our `StartDevice` never called `PcRegisterAdapterPowerManagement` — fix applied and rebuilt

Installed `msvadtest.inf`/`vadsimpl.sys` via Device Manager's Add Legacy Hardware wizard and retested with `kstest.exe`. Result: **decisive.** msvad's filters, appearing as `root#unknown#0000#{65E8773E-...}\wave` (render, `KSCATEGORY_RENDER`) and `root#unknown#0000#{65E8773D-...}\wave` (capture, `KSCATEGORY_CAPTURE`), both `CreateFile OK`; msvad's render pin's `KsCreatePin` also **succeeded** (its capture pin's `KsCreatePin` failed with error 87/`ERROR_INVALID_PARAMETER`, an msvad-specific pin-format quirk, not a security/access issue — not pursued). In the "all `KSCATEGORY_AUDIO` filters" control-test section, msvad's topology and wave filters both show `CreateFile OK` too. Meanwhile our own `hdaudio#func_01&ven_111d&dev_76c7&...` topology and wave filters are, as always, `ERROR_ACCESS_DENIED` (5).

This conclusively rules out a machine-wide KS/PortCls/policy explanation (Stage 5w's other branch of the decision tree) — the exact same account, same machine, same `kstest.exe` binary opens a genuine, real, actively-loaded KS filter with zero trouble. The problem is specific to something in our own driver's filter/device registration.

**Root cause found via direct comparison of `Backported Driver/src/adapter.cpp` against `C:\WinDDK\7600.16385.1\src\audio\msvad\adapter.cpp`:** msvad's `StartDevice` calls `PcRegisterAdapterPowerManagement((PUNKNOWN)pAdapterCommon, DeviceObject)` immediately after the adapter-common object's `Init()` succeeds and *before* installing either subdevice. Our `StartDevice` never called this at all — confirmed via `grep` across `Backported Driver/src`, zero matches. `CHdaAdapterCommon` (`common.h`/`common.cpp`) already fully implements `IAdapterPowerManagement` (`IMP_IAdapterPowerManagement`, `PowerChangeState`/`QueryPowerChangeState`/`QueryDeviceCapabilities`, `m_PowerState` initialized to `PowerDeviceD0`) — the interface was always there, PortCls was just never told about it. Without this registration, PortCls has no confirmed power-up handshake for the adapter, which is consistent with PortCls's create-IRP dispatch treating the device as not fully powered/ready and rejecting every open with `ACCESS_DENIED` regardless of requested access rights (explaining why even the zero-access `CreateFile(path, 0, ...)` from Stage 5v was denied — this was never a security-descriptor check, it's PortCls's own driver-level gate).

**Fix applied**: added the `PcRegisterAdapterPowerManagement` call to `adapter.cpp`'s `StartDevice`, right after `adapterCommon->Init()` succeeds and before either subdevice is installed (matching msvad's ordering exactly), with an error check that bails out (releasing `adapterCommonUnknown`) if it fails.

Rebuilt clean (`chk`, WNET/amd64, via the `C:\stwrtxp_src` junction): "9 files compiled - 1 Warning / 1 executable built", the 1 warning confirmed (via `buildchk_wnet_amd64.log`) to be the same pre-existing harmless forced-include of `warning.h`, not a real code warning. Output: `C:\stwrtxp_src\objchk_wnet_amd64\amd64\stwrtxp.sys`. Sent to the user (`stwrtxp.sys` + the unchanged `stwrtxp.inf`) for install and retest.

**Not yet retested on real hardware — this is the immediate next concrete action.** Per Stage 5w's note, the user should also uninstall the msvad test device (Device Manager → right-click → Uninstall) once this retest is done, to keep the machine clean — it has served its purpose as the A/B control.

### Stage 5y: retested the `PcRegisterAdapterPowerManagement` fix — NO CHANGE, identical `ERROR_ACCESS_DENIED` on both filters. Fix ruled out as the (sole) cause. Two new leads opened, neither yet resolved.

Retested the Stage 5x build. Result: **no change whatsoever.** Both our `hdaudio#...` filters (topology `[0]`, wave `[1]`) still fail every sub-test — plain open, zero-access diagnostic, and `WRITE_OWNER`-after-privilege-escalation — with `ERROR_ACCESS_DENIED` (5), exactly as in every prior retest. The control disk-device test still succeeds. `stwrtxp_log.txt` shows the entire init sequence succeeding cleanly through to `StartDevice: complete, physical connections registered`, including both new `PcRegisterAdapterPowerManagement`/`PcRegisterPhysicalConnection` calls added in Stage 5x — **no error logged from any of them**, meaning they all succeeded silently. This conclusively rules out the missing power-management registration as the (complete) root cause: it was a genuine, real discrepancy versus msvad worth fixing, but fixing it alone does not resolve the symptom.

One more incidental detail from this retest: the "all `KSCATEGORY_AUDIO` filters" enumeration no longer shows any msvad-labeled entries — only our own `[0]`/`[1]` plus the four `root#system#0000` stub entries from Stage 5w. Not yet confirmed with the user, but likely means the msvad A/B test device has already been uninstalled per the Stage 5w/5x standing note. Not investigated further since it doesn't affect our own filters' result either way.

Two new leads were opened this round, reasoned through directly against source (no rebuild yet):

1. **Bridge-pin reuse in the wave miniport (confirmed structural discrepancy, causal link unproven).** `wavecyclicminiport.cpp`'s `MiniportWavePins[]` has only 2 pins total, and both are host-facing streaming pins (`KSPIN_COMMUNICATION_SINK`, `&KSCATEGORY_AUDIO`) — pin 0 (render) and pin 1 (capture). `shared.h` defines `PIN_WAVEOUT_BRIDGE = 0` / `PIN_WAVEIN_BRIDGE = 1`, meaning `adapter.cpp`'s two `PcRegisterPhysicalConnection` calls register these SAME two indices as internal bridge/physical-connection endpoints to the topology filter. So our wave filter's only two pins are simultaneously "the pins `kstest.exe`/apps open directly" and "the pins PortCls treats as internal bridge endpoints to another filter." msvad's own `simple\wavtable.h`, by contrast, has **4 pins**: 2 dedicated host-facing streaming pins plus 2 separate dedicated bridge pins (`KSPIN_COMMUNICATION_NONE`), wired to each other internally via `PCNODE_DESCRIPTOR` (`KSNODETYPE_ADC`/`KSNODETYPE_DAC`) and `PCCONNECTION_DESCRIPTOR` entries — i.e. the streaming pin an app opens is architecturally distinct from the bridge pin PortCls wires to topology. This is a genuine, confirmed deviation from the working reference. **However**, this does not by itself explain why our TOPOLOGY filter is *also* denied — `mintopo.cpp`'s 4 pins are ALL `KSPIN_COMMUNICATION_NONE` (2 bridge to wave, 2 physical jacks to `KSNODETYPE_SPEAKER`/`KSNODETYPE_MICROPHONE`), i.e. it has zero host-facing streaming pins by normal topology-filter design — and yet it's denied identically to wave, while msvad's own topology filter (which almost certainly also has zero host-facing pins, being a topology filter) opened fine in the Stage 5x A/B test. This contradiction is unresolved: msvad's `simple\toptable.h`/`mintopo.h`/`mintopo.cpp` have not yet been read this round to see whether there's a genuine structural difference (not yet done).
2. **Leftover OEM filter driver from the original closed-source package (new, not yet checked at all).** Not yet investigated: whether the original Vista/7-era IDT/Tempo package (`Original Driver/WDM/`, `stwrt64.sys`) left behind a class-wide or device-specific upper/lower filter driver registration (a common pattern for OEM audio packages — DRM/policy enforcement filters, tray-app hook filters, etc.) still bound to either the MEDIA device class (`HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e96c-e325-11ce-bfc1-08002be10318}\UpperFilters`/`LowerFilters`) or this specific device instance (`HKLM\SYSTEM\CurrentControlSet\Enum\HDAUDIO\FUNC_01&VEN_111D&DEV_76C7&SUBSYS_103C2ACD&REV_1001\<instance>\Device Parameters` or the device key's own `UpperFilters`/`LowerFilters` values) on the real p6-2133w. If such a filter is still present and still loaded, it would sit in the same device stack as our FDO and could intercept/deny create IRPs at the driver level (consistent with the zero-access-denied finding from Stage 5v, which only rules out an object-manager DACL check, NOT a driver adding its own custom accept/reject logic in its own `IRP_MJ_CREATE` handling). This is fast to check via `regedit` on the real machine without any rebuild — genuinely new, not yet ruled out or confirmed, and should be checked in parallel with continuing the pin-table comparison.

**Update, same session — lead 1 (bridge-pin reuse) further weakened, now deprioritized.** Read msvad's `simple\toptable.h`/`mintopo.h` in full: msvad's own topology filter has **6** pins, ALL `KSPIN_COMMUNICATION_NONE` (bridge pins to wave/synth plus jack-facing pins), wired only via internal `PCNODE_DESCRIPTOR`/`PCCONNECTION_DESCRIPTOR` graphs (volume/mute/mux nodes) — structurally the same shape as our own `mintopo.cpp` (all-internal pins, no host-facing streaming pins, connections-based wiring). There is no meaningful pin-communication-type difference between msvad's topology filter (which opens fine) and ours (which is denied). Since the bridge-pin-reuse theory cannot explain topology's denial at all — and topology is denied identically to wave — it's now unlikely to be the (sole) explanation for wave's denial either. The pin-table restructuring fix (lead 1c below) is deprioritized until the leftover-filter-driver lead is checked.

**Leftover OEM filter driver is now the leading theory** — it's the only one of the two that would naturally explain BOTH filters being denied uniformly, since a filter driver sitting in the shared device stack (or a class-wide upper/lower filter) would intercept creates for the whole device, regardless of either filter's own pin/node table shape.

**Update, same session — lead 2 (leftover OEM filter driver) RULED OUT.** User checked both registry locations on the real machine: no `UpperFilters`/`LowerFilters` values exist anywhere (not even present as empty/unrelated entries), and no leftover service from the original IDT package is running. This rules out a leftover filter driver in the device stack as the cause.

**Both leads from this stage are now ruled out or thoroughly weakened, and pure source-code comparison against msvad is considered exhausted** — every structural difference found (missing power-management registration, bridge-pin reuse, INF `Security` AddReg placement, INF `AddInterface` structure) has been fixed and/or ruled out with no change to the symptom, and there is no leftover third-party filter to blame. The remaining real, confirmed difference between msvad (works) and our device is **enumeration path**: msvad is root-enumerated (installed via "Add Legacy Hardware"), ours is a genuine bus-enumerated child device under the real HD Audio/UAA bus stack (`HDAUDIO\FUNC_01\...`). Nothing in source review has pinned down what specifically about that path produces `ACCESS_DENIED`, or which component in the stack (ks.sys's generic create dispatch, the HDAUDIO/UAA bus driver, or something power-state-related) actually returns it.

**Pivoting to dynamic instrumentation instead of further static code comparison.** Attempted to install an XP-compatible build of Sysinternals Process Monitor (need an older release, roughly v2.96–v3.1x vintage, since modern builds refuse to run on XP) on the real machine — **could not get any downloadable build to work**, ruled out as an option.

### Stage 5z: Process Monitor unobtainable on XP x64 — instead, our own driver now hooks its own `IRP_MJ_CREATE` dispatch entry to log every create IRP's parameters and PortCls's own resulting status

Since Process Monitor isn't viable on this machine, added self-instrumentation instead: `adapter.cpp`'s `DriverEntry` now saves `DriverObject->MajorFunction[IRP_MJ_CREATE]` (the handler `PcInitializeAdapterDriver` just installed) into a static `g_OriginalMjCreate`, then overwrites the slot with our own `HookedMjCreate`, which logs `DesiredAccess`/`Options`/`ShareAccess`/`FileObject` from the incoming `IO_STACK_LOCATION` via `DOUT`, calls through to `g_OriginalMjCreate`, logs the returned `NTSTATUS`, and returns it unchanged. This is hooking our OWN driver's dispatch table (installed by our own `DriverEntry`), not third-party code — a legitimate, fully source-controlled way to get the dynamic visibility Process Monitor would have given us, reusing the same `DOUT`/`LogToFileF` → `stwrtxp_log.txt` pipeline already proven reliable across the whole investigation. `HookedMjCreate` had to be placed outside the `#pragma code_seg("INIT")` block (normal code segment) since INIT-segment pages are discarded once `DriverEntry` returns and the hook needs to remain resident for the life of the driver.

Rebuilt clean (`chk`, WNET/amd64, via the `C:\stwrtxp_src` junction): "9 files compiled - 1 Warning / 1 executable built", the 1 warning confirmed to be the same pre-existing harmless forced-include of `warning.h`. Output: `C:\stwrtxp_src\objchk_wnet_amd64\amd64\stwrtxp.sys`. Sent to the user for install and retest.

**Not yet retested on real hardware — this is the immediate next concrete action.** Expected new log lines on the next `kstest.exe` run: `MJ_CREATE: DeviceObject=... FileObject=... DesiredAccess=... Options=... ShareAccess=...` immediately followed by `MJ_CREATE: DeviceObject=... FileObject=... returned status=...`, once per `CreateFile` attempt against either filter. Decision tree: (1) if these lines **never appear at all** in `stwrtxp_log.txt` despite `kstest.exe` reporting `ACCESS_DENIED`, the denial is happening before the IRP ever reaches our driver's dispatch table at all — i.e., in the I/O manager's device-interface/symbolic-link resolution itself, pointing back toward something object-manager-level despite the earlier zero-access findings (would need to re-examine that reasoning). (2) if the lines DO appear and `returned status` already shows `STATUS_ACCESS_DENIED` (`0xC0000022`) coming back from `g_OriginalMjCreate` (i.e., from PortCls's own handler), that pins the denial squarely inside PortCls/KS's own generic create dispatch logic, and the next step is figuring out what makes PortCls's create dispatch decide to reject an otherwise-correctly-registered subdevice — likely by examining what's different about this device's `PDEVICE_OBJECT`/`PDEVICE_EXTENSION` state (e.g. power state, `AdapterCommon` power-management confirmation) versus msvad's, now with concrete IRP-level data to reason from instead of pure source comparison. (3) if `DesiredAccess`/`Options`/`ShareAccess` themselves look anomalous (e.g. unexpectedly nonzero even for the zero-access test), that would point back at `kstest.exe` itself or at KS's own create-parameter translation layer.

`stwrtxp.inf` was also read in full this round for a parallel INF-level comparison against `msvadtest.inf`, but no discrepancy was found beyond the already-retired `Security` AddReg lines (Stage 5p/5q, explicitly documented in the INF's own comments as prior dead-end fix attempts) — the `[...Install.NTamd64.Interfaces]`/`AddInterface`/CLSID/FriendlyName structure matches `msvadtest.inf`'s pattern with no meaningful difference. This line of investigation is considered exhausted for now.

### Stage 5aa: retested with `IRP_MJ_CREATE` self-instrumentation — striking mismatch found: our driver's create handler returns SUCCESS for everything it sees, yet `kstest.exe` still gets `ACCESS_DENIED`. Points at branch (1) of the Stage 5z decision tree: the real denial happens BEFORE our dispatch table is ever reached.

Retested on the real p6-2133w with the Stage 5z build. `kstest.exe`'s own output: **every single `CreateFile` attempt still fails with `GetLastError=5` (ACCESS_DENIED)** — the plain open, the zero-access diagnostic open, the `WRITE_OWNER` open, and the retry, for both the render pin and capture pin, plus the direct topology/wave filter opens in the all-`KSCATEGORY_AUDIO` control test. Identical symptom to every prior retest.

`stwrtxp_log.txt` DID show new `MJ_CREATE:` lines this time (10 pairs, right after `StartDevice: complete, physical connections registered`) — so branch (2)/(3) of the decision tree looked reachable at first. But on inspection the data doesn't support either of those branches, and instead undermines the pairing assumption itself:

- **All 10 logged creates returned `status=00000000` (`STATUS_SUCCESS`)** from `g_OriginalMjCreate` (PortCls's own installed create handler). Not one denial was ever logged by our own driver.
- **All 10 have the identical `DesiredAccess=0012019F`** — decodes to a plain `GENERIC_READ|GENERIC_WRITE`-mapped open (`FILE_READ/WRITE_DATA|_EA|_ATTRIBUTES` + `READ_CONTROL|SYNCHRONIZE`). None show `DesiredAccess=0` (the diagnostic zero-access test) or a `WRITE_OWNER`-only mask (`0x00080000`), even though `kstest.exe`'s own output proves it issued both of those specific calls (and both were denied). So the specific `CreateFile` variants we most wanted visibility into never showed up in our log at all.
- The prior build also had a real bug in the diagnostic code itself: it read `ioStack->FileObject` a second time *after* calling `g_OriginalMjCreate(...)`, but by then the IRP may already be completed/freed by the lower handler, so that second read was stale/reused memory — this is why every "returned status" line showed `FileObject=0000000000000000`. (The logged `status` value itself was still trustworthy, since it came from the local `NTSTATUS status =` return value, captured before touching the IRP again — just the accompanying `FileObject` on that second line was garbage.)

**Conclusion**: the 10 logged creates are very likely NOT `kstest.exe`'s failing calls at all — they're probably something else opening the filters right after install completes (PnP/shell device-verification, or similar), which succeed normally. `kstest.exe`'s actual `CreateFile` calls (the ones that get `ACCESS_DENIED`) appear to never reach `HookedMjCreate`/`g_OriginalMjCreate` in the first place. That reopens **branch (1)** from the Stage 5z decision tree: the denial is happening upstream of our driver's own `IRP_MJ_CREATE` dispatch entirely — most likely an I/O-manager/object-manager-level check during device-interface path resolution, before an IRP is even built and sent down to us. This is a genuinely new place to look, since the whole DACL/security-descriptor angle was already exhaustively ruled out in Stages 5p-5v (that was about *why* such a check might deny access, not about *whether the IRP reaches our driver at all* — those are different questions, and this session's data suggests the latter).

**Fix applied to sharpen the next retest** (`adapter.cpp`'s `HookedMjCreate`): fixed the stale-pointer bug (all values now captured into locals *before* calling `g_OriginalMjCreate`, never touched afterward), and added: a monotonic per-create sequence number (`g_MjCreateSeq`, so log lines can be unambiguously ordered/counted), the requesting process ID (`PsGetCurrentProcessId()`, so we can tell whether a logged create actually came from `kstest.exe`'s own PID or from something else like PnP/shell), and the target `FileObject->FileName` (logged via `%wZ`, since `RtlStringCbVPrintfA`/`ntstrsafe` supports `%wZ` for `UNICODE_STRING`, so we can see exactly which pin/interface each create was for). Rebuilt clean (`chk`, WNET/amd64): "9 files compiled - 1 Warning / 1 executable built", same harmless pre-existing `warning.h` warning as every prior build. Sent to the user for install and retest.

**Not yet retested with this sharpened build.** Once retested, the key question to answer from the new log: does the total count of `MJ_CREATE #N: PID=...` entries whose `PID` matches `kstest.exe`'s own process ID equal the number of `CreateFile` calls `kstest.exe` reports as failing? If `kstest.exe`'s PID never appears in the log at all, that's airtight confirmation of branch (1) (denial before our dispatch table), and the next step becomes investigating the device-interface resolution path itself (e.g. whether the symbolic link / device-interface instance the KS enumeration API hands back to `kstest.exe` is actually well-formed, or whether something about being a bus-enumerated HD Audio child device — vs. msvad's root-enumerated virtual device — makes device-interface-path opens behave differently at the object-manager level before any IRP is built). If `kstest.exe`'s PID DOES appear paired with denials this time, that reopens branches (2)/(3) with much better data than before.

### Stage 5ab: two retests confirm `kstest.exe`'s failing creates never reach our dispatch table at all — reopened the Object Manager security-descriptor angle, added a device-security-descriptor dump diagnostic

Retested twice more on the real p6-2133w with the Stage 5aa sharpened build (PID + filename + sequence number added to `HookedMjCreate`). Both retests show the identical pattern:

- `kstest.exe`'s own console output: unchanged — every `CreateFile` attempt (plain open, zero-access diagnostic, `WRITE_OWNER`-only, retry, for both pins, plus the direct filter opens in the all-`KSCATEGORY_AUDIO` test) still fails with `GetLastError=5` (`ACCESS_DENIED`).
- `stwrtxp_log.txt`: 10 `MJ_CREATE #N:` pairs logged, right after `StartDevice: complete, physical connections registered` — same count as Stage 5aa's first retest. But now with PID + filename visible: **every single one shows `PID=4` (the Windows System process) opening a bare relative name, `"\Topology"` or `"\Wave"`** (not a full device-interface path), and every one returns `status=00000000` (`STATUS_SUCCESS`) from PortCls's own installed create handler.
- **Conclusion, now airtight**: these 10 creates are PortCls/KS's own internal bookkeeping opens, issued by the System process during `StartDevice` itself (most likely `PcRegisterSubdevice`/KS filter-factory setup touching its own just-created device objects by relative name) — they have nothing to do with `kstest.exe`. `kstest.exe`'s actual `CreateFile` calls, the ones that get denied, **never invoke `HookedMjCreate`/`g_OriginalMjCreate` at all**. Branch (1) from the Stage 5z decision tree is now confirmed, not just suspected: the denial happens upstream of our driver's own IRP dispatch table entirely.

**Reframed understanding of Stage 5v** (previously used to retire the whole security-descriptor angle): Stage 5v showed that even a zero-`DesiredAccess` `CreateFile` gets denied, and concluded from that "this can't be a security-descriptor/DACL check." That reasoning is incomplete. A zero-access open only guarantees bypassing a check performed *inside a driver's own dispatch routine* (the classic "if `DesiredAccess == 0`, skip our own custom access check" pattern many drivers use). It does **not** guarantee bypassing the **Object Manager's own `SeAccessCheck`** against a device object's security descriptor — that check happens during device-interface/symbolic-link path *resolution*, before an IRP is even built and sent to any driver, and a zero-access open is not exempt from it (a zero-access `SeAccessCheck` is actually a well-known special case — the OS still verifies you can at least query attributes, and denial there is `ACCESS_DENIED` too). Since Stage 5aa/5ab's new data shows `kstest.exe`'s calls never reach our driver at all, that upstream Object-Manager check is now squarely back in play as the likely actual culprit, and needs its own diagnostic rather than remaining "ruled out."

**New diagnostic added** ([`adapter.cpp`](Backported%20Driver/src/adapter.cpp)): a `DumpDeviceSecurity(PDEVICE_OBJECT, PCSTR Label)` function, called from `StartDevice` right after both subdevices are registered, that walks `DeviceObject->DriverObject->DeviceObject`/`NextDevice` (every device object on our driver's device list — our own FDO plus whatever internal Topology/Wave subdevice objects PortCls just created via `PcRegisterSubdevice`) and for each one:
- Gets its security descriptor via `ObGetObjectSecurity`/`ObReleaseObjectSecurity` (already declared in `wdm.h`/`ntddk.h`, no extra include needed).
- Logs the owner SID (`RtlGetOwnerSecurityDescriptor` + `RtlConvertSidToUnicodeString`).
- Logs whether a DACL is present at all (no DACL = unrestricted; `NULL` DACL = full access to everyone; otherwise walks every ACE via `RtlGetAce`, logging each one's type (ALLOW/DENY), access mask, and SID).

**Build gotcha hit and fixed**: `RtlGetOwnerSecurityDescriptor`/`RtlGetDaclSecurityDescriptor`/`RtlConvertSidToUnicodeString`/`RtlGetAce` and the `ACE_HEADER`/`ACCESS_ALLOWED_ACE`/`ACCESS_DENIED_ACE` structures are declared in `<ntifs.h>`, not `<ntddk.h>`/`<wdm.h>`. Naively adding `#include <ntifs.h>` after `shared.h`'s existing `<ntddk.h>` **fails to compile**: `ntifs.h(85/86): error C2371: 'PEPROCESS'/'PETHREAD' : redefinition; different basic types` — this WDK's `ntddk.h` and `ntifs.h` declare these two typedefs with different underlying struct tags (`_KPROCESS`/`_ETHREAD` vs `_EPROCESS`/`_ETHREAD`-variant), and both headers get pulled into the same translation unit once `ntifs.h` is added on top of the existing include chain. **Fix**: don't include `<ntifs.h>` at all — instead hand-declare just the 4 needed `Rtl*` function prototypes (`NTKERNELAPI` linkage, matching their stable documented WDK signatures exactly) and the 3 needed structs (`ACE_HEADER`, `ACCESS_ALLOWED_ACE`, `ACCESS_DENIED_ACE`, plus the `ACCESS_ALLOWED_ACE_TYPE`/`ACCESS_DENIED_ACE_TYPE` constants) directly in `adapter.cpp`, confirmed not already declared anywhere in `wdm.h` (`ACL` itself is, but none of the ACE-related pieces are). Also added `#include <ntstrsafe.h>` (needed for `RtlStringCbPrintfA`, used to build the `"dev#N"` label passed to `DumpDeviceSecurity`; `common.cpp` already includes it for its own `RtlStringCbVPrintfA` use, but `adapter.cpp` hadn't needed it before). Rebuilt clean after this fix: "9 files compiled - 1 Warning / 1 executable built" (same harmless pre-existing `warning.h` warning as every prior build).

**Not yet retested with this build.** Once retested, the new log should show, for each of our device objects, either: (a) a DACL that's missing or explicitly denies access to the SIDs `kstest.exe` runs as (interactive user / `Administrators` / `Everyone` / `Authenticated Users`), which would explain the `ACCESS_DENIED` directly and point at *why* our device objects end up with that SD (most likely something about how the PDO's own SD is inherited/propagated down from the real HD Audio bus driver stack for a bus-enumerated child device, vs. `msvad`'s root-enumerated virtual PDO — a structural difference already flagged as the one remaining known real difference between the working and non-working cases); or (b) a DACL that looks perfectly permissive, in which case the Object-Manager-SD theory itself would be disproven and the investigation needs to look at device-interface *registration* (e.g. `IoRegisterDeviceInterface`/`IoSetDeviceInterfaceState` correctness) instead.

### Stage 5ac: third retest disproves the device-object-DACL theory (DACL is fully permissive) — also discovered `PcRegisterSubdevice` creates no separate per-subdevice device objects — pivoted to a bare-device-interface-open diagnostic in `kstest.c` to test a KS filter-factory-level security theory instead

Retested with the Stage 5ab `DumpDeviceSecurity` build. `kstest.exe`'s own output: unchanged, still `ACCESS_DENIED` (`GetLastError=5`) on every open including the zero-access diagnostic and `WRITE_OWNER` attempts, on both pins; disk control test still succeeds; all-`KSCATEGORY_AUDIO` control test still shows only our own filters denied plus the same unrelated `root#system#0000` stub failures (`GetLastError=1`, different error, not relevant).

`stwrtxp_log.txt` now shows the new diagnostic output, right after `StartDevice: wave subdevice registered`:

```
DumpDeviceSecurity(dev#0): Owner=S-1-5-32-544
DumpDeviceSecurity(dev#0): DACL has 4 ACE(s)
DumpDeviceSecurity(dev#0): ACE[0] ALLOW Mask=001201BF Sid=S-1-1-0
DumpDeviceSecurity(dev#0): ACE[1] ALLOW Mask=001F01FF Sid=S-1-5-18
DumpDeviceSecurity(dev#0): ACE[2] ALLOW Mask=001F01FF Sid=S-1-5-32-544
DumpDeviceSecurity(dev#0): ACE[3] ALLOW Mask=001200A9 Sid=S-1-5-12
```

Two important findings:

- **Only ONE device object (`dev#0`) was found at all** — the loop walks `DeviceObject->DriverObject->DeviceObject`/`NextDevice` and only ever produced a single entry, not one for our FDO plus separate ones for the Topology/Wave subdevices as originally assumed when this diagnostic was designed (Stage 5ab). This means `PcRegisterSubdevice` does **not** create standalone `DEVICE_OBJECT`s linked into the driver's device list for each subdevice — both Topology and Wave are multiplexed through the single FDO via named sub-opens, exactly matching the `MJ_CREATE` log's `Name="\Topology"`/`Name="\Wave"` values from Stage 5aa/5ab. So this diagnostic could only ever inspect one shared SD, not per-subdevice SDs — a real scope limitation of how it was built, now known.
- **That one SD is fully permissive.** Owner is `S-1-5-32-544` (Administrators — the account `kstest.exe` runs as, per its `C:\Documents and Settings\Administrator>` prompt). The DACL has 4 ALLOW ACEs, no DENY ACEs at all: Everyone (`S-1-1-0`) gets `0x001201BF` (closely matches `kstest.exe`'s own requested `DesiredAccess=0012019F`), Local System (`S-1-5-18`) and Administrators (`S-1-5-32-544`) both get `0x001F01FF` (full control), and Restricted Code (`S-1-5-12`) gets `0x001200A9`. A standard `SeAccessCheck` against this SD should succeed for an Administrator account on every access mask `kstest.exe` has tried, including a zero-access open.

**Conclusion: this disproves the device-object-DACL hypothesis** that Stage 5ab's diagnostic was built to test. Branch (a) of that stage's prediction ("DACL restrictive/missing → explains the denial directly") is ruled out; branch (b) ("DACL permissive → theory disproven, look at device-interface registration instead") is confirmed. This is now the **second** upstream mechanism ruled out with live kernel-verified data (after Stage 5aa/5ab's proof that `kstest.exe`'s calls never reach our own `IRP_MJ_CREATE` dispatch) — the root cause is still unknown.

**New leading theory**: since `PcRegisterSubdevice` doesn't create per-subdevice device objects, the `"\Topology"`/`"\Wave"` names `kstest.exe` opens are resolved as **named sub-objects on the single FDO** — i.e., KS filter-factory objects, not raw device objects. It's plausible `ks.sys`/PortCls assigns these named sub-objects their own security descriptor, entirely separate from (and possibly more restrictive than) the device object's own DACL that was just proven permissive — a mechanism `DumpDeviceSecurity` has no visibility into at all, since it only ever sees the one FDO. A secondary, not-yet-distinguished possibility: the device object's DACL is simply never consulted in the first place (the I/O manager only enforces a device object's own SD during open if `FILE_DEVICE_SECURE_OPEN` is set on `DeviceObject->Characteristics` — not yet checked either way for our device).

**New diagnostic added** ([`kstest.c`](Backported%20Driver/tools/kstest/kstest.c)): a `TestOpenBareInterface(LPCWSTR DevicePath)` function, called at the top of `TestOpenAndPin` before the existing filter-open logic. It strips the trailing `"\<reference-string>"` off the full device-interface path (everything after the last backslash — e.g. turns `...\Topology` or `...\Wave` back into the bare base path) and attempts a zero-access `CreateFile` on that bare path directly. This discriminates two remaining explanations:
- If the bare open **fails**: the denial happens at the base device-object level, before any reference-string/named-sub-object is even involved — points at a broken/stale device-interface registration or something else making the base object itself unreachable despite its permissive SD.
- If the bare open **succeeds**: the base device object is reachable and open-able fine, and the denial is specific to the named `"\Topology"`/`"\Wave"` sub-object open — points at a KS filter-factory-level security check, independent of the device object's own DACL.

Also added `#include <wchar.h>` for `wcsrchr`/`wcsncpy`. Rebuilt `kstest.exe` (`fre`, via the pre-existing `C:\stwrtxp_kstest` junction — building directly from the real path under `C:\path\to\stwrtxp` fails with WDK's nmake `error U1087: cannot have : and :: dependents for same target`, the same class of spaced-path issue that motivated the driver's own `C:\stwrtxp_src` junction): "3 files compiled - 1 Warning / 1 executable built". Sent to the user. **No driver rebuild was needed this round** — `stwrtxp.sys`/`stwrtxp.inf` are unchanged from Stage 5ab and remain installed.

**Not yet retested.** Once retested, the key line to check in the new console output is "Bare-interface diagnostic: CreateFile(0) on ... (no reference-string suffix)..." for each pin — does it print `OK` or `FAILED, GetLastError=...`, and does that match for both the render and capture paths.

### Stage 5ad: bare-interface open (no reference-string suffix) ALSO denied — the denial happens even resolving the base device-interface path, apparently contradicting the permissive DACL from Stage 5ac; new theory is a device-interface-level (not device-object-level) security descriptor

Retested with the Stage 5ac `kstest.exe` build (`TestOpenBareInterface`). Result for both the render and capture interface paths:

```
Bare-interface diagnostic: CreateFile(0) on "\\?\hdaudio#...#{65e8773e-...}" (no reference-string suffix)...
Bare-interface CreateFile(0) FAILED, GetLastError=5
```

i.e. **the bare path — the registered device-interface symbolic link with the trailing `\wave`/`\topology` reference-string stripped off — is ALSO denied**, with the same `ERROR_ACCESS_DENIED` (5), not `ERROR_FILE_NOT_FOUND` (2)/`ERROR_BAD_PATHNAME` (161)/anything suggesting the stripped path is simply malformed or unregistered. Everything else is unchanged from every prior retest: full-path open denied, zero-access denied, `WRITE_OWNER` denied even with all three privileges confirmed enabled, disk control test succeeds, all-`KSCATEGORY_AUDIO` control test shows only our filters denied (the `root#system#0000` stub entries fail with the unrelated `ERROR_INVALID_FUNCTION`/1, as in every prior round).

**Why this is a genuine complication, not just "branch (a) after all"**: naively this looks like it confirms branch (a) from Stage 5ac's prediction ("bare open fails → denial is at the base device-object level"). But Stage 5ac's own live kernel dump (`DumpDeviceSecurity`) already proved that base device object's security descriptor is fully permissive (Administrators/System full access, Everyone broad access, zero DENY ACEs) — so a same-flavor `ACCESS_DENIED` at that same base path is hard to explain as a standard `SeAccessCheck` against that same SD. Two ways to reconcile this:

1. **The device object's DACL is never actually consulted by the OS for this open at all.** The I/O manager only enforces a device object's own SD during `IopParseDevice` if `FILE_DEVICE_SECURE_OPEN` is set on `DeviceObject->Characteristics` at creation time — not yet checked either way in our source (PortCls sets `Characteristics` internally via `PcAddAdapterDevice`/`PcRegisterSubdevice`, not something our code controls directly, but worth confirming what value actually lands on the FDO). If that flag isn't set, the permissive SD we dumped is decorative and irrelevant, and access control for this device is happening entirely through some other mechanism.
2. **A device-interface-level (not device-object-level) security descriptor is the actual gate**, and it's separate from the device object's own SD entirely. Windows stores a distinct, optional per-instance security descriptor for each registered device interface under the registry, historically under a `Control\Security` value beneath the interface's subkey in `HKLM\SYSTEM\CurrentControlSet\Control\DeviceClasses\{GUID}\<instance>\`. Stage 5k already confirmed (via regedit) that the interface subkeys for our device exist under both the wave-interface GUID and `KSCATEGORY_RENDER`, but nobody has yet checked whether a `Security`/SDDL value exists there, and if so what it says — this is a genuinely new, unexplored avenue, distinct from everything dumped so far.

**Also unresolved from this round**: the console output alone doesn't tell us whether this new bare-interface `CreateFile` (issued from `kstest.exe`'s own real PID, with an empty residual filename) actually reached our driver's `HookedMjCreate` and got logged with `PID=<kstest's PID>`, `Name=""`, `status=STATUS_ACCESS_DENIED` — or whether it was rejected even before an IRP was ever built and sent down to us (consistent with Stage 5aa/5ab's finding that `kstest.exe`'s calls never appear in the log at all). This is the single most useful piece of data to get next, since it directly distinguishes "our own dispatch handler is denying it" (which nothing in our code currently does — `HookedMjCreate` only logs and forwards) from "something upstream of our driver is denying it, before dispatch."

**Immediate next step, not yet done**: get the `stwrtxp_log.txt` from this same test run and check it for any `MJ_CREATE` entries whose PID matches `kstest.exe`'s own process ID (as opposed to the PID=4/System entries seen in every prior round) — for either the bare-interface open or the full-path opens. If `kstest.exe`'s PID never appears at all, that's consistent with Stages 5aa/5ab (still never reaching our dispatch) and points squarely at the device-interface-registry-security-descriptor theory instead of anything device-object-level. If it does appear this time, that would be new and important data about what our own dispatch chain (`HookedMjCreate` → `g_OriginalMjCreate`, i.e. PortCls's installed handler) actually does with it.

### Stage 5ae: confirmed the log file only reflects driver-start events, never kstest.exe's own opens (even the new bare-interface one) — pivoted to dumping the PDO's security descriptor and both objects' `Characteristics` flags, since the FDO's own permissive SD may simply never be consulted

Asked for `stwrtxp_log.txt` from the same run as the Stage 5ad retest. User clarified (correctly) that `stwrtxp_log.txt` isn't updated by `kstest.exe` at all — it only ever reflects driver load / `StartDevice` time (our diagnostic instrumentation only lives in kernel-mode driver code, which only runs once at load; nothing in `kstest.exe` itself writes to that file). The log handed over was in fact identical in shape to Stage 5ac's/5ad's — the same 11 `MJ_CREATE` entries, all `PID=4`, all at `StartDevice` time, nothing from `kstest.exe`'s own PID. This is still useful confirmation, not wasted effort: it reconfirms (a fourth time, across three different `kstest.exe` builds now) that **none** of `kstest.exe`'s `CreateFile` calls — including the brand-new Stage 5ad bare-interface-path attempt — ever reach our driver's `IRP_MJ_CREATE` dispatch. The denial is happening entirely upstream of our own code, on every single attempted access mask and every path variant tried so far.

This sharpens Stage 5ad's open question: since the FDO's own SD (`dev#0`, dumped in Stage 5ac) is fully permissive, and the bare-interface path denial can't be explained by that SD if it were actually being enforced, one of two things must be true: (1) the I/O manager isn't even enforcing `dev#0`'s SD in the first place (governed by whether `FILE_DEVICE_SECURE_OPEN` — `0x100` — is set on `DeviceObject->Characteristics`; if clear, the SD is decorative for this purpose), or (2) the actual device object the symbolic link/device-interface path resolves to for a bare open is **not** `dev#0` at all — it could be the codec function PDO one level down in the device stack, owned by the HD Audio bus driver (`HDAudBus.sys`)/PnP manager, not by our own driver, with its own independent SD that `DumpDeviceSecurity`'s `StartDevice`-time loop (which only walks `DriverObject->DeviceObject`/`NextDevice`, i.e. objects *our* driver created) has no visibility into at all.

**New diagnostics added** ([`adapter.cpp`](Backported%20Driver/src/adapter.cpp)):
- In `AddDevice` (which already receives the real PDO as its `PhysicalDeviceObject` parameter, straight from the PnP manager — this is the actual bus-owned device object our FDO attaches on top of, previously never inspected): logs `PhysicalDeviceObject->Characteristics` and calls the existing `DumpDeviceSecurity(PhysicalDeviceObject, "PDO")` to dump its owner/DACL, exactly like `dev#0` was dumped before.
- In `StartDevice`'s existing per-device-object loop: added a `Characteristics=%08X` log line for each device object (including `dev#0`), right before its `DumpDeviceSecurity` call.

Rebuilt clean (`chk`, via `C:\stwrtxp_src`): "9 files compiled - 1 Warning / 1 executable built". Sent to the user. **Not yet retested.** Once retested, the two things to check in the new log: (1) does `PDO`'s `DumpDeviceSecurity` output show a restrictive/missing DACL, unlike the permissive one already found for `dev#0`? (2) do either object's `Characteristics` values have bit `0x100` (`FILE_DEVICE_SECURE_OPEN`) set — if `dev#0` does NOT have it set, that alone would mean its SD (however permissive) was never actually the enforcement mechanism to begin with, regardless of what the PDO's SD turns out to be.

### Stage 5af: ROOT CAUSE LIKELY FOUND — the real bus-owned PDO has `FILE_DEVICE_SECURE_OPEN` set AND a DACL with exactly 0 ACEs (deny-all, not absent/NULL) — retroactively reconciles every denial symptom across the whole investigation, including Stage 5u's confusing `WRITE_OWNER` result; INF `Security` AddReg entries re-examined and are likely NOT the source (wrong device instance) but haven't been fully ruled out as harmless noise

Retested the Stage 5ae build. Two rounds of `stwrtxp_log.txt` came back:
- First round: user correctly pointed out kstest.exe never writes to this file (see the standing clarification above) — that log was just a stale pre-rebuild capture with no PDO diagnostic lines at all.
- Second round: contained two concatenated driver-load sequences (an old stale one, lines 1-170, then the real fresh one, lines 171-344). The critical new lines from the fresh sequence:
  ```
  AddDevice: PDO Characteristics=00000180
  DumpDeviceSecurity(PDO): Owner=S-1-5-32-544
  DumpDeviceSecurity(PDO): DACL has 0 ACE(s)
  ...
  StartDevice: dev#0 Characteristics=00000180
  DumpDeviceSecurity(dev#0): Owner=S-1-5-32-544
  DumpDeviceSecurity(dev#0): DACL has 4 ACE(s)
  DumpDeviceSecurity(dev#0): ACE[0] ALLOW Mask=001201BF Sid=S-1-1-0
  DumpDeviceSecurity(dev#0): ACE[1] ALLOW Mask=001F01FF Sid=S-1-5-18
  DumpDeviceSecurity(dev#0): ACE[2] ALLOW Mask=001F01FF Sid=S-1-5-32-544
  DumpDeviceSecurity(dev#0): ACE[3] ALLOW Mask=001200A9 Sid=S-1-5-12
  ```

**Decoding `Characteristics=0x180`**: `FILE_AUTOGENERATED_DEVICE_NAME` (`0x80`) | `FILE_DEVICE_SECURE_OPEN` (`0x100`). Both the PDO and `dev#0` have `FILE_DEVICE_SECURE_OPEN` set — this is the flag that makes `IopParseDevice` actually run a `SeAccessCheck` against a device object's own SD during open at all. This settles Stage 5ad's open question #1 (the FDO's SD is NOT decorative — it genuinely is enforced) but doesn't explain the denial by itself, since `dev#0`'s DACL is permissive.

**The PDO's DACL is the real finding**: "DACL has 0 ACE(s)" is a *populated but empty* ACL (`DaclPresent=TRUE`, non-NULL `Dacl`, `AceCount=0`) — critically different from an *absent* DACL (`DaclPresent=FALSE`, unrestricted) or a *NULL* DACL (`DaclPresent=TRUE`, `Dacl=NULL`, also unrestricted). `SeAccessCheck` only grants access by matching an ALLOW ACE; an ACL with zero ACEs matches nothing, so this is a **deny-all DACL** — nobody, not even the object's own owner (Administrators, `S-1-5-32-544`, confirmed by `DumpDeviceSecurity(PDO): Owner=...`), gets access through it for any requested rights.

This is now the leading root-cause candidate for the *entire* investigation (Stages 5p-5ae), because it explains every symptom observed so far in one shot:
- Full-access, zero-access, and `READ_CONTROL`-only opens all denied identically (Stages 5v/5ab/5ac/5ad) — consistent with a deny-all DACL, which doesn't distinguish by requested access mask at all.
- Administrator-context opens denied same as anyone else's (Stage 5w's control test intent) — consistent, since the DACL has no ACEs naming Administrators (or anyone) at all.
- The bare-interface open (Stage 5ad) denied identically to the full path — consistent, if the device-interface symbolic link's open resolves security enforcement against the PDO (the top of the local device stack) rather than (or in addition to) whichever FDO the symbolic link nominally targets.
- **Retroactively reconciles Stage 5u's most confusing result**: a `WRITE_OWNER`-only open was denied even with `SeTakeOwnershipPrivilege`/`SeSecurityPrivilege`/`SeRestorePrivilege` all confirmed enabled, which was read at the time as proof this couldn't be a DACL check at all (NTFS file-system access checks grant `WRITE_OWNER` unconditionally under that privilege regardless of DACL content). That NTFS-specific privilege bypass is a file-system-driver behavior, not a guarantee of the generic Object-Manager `SeAccessCheck` that `IopParseDevice` runs for a raw device-object open — so a `WRITE_OWNER` denial against a genuinely empty DACL at the Object Manager level is fully consistent, not contradictory, once the file-system-specific assumption is dropped.

**Re-examined `stwrtxp.inf`'s three `Security` AddReg SDDL entries** (`HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;WD)"` at device-level `[StwrtXP.AddReg]` line 74 and both interface-level `[StwrtXP.I.Wave.AddReg]`/`[StwrtXP.I.Topo.AddReg]`, lines 101/109 — see [stwrtxp.inf](Backported%20Driver/inf/stwrtxp.inf)) as a candidate explanation for the empty PDO DACL. Conclusion: **probably not the direct cause, for two independent reasons**:
1. As written, this SDDL decodes to a *permissive* 3-ACE DACL (`GENERIC_ALL` to System/Administrators/Everyone) — not an empty one. If it were being applied verbatim anywhere, we'd expect 3 ACEs, not 0.
2. `[StwrtXP.AddReg]` is wired into `[StwrtXP.Install.NTamd64]` (our own device's own DDInstall section, matched against `HDAUDIO\FUNC_01&...`) — this AddReg's `HKR,,Security` writes to *our own device instance's* software/class key, which is a different registry key entirely from the PDO's live kernel security descriptor. The PDO inspected here is the bus-enumerated codec function device object that our own `AddDevice` attaches on top of — the same device *instance*, yes, but the INF `Security` AddReg mechanism (as documented) targets the device's registry-resident SD used for `SetupDiXxx`-level device-instance permission checks, not the live in-memory `DEVICE_OBJECT`'s Object-Manager SD that `IopParseDevice`/`SeAccessCheck` consults during a raw `CreateFile`. These are two different, easily-conflated security mechanisms. This tracks with Stage 5r's original conclusion ("PortCls's own device-object creation never consults INF-supplied security overrides at all") and dev#0's actually-observed DACL (4 ACEs with SIDs/masks that don't match this SDDL at all, including an unexplained `S-1-5-12` Restricted Code entry) — strongly suggesting dev#0's permissive DACL is some other default (likely PortCls/PnP's own automatic SD), not anything derived from our INF's SDDL either. **Not fully ruled out** (haven't traced exactly which registry key `HKR,,Security` under a DDInstall `AddReg` section actually writes to on this WDK/XP version, or whether anything downstream ever reads it back as a live SD) but is not the leading theory.

**Leading remaining question**: why does the bus-owned PDO have an explicit, populated, zero-ACE DACL in the first place? Nothing in our own driver or INF explicitly sets it (our code has only ever *read* it, via `DumpDeviceSecurity`), so the most likely source is either (a) the HD Audio bus enumerator (whatever creates this FUNC_01 codec PDO — likely the UAA bus driver stack) explicitly securing its child PDOs this way, or (b) a Device Setup Class-level default (`HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e96c-e325-11ce-bfc1-08002be10318}\...\Security`) being applied by PnP at PDO-creation time — both of which are outside our own INF/driver's install-time control. Since we cannot fix another driver's/PnP's registration of the PDO's SD via our own INF, the practical fix is a *runtime* one: overwrite it ourselves.

### Stage 5ag: implemented and built a runtime PDO-DACL fix (`FixPdoSecurity`) — replaces the PDO's empty DACL with a permissive one at `AddDevice` time; clean `chk` build achieved after two rounds of compile-error fixes; **not yet installed/retested**

Added `FixPdoSecurity(PDEVICE_OBJECT PhysicalDeviceObject)` to [adapter.cpp](Backported%20Driver/src/adapter.cpp), called from `AddDevice` immediately after the existing PDO diagnostic dump (Stage 5ae), followed by a second `DumpDeviceSecurity(PhysicalDeviceObject, "PDO-after-fix")` call so the post-fix DACL state is visible in the same log without a second install cycle. The function builds a permissive DACL (one `ACCESS_ALLOWED` ACE granting `GENERIC_ALL` to the Everyone/World SID) entirely by hand in kernel mode (`RtlCreateAcl`/`RtlAddAccessAllowedAce`/`RtlCreateSecurityDescriptor`/`RtlSetDaclSecurityDescriptor`/`RtlAbsoluteToSelfRelativeSD`, no SDDL-string parsing), opens a kernel handle to the PDO via `ObOpenObjectByPointer(..., WRITE_DAC, ...)`, and applies it with `ZwSetSecurityObject(handle, DACL_SECURITY_INFORMATION, ...)`.

**This required hand-declaring several more `ntifs.h`-only symbols** (following the file's existing pattern, since `<ntifs.h>` itself still can't be included — see the standing `PEPROCESS`/`PETHREAD` redeclaration conflict noted at the top of `adapter.cpp`): `RtlCreateSecurityDescriptor`, `RtlCreateAcl`, `RtlAddAccessAllowedAce`, `RtlSetDaclSecurityDescriptor`, `RtlInitializeSid`, `RtlSubAuthoritySid`, `RtlAbsoluteToSelfRelativeSD`, `ObOpenObjectByPointer`, `ZwSetSecurityObject` — plus, newly, the underlying `SID`/`SID_IDENTIFIER_AUTHORITY`/`SECURITY_DESCRIPTOR` **struct layouts themselves** (not just function prototypes), since unlike `PSID`/`PSECURITY_DESCRIPTOR` (already usable as opaque pointers from the existing code), this fix needs to build a real `SID`/`SECURITY_DESCRIPTOR` value on the stack.

**Two compile-error rounds hit and fixed** (both self-caught via rebuild, no user-visible dead ends):
1. `error C2061: ... 'PSID_IDENTIFIER_AUTHORITY'` plus a cascaded `RtlInitializeSid ... does not take 3 arguments` — caused by declaring the `SID`/`SID_IDENTIFIER_AUTHORITY`/`SECURITY_DESCRIPTOR` typedefs *after* the `extern "C"` block whose `RtlInitializeSid` prototype already references `PSID_IDENTIFIER_AUTHORITY` — a forward-reference ordering bug, not a real signature conflict (the "3 arguments" error was purely a cascade from the first syntax error confusing the parser). **Fixed** by moving the struct typedefs above the `extern "C"` block.
2. `RtlInitializeSid` was declared returning `NTSTATUS`; its real WDK signature returns `BOOLEAN`. This wasn't a compile error (nothing else in the translation unit declares the real one to conflict with), but was a latent correctness bug — treating a `BOOLEAN` return value as `NTSTATUS` happens to still evaluate correctly for `TRUE`/`FALSE` via `NT_SUCCESS()` by coincidence of bit layout, but was fixed properly anyway (declared as `BOOLEAN`, checked directly) since it's trivial to get right.

Rebuilt clean (`chk`, WNET/amd64, via `C:\stwrtxp_src` — **user ran this build directly** after the first attempt was blocked by the Claude Code auto-mode safety classifier, see note below): "9 files compiled - 1 Warning / 1 executable built", zero errors.

**Note on the safety-classifier block**: the first rebuild attempt (round 1, before either compile-error fix) was blocked by Claude Code's own auto-mode classifier — flagged as the standing risk noted in earlier stages ("security-descriptor-related code changes" triggering the classifier), and this time correctly so, since the new code doesn't just *read* a device object's SD (as all prior diagnostics did) but actively *sets* one. Handled per the standing instruction: stopped, explained the exact change transparently to the user, and asked them to run the build themselves rather than attempting to route around the block. The user ran both subsequent rebuilds directly from their own terminal.

**Not yet installed or retested.** Once installed: get a fresh `stwrtxp_log.txt` and check (1) the `FixPdoSecurity: ZwSetSecurityObject status=%08X` line for success, (2) the new `DumpDeviceSecurity(PDO-after-fix): ...` lines to confirm the DACL now shows 1 ACE (Everyone/`S-1-1-0`, `GENERIC_ALL`) instead of 0, then (3) run `kstest.exe` and check whether `ACCESS_DENIED` is finally gone on both the render and capture filters.

### Stage 5ah: retest showed `FixPdoSecurity` never actually ran its DACL-overwrite logic — it aborted at the very first step (`RtlInitializeSid` failing at runtime); fixed by hand-constructing the SID instead of calling that API

Installed the Stage 5ag build and retested. Fresh `stwrtxp_log.txt` showed:
```
AddDevice: PDO Characteristics=00000180
DumpDeviceSecurity(PDO): Owner=S-1-5-32-544
DumpDeviceSecurity(PDO): DACL has 0 ACE(s)
FixPdoSecurity: RtlInitializeSid failed
DumpDeviceSecurity(PDO-after-fix): Owner=S-1-5-32-544
DumpDeviceSecurity(PDO-after-fix): DACL has 0 ACE(s)
```
`RtlInitializeSid` (the very first call in `FixPdoSecurity`, used to build the Everyone/World SID) returned `FALSE` on the real machine, so the function returned immediately — `RtlCreateAcl`/`RtlAddAccessAllowedAce`/`RtlCreateSecurityDescriptor`/`RtlSetDaclSecurityDescriptor`/`RtlAbsoluteToSelfRelativeSD`/`ObOpenObjectByPointer`/`ZwSetSecurityObject` were never even attempted, and "PDO-after-fix" shows the exact same empty DACL as before the fix. The matching `kstest.exe` retest showed `ACCESS_DENIED` completely unchanged (bare-interface, full-path, zero-access, and `WRITE_OWNER`-after-ownership-taken opens all still `GetLastError=5` for both pins) — **fully consistent with the fix never having executed**, not a disproof of the Stage 5af root-cause theory. The theory (empty/deny-all PDO DACL is the true blocker) remains unfalsified; what failed was purely the fix's own first line.

Root cause of `RtlInitializeSid` itself failing is unresolved (args looked valid: `SubAuthorityCount=1`, nowhere near the documented `SID_MAX_SUB_AUTHORITIES=15` limit; possibly an ABI mismatch in how the hand-declared prototype passes/returns something, though the sibling hand-declared Rtl routines in this same file work fine using the same declaration pattern — inconclusive). Rather than spend more time on that mystery, **eliminated the dependency entirely**: rewrote the SID construction in `FixPdoSecurity()` ([adapter.cpp](Backported%20Driver/src/adapter.cpp)) to build the well-known Everyone/World SID (S-1-1-0) by hand via direct field assignment on a `PISID`-typed pointer (`Revision=1`, `SubAuthorityCount=1`, `IdentifierAuthority.Value={0,0,0,0,0,1}`, `SubAuthority[0]=0`), using `RtlCopyMemory` for the identifier-authority bytes — no call to `RtlInitializeSid`/`RtlSubAuthoritySid` at all. The SID's binary layout is simple, fixed, and already fully known from the hand-declared `SID` struct, so this sidesteps whatever was wrong with the API call. The `RtlInitializeSid`/`RtlSubAuthoritySid` extern declarations were left in place (harmless, otherwise-correct hand-declarations; not worth removing since another future fix might still need them).

Rebuilt clean (`chk`, WNET/amd64, via `C:\stwrtxp_src`): "9 files compiled - 1 Warning / 1 executable built", zero errors — this time the safety classifier did NOT block the build (unlike Stage 5ag's first attempt), likely because the SID-construction change itself reads as plain struct field assignment rather than a new security-descriptor-setting call site.

**Not yet installed or retested.** Once installed: get a fresh log and check (1) whether `FixPdoSecurity` now proceeds past SID construction to `ZwSetSecurityObject`, (2) whether that call succeeds, (3) whether `DumpDeviceSecurity(PDO-after-fix)` finally shows a non-empty DACL, then (4) retest with `kstest.exe` for `ACCESS_DENIED`.

### Stage 5ai: CRITICAL REGRESSION — the hand-constructed-SID `FixPdoSecurity` build (Stage 5ah) BLUESCREENS the real machine (`SYSTEM_SERVICE_EXCEPTION`) — root cause not yet identified, awaiting bugcheck parameters/log tail from the user

Installed the Stage 5ah build and the machine bluescreened with `SYSTEM_SERVICE_EXCEPTION`. This is new territory: every prior `FixPdoSecurity` attempt (Stage 5ag/5ah's first retest) aborted at the `RtlInitializeSid` check and never reached any of the real DACL-construction/apply calls, so `RtlCreateAcl`, `RtlAddAccessAllowedAce`, `RtlCreateSecurityDescriptor`, `RtlSetDaclSecurityDescriptor`, `RtlAbsoluteToSelfRelativeSD`, `ObOpenObjectByPointer`, and `ZwSetSecurityObject` had never actually executed on real hardware until this build (Stage 5ah's fix removed the only thing blocking them). Any one of these — or the SID-construction change itself — is a live suspect.

**Awaiting from the user before making any further change** (guessing at a fix without this data risks another crash-and-iterate cycle against a physical machine, which is costly): (1) the exact bugcheck STOP code and its four parameters (or a `.dmp` from `C:\Windows\Minidump\`), (2) the last line(s) written to `stwrtxp_log.txt` before the crash — tells us which call it died on, (3) whether the crash happened at driver load/install time or later during a `kstest.exe` run.

**Leading suspects, not yet investigated in order**:
- `ObOpenObjectByPointer` called with `ObjectType=NULL` — the real WDK signature may require a valid `POBJECT_TYPE` (e.g. `*IoDeviceObjectType`) rather than accepting `NULL` as "don't care"; passing the wrong thing here is exactly the kind of hand-declared-API mismatch that caused `RtlInitializeSid` to misbehave earlier, and unlike that call, a bad pointer dereference deep inside `Ob`/`Se` internals would plausibly bugcheck rather than just return an error.
- `ObOpenObjectByPointer`'s `PassedAccessState=NULL` — same category of risk.
- Fixed-size stack buffers (`aclBuffer[64]`, `selfRelBuffer[256]`) being too small for what `RtlCreateAcl`/`RtlAbsoluteToSelfRelativeSD` actually need, causing a buffer overrun into adjacent stack memory (silent corruption rather than a clean status failure) — the code checks `NT_SUCCESS` on `RtlAbsoluteToSelfRelativeSD` but not before it runs, so an overrun in `RtlCreateAcl`/`RtlAddAccessAllowedAce` earlier wouldn't necessarily be caught before something else built on that corrupted stack blows up.
- The hand-declared `SECURITY_DESCRIPTOR` struct layout being subtly wrong (wrong field order/size/padding vs. the real ntoskrnl-internal layout) — `RtlCreateSecurityDescriptor`/`RtlSetDaclSecurityDescriptor` writing through a mis-shaped struct on the stack could corrupt adjacent stack memory in a way that only surfaces later, during a subsequent call.

**Follow-up: checked all hand-declared symbols against the real WDK 7600 headers (`ntifs.h`/`wdm.h`) — everything matches exactly.** `ObOpenObjectByPointer`'s signature (`PassedAccessState`/`ObjectType` are both genuinely `__in_opt`, so passing `NULL` for both is legitimate per the WDK itself, not a mismatch); the `SECURITY_DESCRIPTOR` struct layout (`BYTE Revision, Sbz1; WORD Control; PSID Owner, Group; PACL Sacl, Dacl` — matches `inc/api/winnt.h` line 7551 exactly); and every `Rtl*` prototype (`RtlCreateAcl` in `ntifs.h`, `RtlAddAccessAllowedAce` in `ntifs.h`, `RtlCreateSecurityDescriptor`/`RtlSetDaclSecurityDescriptor` — found in `wdm.h`, not `ntifs.h`, but identical signatures — `RtlAbsoluteToSelfRelativeSD` in `ntifs.h`, `ZwSetSecurityObject` in `ntifs.h`) all match. **This rules out the simple "hand-declared prototype/struct is wrong" explanation** (unlike the earlier `RtlInitializeSid` situation, which remains unexplained but is now moot since that call was removed). Remaining live suspects, not yet investigated further: fixed-size stack buffer sizing/alignment for `aclBuffer`/`selfRelBuffer` (`UCHAR[]` arrays have no strong alignment guarantee; ACL/SD internals may expect better alignment even though x64 generally tolerates misalignment for non-SIMD access — unclear if this is actually sufficient to bugcheck), or something about calling `ZwSetSecurityObject`/`ObOpenObjectByPointer` from within `AddDevice`'s specific execution context (e.g., IRQL, locks held by the PnP manager at that call site, or subject-security-context assumptions inside `ZwSetSecurityObject` that don't hold for a kernel-mode caller with no client token) rather than a pure code-level bug.

**Not yet fixed.** This is a hard blocker on `AddDevice` running at all — every install of this exact build will crash the machine until whichever call is at fault is found and fixed. **Do not reinstall this exact build again** until the crash's actual location (from the bugcheck parameters or a fresh log tail) is known.

### Stage 5aj: Stage 5ai bluescreen resolved by DELETING `FixPdoSecurity`; replaced with two safer attacks on the same root cause — a runtime `FILE_DEVICE_SECURE_OPEN` clear and the documented `[DDInstall.HW]` `Security` INF entry (never tried before); clean `chk` build, staged, **awaiting real-hardware retest**

The previous session ended by asking the user for bugcheck parameters and a log tail before touching anything. That data was never collected, and on review it isn't needed — the only question it could answer is *which line* of a function that should not exist at all was the one that crashed.

**Decision: remove `FixPdoSecurity` entirely rather than debug it.** Setting another driver's PDO security descriptor from a function driver's `AddDevice` is not a supported operation. `ObOpenObjectByPointer` on a `DEVICE_OBJECT` yields a handle that bypasses `IopParseDevice`, and `ZwSetSecurityObject` on that handle dispatches into `IopGetSetSecurityObject`, which is written around file objects. `SYSTEM_SERVICE_EXCEPTION` (0x3B) means precisely "an exception was raised inside a system service", which is where such a call would die. Stage 5ai had already verified every hand-declared prototype and struct layout against the real WDK headers and found them all correct, which rules out the cheap explanation and leaves "this call is being asked to do something it does not support" as the most probable one. Debugging further would have spent more physical-machine crash cycles to earn the right to keep an unsupported mechanism.

**Verified first that the Stage 5af root-cause finding itself is sound.** Before discarding the fix it was worth re-checking the finding it was built on, since a misread diagnostic would have invalidated the whole Stage 5af–5ai thread. `DumpDeviceSecurity()` in [adapter.cpp](Backported%20Driver/src/adapter.cpp) explicitly distinguishes all three cases — `!daclPresent` logs "no DACL present => unrestricted access", `dacl == NULL` logs "NULL DACL => full access to everyone", and only a real ACL reaches the `"DACL has %d ACE(s)"` line. The real machine printed that last one with a count of 0, so the PDO genuinely carries a populated, zero-ACE, deny-all DACL. **The Stage 5af theory stands.**

**Fix 1 (INF — the supported mechanism, and never previously tried).** The only documented way an INF sets a device's PDO security descriptor is a `Security` value in an `AddReg` referenced from a `[DDInstall.HW]` section: that writes to the device's *hardware* key, which is where the PnP manager reads it back and applies it to the PDO. Confirmed against WDK 7600's own samples — `general/toaster/wdm/inf/amd64/toastco.inf` (`[Toaster_Device.NT.HW]` -> `AddReg=Toaster_Device.NT.HW.AddReg` -> `HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;BA)"`), plus `general/pcidrv`, `PLX9x5x`, `portio`, `amcc5933` and the KMDF toaster variants. Every one of them uses this exact placement and no other. **Our INF had no `.HW` section at all.** Stage 5q had put the SDDL in the two interface `AddReg` sections (which write to the device-*interface* key) and Stage 5r moved it to `[StwrtXP.AddReg]`, which — reached via `AddReg=` in the DDInstall section — writes to the driver's *software* key. Neither location is ever read back as a device-object SD, which is exactly why both retests showed no change. Added `[StwrtXP.Install.NTamd64.HW]` -> `[StwrtXP.HW.AddReg]` -> `HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;WD)"` and removed all three misplaced copies. Deliberately did *not* add the `HKR,,DeviceCharacteristics,0x10001,0x100` line the WDK samples pair with it — that flag is `FILE_DEVICE_SECURE_OPEN`, i.e. the very thing that makes the empty DACL bite.

**Also added `DelReg` cleanup, which matters more than it looks.** AddReg-written registry values are *not* removed when a driver is uninstalled, so the stale `Security` values written by the Stage 5p/5q/5r experiments have been sitting in the software key and both device-interface keys across every "clean reinstall" since. New `[StwrtXP.DelReg]` (wired into the DDInstall section) and `[StwrtXP.I.DelReg]` (wired into both interface sections) delete them. Without this, the new `.HW` value would be competing with leftovers from three abandoned experiments.

**Fix 2 (runtime — safe by construction).** `RelaxPdoSecureOpen()` replaces `FixPdoSecurity()` at the same `AddDevice` call site. It does not touch a security descriptor at all; it clears `FILE_DEVICE_SECURE_OPEN` from `PhysicalDeviceObject->Characteristics`. That is one `ULONG` field write on a device object we already hold a referenced pointer to, at `PASSIVE_LEVEL`, with no API calls, no handles and no stack-built structures — it cannot fault and it cannot bugcheck. With the flag clear, the I/O manager stops applying the device object's own SD to opens that carry a trailing name, so the reference-string opens KS and sysaudio actually use stop meeting the empty DACL. Also deleted the now-unused hand-declared security *setter* prototypes (`RtlCreateSecurityDescriptor`, `RtlCreateAcl`, `RtlAddAccessAllowedAce`, `RtlSetDaclSecurityDescriptor`, `RtlInitializeSid`, `RtlSubAuthoritySid`, `RtlLengthSecurityDescriptor`, `RtlAbsoluteToSelfRelativeSD`, `ObOpenObjectByPointer`, `ZwSetSecurityObject`) and the hand-declared `SID`/`SID_IDENTIFIER_AUTHORITY`/`SECURITY_DESCRIPTOR` struct layouts, so a future session cannot casually reach for them again. The read-only ones `DumpDeviceSecurity` needs are kept.

**The two fixes do NOT confound each other** — the log says which one worked, because they leave different fingerprints:

| Log evidence | Reading |
| --- | --- |
| `DumpDeviceSecurity(PDO)` reports a DACL **with ACEs** | The `.HW` INF `Security` took effect. Preferred outcome; `RelaxPdoSecureOpen` is then redundant and should be deleted. |
| `DumpDeviceSecurity(PDO)` still reports **0 ACEs**, `RelaxPdoSecureOpen` logs "cleared FILE_DEVICE_SECURE_OPEN", and `kstest.exe` succeeds | The runtime flag clear is carrying it; the `.HW` mechanism does not reach this PDO. |
| Both applied and `kstest.exe` is **still** denied | The Stage 5af security theory is finally dead. Move the investigation off security entirely. |

**One expected non-result, so it is not misread as failure**: `kstest.exe`'s `TestOpenBareInterface` (the Stage 5ad zero-access open with the reference string stripped) is *still expected to fail with ERROR_ACCESS_DENIED* if only Fix 2 is doing the work. Clearing `FILE_DEVICE_SECURE_OPEN` only skips the SD check for opens that carry a trailing name; a bare device-object open is still checked. Only the two reference-string opens matter for a verdict.

Rebuilt clean (`chk`, WNET/amd64, through the `C:\stwrtxp_src` junction): "9 files compiled - 1 Warning / 1 executable built", zero errors, warning count unchanged from every prior build. The auto-mode safety classifier did not block this build — unsurprisingly, since the change removes security-setting code rather than adding it. Both artifacts staged together at `Backported Driver/package/` (`stwrtxp.sys`, 40448 bytes; `stwrtxp.inf`, 6699 bytes) so there is one folder to copy to the target machine. Pre-change backups kept as `src/adapter.cpp.bak-5ai` and `inf/stwrtxp.inf.bak-5ai`.

**Not yet installed or retested.**

### Stage 5ak: SECURITY WALL BREACHED — Stage 5aj's fixes confirmed working on real hardware, user-mode reached PortCls for the first time ever, and the resulting `0x3B` bugcheck is root-caused to a reference-counting use-after-free on the DMA channel; one-line fix applied, clean build, **awaiting real-hardware retest**

Three things happened in the n27 retest and they must be read in order, because the second is only possible because of the first.

**1. The PDO security fix WORKED — at least on one boot.**

The retest log (`stwrtxp_log.txt`, 546 lines) contains three separate driver-load sessions. Session 1 shows, for the first time anywhere in this investigation, a permissive PDO:

```
AddDevice: PDO Characteristics=00000180
DumpDeviceSecurity: PDO ... DACL has 1 ACE(s)
DumpDeviceSecurity: PDO   ACE[0] ALLOW Mask=001F01FF Sid=S-1-1-0
RelaxPdoSecureOpen: cleared FILE_DEVICE_SECURE_OPEN, PDO Characteristics now 00000080
```

`S-1-1-0` is Everyone and `0x001F01FF` is full access — that is the Stage 5aj `[StwrtXP.Install.NTamd64.HW]` `Security` AddReg entry landing on the real bus-owned PDO. Every previous log in this project showed `DACL has 0 ACE(s)` (deny-all) at that same point. **Both Stage 5aj fixes are confirmed to do what they were designed to do.**

**UNEXPLAINED, STILL OPEN, NOT CURRENTLY BLOCKING:** sessions 2 and 3 of the same log revert to `DACL has 0 ACE(s)`, while still successfully clearing `FILE_DEVICE_SECURE_OPEN`. The logged `original=` PDO pointer distinguishes the boots — sessions 2 and 3 share `original=FFFFFADF2840F960`, session 1 has `original=FFFFFADF1FFB5960`, so session 1 was a different boot from sessions 2 and 3. Leading candidate: XP reused a cached `C:\WINDOWS\inf\oem*.inf`/`.PNF` predating the `.HW` section, so the permissive DACL was only written on the boot where the fresh INF was actually parsed. This does not currently block anything, because the `RelaxPdoSecureOpen` half runs on every single load and is on its own sufficient to get opens through (see point 2). But do not assume the INF half is reliable until it is understood.

**2. Because of that, user-mode reached PortCls dispatch for the first time — and the machine bugchecked there.**

This is *progress*, not a regression. The ACCESS_DENIED wall that blocked Stages 5n through 5aj is gone: the crashing thread's stack bottoms out at a user-mode return address (`0x6a29c28a`), meaning a user-mode process drove a KS request all the way down into PortCls's pin/stream creation path. **That code had never once executed on this machine**, so the first bug sitting in it surfaced immediately. Session 3 of the log shows a second round of `MJ_CREATE` opens (entries #12 through #22) and then simply stops mid-stream — consistent with the crash landing right after.

**3. Root cause: `CMiniportWaveCyclicStreamHda` released a DMA-channel reference it never took.**

Bugcheck `0x3B SYSTEM_SERVICE_EXCEPTION`, parameters `{c0000005, 0, fffffadf231a6130, 0}`. Argument 2 is `0` — execution transferred to address zero, the classic signature of a call through a freed/rewound vtable.

Evidence chain from the dump:

| what | value / finding |
|---|---|
| faulting code in PortCls | `mov rcx,[rbx+1B0h]` / `test rcx,rcx` / `mov rax,[rcx]` / `call qword ptr [rax+10h]` — i.e. `if (p) p->Release();` |
| `[rbx+0x1B0]` (at `fffffadf3f4d6e00`) | holds `fffffadf44198080` |
| pool header at `fffffadf44198070` | `01070001 774e6350` → tag `'wNcP'`, BlockSize 7 → 96-byte payload starting exactly at `...080`. **PortCls's pointer is correct; the object under it is dead.** `'wNcP'` is the WDK `stdunk.h` `operator new` tag, so it is one of ours. |
| `+0x00` | `fffffadf3f49c700` — outside stwrtxp's load range (`fffffadf369bb000`–`fffffadf369ca000`), therefore not a vtable at all: this is a pool/lookaside free-list link written into the freed payload |
| `+0x08` | `fffffadf369bf170` = `stwrtxp!CUnknown::'vftable'` — the **base** class vtable, which the compiler writes as `~CUnknown()` runs |
| `+0x10` | `1` — the `m_lRefCount++` that `CUnknown::NonDelegatingRelease` performs immediately before `delete this` |
| `+0x18` | `fffffadf44198088` = `&self+8` — `m_pUnknownOuter` pointing at the object's own `INonDelegatingUnknown`, which is what `CUnknown`'s constructor does when the outer unknown is **NULL** |

`CUnknown::'vftable'` slot 2 (`+0x10`) is `CUnknown::NonDelegatingRelease` — precisely the method PortCls was calling. Had the object still been alive, the call would have worked.

**Positive identification of the object.** The layout constraints (`INonDelegatingUnknown` at `+0x08`, `m_pUnknownOuter` at `+0x18`) rule out `CHdaAdapterCommon` and `CMiniportWaveCyclicHda`, whose `CUnknown` subobjects sit at `+0x10`/`+0x20`. Size settles the rest — `CHdaDmaChannel` is exactly 96 bytes on x64:

| off | member |
|---|---|
| 0x00 | `IDmaChannel` vptr |
| 0x08 | `CUnknown`: `INonDelegatingUnknown` vptr |
| 0x10 | `CUnknown::m_lRefCount` (+4 pad) |
| 0x18 | `CUnknown::m_pUnknownOuter` |
| 0x20 | `m_pAdapterCommon` |
| 0x28 | `m_EngineHandle` |
| 0x30 | `m_bEngineHandleValid` (+7 pad) |
| 0x38 | `m_pBufferMdl` |
| 0x40 | `m_pSystemAddress` |
| 0x48 | `m_AllocatedSize` |
| 0x50 | `m_RequestedSize` + `m_StreamId` (+3 pad) |
| 0x58 | `m_FifoSize` (+4 tail pad) |
| | **0x60 = 96 bytes** |

That matches the BlockSize-7 payload exactly, whereas `CMiniportWaveCyclicStreamHda` is far larger (its `KTIMER` plus `KDPC` alone are roughly 0x98 bytes). And `+0x18 == &self+8` means the object was constructed with a NULL outer unknown — which in this driver only `new(PoolType) CHdaDmaChannel(NULL)` in `NewStream` does. **The freed object is the DMA channel.**

**The bug in source.** `CMiniportWaveCyclicHda::NewStream` (`wavecyclicminiport.cpp`) takes exactly **one** reference on the channel:

```c
CHdaDmaChannel *pDmaChannel = new(PoolType) CHdaDmaChannel(NULL);
...
pDmaChannel->AddRef();                          // refcount = 1
...
status = pStream->Init(this, m_pAdapterCommon, Capture, pDmaChannel, ...);
...
*DmaChannel = (PDMACHANNEL)pDmaChannel;         // that one reference handed to PortCls
```

and `CMiniportWaveCyclicStreamHda::Init` stored the same pointer under a comment asserting the exact opposite of the truth:

```c
m_pDmaChannel = DmaChannel; // reference already held by the caller (NewStream), transferred to us
```

It was never transferred to the stream — it went to PortCls via the OUT parameter, per `NewStream`'s documented contract. Yet `~CMiniportWaveCyclicStreamHda` unconditionally does `m_pDmaChannel->Release()`. One `AddRef`, two owners, two `Release`s. Tearing the stream down destructs and frees the channel out from under PortCls, and PortCls's own subsequent `Release()` on `[rbx+0x1B0]` calls through the rewound vptr → `rip = 0` → `0x3B`.

The correct pattern was already sitting ten lines below in the same function, for the service group: `PcNewServiceGroup` returns one reference the stream keeps, then `*OutServiceGroup = m_pServiceGroup; m_pServiceGroup->AddRef();` takes a second for the caller. `m_pAdapterCommon` is likewise `AddRef`'d on store. The DMA channel was the only one of the three that released without acquiring — and `wavecyclicstream.h` line 35 had documented the field as `// AddRef'd, released in destructor` all along. This was a dropped line, not a design disagreement.

**Fix applied** (`wavecyclicstream.cpp`, `Init`; pre-change backup at `src/wavecyclicstream.cpp.bak-5aj`): added `m_pDmaChannel->AddRef();` immediately after the store, with a comment recording the reasoning so it cannot be "tidied away" later. Reference accounting after the fix:

- `NewStream` creates the channel, `AddRef` → 1.
- `Init` stores it and `AddRef`s → 2. The stream owns one; PortCls owns the one returned in `*DmaChannel`.
- Normal teardown: PortCls releases the stream → `~stream` releases the channel → 1. PortCls then releases the channel → 0 → freed exactly once.
- `Init`-failure path: `NewStream` does `pStream->Release()` (→ `~stream` → channel 2→1) then `pDmaChannel->Release()` (→ 0). Also correct.
- The `STATUS_INVALID_PARAMETER` early return happens before the store, and `stdunk.h`'s `operator new` zeroes the whole allocation, so `m_pDmaChannel` is NULL there and the destructor's NULL check holds.

**Rebuilt clean** (`chk`, WNET/amd64, through the `C:\stwrtxp_src` junction): "9 files compiled - 1 Warning / 1 executable built", zero errors, the single warning being the usual benign "x64 Native compiling isn't supported. Using cross compilers." notice. Staged to `Backported Driver/package/` — `stwrtxp.sys` (40448 bytes) plus `stwrtxp.inf` (6699 bytes, unchanged from Stage 5aj) — so there is still one folder to copy to the target machine.

**Still unexplained after this stage:** Control Panel continues to report "no audio device", even though the log shows both subdevices registering, both `PcRegisterPhysicalConnection` calls returning success, and PortCls itself opening `\Topology` and `\Wave` with `status=00000000`. That may simply be downstream of the crash (nothing survives long enough to enumerate), or it may be an independent problem of the Stage 5r class. Do not chase it until the `0x3B` fix is confirmed on hardware.

**Tooling note for future sessions:** `C:\WinDDK\7600.16385.1\Debuggers\kd.exe` reads these `MEMORY.DMP` files fine. The Microsoft symbol server was unreachable this session, so `nt!` and `portcls!` frame names in any stack are nearest-export guesses and must NOT be trusted — but `stwrtxp.pdb` is local and yields full private symbols for our own module, which was sufficient to resolve this crash end to end. The useful sequence: `.cxr` on the context record from the bugcheck arguments, then `kb`, then `dps <obj> L8` and `dd <obj>-10 L4` for the pool header and tag, and `x stwrtxp!*vftable*` to identify a vptr.

**Editing note:** the Bash tool's heredoc layer mangles both doubled backslashes and some single-quoted content, which silently corrupts Windows paths and C escapes when splicing text into this file. Two workable techniques: write the content file with the `Write` tool (verbatim, real backslashes) and splice it in with a small Python script, or keep to Bash heredocs but use a `\` placeholder and `.replace("\", chr(92))`. Either way, splice by line index with `assert` guards on the anchors.

### Stage 5al: BREAKTHROUGH — `KsCreatePin` SUCCEEDS on both the render and capture pins, and the Stage 5ak `0x3B` fix is confirmed (pin create + teardown no longer bugcheck); `kstest` extended to stream a real tone, **awaiting the listen test**

The n28 retest came back and it is the biggest result in this project so far.

```
[0] \\?\hdaudio#func_01&ven_111d&dev_76c7&subsys_103c2acd&rev_1001#4&38953da4&0&0001#{65e8773e-...}\wave
    Opening filter via CreateFile...
    CreateFile OK
    Calling KsCreatePin(PinId=0, 44100/16/stereo PCM)...
    KsCreatePin SUCCEEDED - pin handle opened.
```

Identical result for `PinId=1` under `KSCATEGORY_CAPTURE`. **Both pins opened, the process ran to completion, and the machine did not bugcheck.** That single run confirms three separate things at once:

1. **The Stage 5aj security work is fully effective.** `CreateFile` on the filter no longer returns ACCESS_DENIED. The wall that blocked Stages 5n through 5aj is gone.
2. **The Stage 5ak `m_pDmaChannel->AddRef()` fix is correct.** Pin creation runs `CMiniportWaveCyclicHda::NewStream` (constructing both a `CHdaDmaChannel` and a `CMiniportWaveCyclicStreamHda`), and process exit tears the whole thing down again. Under the Stage 5ak bug that teardown *was* the `0x3B`. It now survives, twice per run.
3. **`NewStream`, `CHdaDmaChannel::Init` and `CMiniportWaveCyclicStreamHda::Init` all return success on real hardware** — including `PcNewServiceGroup` and the initial `SetFormat`.

**Correction to a stale kstest diagnostic — do not be misled by it.** The same run also printed:

```
Bare-interface CreateFile(0) FAILED, GetLastError=2
==> Denial happens on the base device object itself, ...
```

That verdict line was hardcoded to print on *any* bare-interface failure, and it was written back when the error was `5` (ACCESS_DENIED). `GetLastError=2` is `ERROR_FILE_NOT_FOUND`, and it is the **expected, correct** result: PortCls registers its filter factories under the `\wave` and `\topology` reference strings, so the bare device-interface path with the suffix stripped names no openable object at all. Nothing is being denied. `TestOpenBareInterface` now distinguishes the two cases explicitly instead of asserting a denial.

Also worth recording from the control tests: our `\topology` and `\wave` both open fine when enumerated under `KSCATEGORY_AUDIO`, and the four `root#system#0000#...` filters (sysaudio/kmixer/wdmaud's own software KS filters) fail with `GetLastError=1` (`ERROR_INVALID_FUNCTION`). That is normal for those software filters and is not a symptom of anything wrong in our driver.

**The remaining symptom is now cleanly isolated.** Control Panel still reports "no audio device". With raw KS pin creation proven working, that can no longer be a security problem, a PnP problem, a registration problem, or a miniport-instantiation problem. It lives above us, in how `sysaudio` builds its virtual audio device out of our topology and wave filters and the physical connection between them.

**Next diagnostic, built this stage: does the driver actually render?** `kstest.c` gained `StreamToneToPin()`. After a successful render-pin open it sets `KSSTATE_ACQUIRE` then `KSSTATE_PAUSE`, queues 8 overlapped `IOCTL_KS_WRITE_STREAM` buffers carrying 3 seconds of a 440 Hz sine (44100/16/stereo, roughly -12 dB), goes to `KSSTATE_RUN`, waits for every completion, then walks back down `PAUSE` → `ACQUIRE` → `STOP`. Supporting pieces: `KsSyncIoctl()` (KS handles are asynchronous, so even a property set needs OVERLAPPED plus `GetOverlappedResult`) and `SetPinState()` (`KSPROPSETID_Connection` / `KSPROPERTY_CONNECTION_STATE`). The tone comes from a 16-point integer sine table driven by a 32-bit phase accumulator, so nothing pulls in float math under `/W4 /WX`. On the way out it calls `CancelIo` and waits for the cancellations to land *before* freeing the buffers, so a timeout cannot leave a pending IRP pointing at freed memory.

The test has three possible outcomes and each one says exactly where to go next:

| result | meaning | where to look next |
|---|---|---|
| tone is audible | the entire kernel-side render path works — codec init, DMA, BDL, servicing timer | stop debugging the driver core entirely; all remaining work is sysaudio/topology graph plumbing for Control Panel |
| all buffers complete, but silence | the data path runs but the codec is muted or routed to the wrong pin complex | `InitCodec()`'s verb sequence, output amp unmute, pin-complex widget selection |
| writes never complete (`TIMED OUT`) | PortCls accepted the writes but stream position never advances | `CHdaDmaChannel`'s position-register plumbing and `CMiniportWaveCyclicStreamHda`'s `TimerDpcRoutine` / `GetPosition` |

**Build note:** `IOCTL_KS_PROPERTY` and `IOCTL_KS_WRITE_STREAM` expand to `CTL_CODE` / `METHOD_NEITHER` / `FILE_DEVICE_KS`, so `kstest.c` now requires `#include <winioctl.h>` **before** `<ks.h>` — the file had deliberately avoided winioctl.h until now, and omitting it produces seven `C2065`/`C4013` errors that look unrelated to the real cause. Rebuilt clean (`fre`, through the `C:\stwrtxp_kstest` junction): "3 files compiled - 1 Warning / 1 executable built". Staged alongside the driver at `Backported Driver/package/kstest.exe` (25088 bytes). Pre-change backup at `tools/kstest/kstest.c.bak-5ak`.

**No driver rebuild this stage** — `stwrtxp.sys` (40448 bytes, the Stage 5ak build) and `stwrtxp.inf` are unchanged and remain installed on the target machine.

### Stage 5am: the tone test bugchecked — a code audit of everything `KSSTATE_RUN` newly reaches found **three** independent defects, all fixed; dump still needed to confirm which one fired

The n29 listen test bugchecked the machine. That is not a regression: `KSSTATE_RUN` had never once been reached in this project's history, so the entire servicing path — `SetState(RUN)`, the notification timer, `TimerDpcRoutine`, `GetPosition`, `Silence`, and PortCls's DPC-level buffer copy — executed for the first time. The audit below was done before the dump arrived, so **none of the three is yet confirmed as the actual faulting one**; all three are provable defects on their own terms and all three are fixed.

**Defect 1 — `GetPosition`, `NormalizePhysicalPosition` and `Silence` were in the pageable code segment.** `wavecyclicstream.cpp` opened with a file-wide `#pragma code_seg("PAGE")` at line 12 and did not reset it until line 411, which put all three of those routines in `PAGE`. PortCls calls all three at **DISPATCH_LEVEL** while servicing a running WaveCyclic stream. Touching pageable code at raised IRQL bugchecks `0xD1` / `0x50` / `0xA` whenever that page happens to have been trimmed — so this is a *probabilistic* crash, which is exactly why nothing caught it until a stream actually ran for three seconds.

The giveaway that this was an accident rather than a decision: those three are the *only* methods in the file with no `PAGED_CODE()` assertion. The author knew they run raised; the file-wide pragma silently overrode that. This is the second time the project has hit this bug class — Stage 4-era notes record `VerbTransferCompletion` having to be moved out of a `PAGE` segment for the same reason.

Checked against Microsoft's own WaveCyclic samples, which bracket **exactly** these routines out of their paged segment:

| | `GetPosition` | `NormalizePhysicalPosition` | `Silence` | `SetFormat`/`SetState` |
|---|---|---|---|---|
| `src\audio\sb16\minwave.cpp` | non-paged (1672) | non-paged (1707) | non-paged (1735) | PAGE |
| `src\audio\msvad\basewave.cpp` | non-paged (679) | non-paged (758) | non-paged (1012) | PAGE |
| ours, before this stage | **PAGE** | **PAGE** | **PAGE** | PAGE (correct) |

Fixed by moving the `#pragma code_seg()` reset up to just before `GetPosition`, so lines 12–372 stay paged (destructor, `NonDelegatingQueryInterface`, `Init`, `SetFormat`, `SetNotificationFreq`, `StartEngine`, `StopEngine`, `SetState` — every one PASSIVE_LEVEL-only and carrying `PAGED_CODE()`) and everything from `GetPosition` to the end of the file is non-paged. **Verified at the binary level, not just in the source:** `.text` grew from `0x29C3` to `0x2C03` (+576 bytes) while `PAGE` shrank from `0x51D4` to `0x4FF4` (−480 bytes).

`dmachannel.cpp` was audited for the same defect and is clean — it has no `code_seg` pragma at all, so `CopyTo` / `CopyFrom` / `SystemAddress` / `BufferSize` are already non-paged. `wavecyclicminiport.cpp` is entirely paged but every method on it is PASSIVE-only, which is legal. `common.cpp` deliberately brackets lines 462–552 non-paged, which correctly covers the `SetDmaEngineState` and `GetLinkPositionRegister` bus wrappers that `GetPosition` calls. `CMiniportWaveCyclicHda::GetPort()` is an inline accessor defined in the header at a point where the default (non-paged) segment is active, so the DPC's call to it is safe even in a `chk` build where it is not inlined away.

**Defect 2 — the servicing DPC could outlive the stream object.** `StopEngine()` cancelled the timer with `KeCancelTimer()` alone. `KeCancelTimer` only stops *future* expirations: if the timer has already fired, its DPC is either sitting in a processor's DPC queue or actively running on another processor, and `KeCancelTimer` neither dequeues it nor waits for it. The destructor then released `m_pServiceGroup`, `m_pAdapterCommon` and `m_pDmaChannel`, `~CUnknown()` rewound the vptr, and `delete this` freed the pool block — while an in-flight `TimerDpcRoutine` was still dereferencing `that->m_pMiniport` and `that->m_pServiceGroup` and calling `port->Notify()`. That is the same use-after-free shape as the Stage 5ak `0x3B`, driven by a timer instead of a refcount, and it is a genuine race on this multi-core target.

Fixed with both halves of the standard teardown: `KeRemoveQueueDpc(&m_Dpc)` in `StopEngine()` for the already-queued case, and `KeFlushQueuedDpcs()` in the destructor for the already-running-on-another-processor case. The flush is legal there because the destructor is PASSIVE_LEVEL (it carries `PAGED_CODE()`), which is what `KeFlushQueuedDpcs` requires.

**Defect 3 — the hardware's buffer wrap point and PortCls's buffer wrap point disagreed.** `CHdaDmaChannel::AllocateBuffer` stored `m_RequestedSize = BufferSize` (what PortCls asked for) while the bus driver reported back its own `allocatedSize`, which `AllocateDmaBuffer` is free to round **up** — HDA works in 128-byte granules. `IDmaChannel::BufferSize()` returned the requested figure, so PortCls indexed its cyclic buffer by that, but `GetPosition()` returned the raw link position register, which counts within the *bus driver's* larger allocation. Whenever the two differed, LPIB could legitimately hand PortCls a position past the end of the buffer PortCls believes it owns; PortCls's servicing logic then computes a byte count from the delta against its last known position and passes that to `CopyTo()`. A wrapped delta there produces an enormous `RtlCopyMemory` and hard pool corruption (`0x19` / `0xC2` / `0x50`). Of the three defects this is the one most likely to fire *immediately and deterministically* the moment RUN starts, so treat it as the leading candidate until the dump says otherwise.

Fixed at both ends: `AllocateBuffer` now reports the allocated size (`m_RequestedSize = (ULONG)allocatedSize`) so the two wrap points agree by construction, and `GetPosition` additionally takes the position modulo `m_pDmaChannel->BufferSize()` as a backstop — belt to the other fix's braces, and it also covers PortCls calling `SetBufferSize()` to shrink its view after the fact.

**Noted but deliberately not changed:** `m_pMiniport` is stored without an `AddRef` (PortCls guarantees the miniport outlives its streams, so this is defensible, and Defect 2's fix removes the window in which it mattered); `AllocateBuffer` early-returns `STATUS_SUCCESS` if a buffer already exists, silently ignoring a size change (fine for one-buffer-per-stream, would need revisiting if format changes mid-stream are ever supported); `TransferCount()` returns `m_AllocatedSize`, which is now identical to `BufferSize()` anyway.

**Build:** `chk x64 WNET`, clean — "9 files compiled - 1 Warning / 1 executable built" (the warning is pre-existing). `stwrtxp.sys` is now 40960 bytes and staged in `Backported Driver/package/` alongside the unchanged `stwrtxp.inf` and the Stage 5al `kstest.exe`. Pre-change backups at `src/wavecyclicstream.cpp.bak-5al` and `src/dmachannel.cpp.bak-5al`.

**Still required to close this out:** the new `C:\WINDOWS\MEMORY.DMP` and the `C:\stwrtxp_log.txt` from the crashing run, plus the stop code if it was visible. Three defects were fixed on static evidence; only the dump can say which one actually fired, and whether a fourth is hiding behind it. If the dump shows a stop code in the `0xD1` / `0x50` / `0xA` family, Defect 1 was it; `0x19` / `0xC2` / a corrupt-pool `0x50` points at Defect 3; a `0x3B` or `0xA` inside `TimerDpcRoutine` points at Defect 2.

### Stage 5an: ROOT CAUSE of the tone-test bugcheck — **PortCls never calls `IDmaChannel::AllocateBuffer`; the miniport must allocate its own cyclic buffer.** The driver waited forever for a call that does not exist, so `BufferSize()` returned 0 and PortCls divided by it

The dump and log from the crashing run are decisive, and the answer is none of the three Stage 5am defects.

**The bugcheck.** `0x3B` again, but a completely different one from Stage 5ak's:

```
Bugcheck code 0000003B
Arguments c0000094  fffffadf204d99a2  fffffadf23b72070  00000000
```

Arg1 `c0000094` is **`STATUS_INTEGER_DIVIDE_BY_ZERO`**, not the `c0000005` of a use-after-free. The faulting instruction is inside PortCls, not us:

```
portcls+0x19a2   41f7f5   div  eax,r13d      <-- r13d = 0
```

Register state at the fault: `r13 = 0`, `rbx = 0x1b90`, `rdi = fffffadf44d1ba50` (the PortCls pin object).

**Identifying the divisor.** The surrounding disassembly pins down what `r13d` is supposed to be — it is used three ways in eleven instructions, and all three say "cyclic buffer size":

```
sub     eax,dword ptr [rdi+1B4h]
shl     ebx,2
add     eax,r8d
div     eax,r13d              ; edx:eax / r13d  -> edx = position MOD buffer size
cmp     ebx,r13d
cmova   ebx,r13d              ; byte count = min(byte count, buffer size)
...
mov     ecx,dword ptr [rdi+1B0h]
mov     eax,r13d
sub     eax,ecx               ; bytes remaining = buffer size - current position
```

`rbx = 0x1b90 = 7056`, and the `shl ebx,2` immediately before means `ebx` was `0x6e4 = 1764` — **exactly** what our `SetNotificationFreq` computes for a 10 ms interval at 44100/16/stereo (`44100 * 4 * 10 / 1000`). Dumping the pin object confirms it: `[rdi+0x1C8] = 0x6e4`, and the two position fields `[rdi+0x1B0]` / `[rdi+0x1B4]` are both zero. So PortCls had our frame size and a fully-built pin, and a buffer size of zero.

**The call stack says this is the write path, not the servicing DPC:**

```
portcls+0x19a2
portcls!PcUnregisterIoTimeout+0x7cc     (nearest-export names; portcls has no private symbols)
portcls!PcDispatchIrp+0xbff
portcls!PcDispatchIrp+0x3b8
portcls!PcDispatchIrp+0x179
nt!NtDeviceIoControlFile+0x551
```

A synchronous `IOCTL_KS_WRITE_STREAM` from the kstest thread — the very first one. PortCls accepted a write against a zero-length cyclic buffer and did modulo arithmetic on it.

**The log names the missing step.** In the crashing session (log lines 568-810, bounded by `DriverEntry` at 568 and the post-crash reboot's `DriverEntry` at 811 — the tail of `stwrtxp_log.txt` is from *after* the reboot, do not read it as the crash):

```
NewStream: entry, Pin=0, Capture=0
SetFormat: 44100 Hz, 2 ch, 16 bit -> engine handle FFFFFADF44E77004
NewStream: success, Pin=0, Capture=0
```

`AllocateBuffer` appears **nowhere** — neither its `DBG_PRINT` success trace (dmachannel.cpp:134) nor its `DBG_ERROR` "no DMA engine handle yet" trace. And `DBG_PRINT` is definitely enabled, because `NewStream: entry` is itself a `DBG_PRINT` and it printed. So `CHdaDmaChannel::AllocateBuffer` was never called, at all, by anyone.

**Root cause: an inverted assumption about who allocates the buffer.** `dmachannel.h` stated it as settled fact — that `SetFormat` "always precedes PortCls calling `AllocateBuffer()` on the WaveCyclic pin-creation sequence". That is false. Checked against every `AllocateBuffer` call site in the WDK 7600 audio samples:

| call site | caller |
|---|---|
| `sb16/minwave.cpp:227` | `CMiniportWaveCyclicSB16::Init` — the **miniport**, 8-bit channel |
| `sb16/minwave.cpp:261` | `CMiniportWaveCyclicSB16::Init` — the **miniport**, 16-bit channel |
| `msvad/basewave.cpp:597` | `CMiniportWaveCyclicStreamMSVAD::Init` — the **stream** |

There is no case of the port driver calling it. `IDmaChannel::AllocateBuffer` is a method a miniport calls on **its own** DMA channel; PortCls only ever consumes `SystemAddress()` and `BufferSize()`. Our driver implemented `AllocateBuffer` correctly and then waited for a call that does not exist in the architecture, so `m_RequestedSize` stayed at its zero-initialised value for the life of every stream, `BufferSize()` returned 0, and the first write bugchecked PortCls.

This also retroactively explains why Stage 5ak's pin create/teardown test passed cleanly: it never wrote a byte, so nothing ever asked for the buffer size.

**The fix**, mirroring `msvad/basewave.cpp:597` exactly — allocate in the stream's `Init`, immediately after `SetFormat` (which is what produces the HDA engine handle that `AllocateBuffer` needs):

```c
status = m_pDmaChannel->AllocateBuffer(HDA_MAX_DMA_BUFFER_SIZE, NULL);
if (!NT_SUCCESS(status))
{
    DOUT(DBG_ERROR, ("Init: AllocateBuffer failed, status=%08X", status));
    return status;
}
```

`HDA_MAX_DMA_BUFFER_SIZE` is `0x16000`, the samples' own `DMA_BUFFER_SIZE` (`msvad/msvad.h:61`). At this driver's worst-case advertised format — 48 kHz, 16-bit, stereo, 192000 bytes/sec, per `PinDataRangePcm` — that holds about 469 ms, and it is an exact multiple of the 128-byte granule HDA works in (`0x16000 / 128 = 704`). `MaximumBufferSize()` now returns that same constant instead of an invented 1 MB ceiling, matching `msvad/basedma.cpp:294`, which returns the very value `basewave.cpp:597` allocates.

**Two related defects fixed alongside it:**

- **`Init` swallowed a `SetFormat` failure**, logging a warning and returning `STATUS_SUCCESS` on the theory that "PortCls should call SetFormat again before RUN". It does not, and a stream with no DMA engine can never allocate a buffer — this is precisely what allowed a half-built stream to reach PortCls. `SetFormat` failure is now fatal to `Init`.
- **A missing `DataFormat` is now rejected** rather than silently producing an engine-less stream.

Both early returns are safe: `NewStream` releases the stream on `Init` failure, and `~CMiniportWaveCyclicStreamHda` releases `m_pDmaChannel`, `m_pServiceGroup` and `m_pAdapterCommon`. With the Stage 5ak `AddRef` in place those balance exactly.

**Status of the three Stage 5am defects.** None of them caused this crash, and none is withdrawn — all three are genuine, independently provable defects that were simply masked by the stream never getting far enough to hit them. Defect 1 (pageable `GetPosition`/`NormalizePhysicalPosition`/`Silence` at DISPATCH_LEVEL) and Defect 2 (servicing DPC able to outlive its stream) become *reachable for the first time* now that a buffer exists and servicing can actually run, so having fixed them in advance is load-bearing for the next test. Defect 3 (hardware wrap point vs PortCls's wrap point) is now doubly covered: `AllocateBuffer` reports the allocated size, and `GetPosition` clamps.

**Build:** `chk x64 WNET`, clean — "9 files compiled - 1 Warning / 1 executable built" (pre-existing warning). `stwrtxp.sys` is 41472 bytes, staged in `Backported Driver/package/` with the unchanged `stwrtxp.inf` and `kstest.exe`. Pre-change backups at `src/wavecyclicstream.cpp.bak-5am`, `src/dmachannel.cpp.bak-5am`, `src/dmachannel.h.bak-5am`.

**Debugging note for next time:** `objchk_wnet_amd64` was rebuilt before the dump was analysed, so the `.pdb` no longer matched the `stwrtxp.sys` loaded in the dump and `dps` could only resolve `stwrtxp+offset`. It did not block this analysis, because the fault was inside PortCls and the evidence came from the disassembly, the register file and the driver log. But if a future crash is inside *our* code, rebuild the exact crashing binary from its `.bak-*` sources into a scratch directory first so the debugger has a matching PDB.

### Stage 5ao: Stage 5an's fix worked — no bugcheck, buffers allocate — but there was no sound, because **the codec's converter was never bound to the stream**. The controller side was complete and the codec side was never programmed at all

The Stage 5an build ran the tone test without crashing. That is the first time this driver has survived `IOCTL_KS_WRITE_STREAM`. The log confirms the buffer fix directly, on every one of the eight streams created:

```
NewStream: entry, Pin=0, Capture=0
SetFormat: 44100 Hz, 2 ch, 16 bit -> engine handle FFFFFADF44EC7004
AllocateBuffer: requested 90112, got 90112 bytes, stream ID 1, fifo 128
NewStream: success, Pin=0, Capture=0
```

`requested 90112, got 90112` — `HDA_MAX_DMA_BUFFER_SIZE` exactly, no rounding, so Stage 5am's Defect 3 (wrap-point mismatch) is confirmed harmless on this hardware although the fix stays. No `0x3B`. But no tone either.

**The missing step: the codec was never told about the stream.** Grepping every verb the driver has ever sent, across the whole log, gives this set:

```
verb=3    SET_AMP_GAIN_MUTE
verb=701  SET_CONN_SELECT
verb=705  SET_POWER_STATE
verb=707  SET_PIN_WIDGET_CTRL
verb=70C  SET_EAPD_BTL_ENABLE
verb=F00  GET_PARAMETER
verb=F02  GET_CONN_LIST_ENTRY
verb=F1C  GET_CONFIG_DEFAULT
```

Two verbs that must be there are absent, and neither has ever been sent by this driver in its entire history:

- **`0x2` SET_CONVERTER_FORMAT** — tells the DAC how to interpret the samples.
- **`0x706` SET_CHAN_STREAMID** — tells the DAC *which stream tag on the link to listen to*.

`HDA_VERB_SET_CHAN_STREAMID` was defined in `hdaverbs.h:21` and referenced from nowhere. `HDAUDIO_CONVERTER_FORMAT m_ConverterFormat` was dutifully filled in by `AllocateRenderDmaEngine`, stored in the stream object, and never read.

**Why this produces silence with no error anywhere.** `AllocateRenderDmaEngine`, `AllocateDmaBuffer` and `SetDmaEngineState` are all *controller*-side operations: they build a DMA engine, give it a buffer, assign it a stream tag, and start it pushing frames onto the HD Audio link. The bus driver does all of that and reports success, because all of it did succeed. The *codec* side is the function driver's job, and a converter that has not been given a stream tag simply ignores the link. So the DMA engine ran, the link position register advanced, PortCls's writes completed on schedule, nothing timed out, nothing faulted — and every DAC on the codec sat there ignoring our audio. This is the quietest possible failure mode, which is why nothing in the log pointed at it.

`InitCodec`/`InitOutputPin` had done all the *static* codec work correctly — power D0, `SET_CONN_SELECT` 0, unmute the DAC and pin amps at max gain (`0xB07F`), `SET_PIN_WIDGET_CTRL` with OUT_ENABLE (+HP_ENABLE on the headphone pin), EAPD on. Five output pins came up (NIDs 10, 13, 15, 16, 17), all selecting the same DAC at connection index 0. What was missing is the *per-stream* work, which cannot happen at init because the stream tag does not exist until a stream is created.

**The fix.** A new `IHdaAdapterCommon::BindRenderConverters(ConverterFormat, StreamTag)`:

- `InitOutputPin` now calls `RecordOutputDac(dacNid)` as it routes each pin, building a deduplicated set of output DAC NIDs (`m_OutputDacNids[8]` / `m_OutputDacCount`) — on this board that resolves to a single DAC, but the set keeps it honest and it is discovered, never hardcoded, per this project's standing rule. `InitCodec` logs what it found and shouts `DBG_ERROR` if the set is empty.
- `BindRenderConverters` sends `SET_STREAM_FORMAT` (0x2) with the bus driver's own `ConverterFormat` word — we do not recompute that encoding — then `SET_CHAN_STREAMID` (0x706) with payload `(tag << 4) | 0`.
- `CMiniportWaveCyclicStreamHda::StartEngine` calls it **before** `SetDmaEngineState(RunState)`, and `StopEngine` calls it with tag 0 (the spec's "not in use" tag) **after** stopping DMA, so the converter is never detached from a stream still feeding it.

Binding at `StartEngine` rather than at `Init` is deliberate. The log shows four separate processes each creating a render *and* a capture pin and then dropping them without ever reaching RUN — sysaudio-style probing. Binding at `Init` would have every probe fight over the one shared DAC.

Capture is deliberately not bound: this backport is playback-only, and the ADC-side equivalent needs the recording-pin setup `InitCodec` explicitly leaves out of scope.

**Known limitation, accepted for now:** `StopEngine` unbinds every recorded output DAC unconditionally, so if two render streams ever ran concurrently, stopping one would mute the other. With a single shared DAC on this board only one render stream can be audible anyway, and kmixer mixes into one stream in the normal path. Revisit only if concurrent hardware streams become real.

**Tracing added, because the Stage 5an log had a blind spot.** `SetState`, `StartEngine`, `StopEngine` and `TimerDpcRoutine` had *no* `DOUT` calls at all, so the log went silent after `NewStream: success` and could not answer the first question worth asking — did the stream ever reach `KSSTATE_RUN`? It can now: `SetState` logs every transition by name, `StartEngine` logs the stream tag, converter format and notification interval, `StopEngine` logs the stop, and `BindRenderConverters` logs each DAC it binds or unbinds. `StartEngine`/`StopEngine` also gained the `PAGED_CODE()` assertions they should always have had — both live in the paged segment and are only ever called from `SetState` and the destructor, all PASSIVE_LEVEL.

**Segment map re-verified after the edits:** PAGE from line 12 to 473 (destructor, `NonDelegatingQueryInterface`, `Init`, `SetFormat`, `SetNotificationFreq`, `StartEngine`, `StopEngine`, `SetState`), non-paged from 473 to EOF (`GetPosition`, `NormalizePhysicalPosition`, `Silence`, `TimerDpcRoutine`). Stage 5am's Defect 1 fix is intact.

**Build:** `chk x64 WNET`, clean — "9 files compiled - 1 Warning / 1 executable built" (pre-existing warning). `stwrtxp.sys` is 44544 bytes, staged in `Backported Driver/package/`. Pre-change backups at `src/shared.h.bak-5an`, `src/common.h.bak-5an`, `src/common.cpp.bak-5an`, `src/wavecyclicstream.cpp.bak-5an`.

**Log-reading note:** `stwrtxp_log.txt` is cumulative across boots. Sessions start at each `DriverEntry` line — in the 16:34 copy those are lines 1, 176, 350, 568 (the Stage 5am crash), 811 (post-crash reboot) and 1007 (the Stage 5an run). Always locate the last `DriverEntry` before reading.

### Stage 5ap: the Stage 5ao build could not open a pin at all — `AllocateRenderDmaEngine` returned `STATUS_INSUFFICIENT_RESOURCES` on the *first* try. **The driver had been leaking one HD Audio DMA engine per pin since the day streams first worked, because the teardown order was inverted: it freed the engine while the engine still owned its DMA buffer**

The Stage 5ao build never reached `StartEngine`, so the converter binding is still untested. Every pin creation failed immediately:

```
NewStream: entry, Pin=0, Capture=0
SetFormat: AllocateRenderDmaEngine failed, status=C000009A
Init: SetFormat failed, status=C000009A
NewStream: stream Init failed, status=C000009A
```

`C000009A` is `STATUS_INSUFFICIENT_RESOURCES`, which the I/O manager maps to Win32 1450 — exactly the `KsCreatePin FAILED, error=1450 (0x000005AA)` kstest printed for both pins. Two processes (PID 1296, PID 412) each tried render and capture; all four failed. Everything *above* the DMA engine was healthy in that same run: codec ID, the 35-widget scan, all five output pins, both subdevices, the physical connections, and `\wave` / `\topology` all opening fine. Stage 5ao's new DAC tracing fired correctly too — `InitCodec: render DAC[0] = NID 21`.

**The evidence is in the previous run's engine handles, and it is unambiguous.** Eight streams were created across four probing processes, and the handles march:

| # | pin | engine handle | stream tag |
|---|-----|---------------|------------|
| 1 | render  | `FFFFFADF44EC7004` | 1 |
| 2 | capture | `FFFFFADF44EC7000` | 1 |
| 3 | render  | `FFFFFADF44EC7005` | 2 |
| 4 | capture | `FFFFFADF44EC7001` | 2 |
| 5 | render  | `FFFFFADF44EC7006` | 3 |
| 6 | capture | `FFFFFADF44EC7002` | 3 |
| 7 | render  | `FFFFFADF44EC7007` | 4 |
| 8 | capture | `FFFFFADF44EC7003` | 4 |

Render engines took slots 4-7, capture took slots 0-3, and the HDA stream tags climbed 1→2→3→4 on each side. `hdaudbus` hands out the lowest free slot and the lowest free tag; **not one was ever reused**. Each of those four processes opened a pin, closed it, and exited, and the engine never came back. Four render + four capture is this controller's entire complement, so the eighth pin exhausted the hardware and the next one — in the *next* driver instance — had nothing left to take. `hdaudbus` only reclaims engines when the bus interface is finally dereferenced, so the shortage outlived the driver that caused it.

**Root cause: `~CMiniportWaveCyclicStreamHda` freed the engine before the buffer.** The sequence was

```c
StopEngine();
KeFlushQueuedDpcs();
if (m_bEngineHandleValid && m_pAdapterCommon)
    m_pAdapterCommon->FreeDmaEngine(m_EngineHandle);   // engine still owns a buffer
...
m_pDmaChannel->Release();                              // -> ~CHdaDmaChannel
                                                       //    -> FreeBuffer()
                                                       //       -> FreeDmaBuffer(dead handle)
```

`hdaudio.h`'s contract is `FreeDmaBuffer` then `FreeDmaEngine`; an engine that still owns a DMA buffer cannot be freed. The only thing that reported the refusal was the `NTSTATUS` — and **both call sites discarded it**, even though `hdaudio.h` marks `PFREE_DMA_ENGINE` `__checkReturn`. So the most expensive resource this driver touches was being thrown away silently, once per pin, with no error, no crash, and no log line, for as long as pin creation has worked. `SetFormat`'s re-allocation path had the identical inversion.

This also explains why the Stage 5an tone test looked so healthy and produced nothing: those eight streams were the four probers, and by the time anything wanted to *run*, the controller was already dry.

**The fix — `CMiniportWaveCyclicStreamHda::ReleaseEngine()`.** One PASSIVE_LEVEL helper, called from both the destructor and `SetFormat`, that does it in the right order: `m_pDmaChannel->FreeBuffer()` first, then `FreeDmaEngine()`, then null the handle in the stream *and* in the channel. It logs the freed handle on success and shouts `DBG_ERROR` with the status on failure, so this class of bug can never be silent again.

Three supporting changes fell out of it:

- **`CHdaDmaChannel::SetEngineHandle(NULL)` now invalidates the channel** instead of marking a NULL handle valid (`m_bEngineHandleValid = (Handle != NULL)`). Without that, `~CHdaDmaChannel`'s own `FreeBuffer()` would hand the just-freed handle back to `hdaudbus`.
- **`SetFormat` re-allocates the cyclic buffer** when it re-allocates the engine. `ReleaseEngine()` takes the buffer with the engine, so without this a second `SetFormat()` would drop `IDmaChannel::BufferSize()` back to zero and re-arm the Stage 5an divide-by-zero. On the first call through `Init()` there is no buffer yet and `Init()`'s own `AllocateBuffer()` still does the work.
- **An outstanding-engine registry on `CHdaAdapterCommon`** (`m_EngineHandles[HDA_MAX_DMA_ENGINES]`, 48 slots — the HDA spec's ceiling is 15+15+15). `AllocateRenderDmaEngine`/`AllocateCaptureDmaEngine` claim a slot, `FreeDmaEngine` releases one, both with a single interlocked pointer swap so there is no lock and no IRQL constraint on the non-paged wrappers. `~CHdaAdapterCommon` sweeps anything still outstanding — `FreeDmaBuffer` then `FreeDmaEngine` — *before* `InterfaceDereference`, logging each one as `DBG_ERROR`. This is a safety net, not the fix: it converts any future leak from "gone until reboot" into "gone until the device is restarted".

**Tracing:** the destructor now logs unconditionally on entry (`~Stream: destroying (capture N, engine ...)`). Its absence was what made this hard to pin down — `StopEngine`'s only `DOUT` sits inside its "engine still valid" branch, so a stream that failed `Init` tore down completely silently, and the logs could not answer whether PortCls was destroying streams at all.

**The running machine needs a reboot before the next test.** The engines leaked by earlier builds are still out on loan inside `hdaudbus` and no new driver can reclaim them; only a reboot resets the controller's allocation state. Reinstalling the driver is not enough — that is precisely what the failing run did.

**Segment map re-verified:** `wavecyclicstream.cpp` PAGE runs 12→560 (destructor, `NonDelegatingQueryInterface`, `Init`, `SetFormat`, `SetNotificationFreq`, **`ReleaseEngine` at 355**, `StartEngine`, `StopEngine`, `SetState`), non-paged 560→EOF (`GetPosition`, `NormalizePhysicalPosition`, `Silence`, `TimerDpcRoutine`). In `common.cpp` the new `TrackEngine`/`UntrackEngine` sit at 504/519, inside the non-paged region 489→631, alongside the DMA wrappers they serve; the adapter destructor stays paged.

**Build:** `chk x64 WNET`, clean — "9 files compiled - 1 Warning / 1 executable built" (pre-existing warning). `stwrtxp.sys` is 46080 bytes, staged in `Backported Driver/package/`. Pre-change backups at `src/shared.h.bak-5ao`, `src/dmachannel.h.bak-5ao`, `src/wavecyclicstream.h.bak-5ao`, `src/wavecyclicstream.cpp.bak-5ao`, `src/common.h.bak-5ao`, `src/common.cpp.bak-5ao`.

### Stage 5aq: **AUDIO. The Stage 5ap build produced audible sound on the real HP Pavilion p6-2133w under 64-bit Windows XP.** First tone this driver has ever made

Reported by the user immediately after installing the Stage 5ap `stwrtxp.sys` (46080 bytes) on a freshly rebooted machine and running `kstest.exe`: **"WE GOT SOUND!!!"**

This is the project's first end-to-end success. Everything from `DriverEntry` to a moving speaker cone now works:

- the codec enumerates (35 widgets, five output pins, DAC NID 21);
- both subdevices register and both KS filter factories open (`\wave`, `\topology`);
- a user-mode process opens a render pin through PortCls;
- an HD Audio DMA engine is allocated **and returned** (Stage 5ap);
- the miniport allocates its own cyclic buffer (Stage 5an);
- PortCls drives the stream STOP → ACQUIRE → PAUSE → RUN;
- `BindRenderConverters()` sends `SET_CONVERTER_FORMAT` (0x2) and `SET_CHAN_STREAMID` (0x706) so the DAC listens for our stream tag (Stage 5ao);
- the controller DMAs the buffer onto the link and the codec converts it to analogue.

**Two stages are validated at once by this single result, and neither had ever executed before.**

*Stage 5ap is confirmed by the fact that a pin opened at all.* The immediately preceding build failed `AllocateRenderDmaEngine` with `STATUS_INSUFFICIENT_RESOURCES` on its very first attempt, because the inverted teardown order (`FreeDmaEngine` while the engine still owned its buffer) had leaked every one of the controller's four render and four capture engines. `ReleaseEngine()` — buffer first, engine second, both handles invalidated — is doing its job.

*Stage 5ao is confirmed by the fact that the tone was audible.* The Stage 5an build reached `KSSTATE_RUN`, advanced its position registers and completed every write, and was completely silent, because nothing had ever told the codec's converter which stream tag to listen for or what format to expect. `BindRenderConverters()` had never once run on hardware until this test. It ran, and the DAC heard it.

By elimination this also settles the last of the Stage 5ao open questions in the affirmative: the flattened one-widget-deep `InitOutputPin` path — `SET_CONN_SELECT` index 0, `SET_AMP_GAIN_MUTE` 0xB07F, `SET_PIN_WIDGET_CONTROL` OUT|HP, `SET_POWER_STATE` D0 — does reach a physical jack on this codec. The full `snd_hda_gen_init` / `activate_path` graph walk that was queued as the fallback in n31b is **not needed for basic playback** and can stay unbuilt until multi-jack or headphone-detect work demands it.

**What is NOT yet confirmed, and needs the log from the successful run:**

1. **Whether the engine handles are stable across runs.** The single decisive test for the leak fix is that repeated `kstest.exe` runs get back *the same* `SetFormat: ... -> engine handle X`. A handle that climbs run to run means engines are still leaking, just more slowly, and the machine will run dry again after enough opens.
2. **Whether `ReleaseEngine: freed engine X` appears, and whether `ReleaseEngine: FreeDmaEngine FAILED` ever does.**
3. **Whether `~AdapterCommon: LEAKED DMA engine ... reclaimed at teardown` fires.** If it does, some stream teardown path is still being missed and the Stage 5ap safety net is covering for it — the log says which handle.
4. Audio *quality*: whether the 440 Hz tone was clean or glitched/stuttering/pitched wrong. A wrong pitch would point at the sample rate actually programmed into the converter format; stuttering would point at the notification timer or the position register.

**Still open, unchanged:** Control Panel reports "no audio device". This success does not touch that. It is above the miniport, in the `sysaudio` graph — the driver's own KS filters are demonstrably healthy, since kstest talks to them directly and gets sound out. See n32.

**Also now unblocked:** item p in the Next Steps list. Real playback is confirmed, so `ulDebugOut` in `debug.h` can be dialled from `DBG_ALL` back to `DBG_DEFAULT` and the shipped artifact can move to a `fre` build — but not before the log questions above are answered, because that log is only being produced by the verbose `chk` build.

### Stage 5ar: the log from the successful run says the DMA-engine leak is **not** fixed. Stage 5ap got the *order* right and still leaked every engine, because `StopEngine()` leaves the engine **paused, not reset** — `hdaudio.h` aliases `StopState` and `PauseState` to the same value, and `hdaudbus` refuses to free an unreset engine

The 17:19 `stwrtxp_log.txt` is the run that produced audio, and it answers n33 bluntly. The handles march exactly as they did before the fix:

| # | pin | engine handle | stream tag | `FreeDmaEngine` |
|---|-----|---------------|------------|-----------------|
| 1 | render  | `FFFFFADF44449004` | 1 | `C0000010` |
| 2 | capture | `FFFFFADF44449000` | 1 | `C0000010` |
| 3 | render  | `FFFFFADF44449005` | 2 | `C0000010` |
| 4 | capture | `FFFFFADF44449001` | 2 | `C0000010` |
| 5 | render  | `FFFFFADF44449006` | 3 | `C0000010` |
| 6 | capture | `FFFFFADF44449002` | 3 | `C0000010` |
| 7 | render  | `FFFFFADF44449007` | 4 | `C0000010` |
| 8 | capture | `FFFFFADF44449003` | 4 | `C0000010` |

Eight engines consumed, none reused, and by `MJ_CREATE #46` the controller was dry again — the last four `NewStream` calls in the same boot failed with `AllocateRenderDmaEngine`/`AllocateCaptureDmaEngine` returning `C000009A`. The tone was audible only because a fresh reboot handed the driver eight engines and the very first render pin got one. **The Stage 5aq success was real but fragile: this build burns through the controller in eight pin opens.**

What Stage 5ap did buy is the diagnosis. `ReleaseEngine()`'s new error path fired on all eight, with a status the old silent code never reported:

```
~Stream: destroying (capture 0, engine FFFFFADF44449004)
StopEngine: stopped
ReleaseEngine: FreeDmaEngine(FFFFFADF44449004) FAILED, status=C0000010
```

`C0000010` is `STATUS_INVALID_DEVICE_REQUEST` — not "bad handle", not "out of memory". The bus driver understood the request and rejected the engine's *state*.

**Root cause: `HDAUDIO_STREAM_STATE` has two names for one value.** `hdaudio.h:143`:

```c
typedef enum _HDAUDIO_STREAM_STATE
{
  ResetState = 0,
  StopState  = 1,
  PauseState = 1,     // <-- same value as StopState
  RunState   = 2
} HDAUDIO_STREAM_STATE;
```

`StopEngine()` called `SetDmaEngineState(StopState, ...)`, which is `SetDmaEngineState(PauseState, ...)`. The engine was paused, never reset. `hdaudbus` will not free the DMA buffer of an unreset engine, and will not free an engine that still owns a buffer — so `FreeDmaBuffer` failed (silently, its status was discarded in `CHdaDmaChannel::FreeBuffer`) and `FreeDmaEngine` then failed with `STATUS_INVALID_DEVICE_REQUEST`. Stage 5ap's ordering fix was correct and necessary, but it reordered two calls that were both going to be refused anyway.

**The destructor was actively undoing the one reset the driver did do.** `SetState(KSSTATE_STOP)` has always issued `SetDmaEngineState(ResetState, ...)`. But the teardown sequence in the log is:

```
SetState: ACQUIRE -> STOP     <- resets the engine (correct)
~Stream: destroying
StopEngine: stopped           <- destructor's StopEngine puts it back to PAUSED
ReleaseEngine: ... FAILED
```

so even a stream that PortCls shut down cleanly arrived at `ReleaseEngine()` un-reset. Streams that never ran at all — the capture pins every prober opens and closes — went straight to the destructor and hit the same wall from the other direction: freshly allocated, then paused by `StopEngine()`, then refused.

**The fix.** Three changes, all small:

- **`ReleaseEngine()` resets the engine before freeing anything.** `SetDmaEngineState(ResetState, 1, &handle)` now runs first, then `FreeBuffer()`, then `FreeDmaEngine()`, and a failed reset is logged as `DBG_ERROR` with the warning that the frees below will be refused.
- **`CHdaDmaChannel::FreeBuffer()` stops discarding `FreeDmaBuffer`'s status.** It now logs success (`FreeBuffer: freed buffer on engine %p`) and failure. That discarded status is what hid half of this bug for the entire life of the driver.
- **`StopEngine()` checks `SetDmaEngineState`'s status** and carries a comment recording the `StopState == PauseState` aliasing, so the next reader does not have to rediscover it.

**Segment map re-verified:** `wavecyclicstream.cpp` PAGE now runs 12→597 (destructor 14, `NonDelegatingQueryInterface` 67, `Init` 94, `SetFormat` 225, `SetNotificationFreq` 303, `ReleaseEngine` 355, `StartEngine` 422, `StopEngine` 484, `SetState` 526), non-paged 597→EOF (`GetPosition` 599, `NormalizePhysicalPosition` 654, `Silence` 678, `TimerDpcRoutine`). `dmachannel.cpp` remains entirely non-paged; `DOUT` is safe at DISPATCH_LEVEL.

**Build:** `chk x64 WNET`, clean — "9 files compiled - 1 Warning / 1 executable built" (pre-existing warning). `stwrtxp.sys` is 47104 bytes, staged in `Backported Driver/package/`. Pre-change backups at `src/wavecyclicstream.cpp.bak-5aq`, `src/dmachannel.cpp.bak-5aq`.

**This build needs a reboot before testing, for the same reason Stage 5ap did:** the eight engines leaked during the 17:19 run are still out on loan inside `hdaudbus` and no newly loaded driver can reclaim them.

### Stage 5as: the DMA-engine leak is **fixed and proven**. Three consecutive `kstest.exe` runs reused the same two engine handles, freed both every time, and the tone was clean on all three. The render path is DONE

The 17:2x `stwrtxp_log.txt` is cumulative across five driver loads; the Stage 5ar build is the load beginning at line 1077, and its stream activity runs from line 1261 to the end of the file. It is unambiguous:

| kstest run | render engine | capture engine | stream tag | `FreeBuffer` | `FreeDmaEngine` |
|------------|---------------|----------------|------------|--------------|-----------------|
| 1 | `FFFFFADF4438F004` | `FFFFFADF4438F000` | 1 | freed | freed |
| 2 | `FFFFFADF4438F004` | `FFFFFADF4438F000` | 1 | freed | freed |
| 3 | `FFFFFADF4438F004` | `FFFFFADF4438F000` | 1 | freed | freed |

**The same two handles come back on every run** — the exact test n34 called for. So does the stream tag: it is 1 every time, where before Stage 5ar it climbed 1→2→3→4 as the controller handed out fresh descriptors it would never get back. Every teardown now logs the pair

```
FreeBuffer: freed buffer on engine FFFFFADF4438F004
ReleaseEngine: freed engine FFFFFADF4438F004
```

and the Stage 5ar section of the log contains **zero** `FAILED`, zero `C0000010`, zero `C000009A`, and no `~AdapterCommon: LEAKED DMA engine`. Six allocations, six buffer frees, six engine frees, perfectly balanced. Capture pins that are opened and closed without ever running — kstest probes pin 1 on every pass — release cleanly too, which was the case that failed from the other direction before.

Contrast with the earlier loads still present in the same file (lines 630-823, the Stage 5ap build): eight engines `...9000` through `...9007`, all eight `FreeDmaEngine` calls refused with `C0000010`, and the controller dry by the twelfth pin open. Same machine, same kstest, one enum value's difference.

**The user confirms the tone is clean on all three runs.** That retires the last of n34's open questions and quietly validates two more things: the sample rate actually programmed into the converter (`format 4011` = 44.1 kHz, 16-bit, stereo) is correct, since the pitch is right; and the 10 ms notification timer plus the link position register are keeping up, since there is no stuttering.

**The render path is finished.** Everything from `DriverEntry` to a moving speaker cone works and is repeatable:

- codec enumerates (35 widgets, five output pins, DAC NID 21);
- both subdevices register, both KS filter factories open;
- a user-mode process opens a render pin through PortCls;
- an HD Audio DMA engine is allocated, used, **and returned**;
- the miniport allocates its own cyclic buffer and frees it;
- PortCls drives STOP → ACQUIRE → PAUSE → RUN → PAUSE → ACQUIRE → STOP;
- `SET_CONVERTER_FORMAT` and `SET_CHAN_STREAMID` bind DAC 21 to the stream tag on RUN and release it on stop;
- audio comes out of the jack, at the right pitch, without glitches.

No source changes in this stage — it is the confirmation of Stage 5ar. `stwrtxp.sys` remains the 47104-byte build staged in `Backported Driver/package/`.

**What is left.** Two things, in order:

1. **n32 — Control Panel still reports "no audio device."** This is now the only functional blocker and it is cleanly isolated: the driver's own KS filters are demonstrably healthy, because kstest talks to them directly and gets clean audio out. The fault is entirely in the `sysaudio` graph above the miniport.
2. **Item p — the cleanup pass.** Real playback is confirmed and repeatable, so `ulDebugOut` in `debug.h` can go from `DBG_ALL` back to `DBG_DEFAULT`, the `IRP_MJ_CREATE` diagnostic hook installed in `DriverEntry` can come out, and the shipped artifact can move to a `fre` build. Do this **after** n32, not before — the verbose `chk` build is the only reason any of the last eight stages were diagnosable.

### Stage 5at: found why Control Panel says "no audio device" — **the wave filter has no bridge pins.** It exposed two streaming sink pins, no nodes, no connections, and `adapter.cpp` registered the physical connection to the topology filter *on those streaming pins*. sysaudio cannot walk that graph

This is n32, and it is not a security, PnP, INF or codec problem. It is the wave filter's `PCFILTER_DESCRIPTOR`.

**What the driver was telling sysaudio.** `MiniportWavePins[]` in `wavecyclicminiport.cpp` held exactly two entries:

| index | direction | communication | role |
|---|---|---|---|
| 0 | `KSPIN_DATAFLOW_IN`  | `KSPIN_COMMUNICATION_SINK` | render streaming pin |
| 1 | `KSPIN_DATAFLOW_OUT` | `KSPIN_COMMUNICATION_SINK` | capture streaming pin |

and the filter descriptor passed `0, 0, NULL, 0, NULL` for NodeSize / NodeCount / Nodes / ConnectionCount / Connections. No bridge pins. No nodes. No connections.

Meanwhile `shared.h` defined

```c
PIN_WAVEOUT_BRIDGE = 0,   // "wave miniport's render bridge pin"
PIN_WAVEIN_BRIDGE,        // "wave miniport's capture bridge pin"
```

— names that promise bridge pins but resolve to **0 and 1, the streaming pins**. So `adapter.cpp`'s two `PcRegisterPhysicalConnection` calls declared the driver's filter-to-filter link to terminate on a pair of `KSPIN_COMMUNICATION_SINK` pins.

**Why that produces exactly this symptom.** A physical connection must terminate on a *bridge* pin — `KSPIN_COMMUNICATION_NONE`, with an analog `KSDATARANGE`, dataflow pointing out of the filter on the render side and into it on the capture side. It is the only thing that tells sysaudio "this filter's audio continues over there." Walking our wave filter, sysaudio found two dead-end sink pins, no route to the topology filter's `KSNODETYPE_SPEAKER` connector pin, and therefore no complete render endpoint to hand to `wdmaud.drv`. Hence "no audio device."

And this is precisely consistent with everything else that works: `kstest.exe` calls `KsCreatePin` on the wave filter directly and never involves sysaudio at all, which is why the same driver has been producing clean audio at the jack since Stage 5aq while the Control Panel applet saw nothing. The INF's `[StwrtXP.AddReg]` wave-mapper attachment (Stage 5j) and its `[StwrtXP.Install.NTamd64.Interfaces]` reference strings were already correct — they were never the missing piece, they were just upstream of a graph that had no path in it.

**The reference shape.** WDK 7600 `msvad\simple\wavtable.h` builds the wave filter as four pins, two nodes, four connections: streaming sink → converter node → bridge pin, once per direction. Bridge pins there advertise a plain `KSDATARANGE` (not `KSDATARANGE_AUDIO`) of `TYPE_AUDIO` / `SUBTYPE_ANALOG` / `SPECIFIER_NONE`, and carry `0, 0, 0` instance counts because nobody opens them as streams. `msvad\adapter.cpp` then registers `KSPIN_WAVE_RENDER_SOURCE → KSPIN_TOPO_WAVEOUT_SOURCE` and `KSPIN_TOPO_WAVEIN_DEST → KSPIN_WAVE_CAPTURE_SOURCE` — bridge pin to bridge pin in both directions.

**The fix.** Four files:

- **`shared.h`** — `HdaWavePin` becomes a real four-entry enum: `PIN_WAVE_RENDER_SINK = 0`, `PIN_WAVE_CAPTURE_SINK = 1`, `PIN_WAVEOUT_BRIDGE = 2`, `PIN_WAVEIN_BRIDGE = 3`. Two new enums join it: `HdaWaveNode` (`NODE_WAVE_DAC`, `NODE_WAVE_ADC`) and `HdaTopoPin` (`PIN_TOPO_WAVEOUT_DEST`, `PIN_TOPO_WAVEIN_SOURCE`, `PIN_TOPO_LINEOUT_DEST`, `PIN_TOPO_MIC_SOURCE`), the latter replacing the bare `0`/`1`/`2`/`3` literals that were scattered across `adapter.cpp` and `mintopo.cpp`.
  **The streaming pins deliberately keep indices 0 and 1**, so `kstest.exe`'s hardcoded `PinId=0` (render) and `PinId=1` (capture) stay valid and the tool needs no rebuild. The bridge pins were appended rather than inserted for exactly this reason.
- **`wavecyclicminiport.cpp`** — adds `PinDataRangesBridge` (plain `KSDATARANGE`, analog, specifier none), the two bridge pin descriptors, a `MiniportWaveNodes[]` table with `KSNODETYPE_DAC` and `KSNODETYPE_ADC`, and a `MiniportWaveConnections[]` table of four entries:

```c
{ PCFILTER_NODE,    PIN_WAVE_RENDER_SINK,  NODE_WAVE_DAC,  1                     },
{ NODE_WAVE_DAC,    0,                     PCFILTER_NODE,  PIN_WAVEOUT_BRIDGE    },
{ PCFILTER_NODE,    PIN_WAVEIN_BRIDGE,     NODE_WAVE_ADC,  1                     },
{ NODE_WAVE_ADC,    0,                     PCFILTER_NODE,  PIN_WAVE_CAPTURE_SINK }
```

  The filter descriptor now spells out all twelve fields including `NodeSize`/`NodeCount`/`Nodes` and `CategoryCount`/`Categories`.
- **`adapter.cpp`** — the two `PcRegisterPhysicalConnection` calls now name real bridge pins on the wave side and the `HdaTopoPin` constants on the topology side: `wave PIN_WAVEOUT_BRIDGE → topo PIN_TOPO_WAVEOUT_DEST`, and `topo PIN_TOPO_WAVEIN_SOURCE → wave PIN_WAVEIN_BRIDGE`.
- **`mintopo.cpp`** — no index changes; the topology filter was already correct. Its connections and pin comments now use the `HdaTopoPin` names, its filter descriptor gains the explicit `CategoryCount`/`Categories` fields, and its file header no longer claims a 2-pin filter.

`CMiniportWaveCyclicHda::NewStream` ignores its `Pin` argument entirely and switches on `Capture` (which PortCls derives from the pin's dataflow), so the renumbering does not reach the streaming code path at all.

**What this does not add yet.** No volume or mute node on either filter — the MVP scope is unchanged, and device *presence* does not require one. If the device appears but the Windows volume slider is dead or missing, that is the next thing to add (`KSNODETYPE_VOLUME` + `KSNODETYPE_MUTE` on the topology filter with a `PropertyHandler_Topology`, per `msvad\simple\toptable.h`), and it is a separate change from this one.

**Build:** `chk x64 WNET`, clean — "9 files compiled - 1 Warning / 1 executable built" (the same pre-existing warning). `stwrtxp.sys` is **47616 bytes**, staged in `Backported Driver/package/`. Pre-change backups at `src/shared.h.bak-5as`, `src/wavecyclicminiport.cpp.bak-5as`, `src/mintopo.cpp.bak-5as`, `src/adapter.cpp.bak-5as`.

**No reboot needed for the DMA engines this time** — Stage 5ar's fix means the previous run returned everything it borrowed. A reboot is still the cleanest way to be sure sysaudio re-enumerates the filter graph from scratch rather than reusing a cached one.

### Stage 5au: the Stage 5at build bluescreened — **`0x7E` inside `sysaudio.sys`, not our driver.** The topology filter wired two of its own pins directly to two others with no node in between, and sysaudio's graph walk NULL-dereferenced on it the very first time it got far enough to look

The user installed the Stage 5at build and the machine bugchecked. `MEMORY.DMP` (590 MB, kernel summary, uptime 3:22) is unambiguous:

```
BugCheck 7E, {ffffffffc0000005, fffffadf2003f2b4, fffffadf36c8d270, fffffadf36c8cc80}
FAULTING_IP: sysaudio+1d2b4
fffffadf`2003f2b4 488b4108        mov     rax,qword ptr [rcx+8]
ExceptionCode: c0000005 (Access violation) reading 0000000000000008
Probably caused by : sysaudio.sys ( sysaudio+1d2b4 )
```

`rcx = 0`. `stwrtxp.sys` loads at `fffffadf'36a2b000` with **private PDB symbols and timestamp Tue Sep 08 17:44:07 2026** — the Stage 5at binary exactly, so the dump is fresh, not a stale one from an older build.

**There is no `stwrtxp` frame anywhere on the stack.** It is sysaudio all the way down to `ks!KsQueueWorkItem`:

```
sysaudio+0x1d2b4   <- fault
sysaudio+0x1d28c
sysaudio+0x1d07c
sysaudio+0x1b6e4   \
sysaudio+0x1e23a    > recursion, two levels deep
sysaudio+0x1aa80   /
sysaudio+0x1d031
sysaudio+0x1b6e4   \
sysaudio+0x1e23a    > same triple again
sysaudio+0x1aa80   /
sysaudio+0x1d521
sysaudio+0x1934c
ks!KsQueueWorkItem+0xdc
```

**Reading the disassembly.** The faulting routine at `sysaudio+0x1d2b0` is a two-argument helper:

```
0x1d2b0  sub  rsp,28h
0x1d2b4  mov  rax,[rcx+8]      <- rcx is NULL
0x1d2c5  mov  rdx,[rax+8]
0x1d2cc  cmp  rdi,rdx
```

and its caller calls it **twice in a row with the same second argument and two different first arguments**:

```
0x1d266  mov  rcx,r15                  ; r15 = fffffa80`0056c8b8  - valid
0x1d269  call 0x1d2b0                  ; returns fine
0x1d279  mov  rbx,[rsp+0B8h]           ; rbx = 0                  - NULL
0x1d284  mov  rcx,rbx
0x1d287  call 0x1d2b0                  ; <- bugcheck
```

So sysaudio resolved one endpoint of something to a real object and the other endpoint to nothing. `r15` sits in a pool block tagged `SYSA` (sysaudio's own tag) holding a `KSPIN_INTERFACE` of `KSINTERFACESETID_Standard` — it is a pin/node object in sysaudio's graph.

And the frame directly below is the pin-pairing code:

```
0x1d057  cmp  dword ptr [rbx+10h],2    ; KSPIN_DATAFLOW_OUT ?
0x1d05d  cmp  dword ptr [rcx+10h],1    ; KSPIN_DATAFLOW_IN  ?
0x1d077  call sysaudio+0x1b320         ; -> the function that then crashes
```

That is sysaudio matching a DATAFLOW_OUT pin to a DATAFLOW_IN pin — i.e. following a physical connection between two filters — and then walking into the filter on the far side.

**Why this is new, and what it points at.** Before Stage 5at the two `PcRegisterPhysicalConnection` calls named `KSPIN_COMMUNICATION_SINK` streaming pins. sysaudio only follows `KSPROPERTY_PIN_PHYSICALCONNECTION` on a *bridge* pin, so it never followed those, never crossed into the topology filter, and never walked its graph — it just quietly failed to build a waveOut device. Stage 5at gave the wave filter real bridge pins. The first thing sysaudio did with them was cross into the topology filter, and the first thing it found there killed it. **The topology filter's graph had never once been walked before this build.**

And that graph was malformed:

```c
static PCCONNECTION_DESCRIPTOR MiniportConnections[] =
{
    { PCFILTER_NODE, PIN_TOPO_WAVEOUT_DEST, PCFILTER_NODE, PIN_TOPO_LINEOUT_DEST  },
    { PCFILTER_NODE, PIN_TOPO_MIC_SOURCE,   PCFILTER_NODE, PIN_TOPO_WAVEIN_SOURCE }
};
```

with `NodeSize = 0, NodeCount = 0, Nodes = NULL`. `PCFILTER_NODE` on **both** sides of both connections: the filter wiring its own pins straight to its own other pins with nothing in between. sysaudio resolves each connection endpoint to a node object; `PCFILTER_NODE` is not a node, so the resolution yields NULL, and `mov rax,[rcx+8]` does the rest.

Checked against the reference: **not one of the seventeen connections in `msvad\simple\toptable.h` is pin-to-pin.** Every single one has a real node on at least one end, and that sample carries twelve nodes to make it so. The wave filter, which Stage 5at gave DAC/ADC nodes and four node-routed connections, matches msvad exactly and is not implicated.

**The fix.** Two files, and it is small on purpose:

- **`shared.h`** — new `HdaTopoNode` enum: `NODE_TOPO_WAVEOUT_SUM = 0`, `NODE_TOPO_MIC_SUM = 1`.
- **`mintopo.cpp`** — a `TopologyNodes[]` table of two `KSNODETYPE_SUM` nodes, and the connections re-routed through them:

```c
{ PCFILTER_NODE,         PIN_TOPO_WAVEOUT_DEST, NODE_TOPO_WAVEOUT_SUM, 1                      },
{ NODE_TOPO_WAVEOUT_SUM, 0,                     PCFILTER_NODE,         PIN_TOPO_LINEOUT_DEST  },
{ PCFILTER_NODE,         PIN_TOPO_MIC_SOURCE,   NODE_TOPO_MIC_SUM,     1                      },
{ NODE_TOPO_MIC_SUM,     0,                     PCFILTER_NODE,         PIN_TOPO_WAVEIN_SOURCE }
```

  plus `NodeSize`/`NodeCount`/`Nodes` filled in on the filter descriptor. Pin 1 of a node is its input and pin 0 its output, hence the 1s and 0s.

  While in the file, the topology pins' data range changed from a `KSDATARANGE_AUDIO` (`2, 8, 32, 8000, 192000`) to a plain analog `KSDATARANGE`, matching `msvad\simple\toptable.h`'s `PinDataRangesBridge` and matching the range the wave filter's own bridge pins already advertise. All four pins on this filter are bridge pins; nobody opens one as a stream, so there was never a PCM range to negotiate here, and the two ends of a physical connection should agree.

**Why `KSNODETYPE_SUM` and not volume/mute.** `KSNODETYPE_SUM` defines no properties, so these two nodes need no automation table and cannot fail a property request. That makes this the smallest change that repairs the graph shape, which matters when the previous build bluescreened: if the device now appears, the diagnosis above is confirmed with almost no new code to have gotten wrong. Volume and mute nodes are a real and wanted addition — they are what the Windows volume slider binds to — but they need `KSPROPERTY_AUDIO_VOLUMELEVEL` / `KSPROPERTY_AUDIO_MUTE` handlers wired to the codec's amplifier verbs, and that is its own change (n36), not something to bundle into a crash fix.

**Build:** `chk x64 WNET`, clean — "9 files compiled - 1 Warning / 1 executable built" (the same long-standing warning). `stwrtxp.sys` is **47616 bytes**, staged in `Backported Driver/package/` along with a matching `stwrtxp.pdb`. Pre-change backups at `src/shared.h.bak-5at` and `src/mintopo.cpp.bak-5at`.

**Symbol caveat, unchanged from HANDOFF.md's earlier note.** The public symbol server no longer serves `ntoskrnl.pdb` for this OS build (3790.srv03_sp2_qfe.190711-0601), so `!analyze -v` says "Kernel symbols are WRONG" and every `nt!` / `ks!` name above is a nearest-export guess. Only `stwrtxp.pdb` resolves for real. Nothing in this diagnosis rests on those names — it rests on the raw disassembly, the register state, the `SYSA` pool tag, and the *absence* of any `stwrtxp` frame.

### Stage 5av: Stage 5au's node fix was correct — **no bluescreen, and sysaudio now walks the graph without complaint** — but Windows still shows no audio device. Every remaining static difference from `msvad\simple` was checked and ruled out, so this stage adds an `IOCTL_KS_PROPERTY` trace to find out what sysaudio actually asks for

The user installed the Stage 5au build. Result: **no BSOD** — the two `KSNODETYPE_SUM` nodes fixed the `sysaudio+0x1d2b4` NULL dereference, confirming the Stage 5au diagnosis. `kstest.exe` still plays a clean tone. But Control Panel still says "no audio device" and no ordinary playback works.

**What the new `stwrtxp_log.txt` (1309 lines) proves.**

The driver side is flawless. Second load: `InitCodec: render DAC[0] = NID 21`, both subdevices registered, and `StartDevice: complete, physical connections registered` with **no `PcRegisterPhysicalConnection ... failed` line** — both calls succeeded. Every `kstest.exe` stream runs the full `STOP → ACQUIRE → PAUSE → RUN → PAUSE → ACQUIRE → STOP` cycle, binds the converter (`verb 2 payload 4011`, `verb 706 payload 10`), and tears down returning both DMA engines.

Create statistics across the whole file: **444 successes, 4 failures** — and all four failures are `Name="(null)"` probes with `DesiredAccess=00100080`, which is normal handle-less querying, not a rejection. Nothing our driver is asked for is refused.

Run-length-encoding the create sequence splits it into two clean phases:

```
4:\Topology x3   4:\Wave x8      <- round 1
4:\Topology x3   4:\Wave x8      <- round 2
4:\Topology x3   4:\Wave x8      <- round 3
4:\Topology x3   4:\Wave x4      <- round 4
4:\Topology x4   4:\Wave x6
480:\Wave x2
3012:\Wave x1  3012:\Topology x1  (x189, alternating, forever)
```

Phase 1 is **sysaudio's kernel worker (PID 4) running four full enumeration rounds** — one per registered device interface, and we register exactly four (`KSCATEGORY_AUDIO`+Wave, `KSCATEGORY_RENDER`+Wave, `KSCATEGORY_CAPTURE`+Wave, `KSCATEGORY_AUDIO`+Topology). That is normal arrival-notification behaviour, not a retry storm. It opened both filters, read them, and stopped. Phase 2 is a user-mode process polling the two filters 189 times; every open succeeds and **no process other than `kstest.exe` (PIDs 1976/1988/1704) ever creates a pin** — pin creates are the only ones whose name starts with the `KSSTRING_Pin` GUID `{146F1A80-4791-11D0-A5D6-28DB04C10000}`.

So the blocker is no longer graph *shape*. sysaudio reads the filters cleanly and then silently declines to build a waveOut device.

**Everything static was checked against `msvad\simple` and matches.** This is worth recording so it is not re-litigated:

| Thing | msvad\simple | stwrtxp | Verdict |
|---|---|---|---|
| `AddInterface` set | AUDIO+Wave, RENDER+Wave, CAPTURE+Wave, AUDIO+Topology | identical | same — and note msvad does **not** register `KSCATEGORY_TOPOLOGY` either, so the suspicion recorded in Stage 5at's notes was wrong |
| `AlsoInstall` / `Needs` | `ks.registration`, `wdmaudio.registration` | `Needs = KS.Registration, WDMAUDIO.Registration` | equivalent |
| Wave filter `Categories` | `0, NULL` ("use defaults") | `0, NULL` | same |
| Topo filter `Categories` | `0, NULL` | `0, NULL` | same |
| Streaming pin range | `KSDATARANGE_AUDIO`, PCM/WAVEFORMATEX | same, `2, 16, 16, 8000, 48000` | same shape |
| Bridge pin range | plain `KSDATARANGE`, TYPE_AUDIO/SUBTYPE_ANALOG/SPECIFIER_NONE | identical since 5au | same |
| Bridge pin instance counts | `0, 0, 0`, `COMMUNICATION_NONE` | identical | same |
| Pin categories | `KSCATEGORY_AUDIO` on the four internal pins, `KSNODETYPE_SPEAKER` / `KSNODETYPE_MICROPHONE` on the connectors | identical | same |
| `DataRangeIntersection` | **returns `STATUS_NOT_IMPLEMENTED`** ("Portcls will handle the request for us") | returns `STATUS_NOT_IMPLEMENTED` | same — this candidate is dead |
| Wave nodes | `KSNODETYPE_DAC`, `KSNODETYPE_ADC`, 4 connections | identical | same |

Only two differences survive the comparison, and both are in the *property* surface rather than the graph:

1. **Both filter descriptors have `AutomationTable = NULL`.** msvad's wave filter uses `&AutomationWaveFilter` (`KSPROPERTY_GENERAL_COMPONENTID`, `KSPROPERTY_PIN_PROPOSEDATAFORMAT`); its topology filter uses `&AutomationTopoFilter` (`KSPROPERTY_JACK_DESCRIPTION`, a Vista-era property that XP never asks for).
2. **The topology node set.** msvad's render path is `WAVEOUT pin → VOLUME → MUTE → SUM → …devspecific… → VOLUME(master) → LINEOUT pin`, and its capture path is `MIC pin → VOLUME → MUX → WAVEIN pin`. Ours is a bare `SUM` on each path with no automation table at all. **Every path in msvad carries at least one `KSNODETYPE_VOLUME`.**

**Why this stage instruments instead of guessing.** Each test cycle costs the user a reboot and a driver install, and the two remaining candidates are both large changes (real volume/mute nodes wired to the codec's amplifier verbs is item n36 and is a substantial amount of new code). The `IRP_MJ_CREATE` hook shows handles opening but nothing about what flows through them, so there is currently no way to tell whether sysaudio ever asks for `KSPROPERTY_PIN_PHYSICALCONNECTION`, whether it gets a sane answer, or which property it first dislikes.

So `adapter.cpp` gains a second dispatch-table wrapper on the same pattern as the create hook — our own driver object, saved before overwrite, non-INIT code segment:

- `HookedMjDeviceControl` decodes `IOCTL_KS_PROPERTY` and logs `set / id / flags / pin-or-node id / in-len / out-len / status`, plus the `FileObject` so requests can be attributed to the Wave or the Topology filter by correlating against the `MJ_CREATE` lines.
- The `KSPROPERTY` lives in `Type3InputBuffer` (KS IOCTLs are `METHOD_NEITHER`) and can be a user-mode pointer, so it is read through `ReadKsPropertySafe` — a plain non-C++ helper with `ProbeForRead` (skipped for `KernelMode` requestors, where probing would raise) inside `__try`/`__except`. Keeping it in its own function avoids any object-unwinding conflict with the `__try`.
- Property sets are named by `Data1` alone (`Pin`, `Topology`, `Connection`, `Audio`, `General`, `Jack`, `Stream`, `Sysaudio`, …) — unique across KS audio and far cheaper than logging a full GUID per line.
- Past `DC_LOG_FULL_LIMIT` (6000) entries only failures are logged, so the 189-iteration user-mode poll loop cannot run the file to megabytes.

**This build is diagnostic only and changes no behaviour.** That is deliberate: bundling it with n36 would mean a new bugcheck teaches nothing, whereas a pure trace explains either outcome.

**Build:** `chk x64 WNET`, clean — "9 files compiled - 1 Warning / 1 executable built" (the same long-standing warning). `stwrtxp.sys` is **49152 bytes**, staged in `Backported Driver/package/` with a matching 478208-byte `stwrtxp.pdb`. Pre-change backup at `src/adapter.cpp.bak-5au`.

### Stage 5aw: the `IOCTL_KS_PROPERTY` trace came back — sysaudio walks both filters flawlessly and refuses **nothing** about the graph, but the very first question it asks each filter, `KSPROPERTY_GENERAL_COMPONENTID`, is answered `STATUS_PROPSET_NOT_FOUND`. Both filters get a real automation table

The Stage 5av build behaved exactly as intended: no bluescreen, `kstest.exe` still plays, Control Panel still empty, and a complete decoded property trace in the log.

**The trace is 120 entries and lives in one driver load (lines 1541-1698 of the log; `KSPROP #1` appears exactly once, and `MJ_CREATE` numbering runs unbroken 1..19).** Attribution took one extra step worth recording: `FileObject` addresses are recycled. `FFFFFADF3F5CE050` is `\Topology` for creates #1-#3 at lines 1541-1545 — which is where the property traffic on it happens — and the *same address* is reused for `\Wave` at line 1687, long after. Correlate by line number, not by address alone.

```
FO FFFFFADF3F5CE050  = Topology filter   KSPROP #1  - #60
FO FFFFFADF44558F40  = Wave filter       KSPROP #61 - #118
FO FFFFFADF4463E970  = Wave, PID 480     KSPROP #119
FO FFFFFADF44A01AF0  = Wave, PID 480     KSPROP #120
```

**What sysaudio asks, and what it gets.** Both passes are identical in shape: `KSPROPSETID_Topology` ids 0/1/2 (categories, nodes, connections), then `KSPROPSETID_Pin` ids 0, 2, 7, 11, 12, 10, 5, 6, 3 across pins 0-3. Sizes decode exactly as they should — `KSMULTIPLE_ITEM` (8 bytes) plus 8-byte-aligned entries:

| Request | Topology filter | Wave filter | Reading |
|---|---|---|---|
| `TOPOLOGY_CATEGORIES` | `out=40` → 2 | `out=56` → 3 | PortCls synthesises these from the registered interfaces; Wave has AUDIO+RENDER+CAPTURE, exactly as the INF declares |
| `TOPOLOGY_NODES` | `out=40` → 2 | `out=40` → 2 | both node tables read correctly |
| `TOPOLOGY_CONNECTIONS` | `out=72` → 4 | `out=72` → 4 | both connection tables read correctly |
| `PIN_DATARANGES` | `out=72` on all 4 pins | `out=184` pins 0/1, `out=72` pins 2/3 | 72 = one plain analog `KSDATARANGE`; 184 = two 88-byte `KSDATARANGE_AUDIO` entries on the streaming pins |
| **`PIN_PHYSICALCONNECTION`** | **pins 0/1 `out=258` SUCCESS**, pins 2/3 `C0000225` | pins 0/1 `C0000225`, **pins 2/3 `out=266` SUCCESS** | **exactly right** — the two bridge pins on each filter resolve, the other two correctly report no connection |

That last row is the important one. **The cross-filter hop works.** `PcRegisterPhysicalConnection` not only returned success at `StartDevice` time, it produces correct answers on precisely the four pins that should have them and correct refusals on the four that should not. The hypothesis that carried over from Stage 5at and 5au — that sysaudio could not follow the wave filter into the topology filter — is **dead**, and should not be revisited.

`PIN_NAME` (id 12) returns `C0000034` wherever a pin has `Name = NULL` and succeeds on the two topology connector pins (`out=16` for `KSNODETYPE_SPEAKER`, `out=22` for `KSNODETYPE_MICROPHONE`), because PortCls resolves those from the pin *category* GUID. That is normal and matches msvad, which also leaves internal pin names NULL.

**Out of 118 enumeration requests exactly one kind is refused, and it is the first question asked of each filter:**

```
KSPROP #1:  TOPO  General(1464EDA5) id=0 f1 in=24 out=72 -> C0000230
KSPROP #61: WAVE  General(1464EDA5) id=0 f1 in=24 out=72 -> C0000230
```

`1464EDA5` is `KSPROPSETID_General`, id 0 is `KSPROPERTY_GENERAL_COMPONENTID`, `C0000230` is **`STATUS_PROPSET_NOT_FOUND`**, and `out=72` is `sizeof(KSCOMPONENTID)` **exactly** — four GUIDs plus two ULONGs. sysaudio is not probing for a size; it arrives with a correctly pre-sized buffer, expecting an answer. It gets told the property set does not exist, because both `PCFILTER_DESCRIPTOR`s carried `AutomationTable = NULL` and this driver published no filter-level property set at all.

This was one of only two differences the Stage 5av comparison table could still find against `msvad\simple`, and it is now confirmed by direct observation rather than inferred. **Every WDK audio sample answers this**: `msvad\simple` on its wave filter (`wavtable.h:193`), and `sb16` on **both** filters from a single shared `AutomationFilter` table (`common.h:341`, used at `minwave.cpp:723` and `tables.h:741`). sb16 is a real hardware driver rather than a virtual one, so its shape is the one copied here.

**The second refusal is the last thing that happens in the entire log:**

```
KSPROP #119: PID=480 WAVE Audio(45FFAAA0) id=40 f2(SET) in=24 out=16 -> C0000230
KSPROP #120: PID=480 WAVE Audio(45FFAAA0) id=40 f2(SET) in=24 out=16 -> C0000230
```

Property 40 on `KSPROPSETID_Audio` is **`KSPROPERTY_AUDIO_PREFERRED_STATUS`**, and `out=16` is `sizeof(KSAUDIO_PREFERRED_STATUS)` exactly (`BOOL Enable; KSPROPERTY_SYSAUDIO_DEFAULT_TYPE DeviceType; ULONG Flags; ULONG Reserved`). A user-mode service (PID 480) opens the Wave filter twice and tries to tell it that it has been selected as the preferred device. `in=24` is a plain `KSPROPERTY`, not a `KSNODEPROPERTY`, so this is aimed at the filter, not at a node.

**This property is guarded `NTDDI_VERSION >= NTDDI_WINXP && < NTDDI_VISTA` in `ksmedia.h`.** It does not exist on Vista. Grepping the whole WDK 7600 source tree for `PREFERRED_STATUS` returns hits only inside `.pdb` files — i.e. it survives in debug info pulled from the header, and **not one sample implements it**, because they are all built Vista-targeted. Which means the closed-source Vista-era `stwrt64.sys` this project is backporting could not have implemented it either. It is precisely the class of gap a Vista-to-XP backport has to fill, and it is worth noting that nothing at all happens in the log after these two failures.

**The change.** On the sb16 pattern: `shared.h` gains one `PropertiesFilter[]` table wrapped by `DEFINE_PCAUTOMATION_TABLE_PROP(AutomationFilter, ...)`, and **both** `MiniportFilterDescriptor` (mintopo.cpp) and `MiniportWaveFilterDescriptor` (wavecyclicminiport.cpp) now point `AutomationTable` at it instead of `NULL`. Two handlers go into `common.cpp`, inside its trailing `#pragma code_seg("PAGE")` region — correct, since PortCls issues property requests at `PASSIVE_LEVEL`:

- `PropertyHandler_ComponentId` — fills a `KSCOMPONENTID` with `MM_MICROSOFT` as manufacturer (there is no `MM_` id for IDT/Sigmatel in `mmreg.h`) plus two GUIDs generated for this project, `PID_STWRTXP` and `NAME_STWRTXP`. It uses sb16's three-way GET size check rather than msvad's single check, so a zero-length probe gets `STATUS_BUFFER_OVERFLOW` and the required size; Stage 5av's trace shows sysaudio skipping the probe, but other callers do not. `Component` is zeroed with `RtlZeroMemory` rather than assigned `GUID_NULL`, to avoid depending on that symbol's storage being linked in.
- `PropertyHandler_PreferredStatus` — accepts the SET and logs `Enable` / `DeviceType` / `Flags`, which the Stage 5av hook could not see. There is nothing to program on the codec for this.

Both also answer `KSPROPERTY_TYPE_BASICSUPPORT` with their access flags, as every sample does.

**And the INF.** `KSCOMPONENTID.Name` is a GUID the audio stack resolves to a display name through `HKLM\SYSTEM\CurrentControlSet\Control\MediaCategories`. `sb16.inf:154` registers `NAME_MSSB16` there; without the entry the GUID this driver reports would dangle. `[StwrtXP.AddReg]` gains `HKLM,%MediaCategories%\%Name.Guid%,Name,,%DeviceDesc%` and `[Strings]` gains `MediaCategories` and `Name.Guid`. This closes the one *other* INF divergence Stage 5av noted. (The remaining one — our `Drivers,SubClasses` is `"wave"` where the samples say `"wave,midi,mixer"` — is deliberately left alone: this driver has no midi and no mixer nodes, and claiming subclasses it cannot back would be a new guess rather than a fix.)

**Honest assessment of the odds.** `KSPROPERTY_GENERAL_COMPONENTID` is documented as an identification property, not a gating one, so this is not a guaranteed fix. What makes it the right next move anyway is that it is now the *only* thing in a 120-entry trace that sysaudio asked for and did not get, it is asked before anything else on both filters, every reference driver implements it, and it is cheap. If the device still does not appear, the next trace will look different in a way that says so — and n36 (real volume/mute nodes) is the remaining candidate.

**Build:** `chk x64 WNET`, clean — "9 files compiled - 1 Warning / 1 executable built", the same long-standing warning. `stwrtxp.sys` is **51200 bytes** (up from 49152), staged in `Backported Driver/package/` with a matching 486400-byte `stwrtxp.pdb` and the updated `stwrtxp.inf` (7156 bytes). The Stage 5av `IRP_MJ_CREATE` / `IRP_MJ_DEVICE_CONTROL` diagnostic hooks are **still in** deliberately, so the next log is directly comparable to this one. Pre-change backups: `src/shared.h.bak-5av`, `src/common.cpp.bak-5av`, `src/mintopo.cpp.bak-5av`, `src/wavecyclicminiport.cpp.bak-5av`, `src/stwrtxp.inf.bak-5av`.

### Stage 5ax: the Stage 5aw property fix is CONFIRMED WORKING and CONFIRMED NOT TO BE THE BUG — sysaudio's enumeration is byte-for-byte identical with it and without it. The last remaining divergence from every WDM audio driver in the WDK is that this INF registers only the `wave` subclass, never `mixer`

The Stage 5aw build installed cleanly. The new log (`H:\BACKPORTED IDT DRIVER\stwrtxp_log.txt`, 2517 lines / 212,240 bytes) accumulates **four** driver loads, delimited by `DriverEntry:` at lines 23, 220, 1384 and 2077. Load 3 (1384-2076) is Stage 5av; load 4 (2077-end) is Stage 5aw. Both carry the `IRP_MJ_DEVICE_CONTROL` hook, so they are directly comparable.

**Both new property handlers fire and both succeed:**

```
2240: PropertyHandler_ComponentId:     verb=00000001 size=72 -> 00000000
2309: PropertyHandler_ComponentId:     verb=00000001 size=72 -> 00000000
2390: PropertyHandler_PreferredStatus: Enable=1 DeviceType=1 Flags=00000000
2391: PropertyHandler_PreferredStatus: verb=00000002 size=16 -> 00000000
2395: PropertyHandler_PreferredStatus: Enable=1 DeviceType=2 Flags=00000000
2396: PropertyHandler_PreferredStatus: verb=00000002 size=16 -> 00000000
```

`DeviceType` 1 and 2 are `KSPROPERTY_SYSAUDIO_PLAYBACK_DEFAULT` and `KSPROPERTY_SYSAUDIO_RECORD_DEFAULT` (`ksmedia.h:1584-1589`), both with `Enable=1`. A **user-mode** process (PID 480, i.e. a service, not the PID 4 kernel worker) opened `\Wave` on its own and marked this device the system default for both playback and recording. The device interface is discoverable from user mode and the system considers it the preferred device.

**And it made no difference whatsoever.** Extracting sysaudio's enumeration (PID 4 only) from each load and diffing:

```
load3 PID4 lines: 118   load4 PID4 lines: 118
1c1
< set=General(1464EDA5) id=0 ... out=72 -> C0000230
> set=General(1464EDA5) id=0 ... out=72 -> 00000000
61c61
< set=General(1464EDA5) id=0 ... out=72 -> C0000230
> set=General(1464EDA5) id=0 ... out=72 -> 00000000
```

Two lines differ out of 118, and they are exactly the two statuses Stage 5aw set out to change. **sysaudio asks the identical 118 questions in the identical order and stops in the identical place.** `KSPROPERTY_GENERAL_COMPONENTID` was a real gap and is correctly closed, but it was never the gate. Stage 5aw's own honest caveat ("documented as an identification property, not a gating one") turned out to be the accurate reading.

**A Stage 5aw claim that has to be withdrawn.** Stage 5aw asserted that no process except `kstest.exe` ever creates a pin, and nominated "a pin create from another process" as the success signal to watch for. That was drawn from a log snapshot that had been truncated at line 1698, mid-load-3. The complete load 3 contains four pin creates from PID 1704 (`MJ_CREATE` with `Name="{146F1A80-4791-11D0-A5D6-28DB04C10000}"`, i.e. `KSNAME_Pin`), all under the *old* driver. PID 1704 is kstest. Load 4 has two, from the same PID. So pin creation was never a discriminator, and load 4's streaming activity is kstest, not the system. **Lesson, on top of Stage 5aw's own: a log the user attaches mid-session may be a prefix of the file, not the whole of it. Re-derive load boundaries from `DriverEntry:` every time rather than trusting a previous window's line numbers.**

**Everything the trace can see is healthy.** Every returned size in load 4 decodes exactly, on both filters: `PIN_CTYPES` out=4 (4 pins), `PIN_CINSTANCES` out=8, `PIN_CATEGORY` out=16, `PIN_INTERFACES` out=56 on the streaming pins and out=32 on the bridge pins (a `KSPIN_INTERFACE` is a 24-byte `KSIDENTIFIER`, so 8+24+24 and 8+24 - two interfaces and one), `PIN_MEDIUMS` out=32 (one), `PIN_DATARANGES` out=184 on the streaming pins and out=72 on the bridge pins, `PIN_PHYSICALCONNECTION` out=266/258 on exactly the four bridge pins, `TOPOLOGY_CATEGORIES` out=56 on wave (3 GUIDs) and out=40 on topology (2), `TOPOLOGY_NODES` out=40 (2) and `TOPOLOGY_CONNECTIONS` out=72 (4) on both. The only non-property IOCTL in either load is `002F8013` = `IOCTL_KS_WRITE_STREAM` (`FILE_DEVICE_KS`, function 4, `METHOD_NEITHER`, `FILE_WRITE_ACCESS`), always returning `STATUS_PENDING`, always from kstest's streams.

So the fault is not in anything PortCls exposes. It is above PortCls, in a layer this trace cannot see.

**Which leaves exactly one divergence from every WDM audio driver in the WDK, and it is in the INF.** The interface registrations were re-checked against `msvad.inf:91-94` and are identical to ours, including the fact that the topology filter is registered only under `KSCATEGORY_AUDIO` and not `KSCATEGORY_TOPOLOGY` - so that is not a difference. `Include`/`Needs` (`ks.inf`, `wdmaudio.inf`, `KS.Registration`, `WDMAUDIO.Registration`) are present and correct. `AssociatedFilters`, `Driver` and `Drivers\wave\wdmaud.drv\Driver` are all present. What is missing is the rest of the subclass registration:

```
                              msvad.inf:100-108        stwrtxp.inf (before 5ax)
Drivers,SubClasses            "wave,midi,mixer"        "wave"
Drivers\midi\wdmaud.drv       Driver + Description     absent
Drivers\mixer\wdmaud.drv      Driver + Description     absent
```

Stage 5aw looked at this and deliberately left it alone, reasoning that "this driver has no midi and no mixer nodes, and claiming subclasses it cannot back would be a new guess rather than a fix." **That reasoning was wrong on both halves.** msvad has no MIDI hardware either - it declares the `midi` subclass because `swmidi`, the software synth, supplies it, and `swmidi` is already named in our own `AssociatedFilters` line. And the mixer device is not built from miniport nodes at all: `wdmaud.drv` builds it from the **topology filter**, which this driver has and which Stage 5aw's trace proves sysaudio reads correctly. Declaring these subclasses does not claim hardware; it names which `wdmaud` sub-drivers `winmm` should attach to this device.

This is a good fit for the symptom. XP's "Sounds and Audio Devices" applet reports **"no audio device"** off the mixer/volume device, and the mixer subclass was never registered - and `wdmaud`/`winmm` is precisely the one layer above PortCls that the 118-request trace cannot see, while everything below it is provably healthy and a user-mode service can already find `\Wave` and mark it preferred.

**The change** is INF-only; no code changed and `stwrtxp.sys` is untouched at Stage 5aw's 51200 bytes. `[StwrtXP.AddReg]` now reads `HKR,Drivers,SubClasses,,"wave,midi,mixer"` and gains `Driver`/`Description` pairs for `Drivers\midi\wdmaud.drv` and `Drivers\mixer\wdmaud.drv`; `[Strings]` gains `MidiDesc`. `stwrtxp.inf` is 8375 bytes. Pre-change backup: `src/stwrtxp.inf.bak-5aw`.

**Because it is registry-only, it can be tested without reinstalling anything.** `HKR` in `[StwrtXP.AddReg]` resolves to the device's software key, `HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e96c-e325-11ce-bfc1-08002be10318}\NNNN`, so the same three entries can be written by hand and the theory settled with a reboot instead of an uninstall/reinstall cycle. `Backported Driver/package/quicktest-5ax.vbs` does exactly that. It finds `NNNN` by itself: it enumerates the MEDIA class subkeys through WMI `StdRegProv` (`root\default`), picks the one whose `MatchingDeviceId` contains `dev_76c7`, and refuses to write unless that key's existing `Drivers\wave\wdmaud.drv\Driver` reads back as `wdmaud.drv`. Run it as `cscript //nologo quicktest-5ax.vbs` from an Administrator Command Prompt. `quicktest-5ax.reg` is the manual fallback with the same three entries. **Correction worth recording so it is not repeated: Windows XP's Device Manager Details tab has NO "Driver key" entry — that field is Vista-era.** On XP the index has to be found in regedit by clicking each numbered subkey under the MEDIA class GUID and matching `MatchingDeviceId` / `DriverDesc`, which is exactly the manual step the `.vbs` removes. XP has no PowerShell, but WSH (`cscript`) and WMI `StdRegProv` are both present. The corrected INF is what should be used for the next real reinstall either way.

**Honest assessment of the odds.** Better than Stage 5aw's, for one specific reason: Stage 5aw was fixing something sysaudio asked for and did not get, which was suggestive but turned out to be non-gating, whereas this is a registration that every working WDM audio driver on this OS has and this one does not, sitting in exactly the layer that the evidence has now cornered the fault into. It is still not certain - if `winmm` is not the blocker then the mixer registration changes nothing - but the trace has eliminated everything below it. If this does not do it, n36 (real volume/mute nodes) is next, and it may well be needed *together* with this: a mixer device built from a topology whose only nodes are property-less `KSNODETYPE_SUM` may enumerate with no lines and be rejected.

### Stage 5ay: the mixer subclass WORKED — Control Panel lists the device and system playback runs end to end — but it is **silent while kstest is audible on the same boot**, and the trace of the two is identical, so the fault is buffer *content*. Real `KSNODETYPE_VOLUME`/`KSNODETYPE_MUTE` nodes wired to the codec's amplifier (item n36), plus a DMA-buffer scan to settle it outright

**Stage 5ax is confirmed.** The three registry entries went in, and after the reboot the user reported: *"It lists the device!! but no sounds work, even though windows DOES INDEED detect a device, it lets me play the sounds, I just can't hear em."* The log backs that up completely and it is a bigger step than it sounds. For the first time in this project's history the log contains `KSNAME_Pin` creates that are **not** from `kstest.exe` — PIDs 472 and 1752 — with two complete playback sessions: pin create, `SetFormat`, engine allocate, buffer allocate, `KSSTATE_RUN`, **497 `IOCTL_KS_WRITE_STREAM`** (`0x2F8013`), then a clean stop, engine free and teardown with no leak. The entire WDM audio stack above PortCls — `winmm`, `wdmaud`, `kmixer`, `sysaudio` — is now wired to this driver and streaming through it. The `"wave"`-only `SubClasses` value really was the blocker for device *recognition*, exactly as Stage 5ax predicted.

**Two things remain wrong**, and the user confirmed both: no audible output from system playback, and the volume sliders are still greyed out. The user also confirmed the decisive control: *"ktest's audio test still works"* — **kstest is audible on the same boot on which system playback is silent.**

**That A/B is what makes this diagnosable.** It rules out, in one observation, every hardware-side explanation that was still live: the codec is initialized, the DAC is unmuted, the pin widget is powered and enabled, EAPD is set, the DMA engine runs, the physical output path works, the amplifier is not muted. Both paths reach the same pin on the same miniport through the same PortCls copy loop into the same cyclic buffer. So the difference has to be upstream of the driver, and there is exactly one thing upstream that differs: **kstest writes its tone straight to the pin, and system playback goes through `kmixer` first.**

**The trace diff confirms there is nothing else left to find in the driver.** Comparing the silent system-playback session against the audible kstest session in the same log:

| | kstest session | system session |
|---|---|---|
| codec verbs issued | md5-identical | md5-identical |
| stream lifecycle | create → SetFormat → alloc engine → alloc buffer → RUN → STOP → free | same, same order |
| format negotiated | `4111` (22050/16/2) or `4011` (44100/16/2) | same words |
| write IOCTL | `0x2F8013`, 56-byte `KSSTREAM_HEADER` | same |
| property misses | same benign set | same benign set |

Every observable is the same. The driver cannot tell the two apart, which means the driver is not what is making one of them silent. **The one variable no log line has ever looked at is what is actually in the bytes.**

**Why kmixer would deliver silence, and why that is the same bug as the greyed-out sliders.** `wdmaud.drv` builds the Windows mixer device out of the **topology filter**. Since Stage 5au that filter has had exactly two nodes, both bare `KSNODETYPE_SUM` pass-throughs with no automation table at all — chosen deliberately then as the minimum shape that made the graph traversable without adding a property surface to get wrong, which was the right call for a crash fix and the wrong thing to ship. A mixer device built from a topology with no volume or mute node has no line to draw a slider for (hence: greyed out) and, more to the point, nothing for XP's `kmixer` to read a per-stream gain from. `kmixer` applies gain in software on the way to the pin. A mixer line whose volume cannot be read is exactly the shape that ends with it multiplying by zero. Stage 5ax's own closing paragraph called this shot: *"a mixer device built from a topology whose only nodes are property-less `KSNODETYPE_SUM` may enumerate with no lines and be rejected"* — and item n36 was already on the list for it.

**So Stage 5ay implements n36: real volume and mute nodes, backed by real hardware.**

The node graph is now the msvad shape (`shared.h`'s `HdaTopoNode`, seven nodes, and it replaces the two old `NODE_TOPO_*_SUM` entries entirely):

```
render:   WAVEOUT pin -> VOLUME -> MUTE -> SUM -> VOLUME -> MUTE -> LINEOUT pin
capture:  MIC pin -> VOLUME -> SUM -> WAVEIN pin
```

Nine connections, and **the Stage 5au rule still holds throughout: not one of them is pin-to-pin.** Node pin 1 is input, pin 0 is output. Each volume node gets `KSPROPERTY_AUDIO_VOLUMELEVEL` (GET/SET/BASICSUPPORT), each mute node `KSPROPERTY_AUDIO_MUTE`, and both also answer `KSPROPERTY_AUDIO_CPU_RESOURCES` — `kmixer` asks that of each node it finds and a node that refuses may not get a line built for it. The nodes carry the well-known `KSAUDFNAME_*` name GUIDs (`WAVE_VOLUME`, `WAVE_MUTE`, `MASTER_VOLUME`, `MASTER_MUTE`, `MIC_VOLUME`), which is where the mixer line's displayed text comes from; `wdmaudio.inf` registers those under `Control\MediaCategories` on every XP install.

**The design decision worth recording, because the obvious implementation is wrong.** The hardware has exactly **one** attenuator on the render path — the output amp on the render DACs — but the msvad shape has **two** cascaded gain stages, wave and master. The tempting shortcut is to let the master drive the hardware and make the wave stage a software no-op, which ships a slider that silently does nothing. Instead `IHdaAdapterCommon` keeps independent per-stage, per-channel levels in KS's 1/65536 dB units and **adds them** before programming the single amp, muting if **either** stage is muted (`CHdaAdapterCommon::ProgramOutputAmp`). Decibels are logarithmic, so summing them is precisely what two real attenuators in series do — this is not an approximation. A third stage, `HDA_GAIN_CAPTURE`, is state-only: this backport has no ADC path, and the node exists only so the recording mixer line is well formed.

**The range is read from the codec, not invented.** `QueryOutputAmpCaps()` runs at the end of `InitCodec` and reads `HDA_PARAM_OUTPUT_AMP_CAP` (`0x12`) from the first render DAC, falling back to the AFG's if the widget does not override it. `AMP_CAP` decodes as offset (bits 6:0, the step index meaning 0 dB), NumSteps (14:8) and StepSize (22:16, where a step is `(StepSize+1) x 0.25 dB`); 0.25 dB is 16384 KS units. That gives `m_VolumeMinimum`/`m_VolumeMaximum`/`m_VolumeStep` for `BASICSUPPORT` to report and `LevelToAmpStep()` to quantize against. If the codec reports no output amp, `m_bAmpCapsValid` stays FALSE and the nodes cache values without sending verbs — a dead slider rather than a wild verb.

**Also added: a temporary DMA-buffer scan, and it is the part that settles the argument.** `StopEngine()` now walks the cyclic buffer and logs sample count, non-zero count and peak absolute sample. This is deliberately independent of whether the volume fix works:

- **peak = 0 on a system playback session** → kmixer really did deliver silence, the diagnosis above is confirmed, and if the volume nodes did not fix it the next place to look is what `kmixer` reads off the new mixer line.
- **peak large but still inaudible** → the diagnosis is wrong, the samples were there all along, and the fault is between the buffer and the speaker — which would point at the amp programming or the pin widget, not at kmixer.

Either answer is worth having, so it goes in this build regardless. Safe to walk: `StopEngine` is `PAGED_CODE()`/PASSIVE_LEVEL, the engine has just been paused, and `SystemAddress()` is the MDL's kernel mapping. **It is a diagnostic and is listed in item p for removal.**

**Files changed** (backups `*.bak-5ax` in `src/`): `hdaverbs.h` 5430→6799 (`HDA_PARAM_OUTPUT_AMP_CAP`/`INPUT_AMP_CAP`, the `HDA_AMPCAP_*` decode macros, the `HDA_AMP_SET_*` payload bits, and the two widget-cap bits); `shared.h` 17975→21164 (the new `HdaTopoNode` and `HdaGainStage` enums, five new `IHdaAdapterCommon` methods); `common.h` 6761→8665; `common.cpp` 50740→60236 (`QueryOutputAmpCaps`, `LevelToAmpStep`, `ProgramOutputAmp`, and the five interface methods); `mintopo.h` (a public `AdapterCommon()` accessor — the property handlers are plain functions and recover the miniport by casting `PropertyRequest->MajorTarget`, the standard msvad/sb16 pattern); `mintopo.cpp` (the node/connection tables, the two automation tables, `PropertyHandler_Volume`/`_Mute`/`_CpuResources` and the two `BASICSUPPORT` helpers); `wavecyclicstream.cpp` (the buffer scan). **Built clean:** `chk x64 WNET`, 9 files compiled, 1 warning (the standing benign "x64 Native compiling isn't supported"), 1 executable built, zero errors. `stwrtxp.sys` is **60928 bytes**, up from Stage 5aw/5ax's 51200. The INF is unchanged at 8375 bytes and already correct.

**WDK facts verified while writing this, recorded so they are not re-checked:** `KSPROPERTY_STEPPING_LONG` is `{ SteppingDelta; Reserved; Bounds; }` — `Bounds` is **last**, and is reached as `.Bounds.SignedMinimum`/`.SignedMaximum`. `KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU` is **`0x00000000`**, not 1. `VT_I4`/`VT_BOOL` are already enum members in `ks.h:161`, so no `#define` is needed (the guards in `mintopo.cpp` are inert). `stdunk.h`'s placement `operator new` zeroes the allocation, so the new `CHdaAdapterCommon` members start at zero. In C++ the `KSAUDFNAME_*`/`KSNODETYPE_*` names expand through `DEFINE_GUIDNAMED` to `__uuidof(...)`, so taking their address needs no `ksguid.lib`.

**~~A latent bug found while reading the log and deliberately left alone~~ — WITHDRAWN IN STAGE 5az; THERE IS NO SUCH BUG.** The claim was that `FreeBuffer` + `ReleaseEngine` fire *between* the two `AllocateBuffer` calls, leaving a DMA buffer allocated against a just-released engine handle, because `AllocateBuffer` only guards on `m_bEngineHandleValid` and `ReleaseEngine` never clears it. Both halves are wrong and both were checked against the source rather than the log in Stage 5az. `ReleaseEngine` **does** clear `m_bEngineHandleValid` (`wavecyclicstream.cpp`, right after the `FreeDmaEngine` block, alongside `m_EngineHandle = NULL` and `SetEngineHandle(NULL)`). And the second `AllocateBuffer` is not stray: it is the Stage 5ap re-allocation inside `SetFormat` itself, which runs **after** `AllocateRenderDmaEngine` has produced the replacement handle and after `SetEngineHandle(handle)` has installed it. It only *appears* to precede the new engine in the trace because `SetFormat`'s own `DOUT` is the last statement in the function, so it prints after the `AllocateBuffer` line it actually precedes in execution. Reading the log as a call order rather than as a print order is what produced the phantom. **Do not re-open this.**

**Honest assessment of the odds.** Good, with a named fallback. The greyed-out sliders are *certainly* this bug — there is no other way for a slider to appear for a node that does not exist — so that half is a fix, not a guess. The silence is a strong inference rather than a proof: kstest-audible-while-system-silent isolates the fault to kmixer, and a missing volume control is the most common reason kmixer outputs zeros, but "most common" is not "only". The DMA scan is in the build precisely so that a failed attempt still returns a hard answer instead of another round of speculation.

**Next action: install this build and retest.** Ship `package/stwrtxp.sys` (60928 bytes) with the unchanged `package/stwrtxp.inf`. What to look for in the fresh log: `QueryOutputAmpCaps:` at load (the decoded amp geometry — if it says `AMP_CAP=00000000` or logs the "no output amp" warning, the codec is not reporting caps and the range is the fallback), `PropertyHandler_Volume: ... BASICSUPPORT` and `ProgramOutputAmp:` lines when the sliders are touched, and above all the `StopEngine: DMA buffer ... peak |sample| = N` line after a system playback attempt.

### Stage 5az: the volume nodes worked, and the DMA scan disproved its own diagnosis — the buffer was **full of loud audio** the whole time. The silence is the **cyclic buffer size**: PortCls fills a WaveCyclic buffer *behind* the play cursor, so latency is one full buffer lap, and at `0x16000` the lap was longer than any sound Windows was trying to play

**What the Stage 5ay build actually did on hardware.** Half of it is a confirmed success and the other half is a confirmed refutation, which is exactly what it was built to produce.

Success: the user reports **the volume sliders work and Control Panel definitely sees the device.** The log backs it up at every level — `QueryOutputAmpCaps: NID 21 AMP_CAP=80027F7F offset=127 nsteps=127 stepsize=2 -> range -6242304..0 step 49152 (1/65536 dB)` at load, `PropertyHandler_Volume: node=N BASICSUPPORT range -6242304..0 step 49152 ... -> 00000000`, `PropertyHandler_CpuResources: node=N verb=10000001 size=4 -> 00000000`, and a long run of `ProgramOutputAmp: wave L=0 R=0 mute=0, master L=... -> amp steps L=127 R=127 mute=0` tracking slider drags. **Item n36 is done.** Every one of the 40-odd `ProgramOutputAmp` lines in the run lands on amp step 125, 126 or 127 with `mute=0`, so the attenuator was within about half a decibel of full scale for the whole session and the amp is definitively **not** the reason for the silence. (That the whole slider only spans two steps is its own, separate, cosmetic problem — recorded as n42.)

Refutation: **the kmixer-delivers-silence diagnosis is dead.** The `StopEngine` scan added in 5ay for precisely this purpose returned, over five separate playback sessions:

```
StopEngine: DMA buffer 90112 bytes / 45056 samples, 42498 non-zero, peak |sample| =   751
StopEngine: DMA buffer 90112 bytes / 45056 samples, 38881 non-zero, peak |sample| =   270
StopEngine: DMA buffer 90112 bytes / 45056 samples, 17002 non-zero, peak |sample| =  8477
StopEngine: DMA buffer 90112 bytes / 45056 samples, 20122 non-zero, peak |sample| = 22679
StopEngine: DMA buffer 90112 bytes / 45056 samples, 37942 non-zero, peak |sample| = 22679
```

(The interleaved `peak = 0` lines are the `ACQUIRE -> PAUSE` transition, before anything has been written. They are the control, and they confirm the buffer starts genuinely zeroed.)

That is real, loud audio — `22679` is two thirds of full scale. Stage 5ay said in advance that a large peak "disproves the whole diagnosis and moves the hunt to between the buffer and the speaker". So it does. kmixer was never multiplying by zero.

**Reading the numbers is what solves it.** With the samples in the buffer, the amp at 0 dB unmuted, the converter bound (`BindRenderConverters: NID 21 bound to tag 1, format 4111`) and `StartEngine: running`, the fault has to be between the buffer and the hardware — and the arithmetic of *how much* of the buffer was written names it exactly.

`GetPosition()` reads the real hardware link position register, so the fact that PortCls kept writing at all proves the DMA engine was genuinely running and its position genuinely advancing. Now compare, per session, the bytes written against one buffer's worth of playing time:

| non-zero samples | share of buffer | bytes | rate | that many bytes = | one lap = | heard? |
|---|---|---|---|---|---|---|
| 42498 / 45056 | 94% | 84705 | 44100 | 480 ms | **511 ms** | no |
| 38881 / 45056 | 86% | 77762 | 22050 | 881 ms | **1022 ms** | no |
| 37942 / 45056 | 84% | 75884 | 22050 | 860 ms | **1022 ms** | no |
| 20122 / 45056 | 45% | 40244 | 22050 | 456 ms | **1022 ms** | no |
| 17002 / 45056 | 38% | 34004 | 22050 | 385 ms | **1022 ms** | no |

**Not one session ever filled a full lap.** Not one of them came within 40 ms of it, and the largest got to 94%. That is not a coincidence across five independent sessions; it is a mechanism.

The mechanism is how PortCls's WaveCyclic pin fills the buffer. On each service call it reads `IDmaChannel::GetPosition()`, computes how many bytes the hardware consumed since the previous call, and copies exactly that many bytes from the client's queued IRPs into **exactly the region they were just consumed from**. It fills *behind* the play cursor, never ahead of it. The consequence is that audio handed to a WaveCyclic pin is not heard until the cursor comes round again: **the latency of a WaveCyclic stream is one full buffer lap, and the lap is the buffer's own duration.**

The independent check on that model is the fill rate itself. In the 20122-sample session PortCls issued 48 `IOCTL_KS_WRITE_STREAM` at a 10 ms notification interval — 480 ms of audio — and wrote 456 ms worth of bytes. Written ≈ elapsed, to within one service period. That is the signature of "copy exactly what was consumed". Had PortCls been filling *ahead* of the cursor it would have run the buffer to full almost immediately and stayed there, and every one of those five lines would have read ~100% non-zero regardless of how short the sound was. None of them do.

So: the buffer is `0x16000` = 90112 bytes, the lap is **1.02 s at 22.05 kHz and 511 ms at 44.1 kHz**, and every sound Windows asked this driver to play ended (`RUN -> PAUSE`) before the cursor got back round to its own data. The hardware faithfully played the zeros the buffer was created with, one lap at a time, and then the stream was torn down. **The audio was always there and was never once reached.**

**And that is why kstest was audible on every single boot for the last six stages.** Same code, same path, same converter, same amp. kstest plays a continuous tone from 8 large pre-queued buffers, so it simply outlives the first lap; everything after the first half-second is heard normally. A Windows event sound is 200 ms to 1 s and dies inside the lap. The "kstest works but nothing else does" pattern that has shaped this whole investigation was never about kstest bypassing kmixer at all — it was about **duration**.

**The fix is the buffer size.** `HDA_MAX_DMA_BUFFER_SIZE` in `dmachannel.h` goes from `0x16000` to `0x4000`. The old figure was copied from `msvad/msvad.h:61` — and msvad is a *virtual* driver whose "hardware" never plays a sample, so the one property of that number that matters here was never exercised where it came from. `0x4000` is the figure the WDK samples that drive real hardware use, and it puts the latency back where WaveCyclic latency belongs: **85 ms at 48 kHz, 93 ms at 44.1 kHz, 186 ms at 22.05 kHz.** It stays an exact multiple of the 128-byte granule HDA works in (`0x4000 / 128 = 128`) and of `PAGE_SIZE`, and at a 10 ms notification interval it is still 8 to 18 service calls deep, so there is ample margin against DPC jitter.

It has to stay a **constant**, not a figure computed from the negotiated format. PortCls reads `BufferSize()` once when the pin is created and caches it, and `SetFormat` tears the buffer down and rebuilds it later (PortCls re-negotiates the format after `NewStream` returns — visible all over the log as `48000` then `22050`/`44100`), so a size that varied with the format could hand PortCls a buffer smaller than the one it still believes it owns.

**Two other changes ride along, both prompted by the same log.**

`SetFormat` now logs the format it was *asked* for, before anything can reject it. The old trace only printed a format once it had already been accepted, which is why the fifteen consecutive `SetFormat: AllocateRenderDmaEngine failed, status=C000000D` sessions in this log name no format at all and there is no way to tell what the codec would not take.

`SetFormat` also no longer destroys a working stream when it fails. It tore down a perfectly good engine through `ReleaseEngine()` before finding out whether the replacement format was acceptable, so those fifteen sessions each left a pin with no engine and a zero-length buffer — visible as `StopEngine: no DMA buffer to scan (addr=0000000000000000 size=0)` repeated for the rest of the stream's life, and `~Stream: destroying (capture 0, engine 0000000000000000)`. It now saves the outgoing format, and on failure re-allocates the old engine and buffer and logs `SetFormat: rolled back to N Hz, ...`. Failing a format change is legal; destroying the format that was already working is not.

The Stage 5ay scan is sharpened rather than removed: it now also reports the **byte span** the audio occupies and the final **link position**. Those are what separate "PortCls filled behind the cursor and the stream ended before the lap came round" (span starts at ~0, contiguous, ends level with the link position) from "the engine was DMAing from somewhere else entirely". `AllocateBuffer` additionally logs the buffer's system and physical address, so a buffer re-allocated across a format change can be told apart from the one it replaced — the one remaining way the hardware could be reading memory nobody is writing.

**Files changed:** `dmachannel.h` (the constant and the reasoning behind it), `dmachannel.cpp` (the address tail on the `AllocateBuffer` line), `wavecyclicstream.cpp` 29234→33115 (the requested-format log, the `SetFormat` rollback, the sharpened scan). **Built clean:** `chk x64 WNET`, 9 files compiled, 1 warning (the standing benign "x64 Native compiling isn't supported"), 1 executable built, zero errors. `stwrtxp.sys` is **62976 bytes**, up from Stage 5ay's 60928. The INF is unchanged at 8375 bytes.

**A methodological note worth keeping.** Stage 5ay's own write-up carried a claim that the `FreeBuffer`/`ReleaseEngine`-between-`AllocateBuffer`s ordering was "provably not the silence cause", and this session very nearly re-opened it as the prime suspect on the strength of the log alone. Checking the source instead killed it twice over: `ReleaseEngine` *does* clear `m_bEngineHandleValid`, and the second `AllocateBuffer` runs *after* the replacement engine exists — it merely *prints* before `SetFormat`'s trailing `DOUT`. **A DbgPrint trace records print order, not call order, and a function that logs its result at the end will always appear to run after everything it called.** Two separate wrong conclusions came out of forgetting that. See n40, now withdrawn.

**Honest assessment of the odds.** Much better than 5ay's. The volume-node half of 5ay is confirmed working on hardware, not inferred. The silence diagnosis is arithmetic over five independent sessions that all agree, not a plausibility argument: every session wrote less than one lap, and the one variable that decides whether that matters is the number being changed. The residual risk is not that the diagnosis is wrong about *what* the buffer size does — that part is measured — but that something else is *also* broken behind it and 93 ms of newly-audible latency reveals a second fault. The sharpened scan and the buffer addresses are in the build so that, if so, the next log says which.

**Next action: install this build and retest — see n41.**

### Stage 5ba: the buffer-size fix did exactly what it was designed to do and the sound is still inaudible — because what arrives in the buffer is **40 to 50 dB too quiet**. A registry-gated substitute test tone settles whether that is the whole remaining story

**Stage 5az's mechanism is confirmed, and it was not sufficient.** The user installed the 62976-byte build, retested, and reported *"still can't hear anything"*. The log (3632 lines, one boot, two clean playback sessions) shows the buffer behaving exactly as Stage 5az predicted it would once it was small enough:

```
StopEngine: DMA buffer 16384 bytes / 8192 samples, 7970 non-zero, peak |sample| = 103, audio spans bytes 0..16380, link position 11488
StopEngine: DMA buffer 16384 bytes / 8192 samples, 3708 non-zero, peak |sample| =  23, audio spans bytes 4..16382, link position 15996
```

Compare that with every session in the Stage 5ay log, where the fill never once reached the end of the buffer. **The audio now spans the entire buffer, from byte 0 to byte 16380**, and the sessions are 423 and 482 `IOCTL_KS_WRITE_STREAM` long — at a 10 ms notification interval, roughly 4.2 s and 4.8 s of audio through a buffer whose lap is 186 ms at 22.05 kHz. **The cursor went round about 23 times.** Whatever is wrong, it is no longer that the sound ends before the buffer wraps. That question is closed.

**Two further things the new diagnostics settle, both of which close off suspects.**

*The position register is the genuine stream LPIB, not a free-running counter.* This was the live alternative to the Stage 5az reading — if `GetLinkPositionRegister` had handed back something like the wall clock, PortCls would have filled the buffer just as eagerly with the DMA engine doing nothing. It didn't: the `RUN -> PAUSE` scan and the `ACQUIRE -> STOP` scan a few state transitions later read **the same value, 11488, twice** (and 15996 twice in the second session). A wall clock would have advanced by millions of ticks between those two reads. A paused stream's LPIB holds. The DMA engine really is running, really is advancing through our buffer, and really does stop when told.

*The codec's whole output path is configured, and by this driver, at load time.* Auditing every non-query verb in the log:

```
nid=1  705 payload=0      AFG to D0
for each of nid = 10, 13, 15, 16, 17:
  nid   705 payload=0     pin to D0
  nid   701 payload=0     connection select 0
  nid=21 705 payload=0    DAC to D0
  nid=21 3   payload=B07F output amp, both channels, MUTE CLEAR, gain 127/127
  nid   3   payload=B07F  pin amp, both channels, MUTE CLEAR, gain 127/127
  nid   707 payload=C0/40 pin widget control: OUT enable (+ HP enable on nid 10)
  nid   70C payload=2     EAPD enable
```

That is the complete list of things that normally cause "everything looks right but nothing comes out" — power state, connection selection, amp mute, amp gain, pin output enable, external amplifier power — and every one of them is set correctly before the first stream is ever opened. `0xB07F` decodes as set-output-amp, left and right, index 0, mute bit clear, gain 0x7F = the maximum the codec reports. Note also that **there is not one `ProgramOutputAmp` line in this log**: the user never touched a slider this boot, so nothing has moved the amp away from that initial maximum. The hardware is wide open.

**So what is left is the one thing the driver has never controlled: how loud the data is.**

| session | non-zero / 8192 | peak | dBFS | audible? |
|---|---|---|---|---|
| Stage 5ba #1 | 7970 (97%) | 103 | **-50** | no |
| Stage 5ba #2 | 3708 (45%) | 23 | **-63** | no |
| kstest (every boot since 5aq) | — | ~8200 | **-12** | **yes** |

kstest's 440 Hz sine is generated at roughly -12 dBFS (`kstest.c`, `StreamToneToPin`), and it has been clearly audible on every boot since Stage 5aq. What Windows is putting in the buffer is **38 to 51 dB below that**. -63 dBFS is four bits of a sixteen-bit sample. Through desktop speakers that is not quiet, it is nothing.

That also re-reads the Stage 5ay numbers, which were never uniform: 22679, 8477, 751, 270. Amplitude has been varying wildly across sessions the whole time and nobody was looking at it, because the question then was whether the buffer contained *anything*.

**This is a real finding but it is not yet a diagnosis, and the difference matters.** "The data is too quiet" is measured. *Why* it is too quiet is not: it could be a mixer gain applied somewhere between wdmaud and kmixer, it could be that the user simply had the volume down, or it could be that the amplitude is a coincidence and something else below PortCls is broken. The temptation is to reason about which — that is exactly the move that produced the two withdrawn claims in Stage 5ay and 5az. So instead:

**Stage 5ba's diagnostic substitutes a known-loud signal at the last point the driver touches the data.** `CHdaDmaChannel::CopyTo` — the single funnel every byte PortCls writes into the cyclic buffer passes through — now discards its source and writes a 689 Hz sine at -6 dBFS instead, when and only when a registry value says so:

```
HKLM\SYSTEM\CurrentControlSet\Services\stwrtxp   "TestTone" = 1   (REG_DWORD)
```

read once in `DriverEntry` via `RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, ...)`, so it costs nothing and changes nothing when it is absent, and `package/testtone-on.reg` / `package/testtone-off.reg` toggle it. The phase is derived from the destination's **offset within the cyclic buffer**, not from a running counter, so it is independent of the order and size of PortCls's service calls, and the 64-sample table divides the 8192-sample buffer exactly (8192/64 = 128) so the tone is phase-continuous across the lap with no per-lap click to misread as a fault.

The outcome is binary and there is no third branch:

- **The tone is audible** — every layer from PortCls through the DMA engine, the converter, the amp, the pin and the speaker is proven end to end *on the ordinary Windows playback path*, and the entire remaining fault is the level of what the mixer delivers. That is a mixer problem, and n42 (the slider spanning only ~0.5 dB) stops being cosmetic and becomes the main line.
- **The tone is silent** — the amplitude reading is a red herring, the fault is below PortCls, and the buffer-address and link-position data in the same log say where to look next.

`StopEngine`'s scan also now reports **RMS** alongside peak, by integer square root of the mean square (16 iterations, no FPU — kernel code may not touch the FPU without saving state). Peak alone cannot tell a loud signal with one stray sample from one that is uniformly 50 dB down, and that is now precisely the distinction that matters. For reference: -12 dBFS is RMS ≈ 5800, -40 dBFS is RMS ≈ 328, -60 dBFS is RMS ≈ 33.

**The control this log is missing.** There is no `kstest.exe` run anywhere in it — all pin creates come from PID 2712 and PID 4. So there is no same-boot proof that the hardware path still works under the 16 KB buffer, and running kstest costs nothing and rules out a Stage 5az regression in one step. It is the first thing to ask for.

**Also confirmed working, in passing:** the Stage 5az `SetFormat` request log fires correctly (`SetFormat: requested 48000 Hz, 2 ch, 16 bit` then `requested 22050 Hz` in both sessions — PortCls opens at 48 kHz and immediately re-negotiates down), and **there is not a single `AllocateRenderDmaEngine failed` in this log**, so the fifteen dead sessions of the Stage 5ay log did not recur and the rollback path was never needed. The buffer addresses differ between the two allocations of a session, as expected (`phys B72E9000` then `98F6D000`), and the scan finds the audio in the *current* buffer, so nothing is writing to a buffer the engine has stopped using.

**Files changed:** `dmachannel.h` (the `g_HdaTestTone` declaration and the reasoning), `dmachannel.cpp` 8074→11076 (the flag, the sine table, the `CopyTo` substitution), `adapter.cpp` 30693→32475 (`ReadTestToneSetting` in `DriverEntry`), `wavecyclicstream.cpp` 33115→34142 (RMS). New: `package/testtone-on.reg`, `package/testtone-off.reg`. **Built clean:** `chk x64 WNET`, 9 files compiled, 1 warning (the standing benign "x64 Native compiling isn't supported"), 1 executable built, zero errors. `stwrtxp.sys` is **64512 bytes**. The INF is unchanged at 8375 bytes.

**Everything added this stage is a diagnostic and goes with item p** — the `TestTone` flag, the sine table, the `CopyTo` branch, `ReadTestToneSetting`, the RMS, and both `.reg` files. The Stage 5az buffer size stays.

**Next action: n43.**

### Stage 5bb: the test tone came back SILENT, which kills the amplitude theory outright — and hands over the real discriminator. **Every audible stream this project has ever produced was 44.1 kHz; every silent one was 22.05 kHz.** The driver advertised 8-48 kHz without ever asking the codec

The Stage 5ba experiment ran exactly as designed and returned the answer I did not expect. Three boots in one log:

| boot | `TestTone` | what played | buffer peak | buffer RMS | audible |
|---|---|---|---|---|---|
| 2 | 0 | system sound, 22050 Hz | 16444 | 4666 | **no** |
| 2 | 0 | **kstest, 44100 Hz** | 8191 | 5328 | **YES** |
| 2 | 0 | system sound, 22050 Hz | 107 | 24 | no |
| 3 | **1** | system sound, 22050 Hz | **16384** | **11585** | **no** |
| 3 | 1 | (five sessions, all 22050 Hz) | 16384 | 11193-11585 | no |

The tone flag was read correctly (`DriverEntry: TestTone=1 - EVERY render stream will carry a 689 Hz sine at -6 dBFS...`), the substitution worked (`peak |sample| = 16384` is the tone table's exact maximum, RMS 11585 is a full sine at that peak, spanning bytes 2..16382), and **nothing came out**. The user's sliders were at 100%.

**So the amplitude finding is dead, and it deserves to be said plainly: it was a real measurement and the wrong lead.** A buffer at RMS 11585 was silent; a buffer at RMS 5328 was audible. Loudness in the DMA buffer does not predict audibility here, and 5ba's whole framing — that the mixer's level was the remaining fault — is withdrawn. **This is the third time in this project that a plausible cause has survived only until the experiment that could refute it was actually run** (kmixer-delivers-silence in 5ay, the `ReleaseEngine` ordering bug in 5az, amplitude in 5ba). The pattern is worth naming: none of the three was disproved by more reading. Build the thing that can only come back one of two ways.

**What the same log hands over is a discriminator that has been sitting in plain sight since Stage 5aq.** Line up every playback session in the project's history by sample rate:

- **44.1 kHz** — kstest.exe, every boot since 5aq, always audible.
- **22.05 kHz** — every Windows system sound, every boot since 5ax, never once audible.

There is no counterexample in either direction. And Stage 5ba's diagnostics have independently eliminated everything else the two paths could have differed in: the DMA buffer contents are proven identical in kind (a full-scale tone in the silent case), the link position register is proven to be the real LPIB (it holds across a pause and advances across a run), and the codec's output path — DAC and all five pins at D0, connection select 0, amps unmuted at gain 127/127, pin OUT enable, EAPD on — is proven configured before the first stream opens and untouched afterwards (not one `ProgramOutputAmp` in the log). The verb streams are structurally identical:

```
system, silent                          kstest, audible
  SetFormat: requested 48000              SetFormat: requested 44100
  SetFormat: requested 22050
  verb (nid=21 verb=2 payload=4111)       verb (nid=21 verb=2 payload=4011)
  verb (nid=21 verb=706 payload=10)       verb (nid=21 verb=706 payload=10)
  BindRenderConverters: tag 1, 4111       BindRenderConverters: tag 1, 4011
  StartEngine: running, stream tag 1      StartEngine: running, stream tag 1
```

The **only** difference is the rate, and the `0x4111` vs `0x4011` that encodes it.

**Why 22.05 kHz would be silent, and why nothing upstream catches it.** `0x4111` is the HDA converter format for "44.1 kHz base, divide by 2". Whether a codec can actually *do* the divide-by-two is reported in `PARAMETER 0x0A` (`SUPPORTED_PCM_SIZE_RATES`), bit 3. **This driver has never once queried that parameter** — there is no `verb=F00 payload=A` anywhere in any log in this project — and `wavecyclicminiport.cpp` advertised a flat range with a TODO next to it admitting as much:

```c
// A conservative single PCM format range; the codec's actual supported
// rates/formats come from HDA_PARAM_SUPP_PCM_RATES / SUPP_STREAM_FORMATS
// (read but not yet enforced here - TODO ...)
    2, 16, 16, 8000, 48000
```

Windows XP's system sounds are 22.05 kHz files. kmixer read that range, saw 22050 was allowed, and negotiated the pin down to the source rate rather than resampling — which is the correct thing for it to do given what we told it.

Nothing below us can catch the error either, and this is the part worth remembering: **`AllocateRenderDmaEngine` returns success for 22050 Hz and fills in `ConverterFormat = 0x4111` itself.** The bus driver only checks that a rate is expressible in the stream descriptor's format register; it never sees the codec's capability bitmap. So a rate the DAC cannot produce passes every check the driver makes, gets programmed into the converter verbatim (`common.cpp` passes `ConverterFormat` through untouched, by design, since Stage 5ao), and produces a DMA engine that runs, a position register that advances, writes that complete, and no sound. That is precisely the Stage 5ao failure shape reappearing one layer up.

**Stage 5bb's change is the TODO, done.**

- `QueryPcmCaps()` in `common.cpp` reads `0x0A` and `0x0B` from the AFG **and** from the render DAC, and picks between them the way the spec says to: a widget's own answer governs only when `AUDIO_WIDGET_CAP` bit 4 (Format Override) is set. NID 21 reports `0x000D0C05`, so bit 4 is clear and the AFG is the authority here — but both are read and both are logged, decoded rate by rate, because guessing which one applies is exactly the sort of thing that costs a stage.
- `NarrowPcmRangeToCodec()` in `wavecyclicminiport.cpp` runs in `Init()`, before `GetDescription()` can hand the pin descriptors to PortCls, and cuts the advertised range to **44100..48000**. A contiguous range loses nothing: kmixer only ever picks from the standard rate set, and those two are its only members between the bounds. kmixer now resamples the 22.05 kHz system sounds on our behalf, which is its job and which it does transparently.

**The window is the 44.1/48 pair regardless of what `0x0A` says, and that is a deliberate choice worth being honest about.** If the codec's bitmap turns out to include 22.05 kHz, then this is a workaround and not a fix — the rate would be one the DAC claims it can produce and cannot, or one this driver mishandles somewhere else — and the `QueryPcmCaps` lines in the next log will say so in one glance. Either way the user gets audio, and the distinction is readable off the log rather than argued about. Advertising only 44.1 and 48 kHz is in any case what real HD Audio drivers do.

**Files changed:** `hdaverbs.h` 6799→7969 (the twelve rate bits and the `g_HdaPcmRateHz` table declaration), `shared.h` 21164→21443 (`GetSupportedPcmRates` on `IHdaAdapterCommon`), `common.h` 8665→9057, `common.cpp` 60236→64166 (`QueryPcmCaps`, the rate table, the `InitCodec` call), `wavecyclicminiport.cpp` 13863→17565 (`NarrowPcmRangeToCodec` and the `Init` call). **Built clean:** `chk x64 WNET`, 9 files compiled, 1 warning (the standing benign "x64 Native compiling isn't supported"), 1 executable built, zero errors. `stwrtxp.sys` is **67072 bytes**. The INF is unchanged at 8375 bytes.

**The Stage 5ba test tone stays in this build**, defaulted off. It has just proved its worth as a control and it costs nothing when the registry value is absent. It still goes with item p.

**Next action: n44.**

## Stage 5bc — the codec's answer came back, the clamp did not hold, and the miniport is the only gate there is

**Status: built clean, staged, NOT YET TESTED (that is n45).** `stwrtxp.sys` is **69120 bytes** (was 67072). The INF is unchanged at 8375 bytes.

### What the Stage 5bb test returned

Two things, and they point in opposite directions, so keep them apart.

**The diagnosis is confirmed by the hardware.** The capability read that this driver had never performed came back:

```
QueryPcmCaps: DAC NID 21 caps=000D0C05 (format override clear), own 0x0A=00000000
QueryPcmCaps: AFG NID 1 0x0A=000E05E0 0x0B=00000001 -> effective rate mask 000E05E0
QueryPcmCaps:    8000 Hz not supported      QueryPcmCaps:   44100 Hz SUPPORTED
QueryPcmCaps:   11025 Hz not supported      QueryPcmCaps:   48000 Hz SUPPORTED
QueryPcmCaps:   16000 Hz not supported      QueryPcmCaps:   88200 Hz SUPPORTED
QueryPcmCaps:   22050 Hz not supported      QueryPcmCaps:   96000 Hz SUPPORTED
QueryPcmCaps:   32000 Hz not supported      QueryPcmCaps:  176400 Hz not supported
                                            QueryPcmCaps:  192000 Hz SUPPORTED
                                            QueryPcmCaps:  384000 Hz not supported
NarrowPcmRangeToCodec: rate mask 000E05E0 -> advertising 44100..48000 Hz, 16..16 bit, 2 ch (was 8000..48000)
```

`0x000E05E0` has **bit 3 clear**. The codec genuinely cannot produce 22.05 kHz. The Stage 5bb caveat — *"if the codec claims 22.05 kHz outright, this is a workaround and not a fix"* — therefore resolves in favour of the diagnosis, and the DAC's own `0x0A` being zero with Format Override clear (`caps=000D0C05`, bit 4 clear) confirms the AFG is the authority, exactly as Stage 5bb assumed. Nothing here needs revisiting.

**And the clamp did not stop anything.** Every playback session in the test boot still reads:

```
SetFormat: requested 48000 Hz, 2 ch, 16 bit (capture 0)
SetFormat: 48000 Hz, 2 ch, 16 bit -> engine handle FFFFFADF4441C004
SetFormat: requested 22050 Hz, 2 ch, 16 bit (capture 0)
SetFormat: 22050 Hz, 2 ch, 16 bit -> engine handle FFFFFADF4441C004
BindRenderConverters: NID 21 bound to tag 1, format 4111
StartEngine: running, stream tag 1, converter format 4111
```

Six sessions, all silent, buffer peaks 5353 / 1231 / 1209 / 24 / 14647. No `AllocateRenderDmaEngine failed`, no `rolled back` — the driver accepted 22050 every single time.

### Why the clamp did not hold

Read the pin-create traffic in order and the mechanism is unambiguous:

```
MJ_CREATE #26: Name="\Wave"
KSPROP #318: set=Pin id=4 (DATAINTERSECTION) in=216 out=0  -> 80000005   (size probe)
KSPROP #319: set=Pin id=4 (DATAINTERSECTION) in=216 out=82 -> 00000000   (82 = KSDATAFORMAT + WAVEFORMATEX)
NewStream: entry, Pin=0, Capture=0
SetFormat: requested 48000 Hz          <-- the intersection result. The clamp WORKED here.
...
MJ_CREATE #27 returned status=00000000
SetFormat: requested 22050 Hz          <-- a KSPROPERTY_CONNECTION_DATAFORMAT set, arriving AFTER the pin exists
KSPROP #320: set=Connection(1D58C920) id=2 flags=2 in=24 out=82 -> 00000000
```

`Connection` property id 2 is `KSPROPERTY_CONNECTION_DATAFORMAT`, `flags=2` is `KSPROPERTY_TYPE_SET`, and the trace prints on IRP completion, so #320 *is* the 22050 request. So:

- The narrowed range reached PortCls and was honoured. `NarrowPcmRangeToCodec` logs at load, well before the pin traffic, and `Init()` runs before `GetDescription()`, so the descriptor PortCls copied was the narrow one — and the data intersection duly returned 48000 Hz. **Stage 5bb's mechanism works; it just does not cover this case.**
- **A dynamic format change is not checked against the pin's data ranges by anything.** PortCls takes the `KSDATAFORMAT` off the property set and hands it to `IMiniportWaveCyclicStream::SetFormat`. The WDK's own `msvad` confirms the contract from the other side: it validates in `NewStream` only, and the comment in `basewave.cpp`'s stream `SetFormat` says outright *"MSVAD does not validate the format"* — which is harmless for a sample driver with no hardware behind it and is not harmless for one with a real DAC.
- **`AllocateRenderDmaEngine` does not check either**, as established in Stage 5bb: it only asks whether the rate is expressible in the stream descriptor's format register, computes `ConverterFormat = 0x4111` itself and returns success. It never sees the codec's capability bitmap.

Which leaves exactly one place in the whole path where the codec's real answer can stop a rate it cannot play: **the miniport's own `SetFormat`.** The data range describes the pin. It does not enforce anything.

### The change

**`wavecyclicminiport.h` / `.cpp` — `CMiniportWaveCyclicHda::ValidateFormat(PKSDATAFORMAT)`.** New public method, the `msvad` `ValidateFormat`/`ValidatePcm` pattern with the one thing `msvad` has no reason to do — a lookup in the codec's rate bitmap. In order it rejects: a `FormatSize` too small to hold a `KSDATAFORMAT_WAVEFORMATEX`; channels or bit depth outside the advertised `KSDATARANGE_AUDIO`; a rate outside the advertised `Minimum`/`MaximumSampleFrequency`; and a rate that is inside that span but absent from `GetSupportedPcmRates()`. That last check is not redundant — the advertised span is contiguous and the codec's rate list is not, so the endpoints alone would let a hypothetical 45000 Hz through.

**`wavecyclicstream.cpp` — the guard runs first.** `SetFormat` now bounds-checks `FormatSize` before making the `PKSDATAFORMAT_WAVEFORMATEX` cast (everything below it, including the trace, reads a `WAVEFORMATEX` out of that buffer), logs the requested format, then calls `ValidateFormat` and returns on failure. The ordering is the point: the guard sits **above** `hadBuffer`, above the saved-format bookkeeping and above `ReleaseEngine()`, so a refused format change costs the caller its `SetFormat` and nothing else. A stream that was already running keeps its engine, its buffer and its format, and the Stage 5az rollback path is never even reached. The Stage 5az trace moved up with it, so a rejected format is still named in the log — that was the whole point of Stage 5az's reordering and it survives intact.

**`wavecyclicminiport.cpp` — `GetDescription` now logs the range it publishes.** One line, at the exact moment PortCls copies the data ranges into the subdevice descriptor. Stage 5bb could only answer "did the narrowing get there first?" by inference from timestamps; now it is stated.

### What this predicts, and what it would mean if it is wrong

kmixer asks for the source rate as an optimisation — if the sink can take 22.05 kHz natively there is no need to resample. When the sink refuses, kmixer keeps the format the pin was created with and resamples into it. That is the designed behaviour of the sysaudio/kmixer format-change path, and it is why refusing is a fix rather than a refusal to play.

So the log should now show `SetFormat: refused 22050 Hz ...` followed by the stream continuing at 48000 Hz, and the buffer should contain resampled audio at the rate the DAC can actually consume.

If instead the *whole session* dies at the refusal — no `StartEngine`, an error back to the application — then kmixer is not resampling on our behalf and the driver has to do the work itself, which is a much bigger change (accept 22050 at the pin, resample in `CopyTo`, or advertise a rate the codec supports while lying about it upstream). Worth knowing before designing anything: the current state is already silent, so a refusal cannot make the audible situation worse, only the error reporting louder.

## Stage 5bc RESULT — **it works. Windows audio plays through this driver.**

**TESTED on the real HP Pavilion p6-2133w, and the user's words were "THERE'S AUDIO!!!".** This is the end of the silence. Everything below is the log confirming it, and the one application that is still quiet.

### The guard did exactly what it was built to do

```
GetDescription: publishing PCM range 44100..48000 Hz, 16..16 bit, max 2 ch
ValidateFormat: rejecting 22050 Hz - outside the advertised 44100..48000 Hz range
SetFormat: refused 22050 Hz, 2 ch, 16 bit - the stream keeps the format it already had, status=C000000D
StartEngine: running, stream tag 1, converter format 0011, notification interval 10 ms
StopEngine: DMA buffer 16384 bytes / 8192 samples, 8192 non-zero, peak |sample| = 18925, RMS = 8086, ...
```

Fifteen render sessions in the test boot. **Fourteen of them asked for 22050 Hz, were refused, kept the 48000 Hz format the pin was created with, and played.** Peaks across those fourteen: 14746, 2945, 2945, 2, 2, 3581, 7419, 2531, 12845, 18925, 18733, 18925, 1743, 86. Converter format `0011` throughout — 48 kHz base, no divisor, 16-bit, 2 channels. The `C000000D` count in the whole boot is exactly 14, so every refusal is accounted for and nothing else in the driver failed. `GetDescription` confirms the narrowed range reaches PortCls, closing the last ordering question from Stage 5bb.

**The open question at the end of Stage 5bc is answered: kmixer does resample on our behalf.** When the sink refuses the source rate, it keeps the pin's format and converts into it, exactly as predicted. The driver never has to resample for itself, and the three fallback designs sketched at the end of the Stage 5bc section are not needed. Delete that worry.

### The chain of five wrong-then-right diagnoses that got here

Worth preserving, because four of the five were wrong and each one was only killed by an experiment designed to have two possible answers:

1. **5ay — "kmixer is delivering silence."** Killed by the `StopEngine` DMA scan: the buffer was full of loud audio.
2. **5az — "the buffer is too big and the stream never completes a lap."** Real defect, correctly fixed (`0x16000` -> `0x4000`), not the cause.
3. **5ba — "the audio is 40-50 dB too quiet."** Killed by the substitute test tone: a full-scale sine (peak 16384, RMS 11585) was *silent* while kstest at RMS 5328 was audible on the same build. **Amplitude does not predict audibility.**
4. **5bb — "the rate is wrong."** Correct, and confirmed by the hardware (`0x0A = 0x000E05E0`, bit 3 clear). The fix — narrowing the advertised `KSDATARANGE_AUDIO` — was necessary and insufficient.
5. **5bc — "the data range describes the pin and enforces nothing."** Correct and sufficient. A `KSPROPERTY_CONNECTION_DATAFORMAT` set is checked by nobody: not PortCls, not the bus driver. The miniport's `SetFormat` is the only gate in the path.

### What is still quiet: VLC

The user reports Windows sounds working and **VLC Media Player producing nothing.** The log's last session is almost certainly it, and it is informative:

```
MJ_CREATE #430: PID=1752 ... Name="\Wave"
NewStream: entry, Pin=0, Capture=0
SetFormat: requested 48000 Hz -> engine handle FFFFFADF4441C004
AllocateBuffer: ... sys FFFFFADF1A44D000 phys 8A7D0000
SetFormat: requested 44100 Hz, 2 ch, 16 bit          <-- ACCEPTED, 44.1 is supported
FreeBuffer / ReleaseEngine / AllocateBuffer ... sys FFFFFADF1D0A0000 phys 8A7D0000
BindRenderConverters: NID 21 bound to tag 1, format 4011
StartEngine: running, stream tag 1, converter format 4011
SetState: RUN -> PAUSE
StopEngine: ... 4081 non-zero, peak |sample| = 344, RMS = 25, audio spans bytes 6..16380, link position 2356
```

Read that carefully, because it rules out almost everything:

- **VLC reached the driver.** It opened `\Wave`, created a render pin, and got a stream. This is not a device-visibility problem.
- **It got the rate it asked for.** 44100 is in the codec's bitmap, so `ValidateFormat` passed it, and the converter was bound at `4011` — the exact format kstest.exe has always been audible at. This is not a rate problem.
- **The engine ran.** `StartEngine` succeeded and the link position advanced to 2356.
- **But the session lasted one buffer.** `StartEngine` and `RUN -> PAUSE` are five log lines apart with no servicing in between, and the audio in the buffer peaked at **344 of 32767 (about -39 dBFS)** with an RMS of 25 (about -62 dBFS).

So VLC opened the device successfully, put a fraction of a second of very quiet content through it, and stopped. That is the shape of an application-side failure — its output module erroring out after the open, or its own volume down — not of a driver that cannot play. It is worth confirming with the one-minute experiment (play the same file in Windows Media Player) before touching any driver code, and worth reading VLC's own Tools -> Messages at verbosity 2, which will name the failure directly.

**One caveat on the identification:** PID 1752 is inferred to be VLC from position (last session in the log, the only one at 44100, the only one from that PID) rather than from anything the driver can see. If WMP also fails, that inference is wrong and the session belongs to something else.

### Two small things the log shows in passing

- **`KSPROPERTY_AUDIO_PEAKMETER` (Audio property id 33) returns `C0000225` on about half the calls.** Four separate processes (PIDs 736, 1552, 2064, 2140) poll it continuously alongside `VOLUMELEVEL` — they are volume-meter UIs. Harmless, and the meters simply do not move. Worth adding when the topology is next touched; not worth a stage of its own.
- **`KSPROPERTY_AUDIO_CHANNEL_CONFIG` (id 3) SET returns `C0000225`** on every attempt, 376 times across those same processes. Also harmless for a stereo-only filter, also cheap to add.

## Stage 5bd — VLC answered: it was never this driver (WASAPI on XP), and Windows Media Player becomes the lead

The user tested Stage 5bc's working build against VLC and Windows Media Player and supplied VLC's own debug log at verbosity 2. It resolves n46 outright.

**VLC's fault is in VLC.** The whole log is one block repeated once per buffer for the entire file:

```
main debug: reusing audio output
main debug: looking for aout stream module matching "any": 2 candidates
wasapi error: cannot initialize audio client (error 0x80070057)
main debug: no aout stream modules matched
main error: module not functional
main debug: keeping audio output
main error: failed to create audio output
```

ending in `main error: buffer deadlock prevented`. Three findings, all of them from VLC's own words:

1. **The active output module is `mmdevice`, whose stream submodule is `wasapi`** — the Vista+ WASAPI client API. `0x80070057` is `E_INVALIDARG` returned by `IAudioClient::Initialize`. Windows XP has no WASAPI at all, so this cannot succeed here regardless of what the driver does. The trailing `mmdevice debug: simple volume changed: 1.000000, muting disabled` lines confirm `mmdevice` is the live aout rather than a module VLC merely probed.

2. **VLC never attempted DirectSound or WaveOut for playback.** The only `directsound` line anywhere in the log is `directsound debug: found 2 devices`, emitted immediately after `qt debug: Saving the simple preferences` — that is the preferences dialog enumerating devices to fill its dropdown, not a playback attempt. Worth noting the polarity of it: DirectSound **found** the devices. That is independent confirmation, from outside this project's own instrumentation, that the driver is visible to the DirectSound enumerator.

3. **Switching the output module in Preferences did not take effect**, which is why the user's WaveOut attempt produced an identical log. Every cycle logs `main debug: reusing audio output` and `main debug: keeping audio output`: VLC caches the aout instance and only re-probes the *aout stream* submodule beneath it. The cached aout stays `mmdevice` until VLC itself restarts. The fix is to restart VLC after changing the setting, or to force it per-run on the command line:

```
"C:\Program Files (x86)\VideoLAN\VLC\vlc.exe" --aout=directsound
```

(`--aout=waveout` is equally valid; `Tools -> Preferences -> Reset Preferences` clears a stuck cached choice.)

**Corollary, and it retracts a Stage 5bc inference: the PID-1752 session in the Stage 5bc log was NOT VLC.** No `IRP_MJ_CREATE` from VLC could have reached the driver on that run, because VLC never got past `IAudioClient::Initialize` in user mode. The PID-1752 identification had been inferred purely from position (last session in the log, the only one at 44100 Hz, the only one from that PID) and is now falsified. Whatever PID 1752 was — plausibly the preferences dialog's own enumeration, or a system probe — it opened the pin, had 44100 Hz accepted, bound the converter at `4011` and ran the engine for one buffer at peak 344. **So the "44100 succeeds, which frees and reallocates the DMA buffer mid-stream" path still has no known defect and, more importantly, no known exerciser.** The speculation at the end of the old n46 that this path might be buggy is unsupported by anything and must not be acted on.

**Windows Media Player is the real open question.** It refuses to play with *"there is a problem with your sound device"*. Unlike VLC's failure, that is a claim about the device, and it is the first symptom since Stage 5bc that could still be a driver gap. It is also not yet diagnosable, because no driver log was captured across a WMP attempt — see n47 for exactly what to collect and why the first item is binary.

## Stage 5be — the WMP log read in full, and `KSPROPERTY_AUDIO_CHANNEL_CONFIG` on the wave DAC node

The user captured a driver log across a Windows Media Player attempt. Thirty-one lines, one process (PID 2408), and it answers n47's binary question immediately: **WMP does reach this driver.**

### Reading the log

First, the thing that makes this log unusually easy to read. The Stage 5av dispatch hook throttles itself — `adapter.cpp`'s `DC_LOG_FULL_LIMIT`, then 6000 — and past that limit it logs **only failures**:

```c
if (seq <= DC_LOG_FULL_LIMIT || !NT_SUCCESS(status))
```

The sequence numbers here run from #11078 to #11105, far past 6000. **So every numbered `KSPROP`/`KSIOCTL` line in this capture is a failure, and the list of them is complete.** The seven un-numbered `PropertyHandler_*` traces interleaved between them are suppressed *successes*, printed by the handlers themselves rather than by the hook.

What failed, in order:

| Line | Request | Result |
|---|---|---|
| #11078, #11084, #11090 | `Topology` id 3 = `KSPROPERTY_TOPOLOGY_NAME`, GET, `out=0` | `80000005` — `STATUS_BUFFER_OVERFLOW`, i.e. a **size probe**, not a fault |
| #11092–11094 | `code=002F0007` = **`IOCTL_KS_ENABLE_EVENT`**, `in=32` (`KSE_NODE`) | `C0000230` `STATUS_PROPSET_NOT_FOUND` — no event sets published |
| #670 (`MJ_CREATE`) | `Name="\Wave"` | **`00000000` — WMP opens the wave filter successfully** |
| #11095, #11096, #11104 | `Audio` id 33 = `KSPROPERTY_AUDIO_PEAKMETER`, GET | `C0000225` `STATUS_NOT_FOUND` |
| **#11105** | **`Audio` id 3 = `KSPROPERTY_AUDIO_CHANNEL_CONFIG`, `flags=10000002` (SET\|TOPOLOGY), node 8, `in=32 out=4`** | **`C0000225`** |

**And then the log stops.** No `NewStream`, no `SetFormat`, no `KSSTATE` transition. WMP opens the wave filter, asks it for a peak meter, asks it to set a channel configuration, is refused, and gives up without ever creating a pin. That refusal is the last thing this driver ever says to it.

### Why the refusal happens

Because until now **the wave filter had no node property surface at all**. Both of its nodes carried a NULL automation table:

```c
static PCNODE_DESCRIPTOR MiniportWaveNodes[] =
{
    { 0, NULL, &KSNODETYPE_DAC, NULL },   // NODE_WAVE_DAC
    { 0, NULL, &KSNODETYPE_ADC, NULL }    // NODE_WAVE_ADC
};
```

so *every* node property aimed at this filter returns `STATUS_NOT_FOUND`, whatever it is and whatever node it names.

### What the reference samples say

Checked against the WDK 7600 samples, which settles which of these three failures is worth fixing:

- **`CHANNEL_CONFIG` is a genuine gap.** `msvad\pcmex` is the one sample in the WDK that implements it, and it puts it on **the wave miniport's DAC node** — `PropertiesDAC` → `DEFINE_PCAUTOMATION_TABLE_PROP(AutomationDAC, PropertiesDAC)` → `{ 0, &AutomationDAC, &KSNODETYPE_DAC, NULL }`, `GET | SET`, backed by a cached `KSAUDIO_CHANNEL_CONFIG m_ChannelConfig`. That is exactly the filter and exactly the node WMP is aiming at.
- **`PEAKMETER` and audio control-change events are not gaps.** `grep` across `msvad` and `ac97` finds `KSPROPERTY_AUDIO_PEAKMETER` and `KSEVENTSETID_AudioControlChange` in **no sample source** (binary-only matches in `.obj`/`.sys`). Our `C0000225`/`C0000230` on those match reference behaviour. They also occur on the Windows-sounds path, which plays fine. Not the blocker; left alone.

### The honest gap in this diagnosis: node 8

**The node id in the failing request is 8, and this filter has nodes 0 and 1.** The wave filter has 2 nodes and the topology filter has 7 (0..6); 2 + 7 = 9, giving sysaudio virtual node ids 0..8. So node 8 looks like an **untranslated sysaudio virtual node id delivered to a physical filter**, which should not happen.

Why it is untranslated is **not proven**. The working theory — stated as a theory — is that because no filter in this driver implements `CHANNEL_CONFIG` anywhere, sysaudio has nothing to map the virtual node onto and forwards the raw id; publishing the property gives it the mapping. **If that theory is wrong, this change closes a real gap against the reference sample but is not what fixes WMP.** So the same build widens the instrumentation rather than betting on one answer.

### The change

`wavecyclicminiport.h` / `.cpp`:

- `KSAUDIO_CHANNEL_CONFIG m_ChannelConfig` member, defaulted to `KSAUDIO_SPEAKER_STEREO` in `Init()`, reachable from the handler via a `ChannelConfig()` accessor (the same pattern as `CMiniportTopologyHda::AdapterCommon()`, since a `PCPFNPROPERTY_HANDLER` is a plain function that recovers the object from `PropertyRequest->MajorTarget`).
- `PropertiesDAC[]` / `AutomationDAC`, `GET | SET`, attached to `NODE_WAVE_DAC`. `NODE_WAVE_ADC` stays NULL — there is no capture control surface here.
- `PropertyHandler_ChannelConfig`, with msvad's `ValidatePropertyParams` (`kshelper.cpp:156`) inlined: `ValueSize == 0` is a size probe answered with `sizeof(KSAUDIO_CHANNEL_CONFIG)` and `STATUS_BUFFER_OVERFLOW`; short buffers get `STATUS_BUFFER_TOO_SMALL` with `ValueSize` cleared. GET returns the cached mask. SET accepts `KSAUDIO_SPEAKER_MONO` and `KSAUDIO_SPEAKER_STEREO` and refuses everything else with `STATUS_NOT_SUPPORTED`, gated on `PinDataRangePcm.MaximumChannels` — the same gating `pcmex` applies against its `m_MaxChannelsPcm`. **Refusing rather than silently accepting is deliberate, and it is the Stage 5bc lesson restated: this miniport is the only thing in the stack that checks.**

`adapter.cpp`: **`DC_LOG_FULL_LIMIT` 6000 → 40000**, so the retest log shows the successful traffic around the failures instead of failures alone.

Built clean, `chk` x64: `stwrtxp.sys` **69120 → 70656 bytes**. Staged to `Backported Driver/package/` with the matching `.pdb`. Awaiting reinstall and retest.

## Stage 5bf — node 8 proved, an `id=33` misreading corrected, and the rest of the DirectSound speaker-config pair

The Stage 5be build was installed and retested. WMP still refuses, but the log is 1490 lines instead of 31 and it settles three things.

### 1. The `CHANNEL_CONFIG` fix worked, and node 8 is no longer a mystery

```
PropertyHandler_ChannelConfig: node=0 verb=10000002 mask=00000003 -> 00000000
KSPROP #715: PID=2192 FO=... set=Audio(45FFAAA0) id=3 flags=10000002 extra=00000008 in=32 out=4 -> 00000000
```

The property arrives addressed to node **8**, our handler runs on node **0**, and it succeeds. **Node 8 is a sysaudio VIRTUAL node id, and virtual 8 is this wave filter's DAC node.** With the trace widened to log successes, every hook line now sits next to the handler that ran, and the pairs give the whole mapping:

| virtual | physical | | virtual | physical |
|---|---|---|---|---|
| 0 | topology node 6 | | 4 | topology node 2 |
| 1 | topology node 5 | | 5 | topology node 1 |
| 2 | topology node 4 | | 6 | topology node 0 |
| 3 | topology node 3 | | **8** | **wave node 0 (DAC)** |

sysaudio enumerates the composite graph in reverse walk order: the topology filter's seven nodes mirrored onto 6..0, then the wave filter's two onto 8 (DAC) and 7 (ADC). Nothing was untranslated and nothing was leaking — the Stage 5be theory about a raw id escaping was **wrong in its mechanism and right in its conclusion**, and the 5be refusal was the plain fact that the node published no properties at all. The comment in `wavecyclicminiport.cpp` has been rewritten to state the mapping rather than the theory.

Independent corroboration that the numbering is a mirror, not a coincidence: `KSPROP #706` asks `CPU_RESOURCES` of virtual node **4** and is refused `C0000225`, and virtual 4 maps to topology node **2** — `NODE_TOPO_LINEOUT_MIX`, the Stage 5au summer, which is one of the only two topology nodes with a NULL automation table. A refusal there is exactly what the mapping predicts.

### 2. Correction: `Audio id=33` is `CPU_RESOURCES`, not `PEAKMETER`

Stage 5bd and 5be both read `id=33` as `KSPROPERTY_AUDIO_PEAKMETER` and concluded, on the strength of `grep` finding no `PEAKMETER` implementation in `msvad` or `ac97`, that those refusals matched reference behaviour and could be ignored. **That was wrong.** Counting `ksmedia.h`'s `KSPROPERTY_AUDIO` enum (`LATENCY = 1`):

* **33 = `KSPROPERTY_AUDIO_CPU_RESOURCES`**
* **34 = `KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY`**
* 37 = `KSPROPERTY_AUDIO_PEAKMETER`

The new log confirms the decode from the driver's own side rather than by counting: `KSPROP #707` (`id=33`, virtual node 6) is immediately preceded by `PropertyHandler_CpuResources: node=0 ... -> 00000000`, and node 0 is a volume node whose automation table carries `CPU_RESOURCES` and nothing else that could match. **`id=37` does not appear anywhere in the log** — nothing has ever asked this driver for a peak meter, and the two "PEAKMETER is missing" notes in Stage 5bd/5be n47(4) are void.

So the Stage 5be decision to leave `id=33` alone was made for a reason that does not hold. `CPU_RESOURCES` is implemented in this driver already, on every topology control node since Stage 5ay; the wave DAC node simply never had it.

### 3. What WMP now fails on

Every failure in the post-boot part of the log, deduplicated — and because the limit is now 40000 against a peak sequence of ~735, this really is everything, successes included:

| Request | Count | Result | Reading |
|---|---|---|---|
| `IOCTL_KS_ENABLE_EVENT` | 12 | `C0000230` | no event sets published; no WDK sample publishes any either |
| `Pin` id 12 (`CONSTRAINEDDATARANGES`) | 6 | `C0000034` | absent by design, asked of all four pins |
| `Pin` id 10 | 4 | `C0000225` | pin enumeration probe, occurs on the working path |
| `Connection` id 2 (`DATAFORMAT`) SET | 1 | `C000000D` | **Stage 5bc doing its job** — a 22050 Hz set refused |
| **`Audio` id 33 `CPU_RESOURCES` GET, virtual node 8** | **2** | **`C0000225`** | **gap, wave DAC** |
| `Audio` id 33 `CPU_RESOURCES` GET, virtual node 4 | 1 | `C0000225` | topology summer, no automation table (matches `msvad`) |
| **`Audio` id 34 `STEREO_SPEAKER_GEOMETRY` SET, virtual node 8** | **1** | **`C0000225`** | **gap, wave DAC — TERMINAL** |

The tail of WMP's session (PID 2192) reads, in order: `CPU_RESOURCES` on node 8 refused → `CHANNEL_CONFIG` SET on node 8 **succeeds** → `STEREO_SPEAKER_GEOMETRY` SET on node 8 refused → nothing, ever again. It advanced exactly one property and stopped on the next.

**`CHANNEL_CONFIG` and `STEREO_SPEAKER_GEOMETRY` are the two halves of `IDirectSound::SetSpeakerConfig`.** dsound decomposes one call into a channel mask and a speaker angle, set back to back on the same node — which is precisely the pair in the log, in that order. Half a `SetSpeakerConfig` succeeding is still a failed `SetSpeakerConfig`, which is a DirectSound error, which is *"there is a problem with your sound device"*. No WDK sample implements the geometry half (`grep` across all of `src\audio` finds it nowhere), so there is no reference to copy here — only `ksmedia.h`'s contract.

### 4. Also in this log: the streaming path is provably healthy on this very boot

A second process (PID 1728) opened `\Wave`, ran a pin data intersection, created a stream, had 48000 Hz accepted and then 44100 Hz accepted, bound the converter at `4011`, and played ~70 ms of real content — `5356 non-zero, peak |sample| = 755, RMS = 100`. **Whatever WMP's problem is, it is not the pin, the format negotiation, the DMA engine or the converter.** (This process cannot be identified from the driver's side; the driver sees PIDs, not images. It is not WMP: WMP's session is the one that ends at the geometry refusal without ever creating a pin, matching the Stage 5bd capture's signature exactly.)

One thing worth recording so it is not misread later: the two boot-time streams in the same log report `peak 6` and `peak 2`, which looks alarming next to 755. It is not. The `StopEngine` DMA scan reads the ring at stop time, and those two streams ran 320 and 481 write IOCTLs (~3.2 s and ~4.8 s) before stopping, so what the scan caught was the trailing silence. The 44100 Hz stream ran 7 write IOCTLs — one buffer — so its scan caught the whole sound. **Short streams are the only ones this scan can measure meaningfully.**

### The change

`wavecyclicminiport.cpp` / `.h`, all on `NODE_WAVE_DAC`'s existing `AutomationDAC` table:

- **`KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY`, GET | SET.** A cached `LONG m_SpeakerGeometry`, defaulted in `Init()` to `KSAUDIO_STEREO_SPEAKER_GEOMETRY_WIDE` (20), which is what dsound uses for a plain stereo device. SET accepts `_HEADPHONE` (-1) or `_MIN..._MAX` (5..180) and refuses anything else with `STATUS_INVALID_PARAMETER`, logging the value. There is no 3D panning in this backport for an angle to steer, so this is state only — and unlike a sample rate, a geometry the driver ignores cannot make anything inaudible, which is why accepting it is safe where Stage 5bc's format lie was not.
- **`KSPROPERTY_AUDIO_CPU_RESOURCES`, GET | BASICSUPPORT**, answering `KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU` — same question, same answer and same reason as `mintopo.cpp`'s handler on the topology nodes. The two cannot share code: each is file-static in the translation unit whose automation tables name it.
- `ValidatePropertyParams`'s size handling factored out of the 5be `ChannelConfig` handler into a shared `ValidateValueSize`, now that three handlers need it.

The topology summers (nodes 2 and 6) are deliberately **left** with NULL automation tables. `msvad\simple`'s summers have none either, the mixer built from this filter works, and adding a property surface to a node wdmaud currently ignores risks the working mixer for a non-terminal refusal.

Built clean, `chk` x64: `stwrtxp.sys` **70656 → 71680 bytes**. Staged to `Backported Driver/package/` with the matching `.pdb`.

## Stage 5bg — the property chain closes, WMP still stops, and the log stops guessing at process identity

Stage 5bf was installed and retested. **Every property it added works.** The three new traces, in WMP's own session:

```
PropertyHandler_CpuResourcesDac: node=0 verb=10000001 -> 00000000
PropertyHandler_ChannelConfig:   node=0 verb=10000002 mask=00000003 -> 00000000
PropertyHandler_SpeakerGeometry: node=0 verb=10000002 value=20 -> 00000000
```

`CPU_RESOURCES`, the full `IDirectSound::SetSpeakerConfig` pair, all on the wave DAC node, all succeeding. **There is no longer any failing request anywhere on the wave filter.** And WMP still refuses to play.

### What is left in that session, and why none of it is the cause

The session runs `#684`–`#716` (`PID=3060`; process identity is the open question, see below). Complete failure list for it:

| # | Request | Result |
|---|---|---|
| 702, 703, 704 | `IOCTL_KS_ENABLE_EVENT` | `C0000230` PROPSET_NOT_FOUND |
| 706 | `Audio` id 33 `CPU_RESOURCES` GET, virtual node 4 | `C0000225` NOT_FOUND |

**Both happen before the successful speaker-config pair**, and the session carried on past both to `#716`. Then it stops — **after a success, not after a failure.** For the first time in this investigation the terminal event is invisible to the driver: whatever kills it happens above us, in sysaudio / kmixer / dsound, or in the client itself.

### Two corrections, one of them the same mistake twice

**(a) `KSEVENTSETID_AudioControlChange` IS implemented in a WDK sample.** Stage 5bd and 5be both dismissed the twelve `ENABLE_EVENT` refusals on the grounds that no sample publishes any event set. `sb16` does:

* `sb16\tables.h:383` — `PCEVENT_ITEM NodeEvent[]` with `KSEVENTSETID_AudioControlChange` / `KSEVENT_CONTROL_CHANGE`, `KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT`
* `sb16\tables.h:402` — `DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationVolumeWithEvent, PropertiesVolume, NodeEvent)`
* `sb16\tables.h:529` — that table wired onto a real node
* `sb16\mintopo.cpp:1444` — `CMiniportTopologySB16::EventHandler`, and `:1505` `ServiceEvent` firing it from the ISR

The earlier search had been narrowed to `msvad` and `ac97`, which genuinely do not implement it. **This is the second time in three stages that a "no sample does this" conclusion came from a search that was too narrow** — the first was reading `id=33` as `PEAKMETER` (Stage 5bf). The pattern to avoid: concluding *absence* from a grep whose scope was not stated.

**(b) The summer's `NULL` automation table is confirmed correct — this time against real hardware.** `ac97\driver\mintopo.cpp:1178` (`NODE_MAIN_MIX`) and `:1233` (`NODE_BEEP_MIX`) both pass `NULL` for the automation table of a `KSNODETYPE_SUM` node. ac97 is a shipping driver for real hardware that worked on XP, so a `CPU_RESOURCES` refusal on a summer is reference behaviour, not a gap. `NODE_TOPO_LINEOUT_MIX` and `NODE_TOPO_WAVEIN_MIX` stay as they are, and `#706` is not a defect.

### The open question, stated rather than guessed

Two processes matter in this log and **the driver cannot tell which is Windows Media Player.** A PID is all it records.

| | PID 3060 (`#684`–`#716`) | PID 2464 (`#661`–`#735`) |
|---|---|---|
| mixer enumeration | yes, full walk | yes, full walk |
| opens | `\Wave`, `0012019F`, share 0 | `\wave` `00120089` share 3, then `\Wave` `0012019F` |
| pin data-intersection | **never** | yes, 216 in / 82 out |
| creates a pin | **never** | yes |
| speaker config | **the whole pair, succeeding** | never asks |
| streams | no | 48000 then 44100, converter `4011` |
| audio produced | none | `5323 non-zero, peak 755, RMS 100` (~93 ms, one buffer) |

Both readings are internally consistent and they select **different next fixes**:

* **If 3060 is WMP**, then WMP got everything it asked for and died on something invisible — the next move is upstream instrumentation or a different client, not another property.
* **If 2464 is WMP**, then WMP successfully created a pin, played one buffer of real audio and tore down cleanly *while reporting a device problem* — a completely different failure, about what happens after the first buffer, and the 93 ms stop becomes the symptom to chase.

Note also that PID 2464's playback is near-identical to the previous log's PID 1728 (`5356` vs `5323` non-zero, peak `755`, RMS `100` in both), so whatever it is, it is the same short sound twice across two boots.

**Guessing here picks the wrong half of the work.** So Stage 5bg measures it.

### The change

**Diagnostic (both go with item p):**

- **`adapter.cpp`: the process image name on every `MJ_CREATE` line.** `PsGetProcessImageFileName` returns EPROCESS's 15-character `ImageFileName`; it is undeclared in the WDK headers and absent from this target's `ntoskrnl.lib`, so it is resolved by name with `MmGetSystemRoutineAddress` at PASSIVE_LEVEL from `DriverEntry` and everything degrades to `"?"` if the lookup fails. Both `MJ_CREATE` lines now read `PID=3060(wmplayer.exe)`. Only `MJ_CREATE` is annotated — every process of interest opens `\Wave`, and putting it on all ~735 property lines would only pad the log.
- **`adapter.cpp`: `IOCTL_KS_ENABLE_EVENT` / `_DISABLE_EVENT` decoded.** These have been logging as bare `KSIOCTL code=002F0007 -> C0000230` with no indication of set, id or node. `KSEVENT` is the same `KSIDENTIFIER` shape as `KSPROPERTY` (GUID + id + flags), and `KSE_NODE` appends its node id exactly where `KSP_NODE` does — both 32 bytes, which is what these requests report — so `ReadKsPropertySafe` decodes them verbatim and only the label changes. New `KsEventSetName` covers `AudioControlChange`, `PinCapsChange`, `StreamAllocator`, `Clock` and `Connection`.

**Functional:**

- **`mintopo.cpp`/`.h`: `KSEVENTSETID_AudioControlChange` / `KSEVENT_CONTROL_CHANGE` published on all five volume and mute nodes**, structured after `sb16`. `PPORTEVENTS m_pPortEvents` acquired via `Port->QueryInterface(IID_IPortEvents)` in `Init` (non-fatal on failure, released in the destructor — `stdunk.h`'s placement `operator new` zeroes the allocation, so it starts `NULL` without an explicit initialiser), `NodeControlChangeEvent[]` added to both automation tables via `DEFINE_PCAUTOMATION_TABLE_PROP_EVENT`, and `EventHandler_ControlChange` handling `PCEVENT_VERB_SUPPORT` / `_ADD` / `_REMOVE`. Unlike sb16 it does **not** validate the node: PortCls only routes an event request to a node whose automation table names the item, so by the time the handler runs the node is already one of the five.

  **This driver never fires one of these events.** The codec's unsolicited-response path is not wired up, so nothing here can notice a control moving except the SET that moved it. Publishing the set makes `ENABLE` succeed rather than fail; the notifications simply never arrive, exactly as if the hardware never changed. That is strictly better than a refusal — a refusal is a fact about this driver that a client can misread as a fact about the graph — but it is not the same as event support, and it should not be recorded as such.

**Honest expectation:** the event refusals happened *before* the point where the session stops and it continued past them, so **this is not predicted to fix WMP.** It closes the last gap in the published surface that has a reference implementation behind it. The build's real value is the process-name measurement, which settles the table above whichever way it falls.

Built clean, `chk` x64: `stwrtxp.sys` **71680 → 75264 bytes**. Staged with the matching `.pdb`.

## Stage 5bh — Stage 5bg bluescreened on install: PortCls calls event handlers at DISPATCH_LEVEL

**The Stage 5bg build bugchecked while the driver was being installed.** `SYSTEM_SERVICE_EXCEPTION`, stop `0x3B`. This was a defect in the change described immediately above — not a new hardware discovery — and it is written up at length because the mistake is a *class* of mistake rather than a typo.

### The dump

`MEMORY.DMP`, 600387584 bytes, 05:29, at the project root. Read with `C:\WinDDK\7600.16385.1\Debuggers\kd.exe` and `_NT_SYMBOL_PATH` pointed **only** at `Backported Driver\package`. `nt` resolves from exports alone and `!analyze` complains loudly about it, which does not matter here: the answer is in arg1 and in our own frames, and those come from `package\stwrtxp.pdb`. (The public MS symbol server still does not serve `ntoskrnl.pdb` for this build — do not retry it.)

```
BugCheck 3B, {80000003, fffff80001026e88, fffffadf23b12710, 0}
Probably caused by : stwrtxp.sys ( stwrtxp!EventHandler_ControlChange+50 )
```

`.cxr fffffadf23b12710` then `k` — the frames named `portcls!PcRegisterIoTimeout+...`, `ks!KsGenerateEventList+...` etc. are nearest-preceding-export guesses for private routines, so only the module and the shape of the chain are real:

```
nt!DbgBreakPointWithStatus+0x47          <- int 3
nt!RtlAssert+0xb6
stwrtxp!EventHandler_ControlChange+0x50
portcls!<internal event dispatch>
ks!<internal>  ks!<internal>  ks!<internal>
portcls!<internal>
portcls!PcDispatchIrp+0x179
stwrtxp!HookedMjDeviceControl+0x167
sysaudio+0x19bd
sysaudio+0x1e844
ks!KsSynchronousIoControlDevice+0xe7
wdmaud+0x24368 ... wdmaud+0x22d50
nt!NtDeviceIoControlFile+0x551
```

**Read arg1 before anything else.** `80000003` is `STATUS_BREAKPOINT`, not an access violation — so nothing dereferenced anything bad and no pointer was stale. An `ASSERT` fired. The only assert in `EventHandler_ControlChange` was its `PAGED_CODE()`, and `PAGED_CODE()` asserts exactly one thing: that IRQL is at or below `APC_LEVEL`.

**So: PortCls invokes miniport event handlers at DISPATCH_LEVEL** — it holds its event-list lock across the call. The stack is the very first `IOCTL_KS_ENABLE_EVENT` that Stage 5bg made reachable: wdmaud → sysaudio → our own dispatch hook → PortCls → the new handler → assert. Before Stage 5bg, PortCls refused those IOCTLs with `C0000230` before there was any handler to call, which is precisely why twelve of them per boot were harmless.

### The reference said so, and it was not read

`sb16\mintopo.cpp:1438` emits `#pragma code_seg()` immediately before its `EventHandler`, and `:1501` restores `#pragma code_seg("PAGE")` after `ServiceEvent`. That pragma pair *is* the sample saying the handler must not be pageable. Stage 5bg copied the handler's body and dropped the two lines around it.

sb16 does also leave a `PAGED_CODE()` inside its handler, which is a latent bug in the sample — the whole function is inside `#ifdef EVENT_SUPPORT`, which the sample never defines, so it was never once executed. Copying that line was copying an untested line.

### Two defects, not one

1. **`EventHandler_ControlChange` had `PAGED_CODE()` *and* sat inside `mintopo.cpp:61`'s `#pragma code_seg("PAGE")`.** The assert is the lesser half. The real hazard is the segment: paged code entered at DISPATCH_LEVEL faults if the page happens to be out, and a page fault at DISPATCH_LEVEL is `IRQL_NOT_LESS_OR_EQUAL`. A **free** build, with `PAGED_CODE()` compiled away, would not have asserted — it would have worked until the day that page was trimmed and then bugchecked somewhere that looked unrelated. **The checked build converted a latent, boot-dependent crash into a deterministic one on the first install.** That is the good outcome, and it is an argument for staying on `chk` until item p.

2. **`LogToFileF` does `ZwCreateFile`/`ZwWriteFile` and was also in a `PAGE` segment.** Every `DOUT` in the new handler routed into it. Its own header comment stated the invariant it depended on — *“Called from DOUT … at PASSIVE_LEVEL only (every DOUT call site in this driver runs from PAGED_CODE paths)”* — and Stage 5bg silently falsified it. Even with defect 1 fixed on its own, the first `DOUT` in the handler would have attempted file I/O at DISPATCH_LEVEL and crashed again.

### The fix

- **`mintopo.cpp`: `#pragma code_seg()` before `EventHandler_ControlChange`, `#pragma code_seg("PAGE")` after it, and the `PAGED_CODE()` removed.** Exactly sb16's arrangement.
- **`common.cpp`: `LogToFileF` moved out of the `PAGE` segment and given `if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;` at the top.** This is the durable half of the fix. Rather than audit every present and future `DOUT` call site for IRQL, the logger now declines to do file I/O when it cannot legally do it — and it is non-paged so that the call itself cannot fault on the way in. `DbgPrint`, the other half of `DOUT`, is safe at any IRQL and still runs.

  **Consequence to remember when reading any future log: a `DOUT` from a raised-IRQL path produces no line in `stwrtxp_log.txt`.** The event handler's own trace lines are therefore invisible on disk. What proves the handler ran is the `KSEVENT #N … -> 00000000` result line from the dispatch hook in `adapter.cpp`, which is emitted after PortCls returns and IRQL has dropped back to PASSIVE.

**Verified in the binary, not merely in the source.** `dbh.exe <stwrtxp.sys> enum <symbol>` against the section table from `link -dump -headers` (image base `0x1000000`; `.text` = RVA `0x1000`–`0x549C`, `PAGE` = RVA `0x9000`–`0x23A5F`):

| symbol | RVA | section | |
|---|---|---|---|
| `EventHandler_ControlChange` | `0x3350` | `.text` | non-paged, as intended |
| `LogToFileF` | `0x2B40` | `.text` | non-paged, as intended |
| `PropertyHandler_CpuResources` | `0xDEE0` | `PAGE` | control case |

The third row is the point of the exercise: it proves the `code_seg("PAGE")` restoration took and the rest of the file is still paged as intended, rather than the patch having accidentally un-paged everything after it.

**`dbh.exe` plus `link -dump -headers` is the way to check segment placement on this project.** `link -dump -symbols` cannot help — this build's object files report as `ANONYMOUS OBJECT` (LTCG) and carry no readable symbol table.

Also checked while in there: `adapter.cpp`'s two dispatch hooks (`HookedMjCreate` `:157`, `HookedMjDeviceControl` `:316`) and the `ResolveProcessNameHelper` `:120` / `CurrentProcessName` `:138` helpers all sit *before* that file's first `code_seg` pragma at `:551`, so they are in the default non-paged `.text` already — correct by construction rather than by luck. Nothing else in Stage 5bg was implicated: the `Port->QueryInterface(IID_IPortEvents)` in `Init`, the destructor's `Release`, and the process-name diagnostic all run at PASSIVE_LEVEL and were never on the faulting path, despite `Init` being the leading suspect on timing alone before the dump was read.

Built clean, `chk` x64: `stwrtxp.sys` **75264 bytes, unchanged** — the edit relocates code between sections and adds two lines. Staged with the matching `.pdb`.

### The rules this earns

**When copying a handler out of a WDK sample, copy the `#pragma code_seg` lines around it, and treat their presence or absence as load-bearing.** More sharply: **PortCls calls property handlers at PASSIVE_LEVEL but event handlers at DISPATCH_LEVEL.** Any handler added to this driver from here on needs that question answered *before* it is written — and `PAGED_CODE()` is not a safety net, because in a free build it is not there at all.

And a second-order rule, this being the third correction of the same family in three stages: the Stage 5bf `id=33` misreading and the Stage 5bg `sb16` event-set miss both came from an incomplete *read* of the reference; this one came from an incomplete *copy* of it. **The reference is only evidence for the part of it you actually looked at.**

## Stage 5bh RESULT — no bluescreen, VLC plays, and the process-identity question is finally settled

**The fix held.** The user reinstalled the Stage 5bh build and the machine did not bugcheck. **VLC now plays audio.** Windows Media Player is still silent, and — asked directly — there is no short blip or ding either, nothing at all.

Every `IOCTL_KS_ENABLE_EVENT` in the new log returns `00000000`, where the two previous logs had twelve `C0000230` refusals per boot. So Stage 5bg's substance was right and only its IRQL discipline was wrong.

One thing to notice about the log rather than the fix: `EventHandler_ControlChange`'s own `DOUT` lines **do** appear on disk in this log. The IRQL guard only suppresses the file half when IRQL is raised, so their presence proves those particular ENABLE calls arrived at PASSIVE_LEVEL. The crash proved the call *can* arrive at DISPATCH_LEVEL. Both are true; the handler has to be correct at the higher one regardless of how often the lower one happens to occur.

### `{146F1A80-4791-11D0-A5D6-28DB04C10000}` is `KSSTRING_Pin` — the most useful log-reading technique this project has found

An `IRP_MJ_CREATE` whose name is that GUID string **is pin instantiation**. `HookedMjCreate` was already logging the create name; nothing had recognised what the string meant. Decoding it turns the process-identity question from an inference into a table:

| PID | image | opens `\Wave` | creates a pin |
|---|---|---|---|
| 4 | System | yes (many) | no |
| 472 | winlogon.exe | yes | **yes (#24)** |
| 1772 | explorer.exe | yes (#25, #29) | **yes (#30)** |
| **1608** | **wmplayer.exe** | **yes (#26), once** | **NEVER** |
| 2712 | vlc.exe | yes (#27) | **yes (#28)** |

This retires the 3060-vs-2464 ambiguity that blocked Stage 5bf and 5bg — the two readings that selected different next fixes were both wrong, because **WMP is the session that opens the wave filter and stops.** It has never created a pin in any log this project has collected. The sessions that produced audio were winlogon, explorer and vlc.

**Read the `MJ_CREATE` names first in any future log**, and check for that GUID specifically: it is the difference between knowing a client tried to stream and guessing.

### WMP's whole interaction, and the entire failure inventory of a 2011-line log

WMP is requests **#693–#728**. Excluding 54 benign `80000005` size probes, the log's complete failure set is four items:

| what | status | verdict |
|---|---|---|
| `Pin id=12` ×5 (PID 4) | `C0000034` | pin NAME — absent by design, closed in 5bf |
| `Pin id=10` ×4 (PID 4) | `C0000225` | PHYSICALCONNECTION — none exists on those pins, normal |
| `Connection id=2` ×2 (winlogon, **vlc**) | `C000000D` | Stage 5bc rate refusal — **both sessions then played audio**, so proven benign |
| `#718` WMP `Audio id=33` virtual node 4 | `C0000225` | CPU_RESOURCES on a SUM node — reference behaviour, closed below |

That is all of it. Nothing else in the log fails.

WMP and VLC do the **identical** mixer-line setup — MUTE, Topology NAME, VOLUME, NAME, VOLUME, NAME, then three `ENABLE_EVENT` (WMP #693–#716, VLC #729–#752). Then they diverge completely:

- **VLC**: opens `\Wave` → `Pin id=4 DATAINTERSECTION in=216 out=82 → 00000000` → creates a pin → streams. **VLC never asks `id=33` at all.**
- **WMP**: opens `\Wave` → walks `id=33` CPU_RESOURCES over virtual nodes 8, 4, 6, 6, 5, 8 → `id=3 CHANNEL_CONFIG SET mask=00000003 → 00000000` → `id=34 STEREO_SPEAKER_GEOMETRY SET value=20 → 00000000` → **stops. PID 1608 never appears again.**

That walk followed by the CHANNEL_CONFIG/GEOMETRY pair on node 8 is the signature of `IDirectSound::SetSpeakerConfig`. **WMP's last request succeeds.** It never asks for a data intersection and never creates a pin. So whatever WMP rejects, it rejects on information it already holds — not on an answer this driver got wrong at the moment of the decision.

### `#718` and `CPU_RESOURCES`: two hypotheses closed on maximal scope

Both are recorded because each was a *fourth* attempt at "the driver answered something wrong", and both died against the reference. The pattern is worth more than either result.

**`#718` — the two `KSNODETYPE_SUM` nodes with NULL automation.** Justified twice before on narrow scope (once against msvad, once against ac97). Closed this time by reading **every** SUM descriptor in the WDK: msvad (7 variants at `toptable.h:287`, plus `simple/toptable.h:330`), sb16 (`tables.h:514`, `:558`) and ac97 (`mintopo.cpp:1166`, `:1221`, both passing `NULL` as `INIT_NODE`'s automation table, in contrast to `INIT_NODE(NODE_AUX_MUTE, ..., &AutomationMute, Index)` right beside them). **Eleven SUM nodes, three driver families, all NULL.** ac97's render path would return the same `C0000225`. And decisively: **WMP continued past #718 to two more requests that succeeded**, so it is not terminal.

**`KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU` — is claiming "hardware" a lie that misleads DirectSound?** It is arguably a lie for a WaveCyclic driver where kmixer does all the work, and the hypothesis was that DirectSound reads it, concludes the device does hardware mixing, and then fails on a path that never touches us. Dead: **every WDK audio sample returns exactly this value** — ac97 `prophnd.cpp:1651`, fmsynth `miniport.cpp:1283`, msvad `basetopo.cpp:354` and `basewave.cpp:267`, sb16 `mintopo.cpp:1267`. Five families, one answer, and `ksmedia.h:1473` defines it as `0x00000000` against `HOST_CPU` `0x7FFFFFFF`. Whatever this value does, it does the same thing for every reference driver.

### The `peak |sample| = 1, RMS = 0` scan results are a timing artefact, not a signal loss

The Stage 5ay/5az `StopEngine` DMA scan reads differently in this log than in the previous two (`peak 755, RMS 100`), which mattered because a lot of past reasoning rested on those numbers. Reading all thirteen scan lines explains it completely:

| session | non-zero | peak | RMS |
|---|---|---|---|
| setup-time `STOP` (×3, one per stream) | 0 | 0 | 0 |
| winlogon teardown | 1279 | 1 | 0 |
| vlc teardown | 2084 | 1 | 0 |
| explorer teardown | 5216 | **400** | **53** |

The scan reads whatever is in the 16 KB cyclic buffer at stop time — at 48 kHz stereo 16-bit that is an **85 ms window**. explorer played a short system ding that still fitted inside the window; VLC played a long file whose last 85 ms before stop were the fade-to-silence tail, which is exactly what `peak 1` is: kmixer's rounding noise around digital silence. **VLC was audible in this very log**, which settles it.

So: **the DMA scan is only meaningful for short sounds.** It cannot distinguish "silent path" from "stopped after a long stream" — but `0 non-zero, peak 0` (the three setup-time lines) still reliably means nothing has been written yet.

For the record, all three sessions ran the identical clean lifecycle: `NewStream: entry, Pin=0, Capture=0` → `SetFormat 48000` OK → engine handle `FFFFFADF44B96004` → `AllocateBuffer: requested 16384, got 16384, stream ID 1, fifo 128` → `NewStream: success` → `STOP→ACQUIRE→PAUSE→RUN` → `StartEngine: running, stream tag 1, converter format 0011, notification interval 10 ms`. winlogon and vlc then each had `SetFormat 22050` refused `C000000D` and kept their prior format; explorer instead had `SetFormat 44100` **accepted**, with a re-`AllocateBuffer` and `converter format 4011`.

## Stage 5bi — the data ranges were a contiguous span, and no reference driver states them that way

With WMP's failure now known to happen **after** the last request this driver answers, and every property answer on that path matched to the reference, the remaining candidates are things the driver states *before* the decision. The largest of them is the pin's data-range list, which sysaudio caches when it enumerates the graph at boot and DirectSound consults before it creates a buffer.

**This driver published one `KSDATARANGE_AUDIO` with `MinimumSampleFrequency = 44100, MaximumSampleFrequency = 48000`.** Stage 5bb had narrowed it from `8000..48000` — which was a real fix, and it is why audio works at all — and argued in its own comment that *"a contiguous range loses nothing here: kmixer only ever picks from the standard rate set."* About kmixer that is correct. It is not how any reference driver states its rates:

> **ac97 `BuildDataRangeInformation`** (`wavepciminiport.cpp:739-762`, identically in `rtminiport.cpp:701`) walks `dwWaveSampleRates = {48000, 44100, 32000, 22050, 16000, 11025, 8000}`, **programs each rate into the hardware** to see whether it takes, and emits one range per surviving rate with `MinimumSampleFrequency == MaximumSampleFrequency`. It then patches the pin descriptors' `DataRangesCount` at `:783-789` by matching on the `DataRanges` pointer.

The consequence worth noting: on a non-VSR AC'97 codec exactly **one** range survives, `48000..48000`, and WMP plays fine on those machines. **So a narrow rate *set* is demonstrably not the problem. The contiguous *span* is the part that is unlike the reference.**

The span was also a claim this driver did not honour. It said every rate from 44100 to 48000 inclusive was acceptable, while `ValidateFormat` accepted 44100 and 48000 and refused everything between. Nothing in the stack is obliged to ask only about the endpoints.

**The change**, in `wavecyclicminiport.cpp`:

- `PinDataRangePcm` is now only a **template** holding the rate-invariant fields (max 2 channels, 16..16 bit) and is no longer pointed at by any pin.
- `PinDataRangesPcm[HDA_PCM_RATE_COUNT]` plus `PinDataRangesOut[]`/`PinDataRangesIn[]` carry **one discrete range per rate**, `Minimum == Maximum`.
- `NarrowPcmRangeToCodec` → **`BuildPcmDataRanges`**, which intersects `PinAdvertisedRateHz = {48000, 44100}` with the codec's bitmap via the existing `g_HdaPcmRateHz` table, sets `DataRange.SampleSize` to the frame size (4 bytes) as ac97 does at `:750`, and patches each streaming pin's `DataRangesCount` using ac97's pointer-match idiom. The bridge pins are untouched.
- **`ValidateFormat` now tests membership in `PinDataRangesPcm` instead of comparing against two endpoints**, so the advertisement and the validator read from the same array and cannot drift apart again. The codec-bitmap check stays as the one with hardware behind it.
- `GetDescription` logs the count and every rate, because there is no longer a single span to print — and a count of zero there would mean the pins shipped with no PCM formats at all.

The advertised capability **set is unchanged** (44.1 and 48 kHz); only its shape is. The codec also reports 88.2, 96 and 192 kHz; adding one is a one-line change to `PinAdvertisedRateHz`, but it would advertise a converter-format and buffer-maths path nothing has exercised, so it stays out.

Built clean `chk` x64, `stwrtxp.sys` 76288 bytes (was 75264). Segment placement re-verified with `dbh.exe`, including the Stage 5bh invariants, since a new build is exactly when those could silently regress:

| symbol | RVA | section | |
|---|---|---|---|
| `EventHandler_ControlChange` | `0x3350` | `.text` | 5bh invariant intact |
| `LogToFileF` | `0x2B40` | `.text` | 5bh invariant intact |
| `BuildPcmDataRanges` | `0xF1A0` | `PAGE` | correct — called from `Init` at PASSIVE |
| `PropertyHandler_CpuResources` | `0xDEE0` | `PAGE` | control case |

**This is a correctness fix that is also the best remaining WMP candidate — and those are two different claims.** The span/validator mismatch is a defect on its own and worth removing regardless. Whether it is what WMP rejects is unproven, and if the next log shows WMP behaving identically then the honest conclusion is that the driver's *stated* capabilities are no longer the place to look, and the next move is client-side: what `wdmaud`/`dsound` do between our last answered request and WMP's error.

## Stage 5bi RESULT — the discrete ranges published correctly and WMP did not care

The user installed the Stage 5bi build and reported "Same result". The log is
`_TSD17B.tmp\_TS21.tmp\stwrtxp_log.txt`, 2679 lines.

**Read the session boundaries first.** `stwrtxp_log.txt` is append-mode across
installs and boots. `DriverEntry:` appears at lines **1, 181 and 2015**, so only
lines 2015-2679 are Stage 5bi data. Lines 172 and 352 still print the *old*
`GetDescription: publishing PCM range 44100..48000 Hz` message from the previous
build. Analysing the file as a whole would have produced nonsense.

**Stage 5bi did exactly what it was written to do.** Session C contains:

```
BuildPcmDataRanges: range 1 of 2 - 48000 Hz exactly, 16..16 bit, max 2 ch, frame 4 bytes
BuildPcmDataRanges: range 2 of 2 - 44100 Hz exactly, 16..16 bit, max 2 ch, frame 4 bytes
BuildPcmDataRanges: rate mask 000E05E0 -> 2 discrete range(s) (was one contiguous 44100..48000 span)
GetDescription: publishing 2 discrete PCM range(s)
```

and sysaudio really did read them — `set=Pin id=3` (DATARANGES) is queried on all
four pins of both filters, size probe then success.

**The verification gap from the previous window is closed, and it is closed
statically — no rebuild was needed.** The worry was that
`BuildPcmDataRanges`' patch loop compares `MiniportWavePins[p].KsPinDescriptor.DataRanges`
against `PinDataRangesOut`, and that both live log lines print
`PinDataRangesPcmCount` rather than the descriptor's own `DataRangesCount`; if the
comparison silently failed the pins would still advertise `HDA_PCM_RATE_COUNT` = 12
ranges, ten of them zeroed structs. It cannot fail:
`wavecyclicminiport.cpp:119` initialises the descriptor field with the array
`PinDataRangesOut` (which decays to `&PinDataRangesOut[0]`), and `:716` compares
against that identical expression. Both sides are the same compile-time address.
The bridge pins use `PinDataRangePointersBridge`, so they are correctly skipped.
`DataRangesCount` really was patched 12 -> 2 on both streaming pins.

So **session C is a genuine no-change result**, not a botched deployment. That
matters, because it is what licenses the pivot below.

**WMP's interaction is the same shape as before, with a new PID (2416).** Requests
#256-#291 mirror session B's #693-#728 step for step: the mixer-line setup
(`Audio id=13` MUTE, `Topology id=3` NAME, `Audio id=4` VOLUME on nodes 3 and 1,
three `ENABLE_EVENT` all returning `00000000`), `MJ_CREATE #25 Name="\Wave"` ->
`00000000`, the `id=33` CPU_RESOURCES walk over virtual nodes 8, 4, 6, 6, 5, 8,
then two successful sets:

```
PropertyHandler_ChannelConfig:   node=0 verb=10000002 mask=00000003 -> 00000000
PropertyHandler_SpeakerGeometry: node=0 verb=10000002 value=20     -> 00000000
```

and then the log ends. No pin, no data intersection, no stream. **The last thing
the driver does for WMP is succeed.**

Session C has **no streams at all** — zero `146F1A80` creates, no
`NewStream`/`SetFormat`/`StartEngine`/`StopEngine`. That is *not* a regression, and
an early draft of this analysis wrongly called it "a different and worse story".
The failure inventory is unchanged: 322 x `00000000`, 54 x `80000005` (benign size
probes), 6 x `C0000034` (Pin id=12 NAME), 5 x `C0000225`. **No new failure class.**
The absence of streams is simply because the log ends at WMP's attempt — the user
booted, tried WMP, and captured; no system sounds and no VLC were exercised in that
session.

**Two more theories died here, both by positive evidence rather than absence.**

*Physical connections are registered and reported correctly.* The 4 x `C0000225` on
`Pin id=10` (PHYSICALCONNECTION) look alarming until the file objects are separated.
Two FOs appear, with exactly complementary answers:

| file object | pins 0,1 | pins 2,3 | identity |
|---|---|---|---|
| `FFFFFADF3F3AC810` | **success, out=258** | `C0000225` | topology filter |
| `FFFFFADF44998EE0` | `C0000225` | **success, out=266** | wave filter (bridges are pins 2,3) |

Our wave filter's bridge pins *are* pins 2 and 3, and they are the ones that answer.
`C0000225` on a streaming pin is the correct "this pin has no physical connection".
`StartDevice: complete, physical connections registered` appears with no
`PcRegisterPhysicalConnection ... failed` line. This is working.

*Endpoint enumeration is not the problem either.* `stwrtxp.inf:127-131` already
registers `KSCATEGORY_RENDER` `{65E8773E-...}` and `KSCATEGORY_CAPTURE`
`{65E8773D-...}` alongside `KSCATEGORY_AUDIO` on the wave interface.

**Tally: five "the driver answered something wrong" theories have now died against
maximal-scope reference checking** — `#718` Audio id=33 on a SUM node, pin instance
counts, CPU_RESOURCES' `NOT_HOST_CPU`, physical connections, and interface
categories. The contiguous-span data range was a real defect and fixing it was
right, but it was not the cause either.

## Stage 5bj — stop interrogating the driver; make the client say where it stops

The driver log physically cannot show why WMP gives up, because **nothing in the
driver fails**. WMP's final request succeeds and then it stops. The decision is
made in user mode, above PortCls, and every remaining driver-side hypothesis is a
guess about code we cannot see.

So the next move is a user-mode probe. `package\dstest.exe` (source
`scratchpad\dstest.c`) walks the exact DirectSound initialisation sequence WMP's
DirectSound renderer uses and prints the HRESULT of **every** step, to stdout and to
`dstest_log.txt` beside itself. It reports:

- `waveOutGetNumDevs` / `waveOutGetDevCaps` including the decoded `dwFormats`
  bitmask — the control, since waveOut already works;
- `DirectSoundEnumerate` — whether the device appears to DirectSound at all;
- `DirectSoundCreate` on the default device and on each enumerated GUID;
- **`IDirectSound::GetCaps`** with `dwFlags` decoded and, above all,
  **`dwMinSecondarySampleRate` / `dwMaxSecondarySampleRate`**, which sysaudio derives
  from the data ranges our pin advertises. `DSCAPS_EMULDRIVER` is called out
  explicitly: if it is set, DirectSound is emulating over waveOut rather than using
  our pin;
- `SetCooperativeLevel(DSSCL_PRIORITY)` and `GetSpeakerConfig`;
- primary buffer create, `GetFormat`, and `SetFormat` at 48000 and 44100 — this is
  the object whose manipulation produces the `CHANNEL_CONFIG` and
  `STEREO_SPEAKER_GEOMETRY` sets already visible in the driver log;
- secondary buffer create across 48000/44100/32000/22050/16000/11025/8000/96000 at
  16-bit stereo, plus 8-bit and mono variants at 44100;
- an audible test at 48000 then 44100, reporting whether the **play cursor actually
  advances** (a cursor stuck at 0 means the DMA never ran).

**The hypothesis it is built to test** — stated as a hypothesis, not a diagnosis — is
that we advertise only 44100 and 48000 (all the codec's rate bitmap `000E05E0`
reports below 88200), sysaudio turns that into a narrow
`dwMinSecondarySampleRate`/`dwMaxSecondarySampleRate` window, and DirectSound then
refuses `CreateSoundBuffer` at some rate WMP wants with `DSERR_BADFORMAT` — while
VLC keeps working because it uses waveOut, where kmixer resamples unconditionally.
**The probe's value does not depend on that being right.** It reports where
DirectSound actually stops, whatever the reason.

**Build recipe** (`scratchpad\dstest_build.cmd`), which is worth keeping because it
is the first user-mode build in this project:

```
call C:\WinDDK\7600.16385.1\bin\setenv.bat C:\WinDDK\7600.16385.1 fre x64 WNET no_oacr
cl /nologo /W3 /O2 /MT /GS- /DDIRECTSOUND_VERSION=0x0900 ^
   /I"%DDK%\inc\api" /I"%DDK%\inc\crt" dstest.c ^
   /link /SUBSYSTEM:CONSOLE,5.02 /MACHINE:AMD64 ^
   /LIBPATH:"%DDK%\lib\wnet\amd64" /LIBPATH:"%DDK%\lib\crt\amd64" ^
   dsound.lib winmm.lib ole32.lib user32.lib kernel32.lib
```

Points that matter and were each checked:

- **WNET is the right target.** `lib\wnet\amd64` is the Server 2003 SP2 x64
  environment, which is exactly XP x64's kernel and CRT vintage. The WDK ships
  `inc\api\dsound.h` and `lib\wnet\amd64\dsound.lib`, so no separate DirectX SDK
  is needed.
- **`/MT` against `lib\crt\amd64\libcmt.lib` is mandatory**, not a preference. The
  XP x64 target has no Visual C++ 2008 redistributable. Verified after the fact:
  `link -dump -imports` shows only `DSOUND.dll`, `WINMM.dll`, `ole32.dll`,
  `USER32.dll`, `KERNEL32.dll` — no `MSVCR90`.
- **`/SUBSYSTEM:CONSOLE,5.02` is mandatory.** The default subsystem version is too
  new and XP x64's loader would refuse the image. `link -dump -headers` confirms
  `5.02 subsystem version`, `8664 machine (x64)`, `3 subsystem (Windows CUI)`.
- `/GS-` avoids needing `BufferOverflowU.lib`; there is no untrusted input.
- The sine generator was deliberately written as an **integer** triangle wave to
  avoid linking the CRT math library, keeping the build to a single `cl` command.
- The `_CRT_SECURE_NO_DEPRECATE` macro-redefinition warning and the
  "x64 Native compiling isn't supported. Using cross compilers." notice are both
  harmless.

Add `package\dstest.exe` and the `scratchpad\dstest.*` files to the item p cleanup
list; the probe is a diagnostic, not part of the shipped driver.

## Stage 5bj RESULT — DirectSound is completely healthy, and that is a big result

`dstest.exe` ran on the target. **Every single call returned `DS_OK`.** Not one
failure anywhere in the DirectSound path.

The headline: `CreateSoundBuffer` succeeded at **48000, 44100, 32000, 22050, 16000,
11025, 8000 and 96000 Hz**, plus 8-bit mono, 8-bit stereo and 16-bit mono at 44100.
`GetCaps` reported `dwMinSecondarySampleRate = 44100`, `dwMaxSecondarySampleRate =
48000`, exactly as derived from our pin — **and it gated nothing.**

**Why the hypothesis was wrong, stated properly, because the reasoning is reusable.**
`dwMin`/`dwMaxSecondarySampleRate` describe the rate window for **hardware-mixed**
buffers only. Software buffers are unconstrained — DirectSound allocates them in
system memory and resamples on the way to the primary. And `GetCaps` also reported
`dwMaxHwMixingAllBuffers = 1` with `dwFreeHwMixingAllBuffers = 0`, so every buffer
`dstest` created was a software buffer regardless. The narrow window was real and
was correctly derived from our data ranges; it simply is not a gate on anything a
media player does. **Sixth theory dead, and the first one killed by direct
measurement rather than by reference comparison.**

Everything else in that output is also healthy:

- `DirectSoundEnumerate` lists two entries — `Primary Sound Driver` (NULL GUID) and
  **`IDT 92HD89E2 HD Audio (XPDM backport)` with `module = stwrtxp.sys`** and GUID
  `{BD6DD71A-3DEB-11D1-B171-00C04FC20000}`. Both the default device and the
  explicitly-named device behave identically.
- `DSCAPS_EMULDRIVER` is **not** set, so DirectSound is using our KS pin, not
  emulating over waveOut. `CERTIFIED` and `CONTINUOUSRATE` are set.
- `GetSpeakerConfig` returns `00140004` = `DSSPEAKER_STEREO` with
  `DSSPEAKER_GEOMETRY_WIDE` (0x14 = 20), which matches the `value=20` the driver
  log records for `KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY`. Consistent end to end.
- The primary buffer's default format is `22050 Hz, 8 bit, 2 ch`, which is simply
  DirectSound's documented default and not a driver artefact. `SetFormat` to both
  48000 and 44100 16-bit stereo succeeded.
- **The play cursor advanced on all four playback attempts** (48000: 38496/38432 of
  a 192000-byte buffer; 44100: 35220/35404), so the DMA engine genuinely ran under
  DirectSound.

**Two observations from that log worth carrying forward:**

1. `waveOutGetDevCaps` reports **`wChannels = 65535` (0xFFFF)** and
   **`wPid = 0xFFFF`**, while our pin advertises `MaximumChannels = 2`. Two adjacent
   `WORD`s both `0xFFFF` looks like wdmaud's "unknown" filler rather than a misread
   of our data, but it is unconfirmed and is re-probed in Stage 5bk.
2. `dwSupport = 0000002C` = `WAVECAPS_VOLUME | WAVECAPS_LRVOLUME |
   WAVECAPS_SAMPLEACCURATE`. `dwFormats = 000BFFFF` — every standard format up to
   48 kHz, plus three of the four 96 kHz ones (96M16 absent, which is wdmaud's
   doing, not ours).

**One real bug in `dstest.c`, fixed in `audiodiag.c`:** the log was opened as
`fopen("dstest_log.txt", "w")` — a *relative* path, so it landed in whatever the
working directory happened to be and the user could not find it. The replacement
derives the path from `GetModuleFileNameA(NULL, ...)`, falls back to `GetTempPathA`,
and prints the resolved path at both the start and the end of the run. **Any future
probe in this project must do the same.** The user pasted the console text instead,
so nothing was lost.

## Stage 5bk — the mixer API, and n42 wrongly promoted from "cosmetic" to "evidence"

**Heading amended after the fact.** The promotion below was wrong and is retracted in the RESULT section that follows. The section is kept as written because the reasoning it contains is the reason the measurement was worth taking, and because a hypothesis that was disproved is only useful if it is still legible.

With DirectSound cleared, the remaining user-mode layer WMP touches is the **mixer
API**, and it is the one it touches *first*: WMP's opening move in every driver log
is mixer-line setup — `Audio id=13` MUTE, `Topology id=3` NAME, `Audio id=4` VOLUME
on two nodes, three `ENABLE_EVENT`s. All succeed at the driver level.

**Item n42 is the reason to look here, and it was mis-filed.** n42 records that the
Windows volume slider traverses only about **0.54 dB** of the range the driver
reports. The driver's own numbers are correct: `BASICSUPPORT range -6242304..0 step
49152`, and since KS volume units are 1/65536 dB that is **0 dB down to −95.25 dB in
0.75 dB steps** — 128 steps, entirely sane. So something *above* the driver is
misreading them, and 0.54 dB is suspiciously close to a single 0.75 dB step. That
was filed as cosmetic **on assumption, not on evidence**, and a mixer layer that
computes a broken range from sane input is exactly the kind of thing that makes a
client conclude the device is defective.

`package\audiodiag.exe` (source `scratchpad\audiodiag.c`, build
`scratchpad\audiodiag_build.cmd`) supersedes `dstest.exe` and dumps:

- **the entire mixer topology as the mixer API presents it** — `mixerGetNumDevs`,
  `mixerGetDevCaps`, then for every destination and every source line:
  `dwLineID`, `dwComponentType` decoded, `cChannels`, `cConnections`, `cControls`,
  `fdwLine` (with `ACTIVE` / `DISCONNECTED` called out), names, and the
  `Target` block;
- **every control on every line** via `mixerGetLineControls(ALL)`: id, type decoded,
  `fdwControl` (with `UNIFORM` / `MULTIPLE` / `DISABLED` called out),
  `Bounds.lMinimum`/`lMaximum`, `Metrics.cSteps`, and the **live value** from
  `mixerGetControlDetails`. A `MIXERCONTROL_CONTROLTYPE_VOLUME` whose bounds are not
  the canonical `0..65535` is flagged explicitly — that is the n42 test;
- **the exact lookup a media player performs**:
  `mixerGetLineInfo(COMPONENTTYPE, DST_SPEAKERS)` and
  `mixerGetLineInfo(COMPONENTTYPE, SRC_WAVEOUT)`. If either returns
  `MIXERR_INVALLINE`, a player cannot build its volume control and that is a
  sufficient explanation on its own;
- `waveOutGetDevCaps` again with `wChannels == 0xFFFF` called out, plus
  `waveOutGetVolume`;
- **a real `waveOut` open/write/play test** at 48000 and 44100 — the VLC-equivalent
  path, as the audible control against DirectSound — reporting the `MMRESULT` of
  `waveOutOpen(WAVE_FORMAT_QUERY)`, `waveOutOpen`, `waveOutPrepareHeader`,
  `waveOutWrite`, whether `WHDR_DONE` ever set, and the final `waveOutGetPosition`;
- a trimmed DirectSound recheck, including the
  `dwMaxHwMixingAllBuffers > 0 && dwFreeHwMixingAllBuffers == 0` condition.

**This is a measurement, not a claim.** It may show a perfectly healthy mixer, in
which case the mixer is eliminated the way DirectSound just was, and the next target
is WMP's own DirectShow renderer graph. Note for that eventuality: the WDK ships
`strmiids.lib` and `quartz.lib` for wnet/amd64 but **not** `dshow.h` or `strmif.h`,
so a DirectShow probe would require hand-declaring the COM vtables for
`IGraphBuilder`/`IBaseFilter`/`IPin` — doable but fragile, and worth avoiding until
the cheaper layers are exhausted.

**One datum is still outstanding and matters:** the user has not yet reported
whether `dstest`'s two buzzes were *audible*. The play cursor advanced, which proves
the DMA ran, but "cursor advanced" and "sound came out" are different claims. If
DirectSound is audible then the entire audio path works under DirectSound and WMP's
failure is in its own device validation; if it is not audible while VLC is, that is
a new and sharper divergence than anything found so far. `audiodiag.exe` asks the
same question of both waveOut and DirectSound in one run so the comparison is direct.

## Stage 5bk RESULT — the mixer is completely healthy, and n42 really was cosmetic

`audiodiag.exe` ran on the target and the user confirmed **both waveOut tones were
audible**, and separately that **the two DirectSound buzzes from Stage 5bj were
audible too**. Log: `_TSD17B.tmp\_TS22.tmp\audiodiag_log.txt`, 235 lines.

**Every single mixer call returned `MMSYSERR_NOERROR`.** There is no failure of any
kind anywhere in the log.

**The n42 hypothesis is dead, and it was my error.** Stage 5bk promoted n42 from
"cosmetic" to "evidence" on the theory that some layer above the driver was computing
a broken range from our sane `-6242304..0 step 49152`. The probe flagged any VOLUME
control whose bounds were not the canonical `0..65535`. **Not one control was
flagged.** Every VOLUME control on every line reports exactly:

```
Bounds  min=0 (00000000)  max=65535 (0000FFFF)
Metrics cSteps=192 (000000C0)
```

The mixer API is presenting a perfectly canonical range. So the ~0.54 dB of slider
travel in n42 is *not* a mixer-layer misread of our data; it lives above the mixer
API, in the slider UI's own dB-to-linear mapping. **n42 was correctly filed as
cosmetic the first time and my second-guessing of it was wrong.** Seventh theory
dead. The general lesson, which has now been paid for twice in this project: an
anomaly that is *unexplained* is not thereby *evidence*, and "suspicious-looking
number near a real bug" is not a mechanism.

**The full topology, which is worth recording because it is correct and is the
reference for any future regression:**

| line | dwLineID | componentType | ch | conns | ctrls | fdwLine |
|---|---|---|---|---|---|---|
| dest 0 "Speaker" | `FFFF0000` | `DST_SPEAKERS` | 2 | 3 | 2 | ACTIVE |
| src 0 "Wave" | `00000000` | `SRC_WAVEOUT` | 2 | 0 | 2 | ACTIVE SOURCE |
| src 1 "SW Synth" | `00010000` | `SRC_SYNTHESIZER` | 2 | 0 | 2 | ACTIVE SOURCE |
| src 2 "CD Player" | `00020000` | `SRC_COMPACTDISC` | 2 | 0 | 2 | ACTIVE SOURCE |
| dest 1 "Wave" | `FFFF0001` | `DST_WAVEIN` | 1 | 1 | 0 | ACTIVE |
| src 0 "Microphone" | `00000001` | `SRC_MICROPHONE` | 2 | 0 | 1 | ACTIVE SOURCE |

Control ids 0-8, alternating `VOLUME` (`50030001`) and `MUTE` (`20010002`), all
`fdwControl=00000000` — **no `DISABLED` flag anywhere**, no `DISCONNECTED` line, no
zero-channel line. Live values: every volume `65535 65535`, every mute `0 0`.

**Both of the lookups a media player actually performs succeed**, which is the test
Stage 5bk said would be sufficient on its own if it failed:

```
mixerGetLineInfo(COMPONENTTYPE, DST_SPEAKERS) -> MMSYSERR_NOERROR
  -> "Speaker" lineID=FFFF0000 cControls=2 cConnections=3
mixerGetLineInfo(COMPONENTTYPE, SRC_WAVEOUT)  -> MMSYSERR_NOERROR
  -> "Wave" lineID=00000000 cControls=2 cChannels=2
```

Neither returned `MIXERR_INVALLINE`. **The mixer is eliminated exactly the way
DirectSound was.**

**waveOut playback fully succeeded and was audible.** `waveOutOpen(WAVE_FORMAT_QUERY)`,
`waveOutOpen(WAVE_MAPPER)`, `waveOutPrepareHeader` and `waveOutWrite` all
`MMSYSERR_NOERROR` at both 48000 and 44100; final position `192000 of 192000` and
`176400 of 176400`; `WHDR_DONE = yes` both times.

**`wChannels = 65535` is confirmed as a non-lead, and should not be chased again.**
It reproduced identically in both probes, and it was the only anomalous value found
in either. It is not a lead, for three independent reasons: the adjacent `wPid` is
also `0xFFFF` (two adjacent `WORD`s of all-ones is filler, not a computed value);
`0xFFFF` is *larger* than 2, so any client checking "does this device have enough
channels" passes; and `waveOutOpen` with `nChannels = 2` succeeded and produced
audible sound in the same run, which is the only thing the value could have
plausibly gated. **Closed.**

## Stage 5bl — the DirectShow renderer, which is the last layer left and the one WMP actually uses

**State of the evidence.** Mixer: healthy and correct. waveOut: healthy and
*audible*. DirectSound: healthy and *audible*. kstest: works. VLC: works. System
sounds: work. Pinball: works. **WMP alone fails, and every user-mode audio layer
that can be probed from outside WMP is now clean.** Seven theories dead.

That exhausts the cheap layers, and Stage 5bk pre-committed to this pivot: "It may
show a perfectly healthy mixer, in which case the mixer is eliminated the way
DirectSound just was, and the next target is WMP's own DirectShow renderer graph."

**Two concrete gaps in the Stage 5bj/5bk coverage, found by re-reading the probe
source rather than assuming.** `dstest.c:271` creates every secondary buffer with

```c
dsbd.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
```

and `dstest.c:413-425` only ever passes `bits` of **8 or 16**. So:

1. **The DirectShow DirectSound Renderer's actual flag set was never tested.** It
   creates its buffer with `DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN |
   DSBCAPS_CTRLFREQUENCY | DSBCAPS_GLOBALFOCUS | DSBCAPS_STICKYFOCUS |
   DSBCAPS_GETCURRENTPOSITION2`. Those `CTRL*` flags are not decoration —
   `DSBCAPS_CTRLPAN` requires a buffer DirectSound can pan, and
   `DSBCAPS_CTRLFREQUENCY` requires it to be resampleable. A buffer that creates
   fine bare can fail with `DSERR_CONTROLUNAVAIL` or `DSERR_INVALIDPARAM` when a
   `CTRL*` flag is added.
2. **24-bit and 32-bit were never tested, and neither was `WAVEFORMATEXTENSIBLE`.**
   Our pin publishes `16..16` bit only. WMP 11's Options -> Devices -> Speakers ->
   Properties has a **"Use 24-bit audio"** checkbox; if it is set, WMP asks for
   24-bit. And `WAVEFORMATEXTENSIBLE` with `KSDATAFORMAT_SUBTYPE_PCM` is a distinct
   code path from plain `WAVE_FORMAT_PCM` in both DirectSound and our
   `ValidateFormat`.

**The error string points at exactly one HRESULT.** WMP's "there is a problem with
your sound device" is the surfaced form of DirectShow's
**`VFW_E_NO_AUDIO_HARDWARE = 0x80040256`** — "cannot play back the audio stream: no
audio hardware is available, or the hardware is not responding." That is returned by
`IGraphBuilder::RenderFile` when the graph cannot build an audio renderer. If
`RenderFile` returns it on this machine, the failure is localised to one call.

**`package\wmpdiag.exe` (source `scratchpad\wmpdiag.c`) is Stage 5bl.** It does
four things, in increasing order of fragility so that a crash in the last one does
not cost the first three:

- **Filter instantiation, needing zero hand-declared vtables.** `CoCreateInstance`
  for `CLSID_DSoundRender` `{79376820-07D0-11CF-A24D-0020AFD79767}`,
  `CLSID_AudioRender` `{CD8743A1-3736-11D0-9E69-00C04FD7C15B}` and
  `CLSID_FilterGraph` `{E436EBB3-524F-11CE-9F53-0020AF0BA770}`, each then
  `QueryInterface`'d for `IID_IBaseFilter` `{56A86895-0AD4-11CE-B03A-0020AF0BA770}`.
  Only `IUnknown` is needed, which `objbase.h` already provides. **If the DirectSound
  Renderer cannot even be created, that is the entire answer.**
- **A DirectShow-renderer-shaped DirectSound buffer matrix**: the six-flag set above
  at 44100 and 48000, then each `CTRL*` flag individually to isolate which one (if
  any) is refused, then 24-bit and 32-bit, then `WAVEFORMATEXTENSIBLE` 16-bit with
  `KSDATAFORMAT_SUBTYPE_PCM` and `SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT`, then
  `SetFrequency`/`SetPan`/`SetVolume` on a created buffer.
- **WMP's own persisted state**, read with plain `Reg*` APIs — no COM, no build risk.
  `HKCU\Software\Microsoft\MediaPlayer\Preferences` and its `VideoSettings`
  subkey, plus the installed WMP version. A stale cached audio-device selection in
  WMP's own preferences would produce exactly this symptom while every system API
  stayed healthy, and this machine has had several driver installs and spent time
  with no working audio device at all, which is precisely how such a value goes
  stale.
- **`IGraphBuilder::RenderFile` on a real file**, the one part needing hand-declared
  COM. The WDK ships `strmiids.lib` and `quartz.lib` for wnet/amd64 but **not**
  `dshow.h` or `strmif.h`, so the vtable is declared by hand. **The slot order is
  load-bearing and getting it wrong calls the wrong function pointer**, so record it:
  `IGraphBuilder` = `IUnknown` (QueryInterface 0, AddRef 1, Release 2) + `IFilterGraph`
  (AddFilter 3, RemoveFilter 4, EnumFilters 5, FindFilterByName 6, ConnectDirect 7,
  Reconnect 8, Disconnect 9, SetDefaultSyncSource 10) + `IGraphBuilder` proper
  (Connect 11, Render 12, **RenderFile 13**, AddSourceFilter 14, SetLogFile 15,
  Abort 16, ShouldOperationContinue 17). `RenderFile(LPCWSTR file, LPCWSTR playlist)`.
  `IMediaControl` = `IDispatch` (0-6) + Run 7, Pause 8, Stop 9, GetState 10.

**The hypothesis, stated as a hypothesis.** WMP's graph build fails at the audio
renderer with `VFW_E_NO_AUDIO_HARDWARE`, either because the DirectSound Renderer
refuses a buffer with the `CTRL*` flags or an extended format that our `16..16`-bit
single-instance pin cannot satisfy, or because WMP has a stale device selection of
its own. **As with Stage 5bj, the probe's value does not depend on that being right**
— it reports where the graph actually stops.

### Stage 5bl build — and the hand-declared vtables validated before shipping

**Both bitnesses are built, because WMP on XP x64 is a 32-bit process.** It loads
the 32-bit `quartz.dll` and `dsound.dll`, so a 64-bit probe exercises a *different
copy* of the DirectShow stack and would not reproduce WMP's environment.
`wmpdiag32.exe` is therefore the primary and `wmpdiag64.exe` the control. Scripts:
`scratchpad\bld64.cmd` and `scratchpad\bld32.cmd`.

**Recipe deltas from the Stage 5bj user-mode recipe, all of which cost a build cycle
to find:**

- `setenv.bat` does **not** define `DDKROOT`. The Stage 5bj script worked because it
  did `set DDK=C:\WinDDK\7600.16385.1` itself first. The new scripts avoid the
  variable entirely and use the literal path with **forward slashes**, which `cl` and
  `link` accept and which sidesteps this project's documented backslash mangling.
- **`mmsystem.h` in the WDK does not pull in `mmreg.h`**, so `WAVE_FORMAT_EXTENSIBLE`
  is undeclared. Defined locally as `0xFFFE` under an `#ifndef`.
- The x86 image is stamped **`/SUBSYSTEM:CONSOLE,5.01`**, not 5.02. 5.01 is plain XP
  and WOW64 accepts it too, so it is the safer stamp for a 32-bit image; 5.02 stays
  correct for the native 64-bit one.
- `/NODEFAULTLIB:libc.lib` alongside `/MT`, as in Stage 5bj.
- A duplicate `case` label is a **hard** compile error in C, and the `DSERR_*` set
  contains two aliases that collide with the generic HRESULTs: `DSERR_INVALIDPARAM`
  *is* `E_INVALIDARG` (`0x80070057`) and `DSERR_UNSUPPORTED` *is* `E_NOTIMPL`
  (`0x80004001`). They are folded into the generic cases and labelled with both
  names. Also worth recording because the first draft got them all wrong:
  **`MAKE_DSHRESULT(n) == 0x88780000 | n`**, so `DSERR_ALLOCATED` is `0x8878000A`,
  `DSERR_CONTROLUNAVAIL` `0x8878001E`, `DSERR_INVALIDCALL` `0x88780032`,
  `DSERR_PRIOLEVELNEEDED` `0x88780046`, `DSERR_BADFORMAT` `0x88780064`,
  `DSERR_NODRIVER` `0x88780078`, `DSERR_ALREADYINITIALIZED` `0x88780082`,
  `DSERR_BUFFERLOST` `0x88780096`, `DSERR_OTHERAPPHASPRIO` `0x887800A0`,
  `DSERR_UNINITIALIZED` `0x887800AA`.

PE verification, both images: `wmpdiag64.exe` = `8664 machine (x64)`, `5.02 subsystem
version`, `3 subsystem (Windows CUI)`; `wmpdiag32.exe` = `14C machine (x86)`, `5.01
subsystem version`, `3 subsystem`. Imports for both are only `DSOUND.dll`,
`ole32.dll`, `ADVAPI32.dll`, `USER32.dll`, `KERNEL32.dll` — **no `MSVCR*`**, so the
static CRT link held.

**The hand-declared COM vtables were validated on the development machine before
being shipped to the target, and this is the part worth reusing.** The risk with
hand-declared vtables is that a wrong slot index silently calls the wrong function
pointer — which either crashes or, worse, returns a plausible-looking wrong answer.
Running `wmpdiag64.exe` locally on Windows 10 exercised every declared slot against a
real `quartz.dll`:

- `IGraphBuilder::RenderFile` at **slot 13** returned `S_OK` and built a working graph
  from `C:\Windows\Media\ding.wav`;
- `IBasicAudio::get_Volume` at slot 8 returned a sane `0`;
- `IMediaControl::Run` (slot 7) returned `S_FALSE`, which is *correct* and documented
  — the graph is still transitioning — and `GetState` (slot 10) then reported state
  `2` (Running), and `Stop` (slot 9) returned `S_OK`.

Every slot behaved as its declaration claimed. **The slot order recorded in the Stage
5bl section is confirmed correct**, so any anomaly seen on the XP target is a
property of the target and not of the probe.

Two incidental findings from the local run, neither a lead but both worth knowing so
they are not misread on the target:

- `CLSID_AudioRender` (the waveOut renderer) returns **`REGDB_E_CLASSNOTREG`** on
  Windows 10 — that filter was removed from modern Windows. On XP it should exist,
  and its absence *there* would be meaningful.
- `CLSID_FilterGraph` correctly returns `E_NOINTERFACE` for `IID_IBaseFilter`; a
  graph is not a filter. That line is a control, not a fault.

**A PowerShell trap that cost a false crash report:** piping the probe through
`Select-Object -First N` terminates the upstream process, so the run exits `-1` with
a truncated log and looks like a crash in whatever section it reached. Capture with
`| Out-String` instead. The same applies to any future probe in this project.

## Stage 5bl RESULT — WMP's exact error reproduced, and the fault is BITNESS-SPECIFIC

**`wmpdiag32.exe` reproduced WMP's failure exactly. `wmpdiag64.exe`, run on the same
machine minutes apart, succeeded completely and was audible.** That single contrast is
the whole finding, and it retroactively reframes Stages 5bj and 5bk.

| | `wmpdiag64.exe` | `wmpdiag32.exe` |
|---|---|---|
| `DirectSoundCreate(NULL)` | `S_OK` | **`80070057` E_INVALIDARG / DSERR_INVALIDPARAM** |
| everything in part 3 | ran, all `S_OK` | **never reached — part 3 aborted at the first call** |
| `IGraphBuilder::RenderFile` | `S_OK` | **`80040256` VFW_E_NO_AUDIO_HARDWARE** |
| audible? | **yes**, user confirmed | **no**, user confirmed |

`VFW_E_NO_AUDIO_HARDWARE` is literally the HRESULT behind WMP's "there is a problem
with your sound device". The probe is not approximating WMP's failure; it *is* WMP's
failure, in a 90 KB console app we control.

### The methodological error this exposes, stated plainly

**Every user-mode probe this project has ever built was compiled WNET/amd64.**
`kstest.exe`, `dstest.exe` (Stage 5bj), `audiodiag.exe` (Stage 5bk) — all x64. Every
"DirectSound is completely healthy" and "the mixer is completely healthy" conclusion
was measured against the **64-bit** copy of the audio stack.

**WMP on XP x64 is a 32-bit process.** It loads `SysWOW64\quartz.dll` and
`SysWOW64\dsound.dll`. Those are a *different copy* of the stack, and they were never
once exercised until `wmpdiag32.exe`. Two full stages were spent measuring a code path
the failing application does not use. The Stage 5bj and 5bk results are not wrong —
64-bit DirectSound and the 64-bit mixer really are healthy — but they were never
evidence about WMP, and they were treated as though they were.

**Rule for the rest of this project: match the probe's bitness to the failing client's
bitness, and say which bitness a result came from every time one is quoted.** A result
without a stated bitness is not a result. Building x86 costs one `sed` and one build
command (see `bld32.cmd`), so there is no excuse for a 64-bit-only measurement.

### Corroborating detail from the 32-bit run

- `QueryInterface(IID_IBasicAudio)` returned `S_OK` but **`get_Volume` returned
  `E_NOTIMPL`**. That is the exact signature of a filter graph with **no audio renderer
  in it**: the Filter Graph Manager always hands out `IBasicAudio`, then delegates to
  the audio renderer, and returns `E_NOTIMPL` when there is none. On the 64-bit run the
  same call returned a sane `0`.
- `IMediaControl::Run` returned `S_OK` and `GetState` reported `2` (Running) — a graph
  with only a source and a splitter, running happily and producing nothing. This is why
  WMP can appear to "play" while silent.
- **Part 1 passed in 32-bit.** `CLSID_DSoundRender` and `CLSID_AudioRender` both created
  and both gave up `IID_IBaseFilter`. So the renderer *filters* are registered and
  instantiable; the DirectSound *device* underneath them is what is missing.
  (`CLSID_AudioRender` returning `REGDB_E_CLASSNOTREG` in the **64-bit** run is normal
  for XP x64 — the waveOut renderer is only registered 32-bit there. Same code, both
  bitnesses, opposite results, and both are correct. Another bitness trap.)

### Part 2 killed the stale-preference theory

I proposed that WMP might hold a stale audio-device selection of its own. It does not.
`HKCU\...\Multimedia\Sound Mapper` reads `Playback = "IDT 92HD89E2 HD Audio (XPDM bac"`
and `Record` the same (truncated at 31 chars, which is just `MAXPNAMELEN`), so the
system's preferred device *is* ours. WMP's `Preferences` key holds no device selection
at all, `Volume = 50`, `Mute = 0`, `Balance = 0`. Nothing stale, nothing muted.
**Theory nine, dead.**

One incidental observation, filed and *not* being chased: the **64-bit** `Drivers32`
key carries `wave`/`wave1`/`wave2`, `midi`/`midi1`/`midi2` and `mixer`/`mixer1`/`mixer2`
all pointing at `wdmaud.drv`, where the 32-bit view has exactly one of each. Triplicate
`waveN` entries make winmm load `wdmaud.drv` three times. It is very likely debris from
the earlier install attempts. It is not the fault here, because the 64-bit side is the
side that **works** — but it should be cleaned up before final delivery, and if a
future symptom involves duplicate device entries, this is where they come from.

### Parked, not chased: 24-bit and 32-bit buffers

In the 64-bit run, `48000/24/2`, `44100/24/2` and `48000/32/2` with the renderer flag
set all returned **`DSERR_CONTROLUNAVAIL` (`8878001E`)**, while every 16-bit variant —
including `WAVEFORMATEXTENSIBLE` and two simultaneous buffers — returned `S_OK`. Our pin
publishes `16..16` only. This is worth remembering because **WMP 11's Options > Devices >
Speakers > Properties has a "Use 24-bit audio" checkbox**, and a ticked box would make
WMP ask for exactly the format that fails. But it is *not* the current lead: the 32-bit
failure happens at `DirectSoundCreate`, which is upstream of any format negotiation.
Fix the bitness problem first; re-test 24-bit only if WMP still fails afterwards.

### The next measurement, and its decision tree

`DirectSoundCreate(NULL, ...)` resolves the default device by asking winmm, via
`waveOutMessage(DRV_QUERYDSOUNDGUID / DRV_QUERYDSOUNDDESC)`, which `wdmaud.drv` answers
from the KS device interface. `DSERR_INVALIDPARAM` rather than `DSERR_NODRIVER` is what
that resolution failing looks like. The question is how far down the 32-bit blindness
goes, and `dstest.c` and `audiodiag.c` already contain every call needed to answer it —
they just needed rebuilding x86. Done: **`dstest32.exe`** and **`audiodiag32.exe`**
(`bld32_dstest.cmd`, `bld32_audiodiag.cmd`, both derived from `bld32.cmd` by `sed`;
both verified `14C machine (x86)`, `5.01 subsystem version`, imports `DSOUND`/`WINMM`/
`ole32`/`KERNEL32` only).

Three outcomes, each pointing somewhere different:

1. **32-bit `waveOut` works AND `DirectSoundEnumerate` lists our device** → winmm and
   dsound can both see it, and only the *default-device resolution* is broken. Look at
   `DRV_QUERYDSOUNDGUID`, and try `DirectSoundCreate` with the explicit enumerated GUID
   — `dstest.c` already does exactly that for every enumerated device.
2. **32-bit `waveOut` works but `DirectSoundEnumerate` count is 0** → winmm sees the
   device and dsound does not. That is dsound's own discovery path: the KS device
   interface it opens directly from user mode, and its DACL. Note that this is the same
   territory as the deleted `FixPdoSecurity` — **which is never to be reinstated**
   (confirmed reproducible bugcheck); if a DACL turns out to be implicated, it must be
   fixed declaratively in the INF, not by rewriting security at runtime.
3. **32-bit `waveOut` also fails** → the whole WOW64 multimedia path is blind to our
   device and the driver or INF is directly implicated. This would be the best outcome
   for the project, because it is the only one of the three that is ours to fix in
   `stwrtxp`.

Also watch `DSCAPS_EMULDRIVER` in the 32-bit `dstest` output: `dstest.c` already labels
it "DirectSound is emulating over waveOut!". If 64-bit is accelerated and 32-bit is
emulating, that difference alone would explain the asymmetry.

## Stage 5bl RESULT, part 2 — the 32-bit split, and it is narrower than any of the three predicted outcomes

`dstest32.exe` and `audiodiag32.exe` came back, and the answer is **none of the three
branches of the decision tree as written**. It is better than all of them, because it
brackets the fault into a single API call.

**32-bit winmm is completely healthy.**

- `waveOutGetNumDevs = 1`, `dev 0 = "IDT 92HD89E2 HD Audio (XPDM bac"`,
  `dwFormats=000BFFFF`, `dwSupport=0000002C` (VOLUME | LRVOLUME | SAMPLEACCURATE).
- `waveOutOpen(WAVE_FORMAT_QUERY)` then `waveOutOpen(WAVE_MAPPER)` then
  `waveOutWrite` — `MMSYSERR_NOERROR` throughout, at 48000/440 Hz and 44100/220 Hz,
  final position `192000 of 192000` and `176400 of 176400`, `WHDR_DONE = yes`.
- The **entire mixer topology is byte-for-byte what the 64-bit run reported**: 2
  destinations, `DST_SPEAKERS` "Speaker" with Master Volume `0..65535 cSteps=192` and
  Master Mute, `SRC_WAVEOUT` "Wave", SW Synth, CD Player, `DST_WAVEIN` +
  `SRC_MICROPHONE`, every call `MMSYSERR_NOERROR`, both player-style
  `mixerGetLineInfo(COMPONENTTYPE, ...)` lookups succeeding.

So outcome 3 is dead: WOW64 is **not** blind to our device.

**32-bit DirectSound enumeration is also healthy — and this is the sharp part.**

```
  [0] Primary Sound Driver
      module = (none)
      guid   = NULL (primary / default device entry)
  [1] IDT 92HD89E2 HD Audio (XPDM backport)
      module = stwrtxp.sys
      guid   = {BD6DD71A-3DEB-11D1-B171-00C04FC20000}
  DirectSoundEnumerate returned -> 00000000  DS_OK
  device count = 2
```

DirectSound **finds our device, names it correctly, and knows its kernel module is
`stwrtxp.sys`**. It is not emulating over waveOut and it is not looking at the wrong
device. Outcome 2 is dead too.

**And then:**

```
  DirectSoundCreate  (NULL == default device)      -> 80070057  DSERR_INVALIDPARAM
  DirectSoundCreate  (enumerated device [1])       -> 80070057  DSERR_INVALIDPARAM
```

**The explicit-GUID create fails identically to the default create.** That kills outcome
1 as stated — this is not a default-device *resolution* problem, because naming the
device explicitly changes nothing.

### Where that leaves the fault

Everything up to and including enumeration works in 32-bit. Everything from
`DirectSoundCreate` onwards fails in 32-bit. The same calls in 64-bit, on the same
machine, against the same driver, minutes apart, all succeed and are audible. The fault
is bracketed inside `dsound.dll`'s device-instantiation path, and it is
**bitness-dependent**, which is the only property that distinguishes the working case
from the failing one.

`{BD6DD71A-3DEB-11D1-B171-00C04FC20000}` was grepped against the whole WDK `inc` tree
and against the driver source: **it is not a WDK constant**, so it is generated at
runtime and carries no meaning on its own. Do not spend time decoding it. What matters
is that it round-tripped through enumeration and still failed on create.

Note also that `DSCAPS` was never reached in 32-bit, so **`DSCAPS_EMULDRIVER` is still
unmeasured on that side**. `dstest.c` labels that flag "DirectSound is emulating over
waveOut!" and it remains worth reading if a 32-bit create ever succeeds.

## Stage 5bm — make the driver testify about the difference

Every remaining question is "what does 32-bit dsound ask the driver that 64-bit dsound
does not, and what do we answer?" The machinery to answer that already exists and is
still compiled in: `HookedMjCreate` logs `PID`, **process name** and the requested file
name; `HookedMjDeviceControl` logs every `IOCTL_KS_PROPERTY` as
`KSPROP #n: PID=... FO=... set=... id=... flags=... extra=... in=... out=... -> status`.
`DC_LOG_FULL_LIMIT` is **40000**, so effectively nothing is dropped, and the condition
is `seq <= LIMIT || !NT_SUCCESS(status)` — **a failing request is logged unconditionally,
however late it happens**.

**The experiment has its own control built in.** Run `dstest.exe` (x64, known to
succeed) and then `dstest32.exe` (x86, known to fail) back to back, then collect a
single `stwrtxp_log.txt`. The log then contains a *successful* KS sequence and a
*failing* one from the same driver minutes apart, and the two processes have distinct
image names, so `HookedMjCreate`'s process-name line maps each PID to its bitness.
Diffing the two sequences is the measurement.

Three things to read out of that diff:

1. **Does the 32-bit sequence reach the driver at all?** If `dstest32.exe` never appears
   in a `MJ_CREATE` line, dsound gave up before touching us and the fault is in
   `dsound.dll` or `sysaudio`, not in `stwrtxp`.
2. **Which request is the last one before the 32-bit run stops**, and did we fail it?
   Any non-`NT_SUCCESS` status is logged even past the limit, so a failure cannot hide.
3. **Which requests appear only in the 64-bit sequence?** A property the 64-bit path
   asks for and the 32-bit path does not is a fork inside dsound, and tells us which
   branch it took.

A hypothesis worth holding loosely while reading it: `ks.sys` thunks 32-bit KS requests
for WOW64, and the thunking is per-structure. Most audio property structures are
pointer-free and pass through unchanged, but if any request we serve arrives with a
32-bit-shaped payload we interpret as 64-bit, we would return a plausible-looking wrong
answer or `STATUS_INVALID_PARAMETER` — and `DSERR_INVALIDPARAM` is exactly what dsound
surfaced. **This is a hypothesis and nothing in the logs yet supports it.** The diff
either shows it or kills it.

**If a DACL turns out to be implicated at any point here, it is fixed declaratively in
the INF. `FixPdoSecurity` is never to be reinstated** — it is a confirmed, reproducible
bugcheck and has been deleted from the source.

### Probe hygiene fixed before the next round (Stage 5bm)

Two defects in the probes were found while reading these logs and are now fixed, because
both would have cost a round trip later:

- **Neither `dstest.c` nor `audiodiag.c` stamped its bitness.** The Stage 5bl RESULT rule
  is that a result without a stated bitness is not a result, and the very next pair of
  logs violated it — the 32-bit provenance of these two had to be *inferred* from the
  fact that `DirectSoundCreate` failed. Both now print
  `build: x64 (64-bit)` / `build: x86 (32-bit)` in the banner, from `#ifdef _WIN64`.
- **`dstest.c` opened its log with a bare relative `fopen("dstest_log.txt", "w")`.** That
  is the exact Stage 5bj defect that made the first `dstest` log unfindable; the fix was
  applied to `audiodiag.c` and `wmpdiag.c` at the time and `dstest.c` never got it. It
  now derives the path from `GetModuleFileNameA`, falls back to `GetTempPathA`, and
  prints the resolved path at the start and the end of the run.

All four probes rebuilt and restaged: `dstest.exe` 71168, `dstest32.exe` 66048,
`audiodiag.exe` 75264, `audiodiag32.exe` 69632. Backups at `dstest.c.bak-5bm` and
`audiodiag.c.bak-5bm`.

## Stage 5bm RESULT — the driver testified, and it named a real bug

The back-to-back `dstest.exe` / `dstest32.exe` run produced exactly the log the plan
asked for: one successful KS sequence and one failing one, from the same driver, minutes
apart, tagged by process name. Three boot sessions in the file; the relevant one starts
at the third `DriverEntry:`.

**Read 1 — where the 32-bit run stops.** Grepping `MJ_CREATE` by process name:

```
PID=1396(dstest.exe)     Name="\Wave"                                    -> 00000000
PID=1396(dstest.exe)     Name="{146F1A80-4791-11D0-A5D6-28DB04C10000}\..." -> 00000000   <== PIN
PID=1384(dstest32.exe)   Name="\Wave"                                    -> 00000000
PID=1384(dstest32.exe)   Name="\Wave"                                    -> 00000000
                         (no pin create, ever)
```

`{146F1A80-4791-11D0-A5D6-28DB04C10000}` is `KSSTRING_Pin`. So **the 32-bit path opens
our filter successfully — twice — and never creates a pin.** It reaches us, it is not
blind, and it is not refused. It walks the topology, opens the filter a second time,
walks it again, disables its events and leaves. Opening the filter twice and redoing the
whole probe is the signature of a **retry loop that gave up**.

**Read 2 — we never fail a 32-bit request.** Every non-success status returned to either
32-bit process (PIDs 1384 and 1640) is:

```
Topology id=3 flags=10000001 extra=1/2/3 in=32 out=0  -> 80000005   (STATUS_BUFFER_OVERFLOW, a size probe)
Audio    id=33 flags=10000001 extra=4     in=32 out=4 -> C0000225   (STATUS_NOT_FOUND)
```

and **the 64-bit run returns the identical statuses at the identical points** (`KSPROP
#2852` is the same `C0000225`). So the fault is not a status code we return. Point 2 of
the Stage 5bm plan is answered: there is no failing request to find.

**Read 3 — the diff, which is where it was hiding.** Normalising each request to
`set/id/flags/extra/in/out/status` and diffing PID 1384 against PID 1396, the *only*
thing the 32-bit run does that the 64-bit run does not, twice:

```
< set=Audio(45FFAAA0) id=4 flags=10000200 extra=00000006 in=32 out=4 -> 00000000
```

`id=4` is `KSPROPERTY_AUDIO_VOLUMELEVEL`, `flags=10000200` is
`KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_TOPOLOGY`. Both runs first ask it with
`out=40`. The 64-bit run then moves on. The 32-bit run **comes back and asks the same
property again with a 4-byte buffer** — the behaviour of a caller that was told the
property has no description, falling back to reading bare access flags.

### The bug

`BasicSupportStepped()` in `mintopo.cpp` gated its description branch on the wrong size.

```c
const ULONG cbFull   = sizeof(KSPROPERTY_DESCRIPTION)      // 40
                     + sizeof(KSPROPERTY_MEMBERSHEADER)    // 16
                     + sizeof(KSPROPERTY_STEPPING_LONG);   // 16   = 72
const ULONG cbHeader = sizeof(KSPROPERTY_DESCRIPTION)
                     + sizeof(KSPROPERTY_MEMBERSHEADER);   //      = 56

if (PropertyRequest->ValueSize >= cbHeader)      // <-- WRONG: 56
```

Sizes verified against `inc/api/ks.h`: `KSPROPERTY_DESCRIPTION` is
`AccessFlags(4) + DescriptionSize(4) + KSIDENTIFIER(24) + MembersListCount(4) +
Reserved(4)` = **40**; `KSPROPERTY_MEMBERSHEADER` = 16; `KSPROPERTY_STEPPING_LONG` =
`SteppingDelta(4) + Reserved(4) + KSPROPERTY_BOUNDS_LONG(8)` = 16.

The documented two-step sizing probe is: **ask with exactly
`sizeof(KSPROPERTY_DESCRIPTION)` = 40 bytes, read `DescriptionSize` out of the reply,
reallocate to that, ask again.** That is precisely what the caller did — `out=40`. Our
threshold of 56 rejected it, so control fell through to the bare-`ULONG` branch, which
wrote 4 bytes of access flags and set `ValueSize = 4`.

**The caller could therefore never learn `DescriptionSize`, and never came back for the
range. Our volume range was never delivered to anybody, in either bitness.**

This is confirmed in the driver's own log, which had been printing it all along and was
misread: `PropertyHandler_Volume: node=0 BASICSUPPORT range -6242304..0 step 49152
size=4` — `size=4` is `ValueSize` *after* the call, i.e. the answer length, and it is 4
on every single volume BASICSUPPORT in the entire log, including the `out=40` ones.

`msvad`'s `PropertyHandlerBasicSupportVolume` (`src/audio/msvad/basetopo.cpp:262`) —
which this function's comment already claimed to be shaped after — gates on
`sizeof(KSPROPERTY_DESCRIPTION)` and returns `sizeof(KSPROPERTY_DESCRIPTION)` in the
partial case. Ours deviated on both.

### Two independent corroborations, both already in the logs

1. **The mute node, which is handled correctly, gets no fallback probe.** `id=13`
   (`KSPROPERTY_AUDIO_MUTE`) goes to `BasicSupportBoolean`, which *does* gate on
   `sizeof(KSPROPERTY_DESCRIPTION)`. In the same 32-bit log its BASICSUPPORT is asked
   with `out=40`, answered completely, and **never re-asked with `out=4`**. Volume is
   the only property that gets the fallback, and volume is the only one with the bug.
   That is a control group inside the same measurement.

2. **The mixer API was reporting a range that is not ours.** `audiodiag` reported Master
   Volume `0..65535 cSteps=192`. 192 = 96 dB / 0.5 dB — the msvad default. *Our*
   advertised range is `-6242304..0` step `49152`, i.e. 95.25 dB in 0.75 dB steps = **127
   steps**. The mixer never saw our range either; it was using a default. Stage 5bk read
   "the mixer is completely healthy" off that number, which was true as far as it went —
   the mixer was healthy, it just was not quoting us. (Caveat: it is not proven that
   `kmixer` derives `cSteps` from the driver range at all. What is certain is that 192
   is not derivable from our numbers.)

### The fix — Stage 5bn

`mintopo.cpp`, `BasicSupportStepped()`: gate the description branch on
`sizeof(KSPROPERTY_DESCRIPTION)`, drop `cbHeader` entirely, and return
`sizeof(KSPROPERTY_DESCRIPTION)` when only the description fits. `cbFull` still gates
the members header and stepped range, exactly as msvad does.

The diagnostic was also changed to log **asked** as well as **answered**:

```c
ULONG asked = PropertyRequest->ValueSize;
...
"step %d asked=%d answered=%d -> %08X"
```

`asked=40 answered=4` was the whole bug and the old log could not show it — it printed
only the post-call value. **Rule: when logging a size that a callee rewrites in place,
log it before and after, or the log cannot show a mismatch.**

`BasicSupportBoolean` was audited and is correct; it is the only other BASICSUPPORT
helper in the source.

Built `chk x64`, clean, `stwrtxp.sys` 76800 bytes. Backup at `mintopo.cpp.bak-5bn`.

### What this does and does not claim

It is **not** proven that this bug is the whole cause of `DSERR_INVALIDPARAM`. What is
proven: it is a real protocol defect, it sits on the exact request where the two
bitnesses diverge, it is the last novel thing the 32-bit client does before abandoning
the device, and the correctly-implemented sibling property produces no such divergence.
If 32-bit `DirectSoundCreate` still fails after this, the next measurement is the same
one again — the log will now show `asked=40 answered=72` and the diff will have moved.

### 32-bit kstest, built at last

`kstest32.exe` (23040 bytes, machine x86, subsystem 5.01) is built and staged. The
earlier hand-rolled `cl` command line was the wrong approach — it fought
`GUID_DEVINTERFACE_*` redefinitions from `inc/api/winioctl.h` and a missing `accctrl.h`.
The WDK `build` system already resolves all of that via `INCLUDES=$(DDK_INC_PATH)`; the
only reason it had not been used was `error U1087: cannot have : and :: dependents for
same target`, which is `build` choking on **spaces in the source path**. Copying
`kstest.c` and `sources` to `C:\kstest32_src` and running `build -cZ -w` from an
`fre x86 WNET` environment builds it unchanged, no source edits, no include surgery.

**Rule: never hand-roll a `cl` command line for something that already has a `sources`
file. Copy the directory somewhere without spaces and use `build`.** This applies to the
x64 tools too.

## Stage 5bo — pindump: three theories killed, one hard new failure found

Stage 5bn fixed a real bug (the `BasicSupportStepped` size threshold) and WMP still
failed, so the next round stopped guessing and measured. `tools/pindump/pindump.c`
was written to dump every KS pin's advertised data ranges and probe
`KSPROPERTY_PIN_DATAINTERSECTION` directly, and — the whole point — it was built in
**both** bitnesses (`pindump32.exe`, `pindump64.exe`), because the failure is
bitness-specific.

Unlike kstest's helper, pindump's `KsSyncIoctl` **returns the byte count**. That is
what made the 360-byte question answerable.

### Result 1 — the 360-byte DATARANGES mystery is SOLVED, and our code is correct

Wave pins 0 and 1 report `360` bytes / `Count=4`, from only two ranges we build:

```
[0] WAVEFORMATEX @ 48000   FormatSize=88  SampleSize=4  MaxCh=2  Bits=16..16
[1] DSOUND       @ 48000   FormatSize=88  ...
[2] WAVEFORMATEX @ 44100   FormatSize=88  ...
[3] DSOUND       @ 44100   FormatSize=88  ...
```

**PortCls auto-generates a `KSDATAFORMAT_SPECIFIER_DSOUND` duplicate of every
WAVEFORMATEX range it is given.** `8 + 4*88 = 360`. `BuildPcmDataRanges` and the
descriptor patch loop in `wavecyclicminiport.cpp` are both correct and need no
change. Bridge pins 2 and 3 reconcile exactly at `72 = 8 + 1*64` (one plain
`KSDATARANGE`, SUBTYPE_ANALOG / SPECIFIER_NONE).

### Result 2 — THEORY DEAD: the missing DSOUND intersection handler

`CMiniportWaveCyclicHda::DataRangeIntersection` returns `STATUS_NOT_IMPLEMENTED`,
and every WDK audio sample (`ac97`, `msvad`, `sb16`) hand-writes DSOUND handling
there. That looked like the bug. It is not:

```
SPECIFIER_DSOUND 2ch/16bit/48000 -> OK, 90 bytes, KSDATAFORMAT_DSOUND, tag=1 ch=2 rate=48000 bits=16
SPECIFIER_DSOUND 2ch/16bit/44100 -> OK, 90 bytes, KSDATAFORMAT_DSOUND, tag=1 ch=2 rate=44100 bits=16
```

PortCls's default intersection handler answers DSOUND correctly on our behalf.
Returning `STATUS_NOT_IMPLEMENTED` is harmless — that return value is precisely
how a miniport asks PortCls to take over. Only the deliberately unadvertised
8-bit/22050 probes fail, with error 1169 (`ERROR_NOT_FOUND`), which is correct
behaviour.

### Result 3 — THEORY DEAD: broken WOW64 KS property access

`pindump32_log.txt` and `pindump64_log.txt` are **identical in content**, including
the struct sizes each build prints for itself:

```
sizeof(KSDATARANGE)=64  sizeof(KSDATARANGE_AUDIO)=88
sizeof(KSMULTIPLE_ITEM)=8  sizeof(KSP_PIN)=32
```

32-bit KS *property* access to our filter is completely healthy. Whatever breaks
WMP is not the property path.

### Result 4 — THE HEADLINE: 32-bit pin creates fail with STATUS_INVALID_BUFFER_SIZE

The user ran `kstest32.exe` and heard **no tone** (`kstest.exe`, 64-bit, is
audible). The driver log says why. kstest32 opens `\wave` fine, then fails **every**
pin create:

```
MJ_CREATE #43: PID=628(kstest32.exe) DesiredAccess=00120116 Options=01000000
               Name="{146F1A80-4791-11D0-A5D6-28DB04C10000}..."
MJ_CREATE #43: PID=628(kstest32.exe) returned status=C00000F2
```

`C00000F2` is `STATUS_INVALID_BUFFER_SIZE`. Tally across the whole log, paired by
PID (never by adjacency — see the Stage 5bm probe-hygiene note):

```
explorer.exe   pin create -> 00000000   x9
winlogon.exe   pin create -> 00000000   x2
kstest32.exe   pin create -> C00000F2   x4
```

A clean bitness split: every 64-bit create succeeds, every 32-bit create fails, with
**identical** `DesiredAccess=00120116` and `Options=01000000`. The only variable left
is the create-parameter buffer itself.

`C00000F2` is **not** ours — nothing in this driver's source returns
`STATUS_INVALID_BUFFER_SIZE` (grepped; the only hit is a comment in `common.cpp`).
It comes out of ks.sys/PortCls before our miniport is ever entered.

### Hypothesis (NOT YET VERIFIED) — the KSPIN_CONNECT size split

`KSPIN_CONNECT` embeds a pointer:

```
KSPIN_INTERFACE Interface;    // 24
KSPIN_MEDIUM    Medium;       // 24
ULONG           PinId;        //  4
HANDLE          PinToHandle;  //  4 (win32) or 8 (win64, after 4 bytes of padding)
KSPRIORITY      Priority;     //  8
```

That is **64 bytes in a 32-bit process and 72 in a 64-bit one**. The layout half of
this is now **confirmed against `inc/api/ks.h:1083`** — `PinToHandle` is a bare
`HANDLE` with no padding compensation. The contrast that makes this interesting:
`KSSTREAM_HEADER` in the same header (ks.h:1978) *does* carry an explicit
`#if _WIN64 ULONG Reserved; #endif` to keep its 32- and 64-bit layouts matched. So KS
demonstrably thought about the 32/64 layout problem for one structure and did not
apply the same treatment to `KSPIN_CONNECT`. `KsCreatePin` sizes
the create buffer as `sizeof(KSPIN_CONNECT) + FormatSize` using its own bitness. If
the kernel side validates against the 64-bit layout with no WOW64 thunk, a 32-bit
request is 8 bytes short — exactly `STATUS_INVALID_BUFFER_SIZE`.

**Do not treat this as established.** It has a strong counter-argument: if it were
unconditionally true, no 32-bit KS client could ever create a pin on XP x64, and
stock drivers on that OS plainly work. So either ks.sys does thunk and something
about *our* filter defeats it, or the real cause is elsewhere. Verify against
`inc/api/ks.h` and find where the size is actually checked before building on it.

### Caveat that must not be lost

**WMP (PID 1828) and dstest32 (PID 1920) never issue a pin create at all.** They
open the filter and stop earlier, right after `set=Audio id=34` (SpeakerGeometry).
So the kstest32 pin-create failure may be a **second, separate bug** rather than the
cause of WMP's `DSERR_INVALIDPARAM`. Do not conflate the two without evidence.

### Note on the create-name case split — and on who really opened the device

`Name="\Wave"` / `"\Topology"` (capital) is the reference string exactly as PortCls
registered it and SetupAPI hands it back. `Name="\wave"` / `"\topology"` (lowercase)
is a literal our own test tools type into the path themselves; the object manager is
case-insensitive so both resolve. `ShareAccess` separates them even more cleanly than
the case does. Across the whole Stage 5bo log, without exception:

```
ShareAccess=0000   capital \Wave    System, winlogon, explorer, wmplayer, dstest32
ShareAccess=0003   lowercase \wave  kstest32, pindump32, pindump64  (+1 explorer,
                                    DesiredAccess=00120089, a read-only shared open
                                    - the tray/control-panel applet, not a client)
```

**The PID on an `MJ_CREATE` line does not tell you which component issued the
create.** A kernel-mode `ZwCreateFile` runs in the requesting thread's context, so
when sysaudio opens our filter on a client's behalf the create is attributed to the
*client's* PID. `wmplayer.exe` and `dstest32.exe` appearing as the opener of a
capital-`\Wave` is therefore exactly what sysaudio-on-their-behalf looks like, not
evidence that they opened it directly.

(An earlier draft of this section was "corrected" to say the opposite — that capital
`\Wave` could not be a sysaudio marker because wmplayer opens it under its own PID.
That correction was wrong, for the reason just given. The original reading stands.)

### Running tally

**Eleven theories dead**: `#718` Audio id=33 on a SUM node; pin instance counts;
CPU_RESOURCES' `NOT_HOST_CPU`; physical connections; interface categories; the
DirectSound rate window; the mixer/n42 range; `wChannels=65535`; WMP's stale device
preference; the missing DSOUND intersection handler; broken WOW64 KS property
access. Plus one real bug found and fixed (5bn) that did not resolve the symptom,
and one hard new failure located (32-bit pin create).

---

## Stage 5bo — the release (free) build, and item p partially done

Cut at the user's request as insurance: most audio already works, so there is now a
keeper build that does not write a log file.

**`Backported Driver/package-release/`** — `stwrtxp.sys` (25,088 bytes, free x64
WNET), `stwrtxp.inf` (byte-identical to the one in `package/`), `stwrtxp.pdb`, and a
`README.txt`. Compiled with zero warnings.

Four diagnostics that were **not** previously gated are now behind `#if (DBG)` in
`adapter.cpp` (backup: `adapter.cpp.bak-5bo`):

1. `ReadTestToneSetting` — free builds never read the `TestTone` registry value, so
   `g_HdaTestTone` stays 0 and no tone can be injected regardless of what is left in
   the registry.
2. The `IRP_MJ_CREATE` / `IRP_MJ_DEVICE_CONTROL` diagnostic dispatch hooks — a free
   build leaves PortCls's own dispatch table untouched.
3. `DumpDeviceSecurity` in `AddDevice`.
4. The `DumpDeviceSecurity` device-list loop in `StartDevice`.

`RelaxPdoSecureOpen` was deliberately **kept** — it is not a diagnostic, it clears
`FILE_DEVICE_SECURE_OPEN` on the PDO and is what makes the device openable at all.

Verified rather than assumed. The free binary contains none of `stwrtxp_log`,
`ZwCreateFile`, `ZwWriteFile`, `DbgPrint`, `TestTone`, `MJ_CREATE` or
`DumpDeviceSecurity`; the checked binary contains all of them. (`strings` is not
installed in this environment — an earlier check using it silently returned "absent"
for everything, including the checked build. Scan the bytes with python instead, and
check UTF-16 as well as ASCII: the log path is a wide string.)

The gates are no-ops for `chk`. Confirmed by rebuilding: the checked `stwrtxp.sys` is
still 76,800 bytes and still contains every diagnostic string. One source tree
therefore serves both flavours — build `fre` or `chk` from `C:\stwrtxp_src` as
needed, and `C:\stwrtxp_rel` (a plain copy, not the junction) is now redundant.

**Item p is still open** for the rest: dial `ulDebugOut` back from `DBG_ALL`, delete
the hook/dump code outright rather than gating it, drop the Stage 5ay/5az DMA scan
and the Stage 5ba blocks, remove the one-off `.reg`/`.vbs` aids and the diagnostic
`.exe`s from `package/`, and clean up `*.bak-5ax` … `*.bak-5bo`.

---

## Stage 5bp — the KSPIN_CONNECT ABI split, measured; a retracted diff; payload logging

### szprobe — the 5bo pin-create hypothesis, now measured instead of reasoned

`tools/szprobe/` is a compile-time ABI probe. It runs on the **dev** machine, not the
XP box, because every number it prints is a property of the compiler's ABI for the
target architecture — a Win10 x64 host reports exactly what XP x64 would. Built both
bitnesses from the WDK; costs no hardware cycle.

| | 64-bit | 32-bit |
|---|---|---|
| `sizeof(void*)` | 8 | 4 |
| **`sizeof(KSPIN_CONNECT)`** | **72** | **64** |
| `offsetof PinToHandle` | 56 | 52 |
| `offsetof Priority` | 64 | 56 |
| `sizeof(KSPIN_INTERFACE)` | 24 | 24 |
| `sizeof(KSPIN_MEDIUM)` | 24 | 24 |
| `sizeof(KSPRIORITY)` | 8 | 8 |
| `sizeof(KSDATAFORMAT)` | 64 | 64 |
| `sizeof(WAVEFORMATEX)` | 18 | 18 |
| `sizeof(KSDATAFORMAT_WAVEFORMATEX)` | 82 | 82 |
| `sizeof(KSDATARANGE)` | 64 | 64 |
| `sizeof(KSDATARANGE_AUDIO)` | 88 | 88 |
| `sizeof(PIN_CONNECT_FORMAT)` (kstest's) | 160 | 152 |
| `offsetof(.Format)` | 72 | 64 |
| **`KsCreatePin` buffer = `sizeof(CONNECT)+FormatSize`** | **154** | **146** |
| Format follows Connect contiguously? | YES | YES |

Two conclusions:

1. **The 64-vs-72 split is real.** A 32-bit `KsCreatePin` sends a 146-byte create
   buffer where the 64-bit one sends 154 — exactly 8 bytes short, which is exactly
   the kind of thing that produces `STATUS_INVALID_BUFFER_SIZE`.
2. **kstest is not at fault.** `offsetof(.Format) == sizeof(KSPIN_CONNECT)` in *both*
   bitnesses, so the format really does follow the connect struct contiguously and
   kstest.c's declaration is correct as written.

`KSPIN_CONNECT` at `inc/api/ks.h:1083` has a bare `HANDLE PinToHandle` with no
padding compensation. The telling contrast is `KSSTREAM_HEADER` at `ks.h:1978`, which
*does* carry `#if _WIN64 ULONG Reserved; #endif` specifically to keep the 32- and
64-bit layouts matched. KS demonstrably thought about this problem for one struct and
not for the other.

**Still unverified** — whether that shortfall is what ks.sys actually rejects. The
counter-argument from 5bo stands unrefuted: if it were unconditionally fatal no
32-bit KS client could create a pin on XP x64, and stock drivers plainly work there.

### RETRACTED — the "seq32 vs seq64" diff and its four bare-ULONG queries

Stage 5bo produced `seq32.txt` / `seq64.txt`, stripped-down request sequences whose
diff looked beautifully tight: the only 32-bit additions were four copies of
`set=Audio id=4 flags=10000200 extra=00000006 in=32 out=4 -> 00000000` (VOLUMELEVEL
BASICSUPPORT on node 6 in the bare-ULONG form), and the only removals were the entire
tail after the SpeakerGeometry SET. **Both halves of that reading are wrong.**

1. **The files are stale.** Both show the two-step BASICSUPPORT probe as `out=40` then
   `out=40`. The current log shows `out=40` then `out=72`. So they were captured
   *before* the Stage 5bn threshold fix. The four extra bare-ULONG queries are the
   5bn bug itself — the client asked with 40, got only `AccessFlags` back with no
   `DescriptionSize`, and retried with 4. That behaviour no longer exists.
2. **The two files are not the same API.** The `C000000D` on
   `set=Connection id=2` that anchors the seq64 tail occurs in this log only under
   PID 472 and PID 2336 — both `winlogon.exe`. seq64 is a **waveOut** playback trace.
   seq32 is a **DirectSound** trace. The "tail 32-bit never reaches" is just the tail
   of a stream that actually started, which a failed `DirectSoundCreate` could never
   reach by definition. The diff compared two different APIs, not two bitnesses.

Nothing was concluded from it, but it cost a round of analysis. **Rule: a filtered
trace file must carry the PID, the process name and the driver build it came from, or
it cannot be diffed against anything later.**

### What the current log does establish

`dstest32.exe` has a short, fully-known call list, which makes its trace attributable
line by line. In the Stage 5bo log (PID 1920):

```
#814-835                 mixer-line enumeration (MUTE, node NAME, VOLUME, 3 events)
MJ_CREATE #37 "\Wave"
#836-847                 DirectSoundCreate(NULL)      -> DSERR_INVALIDPARAM
MJ_CREATE #38 "\Wave"
#848-859                 DirectSoundCreate(guid)      -> DSERR_INVALIDPARAM
#860-862                 event teardown
```

Both create attempts are byte-identical traces:

```
id=33 CPU_RESOURCES node 8 -> OK
id=33 CPU_RESOURCES node 4 -> C0000225   STATUS_NOT_FOUND
id=33 CPU_RESOURCES node 6 -> OK
id=4  VOLUME BASICSUPPORT node 6, out=40 -> OK   (two-step, now correct)
id=4  VOLUME BASICSUPPORT node 6, out=72 -> OK
... repeats ...
id=33 CPU_RESOURCES node 5 -> OK
id=33 CPU_RESOURCES node 8 -> OK
id=3  CHANNEL_CONFIG      SET node 8 -> OK
id=34 SPEAKER_GEOMETRY    SET node 8 -> OK
<nothing further>
```

**Our driver returns no failure at all to the failing client.** A whole-log status
census confirms it — the only non-success codes anywhere are:

| status | count | who |
|---|---|---|
| `00000103` STATUS_PENDING | 903 | PID 4, `IOCTL_KS_WRITE_STREAM` (`002F8013`), normal |
| `80000005` BUFFER_OVERFLOW | 129 | size probes, normal |
| `C0000034` OBJECT_NAME_NOT_FOUND | 12 | PID 4, `Pin id=12` |
| `C0000225` NOT_FOUND | 11 | `Pin id=10`, and CPU_RESOURCES on node 4 |
| `C0000272` | 8 | pindump's deliberately-unadvertised 8-bit/22050 probes |
| `C000000D` INVALID_PARAMETER | 2 | **winlogon only** — `Connection id=2` SET DATAFORMAT |

So `DSERR_INVALIDPARAM` is not a status of ours being propagated. It is dsound
**rejecting a value it read**. Note `0x80070057` is precisely
`HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER)`, which is what `STATUS_INVALID_PARAMETER`
maps to — but no `C000000D` ever reaches PID 1828 or PID 1920, so it is not ours.

Also worth keeping: the hook logs a `KSIOCTL` fallback line for any code it cannot
decode, and the `seq <= DC_LOG_FULL_LIMIT || !NT_SUCCESS(status)` condition means
**every failing ioctl is logged regardless of the cap**. There is no blind spot in
*which requests* get logged.

### THEORY DEAD (properly, this time) — `wChannels = 65535`

`waveOutGetDevCaps` reports `wChannels=0xFFFF`, `wPid=0xFFFF`, and a `dwFormats` that
claims 96 kHz support we never advertise. All four of our data ranges report
`MaximumChannels=2` (pindump), so the `0xFFFF` is wdmaud's own filler, not something
we hand it. And `dstest.c`'s own header records that the **64-bit** run saw the same
`65535` while every DirectSound call returned `DS_OK` and `CreateSoundBuffer`
succeeded at every rate from 8000 to 96000. Same input, working output.

### The real blind spot, now closed — Stage 5bp payload logging

The log recorded request sizes and statuses but **never the bytes we returned**. With
the failure now known to be dsound rejecting a value rather than a status, that was
the one thing the log could not answer.

`adapter.cpp` (backup `adapter.cpp.bak-5bp`) gains `KsHexDumpSafe`, and both the
`KSPROP` and `KSEVENT` lines gain a trailing `[hex...]` of the reply buffer.

Three safety points, all deliberate:

* The output buffer for a METHOD_NEITHER KS ioctl is `Irp->UserBuffer`. That pointer
  is **snapshotted before calling down** and read afterwards. The IRP itself is never
  touched after the call down, because by then it may have been completed and freed.
* For the same reason we do **not** log `Irp->IoStatus.Information`. `out=` remains
  the length the caller *offered*, not the length written — read the hex against the
  two-step size probe, not on its own.
* Reading a user address can take a page fault, and a page fault above APC_LEVEL
  bugchecks where SEH cannot catch it. `KsHexDumpSafe` therefore refuses unless
  `KeGetCurrentIrql() == PASSIVE_LEVEL`, the same guard `LogToFileF` uses. Capped at
  96 bytes, truncation marked with `...`.

Checked build 77,312 bytes (was 76,800), zero warnings.

`tools/dstest/dstest.c` (backup `dstest.c.bak-5bp`) also gains two cheap signals on
the failure path only: `GetLastError()` immediately after the failed
`DirectSoundCreate` (if dsound is wrapping a failed ioctl the thread's last error is
still 87; if it synthesised the HRESULT it will be 0 or something else), and a
`DirectSoundCreate8` attempt, which is a different init path inside `dsound.dll`.

### The experiment this sets up — never actually run before

Every previous comparison had the 64-bit run and the 32-bit run in **different boot
sessions**, or compared different APIs. The pending cycle is: install, reboot,
**delete `C:\stwrtxp_log.txt`**, run `dstest.exe` then `dstest32.exe`. That yields one
log containing both bitnesses of the *same* API against the *same* build, with reply
payloads — the first genuine like-for-like 32-vs-64 DirectSound comparison.

### Open, and genuinely undecided

* Whether the kstest32 pin-create failure (`C00000F2`) and the WMP/dstest32
  `DSERR_INVALIDPARAM` are the same bug or two. WMP and dstest32 never issue a pin
  create at all, so there is still no evidence linking them.
* `KSPROPERTY_AUDIO_CPU_RESOURCES` returns `STATUS_NOT_FOUND` for node 4 but succeeds
  for nodes 5, 6 and 8. That inconsistency is ours and is worth fixing on its own
  merits, but it appears identically in the working 64-bit waveOut path, so it is not
  the discriminator.

---

## Stage 5bp RESULT — the divergence found, and it is above our driver

The Stage 5bp same-boot experiment ran exactly as designed and answered the
question it was built to ask. For the first time the comparison was genuinely
like-for-like: **same boot, same driver build, same API, same log file**,
differing only in the bitness of the calling process. Three artifacts came
back together — `stwrtxp_log.txt` (1,215 lines), `32dstest_log.txt`,
`64dstest_log.txt`.

Because the user deleted the log after boot and before the run, the file has
no `DriverEntry:` line and sequence numbers continue from **#724**. That is
expected, not a corruption.

### PID map for this log (from the MJ_CREATE lines)

| PID | process |
|---|---|
| **2452** | `dstest.exe` (64-bit) — the working control |
| **2876** | `dstest32.exe` (32-bit) — the WMP symptom, reproduced |
| 1800 | `explorer.exe` |

### Finding 1 — `GetLastError` = 234, and what it does NOT mean

`dstest.c` was modified to call `SetLastError(0)` immediately before
`DirectSoundCreate`, so anything non-zero afterwards was set *during* the
call. Both the NULL-device and the enumerated-device attempts reported:

```
-> 80070057  DSERR_INVALIDPARAM
GetLastError after the failure = 234 (0x000000EA)
```

234 is **ERROR_MORE_DATA**, the Win32 mapping of `STATUS_BUFFER_OVERFLOW`.
That looked, for about ten minutes, like the smoking gun: dsound asks us a
sizing question, gets "buffer too small", gives up instead of re-asking.

**It is not.** The only `80000005` results in the 32-bit trace are the three
*normal* `Topology id=3` (component name) size probes, and each one is
immediately followed by a successful re-ask with the right size — `out=24`
("Master Mute"), `out=28` ("Master Volume"), `out=22` ("Mic Volume"). All
three appear identically in the 64-bit trace, which succeeds. A successful
`DeviceIoControl` does not clear the thread's last-error value, so 234 is
simply stale residue from a routine, correctly-handled probe.

**Conclusion: `SysWOW64\dsound.dll` synthesised `DSERR_INVALIDPARAM` itself.
It is not wrapping any ioctl failure of ours.** The "dsound is surfacing a
failed ioctl" reading — which had survived several stages — is dead.

`DirectSoundCreate8` fails identically (same HRESULT, same last error), so
this is not a legacy-vs-DS8 init-path difference either.

### Finding 2 — THE DIVERGENCE, located exactly

Both bitnesses run **byte-identical** through the entire initialisation
sequence. Same property sets, same ids, same buffer sizes, same payloads, same
statuses, in the same order. Both end that identical run with:

```
Audio id=33  CPU_RESOURCES        node 8        -> 00000000  [00 00 00 00]
Audio id=3   CHANNEL_CONFIG   SET node 8        -> 00000000  [03 00 00 00]   KSAUDIO_SPEAKER_STEREO
Audio id=34  STEREO_SPEAKER_GEOMETRY SET node 8 -> 00000000  [14 00 00 00]   GEOMETRY_WIDE (=20)
```

Then the 64-bit run continues, and the 32-bit run stops:

```
#758  PID=2452  set=Pin id=4 (DATAINTERSECTION)  in=216 out=0   -> 80000005   (size probe)
#759  PID=2452  set=Pin id=4 (DATAINTERSECTION)  in=216 out=82  -> 00000000   (gets the format)
      PID=2452  MJ_CREATE #31 = KSSTRING_Pin     <- the pin is instantiated
```

**PID 2876 issues nothing further at all.** It has **zero `set=Pin` requests
of any id** in the entire 1,215-line log. Its four `\Wave` creates (#36–#39 =
`DirectSoundCreate(NULL)`, `DirectSoundCreate8(NULL)`, `DirectSoundCreate(guid)`,
`DirectSoundCreate8(guid)`) each produce an identical 12-request block and
then stop.

The 82 bytes returned at #759 decode as a well-formed
`KSDATAFORMAT_WAVEFORMATEX`: size 0x52 = 82, `KSDATAFORMAT_TYPE_AUDIO`
("auds"), `KSDATAFORMAT_SUBTYPE_PCM`, `KSDATAFORMAT_SPECIFIER_WAVEFORMATEX`,
then `WAVEFORMATEX` = `01 00` PCM, `02 00` 2 channels, `80 BB 00 00` = 48000 Hz,
`00 EE 02 00` = 192000 B/s, `04 00` block align, `10 00` 16 bits, `00 00` cbSize.
Our intersection handler is answering correctly.

### Finding 3 — bitness cannot explain the missing request

The 216-byte input buffer decomposes as:

| part | size | 64-bit | 32-bit |
|---|---|---|---|
| `KSP_PIN` (KSPROPERTY 24 + PinId 4 + Reserved 4) | 32 | 32 | **32** |
| `KSMULTIPLE_ITEM` | 8 | 8 | **8** |
| 2 x `KSDATARANGE_AUDIO` | 176 | 88 each | **88 each** |
| **total** | **216** | 216 | **216** |

None of those structures contains a pointer, so per the szprobe measurements
they are **identical in both bitnesses**. Had 32-bit dsound issued that
request, it would have been byte-for-byte the same request. It did not issue
it.

**Therefore the decision to give up happens entirely in user mode, inside
`SysWOW64\dsound.dll`, before any ioctl is sent.** Nothing our driver returns
on the wire is being rejected — something dsound reads *elsewhere* fails a
check, and it never gets as far as asking us.

### Finding 4 — `C000000D` is definitively not the discriminator

Per-PID status census for this log:

| status | PID 2452 (64-bit, WORKS) | PID 2876 (32-bit, FAILS) |
|---|---|---|
| `00000000` success | 70 | 66 |
| `80000005` BUFFER_OVERFLOW | 5 | 3 |
| `C000000D` INVALID_PARAMETER | **4** | **0** |
| `C0000225` NOT_FOUND | 2 | 4 |

The **working** run is the one that collects four `STATUS_INVALID_PARAMETER`s
and shrugs. All four are the same request:

```
set=Connection(1D58C920) id=2 flags=00000002 extra=FFFFFFFF in=24 out=82 -> C000000D
```

That is a `KSPROPERTY_CONNECTION_DATAFORMAT` **SET** with `PinId = 0xFFFFFFFF`,
which we correctly reject; dsound then proceeds normally. Any theory keyed on
"we return an error dsound doesn't like" now has to explain why the failing
run returns *fewer* errors than the working one.

### Finding 5 — `wChannels = 65535` dead from both sides in a single log

Both dstest builds report the identical bogus
`mid=0001 pid=FFFF channels=65535 support=0000002C formats=000BFFFF`, and the
64-bit run still returns `DS_OK` at 48000/44100/32000/22050/16000/11025/8000/96000,
at 8-bit mono, 8-bit stereo and 16-bit mono, with the play cursor advancing on
both audible tests. Same bogus caps, opposite outcomes. Confirmed dead.

### Bonus — the 5bn BasicSupportStepped fix verified on the wire

The hex dumps show the stepped-range reply decoding correctly:
`AccessFlags = 0x203`, `DescriptionSize = 72`, `KSPROPTYPESETID_General`,
`VT_I4`, one range, min `0xFFA0C000` = **-95.25 dB**, max 0, step 0.75 dB.
Stage 5bn is confirmed good.

### Running tally — twelve theories dead

The eleven previously recorded, plus **"dsound is wrapping a failed ioctl of
ours"**. The `BasicSupportStepped` bare-ULONG lead stays retired as a
stale-data artifact.

### The two failures are probably NOT the same bug

`kstest32`'s pin create fails with `C00000F2` STATUS_INVALID_BUFFER_SIZE,
plausibly the measured 146-vs-154-byte `KSPIN_CONNECT` split. But
**dstest32 never reaches a pin create at all** — it abandons
`DirectSoundCreate` two requests earlier. So the `KSPIN_CONNECT` size split
cannot be what stops DirectSound. Treat them as two separate bugs until
evidence says otherwise. (The `KSPIN_CONNECT` one is still real and still
worth fixing: `inc/api/ks.h:1083` has a bare `HANDLE PinToHandle` with no
`#if _WIN64` padding, while `KSSTREAM_HEADER` at ks.h:1978 *does* carry one —
a telling contrast.)

### THE NEXT THREAD TO PULL — where does 32-bit dsound get the data ranges?

To *build* the `KSPROPERTY_PIN_DATAINTERSECTION` request at #758, dsound needs
the pin's data ranges. Yet **no `Pin id=3` (`KSPROPERTY_PIN_DATARANGES`) query
appears anywhere in this log, from any PID.** (`set=Pin` appears only from
PID 1800 — six probe/re-ask pairs — and PID 2452 — two pairs, #758/#759 and
#1030/#1031. Never from 2876.)

So dsound obtains the ranges from somewhere that never touches our dispatch
table. The candidates, in order of likelihood:

1. **`sysaudio.sys`'s cached graph.** The `Sysaudio` (`CBE3FAA0`) and
   `SysaudioDev` (`0C4F9C81`) property sets are answered by sysaudio itself,
   not by us — they never appear in our log. sysaudio builds and caches a
   device description at graph-build time; whatever 32-bit dsound reads out of
   that cache is what it is rejecting.
2. A **registry-cached** format (the `Drivers32` / media-categories keys).
   Note the outstanding cleanup item: the 64-bit `Drivers32` key still has
   triplicate `wave`/`wave1`/`wave2`, `midi1`/`midi2`, `mixer1`/`mixer2`
   entries. A 32-bit process reads `Wow6432Node\...\Drivers32`, a **different
   key** — worth dumping and comparing, and cheap to check.
3. `kmixer`'s own cached preferred format.

That cached description is now the only remaining place a value could differ
between the two bitnesses, because everything that crosses our dispatch table
has been proven identical.

**Concrete next experiment:** on the XP box, export and diff
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32` against
`HKLM\SOFTWARE\Wow6432Node\Microsoft\Windows NT\CurrentVersion\Drivers32`.
If the WOW64 view is missing or stale, that alone would explain a 32-bit-only
`DSERR_INVALIDPARAM` with no ioctl ever sent. This costs one `reg export` and
no build.

### Separate real bug, noted and parked

`KSPROPERTY_AUDIO_CPU_RESOURCES` returns `STATUS_NOT_FOUND` for **node 4**
while succeeding for nodes 5, 6 and 8. It appears identically in the working
64-bit path, so it is not the discriminator — but it is a genuine
inconsistency in our node table and should be fixed before delivery.

## Stage 5bq — n42 gets a measurement at last, and n49 gets a one-trip capture

The user reported, independently and by ear, that **the Windows volume slider
does nothing audible**. That is not a new bug: it is **n42**, filed in Stage 5ay
and demoted to cosmetic on the assumption that it was a display-units quirk.
The report upgrades it, because "does nothing" is a functional failure, and
the Stage 5ay measurement says why: across a full drag the driver only ever
receives levels spanning **0 to -35230** in 1/65536 dB units — **0.537 dB out
of the 95.25 dB the hardware actually has.** At 0.75 dB per codec step, a full
slider drag moves the attenuator by **zero or one of its 127 steps.**

### The driver side is correct, and that is now established rather than assumed

Re-audited this stage, end to end:

- `hdaverbs.h:85-87` decodes AMP_CAP per the HDA spec: offset = bits 0..6,
  num steps = bits 8..14, step size = bits 16..22.
- `common.cpp:1089` computes `stepLevel = (stepSize + 1) * 16384`, which is
  `(n+1) x 0.25 dB` in 1/65536 dB units. Correct.
- `common.cpp:1093-1095` gives `step = 49152` (0.75 dB), `maximum = 0`,
  `minimum = -6242304` (-95.25 dB) for this codec's offset=127 / numSteps=127.
- `BasicSupportStepped` (`mintopo.cpp:409`) publishes exactly those numbers,
  and the **Stage 5bp hex dump confirms them on the wire**: `AccessFlags=0x203`,
  `DescriptionSize=72`, `KSPROPTYPESETID_General`, `VT_I4`, one stepped range,
  min `0xFFA0C000`, max 0, delta `0xC000`.

So the driver tells the truth, the reply reaches the caller intact, and
**something above us collapses a 95.25 dB range into half a decibel.**

### What was never measured, and why that is the whole problem

Every look at this path so far has been **read-only**. We knew what the driver
publishes and what the driver receives, but never the function connecting
them. The question that decides the fix is a mapping question: *for mixer
value V in 0..65535, what KS level does wdmaud hand us?* Three different
answers imply three different bugs:

| what the sweep shows | the fault is |
|---|---|
| sets accepted, read-back matches, driver sees 0.5 dB spread | wdmaud's dB conversion — look at what else it read from us |
| read-back quantised to a few values | the mixer control's `cSteps`, i.e. our range/step pair as wdmaud rounds it |
| sets rejected, or bounds not 0..65535 | the mixer line wdmaud built from our topology is malformed |

### Stage 5bq — the sweep

`tools/audiodiag/audiodiag.c` already dumped every mixer line, every control,
its `Bounds`, its `cSteps` and its live value — but it **never wrote one**.
Added `SweepVolumeControl`: it finds the VOLUME control on a line, records the
user's current setting, walks
`65535, 49152, 32768, 16384, 8192, 4096, 1024, 256, 64, 0`, reads back after
each write, and **restores the original at the end** so running it cannot
leave the machine silent. It runs on both lines a slider can drive —
`DST_SPEAKERS` (the tray/master slider) and `SRC_WAVEOUT` (a player's own
slider) — because those are different KS nodes in our topology and can fail
differently.

Matched against `PropertyHandler_Volume` SET lines in `C:\stwrtxp_log.txt`,
the ten writes give the mapping directly. **No inference required, and the
sets are in a known order, so correlation is by position.**

Also fixed while in there: `OpenLog` hardcoded `audiodiag_log.txt`, so the
32-bit and 64-bit builds — which now live in the same folder — would each
silently destroy the other's evidence. The log name now follows the exe name.

**Built both bitnesses**, clean (`audiodiag.exe` 78,336 / `audiodiag32.exe`
72,192; the two C4005 warnings are pre-existing and harmless). The 32-bit
build is not padding: **Stage 5bp proved this stack behaves differently for a
32-bit caller, and XP x64's volume control is a 32-bit process.** If the mixer
sweep also diverges by bitness, n42 and n49 are one investigation, not two.
`audiodiag_build32.cmd` is new and builds in place (the old
`bld32_audiodiag.cmd` built out of a scratchpad directory).

### n49 folded into the same trip — `package/regdump.cmd`

Since the machine has to be touched anyway, `regdump.cmd` captures the n49
evidence in the same visit. It **only reads**; it exports and changes nothing.
Eight keys, of which two are the actual experiment and one is the dark horse:

- `Drivers32` **64-bit vs `Wow6432Node`** — the experiment. A 32-bit process is
  silently redirected to the second key. If it is missing, stale, or points
  elsewhere, that alone explains a 32-bit-only `DSERR_INVALIDPARAM` with no
  ioctl ever sent. The 64-bit view is already known to be untidy (triplicate
  `wave`/`wave1`/`wave2`, `midi1`/`midi2`, `mixer1`/`mixer2`).
- `drivers.desc`, both views — same redirection, same reasoning.
- **`HKLM\SYSTEM\CurrentControlSet\Control\MediaResources`** — the dark horse,
  and arguably the best single key in the list. It is where the media APIs
  record which KS filter backs each device for wave, mixer, midi **and
  DirectSound**. It lives under `SYSTEM`, which is **not** redirected — so if
  it is wrong it is wrong for both bitnesses, which would be a different and
  more interesting answer than the redirection theory.
- `MediaCategories`, the media class key, and `DeviceClasses` — context for
  reading the above.

### Cost of this stage

**Zero hardware cycles beyond one run.** No driver rebuild, no reinstall, no
reboot: `package/stwrtxp.sys` is unchanged at **77,312 bytes**, the same
checked 5bp build already installed. Everything added is user-mode.

### What to do with the results

1. In `audiodiag32_log.txt` / `audiodiag_log.txt`, read the `volume sweep`
   blocks. Check `Bounds` is `0..65535` and note `cSteps`.
2. In `stwrtxp_log.txt`, pull the `PropertyHandler_Volume: ... verb=...2 level=N`
   lines for the run and line them up with the ten writes, in order.
3. Diff the 64-bit and 32-bit sweep blocks against each other. **If they
   differ, stop and treat n42 and n49 as one bug.**
4. `drivers32_64bit.reg` vs `drivers32_32bit.reg` — diff them first.

## Stage 5br RESULT — n42 SOLVED. The right channel was never attenuated.

The Stage 5bq sweep ran and it found the bug in one pass. It also **refutes
the Stage 5ay premise this item was filed under**, which is worth stating
plainly because that wrong premise sent three stages looking in the wrong
place.

### wdmaud's dB conversion is perfect

The sweep wrote ten known mixer values and the driver log recorded what
arrived. The mapping is exactly `20 * log10(V / 65535)`, accurate to a
thousandth of a decibel across the whole range:

| mixer value | KS level (1/65536 dB) | dB | 20log10(V/65535) | left amp step | right amp step |
|---|---|---|---|---|---|
| 65535 | 0        |   0.0000 |   0.0000 | 127 | 127 |
| 49152 | -163749  |  -2.4986 |  -2.4986 | 124 | 127 |
| 32768 | -394551  |  -6.0204 |  -6.0205 | 119 | 127 |
| 16384 | -789111  | -12.0409 | -12.0411 | 111 | 127 |
|  8192 | -1183671 | -18.0614 | -18.0617 | 103 | 127 |
|  4096 | -1578231 | -24.0819 | -24.0823 |  95 | 127 |
|  1024 | -2367351 | -36.1229 | -36.1235 |  79 | 127 |
|   256 | -3156471 | -48.1639 | -48.1647 |  63 | 127 |
|    64 | -3945591 | -60.2049 | -60.2059 |  47 | 127 |
|     0 | LONG_MIN, clamped to -6242304 | -95.25 | -inf |   0 | 127 |

So **the full 95.25 dB range does reach the driver**, `LevelToAmpStep` converts
it correctly, and `ProgramOutputAmp` emits well-formed verbs. The Stage 5ay
claim that "a full drag spans only 0..-35230, i.e. 0.54 dB" was simply a
mis-measurement — almost certainly a few pixels of slider travel, not a full
drag. **Delete that belief.**

### The actual bug, visible in one line

```
verb (addr=0 nid=21 verb=3 payload=A07C) -> response 00000000
verb (addr=0 nid=21 verb=3 payload=907F) -> response 00000000
ProgramOutputAmp: wave L=0 R=0 mute=0, master L=-163749 R=0 mute=0 -> amp steps L=124 R=127 mute=0
```

`master L=-163749 R=0` — and `R=0` on **every one of the ten writes**. Payload
`0xA07C` is set-output-amp / set-left / gain 124; `0x907F` is
set-output-amp / set-right / gain 127. The encoding is right, the left channel
tracks the slider perfectly, and **the right channel sits at 0 dB forever.**

Both speakers carry the same content, so with one channel pinned at full
volume the perceived loudness barely moves even when the left channel is
driven to full attenuation. That is precisely "changing the volume does
nothing."

### Root cause: a BASICSUPPORT reply that lied about the channel count

`BasicSupportStepped` (`mintopo.cpp`) was emitting:

```c
members->MembersCount = 1;
members->Flags        = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL;
```

`KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL` (ks.h:227, value `0x2`)
declares **"the members list carries one stepping range per channel."** So for
a KS client the member count *is* the channel count. We set the flag and then
supplied one range for a stereo node. wdmaud read that as a one-channel
volume node, built a `MIXERCONTROL_CONTROLF_UNIFORM` mixer control with a
single value (visible in `audiodiag_log.txt`: `fdwControl=00000001 UNIFORM`,
`value(s): 65535` — one number, where the Mute control on the same line shows
two), and thereafter addressed only `Channel = 0`. Every SET in the log is
`ch=0`. Channel 1 was never written by anyone.

The internal control that makes this certain: the **Mute** control on the same
node pair reports two values, because mute does not go through
`BasicSupportStepped` and therefore gets per-channel treatment by default.
Same line, same node family — wdmaud builds per-channel controls when the
driver describes them honestly.

Note also that `SetVolumeLevel` (`common.cpp:1247`) *already* handled
`Channel < 0` as "all channels". The adapter was ready; nothing ever asked.

### The fix (Stage 5br, built)

`mintopo.cpp`:

- New `NodeChannelCount(Node)` beside `NodeToGainStage`: 1 for
  `NODE_TOPO_MIC_VOLUME` (the capture line genuinely is mono, and the mixer
  agrees: `SRC_MICROPHONE cChannels=1`), 2 otherwise.
- `BasicSupportStepped` takes a `Channels` argument, sizes `cbFull` as
  `40 + 16 + Channels * 16`, sets `MembersCount = Channels`, and writes one
  identical `KSPROPERTY_STEPPING_LONG` per channel in a loop.
- `PropertyHandler_Volume` passes `NodeChannelCount(PropertyRequest->Node)`
  and logs `channels=%u`.

Stereo `DescriptionSize` therefore goes **72 -> 88**. The two-step sizing probe
still works unchanged: the caller asks with 40, reads `DescriptionSize`, and
comes back with 88.

Built checked, clean (no `build.wrn`, no `build.err`). `stwrtxp.sys` is
**77,312 bytes** — coincidentally the same size as the 5bp build, because the
change lands inside existing section padding; confirmed genuinely new by the
presence of the `channels=%u` format string at offset 68786.

### How to confirm it worked

In the next `C:\stwrtxp_log.txt`:

1. `PropertyHandler_Volume: node=3 BASICSUPPORT ... channels=2 asked=88 answered=88`
   (was `asked=72 answered=72`).
2. SET lines appear for **both** `ch=0` and `ch=1`.
3. `ProgramOutputAmp` shows `master L=<n> R=<n>` with the two tracking each
   other, and `amp steps L=<k> R=<k>` likewise.
4. In `audiodiag*_log.txt`, the master control should lose `UNIFORM` and report
   **two** values.

And the user should simply hear the slider work.

### n49 in the same trip: two theories eliminated, none confirmed

`regdump.cmd` came back clean, which is a real result even though it is a
negative one:

- **`Drivers32` 64-bit vs `Wow6432Node`: both healthy.** Each carries
  `wave`/`midi`/`mixer`/`aux` = `wdmaud.drv`. The 32-bit view is in fact the
  *tidier* of the two — it has no duplicates at all, while the 64-bit view
  still has the known `wave1`/`wave2`, `midi1`/`midi2`, `mixer1`/`mixer2`
  clutter. **The WOW64 redirection theory is dead — theory thirteen.**
- **`MediaResources` is clean.** `DirectSound\Device Presence` has
  `VxD=1 WDM=1 Emulated=1`; `Speaker Configuration = 0x00140004`, which decodes
  to `DSSPEAKER_STEREO` (4) with geometry `WIDE` (0x14) and matches byte for
  byte the `STEREO_SPEAKER_GEOMETRY SET [14 00 00 00]` we program. Everything
  else under the key is stock XP DirectSound app-compat shims. **Theory
  fourteen dead.**
- **New and useful:** `audiodiag32.exe` (PID 2784) **successfully created a
  pin** — `MJ_CREATE #40 ... Name="{146F1A80-4791-11D0-A5D6-28DB04C10000}\..."
  returned status=00000000`. So a 32-bit process *can* open a pin on this
  driver; the waveOut path proves it end to end. That kills any remaining
  "32-bit pin create is structurally broken" reading of the `kstest32`
  `KSPIN_CONNECT` 146-vs-154 discrepancy as an explanation for n49, and
  isolates the DirectSound failure even more tightly to a pre-flight check
  inside `SysWOW64\dsound.dll`.
- One loose thread worth a look: 64-bit `GetCaps` reports
  `dwMaxHwMixingAllBuffers = 1` but `dwFreeHwMixingAllBuffers = 0`, i.e. the
  single pin instance reads as already taken. If sysaudio's cached graph tells
  32-bit dsound there are zero free instances, that is a candidate for a
  user-mode give-up with no ioctl — which is exactly n49's signature. Check
  whether the miniport's instance count is being released on close.

**Running tally: fourteen theories dead.**

### VERIFIED ON HARDWARE — all four checks pass

The post-install log (4,878 lines) confirms every criterion:

```
PropertyHandler_Volume: node=3 BASICSUPPORT range -6242304..0 step 49152 channels=2 asked=88 answered=88 -> 00000000
PropertyHandler_Volume: node=5 BASICSUPPORT range -6242304..0 step 49152 channels=1 asked=72 answered=72 -> 00000000
```

Node 3 (master) now negotiates the stereo 88-byte reply; node 5 (mic) stays
correctly mono at 72. The two-step sizing probe handled the change without
complaint.

SET traffic is balanced across channels for the first time:

| verb | ch=0 | ch=1 |
|---|---|---|
| `10000001` GET | 408 | 406 |
| `10000002` SET | **90** | **90** |

Ninety SETs on each channel. Before the fix, channel 1 received exactly zero.

```
ProgramOutputAmp: wave L=0 R=0 mute=0, master L=-127020 R=-127020 mute=0 -> amp steps L=124 R=124 mute=0
```

L and R track. (wdmaud sends ch=0 then ch=1 as two separate SETs, so there is
a single-write transient where the two differ by one step. Harmless — it is
sub-millisecond and 0.75 dB.)

**Measured travel:** levels from `0` down to `-2286636` (-34.9 dB), amp steps
spanning **127 down to 80 — 47 distinct steps.** The user did not drag to the
very bottom, so the remaining 60 dB is available but untested from the UI.
Compare to the old behaviour: at most one step, on one channel.

Log health: 2,182 success, 1,153 `STATUS_PENDING`, 26 `BUFFER_OVERFLOW` (normal
size probes), 22 `INVALID_PARAMETER` — and all 22 of those are the eleven
intentional `ValidateFormat` rejections of 22050 Hz, two log lines each. No
warnings, no errors, no bugcheck.

**n42 is closed.**

Side observation for later: something asks this pin for 22050 Hz eleven times
per session and we refuse it. kmixer should be resampling instead. Not a bug in
the fix, but see the rate-advertisement items.

## Stage 5bs — n49 / WMP. The divergence point, located to the instruction.

The user has scoped the project down to one item: *"The only thing we have left
is Windows Media Player being broken, and once that's fixed, we'll be done!"*

### WMP and 32-bit DirectSound are the same bug — settled

HANDOFF already records that WMP (PID 1828) and dstest32 (PID 1920) both open
the filter and stop right after `set=Audio id=34` (SpeakerGeometry). Stage 5bq's
`audiodiag32` (PID 2784) stops at **exactly the same property**. Same trace,
same stopping point. **Fixing n49 fixes WMP.** There is no separate WMP bug.

(The old "do not conflate the two without evidence" caveat refers to the
`kstest32` `KSPIN_CONNECT` 146-vs-154 size bug versus WMP's `DSERR_INVALIDPARAM`
— *not* to WMP versus DirectSound. Do not re-read it the other way.)

### The divergence, from one boot, one build, one log

Same driver, same moment, 64-bit PID 2580 against 32-bit PID 2784:

| step | 64-bit | 32-bit |
|---|---|---|
| `MJ_CREATE "\Wave"` `0012019F` | #35 ok | #41 ok |
| CPU_RESOURCES node 8 / 4 / 6 | `0`, `C0000225`, `0` | `0`, `C0000225`, `0` |
| Volume BASICSUPPORT node 6, out=40 then 72 | ok, ok (twice) | ok, ok (twice) |
| CPU_RESOURCES node 5, node 8 | `0`, `0` | `0`, `0` |
| CHANNEL_CONFIG SET `03 00 00 00` | `0` | `0` |
| SPEAKER_GEOMETRY SET `14 00 00 00` | `0` | `0` |
| **Pin id=4 DATAINTERSECTION** | `in=216 out=0 -> 80000005`, then `out=82 -> 0` | **never issued** |
| pin create + `NewStream` | success | — |
| outcome | `DirectSoundCreate(NULL) -> 00000000` | `-> 80070057` |

**Eleven consecutive property requests byte-identical, then the 32-bit process
simply stops sending.** The decision is made entirely in user mode inside
`SysWOW64\dsound.dll`, before it ever asks us anything else. Any theory that
requires us to answer something *wrongly* is therefore suspect: every answer we
gave was identical in both bitnesses.

### Cleared this window (do not re-chase)

- **Our data ranges are innocent.** `PinDataRangePcm` has `MaximumChannels = 2`;
  `BuildPcmDataRanges` (`wavecyclicminiport.cpp:624`) correctly patches every
  streaming pin's `DataRangesCount` down to `PinDataRangesPcmCount`. No stale or
  NULL entries. And **nobody ever queries `Pin id=3` (DATARANGES)** from any PID
  — sysaudio cached it at boot, which is why the ranges never appear on the wire.
- **The caps do not come from our pin.** `dwFormats=000BFFFF` advertises
  11.025 / 22.05 / 96 kHz that our pin never offers, so `waveOutGetDevCaps`
  reflects sysaudio's virtual device with kmixer folded in, not us. That also
  explains `wChannels=65535` as a downstream artefact — **dead from both sides**,
  since 64-bit sees the same 65535 and works.
- **`dwFreeHwMixingAllBuffers = 0` is explained; the lead is dead.** GetCaps ran
  *after* 64-bit dsound had created its own pin, and the render sink descriptor
  is `MaxGlobalInstanceCount = 1`. free=0 is correct and self-consistent. Not a
  pin-instance leak.
- **32-bit waveOut works at the DMA level.** PID 2784's stream logged
  `StopEngine: ... 3269 non-zero, peak |sample| = 12000, RMS = 3845`. The only
  32-bit failure anywhere is DirectSound.
- **n50 downgraded.** `CPU_RESOURCES -> C0000225` on virtual node 4 maps to
  topology node 2 (`NODE_TOPO_LINEOUT_MIX`, a summer). Summer nodes deliberately
  keep NULL automation tables, matching `ac97\driver\mintopo.cpp:1178`/`:1233` on
  real hardware, and **64-bit dsound tolerates the identical failure**. Not the
  discriminator; probably correct behaviour.

### Live hypothesis, free to test

DirectSound wants per-channel volume/pan on a stereo primary buffer. Before
Stage 5br our master volume was a `UNIFORM` **one-channel** control
(`audiodiag32_log.txt`: `fdwControl=00000001 UNIFORM ... value(s): 65535` — one
value); after 5br it is a genuine two-channel control. A one-channel volume node
on a stereo device is exactly the class of value 32-bit dsound would reject in
user mode with a synthesised `E_INVALIDARG`. **The 5br build that changes this is
already installed on the user's machine**, so the test costs nothing: open WMP.

### The Stage 5bs probe (built, staged, user-mode only)

`tools/audiodiag/audiodiag.c`, backup `audiodiag.c.bak-5bs`. **No driver
rebuild, no reinstall, no reboot** — it only exercises user-mode APIs.

`ProbeDsoundPlumbing()` walks the layer *underneath* DirectSound, the waveOut
driver messages dsound itself uses before it touches KS:

- `DRV_QUERYDEVICEINTERFACESIZE` / `DRV_QUERYDEVICEINTERFACE` (`DRV_RESERVED+12`
  and `+13`). **Both are tried as the size query and whichever answers wins**, so
  a mis-remembered constant self-corrects instead of costing a hardware trip.
- `CreateFileW` on the returned interface path — literally dsound's next move.
- `DRV_QUERYDSOUNDDESC` (`+21`). **This is the one that matters.**
  `DSDRIVERDESC` is the only structure in this path that **contains pointers**:
  **556 bytes in the 32-bit build, 576 in the 64-bit one.** If the thunk in
  `SysWOW64\wdmaud.drv` mishandles it, 32-bit dsound reads a garbage device
  description and rejects the device without sending anything — precisely n49's
  signature. The probe prints `sizeof` and the decoded contents in both builds.
- `DRV_QUERYDSOUNDIFACE` (`+20`).

`ProbeDirectSoundFull()` replaces `ProbeDirectSoundBrief` (kept under `#if 0`):
`DirectSoundEnumerate` with every GUID printed, `DirectSoundCreate(NULL)`, then
**each enumerated GUID explicitly** (the default-device lookup and the
explicit-GUID path are different code inside dsound.dll), `GetLastError` after
each, `GetCaps`, and then a real `SetCooperativeLevel` + `CreateSoundBuffer` +
`Play` of a 1-second 440 Hz stereo tone — so a success is *audible*, not just a
zero in a log.

Deliberately **not** probed: speculative `KSPROPSETID_Sysaudio` (`CBE3FAA0`)
property ids. Guessing them risks a wasted hardware round-trip.

Built clean both bitnesses (only the pre-existing `C4005 _CRT_SECURE_NO_DEPRECATE`
redefinition warning). Staged in `package/`: `audiodiag.exe` 83,456 bytes,
`audiodiag32.exe` 76,288 bytes; both verified new by the presence of the
`Stage 5bs`, `sizeof(DSDRIVERDESC)` and `LISTEN: 440 Hz` strings.

**Reading the result: run both, then diff the two logs. The first line that
differs is the bug.**

### RESULT - the probe ran, the hypothesis was wrong, five more theories died

The user's verdict: *"Sadly WMP still has the same problem."* The 5br
two-channel volume fix is **not** what n49 was waiting for. The three logs are
preserved in `logs/stage5bs/`.

**The two probe logs are byte-identical except three lines.** 297 lines (64-bit)
against 286 (32-bit); the only differences are the build banner, the
`sizeof` line, and the DirectSound outcome. Everything else - mixer
enumeration, waveOut caps, both waveOut tone tests, the whole plumbing block -
matches exactly.

| theory | evidence | verdict |
|---|---|---|
| `DSDRIVERDESC` WOW64 thunking | `waveOutMessage(0815) DSOUNDDESC -> 8 (NOTSUPPORTED)` in **both** bitnesses - the struct is never exchanged, so its 556-vs-576 size cannot matter | **dead** |
| device-interface path discovery | both resolve the identical 124-wide-char path and both report `CreateFile on it -> OK` | **dead** |
| DirectSound enumeration | both enumerate 2 devices including ours, GUID `{BD6DD71A-3DEB-11D1-B171-00C04FC20000}`, module `stwrtxp.sys` | **dead** |
| per-channel volume (the 5br hypothesis) | 5br is installed, both logs show `value(s): 65535 65535`, WMP still fails | **dead** |
| narrow rate window / no 8-bit | see below | **dead** |
| `Wow6432Node` registry redirection | see below | **dead** |
| `MaxGlobalInstanceCount = 1` pin exhaustion | see below | **dead** |

### WMP is captured in the same log as a WORKING 64-bit DirectSound

First time this has happened. `wmplayer.exe` is **PID 2064**, lines 4943-5007,
39 property entries. Its complete trace ends:

```
CpuResourcesDac node 0        -> ok
CpuResources extra=4          -> C0000225
volume BASICSUPPORT 40 then 88 -> ok
ChannelConfig SET 03 00 00 00 -> ok
SpeakerGeometry SET 14 00 00 00 -> ok
three KSEVENT DISABLE
```

**No pin create. No DATAINTERSECTION. No `NewStream`.** Byte-for-byte the same
shape as `audiodiag32`'s DirectSound attempt in the same boot. WMP and 32-bit
DirectSound are the same bug, now proven in one capture rather than inferred
across two.

PID map for `logs/stage5bs/stwrtxp_log.txt` (a partial capture - starts at
request #2602, no `DriverEntry:` line):

| PID | process | lines | entries |
|---|---|---|---|
| 1312 | sndvol32.exe | 2-4065 | 1231 |
| 4 | System | 108-6341 | 1678 |
| 2372 | explorer.exe | 4066-4934 | 104 |
| **2064** | **wmplayer.exe** | 4943-5007 | **39** |
| 2532 | audiodiag.exe | 5009-5776 | 172 |
| 1184 | audiodiag32.exe | 5778-6418 | 173 |

### The 22050 Hz rejections are exonerated - do not chase them again

All 22 are attributable: **16 from sndvol32.exe, 6 from explorer.exe, zero from
wmplayer, audiodiag or audiodiag32.** They arrive as
`set=Connection(1D58C920) id=2 flags=00000002 in=24 out=82 -> C000000D` right
after a *successful* pin create, and the stream proceeds through `SetState`
regardless. System sounds, resampled by kmixer. Not DirectSound, not WMP.

### The rate window is honest - do not widen it

The 64-bit `GetCaps` reports `secondary rate window = 44100 .. 48000` and
`dwFlags = 00000B5B`, in which `DSCAPS_PRIMARY8BIT` (0x004) and
`DSCAPS_SECONDARY8BIT` (0x400) are both **clear**. That looks damning next to
DirectSound's legacy 22050 Hz / 8-bit / mono default primary format - but the
codec bitmap measured at Stage 5bc is `0x000E05E0`:

| bits | meaning |
|---|---|
| 5, 6, 7, 8, 10 | 44.1, 48, 88.2, 96, 192 kHz |
| 17, 18, 19 | 16, 20, 24 bit |
| **3 clear** | **22.05 kHz genuinely absent** |
| **16 clear** | **8-bit genuinely absent** |

So the advertisement is truthful, `BuildPcmDataRanges` would drop any added low
rate against the bitmap anyway, and a 48 kHz-only HDA codec is entirely
ordinary for XP DirectSound. Widening `PinAdvertisedRateHz` would be a no-op at
best and would advertise untested 88.2/96/192 kHz paths at worst. **Mono is
already accepted** - `ValidateFormat` tests `channels < 1`, and
`DSCAPS_PRIMARYMONO` / `SECONDARYMONO` are both set.

### Registry: `MediaResources` is not WOW64-redirected

The earlier "registry redirection is dead" verdict was reached from `Drivers32`
alone, so this was re-checked properly. `logs/stage5bq/regdump/mediaresources.reg`
shows the key lives at
`HKLM\SYSTEM\CurrentControlSet\Control\MediaResources\DirectSound` - under
**`SYSTEM`**, which WOW64 does **not** redirect. There is no `Wow6432Node` view
for 32-bit dsound to read differently. Contents are unremarkable and correct:

```
Device Presence        VxD=1  WDM=1  Emulated=1
Speaker Configuration  0x00140004  = DSSPEAKER_STEREO | (DSSPEAKER_GEOMETRY_WIDE << 16)
Speaker Type           2
Mixer Defaults         Acceleration=0  SRC Quality=3
```

`0x00140004` matches exactly the `CHANNEL_CONFIG = 3` and
`SPEAKER_GEOMETRY = 0x14` that both bitnesses SET on us. Note
`Mixer Defaults\Acceleration = 0` is **not** the dsound acceleration level -
the 64-bit run took a real hardware pin and its DSCAPS has `DSCAPS_EMULDRIVER`
(0x20) **clear**.

Stale-but-worth-knowing, from the same Stage 5bq dump: the persisted mixer
cache at `...{6994ad04...}\#Wave\Device Parameters\Mixer` has `Line Count = 6`,
and line 0 (`LineId=ffff0000`, the master destination) carries a
`Control Type = 50030001` (VOLUME) with **`Channel Count = 1`** while its
`20010002` (MUTE) says 2, and every source line says 2. That is a snapshot of
the pre-5br one-channel master volume, captured before the fix. It is a cache
wdmaud rewrites, not a cause.

### Pin instance exhaustion is dead

`MaxGlobalInstanceCount = 1` on the render sink pin means only one hardware
DirectSound client can exist at a time, so the ledger was counted across the
whole 6,418-line log. **Peak concurrency 1, and it returns to baseline every
time.** At line 5628 the 64-bit DirectSound pin is created and freed at 5773;
the 32-bit process then opens its two waveOut streams (6066, 6217) and frees
both, and never opens a third. Availability at the two DirectSound attempts was
identical. Nothing leaks.

### Where this leaves n49

The fix cannot be found by changing what we return during that window, because
what we return is provably identical in both bitnesses. It must be one of:

1. something we return **earlier** that sysaudio cached at boot and 32-bit
   dsound reads from the cache (note: nothing ever queries `Pin id=3`
   DATARANGES on the wire, so the ranges reach dsound only via that cache);
2. something we **do not expose at all** that 32-bit dsound requires;
3. a declaration in the INF.

Note also that XP x64's two dsound binaries are **different builds**, not one
build in two modes: `SysWOW64\dsound.dll` is 32-bit XP SP2 code and the 64-bit
one is from the Server 2003 x64 line. Looking for a WOW64 *thunk* may be the
wrong frame entirely - the simpler reading is that the 32-bit build is stricter
about something the 64-bit build does not check.

**Unexplained datum, still open:** `GetLastError=234` (`ERROR_MORE_DATA`) after
**both** 32-bit `DirectSoundCreate` calls, versus `GetLastError=0` on the 64-bit
success. `SetLastError(0)` is called immediately before each, so 234 is set
*during* the call. Our driver returns no `80000005` anywhere in the 32-bit
DirectSound window - the only non-success it returns is `C0000225` on
CpuResources extra=4, which the 64-bit path also gets and tolerates. So the 234
originates outside our driver: sysaudio's cache, ksuser, or a registry read.

**`KSPIN_CONNECT` arithmetic, worth keeping:** `KSPIN_INTERFACE`(24) +
`KSPIN_MEDIUM`(24) + `ULONG PinId`(4) + pad + **`HANDLE PinToHandle`** +
`KSPRIORITY`(8) = **72 on x64, 64 on x86**; plus the 82-byte
`KSDATAFORMAT_WAVEFORMATEX` that is exactly the previously-observed 154 vs 146.
It is a genuine pointer-bearing WOW64 surface, but it is **not** n49's cause,
because 32-bit pin creation demonstrably works (kstest32, and both waveOut
tones in `audiodiag32`).

**Running tally: nineteen theories dead.**

---

## Stage 5bt - the emulation workaround WORKS, and it finishes the localisation

The user was given a zero-code test: Control Panel -> Sounds and Audio Devices ->
Audio -> Sound playback -> Advanced -> Performance -> **Hardware acceleration =
None**. Verdict: *"After doing what you said, Windows Media Player works!"*

Logs preserved in `logs/stage5bt/` (`audiodiag32_log.txt`,
`stwrtxp_log.txt` - 6,341 lines, a partial capture with no `DriverEntry:` line).

### What the workaround proves

`SysWOW64\dsound.dll` is **not** broken, and 32-bit DirectSound works perfectly on
this machine. Forced into waveOut emulation it reports:

| | 32-bit EMULATED (works) | 64-bit HARDWARE (works) | 32-bit HARDWARE (fails) |
|---|---|---|---|
| `DirectSoundCreate(NULL)` | `00000000`, `GetLastError=0` | `00000000`, `GetLastError=0` | `80070057`, `GetLastError=234` |
| `dwFlags` | `0000002F` | `00000B5B` | - |
| secondary rate window | 100 .. 100000 | 44100 .. 48000 | - |
| `dwMaxHwMixingAllBuffers` | 0 | 1 | - |
| `DSCAPS_EMULDRIVER` | set | clear | - |
| device GUID | `{C2AD1800-B243-11CE-A8A4-00AA006C4500}` | `{BD6DD71A-3DEB-11D1-B171-00C04FC20000}` | - |
| module | `"WaveOut 0"` | `stwrtxp.sys` | - |

So the fault is confined to **32-bit dsound's hardware-accelerated KS path** talking
to our filter, at a moment when 64-bit dsound's hardware path succeeds against the
same filter in the same boot.

### WMP working, on the wire

`wmplayer.exe` (PID 1104) goes from **39** driver entries when it fails to **4,437**
when it works. The working path is ordinary kmixer traffic, repeated ~36 times:

```
MJ_CREATE  Name="\Wave"                                        -> ok
Pin id=4 DATAINTERSECTION  in=216 out=0                        -> 80000005
Pin id=4 DATAINTERSECTION  in=216 out=82                       -> ok (48000/2/16)
MJ_CREATE  Name="{146F1A80-...}"   (pin create)
NewStream / SetFormat 48000 Hz 2 ch 16 bit                     -> ok
SetFormat 22050 (or 8000) Hz                                   -> C000000D, refused
Connection id=2 SET  in=24 out=82                              -> C000000D
SetState STOP->ACQUIRE->PAUSE->RUN ... and audio plays
```

**New and worth keeping: the 22050/8000 Hz refusals are kmixer's own optimisation
attempt, and kmixer does not care.** It creates the pin at 48000, asks once whether
it can hand us the source rate directly, takes `C000000D` for an answer and
resamples. Stage 5bc's design is vindicated on the exact path WMP uses. This also
retires the last suspicion that those refusals were ever implicated in n49.

### The decisive alignment: 64-bit success vs 32-bit failure, same log

From `logs/stage5bs/stwrtxp_log.txt`, aligned call for call:

```
64-bit audiodiag (WORKS)                 32-bit wmplayer (FAILS)
------------------------------------     ------------------------------------
MJ_CREATE "\Wave"  (sysaudio proxy)      MJ_CREATE "\Wave"  (sysaudio proxy)
CpuResourcesDac node 0        -> ok      CpuResourcesDac node 0        -> ok
CpuResources extra=4          -> C0000225   CpuResources extra=4       -> C0000225
CpuResources extra=6          -> ok      CpuResources extra=6          -> ok
Volume BASICSUPPORT 40, 88    -> ok      Volume BASICSUPPORT 40, 88    -> ok
CpuResources extra=6          -> ok      CpuResources extra=6          -> ok
Volume BASICSUPPORT 40, 88    -> ok      Volume BASICSUPPORT 40, 88    -> ok
CpuResources extra=5          -> ok      CpuResources extra=5          -> ok
CpuResourcesDac node 0        -> ok      CpuResourcesDac node 0        -> ok
ChannelConfig SET 03          -> ok      ChannelConfig SET 03          -> ok
SpeakerGeometry SET 14        -> ok      SpeakerGeometry SET 14        -> ok
Pin id=4 DATAINTERSECTION     -> ok      << nothing, ever >>
pin create / NewStream        -> ok      three KSEVENT DISABLE (teardown)
```

Two supporting facts nailed down this stage:

1. **`dwMaxHwMixingAllBuffers=1` with `dwFreeHwMixingAllBuffers=0` means the pin
   create happens INSIDE `DirectSoundCreate`.** `GetCaps` runs immediately after
   `DirectSoundCreate` returns and already sees the single instance taken. So the
   two traces above are both `DirectSoundCreate`, not `DirectSoundCreate` versus
   `CreateSoundBuffer`, and the divergence is a single step wide.
2. **The lowercase `Name="\wave"` open is a red herring** and was checked to
   destruction. Census across both logs: every lowercase `\wave` open
   (`ShareAccess=0003`) belongs to a test tool doing its own `CreateFile`
   (`audiodiag`, `audiodiag32`, `pindump`) or to the tray applet; every capital
   `\Wave` open (`ShareAccess=0000`, `Options=01000000`) is sysaudio's kernel-mode
   `ZwCreateFile` in the client's thread context. 64-bit audiodiag does both because
   it probes the path itself first; WMP does only the sysaudio one. Not a difference.

### Where the bug is, stated as narrowly as the evidence allows

**32-bit `DirectSoundCreate` fails in the single user-mode step between
`KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY` SET returning success and
`KSPROPERTY_PIN_DATAINTERSECTION` being issued, and that step sends no IOCTL to this
driver.**

Everything in that step is answered by `sysaudio.sys` from its cached graph - the
pin enumeration properties (`CTYPES`, `COMMUNICATION`, `DATAFLOW`, `CATEGORY`,
`DATARANGES`, `CINSTANCES`) never appear on our wire in EITHER bitness, including
the 64-bit run that then correctly asks for pin **0**. dsound therefore already
knows the pin layout before it opens us, and it learned it from sysaudio.

Note also that the intersection request 64-bit dsound builds carries exactly **two**
`KSDATARANGE_AUDIO`s (`in = 32 + 8 + 2*88 = 216`) - one per rate this pin
advertises. dsound is demonstrably building that request FROM our data ranges, which
it can only have got from sysaudio's cache.

### Do not re-chase these

- **`GetLastError=234`.** Stage 5bp already settled this (theory twelve): it is
  `ERROR_MORE_DATA` left over from a normal, correctly re-asked `Topology id=3`
  name-size probe. dsound **synthesises** `DSERR_INVALIDPARAM`; it does not wrap an
  ioctl error. The Stage 5bs write-up listed 234 as an open datum - that was a
  regression to an already-dead theory.
- **Pin categories.** All four pins carry `&KSCATEGORY_AUDIO`, which is exactly what
  `msvad\simple\wavtable.h` and `ac97\wavtable.h` do for both streaming and bridge
  pins. Checked, conventional, not a lead.
- **`MaxGlobalInstanceCount = 1`.** `ac97` ships `MAX_OUTPUT_STREAMS 1` and 32-bit
  DirectSound works against ac97 on XP. One hardware pin is the normal XP model;
  kmixer does the mixing. Not a lead.
- **A missing DSOUND intersection handler.** Dead since 5bo - PortCls
  auto-generates a `KSDATAFORMAT_SPECIFIER_DSOUND` duplicate of every
  WAVEFORMATEX range (that is the whole 360-byte / `Count=4` answer) and its default
  handler answers DSOUND intersections correctly on our behalf.

### The only route left, and it needs no hardware round trip

Every observable on the wire is identical between the two bitnesses, so no amount of
further driver logging can see the decision. The decision is instructions inside
`SysWOW64\dsound.dll`. XP x64 ships **two different dsound builds** - the 32-bit one
is XP SP2 code, the 64-bit one is from the Server 2003 x64 line - so the 32-bit
build is simply checking something the 64-bit build does not.

**Next step: disassemble it.** `tools/ghidra/ghidra_12.1.3_PUBLIC` and
`tools/jdk21` are already set up in this repo (they were used for `stwrt64.sys`, see
`tools/ghidra_projects/stwrt64_analysis.rep`). What is needed from the XP machine is
one file copy, no rebuild, no reinstall, no reboot:

```
C:\Windows\SysWOW64\dsound.dll     <- the failing 32-bit build
C:\Windows\System32\dsound.dll     <- the working 64-bit build, for contrast
```

Target: `DirectSoundCreate` -> `CDirectSound::Initialize` -> the KS render-filter
init, and specifically the predicate evaluated after the speaker-geometry set and
before the data-intersection ioctl. Whatever that predicate reads, it reads from
sysaudio's cached graph, and what sysaudio cached came from us at boot - so the fix,
when found, is expected to be in what this driver advertises, or declaratively in
the INF.

### Minor, unrelated, noted so it is not lost

In the emulated `audiodiag32_log.txt` the **master** volume sweep reads back
`0 -> 1 1  <-- NOT what was written` while the wave slider correctly gives
`0 -> 0 0`. A one-step floor on the master control only. Cosmetic, and the emulated
path is not the shipping path, but it should be checked once n49 is closed.

### Shippable fallback

Hardware acceleration = None is a real, working configuration for this driver today.
If n49 proves unfixable it is a legitimate thing to document for the user. It is
**not** a thing to write into the INF unattended: the acceleration level found in
`MediaResources\DirectSound\Mixer Defaults` is a machine-wide setting, not a
per-device one, and silently degrading every audio device on the box is not ours to
do.

**Running tally: nineteen theories dead.** (5bt killed no new theories - it removed
the last hiding place instead.)




## Stage 5bu - THE MISSING PIECE IS NOT IN THIS DRIVER: `ksthunk` is gone from this machine

**Finding: the target machine is missing `UpperFilters = ksthunk` on the MEDIA
device class, and a stock Windows XP x64 installation has it.**

`ksthunk.sys` ships with XP x64. Its service display name is, verbatim,
**"Kernel Streaming WOW64 Thunk Service"**. It is an upper filter driver that
attaches to every device in the MEDIA class, and its entire job is to translate
kernel-streaming requests issued by **32-bit** processes into the 64-bit layout
the kernel expects. Microsoft installs it from `wdmaudio.inf`:

```
[ClassInstall32.NT]
Addreg    = ClassInstall.AddReg
CopyFiles = ClassInstall.CopyFiles

[ClassInstall.AddReg]
HKR,,,,%MediaClassName%
HKR,,Installer32,,"MmSys.Cpl,MediaClassInstaller"
HKR,,EnumPropPages32,,"MmSys.Cpl,MediaPropPageProvider"
HKR,,TroubleShooter-0,,"hcp://help/tshoot/tssound.htm"
HKR,,Icon,,"3004"
HKLM,"SYSTEM\CurrentControlSet\Control\Class\{4d36e96c-e325-11ce-bfc1-08002be10318}","UpperFilters",0x00010002,"ksthunk"

[ClassInstall.CopyFiles]
ksthunk.sys

[ClassInstall32.NT.Services]
AddService = ksthunk,,ksthunk_Service_Inst
```

Note where that lives: `[ClassInstall32.NT]`, the **class** installer. It is not
reachable from `WDMAUDIO.Registration`, which is the only section our INF pulls
in via `Include=`/`Needs=`. No device INF supplies this; the class installer does,
once, when the MEDIA class is first registered.

### The control experiment

`VMs/XPx64/` in this project is a clean VMware Windows XP x64 SP2 install with
**no audio hardware of any kind**. Its `SYSTEM` hive was read directly out of the
VMDK (see the recipe below) and it says:

```
ControlSet001\Control\Class\{4d36e96c-e325-11ce-bfc1-08002be10318}
   Class             MEDIA
   (Default)         Sound, video and game controllers
   Installer32       MmSys.Cpl,MediaClassInstaller
   EnumPropPages32   MmSys.Cpl,MediaPropPageProvider
   TroubleShooter-0  hcp://help/tshoot/tssound.htm
   Icon              3004
   UpperFilters      REG_MULTI_SZ { ksthunk }        <-- present

ControlSet001\Services\ksthunk
   Type 1   Start 3   ErrorControl 1   Tag 1
   ImagePath    system32\drivers\ksthunk.sys
   DisplayName  Kernel Streaming WOW64 Thunk Service
   Group        PNP Filter
```

The same VM has **no** `sysaudio`, `kmixer`, `wdmaud`, `portcls`, `swmidi`, `aec`
or `splitter` service at all, and none of those `.sys` files on disk - because it
has no sound card, so `wdmaudio.inf`'s *device* sections never ran. Yet `ksthunk`
is there. That pins down the provenance exactly: the MEDIA class gets registered
during base OS setup for the five legacy pseudo-devices `wave.inf` installs
(`Media Control Devices`, `Video Codecs`, `Audio Codecs`, `Legacy Video Capture
Devices`, `Legacy Audio Drivers`), the class installer runs then, and **every** XP
x64 machine therefore carries `ksthunk` from the day it was installed, sound card
or not.

### What the target machine has

From `logs/stage5bq/regdump/mediaclass.reg`, the same key on the real p6-2133w:

```
Class, (Default), Installer32, EnumPropPages32, TroubleShooter-0, Icon
                                                      ... and no UpperFilters.
```

Six of seven. Every standard value from `ClassInstall.AddReg` is present **except
the filter**. The five `wave.inf` legacy subkeys `0000`-`0004` are there too, in
the same order as the VM, so the class was installed normally and the value was
lost afterwards - it was never simply absent from the start.

That dump is trustworthy on this point: `package/regdump.cmd` uses `reg export`,
which writes `REG_MULTI_SZ` as `hex(7):`, so a multi-string value would have shown
up. (It is worth knowing that there is not a single `hex(7)` anywhere in the eight
exported files, which is unusual enough to have been worth checking.)

### This corrects Stage 5y

Stage 5y asked the user to look for `UpperFilters`/`LowerFilters` on the MEDIA
class and on the device instance, hunting a **leftover hostile filter** from the
original IDT package. The user answered that no such values exist anywhere, and
that was recorded as *"lead 2 (leftover OEM filter driver) RULED OUT"*.

The observation was right and the reading was backwards. There is no hostile
filter - but the absence of `UpperFilters` is not an all-clear, it is the defect.
We were looking straight at it and filed it as good news. **n14 must be reopened
in that light**; the half of it about msvad's topology shape still stands.

### Why this fits every symptom

| observed | goes through 32-bit KS structures? | works? |
|---|---|---|
| 64-bit DirectSound (`dstest.exe`) | no - native | **yes, streams** |
| 32-bit DirectSound (`dstest32.exe`, WMP) | **yes** | **no** |
| WMP with Hardware acceleration = None | no - falls back to waveOut/kmixer, all kernel-mode | **yes** (Stage 5bt) |
| Windows system sounds, `sndvol32`, mixer API | no - `wdmaud.sys` in kernel mode | yes |
| `audiodiag32`'s own KS property calls | yes, but only fixed-size structs | yes |

That last row is the one that makes the theory specific rather than merely
plausible. `KSPROPERTY` (24), `KSP_PIN` (32), `KSNODEPROPERTY` (32),
`KSDATARANGE` (64), `KSDATARANGE_AUDIO` (88) and `KSMULTIPLE_ITEM` (8) are
**identical in both bitnesses** (measured by `szprobe`, Stage 5bs) - so a 32-bit
property GET/SET sails through an unthunked stack untouched, which is exactly what
the logs show: eleven property requests byte-identical in both bitnesses. The
structures that *do* differ are the ones with embedded pointers and handles -
`KSPIN_CONNECT` is **64 bytes on x86 and 72 on x64** - and the first of those the
DirectSound path reaches is **the pin create**. Which is precisely and only where
32-bit stops.

It also explains the thing Stage 5bp found most confusing: the 32-bit run sends us
*nothing* after the speaker-geometry set. Of course it does not. dsound talks to
**`sysaudio.sys`**, not to us; sysaudio is a MEDIA-class device too (`SW\{A7C7A5B0-
5AF3-11D1-9CED-00A024BF0407}`, `Class=MEDIA` in `wdmaudio.inf`), so it is missing
the same filter, and the request dies in sysaudio's stack before sysaudio ever
proxies anything down to us. A failure on the far side of sysaudio is invisible to
every driver trace we have ever taken - which is why nineteen theories died
looking for it in the wrong place.

And `ksthunk.sys` itself corroborates: scanning the binary, it references
`IOCTL_KS_PROPERTY` (`0x2F0003`) seven times and `IOCTL_KS_ENABLE_EVENT` once, and
imports from `ks.sys`. It is doing exactly what the name says.

### The test - no rebuild, no reinstall, one reboot

`package/fixthunk.cmd`. It reports the current state of all three things first
(file on disk, service key, class value), then registers the service with the
stock values above and creates `UpperFilters`.

**It refuses to create the filter registration unless it can see
`ksthunk.sys` on disk**, because an upper filter that cannot load stops every
device in its class from starting (Device Manager code 39). If the file is absent
it first tries to `expand` it out of `%SystemRoot%\Driver Cache\amd64\*.cab`, and
if that fails it changes nothing and says so. It also writes `unfixthunk.cmd`
beside itself, which removes both.

The user must put **Hardware acceleration back to Full** before testing, or the
run proves nothing - Stage 5bt's workaround routes around the very path being
tested.

Delivery note for every `.cmd` sent to the XP machine from here on: **write it
with CRLF line endings.** This repo's tooling produces LF-only files by default
(`regdump.cmd` shipped that way and happened to work, having no labels or
blocks), but `fixthunk.cmd` uses `goto`/`:labels` and parenthesised `if` blocks,
and XP-era `cmd.exe` parses those by byte offset - an LF-only file can jump to
the wrong place. `fixthunk.cmd` was converted before delivery (7,105 -> 7,280
bytes).

### If it works, the fix belongs in the INF - declaratively

Add to `stwrtxp.inf`, mirroring Microsoft's own wording:

```
[StwrtXP.Install.NTamd64]
AddReg = StwrtXP.AddReg, StwrtXP.Thunk.AddReg

[StwrtXP.Thunk.AddReg]
HKLM,"SYSTEM\CurrentControlSet\Control\Class\{4d36e96c-e325-11ce-bfc1-08002be10318}","UpperFilters",0x00010002,"ksthunk"

[StwrtXP.Install.NTamd64.Services]
AddService = ksthunk,,StwrtXP.Thunk.Service
```

`0x00010002` is `FLG_ADDREG_TYPE_MULTI_SZ | FLG_ADDREG_APPEND`, and appending is
duplicate-safe - Microsoft relies on that, since any number of audio INFs may run
on one machine. **Do not make this change until the `.cmd` has proved it on
hardware.** An INF that registers a filter whose driver file is missing would be
strictly worse than the current state.

### Static analysis of `SysWOW64\dsound.dll` - what it settled, and what it did not

The Stage 5bt plan was to disassemble 32-bit dsound. That was done, and although
the `ksthunk` finding supersedes it, three of its results stand and two
earlier claims have to be withdrawn.

**The planned ask is obsolete.** Stage 5bt said "what is needed from the XP machine
is one file copy". It is not - `VMs/XPx64/` supplies both binaries locally. 7-Zip
reads the VMDK -> MBR -> NTFS chain directly with no mounting and no user round
trip:

```
"C:\Program Files\7-Zip\7z.exe" x XPx64.vmdk -r "WINDOWS/SysWOW64/dsound.dll"
```

Extracted into `tools/xpbin/`: `SysWOW64\dsound.dll` (360,960), `System32\dsound.dll`
(651,264), `SysWOW64\wdmaud.drv` (23,552), plus `wdmaudio.inf`, `ks.inf`,
`ksfilter.inf`, `syssetup.inf`, `ksthunk.sys` and the `SYSTEM` hive.

**WITHDRAWN: "XP x64 ships two different dsound builds".** Stage 5bt asserted the
32-bit one is XP SP2 code and the 64-bit one is from the Server 2003 x64 line.
That is wrong. Both are dated **2007-02-18**, both carry PDB name `dsound.pdb`,
both import the same nine DLLs and both contain the same KS GUID set. They are one
source drop. Any divergence between them is therefore **architectural** - pointer
size, WOW64 - not a difference of code lineage. Which is itself a point in favour
of the `ksthunk` explanation.

**WITHDRAWN: "dsound issues the `DATAINTERSECTION`".** 32-bit dsound contains **no
`KSPROPERTY_PIN_DATAINTERSECTION` call site at all** - there are only eight KS
ioctl sites in the whole GUID-referencing set and none takes a variable-length pin
input. The 216-byte `Pin id=4` request seen in the 64-bit trace is issued by
**`sysaudio.sys`** while instantiating the pin on dsound's behalf. So "64-bit
issues the intersection and 32-bit does not" was never a statement about dsound's
code; it was a statement about how far sysaudio got.

**STANDS:** `FUN_73e8722c` is `SetSpeakerConfig`, and it reproduces the observed
wire trace exactly - `CHANNEL_CONFIG` SET mask 3, then `STEREO_SPEAKER_GEOMETRY`
SET 0x14 only when the mask reads back 3. From the *absence* of the `0x23`
(`SURROUND_ENCODE`) set and the *absence* of a readback GET it follows that
`this->0xcc == -1` and `this->0x88 == NULL` in **both** bitnesses, so it returns
`DSERR_CONTROLUNAVAIL` (`0x8878001E`) in both and the caller must be ignoring it.
**The speaker-config step is not the divergence.** That is worth keeping: it was
the obvious suspect and it is cleanly eliminated.

**STANDS, as an open sub-lead:** `FUN_73e850df` is dsound's pin matcher. It
requires `KSPROPERTY_PIN_DATAFLOW` to equal a wanted value, `KSPROPERTY_PIN_COMMUNICATION`
likewise, and then walks the `KSMULTIPLE_ITEM` returned by
`KSPROPERTY_PIN_INTERFACES` (id 5) looking for an entry whose GUID and `Id` match
`{KSINTERFACESETID_Standard, KSINTERFACE_STANDARD_STREAMING}`; `Flags` is not
compared. All four of our `PCPIN_DESCRIPTOR` entries in `wavecyclicminiport.cpp`
declare `InterfacesCount = 0, Interfaces = NULL`. msvad and ac97 do the same, so
PortCls is presumed to substitute the standard interface - **but that has not been
verified**, and it is the only structural requirement dsound's own code has been
shown to impose on us. Worth confirming against the WDK `portcls` sources
regardless of how the `ksthunk` test turns out.

### Tooling added this stage (dev machine only - never ships)

- `tools/xpbin/` - binaries and INFs extracted from `VMs/XPx64/XPx64.vmdk`,
  including the `SYSTEM` hive. Reference material for comparison against the
  target machine. **Must never appear in the INF's `CopyFiles`** - same ruling as
  `tools/szprobe/`.
- `tools/ghidra_scripts/DsoundKsAnchors.java` - byte-searches ten KS/DirectSound
  GUIDs across all initialized blocks, lists every xref with its containing
  function, decompiles the referencers plus one level of callers.
- `tools/ghidra_scripts/CallTreeKs.java` - walks the call graph from a named
  export to a depth limit, annotating each function with the KS ioctl codes and
  DirectSound HRESULTs its instructions reference. **Caveat learned the hard way:
  it cannot follow vtable dispatch.** `DirectSoundCreate` reaches only 53
  functions before the work disappears into `(**(code **)(*this + 0x2c))(...)`,
  so the tree misses the entire KS path. Use `DsoundKsAnchors` for that; use this
  one only for direct call chains.
- `tools/ghidra_projects/dsound_analysis` - `dsound32.dll` (2,101 functions, base
  `0x73e50000`), `dsound64.dll`, `wdmaud32.drv`, all analyzed.
- `scratchpad/hive.py` - a minimal `regf` reader (nk/vk/lf/lh/li/ri cells). This
  is how the VM's `SYSTEM` hive was read without booting the VM, and it is the
  tool that produced the control experiment above. Worth keeping the technique in
  mind: **any question of the form "what does a stock XP x64 have here?" can now
  be answered locally, in seconds, with no user round trip.**

Ghidra headless recipe, for the record:

```
$env:JAVA_HOME = "<tools>\jdk21\jdk-21.0.12.1+1"
& "<tools>\ghidra\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat" `
    "<tools>\ghidra_projects" dsound_analysis -process <file> -noanalysis `
    -scriptPath "<tools>\ghidra_scripts" -postScript <Script>.java <args>
```

### Item-p additions

`tools/xpbin/`, `tools/ghidra_scripts/` and `tools/ghidra_projects/` are
development-machine artefacts and stay out of the shipped package, like
`tools/szprobe/`. `package/fixthunk.cmd` and the `unfixthunk.cmd` it generates are
diagnostics and go with the rest of them - **unless** the fix lands, in which case
the INF change above replaces both and they are deleted. Add `HANDOFF.md.bak-5bu`
to the backup sweep.

**Running tally: still nineteen theories dead - but for the first time since 5bp
the next test is not a guess.** Every earlier theory was "what might this driver be
doing wrong?". This one is a measured difference between the target machine and a
known-good reference installation of the same operating system, in the one
component whose documented purpose is to make 32-bit kernel streaming work on
64-bit Windows, matching a failure that is exclusively 32-bit.


---

## Stage 5bv RESULT - CONFIRMED ON HARDWARE. `ksthunk` was the whole of it. n49 and WMP are CLOSED.

The user ran `package/fixthunk.cmd`, rebooted, put Hardware acceleration back
to **Full**, and opened Windows Media Player. **It plays.** Evidence preserved
in `Backported Driver/logs/stage5bv/driver_stwrtxp_log.txt` (2,625 lines, one
`DriverEntry:` at line 380, checked 5br build, 77,312 bytes).

### The proof is at the exact instruction the theory named

Stage 5bp/5bs localised the divergence to one step: after
`STEREO_SPEAKER_GEOMETRY` SET succeeded, 64-bit issued
`KSPROPERTY_PIN_DATAINTERSECTION` and streamed, and 32-bit sent nothing ever
again. That is now the step that changed, and nothing else had to:

```
1526: KSPROP #741: PID=3044 FO=... set=Pin(8C134960) id=4 ... in=216 out=0  -> 80000005 []
1527: KSPROP #742: PID=3044 FO=... set=Pin(8C134960) id=4 ... in=216 out=82 -> 00000000
      [52 00 .. 80 BB 00 00 .. 02 00 .. 10 00 ..]      <- 48000 Hz, 2 ch, 16 bit
```

`PID=3044` is `wmplayer.exe`. The `in=216` request is the same 216-byte
`KSP_PIN` + `KSMULTIPLE_ITEM` + 2 x `KSDATARANGE_AUDIO` that only the 64-bit
run had ever sent; the `80000005` then `out=82` pair is the normal two-step
size probe. WMP does this **six times** (`#741/#742`, `#763/#764`,
`#785/#786`, `#807/#808`, `#829/#830`, `#1487/#1488`), and every one returns a
well-formed `KSDATAFORMAT_WAVEFORMATEX`.

It then creates pins - `MJ_CREATE` on
`{146F1A80-4791-11D0-A5D6-28DB04C10000}` (`KSNAME_Pin`), six of them, all
`status=00000000` - which is the thing that had never once happened from a
32-bit process in this project's entire history.

And it plays. The stream created at line 1805 reaches
`SetState: PAUSE -> RUN` at 1834, binds `NID 21 ... tag 1, converter format
0011` at 48 kHz, and runs for 637 lines of `KSPROPERTY_AUDIO_POSITION` polls
with the play cursor advancing monotonically (`0x35C8` -> `0x8144` -> ...)
before a clean `RUN -> PAUSE -> ACQUIRE -> STOP` teardown at 2471-2479.

### The log is otherwise clean

Status census over the whole boot:

| status | count | what it is |
|---|---|---|
| `00000103` | 1310 | `STATUS_PENDING` - the streaming ioctls. Normal. |
| `00000000` | 588 | success |
| `80000005` | 58 | `BUFFER_OVERFLOW` - two-step size probes. Normal. |
| `C000000D` | 14 | our own deliberate 22050 Hz / 100 Hz rate refusals (`ValidateFormat`) |
| `C0000225` | 10 | `Pin id=10` and `Audio id=33` `CPU_RESOURCES` on virtual node 4 - n50, tolerated by 64-bit DirectSound too |
| `C0000034` | 6 | `Pin id=12` `PHYSICALCONNECTION` probes from sysaudio on pins that have none |

Zero warnings, zero errors, no bugcheck. Every failing status in the log is
either something this driver refuses on purpose or something the working
64-bit path also collects.

### n49 is closed, and the nineteen dead theories were all looking in the
### right place for the wrong thing

Every one of them asked "what is this driver doing differently for 32-bit
callers?" The answer was **nothing** - the driver was never asked. The
request died in `sysaudio.sys`'s stack, which is itself a MEDIA-class device
missing the same filter, before sysaudio proxied anything down to our
dispatch table. That is why no driver-side trace could ever see it, and why
the eleven property requests ahead of the pin create were byte-identical in
both bitnesses: those structures are the same size in both, so an unthunked
stack passes them through untouched.

**Running tally: nineteen theories dead, and the twentieth was right.**

### The fix is now declarative in the INF

`Backported Driver/package/stwrtxp.inf`, Microsoft's own wording copied from
`wdmaudio.inf`'s `ClassInstall.AddReg` and `ksthunk_Service_Inst`:

```
[StwrtXP.Install.NTamd64]
AddReg      = StwrtXP.AddReg, StwrtXP.Thunk.AddReg

[StwrtXP.Install.NTamd64.Services]
AddService  = ksthunk,,StwrtXP.Thunk.Service

[StwrtXP.Thunk.AddReg]
HKLM,"SYSTEM\CurrentControlSet\Control\Class\{4d36e96c-e325-11ce-bfc1-08002be10318}","UpperFilters",0x00010002,"ksthunk"

[StwrtXP.Thunk.Service]
DisplayName    = %ksthunk.SvcDesc%
ServiceType    = 1
StartType      = 3
ErrorControl   = 1
ServiceBinary  = %10%\system32\drivers\ksthunk.sys
LoadOrderGroup = PNP Filter
```

`0x00010002` = `FLG_ADDREG_TYPE_MULTI_SZ | FLG_ADDREG_APPEND`, and APPEND does
not duplicate a string already present in the value - which is what makes it
safe alongside any other audio INF on the same machine. No `CopyFiles` for
`ksthunk.sys`: it is an OS file, present even on installs with no audio
hardware, and copying it would be both wrong and unlicensed.

**The one residual risk, stated plainly:** if a machine somehow lacked
`ksthunk.sys`, this INF would register an upper filter whose driver cannot
load, and every device in the MEDIA class would fail to start with Device
Manager code 39. Both control samples had the file (the clean VM and the
target machine), and `fixthunk.cmd` refuses to proceed without it, so the
recovery path is documented in the INF comment and in
`package-release/README.txt`.

### Two INF discoveries worth remembering

1. **`Backported Driver/inf/stwrtxp.inf` was FIFTEEN LINES STALE.** It had
   never received the Stage 5ax `SubClasses = "wave,midi,mixer"` fix, the
   `midi`/`mixer` `wdmaud.drv` entries, or the `MediaCategories` name
   registration - it still said `SubClasses = "wave"`, which is the exact
   defect Stage 5ax diagnosed as the cause of "no audio device" in the
   Control Panel applet. `package/stwrtxp.inf` was the live copy all along.
   Both are now identical (`package` -> `inf`). **Anyone installing from
   `inf/` between Stage 5ax and Stage 5bv would have reproduced a solved
   bug.** Do not let the two diverge again; better, delete `inf/` in the
   item-p pass and keep one copy.

2. **Batch files delivered to the XP machine must be CRLF.** This repo's
   tooling writes LF-only by default. `regdump.cmd` shipped that way and
   happened to work because it has no labels or blocks; `fixthunk.cmd` has
   both `goto`/`:labels` and parenthesised `if` blocks, and XP-era `cmd.exe`
   seeks by byte offset, so an LF-only file can jump to the wrong place. It
   was converted before delivery (7,105 -> 7,280 bytes).

### The release build

`fre` build made from the current source (which includes the 5br per-channel
volume fix): **25,088 bytes**, MD5 `0672d29c73a37fa190a07d7c3643c33c`. That is
the same size as the 9 September release build but **not the same file** (MD5
`94a2f1327db8fd2aa42667494dabe595`) - section padding coincidence, same as the
77,312-byte collision in 5br. Verified free of the trace apparatus by byte
scan: `SetFormat:` and `MJ_CREATE` are absent from the `fre` image and present
in the `chk` one, because `DriverEntry`'s dispatch hooks and every `DOUT` sit
inside `#if (DBG)`.

`Backported Driver/package-release/` now holds that binary, its `.pdb`, the
new INF, a rewritten `README.txt`, and a copy of `fixthunk.cmd` for state
inspection and recovery only.

### What is left

Item p (the cleanup pass) and delivery. The functional work is done.

---

## Stage 5bw - item p, the cleanup pass. DONE.

No functional change. Every edit in this stage removes something or moves it;
nothing in the driver's behaviour is different, and the free build that ships
is byte-identical in behaviour to the one Stage 5bv confirmed on hardware.

### The one real defect this pass found

**The previous release build physically contained the test-tone table.** Item
p's instruction that "a shipped driver must never contain a code path that
replaces the user's audio with a tone" turned out to describe a defect, not
just a principle.

The Stage 5ba tone apparatus was in four pieces, and only one of them was
gated:

| piece | where | gated? |
|---|---|---|
| `ReadTestToneSetting` call | `DriverEntry` | `#if (DBG)` |
| `ULONG g_HdaTestTone = 0;` | `dmachannel.cpp` | **no** |
| `g_ToneTable[64]` sine | `dmachannel.cpp` | **no** |
| `if (g_HdaTestTone && ...)` substitution | `CopyTo` | **no** |

So the `fre` build carried the table and the branch. A byte scan of
`package-release/stwrtxp.sys` as it stood before this stage finds the sine's
first eight samples (`0, 1606, 3196, 4756, 6270, 7723, 9102, 10394`) in the
binary.

It could not actually fire: with the registry read compiled out, nothing ever
set `g_HdaTestTone`, so the branch was dead. The old release README's claim
that no tone could be injected was therefore true in effect - but its stated
reason, that the whole thing was behind `#if (DBG)`, was wrong, and a driver
has no business shipping that path at all. It is now deleted from the source,
so it is in neither build. The release README has been corrected to say this
rather than repeat the old claim.

### What was removed from the source

`cleanup5bw.py` backed up six files as `src/*.bak-5bw` and excised eleven
blocks:

- `debug.h` - `ulDebugOut` from `DBG_ALL` back to `DBG_DEFAULT`.
- `adapter.cpp` - `#include <ntstrsafe.h>`; the entire contiguous run from the
  `extern "C"` Rtl security declarations through `DumpDeviceSecurity`, which
  is both dispatch hooks (`HookedMjCreate`, `HookedMjDeviceControl`) and every
  helper they pulled in (`ReadKsPropertySafe`, `KsHexDumpSafe`,
  `KsPropertySetName`, `KsEventSetName`, `ResolveProcessNameHelper`,
  `CurrentProcessName`, the ACE typedefs, the sequence counters); the Stage
  5ba `ReadTestToneSetting` block; both `#if (DBG)` blocks in `DriverEntry`;
  and the AddDevice and StartDevice security dumps.
- `wavecyclicstream.cpp` - the Stage 5ay `StopEngine` DMA-buffer scan.
- `dmachannel.cpp` / `dmachannel.h` - the whole tone apparatus above.

`DriverEntry` is now four lines and `CopyTo` is a single `RtlCopyMemory`.
Zero residual references to any removed symbol.

**`RelaxPdoSecureOpen` is untouched and still ungated**, as item p requires -
clearing `FILE_DEVICE_SECURE_OPEN` on the PDO is the single thing that makes
the device openable at all. `FixPdoSecurity` remains deleted and must never
come back; if a DACL is ever implicated again the fix belongs in the INF,
declaratively.

The trace apparatus is about 400 lines of SEH-wrapped kernel code that was
expensive to get right, so it was not simply discarded: `src/*.bak-5bw` are
kept as the single restore point for it. All 54 older `src/*.bak-*` files and
21 of the 22 `HANDOFF.md.bak-*` files were deleted.

### Builds

Both rebuilt from the cleaned source, both with **zero compiler warnings**
(`grep -ci "warning C[0-9]"` returns 0 on both build logs and no `.wrn` files
exist - the "1 Warning" in BUILD's own summary line is the environment banner,
"x64 Native compiling isn't supported. Using cross compilers.").

| | before | after | MD5 |
|---|---|---|---|
| `chk` | 77,312 | **65,536** | `9ffb38f221b9c2b3c815441a95d021cd` |
| `fre` | 25,088 | **25,088** | `637eaa9056f05f077fa7caea9617c18a` |

The `fre` size is unchanged because almost everything removed was already
being compiled out of it. That makes **three consecutive release builds at
exactly 25,088 bytes**, all different files - the size is a section-padding
artefact and is useless for telling builds apart. Use the MD5.

A byte scan confirms the new `fre` binary contains no tone table, no
`ZwCreateFile` import, no log path and none of the trace strings; the new
`chk` binary has the log path and `ZwCreateFile` and none of the removed
apparatus.

### Packaging

`package/` was swept of fourteen diagnostic files (all ten `.exe` probes, the
two `quicktest-5ax` aids and the two `testtone-*.reg` files) and now holds
only `stwrtxp.sys` (chk), `stwrtxp.pdb`, `stwrtxp.inf`, `fixthunk.cmd` and a
new `README.txt`. `regdump.cmd` was moved rather than deleted - it is
diagnostic *source*, the same category as the preserved tool sources.

**`Backported Driver/inf/` is gone.** It was the redundant third copy of the
INF and the stale trap that cost a day in Stage 5bv. The INF is now
maintained in `package/` and copied to `package-release/`; both READMEs say
so, and both copies are currently MD5 `27305dadcfe018be3f377730b8f610d5`.

**The stale `IDT Audio Backported Driver (Near Release).zip` is gone too**, and
this is worth recording: it was dated 9 September and was unfit on two counts
at once - its INF was the 8,375-byte pre-ksthunk version with no filter
registration (so 32-bit callers, i.e. Windows Media Player, would have failed
exactly as they did before Stage 5bu), and its `.sys` was the tone-carrying
build. It has been replaced by **`IDT Audio Backported Driver (Release).zip`**,
136,322 bytes, built from the current `package-release/` and verified after
zipping: tone table absent, `ZwCreateFile` absent, log path absent, ksthunk
registration present.

### Tool preservation - and a duplication I had to undo

Item p says to keep the tool *sources* while deleting the binaries. Partway
through this pass I preserved them into a new top-level `tools/` tree, which
was a mistake: `Backported Driver/tools/` already held them, and held better
copies. Its `audiodiag.c` is 49,269 bytes and carries `ProbeDsoundPlumbing`
and the Stage 5bq/5br/5bs work; the copy I had taken from the session
scratchpad was a 28,420-byte snapshot predating all of it. Had I let that
stand, the newer source would have been the one that looked redundant.

Everything is now consolidated into **`Backported Driver/tools/`** (next to
the driver the tools interrogate), with sources, build scripts, `sources`
files, `patch_kstest.py`, `msvad_abtest/`, `regdump.cmd`, the wmpdiag sample
log and a new `README.txt`. All `.exe`, `.obj`, `.pdb`, `objfre_*`, `.bak-*`
and build logs were deleted from it.

The repository-level `tools/` now holds only the toolchain - `ghidra/`,
`ghidra_projects/`, `ghidra_scripts/`, `jdk21/`, `installers/`, `xpbin/` -
and has its own `README.txt` recording what each is worth. Two notes from it:
`xpbin/WINDOWS/inf/wdmaudio.inf` is the most important file in there, being
where the ksthunk registration wording came from; and `installers/prod key.txt`
is the target machine's XP product key, so that directory must not be
published.

The ten `.cmd` files in the tool tree were repointed from the dead session
scratchpad to `cd /d "%~dp0"` and converted to CRLF. Flag sets were left
byte-for-byte as they were actually run.

**Verified, not assumed:** `audiodiag_build.cmd`, `audiodiag_build32.cmd` and
`dstest_build.cmd` were run in place from the repo, spaces in the path and
all, and produced `audiodiag.exe` at 83,456 bytes, `audiodiag32.exe` at 76,288
and `dstest.exe` at 72,192 - byte-for-byte the sizes of the binaries that had
just been deleted. The outputs were then deleted again.

### Two build facts worth keeping

Both were diagnosed while trying to rebuild kstest, and both are now in
`Backported Driver/tools/README.txt`:

1. **Spell WDK include paths with backslashes.** The terse `bld32_*.cmd`
   scripts use `/IC:/WinDDK/7600.16385.1\inc\api`. That reaches the same
   directory as the backslash spelling already on `%INCLUDE%`, but MSVC tracks
   already-included headers *by path string*, so it does not recognise the two
   spellings as the same file. Any header carrying `DEFINE_GUID` then gets
   processed twice and every GUID in it becomes a C2374 redefinition.
2. **`kstest`'s x86 `build` failure was `U1087`, not a compile error** - the
   spaces-in-path rule, because it was run in place here. The x64 build of the
   same tool succeeded only because it ran from `C:\stwrtxp_kstest`. The
   `sources` + junction route works for both bitnesses and is how the surviving
   binaries were produced. (Separately, `bld32_kstest.cmd` genuinely cannot
   work as written: `kstest.c` defines `INITGUID` and includes both
   `<setupapi.h>` and `<winioctl.h>`, and winioctl.h keeps its `DEFINE_GUID`
   block outside its include guard deliberately. It also needs `inc\\ddk` for
   `accctrl.h`. Left alone rather than changed untested - nothing needs kstest
   any more.)

### What is left

Nothing required. Two optional items, both the user's call:

- **End-to-end INF validation. PARTLY DONE in Stage 5bx, and the remaining
  gap is narrow.** `package-release/` was installed from the INF, the machine
  was rebooted, and WMP plays - so the install path is exercised and the
  shipping build is confirmed. What that does *not* isolate is whether the
  INF's `[StwrtXP.Thunk.AddReg]` stanza can supply the registration **from
  scratch**, because `fixthunk.cmd` had already written it and it was present
  throughout. Closing that last gap means `unfixthunk.cmd`, reboot, install
  from `package-release/`, reboot, check WMP. Fully recoverable either way:
  `fixthunk.cmd` puts it back. Low value now - the INF wording is copied
  verbatim from `wdmaudio.inf` and `FLG_ADDREG_APPEND` on an absent value is
  the ordinary create case - but it is the one claim in this project resting
  on reading rather than on hardware.

  **The user believes this is already closed**, recalling having run
  `unfixthunk.cmd` when installing the logless driver - which would mean the
  registration `fixthunk.cmd` found in Stage 5bx had to have been created by
  the INF. If that is right, the item is done. It could not be corroborated
  from this file, and two things point the other way: `package-release/` was
  cut long before Stage 5bu, which is the stage that first *wrote*
  `fixthunk.cmd`, so `unfixthunk.cmd` did not exist when the insurance build
  was installed; and the only `fixthunk` run on record is the successful
  Stage 5bv one, which wrote the value. A likely source of the recollection
  is `package-release/README.txt`, which tells the reader to run
  `unfixthunk.cmd` **if** the device ever shows Device Manager code 39 -
  conditional recovery advice, read at exactly the moment described.

  **The Stage 5bx `fixthunk` output cannot settle it.** "unfixthunk ran and
  the INF recreated the value" and "unfixthunk never ran and the Stage 5bv
  value persisted" produce byte-identical reports. Only the user knows which
  occurred. Treat this item as *probably* closed and *not* demonstrated, and
  do not let it be written up as confirmed without the reversible test above.
  Nothing about the working install depends on the answer.
- **Machine-state tidying only:** the 64-bit `Drivers32` key carries
  triplicate `wave`/`wave1`/`wave2`, `midi1`/`midi2` and `mixer1`/`mixer2`
  entries. The 32-bit view is clean. Cosmetic; not an INF change.

`waveOutGetDevCaps` reporting `wChannels = 65535` and n50
(`KSPROPERTY_AUDIO_CPU_RESOURCES` returning `C0000225` on virtual node 4)
remain closed-in-practice and documented; the working 64-bit path collects the
identical n50 refusal, so it is not a fault.

---

## Stage 5bx - the WMP "regression" was a missing reboot. CONFIRMED ON THE SHIPPING BUILD.

**Report.** After the item-p release package was installed from the INF,
Windows Media Player errored again. `fixthunk.cmd` was run and reported
everything already correct: `ksthunk.sys` on disk (24,192 bytes, 02/18/2007),
the service registered with all seven values right, and the MEDIA class
`UpperFilters` holding a single `REG_MULTI_SZ` entry, `ksthunk`.

**Cause: the machine was not rebooted after the install.** PnP reads a class
`UpperFilters` value when it **builds** a device stack. Updating a driver
through Device Manager restarts the device but does not reliably rebuild the
stack with a newly attached filter, so the registration can be perfect while
`ksthunk` is not in the stack. The symptom of that state is precisely the
symptom `ksthunk` was added to cure - 64-bit audio fine, system sounds fine,
WMP reporting "There was a problem with your sound device" - which is what
made it read as a regression. A reboot fixed it. **WMP now plays on the free
build.**

This is documented in `package-release/README.txt` under Install, in capitals,
because it will catch anyone who installs this driver. `fixthunk.cmd` also now
volunteers it (below).

### What was ruled out first, and why it was worth doing

Three suspects were eliminated by inspection before the reboot question was
even asked. All three findings stand on their own and are worth keeping.

1. **The item-p source cleanup is innocent.** A `find start -> find end`
   excision can silently swallow functional code and still compile. Diffing
   the `.bak-5bw` backups function-by-function: the only symbols removed from
   `adapter.cpp` are `CurrentProcessName`, `DumpDeviceSecurity`,
   `KsEventSetName`, `KsPropertySetName`, `ReadTestToneSetting` and
   `ResolveProcessNameHelper`, every one diagnostic. **Zero** function-level
   removals from `wavecyclicstream.cpp`, `dmachannel.cpp` or `dmachannel.h`;
   the only excision there is the read-only DMA scan loop.

2. **The INF is innocent**, including the part that looks alarming. The
   `[StwrtXP.Install.NTamd64.HW]` `Security` stanza carries a comment saying
   the placement "has never been tried". That comment is stale - it was
   written when the section was added in Stage 5aj, and Stage 5aj's own
   hardware result (the `S-1-1-0` / `0x001F01FF` PDO reading recorded earlier
   in this file) proves it works. It predates the Stage 5bv success. **Do not
   re-suspect that section; read its comment as history, not as status.**

3. **The free build is functionally identical to the checked build**, and
   this was established rather than assumed:

   | check | result |
   |---|---|
   | `#if (DBG)` blocks in the whole project | **4** - `common.cpp:70` (`LogToFileF`), `debug.h:25/36/45` (the variable, its `extern`, the prototype plus the `DOUT`/`BREAK` macros). All logging. Nothing functional is gated. |
   | `ASSERT` sites | **6**, in `common.cpp` (2), `dmachannel.cpp`, `mintopo.cpp`, `wavecyclicminiport.cpp`, `wavecyclicstream.cpp`. **None** has a call inside the parentheses - the classic trap where a free build silently skips real work. |
   | raw MMIO exposed to `/O2` | **none**. Every hardware access goes through the `HDAUDIO_BUS_INTERFACE` function pointers or `READ_REGISTER_ULONG` (`wavecyclicstream.cpp:724`). The only `volatile` in the project is `m_EngineHandles`, used with `InterlockedCompareExchangePointer`, which does not need it. |

### Record correction - a real error in what was shipped

`package-release/README.txt` listed Windows Media Player as working. **That
was never verified on that build.** The Stage 5bv proof is a *driver log*, and
only the checked build can write one, so every WMP confirmation in this
project up to Stage 5bx was obtained on `chk`. The claim was carried across to
`fre` on the strength of the audit in the table above.

The audit is correct. It is still not a test. The README now says so
explicitly and reports the direct confirmation instead. **The general lesson,
which this project has now hit twice: a result obtained on the diagnostic
build is a result about the diagnostic build.**

### `fixthunk.cmd` - fixed a defect it exposed in itself

The script conflated two different questions - "does `UpperFilters` exist?"
and "is `ksthunk` in it?" - and on finding the value present it stopped with
"Send the value above back and it will be handled by hand." The correct answer
was "nothing to do."

It now tests for `ksthunk` **inside** the `REG_MULTI_SZ` and reports the
already-correct state, pointing at the reboot as the likely real cause. The
merge-refusal message is kept for the case it was actually written for: a
value that exists but belongs to some other filter.

Written deliberately flat, outside the `if` block that precedes it:

```
set HAVEKSTHUNK=0
if not "%HAVEFILTER%"=="1" goto ksdone
reg query "%CLASSKEY%" /v UpperFilters | find /i "ksthunk" >nul
if not errorlevel 1 set HAVEKSTHUNK=1
:ksdone
```

`cmd` parses an entire parenthesised block before running it, so a pipe inside
one is a reliable way to be surprised on XP. (`if errorlevel N` is safe inside
a block - it queries the live errorlevel; it is `%ERRORLEVEL%` that gets
expanded at parse time.) The file stays CRLF throughout - 202 CRLF, 0 bare LF
- because XP `cmd.exe` seeks batch files by byte offset and this one has
labels.

**Validated by execution**, not by reading: the development machine carries
the identical state (`ksthunk.sys` present, `UpperFilters = ksthunk`), which
makes the new branch provably reach `goto end` before any `reg add`. Run from
a scratch directory so `%~dp0` could not write into `package/`. Result: new
branch taken, correct text, **zero registry writes and no `unfixthunk.cmd`
created**.

### MD5 identifies a FILE, not a SOURCE STATE

Rebuilding `chk` from source that differed only in one constant, then
rebuilding again after putting that constant back, produced **three distinct
65,536-byte binaries**:

| build | `ulDebugOut` | MD5 |
|---|---|---|
| item p (Stage 5bw) | `DBG_DEFAULT` | `9ffb38f221b9c2b3c815441a95d021cd` |
| Stage 5bx diagnostic | `DBG_ALL` | `5440eeed98fb0557729bb222cbbd00c6` |
| Stage 5bx restored | `DBG_DEFAULT` | `22889b8dcb5034e75ce14dc87a8dcfd5` |

The first and third are the same source. WDK 7600 stamps a build timestamp
into the PE header. So the release note's advice to "use the MD5, never the
size" is still right for telling two *files* apart, but **never read an MD5 as
a fingerprint of what went into a binary.** Both READMEs now say this.

### Final state

| artefact | size | MD5 |
|---|---|---|
| `package-release/stwrtxp.sys` (`fre`) | 25,088 | `637eaa9056f05f077fa7caea9617c18a` |
| `package/stwrtxp.sys` (`chk`) | 65,536 | `22889b8dcb5034e75ce14dc87a8dcfd5` |
| `stwrtxp.inf` (both, byte-identical) | 11,556 | `27305dadcfe018be3f377730b8f610d5` |
| `fixthunk.cmd` (both, byte-identical) | 8,712 | `f987c29a96079161a19d3797bbab03cc` |
| `IDT Audio Backported Driver (Release).zip` | 137,475 | - |

The `fre` binary is **unchanged** from Stage 5bw - it is the exact file
confirmed on hardware. Only the README and `fixthunk.cmd` beside it changed,
which is why the zip grew. `debug.h` is back at `DBG_DEFAULT` and
`debug.h.bak-5bx` was deleted as identical to the restored file. Both builds
compile with zero warnings (the "1 Warning" in BUILD's summary is the
environment banner, "x64 Native compiling isn't supported").

The `IDT Audio Backported Driver (Diagnostic 5bx).zip` built during this stage
was deleted once the reboot explained everything - it was a verbose `chk`
build whose hash matches no README.

**Status: the driver is finished.** The only open items are the narrow INF
isolation test described under Stage 5bw's "What is left", and the two
cosmetic machine-state notes, none of which affect a working install.

---

## WDK 7600 sample drivers survey (for Stage 4 base architecture)

Checked `C:\WinDDK\7600.16385.1\src\audio\` for portcls miniport samples to base the new driver on. Two real candidates:

- **`src\audio\msvad\`** — "Microsoft Sample Virtual Audio Device". This is a **WaveCyclic** miniport (confirmed: `basewave.h` defines `CMiniportWaveCyclicMSVAD` / `CMiniportWaveCyclicStreamMSVAD` implementing `IMiniportWaveCyclicStream`), structured as a **shared base + variants**: common code lives directly under `msvad\` (`adapter.cpp`, `basewave.cpp/h`, `basetopo.cpp/h`, `common.cpp/h`, `hw.cpp/h`, `savedata.cpp/h`, `kshelper.cpp/h`), and each subfolder (`simple\`, `pcmex\`, `multstr\`, `micarray\`, `ac3\`, `drmsimp\`, `drmmult\`, `ds2dhw\`) supplies its own `minwave.cpp/h` + `mintopo.cpp/h` + `wavtable.h`/`toptable.h` (format tables + topology tables) that plug into the shared base. **`simple\` is the cleanest starting variant** (single stream format capability, no DRM/multichannel/mic-array complexity). This gives us the WaveCyclic port-class scaffolding (`PropertyHandler`, stream open/close, format negotiation, DPC-driven cyclic buffer notification) essentially for free — our job becomes: swap `hw.cpp`'s fake/software "hardware" simulation for real HDA codec verb transport (`CController`-equivalent, per Stage 3 findings) and swap `mintopo.cpp`'s hardcoded topology table for one built from the codec's actual pin/widget graph.
- **`src\audio\ac97\driver\`** — a **real hardware** codec miniport (Intel ICH AC'97 controller), and notably implements **both** `wavepciminiport.cpp/h` (WavePci — scatter-gather DMA, closer to what real HDA hardware needs) **and** `rtminiport.cpp/h`/`rtstream.cpp/h` (a WaveRT variant, presumably added in a later WDK revision for reference even though XP itself doesn't support WaveRT) alongside `mintopo.cpp` (topology) and `prophnd.cpp` (property handler) and `adapter.cpp`. Since this is a **real PCI hardware device with actual DMA and mixer/topology hardware**, its `wavepciminiport.cpp` + `adapter.cpp` structure (IRP-driven hardware register access, real interrupt/DPC servicing, scatter-gather buffer mapping via `MmProbeAndLockPages`-style APIs — note `stwrt64.sys`'s own imports include exactly this: `MmProbeAndLockPages`, `MmMapLockedPagesSpecifyCache`, `IoAllocateMdl`, matching real DMA buffer handling) is the **more architecturally faithful template for a real HD Audio codec** than the purely-simulated `msvad`.

**Working plan for Stage 4**: base the new driver's overall project/adapter/DriverEntry structure on `ac97\driver\adapter.cpp` (real hardware adapter pattern, PCI resource/interrupt claiming) and its `wavepciminiport.cpp`/`mintopo.cpp` split (WavePci was the plan already per the approved design in Stage 4 of the master plan), but pull the more elaborate stream-state-machine and property-handler boilerplate patterns from `msvad`'s shared `basewave.cpp`/`basetopo.cpp` where they're cleaner/more generic. Neither sample knows anything about HD Audio's codec/verb/pin-complex protocol specifically (that's naturally not present in WDK 7600, since XP never had inbox HDA support) — that part comes entirely from `sigmatel.c` (Stage 1) + `stwrt64.sys` Ghidra findings (Stage 3).

## Next Steps

1. ~~Continue Stage 3 in Ghidra: identify... `CController::TransferCodecVerb`, `CHDACodec::InstantiateCodec`, and the `Presets.bin` parsing routine...~~ **DONE** — see "Stage 3, round 2" findings above (`DecompileAnchors.java` / `stwrt64_decompile.txt`).
2. ~~Locate the WaveCyclic/WavePci portcls miniport samples under `C:\WinDDK\7600.16385.1\src\`...~~ **DONE** — see "WDK 7600 sample drivers survey" above (`msvad` + `ac97\driver`, with a working plan for Stage 4).
3. ~~Decide on the GPIO open question...~~ **DEFERRED** — doesn't block Stage 4/basic playback; revisit only if/when EAPD-via-GPIO turns out to be needed for this board specifically (see Open Questions).
4. ~~Begin Stage 4: design the XPDM replacement driver...~~ **IN PROGRESS** — full skeleton written, see "Stage 4 progress" above. Remaining Stage 4/5 work, in order:
   a. ~~Port the STAC92HD73XX init-verb sequence from `sigmatel.c` into `common.cpp`'s `InitCodec()`~~ **DONE** — see "Stage 4: InitCodec() ported from sigmatel.c" above.
   b. ~~Implement BDL construction in `wavepcistream.cpp`~~ **DONE** — see "Stage 4: BDL construction implemented..." above.
   c. ~~Wire up an ISR/DPC that calls `RequestService()` on each stream's `PSERVICEGROUP`~~ **DONE** — see "Stage 4: ISR/DPC wired up" above.
   d. ~~Recycle BDL slots hardware has already consumed in `Service()`, for indefinite (not just initial-buffer) playback~~ **DONE** — see "Stage 4: BDL slot recycling in Service()" above.
   e. ~~Attempt the first real build against WDK 7600's `wnet`/amd64 environment~~ **DONE — succeeds cleanly, zero errors.** See "Stage 4: first successful build" above. Three real bug categories found and fixed (KSDATARANGE_AUDIO initializer overcounts, KSPIN_DESCRIPTOR field-order mismatches, 3 missing NonDelegatingQueryInterface bodies).
   f. ~~Set up an XP x64 VM so Stage 5 testing has a target~~ **DONE, but VM sanity pass ABANDONED** — VMware never exposes the emulated HDA controller inside this guest no matter what was tried (direct hdaudio config, two different guest-OS relabeling attempts). See "Stage 5: VM sanity pass abandoned" above for the full writeup. Decision made with user: stop chasing this and go straight to the real-hardware track.
   g. **Real-hardware track (historical, WavePci-era result)**: XP x64 is already installed on the real p6-2133w, UAA bus driver present, old WavePci-skeleton build installed cleanly with Code 10 resolved (see Stage 5e) — but that build had stream servicing stubbed out, so it never actually produced sound.
   h. ~~Pivot wave miniport from WavePci to WaveCyclic~~ **DONE** — see "Stage 5f" above for full rationale (the real `HDAUDIO_BUS_INTERFACE` DMA DDI is handle-based/single-buffer, a WaveCyclic fit, not WavePci's scatter-gather model). New files `dmachannel.h/.cpp`, `wavecyclicminiport.h/.cpp`, `wavecyclicstream.h/.cpp` written; `wavepciminiport.h/.cpp`/`wavepcistream.h/.cpp` retired and deleted.
   i. ~~Get the new WaveCyclic code to build cleanly~~ **DONE** — see "Stage 5g" above. Two build-tooling bugs fixed (use `PowerShell` tool not `Bash` for WDK builds; build through the `C:\stwrtxp_src` junction, not the real space-containing path, to avoid WDK 7600's `U1087` link error) and four real source bugs fixed (`OPTIONAL` vs `OPT`, stray `Service()` method, missing `PC_IMPLEMENTATION` define, missing unconditional `ntddk.h` include). `stwrtxp.sys` now builds with zero errors.
   j. ~~Retest the WaveCyclic build on real hardware~~ **DONE — installed cleanly, but no audio and no log file.** Root-caused to a `fre`-build diagnostic gap (see "Stage 5h" above) — fixed by switching to a `chk` build.
   k. ~~Get real diagnostic output from the real machine~~ **DONE** — `chk` build produced a real `stwrtxp_log.txt` showing codec/widget init fully succeeding, silent past that point (later found to be an instrumentation gap, not proof nothing ran further — see below).
   l. ~~Diagnose "no audio"/"no audio specific settings"~~ **DONE, fix applied, not yet hardware-confirmed** — see "Stage 5h" above: `mintopo.cpp`'s topology filter never declared a `KSNODETYPE_SPEAKER`/`KSNODETYPE_MICROPHONE` pin, so sysaudio never recognized a render/capture endpoint. Added the missing physical-jack pins + connections, and added `DOUT` instrumentation to `wavecyclicminiport.cpp`/`mintopo.cpp`/`adapter.cpp` (previously zero DOUT calls in those files — closed the blind spot for the next retest). Rebuilt clean (`chk`, zero errors).
   m. ~~Retest the topology-pin fix on real hardware~~ **DONE — no change.** `StartDevice`/both miniports' `Init()`/both `PcRegisterSubdevice`/`PcRegisterPhysicalConnection` calls all confirmed succeeding via new log instrumentation, but `NewStream` still never called and no "audio specific settings" appear. See "Stage 5i" above. Two hypotheses ruled out (missing `PCFILTER_DESCRIPTOR.CategoryCount/Categories`; broken INF `.Interfaces` wiring).
   m2. ~~Check `adapter.cpp`'s two `PcRegisterPhysicalConnection` calls' return values~~ **DONE, ruled out** — both calls now check their `NTSTATUS` and log on failure; retested and neither ever fails, so this was never the problem.
   m3. ~~Diagnose "no audio device" in the Audio tab despite Device Manager showing no errors~~ **DONE, hypothesis found and fixed, but RETESTED AND RULED OUT** — see "Stage 5j"/"Stage 5k" above. The `AddReg`/`AssociatedFilters`/`wdmaud.drv` fix from Stage 5j made no observable difference on retest (log unchanged, symptom unchanged) and has been retracted as a theory — that registry mechanism is legacy MME/VxD-era and irrelevant to WDM/KS discovery. The `AddInterface` reference-string split is still a legitimate fix but also didn't resolve the symptom on its own.
   m4. ~~Rule out "AddInterface silently failing to register KS device interfaces"~~ **DONE, ruled out** — see "Stage 5k" above. Confirmed via regedit (`HKLM\SYSTEM\CurrentControlSet\Control\DeviceClasses\{6994AD04-...}` and the `KSCATEGORY_RENDER` GUID) that subkeys for our device instance (`HDAUDIO\FUNC_01&VEN_111D&DEV_76C7&SUBSYS_103C2ACD&REV_1001\4&38953DA4&0&0001`) exist under both. Interface registration is confirmed working; the fault is downstream.
   n0. ~~Raw-KS-level test via KsStudio: is our filter visible at all?~~ **DONE, confirmed visible with fully correct metadata** — see "Stage 5l" above. Both Topology and Wave filter factories enumerate correctly under `KSCATEGORY_AUDIO`, with the Wave filter correctly aliased to `KSCATEGORY_RENDER`/`KSCATEGORY_CAPTURE` too. This rules out "filter invisible/broken at raw KS level" as an explanation.
   n1. ~~Determine whether filter instantiation + pin creation actually succeeds in KsStudio~~ **ABANDONED — KsStudio itself confirmed broken on this machine** (control test with a known-good Microsoft filter failed identically; KsStudio also threw its own "sys file missing" startup error). See "Stage 5l" above.
   n2. ~~Run `kstest.exe` (custom KsCreatePin test tool) on the target machine~~ **DONE — result is `ERROR_ACCESS_DENIED` (5) on `CreateFile` itself, for both categories.** See "Stage 5n" above. `KsCreatePin` was never reached; this is an OS-level access-control denial on the device object, occurring before any KS negotiation. Ruled out our own driver code as the cause (no security/device-object-creation code anywhere in the source — it's all standard `PcAddAdapterDevice`/`PcRegisterSubdevice`).
   n3. ~~Fully uninstall + delete driver software + reboot + clean reinstall on the real hardware, then rerun `kstest.exe`~~ **DONE — NO CHANGE, and this was already standard practice, not a new variable.** See "Stage 5o" above. Stale-state from prior installs is ruled out as the explanation.
   n4. ~~Run the updated `kstest.exe` (now with a `GUID_DEVINTERFACE_DISK` control test) on the target machine~~ **DONE — control test succeeded, confirming Access Denied is specific to our device object, not machine-wide.** See "Stage 5p" above.
   n5. ~~Reinstall with the interface-level `Security` AddReg fix and retest~~ **DONE — NO CHANGE.** See "Stage 5q" above. Wrong INF section for a device-object security override; corrected.
   n6. ~~Reinstall with the device-level `Security` AddReg fix (now in `[StwrtXP.AddReg]`) and retest~~ **DONE — NO CHANGE.** See "Stage 5r" above. Both possible INF-level `Security` AddReg placements (interface-level and device-level) are now ruled out; working theory is that PortCls's own device-object creation never consults INF-supplied security overrides at all.
   n7. ~~Run the `kstest.exe` with runtime take-ownership/DACL-reset fallback (name-based `SetNamedSecurityInfoW` version) on the target machine~~ **DONE — take-ownership itself failed with `ERROR_BAD_PATHNAME` (161), a KS device-interface path isn't parseable by that name-based API.** See "Stage 5s" above. Fixed by switching to handle-based `SetSecurityInfo`; rebuilt clean.
   n8. ~~Run the handle-based take-ownership/DACL-reset `kstest.exe` on the target machine~~ **DONE — even the `WRITE_OWNER`-only open was denied.** See "Stage 5t" above. This is a strong signal the denial is NOT an object-manager DACL check at all (that specific open is supposed to succeed unconditionally under `SeTakeOwnershipPrivilege`, regardless of DACL) — added privilege-enable success logging to disambiguate "privilege didn't actually enable" from "this was never a DACL check to begin with", and rebuilt.
   n9. ~~Run the newly-rebuilt `kstest.exe` (with privilege-enable success logging) on the target machine and retest~~ **DONE — all three privileges confirmed `OK`, yet `WRITE_OWNER` STILL denied.** See "Stage 5u" above. This conclusively rules out the object-manager DACL/take-ownership theory entirely (a hard OS guarantee was violated, which can only mean the check being applied isn't a DACL check at all). Pivoted: added a zero-access-open + `READ_CONTROL`-open-and-dump-SDDL diagnostic to `kstest.c` (`DiagnoseSecurityDenial()`) to get ground truth instead of more guesswork. Rebuilt clean.
   n10. ~~Run the newly-rebuilt `kstest.exe` (with the zero-access/READ_CONTROL security diagnostic) on the target machine and retest~~ **DONE — `CreateFile(0)` (zero access) is ALSO denied.** See "Stage 5v" above. At the time this was read as airtight proof against any security-descriptor check, retiring that whole thread (Stage 5p-5u). **REOPENED in Stage 5ab**: that reasoning only ruled out a check inside our own driver's dispatch logic, not the Object Manager's own `SeAccessCheck` during device-interface path resolution (upstream of any IRP) — see Stage 5ab for the corrected reasoning and the new device-security-descriptor-dump diagnostic this led to. Added an A/B control test (`ControlTestOtherAudioFilters()`) that opens every `KSCATEGORY_AUDIO` filter on the machine, not just ours, to determine whether this is machine-wide or specific to our driver. Rebuilt clean.
   n11. ~~Run the newly-rebuilt `kstest.exe` (with the all-KSCATEGORY_AUDIO A/B control test) on the target machine and retest~~ **DONE — INCONCLUSIVE.** See "Stage 5w" above. Only stub/pseudo `root#system#0000` entries were found besides our own (failing with a different error, `ERROR_INVALID_FUNCTION`, not `ACCESS_DENIED`) — no genuine actively-loaded comparison filter existed on the machine. Pivoted to building + packaging Microsoft's own `msvad\simple` sample driver as a real comparison target; sent to user for install.
   n12. ~~Install the `msvadtest.inf`/`vadsimpl.sys` package via Device Manager's Add Legacy Hardware wizard, confirm it shows as working, then rerun `kstest.exe` and retest.~~ **DONE — decisive, msvad's filter opens FINE.** See "Stage 5x" above. Conclusively ruled out a machine-wide explanation; found the actual root cause via `adapter.cpp` comparison (missing `PcRegisterAdapterPowerManagement` call) and applied the fix.
   n13. ~~Install the newly-rebuilt `stwrtxp.sys`/`stwrtxp.inf` (with the `PcRegisterAdapterPowerManagement` fix) on the real machine and retest with `kstest.exe`.~~ **DONE — NO CHANGE, fix ruled out as the (complete) cause.** See "Stage 5y" above. `PcRegisterAdapterPowerManagement`/both `PcRegisterPhysicalConnection` calls all confirmed succeeding silently via the log, yet both filters are still denied identically to every prior retest.
   n14. ~~Check for a leftover OEM filter driver (`UpperFilters`/`LowerFilters`) from the original closed-source IDT package, and read msvad's `simple\toptable.h`/`mintopo.h` to resolve the bridge-pin-reuse theory's topology contradiction.~~ **DONE — both ruled out.** See "Stage 5y" above. No `UpperFilters`/`LowerFilters` values exist anywhere on the real machine (user-confirmed); msvad's own topology filter has the same all-internal-pins shape ours does, so the bridge-pin-reuse theory doesn't hold up either. Pure static source comparison against msvad is now considered exhausted.
   n15. ~~Install an XP-compatible build of Sysinternals Process Monitor on the real machine.~~ **ABANDONED — could not get any downloadable build working on this XP x64 machine.** Pivoted to self-instrumentation instead (see n16/Stage 5z).
   n16. ~~Install the newly-rebuilt `stwrtxp.sys`/`stwrtxp.inf` (with the `IRP_MJ_CREATE` self-instrumentation hook) on the real machine and retest with `kstest.exe`.~~ **DONE — inconclusive but informative.** See "Stage 5aa" above. All 10 logged creates returned `STATUS_SUCCESS`, none matching `kstest.exe`'s actual failing `DesiredAccess` values — strong evidence the failing calls never reach our dispatch table at all. Found and fixed a stale-pointer logging bug, added PID/filename/sequence-number correlation. Rebuilt, sent to user.
   n17. ~~Install the newly-rebuilt `stwrtxp.sys`/`stwrtxp.inf` (with the sharpened `IRP_MJ_CREATE` hook: PID + filename + sequence number, stale-pointer bug fixed) on the real machine and retest with `kstest.exe`~~ **DONE (twice) — CONFIRMED: `kstest.exe`'s own PID never appears in the log; all 10 logged creates are PID=4 (System) opening `"\Topology"`/`"\Wave"` internally during `StartDevice`, all `STATUS_SUCCESS`.** See "Stage 5ab" above. Airtight proof `kstest.exe`'s real calls never reach our `IRP_MJ_CREATE` dispatch at all — the denial happens upstream, reopening the Object-Manager security-descriptor angle.
   n18. ~~Install the newly-rebuilt `stwrtxp.sys`/`stwrtxp.inf` (now with a `DumpDeviceSecurity` diagnostic dumping owner SID + full DACL/ACE list for every device object on our driver's device list) on the real machine and retest with `kstest.exe`.~~ **DONE — theory DISPROVEN: the DACL is fully permissive (Administrators/System get full access, Everyone gets broad access, no DENY ACEs at all), yet `kstest.exe` (running as Administrator) is still denied everything.** See "Stage 5ac" above. Also discovered only one device object exists on the driver's device list at all (`PcRegisterSubdevice` doesn't create separate per-subdevice device objects), so this diagnostic could never see more than one shared SD anyway. Root cause still unknown; pivoted to a new theory (KS filter-factory-level security on the named `"\Topology"`/`"\Wave"` sub-objects, distinct from the device object's own DACL).
   n19. ~~Run the newly-rebuilt `kstest.exe` (with the new `TestOpenBareInterface` zero-access bare-device-interface-open diagnostic) on the real machine and paste back the console output.~~ **DONE — bare-interface open ALSO denied (`GetLastError=5`), for both pins.** See "Stage 5ad" above. This looks like it confirms "denial at the base device-object level," but that's hard to square with Stage 5ac's proof that the device object's own DACL is fully permissive — two live theories now: (1) the device object's SD is never actually consulted by the OS at all (`FILE_DEVICE_SECURE_OPEN` not set), or (2) a separate device-interface-level security descriptor (potentially stored in the registry under `HKLM\SYSTEM\CurrentControlSet\Control\DeviceClasses\{GUID}\<instance>\Control\Security` or similar) is the real gate, distinct from the device object's own SD.
   n20. ~~Get `stwrtxp_log.txt` from the SAME test run as the n19 retest and check whether any `MJ_CREATE` entries show `kstest.exe`'s own PID.~~ **DONE — confirmed (again) that `kstest.exe`'s calls, including the new bare-interface open, never reach our dispatch table at all; the log only ever reflects driver-load-time events (PID=4), and `kstest.exe` itself never writes to that file.** See "Stage 5ae" above. Pivoted: added a PDO security-descriptor dump (`AddDevice` already receives the real bus-owned PDO, never inspected before) plus `Characteristics`/`FILE_DEVICE_SECURE_OPEN` logging for both the PDO and `dev#0`. Rebuilt clean (`chk`), sent to the user.
   n21. ~~Install the newly-rebuilt `stwrtxp.sys`/`stwrtxp.inf` (with the PDO security dump + `Characteristics` logging) on the real machine and get a fresh `stwrtxp_log.txt`.~~ **DONE — LIKELY ROOT CAUSE FOUND: the PDO has `Characteristics=0x180` (`FILE_DEVICE_SECURE_OPEN` set, so its SD is genuinely enforced) and its DACL has exactly 0 ACEs — a populated deny-all DACL, not an absent/NULL one.** See "Stage 5af" above. This explains every denial symptom seen since Stage 5p, including retroactively reconciling Stage 5u's confusing `WRITE_OWNER`-denied result. Re-examined the INF's three `Security` AddReg SDDL entries as a possible source — concluded they're probably NOT the cause (wrong registry mechanism: device-instance SD, not the live Object-Manager device-object SD; also decode to a permissive DACL, not an empty one) but not 100% ruled out.
   n22. ~~Implement a runtime fix in `AddDevice` (`adapter.cpp`) that explicitly overwrites `PhysicalDeviceObject`'s DACL with a permissive one.~~ **DONE — `FixPdoSecurity()` implemented (`ObOpenObjectByPointer` + `ZwSetSecurityObject`, manually-built permissive ACL), two rounds of compile errors fixed, clean `chk` build achieved (user ran the build directly after the first attempt was blocked by the Claude Code safety classifier — expected, since this code actively sets a device object's SD rather than just reading one).** See "Stage 5ag" above.
   n23. ~~Install the newly-rebuilt `stwrtxp.sys`/`stwrtxp.inf` (with `FixPdoSecurity`) on the real machine, get a fresh `stwrtxp_log.txt`, and retest with `kstest.exe`.~~ **DONE — the fix never ran: `RtlInitializeSid` failed at runtime (returned FALSE), so `FixPdoSecurity` aborted before ever attempting the DACL overwrite; `ACCESS_DENIED` unchanged, consistent with the fix not executing (not a disproof of the root-cause theory).** See "Stage 5ah" above. Fixed by hand-constructing the Everyone/World SID instead of calling `RtlInitializeSid`.
   n24. ~~Rebuild with the hand-constructed-SID fix~~ **DONE — clean `chk` build, zero errors, classifier did not block this time.** See "Stage 5ah" above.
   n25. ~~Install the newly-rebuilt `stwrtxp.sys`/`stwrtxp.inf` on the real machine, get a fresh `stwrtxp_log.txt`, and retest with `kstest.exe`.~~ **DONE — BLUESCREEN (`SYSTEM_SERVICE_EXCEPTION`).** See "Stage 5ai" above. Root cause not yet identified; this is a hard blocker, **do not reinstall this exact build again**.
   n26. ~~Get the exact bugcheck parameters (or a Minidump) and the last `stwrtxp_log.txt` line(s) before the Stage 5ai crash, to identify which of `FixPdoSecurity`'s newly-reached calls is at fault, then fix it.~~ **DROPPED AS UNNECESSARY — the crashing function was deleted instead.** See "Stage 5aj" above. The data was never collected and would only have told us which line of an unsupported mechanism failed. `FixPdoSecurity` is gone and the current build is safe to install.
   n27. ~~Install the newly built `stwrtxp.sys` + `stwrtxp.inf` from `Backported Driver/package/` (Stage 5aj: INF `.HW` `Security` entry + runtime `FILE_DEVICE_SECURE_OPEN` clear) on the real machine and retest.~~ **DONE — AND IT WORKED.** The PDO came up with a permissive `Everyone`/`0x001F01FF` DACL on one boot, and `FILE_DEVICE_SECURE_OPEN` was cleared on every load; user-mode reached PortCls dispatch for the first time in this project's history and the machine then bugchecked `0x3B` inside our own never-before-executed DMA-channel teardown path. See "Stage 5ak" above for the full dump forensics and the fix.
   n28. ~~Install the Stage 5ak build (the missing `m_pDmaChannel->AddRef()`) on the real machine and retest.~~ **DONE — MAJOR SUCCESS.** `KsCreatePin` succeeded on both the render and the capture pin, and the machine did not bugcheck through pin creation or teardown. See "Stage 5al" above.
   n29. ~~Run the new tone-streaming `kstest.exe` on the real machine and report whether the 440 Hz tone is audible.~~ **DONE — the machine bugchecked.** Not a regression: `KSSTATE_RUN` had never been reached before, so the whole servicing path ran for the first time. A static audit found three independent defects, all fixed in "Stage 5am".
   n30. ~~Recover the crash evidence and confirm which Stage 5am defect actually fired.~~ **DONE — and it was none of them.** The dump showed `0x3B` / `c0000094` (STATUS_INTEGER_DIVIDE_BY_ZERO) at `portcls+0x19a2` on `div eax,r13d` with r13d = 0, reached from `NtDeviceIoControlFile` -> `PcDispatchIrp` (the `IOCTL_KS_WRITE_STREAM` path, not a DPC). r13d is the cyclic buffer size. Root cause in "Stage 5an": PortCls never calls `IDmaChannel::AllocateBuffer` - the miniport must allocate its own buffer - so `BufferSize()` returned 0 forever. The three Stage 5am defects remain genuine and stay fixed; they were simply masked by the stream never getting this far.
   n31. ~~Install the Stage 5an build and re-run the tone test.~~ **DONE — no bugcheck, buffers allocate, but silent.** Root cause of the silence in "Stage 5ao": the codec converter was never bound to the stream (`SET_CONVERTER_FORMAT` 0x2 and `SET_CHAN_STREAMID` 0x706 had never been sent by this driver, ever).
   n31a. ~~Install the Stage 5ao build and re-run the tone test.~~ **DONE — worse: `KsCreatePin FAILED, error=1450` on both pins, `AllocateRenderDmaEngine` returning `C000009A` on the very first try.** Root cause in "Stage 5ap": a DMA-engine leak, one engine per pin, present since streams first worked; the eight engines consumed during the Stage 5an run had exhausted the controller. Stage 5ao's converter binding never executed and remains untested.
   n31b. ~~REBOOT, install the Stage 5ap build, re-run the tone test.~~ **DONE — AND IT WORKED. THE TONE WAS AUDIBLE.** See "Stage 5aq" above. This validated Stage 5ap (a pin could be opened at all, so the DMA engine leak is fixed) and Stage 5ao (the tone was heard, so the converter binding reached the DAC) in one shot. It also retires the queued fallback of building the real `snd_hda_gen_init` / `activate_path` graph: the flattened one-widget-deep `InitOutputPin` path does reach a physical jack on this codec, and the full graph walk is not needed for basic playback.
   n33. ~~Get `stwrtxp_log.txt` from the successful run and check whether engine handles are stable.~~ **DONE — THEY ARE NOT. The leak is not fixed.** All eight `FreeDmaEngine` calls returned `C0000010` (STATUS_INVALID_DEVICE_REQUEST), the handles marched 4,0,5,1,6,2,7,3 exactly as before, and the controller was dry again by the twelfth pin open in the same boot. Root cause in "Stage 5ar": `hdaudio.h` aliases `StopState` and `PauseState` to value 1, so `StopEngine()` paused the engine rather than resetting it, and `hdaudbus` refuses to free the buffer or the engine of an unreset stream. `~AdapterCommon: LEAKED DMA engine` never fired — the safety net only runs when the bus interface is dereferenced, which did not happen in that boot.
   n34. ~~REBOOT, install the Stage 5ar build, run `kstest.exe` three times in a row.~~ **DONE — THE LEAK IS FIXED, AND THE TONE IS CLEAN.** See "Stage 5as" above. All three runs reused engine handles `FFFFFADF4438F004` (render) and `FFFFFADF4438F000` (capture) and stream tag 1; every teardown logged `FreeBuffer: freed buffer on engine X` followed by `ReleaseEngine: freed engine X`; the Stage 5ar section of the log contains zero `FAILED`, zero `C0000010`, zero `C000009A` and no `~AdapterCommon: LEAKED DMA engine`. The user confirms the tone was clean on all three runs, which also validates the converter format (`4011` = 44.1 kHz / 16-bit / stereo — right pitch) and the 10 ms notification timer plus link position register (no stuttering). **The render path is done.**
   n32. ~~Attack the remaining "no audio device" Control Panel symptom as a `sysaudio` graph problem.~~ **DONE — DIAGNOSED AND FIXED, NOT YET TESTED.** See "Stage 5at" above. The suspicion recorded here was right on the first count: the pin IDs passed to the two `PcRegisterPhysicalConnection` calls did not name real bridge pins, because **the wave filter had no bridge pins at all.** `MiniportWavePins[]` held only the two `KSPIN_COMMUNICATION_SINK` streaming pins, the filter descriptor passed `0, 0, NULL, 0, NULL` for nodes and connections, and `PIN_WAVEOUT_BRIDGE`/`PIN_WAVEIN_BRIDGE` in `shared.h` were literally 0 and 1 — the streaming pins. sysaudio cannot reach the topology filter's `KSNODETYPE_SPEAKER` pin from a dead-end sink pin, so it never built a waveOut device. The topology filter in `mintopo.cpp` was already correct and needed no index changes. Fixed on the `msvad\simple\wavtable.h` pattern: four wave pins, DAC/ADC nodes, four connections, bridge pins at indices 2 and 3.
   n35. ~~Install the Stage 5au build, confirm no bluescreen, check Control Panel.~~ **DONE — half good.** No bluescreen: the `KSNODETYPE_SUM` node fix is confirmed correct, and `kstest.exe` still plays a clean tone, so Stage 5at's pin renumbering broke nothing. But Control Panel still reports no audio device and no ordinary playback works. See "Stage 5av" for the log reading.
   n37. ~~Install the Stage 5av build and send back the log.~~ **DONE.** No bluescreen, `kstest.exe` still plays, Control Panel still empty — and a complete 120-entry decoded property trace came back. See "Stage 5aw" for the reading; it eliminated the physical-connection theory and found the one property sysaudio asks for first and never gets.
   n38. ~~Install the Stage 5aw build and send back the log.~~ **DONE — the fix works and is not the bug.** Both handlers fire and succeed; a user-mode service now marks the device default for playback and recording. But sysaudio's enumeration is byte-for-byte identical to the old driver's apart from the two statuses that were meant to change. See "Stage 5ax".
   n39. ~~**Write the three missing wdmaud subclass registry entries and REBOOT.**~~ **DONE and CONFIRMED WORKING (Stage 5ay).** Control Panel now lists the device, Windows plays sounds through it, and the log shows the first non-kstest pin creates and 497 stream writes. It did not produce audible output on its own — see Stage 5ay and n36. Original text follows for history. Write the three missing wdmaud subclass registry entries and REBOOT. No reinstall and no rebuild are needed.** Run `cscript //nologo quicktest-5ax.vbs` (in `Backported Driver/package/`) from an Administrator Command Prompt, then reboot. The script locates the device's class key itself and aborts without writing if the key it finds does not already carry the wave registration, so there is no index to look up by hand. `quicktest-5ax.reg` is the manual fallback. **Do not send the user to Device Manager's Details tab for a "Driver key" — XP has no such entry, that field is Vista-era; this was already gotten wrong once.** What to look for, in order: (1) **does Control Panel -> Sounds and Audio Devices now show a playback device, and does ordinary playback work?** That is the whole question; the log cannot answer it, so it has to be reported. (2) Does `mixerGetNumDevs` effectively become non-zero — i.e. is there a volume slider in the tray / on the Volume tab, even a dead one? A mixer device that exists but has no controls still distinguishes "wdmaud never attached" from "wdmaud attached and found nothing to expose", and those point at different next steps. (3) In the log: does the trace grow past 118 PID-4 requests, and does anything ask `set=Audio` (`KSPROPSETID_Audio` - volume, mute, CPU resources) or `set=Sysaudio`? An `Audio` request that gets refused points straight at n36. (4) Does a pin create appear from a PID that is not kstest and not 480? Note that PID 1704 IS kstest - do not read its pin creates as progress. If Control Panel populates, the remaining work is n36 for a live volume slider and then item p. The corrected `stwrtxp.inf` (8375 bytes) is what to use for the next real reinstall regardless of the outcome. Do NOT restart this from the security, PnP, graph-shape, physical-connection or filter-property angle: Stage 5al ruled out the first two, Stage 5au the third, and Stage 5aw/5ax the last two by direct observation.
   n36. ~~**Add `KSNODETYPE_VOLUME` and `KSNODETYPE_MUTE` nodes to the topology filter.**~~ **DONE and CONFIRMED WORKING ON HARDWARE (Stage 5ay, tested Stage 5az).** The user reports the volume sliders are live and Control Panel sees the device; the log shows `QueryOutputAmpCaps` decoding the real amp geometry, `PropertyHandler_Volume` answering `BASICSUPPORT`, and `ProgramOutputAmp` tracking slider drags onto amp steps 125-127 with mute clear. One loose end is recorded as n42. Original text follows for history. The topology filter now carries seven nodes and nine connections in the `msvad\simple` shape (render `WAVEOUT pin -> VOLUME -> MUTE -> SUM -> VOLUME -> MUTE -> LINEOUT pin`, capture `MIC pin -> VOLUME -> SUM -> WAVEIN pin`, still with no pin-to-pin connection anywhere — the Stage 5au rule), each control node answering `KSPROPERTY_AUDIO_VOLUMELEVEL` / `KSPROPERTY_AUDIO_MUTE` / `KSPROPERTY_AUDIO_CPU_RESOURCES` for GET/SET/BASICSUPPORT and carrying the `KSAUDFNAME_*` name GUID the mixer line displays. The handlers drive the codec's real output amplifier through five new `IHdaAdapterCommon` methods rather than caching, with the range read from the codec's own `AMP_CAP` at init. The wave and master stages sum in dB onto the single hardware attenuator (see Stage 5ay for why that is exact, not an approximation); the capture stage is state-only because this backport has no ADC path. `stwrtxp.sys` is **60928 bytes**, built clean `chk x64 WNET`. **Ship it with the unchanged 8375-byte `stwrtxp.inf` and retest.** What to look for in the fresh log, in order: (1) **is system playback audible now, and are the volume sliders live?** That is the whole question and only the user can answer it. (2) `StopEngine: DMA buffer ... peak |sample| = N` after a system playback attempt — **peak 0 proves kmixer delivered silence**, a large peak disproves the whole diagnosis and moves the hunt to between the buffer and the speaker. (3) `QueryOutputAmpCaps:` at load — if it logs the "no output amp" warning or `AMP_CAP=00000000`, the codec is not reporting caps, the range is the fallback, and the amp verbs are being suppressed. (4) `PropertyHandler_Volume:`/`PropertyHandler_Mute:` lines when the sliders are touched, and the `ProgramOutputAmp:` line that should follow each SET.

   n40. ~~**Fix the `ReleaseEngine` / `AllocateBuffer` ordering bug.**~~ **WITHDRAWN (Stage 5az) — the bug does not exist and this item is closed with no code change.** `ReleaseEngine` already clears `m_bEngineHandleValid`, and the second `AllocateBuffer` is `SetFormat`'s own Stage 5ap re-allocation, which runs *after* the replacement engine handle has been installed. It only looks otherwise in the trace because `SetFormat`'s `DOUT` is the last statement in the function. See the withdrawal note in the Stage 5ay section. **Do not re-open this.**

   n41. ~~**Confirm the Stage 5az buffer-size fix on hardware.**~~ **DONE — the mechanism is confirmed and it was not enough.** The buffer now fills completely (`audio spans bytes 0..16380`, ~23 laps per session) and the sound is still inaudible, because what arrives is 40-50 dB too quiet. See Stage 5ba and n43. Original text follows for history. Ship `package/stwrtxp.sys` (**62976 bytes**) with the unchanged 8375-byte `package/stwrtxp.inf` and retest ordinary Windows playback. What to look for in the fresh log, in order: (1) **is system playback audible?** Only the user can answer that and it is the whole question. (2) `StopEngine: DMA buffer 16384 bytes / 8192 samples, ... audio spans bytes A..B, link position P`. Under the Stage 5az diagnosis a playing stream should now show the audio wrapping the whole buffer — `A` at or near 0, `B` near 16382, non-zero close to the sample count — because at 16 KB even a short sound outlives several laps. If instead `B` still stops short of the end and the sound is still silent, the behind-the-cursor model is confirmed *and* insufficient, and the next suspect is the BDL: compare the `sys`/`phys` addresses on the two `AllocateBuffer` lines of a re-negotiated session against each other. (3) `SetFormat: requested N Hz, ...` now prints before anything can reject it — use it to identify the format behind the fifteen `AllocateRenderDmaEngine failed, status=C000000D` sessions, and check that `SetFormat: rolled back to ...` follows each one instead of the stream going dead. (4) Whether any glitching or stutter appeared, which is the one risk a smaller buffer carries.

   p. ~~**The cleanup pass**~~ **DONE (Stage 5bw).** The pass below was carried out in full; the one thing it turned up that was not already known is that the tone table physically shipped in the previous release build, because only the registry read was `#if (DBG)`-gated. Original spec retained for the record: last, after n41 and n42. Dial `ulDebugOut` in `debug.h` from `DBG_ALL` back to `DBG_DEFAULT`, remove **both** diagnostic dispatch hooks installed in `DriverEntry` (`HookedMjCreate` from Stage 5aa and `HookedMjDeviceControl` from Stage 5av, along with `ReadKsPropertySafe` and `KsPropertySetName`) and the `DumpDeviceSecurity` loop in `adapter.cpp`, and ship a `fre` build. Keep the verbose `chk` build until then: it is the only reason any of the last fourteen stages were diagnosable. The Stage 5aw property handlers are NOT diagnostics and must stay. Remove the **Stage 5ay `StopEngine` DMA-buffer scan** as well (it is bracketed by `---- Stage 5ay diagnostic ----` comments in `wavecyclicstream.cpp`). Delete `package/quicktest-5ax.vbs` and `package/quicktest-5ax.reg` too — one-off test aids, superseded by the corrected INF. The Stage 5aw property handlers and the **Stage 5ay volume/mute nodes** are NOT diagnostics and must stay. Also clean up the seven `*.bak-5ax` files in `src/`. The **Stage 5az buffer-size change is NOT a diagnostic and must stay** — only the scan inside the `---- Stage 5ay diagnostic ----` brackets goes. The `sys %p phys %I64X` tail on the `AllocateBuffer` line may stay; it is one line at load-time verbosity and it is the only record of where the DMA buffer actually lives. The **Stage 5ba** additions all go too — `g_HdaTestTone`, `g_ToneTable` and the `CopyTo` branch in `dmachannel.cpp`, `ReadTestToneSetting` and its call in `adapter.cpp`, the RMS in the `StopEngine` scan, and `package/testtone-on.reg` / `package/testtone-off.reg` — all bracketed by `---- Stage 5ba diagnostic ----` comments. **A shipped driver must never contain a code path that replaces the user's audio with a tone**, so this one is not optional to remove. The **Stage 5bb** work is NOT a diagnostic and must stay — `QueryPcmCaps`, `GetSupportedPcmRates` and `NarrowPcmRangeToCodec` are the fix, not instrumentation; only the twelve per-rate `QueryPcmCaps:   NNNN Hz SUPPORTED` log lines are worth dropping to `DBG_VERBOSE`. The **Stage 5bc** work is likewise NOT a diagnostic and must stay — `ValidateFormat` and its call at the top of `SetFormat` are the enforcement, and the `FormatSize` bounds check in front of the `PKSDATAFORMAT_WAVEFORMATEX` cast is a real robustness fix that predates nothing. The one line that is pure instrumentation is `GetDescription: publishing PCM range ...`, and even that is cheap enough to leave at `DBG_PRINT`. The **Stage 5be/5bf** DAC-node properties (`PropertyHandler_ChannelConfig`, `_SpeakerGeometry`, `_CpuResourcesDac`, `ValidateValueSize`, `AutomationDAC` and the `m_ChannelConfig`/`m_SpeakerGeometry` state) are NOT diagnostics and must stay; only the `DC_LOG_FULL_LIMIT` bump goes back down. The **Stage 5bg** event support is NOT a diagnostic and must stay — `NodeControlChangeEvent`, `EventHandler_ControlChange`, `m_pPortEvents` and both `DEFINE_PCAUTOMATION_TABLE_PROP_EVENT` tables — but the **Stage 5bg process-name and event-decode logging is** (`PFN_PS_GET_PROCESS_IMAGE_FILE_NAME`, `g_PsGetProcessImageFileName`, `ResolveProcessNameHelper` and its call in `DriverEntry`, `CurrentProcessName`, `KsEventSetName`, the `isEvent` branch and the `KSEVENT #%d` log line) and goes with the rest of the dispatch-hook removal. **Two things from Stage 5bh must survive the cleanup even though the code around them is diagnostic.** First, `#pragma code_seg()` → `#pragma code_seg("PAGE")` bracketing `EventHandler_ControlChange` in `mintopo.cpp`, and the deliberate absence of `PAGED_CODE()` inside it: PortCls calls event handlers at DISPATCH_LEVEL, and in a `fre` build the missing `PAGED_CODE()` stops asserting but the paged-segment fault does not stop being a bugcheck. If the handler is ever moved, moved back, or reformatted, the pragmas move with it. Second, `LogToFileF`'s `KeGetCurrentIrql() != PASSIVE_LEVEL` guard and its non-paged placement — which is moot in a `fre` build, since the whole function is `#if (DBG)`, but must not be dropped while any `chk` build is still in use. Finally, clean up the `*.bak-5be`, `*.bak-5bf`, `*.bak-5bg`, `*.bak-5bh` and `*.bak-5bi` files in `src/` alongside the `*.bak-5ax` ones. The **Stage 5bi** discrete data ranges are NOT a diagnostic and must stay — `PinDataRangesPcm`, `PinAdvertisedRateHz`, `BuildPcmDataRanges` and `ValidateFormat`'s membership test; only the per-range `GetDescription` log lines are chatter, and they are cheap. The **Stage 5bj** `dstest.exe` probe is entirely diagnostic and its whole footprint goes: delete `package/dstest.exe`, and note that it is a *user-mode* binary with no relationship to `stwrtxp.sys` — it links `dsound.lib`, ships nothing into the driver, and must not end up in the INF's `CopyFiles`. Its source and build script live in the scratchpad (`dstest.c`, `dstest_build.cmd`) and should be preserved in the repo rather than deleted if a DirectSound question is ever reopened, because reconstructing the WNET/amd64 + `/MT` + `/SUBSYSTEM:CONSOLE,5.02` recipe from scratch is the expensive part, not the C. The same applies to **Stage 5bk's `audiodiag.exe`** and **Stage 5bl's `wmpdiag32.exe` / `wmpdiag64.exe`**: delete all three from `package\`, keep `audiodiag.c`, `audiodiag_build.cmd`, `wmpdiag.c`, `bld64.cmd` and `bld32.cmd`. `wmpdiag.c` is the most expensive of the lot to reconstruct, because it carries hand-declared `IGraphBuilder` / `IMediaControl` / `IBasicAudio` vtables whose **slot order has been validated against a real `quartz.dll`** — that validation is not repeatable for free. **`dstest32.exe` and `audiodiag32.exe`** (Stage 5bl RESULT) go the same way: delete the binaries from `package\`, keep `bld32_dstest.cmd` and `bld32_audiodiag.cmd`. None of these are kernel binaries and none may ever appear in the INF's `CopyFiles`. The **Stage 5bo `pindump32.exe` / `pindump64.exe`** and the **Stage 5bp payload hexdump** are the last two diagnostics added and both go entirely: delete `package/pindump32.exe` and `package/pindump64.exe` (keep `tools/pindump/pindump.c` and its `sources`), and delete `KsHexDumpSafe`, the `KS_HEXDUMP_MAX_BYTES` / `KS_HEXDUMP_CHARS` defines, the `outBuf` / `mode` / `hex` locals in `HookedMjDeviceControl`, and the trailing ` [%s]` on both the `KSPROP` and `KSEVENT` format strings. Most of that vanishes with the dispatch hook anyway, but name it so nothing is left dangling. **`tools/szprobe/` is a dev-machine tool that was never in `package/`** — it must stay in the repo and must never ship. Three more things that are NOT diagnostics and must stay: the **Stage 5bn `BasicSupportStepped` threshold fix** (returning `DescriptionSize` when the caller offers exactly `sizeof(KSPROPERTY_DESCRIPTION)`, which is what makes the two-step size probe work); **`RelaxPdoSecureOpen`** in `adapter.cpp`, which is deliberately ungated because clearing `FILE_DEVICE_SECURE_OPEN` on the PDO is the single thing that makes the device openable at all; and the `GetLastError` / `DirectSoundCreate8` additions in `tools/dstest/dstest.c`, which are diagnostic but live in a user-mode tool that is kept, not shipped. **Never reinstate `FixPdoSecurity`** — the runtime `ObOpenObjectByPointer` + `ZwSetSecurityObject` DACL rewrite — it is a confirmed, reproducible bugcheck and has been deleted from the source; if a DACL is ever implicated again the fix belongs in the INF, declaratively. Finally, extend the backup sweep to `*.bak-5bj` through `*.bak-5bp` in `src/`, plus `dstest.c.bak-5bp` in `tools/dstest/`. **Evidence directories are NOT diagnostics and must stay:** `Backported Driver/logs/stage5bp/`, `logs/stage5bq/` (driver log, both audiodiag logs, the whole regdump), `logs/stage5bs/` (the first same-boot 64-vs-32 DirectSound capture, and the only log that holds a failing `wmplayer.exe` trace and a working 64-bit DirectSound trace together) and `logs/stage5bt/` (WMP working under Hardware acceleration = None - the emulated-path baseline). Every n49 conclusion in this document is checkable only against those files. The **Stage 5br `NodeChannelCount` / per-channel stepping ranges** are the n42 fix and must stay. The **Stage 5bs `ProbeDsoundPlumbing`** function in `tools/audiodiag/audiodiag.c` stays with the tool source (the binaries still go). Delete the remaining diagnostic binaries from `package\`: `audiodiag.exe`, `audiodiag32.exe`, `dstest.exe`, `dstest32.exe`, `kstest.exe`, `kstest32.exe`, `pindump32.exe`, `pindump64.exe`, `wmpdiag32.exe`, `wmpdiag64.exe`, and `regdump.cmd` plus any copied-back `regdump\` folder. Extend the backup sweep once more to `*.bak-5bq` through `*.bak-5bt`, including `HANDOFF.md.bak-5bsr`, `HANDOFF.md.bak-5bsb` and `HANDOFF.md.bak-5bt`. One cosmetic item for the same pass: the 64-bit `Drivers32` key carries triplicate `wave`/`wave1`/`wave2`, `midi1`/`midi2` and `mixer1`/`mixer2` entries (the 32-bit view is already clean) - worth tidying before delivery, but it is a machine-state cleanup, not an INF change.

   n43. ~~**Run the Stage 5ba test tone.**~~ **DONE — the tone was SILENT, and that is the useful answer.** A full-scale sine (peak 16384, RMS 11585) sat in the DMA buffer across five sessions and nothing came out, while kstest was audible on the same build. Amplitude is not the fault; **sample rate is** — every audible stream ever recorded here was 44.1 kHz, every silent one 22.05 kHz. See Stage 5bb and n44. Original text follows for history. Install `package/stwrtxp.sys` (**64512 bytes**) with the unchanged 8375-byte `package/stwrtxp.inf`. Then, in order: (1) **run `kstest.exe` first, before anything else.** It is the zero-cost control this project has been leaning on for six stages and the Stage 5ba log has no run of it; if the tone has stopped being audible, Stage 5az's buffer change regressed the hardware path and nothing else in this item matters. (2) Run `package/testtone-on.reg` and **reboot** (the flag is read once in `DriverEntry`, so nothing happens without a reload), then play any ordinary Windows sound. **Audible tone -> the whole driver-and-hardware path is proven on the ordinary playback path and the remaining fault is entirely the level of what the mixer delivers; go to n42. Silent -> the fault is below PortCls and the amplitude reading was a red herring.** (3) Whatever the answer, check where the Windows volume sliders actually sit — Volume Control's Master and Wave, and the tray slider — because a low Wave slider on its own produces exactly the -50 dBFS the scan measured, and nothing in the log can see it. (4) Send back `stwrtxp_log.txt`; the `StopEngine` line now carries `RMS = N` and `test tone N` as well, and with the tone on the RMS should read about 11500. (5) Run `package/testtone-off.reg` and reboot to get normal playback back.

   ~~n44. **Test the Stage 5bb rate clamp.**~~ **DONE — half confirmed, half falsified. The codec answered `0x0A = 0x000E05E0`, bit 3 clear: 22.05 kHz is genuinely unsupported, so the diagnosis was right and the clamp is a fix rather than a workaround. But 22050 Hz still reached `SetFormat` in every session, because a `KSPROPERTY_CONNECTION_DATAFORMAT` set is not checked against the pin's data ranges by anything in the stack. See Stage 5bc. Kept below for the reading instructions, which are still the right ones.**
   n44 (original text). **Test the Stage 5bb rate clamp.** Install `package/stwrtxp.sys` (**67072 bytes**) with the unchanged 8375-byte `package/stwrtxp.inf`. `TestTone` should be 0 (run `package/testtone-off.reg` if in doubt); the tone is still in the build but off by default. Then play any ordinary Windows sound. **What to read in the log, in order:**
   (1) `QueryPcmCaps: AFG NID 1 0x0A=... 0x0B=... -> effective rate mask ...` followed by twelve decoded lines. **This is the answer to the question the driver has never asked.** If `22050 Hz not supported`, the Stage 5bb diagnosis is confirmed outright and the clamp is a fix. If `22050 Hz SUPPORTED`, the clamp is a workaround and there is a second bug underneath it — record which, and do not let the distinction quietly disappear.
   (2) `NarrowPcmRangeToCodec: rate mask ... -> advertising 44100..48000 Hz`.
   (3) `SetFormat: requested N Hz` on the playback sessions. It should now say **44100 or 48000 and never 22050.** If 22050 still appears, kmixer is ignoring the data range and the clamp did not take — that is a different problem and the range is where to look.
   (4) Whether there is any sound. **This is the whole question.**
   (5) `StopEngine: ... peak ... RMS ...` — with real audio at the correct rate the peak should be in the thousands, and it no longer needs to be interpreted as loudness-equals-audibility.
   If sound arrives, go to n42 next: the sliders will finally be measurable. If it does not, the rate theory is dead too, and what remains unexamined is the link between the controller's stream and the codec's converter — the stream tag, the FIFO, and the BDL the bus driver built — none of which this driver has ever read back from the hardware.

   ~~n45. **Test the Stage 5bc rate guard.**~~ **DONE — IT WORKS. Windows audio plays. Fourteen sessions asked for 22050 Hz, all fourteen were refused, all fourteen kept the 48000 Hz pin format and played at converter format `0011` with peaks up to 18925. kmixer resamples on refusal, exactly as predicted, so the driver never has to. See the Stage 5bc RESULT section. What remains is n46 (VLC) and n42 (the slider), and then the item-p cleanup and a `fre` build.**
   n45 (original text). **Test the Stage 5bc rate guard.** Install `package/stwrtxp.sys` (**69120 bytes**) with the unchanged 8375-byte `package/stwrtxp.inf`. `TestTone` should be 0 (`package/testtone-off.reg` if in doubt). Then play any ordinary Windows sound. **What to read in the log, in order:**
   (1) `GetDescription: publishing PCM range 44100..48000 Hz, 16..16 bit, max 2 ch`. If this says `8000..48000`, the narrowing is running after PortCls copies the descriptor and Stage 5bb's ordering assumption is wrong — that would be a genuinely new finding and it changes where the fix belongs.
   (2) `SetFormat: requested 22050 Hz` followed by **`SetFormat: refused 22050 Hz ... status=C000000D`**. That pair is the guard doing its job. If the `refused` line is missing, `ValidateFormat` is passing a rate it should not — read the `ValidateFormat: rejecting ...` warnings, which name the reason.
   (3) What the stream does next. **The whole question is whether kmixer resamples into the 48000 Hz pin or gives up.** If it resamples, there will be a `StartEngine: running, ... converter format 4011` or `4010` and a `StopEngine` scan with a non-trivial peak. If it gives up, the session ends at the refusal and the application reports an error.
   (4) Whether there is any sound.
   (5) `StopEngine: ... peak ... RMS ...`. With real audio at a rate the DAC can consume, this is the first time in the project those numbers can be read at face value.
   If sound arrives: go to n42, the sliders are finally measurable, and then item p. If the session dies at the refusal instead, the driver has to resample for itself — see the last paragraph of Stage 5bc for the three shapes that could take, and do not start writing one before reading the log, because which of them is needed depends on what kmixer actually did.

   ~~n46. **Find out why VLC is silent when everything else plays.**~~ **DONE — and it was never this driver.** VLC's own debug log shows its active output module is `mmdevice`/`wasapi`, the Vista+ WASAPI API, failing `IAudioClient::Initialize` with `E_INVALIDARG` (`0x80070057`) once per buffer, and never once attempting DirectSound or WaveOut for playback. XP has no WASAPI. No create from VLC ever reached this driver. The preference change to WaveOut did not take because VLC caches the aout (`reusing audio output` / `keeping audio output`) and only re-probes the stream submodule under it — restart VLC, or run `vlc.exe --aout=directsound`. See **Stage 5bd** for the full reading, including the corollary that **the PID-1752 session in the Stage 5bc log was not VLC**, which retracts the "44100 succeeds and reallocates the buffer mid-stream" suspicion along with it.

   n47. **Windows Media Player refuses with "there is a problem with your sound device" — CURRENT FOCUS. The property chain is closed; the cause is now above the driver or misattributed.** Three rounds of log; see **Stage 5be**, **5bf**, **5bg**. State of it:
   (1) **Every property the WMP-shaped session asks for on the wave filter now succeeds** — `CPU_RESOURCES`, `CHANNEL_CONFIG` and `STEREO_SPEAKER_GEOMETRY` on the wave DAC node, the last two being the `IDirectSound::SetSpeakerConfig` pair. Confirmed by handler traces in the 5bf retest.
   (2) **The session now stops after a SUCCESS, not a failure** (`#716`, geometry SET, `00000000`). Its only remaining refusals — 3 `ENABLE_EVENT` and one `CPU_RESOURCES` on a summer — both occur *earlier* and it continued past them. So the terminal event is invisible from here.
   (3) **Node 8 is settled**: a sysaudio virtual node id, virtual 8 = the wave filter's DAC node, sysaudio numbering the composite graph in reverse (topology 0..6 → virtual 6..0, wave DAC → 8, ADC → 7). Proved in 5bf.
   (4) ~~**The blocking unknown is now process identity, not a property.**~~ **SETTLED — see Stage 5bh RESULT.** `{146F1A80-4791-11D0-A5D6-28DB04C10000}` in an `MJ_CREATE` name is `KSSTRING_Pin`, so pin instantiation is visible in the log. **wmplayer.exe opens `\Wave` exactly once and never creates a pin, in any log.** winlogon, explorer and vlc all do. Both of the readings this point was weighing were wrong.
   (5) **Two corrections to earlier "no sample implements this" conclusions, both from searches whose scope was too narrow:** `Audio id=33` is `CPU_RESOURCES`, not `PEAKMETER` (5bf); and `KSEVENTSETID_AudioControlChange` *is* implemented, in `sb16` (5bg). `PEAKMETER` (id 37) still appears in no log at all. **Do not conclude absence from a grep without stating its scope.**
   (6) **Confirmed correct and closed:** the two `KSNODETYPE_SUM` nodes keep `NULL` automation tables — `ac97\driver\mintopo.cpp:1178` and `:1233` do the same on real hardware that worked on XP, so the `CPU_RESOURCES` refusal on virtual node 4 is reference behaviour. `Pin` id 12 → `C0000034` is absent by design.
   (7) **Event support is published on the volume/mute nodes, so the `ENABLE_EVENT` refusals should disappear — but the first build of it bluescreened on install and the fix is Stage 5bh.** PortCls calls event handlers at DISPATCH_LEVEL; the handler had `PAGED_CODE()` and lived in a paged code segment, and its `DOUT` calls reached a logger that does file I/O. Read **Stage 5bh** before touching any handler. Note also that this driver never *generates* a control-change event — the codec's unsolicited-response path is not wired up — so `ENABLE` succeeding is all that changed.
   (8) **Corroborating positive:** in every one of the three logs some process negotiates a format, creates a pin and plays real content (peak 755). The pin, format negotiation, DMA engine and converter are not the problem.
   (9) **Stage 5bo/5bp additions.** Two more things are settled and one instrument is
   new. Settled: **the driver returns no failure at all to the failing clients** — a
   whole-log status census shows the only `C000000D` anywhere belongs to `winlogon`, so
   `DSERR_INVALIDPARAM` is dsound rejecting a value it *read*, not a status of ours
   being propagated. And **`wChannels=65535` is dead as a theory from the working side**,
   because the 64-bit run saw the identical bogus caps and returned `DS_OK` at every
   rate. Retracted: the seq32/seq64 diff, twice over (stale build, and it compared
   waveOut against DirectSound). New: **the driver now hex-dumps the bytes it returns**
   on every successful KS property/event reply — see Stage 5bp — because "what value is
   dsound rejecting?" is the only question left and the log could never answer it.

   n48. ~~Run the same-boot 64-vs-32 DirectSound comparison.~~ **DONE, and it worked — see `## Stage 5bp RESULT` above. Headline: `GetLastError = 234` is stale residue from a normal probe, so dsound SYNTHESISED the HRESULT; 32-bit dsound never issues the `KSPROPERTY_PIN_DATAINTERSECTION` request its 64-bit twin issues at #758, and everything before that point is byte-identical. The fault is in user mode, above our dispatch table. The steps below are kept because the recipe is reusable.**
   Every previous 32-vs-64 comparison was across different boots or different APIs, so
   none of them was actually like-for-like. Steps, in order:
   (1) Install `Backported Driver\package\stwrtxp.sys` (**77,312 bytes**, checked x64)
   with the unchanged 8,375-byte `package\stwrtxp.inf`, and **reboot**.
   (2) **Delete `C:\stwrtxp_log.txt` before running anything.** The log is append-mode
   across installs and boots; deleting it first makes the trace contain only the two
   runs and nothing else.
   (3) Run **`dstest.exe`** (64-bit). It plays a series of tones — that is expected.
   (4) Run **`dstest32.exe`** (32-bit).
   (5) Collect `C:\stwrtxp_log.txt`, `dstest_log.txt` and `dstest32_log.txt` (each
   probe prints its own log path at start and finish).
   **What to read, in order:** (a) in `dstest32_log.txt`, the new `GetLastError after
   the failure = N` line. **87 means dsound is wrapping a failed ioctl; 0 means it
   synthesised the HRESULT itself** — those two answers point at completely different
   halves of the stack, and this is the cheapest discriminator available. (b) the new
   `DirectSoundCreate8` result on the same failure path; **success there narrows the
   fault to one specific init path inside `dsound.dll`** rather than DirectSound as a
   whole. (c) in the driver log, PID-filter the two runs apart and diff them with the
   `[hex]` payloads visible. The 64-bit run is the control — it is known to succeed —
   so **the first reply whose bytes differ, or whose bytes could plausibly be read
   differently by a 32-bit caller, is the answer.** Record the PID, the process name
   and the build with any filtered file you produce from this; two derived files were
   already invalidated once for lacking exactly that (see Stage 5bp).


   n49. ~~**Find where 32-bit dsound gets its pin data ranges.**~~ **SOLVED AND CLOSED (Stage 5bu/5bv).** It was never about where dsound sourced its ranges. The MEDIA device class on this machine was missing `UpperFilters = ksthunk`, so `ksthunk.sys` - XP x64's "Kernel Streaming WOW64 Thunk Service" - was not attached to any media device, and no 32-bit kernel-streaming request whose structures differ by bitness could survive the trip. Registering it made Windows Media Player and 32-bit DirectSound work, verified on hardware; the fix is now declarative in `package/stwrtxp.inf`. **WMP is closed with it.** Everything below this line in the n49 history is superseded and kept only as the record of how the search ran.
   Stage 5bp RESULT proved the divergence is above us, and left exactly one place a
   value can still differ: something 32-bit `dsound.dll` reads that never touches our
   driver. No `KSPROPERTY_PIN_DATARANGES` (`Pin id=3`) query appears anywhere in the
   1,215-line log, from any PID, yet the 64-bit run somehow knows enough to build a
   216-byte `DATAINTERSECTION` request. Cheapest experiments first:
   (1) **Registry, costs one `reg export` and no build.** On the XP box dump and diff:
   `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32` against
   `HKLM\SOFTWARE\Wow6432Node\Microsoft\Windows NT\CurrentVersion\Drivers32`.
   A 32-bit process reads the `Wow6432Node` view — **a different key**. If it is
   missing, stale, or points at a different `wavemapper`/`wave` entry, that alone
   explains a 32-bit-only `DSERR_INVALIDPARAM` with no ioctl ever sent. Note the
   already-flagged cleanup item: the 64-bit key carries triplicate
   `wave`/`wave1`/`wave2`, `midi1`/`midi2`, `mixer1`/`mixer2` entries, so the two views
   are known not to be pristine. Also dump
   `HKLM\SYSTEM\CurrentControlSet\Control\MediaCategories` and the device's
   `Drivers\wdmaud.drv` subkeys under the class key.
   (2) **sysaudio's cached graph.** The `Sysaudio` (`CBE3FAA0`) and `SysaudioDev`
   (`0C4F9C81`) property sets are answered by `sysaudio.sys`, not by us, and never
   appear in our log. If (1) comes back clean, the next move is to make those visible —
   either by attaching a filter device to sysaudio's device object, or by writing a
   user-mode probe that opens the sysaudio filter directly and asks it for the device
   description and pin data ranges **from both bitnesses**, then diffing the answers.
   That probe is the 32-bit-visible equivalent of `pindump`, and it is where the next
   real evidence is.
   (3) `kmixer`'s cached preferred format — last, and only if (1) and (2) are clean.
   **Do NOT re-run the 5bp driver-log comparison.** It has already given everything it
   can: the two traces are byte-identical up to the divergence, so no further driver-side
   logging can distinguish them. The next artifact has to come from outside our dispatch.

   n50. **Fix `KSPROPERTY_AUDIO_CPU_RESOURCES` returning `STATUS_NOT_FOUND` for node 4**
   while nodes 5, 6 and 8 succeed. Found in the Stage 5bp log. It appears identically in
   the working 64-bit path so it is **not** the WMP discriminator, but it is a genuine
   inconsistency in our node/automation table and should not ship. Low risk, low cost.

   n42. **SOLVED in Stage 5br — the right channel was never attenuated.** wdmaud's dB conversion turned out to be exact (`20*log10(V/65535)` to within 0.001 dB); the driver advertised `MULTICHANNEL` with `MembersCount = 1` for a stereo node, so wdmaud built a UNIFORM one-channel control and only ever set `ch=0`, leaving the right amp at 0 dB forever. Fixed by `NodeChannelCount` + per-channel stepping ranges in `BasicSupportStepped`. **VERIFIED ON HARDWARE:** `channels=2 asked=88 answered=88`, 90 SETs on each of ch=0 and ch=1 (was 0 on ch=1), `amp steps L=124 R=124` tracking, and 47 distinct amp steps of real travel across ~35 dB. Original (wrong) framing follows for the record. **Find out why wdmaud maps the whole slider onto ~0.5 dB — RE-PRIORITISED, and the measurement is now staged.** The user reported independently that the volume slider does nothing audible, which upgrades this from cosmetic to functional. The driver side has been re-audited and is correct (see `## Stage 5bq`); the sweep in `audiodiag` will produce the missing mixer-value -> KS-level mapping in one run, with no driver change. Original note follows.** Find out why wdmaud maps the whole slider onto ~0.5 dB** — **cosmetic again, and now finally measurable.** Stage 5bc settled the silence, so the -50 to -63 dBFS measurement that made this look urgent in Stage 5ba is explained by the rate fault and not by the gain path. Demote it back to a polish job — but a real one, and the first item that can now be tested by ear rather than by inference. Across a full drag the Stage 5ay log's `ProgramOutputAmp` master values only ever span 0 to -35230 in 1/65536 dB units, i.e. about 0.54 dB out of the -95.25 dB range `QueryOutputAmpCaps` reports, so the slider moves the hardware attenuator by at most two of its 127 steps and the volume control is effectively inert even though it is live. Suspect the `BASICSUPPORT` stepping/bounds reply in `mintopo.cpp` (`BasicSupportStepped`) or the units wdmaud infers from it. Do not touch this until playback is audible — it is unmeasurable before then.
       1. Does the machine still bugcheck? If it does, grab the new `MEMORY.DMP` — but expect a **different** stop code or a different faulting object, because this one is positively identified and fixed. A byte-identical `0x3B` on a 96-byte `'wNcP'` block would mean the fix did not take (check the installed `stwrtxp.sys` is actually the 13:32 build, not a cached copy).
       2. Does `stwrtxp_log.txt` now show `NewStream: success` followed by `SetFormat` / `AllocateBuffer` / `SetState` activity, instead of stopping dead partway through the second round of opens?
       3. Does the PDO's `DumpDeviceSecurity` line report `1 ACE(s)` on *every* load this time, or does it revert to `0 ACE(s)` on some boots again? This is the one open question left over from Stage 5ak.
       4. Does Control Panel still report "no audio device"? Only worth investigating once 1–3 look healthy.
   n. Fix the hardcoded single-stream-descriptor assumption carried over into the WaveCyclic miniport's `NewStream` (originally flagged against `wavepciminiport.cpp`, see item 5 above) — lower priority, only matters once simultaneous playback+record is attempted.
   o. Open follow-up: real-hardware kernel debugging (as opposed to the VM's easy named-pipe transport) will need a physical serial or 1394 debug cable, since XP predates KDNET. Not urgent — only matters if a real-hardware bugcheck needs live debugging.

## File/Tool Inventory

- `Original Driver/WDM/stwrt64.sys` — the closed-source WaveRT miniport binary to reverse-engineer.
- `Original Driver/WDM/STWRT64.INF` — original INF, useful for HDA hardware IDs and file manifest; not directly reusable for XP (wrong OS section, wrong port class assumptions downstream).
- `Original Driver/WDM/Presets.bin`, `C-A1.INI`...`C-F3.INI`, `stwrt64.ini` — candidate sources of codec preset/verb data, not yet analyzed.
- `Original Driver/WDM/DTS_TOWER.INI/.XML`, `EQ*.INI`, `HPToneCtrls64.dll`, `sl*64.dll`, `SR*64.dll` — vendor effects/DSP add-ons (DTS, SRS, HP Tone Controls) — explicitly out of scope per approved plan, listed here only so we don't waste time reverse-engineering them later.
- `Backported Driver/` — `src/` (the driver), `package/` (checked build), `package-release/` (free build), `tools/` (the user-mode diagnostic programs), `reference/` (Linux HDA source + Ghidra dump), `logs/` (preserved evidence), and `IDT Audio Backported Driver (Release).zip`. **`inf/` was deleted in Stage 5bw** — it was a redundant third copy of the INF and the stale copy that cost a day in Stage 5bv. The INF is maintained in `package/` and copied to `package-release/`; the two must stay byte-identical.
- `Backported Driver/src/dmachannel.h/.cpp` — `CHdaDmaChannel` (`IDmaChannel` adapter over `IHdaAdapterCommon`'s DMA-engine wrappers). New in the WaveCyclic rewrite.
- `Backported Driver/src/wavecyclicminiport.h/.cpp` — `CMiniportWaveCyclicHda` (replaces the retired `wavepciminiport.h/.cpp`).
- `Backported Driver/src/wavecyclicstream.h/.cpp` — `CMiniportWaveCyclicStreamHda`, software-timer/DPC-serviced stream (replaces the retired `wavepcistream.h/.cpp`).
- `C:\stwrtxp_src` — a directory junction (`mklink /J`) to `Backported Driver/src`, required because WDK 7600's `build.exe`/nmake cannot build from a path containing spaces (see "Stage 5g" above for the exact error and recreate-command). **Always build through this junction, not the real path.** Not part of the repo — recreate it if missing:
  ```
  cmd /c mklink /J "C:\stwrtxp_src" "C:\path\to\stwrtxp\src"
  ```
  Build via the `PowerShell` tool (not `Bash` — see "Stage 5g"), e.g.:
  ```
  cmd /c "call C:\WinDDK\7600.16385.1\bin\setenv.bat C:\WinDDK\7600.16385.1 fre x64 WNET && cd /d C:\stwrtxp_src && build -cZ -w"
  ```
  **Use `chk` instead of `fre` as the second argument to `setenv.bat` for any on-hardware diagnostic session** (see "Stage 5h" above) — a free/retail build (`fre`, `objfre_wnet_amd64\amd64\stwrtxp.sys`) compiles all `DOUT`/`LogToFileF` debug logging out entirely via `#if (DBG)`, producing zero log output no matter what's wrong. A checked build (`chk`, `objchk_wnet_amd64\amd64\stwrtxp.sys`) still loads fine on a normal free/retail XP install and is the only way to get real diagnostic output on the p6-2133w, which has no kernel debugger or debug cable. Only switch back to `fre` for the final build once real audio playback is confirmed working end-to-end.
- `Backported Driver/reference/stwrt64_dump.txt` — Ghidra headless dump of `stwrt64.sys`: imports, exports, all ~990 defined functions (unnamed, `FUN_xxxxxxxx`), and 187 keyword-matched debug/assert strings revealing the driver's internal C++ class design (see Stage 3 findings above). Regenerate by re-running `tools/ghidra_scripts/DumpDriverInfo.java` via `analyzeHeadless.bat` if the analysis needs refreshing.
- `tools/ghidra_scripts/DumpDriverInfo.java` — custom Ghidra headless post-script used to produce the dump above. Had to be patched once already (removed a dependency on `ghidra.util.string.StringSearcher`/`FoundString`, which aren't present on this Ghidra 12.1.3 headless classpath, in favor of a hand-rolled printable-ASCII byte scanner) — if re-run ever errors with a `ClassNotFoundException`/compile error again, check this class dependency first before assuming something else broke.
- `tools/ghidra_projects/stwrt64_analysis/` — the Ghidra project itself (already imported + analyzed); reuse this rather than re-importing `stwrt64.sys` from scratch (`-process stwrt64.sys -noanalysis` flags, not `-import`, when just re-running a post-script against it).
- `Backported Driver/reference/stwrt64_decompile.txt` — Ghidra headless decompilation of the 5 highest-value functions in `stwrt64.sys` (codec instantiation, verb transport, presets parser, path loop-detection, registry-config dispatcher), located via cross-references from 9 anchor debug strings. See "Stage 3, round 2" findings above for the synthesis. Regenerate via `tools/ghidra_scripts/DecompileAnchors.java` (same `-process stwrt64.sys -noanalysis -postScript DecompileAnchors.java <output_path>` invocation pattern as `DumpDriverInfo.java`).
- `tools/ghidra_scripts/DecompileAnchors.java` — custom Ghidra headless post-script that finds functions referencing a curated list of anchor strings and runs Ghidra's `DecompInterface` on each, dumping C-like pseudocode. Anchor list is hardcoded in the script (`ANCHORS` array) — edit it and re-run to pull in more functions if further decompilation is needed later (e.g. GPIO-handling functions, if the open GPIO question needs resolving from disassembly rather than from `sigmatel.c`).
- `Backported Driver/tools/` — the user-mode diagnostic programs written across Stage 5. **None of them is a kernel binary, none links against `stwrtxp.sys`, and none may ever appear in the INF's `CopyFiles`.** The `.c` and the build script are the parts worth keeping: reconstructing each WDK invocation (the right `setenv` flavour, `/MT`, the right `/SUBSYSTEM:CONSOLE,5.0x`, the `NODEFAULTLIB` list) is far more expensive than rewriting the C.
  - `kstest/` — `KsCreatePin` + audible-tone test. **The zero-cost control for this whole project**: if kstest is audible, the driver-to-hardware path is intact and the fault is above PortCls. Run it first, every time. `kstest32.exe` is the 32-bit build, and it is the one that fails — `KsCreatePin` returns `C00000F2` STATUS_INVALID_BUFFER_SIZE where the 64-bit build succeeds.
  - `dstest/` — DirectSound probe: enumerate, `DirectSoundCreate`, `CreateSoundBuffer` at every rate, play. `dstest.exe` (64-bit) passes everything; `dstest32.exe` fails at `DirectSoundCreate` with `0x80070057` `DSERR_INVALIDPARAM`, which is the WMP symptom reproduced in twelve lines of C. Stage 5bp added a `GetLastError()` readout and a `DirectSoundCreate8` retry on the failure path.
  - `audiodiag/` — waveOut/mixer-API survey. Its **file header is a load-bearing historical record** of what the 64-bit dstest run reported; do not trim it.
  - `wmpdiag/` — DirectShow graph probe. `wmpdiag.c` carries hand-declared `IGraphBuilder` / `IMediaControl` / `IBasicAudio` vtables whose **slot order was validated against a real `quartz.dll`** — that validation is not repeatable for free, so this file is the most expensive one here to lose.
  - `pindump/` — dumps every pin's `KSPROPERTY_PIN_DATARANGES` and probes `DATAINTERSECTION`. This is what proved all four advertised ranges report `MaxChannels=2`, killing the `wChannels=65535` theory from the driver side.
  - `szprobe/` — **runs on the dev machine, not on the XP box.** Every number it prints is a compile-time ABI property, so a Win10 x64 host reports exactly what XP x64 would; it costs no hardware test cycle. Built both bitnesses, it measures the `KSPIN_CONNECT` 64-vs-72 split behind the `kstest32` pin-create failure. See the Stage 5bp table.
  - `msvad_abtest/` — Microsoft's own `msvad\simple` sample, packaged as a known-good comparison filter. This is what turned Stage 5w's inconclusive A/B into Stage 5x's decisive one.
  - `README.txt` — written in Stage 5bw. Records what each tool answered, both build routes (a `cl` command line for dstest/audiodiag/wmpdiag, a `sources` file plus `build` for kstest/pindump/szprobe), and the two traps: **WDK include paths must be spelled with backslashes** (MSVC tracks included headers by path string, so `/IC:/WinDDK/...` double-processes every `DEFINE_GUID` header and yields C2374), and **`build` cannot run from a path containing spaces** (`U1087`), which is what the preserved kstest x86 failure actually was.
- `tools/README.txt` — written in Stage 5bw, for the repository-level toolchain directory (`ghidra/`, `ghidra_projects/`, `ghidra_scripts/`, `jdk21/`, `installers/`, `xpbin/`). Two things in it matter: `xpbin/WINDOWS/inf/wdmaudio.inf` is where the ksthunk registration wording came from, and `installers/prod key.txt` is the target machine's XP product key, so **that directory must not be published**. The Ghidra *projects* are the expensive part — they hold accumulated naming and typing that re-running the scripts does not reproduce.
- `Backported Driver/package/` — the **diagnostic** package that goes to the hardware: the checked `stwrtxp.sys` (65,536, MD5 `22889b8dcb5034e75ce14dc87a8dcfd5` — changed in Stage 5bx by a rebuild, *not* by a source change; see “MD5 identifies a FILE, not a SOURCE STATE” there), `stwrtxp.pdb`, `stwrtxp.inf`, `fixthunk.cmd` and `README.txt`. Item p swept the probe `.exe`s out of it in Stage 5bw; **this is the maintained copy of the INF**. The checked build writes `C:\stwrtxp_log.txt`, so install `package-release/` instead for ordinary use.
- `Backported Driver/package-release/` — the **shipping** package: a free (`fre`) build (25,088, MD5 `637eaa9056f05f077fa7caea9617c18a`) that cannot write a log at all — it does not even import `ZwCreateFile` — so it can be left installed indefinitely. Everything works on it, Windows Media Player included — and as of Stage 5bx that is confirmed **on this build**, not carried across from a checked-build result. Do not overwrite this with a checked build. Zipped for delivery as `IDT Audio Backported Driver (Release).zip` (137,475 bytes); the older `(Near Release).zip` was deleted in Stage 5bw because it held both the pre-ksthunk INF and the tone-carrying `.sys`.
- `Backported Driver/logs/stage5bp/` — **the three Stage 5bp artifacts, preserved.** `driver_stwrtxp_log.txt` (1,215 lines; PID 2452 = dstest.exe 64-bit, PID 2876 = dstest32.exe 32-bit, PID 1800 = explorer.exe; no `DriverEntry:` line because the log was deleted after boot, so sequence numbers start at #724), plus `64dstest_log.txt` (the working control) and `32dstest_log.txt` (the failure). This is the only like-for-like same-boot/same-build/same-API pair this project has ever captured — **do not delete it in the item-p cleanup.**
- `Backported Driver/logs/stage5bq/` — **the Stage 5bq artifacts, preserved.** `driver_stwrtxp_log.txt` (1,033 lines; PID 2580 = audiodiag.exe 64-bit, PID 2784 = audiodiag32.exe 32-bit), `audiodiag64_log.txt`, `audiodiag32_log.txt`, and the whole `regdump/` folder (eight keys). This is the run that **solved n42** — it holds the only mixer-value -> KS-level mapping this project has ever measured, and the `ProgramOutputAmp` lines showing `R=127` on every write. It also holds the registry capture that killed two n49 theories. **Do not delete it in the item-p cleanup.**
- ~~**Delete from `package/` at the item-p cleanup**~~ **done in Stage 5bw.** All ten probe `.exe`s, both `quicktest-5ax` aids and both `testtone-*.reg` files are gone; `regdump.cmd` was moved to `Backported Driver/tools/` rather than deleted, being diagnostic *source*. Every tool source stays — `SweepVolumeControl` in `tools/audiodiag/` is still the only code in this project that can write a mixer control, and it is what found n42.
- `C:\stwrtxp_log.txt` — where the checked driver writes (`common.cpp`, `LogToFileF`). **Append-mode across installs and reboots**, so a log handed back may contain many sessions; `DriverEntry:` lines mark the boundaries. Delete it before a run whenever the trace needs to contain only that run. Requests from different processes interleave on shared FileObjects and FileObject pointers get reused after free, so **always filter by PID, never by adjacency.**
- Tooling: Ghidra + WinDbg + WDK 7600 all installed and verified (Stage 2 complete). An XP x64 VM for testing is still not set up (needed before Stage 5).
