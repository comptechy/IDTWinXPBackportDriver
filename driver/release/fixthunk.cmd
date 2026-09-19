@echo off
rem ===========================================================================
rem fixthunk.cmd - Stage 5bu / item n49
rem
rem RUN THIS ON THE XP x64 MACHINE, from an administrator account.
rem
rem WHAT THIS IS ABOUT
rem -------------------------------------------------------------------------
rem Windows XP x64 ships a driver called ksthunk.sys, whose service display
rem name is literally "Kernel Streaming WOW64 Thunk Service". It is an upper
rem filter driver that attaches to every device in the MEDIA class. Its whole
rem job is to translate kernel-streaming requests coming from 32-bit programs
rem into the 64-bit layout the kernel expects. Without it, 64-bit programs can
rem use the audio hardware directly and 32-bit programs cannot.
rem
rem That is exactly the shape of the remaining bug:
rem   - 64-bit DirectSound works and streams (verified, stage 5bs)
rem   - 32-bit DirectSound fails before it sends our driver anything
rem   - everything that goes through the kernel mixer instead - Windows
rem     sounds, the volume control, and Windows Media Player once hardware
rem     acceleration is turned off - works fine, because none of that puts
rem     32-bit structures on the kernel-streaming wire
rem
rem Microsoft installs the filter from wdmaudio.inf:
rem   HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e96c-...},"UpperFilters"
rem     = ksthunk        (REG_MULTI_SZ)
rem   HKLM\SYSTEM\CurrentControlSet\Services\ksthunk
rem
rem A stock XP x64 installation has both, even on a machine with no sound card
rem at all - this was confirmed by reading the registry hive out of the clean
rem XP x64 virtual machine in this project. THIS MACHINE HAS NEITHER. The
rem registry dump taken in stage 5bq shows the media class key carrying all
rem six of its other standard values and no UpperFilters.
rem
rem SAFETY
rem -------------------------------------------------------------------------
rem An upper filter that cannot load will stop every media device from
rem starting (Device Manager code 39). So this script REFUSES to register the
rem filter unless it can actually see ksthunk.sys on disk first. It also
rem writes unfixthunk.cmd next to itself, which removes everything it added.
rem
rem Nothing here touches the audio driver, and nothing here is undone by a
rem reboot, so if the machine comes back unhappy just run unfixthunk.cmd.
rem ===========================================================================

setlocal
set CLASSKEY=HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E96C-E325-11CE-BFC1-08002BE10318}
set SVCKEY=HKLM\SYSTEM\CurrentControlSet\Services\ksthunk
set SYSFILE=%SystemRoot%\system32\drivers\ksthunk.sys

echo.
echo =============== BEFORE ===============
echo.
echo [1] Is ksthunk.sys on disk?
if exist "%SYSFILE%" (
    echo     YES  %SYSFILE%
    dir /-c "%SYSFILE%" | find "ksthunk"
    set HAVEFILE=1
) else (
    echo     NO   %SYSFILE% is missing
    set HAVEFILE=0
)

echo.
echo [2] Is the ksthunk service registered?
reg query "%SVCKEY%" >nul 2>&1
if errorlevel 1 (echo     NO) else (echo     YES & reg query "%SVCKEY%")

echo.
echo [3] Does the MEDIA class have an UpperFilters value?
reg query "%CLASSKEY%" /v UpperFilters >nul 2>&1
if errorlevel 1 (
    echo     NO  -- this is the suspected fault
    set HAVEFILTER=0
) else (
    echo     YES -- current value:
    reg query "%CLASSKEY%" /v UpperFilters
    set HAVEFILTER=1
)

rem Is ksthunk already IN that value? UpperFilters is a REG_MULTI_SZ and may
rem legitimately name several filters, so "the value exists" and "we are
rem registered" are different questions - conflating them is what made an
rem earlier version of this script ask for help when the answer was
rem "nothing to do". Written flat rather than inside the block above,
rem because cmd parses a whole parenthesised block before running it and a
rem pipe inside one is a reliable way to get surprised on XP.
set HAVEKSTHUNK=0
if not "%HAVEFILTER%"=="1" goto ksdone
reg query "%CLASSKEY%" /v UpperFilters | find /i "ksthunk" >nul
if not errorlevel 1 set HAVEKSTHUNK=1
:ksdone

rem ---- recover the file from the install media / driver cache if needed ----
if "%HAVEFILE%"=="1" goto havefile
echo.
echo ksthunk.sys is not installed. Trying to recover it...
for %%C in ("%SystemRoot%\Driver Cache\amd64\driver.cab" "%SystemRoot%\Driver Cache\amd64\sp2.cab" "%SystemRoot%\Driver Cache\amd64\sp1.cab") do (
    if exist %%C (
        echo   trying %%C
        expand %%C -F:ksthunk.sys "%SystemRoot%\system32\drivers" >nul 2>&1
    )
)
if exist "%SYSFILE%" (
    echo   recovered from the driver cache.
    set HAVEFILE=1
) else (
    echo   could not recover it.
)
:havefile

echo.
echo =============== APPLYING ===============
echo.
if "%HAVEFILE%"=="0" (
    echo REFUSING to continue: ksthunk.sys is not present on this machine.
    echo Registering an upper filter whose driver file is missing would stop
    echo every sound device from starting. Nothing has been changed.
    echo.
    echo Please copy ksthunk.sys from the XP x64 installation media
    echo ^(AMD64\ksthunk.sy_ - it is a compressed cabinet member; "expand
    echo ksthunk.sy_ ksthunk.sys" unpacks it^) into
    echo %SystemRoot%\system32\drivers\ and run this script again.
    goto end
)

if "%HAVEKSTHUNK%"=="1" (
    echo NOTHING TO DO. ksthunk is already registered as an upper filter on
    echo the MEDIA class - it is in the value printed above. That is exactly
    echo the state this script exists to produce, so it is changing nothing.
    echo.
    echo If 32-bit audio ^(Windows Media Player, 32-bit DirectSound^) is still
    echo broken with this in place, the answer is almost certainly a REBOOT.
    echo PnP reads this value when it BUILDS a device stack, so a driver
    echo installed since the last boot can be running without the filter
    echo attached even though the registration is perfect. Reboot and retest
    echo before investigating anything else.
    goto end
)

if "%HAVEFILTER%"=="1" (
    echo The MEDIA class has an UpperFilters value ^(shown above^), and
    echo ksthunk is NOT in it. This script only knows how to create that
    echo value from scratch, not to merge into an existing one, so it is
    echo stopping here rather than overwriting another driver's filter.
    echo Send the value above back and it will be handled by hand.
    goto end
)

echo Registering the ksthunk service...
reg add "%SVCKEY%" /v Type         /t REG_DWORD /d 1 /f >nul
reg add "%SVCKEY%" /v Start        /t REG_DWORD /d 3 /f >nul
reg add "%SVCKEY%" /v ErrorControl /t REG_DWORD /d 1 /f >nul
reg add "%SVCKEY%" /v Tag          /t REG_DWORD /d 1 /f >nul
reg add "%SVCKEY%" /v ImagePath    /t REG_EXPAND_SZ /d "system32\drivers\ksthunk.sys" /f >nul
reg add "%SVCKEY%" /v DisplayName  /t REG_SZ /d "Kernel Streaming WOW64 Thunk Service" /f >nul
reg add "%SVCKEY%" /v Group        /t REG_SZ /d "PNP Filter" /f >nul
echo   done.

echo Attaching it to the MEDIA device class...
reg add "%CLASSKEY%" /v UpperFilters /t REG_MULTI_SZ /d "ksthunk" /f >nul
echo   done.

echo.
echo =============== AFTER ===============
reg query "%SVCKEY%"
reg query "%CLASSKEY%" /v UpperFilters

rem ---- write the undo script ----
> "%~dp0unfixthunk.cmd" echo @echo off
>>"%~dp0unfixthunk.cmd" echo rem Undoes fixthunk.cmd. Reboot afterwards.
>>"%~dp0unfixthunk.cmd" echo reg delete "%CLASSKEY%" /v UpperFilters /f
>>"%~dp0unfixthunk.cmd" echo reg delete "%SVCKEY%" /f
>>"%~dp0unfixthunk.cmd" echo echo Removed. Please reboot.
>>"%~dp0unfixthunk.cmd" echo pause
echo.
echo An undo script has been written next to this one: unfixthunk.cmd

echo.
echo ===========================================================================
echo  NOW DO THIS:
echo.
echo   1. REBOOT. The filter only attaches when a device starts.
echo.
echo   2. Put hardware acceleration back to FULL, or this proves nothing:
echo        Control Panel -^> Sounds and Audio Devices -^> Audio tab
echo        -^> Sound playback -^> Advanced -^> Performance
echo        -^> Hardware acceleration: slide it back to "Full"
echo.
echo   3. Open Windows Media Player and play something.
echo.
echo   4. Check Device Manager. The sound device should still say it is
echo      working properly. If it shows a yellow mark with code 39, run
echo      unfixthunk.cmd and reboot - that means ksthunk.sys would not load.
echo.
echo   5. Send back C:\stwrtxp_log.txt either way.
echo ===========================================================================

:end
echo.
pause
endlocal
