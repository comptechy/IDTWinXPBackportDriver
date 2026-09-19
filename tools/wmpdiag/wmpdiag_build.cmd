@echo off
rem ---------------------------------------------------------------------
rem  wmpdiag - Stage 5bl.  Builds BOTH bitnesses.
rem
rem  The x86 build is the important one: Windows Media Player on XP x64 is
rem  a 32-bit process, so it loads the 32-bit quartz.dll / dsound.dll.  A
rem  64-bit probe exercises a different copy of the DirectShow stack and
rem  would not reproduce WMP's environment.  The x64 build is the control.
rem
rem  Subsystem versions differ on purpose: 5.02 is Server 2003 / XP x64 for
rem  native 64-bit images; 5.01 is plain XP and is accepted by WOW64 too,
rem  so it is the safer choice for the 32-bit image.
rem ---------------------------------------------------------------------

setlocal
set SRCDIR=%~dp0
set DDKROOT=C:\WinDDK\7600.16385.1

echo.
echo ===================== building wmpdiag64.exe (x64) =====================
cmd /c ""%DDKROOT%\bin\setenv.bat" "%DDKROOT%" fre x64 WNET no_oacr && cd /d "%SRCDIR%" && cl /nologo /W3 /O2 /MT /GS- /DDIRECTSOUND_VERSION=0x0900 /I"%DDKROOT%\inc\api" /I"%DDKROOT%\inc\crt" /Fewmpdiag64.exe /Fowmpdiag64.obj wmpdiag.c /link /SUBSYSTEM:CONSOLE,5.02 /MACHINE:AMD64 /LIBPATH:"%DDKROOT%\lib\wnet\amd64" /LIBPATH:"%DDKROOT%\lib\crt\amd64" dsound.lib winmm.lib ole32.lib advapi32.lib user32.lib kernel32.lib"

echo.
echo ===================== building wmpdiag32.exe (x86) =====================
cmd /c ""%DDKROOT%\bin\setenv.bat" "%DDKROOT%" fre x86 WNET no_oacr && cd /d "%SRCDIR%" && cl /nologo /W3 /O2 /MT /GS- /DDIRECTSOUND_VERSION=0x0900 /I"%DDKROOT%\inc\api" /I"%DDKROOT%\inc\crt" /Fewmpdiag32.exe /Fowmpdiag32.obj wmpdiag.c /link /SUBSYSTEM:CONSOLE,5.01 /MACHINE:X86 /LIBPATH:"%DDKROOT%\lib\wnet\i386" /LIBPATH:"%DDKROOT%\lib\crt\i386" dsound.lib winmm.lib ole32.lib advapi32.lib user32.lib kernel32.lib"

echo.
echo ===================== results =====================
dir /b wmpdiag64.exe wmpdiag32.exe 2>nul
endlocal
