@echo off
rem Build audiodiag32.exe - the 32-bit twin of audiodiag.exe.
rem
rem Worth having for the same reason dstest32 was: Stage 5bp proved this stack
rem behaves differently for a 32-bit caller, and XP x64's volume control is a
rem 32-bit process. If the mixer sweep reads differently between the two
rem builds, that is a second instance of the same class of bug as the
rem DirectSound one and should be treated as a single investigation.
rem
rem /SUBSYSTEM:CONSOLE,5.01 for 32-bit (5.02 is the x64/Server 2003 stamp).

setlocal
set DDK=C:\WinDDK\7600.16385.1

call "%DDK%\bin\setenv.bat" %DDK% fre x86 WNET no_oacr
if errorlevel 1 goto :fail

cd /d "%~dp0"
if exist audiodiag32.exe del audiodiag32.exe
if exist audiodiag32.obj del audiodiag32.obj

cl /nologo /W3 /O2 /MT /GS- /D_CRT_SECURE_NO_DEPRECATE /DDIRECTSOUND_VERSION=0x0900 ^
   /I"%DDK%\inc\api" /I"%DDK%\inc\crt" ^
   /Feaudiodiag32.exe /Foaudiodiag32.obj ^
   audiodiag.c ^
   /link /SUBSYSTEM:CONSOLE,5.01 /MACHINE:X86 /NODEFAULTLIB:libc.lib ^
   /LIBPATH:"%DDK%\lib\wnet\i386" /LIBPATH:"%DDK%\lib\crt\i386" ^
   dsound.lib winmm.lib ole32.lib advapi32.lib user32.lib kernel32.lib

if errorlevel 1 goto :fail
if not exist audiodiag32.exe goto :fail

echo.
echo ==== BUILD OK ====
dir audiodiag32.exe | findstr /i audiodiag
endlocal
exit /b 0

:fail
echo.
echo ==== BUILD FAILED ====
endlocal
exit /b 1
