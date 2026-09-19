@echo off
rem ===========================================================================
rem regdump.cmd - Stage 5bq / item n49
rem
rem Run this ON THE XP x64 MACHINE. It exports the registry keys that the
rem audio stack reads but our driver never sees.
rem
rem Why this exists: Stage 5bp proved that 32-bit dsound.dll abandons
rem DirectSoundCreate BEFORE sending our driver a single request that its
rem 64-bit twin sends. Everything that crosses our dispatch table is
rem byte-identical between the two. So whatever differs is something the
rem 32-bit process reads elsewhere - and the registry is the cheapest place
rem to look, because a 32-bit process on x64 is silently redirected to the
rem Wow6432Node view of HKLM\SOFTWARE. If that view is missing, stale, or
rem points somewhere else, it would produce exactly this symptom: a failure
rem with no ioctl ever sent.
rem
rem MediaResources is the highest-value key here. That is where the media
rem APIs record which KS filter backs each device for wave, mixer, midi and
rem DirectSound. MediaResources lives under HKLM\SYSTEM, which is NOT
rem redirected - so if it is wrong, it is wrong for both bitnesses, and that
rem would be a different (and more interesting) answer.
rem
rem This only READS. It exports to files and changes nothing.
rem ===========================================================================

setlocal
set OUT=%~dp0regdump
if not exist "%OUT%" mkdir "%OUT%"

echo Exporting to %OUT%
echo.

rem ---- the redirected pair: this is the actual n49 experiment ----
echo [1/8] Drivers32  (64-bit view)
reg export "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32" "%OUT%\drivers32_64bit.reg" /y >nul 2>&1
if errorlevel 1 echo        ** export FAILED - key may not exist **

echo [2/8] Drivers32  (32-bit / Wow6432Node view)
reg export "HKLM\SOFTWARE\Wow6432Node\Microsoft\Windows NT\CurrentVersion\Drivers32" "%OUT%\drivers32_32bit.reg" /y >nul 2>&1
if errorlevel 1 echo        ** export FAILED - key may not exist, which is itself the answer **

echo [3/8] drivers.desc  (64-bit view)
reg export "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\drivers.desc" "%OUT%\driversdesc_64bit.reg" /y >nul 2>&1
if errorlevel 1 echo        ** export FAILED - key may not exist **

echo [4/8] drivers.desc  (32-bit / Wow6432Node view)
reg export "HKLM\SOFTWARE\Wow6432Node\Microsoft\Windows NT\CurrentVersion\drivers.desc" "%OUT%\driversdesc_32bit.reg" /y >nul 2>&1
if errorlevel 1 echo        ** export FAILED - key may not exist **

rem ---- not redirected, but the most likely place the real answer lives ----
echo [5/8] MediaResources   (wave / mixer / midi / DirectSound registrations)
reg export "HKLM\SYSTEM\CurrentControlSet\Control\MediaResources" "%OUT%\mediaresources.reg" /y >nul 2>&1
if errorlevel 1 echo        ** export FAILED **

echo [6/8] MediaCategories  (the node-name GUID table)
reg export "HKLM\SYSTEM\CurrentControlSet\Control\MediaCategories" "%OUT%\mediacategories.reg" /y >nul 2>&1
if errorlevel 1 echo        ** export FAILED **

echo [7/8] Media class key  (our device's own driver key and its subkeys)
reg export "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E96C-E325-11CE-BFC1-08002BE10318}" "%OUT%\mediaclass.reg" /y >nul 2>&1
if errorlevel 1 echo        ** export FAILED **

echo [8/8] DeviceClasses  (the KS device interfaces sysaudio enumerates)
reg export "HKLM\SYSTEM\CurrentControlSet\Control\DeviceClasses" "%OUT%\deviceclasses.reg" /y >nul 2>&1
if errorlevel 1 echo        ** export FAILED **

echo.
echo ==== done ====
echo.
echo Files written to: %OUT%
echo.
echo Send back the whole regdump folder. The first thing to compare is
echo drivers32_64bit.reg against drivers32_32bit.reg - a 32-bit process
echo reads the second one, and they are supposed to agree.
echo.
pause
endlocal
