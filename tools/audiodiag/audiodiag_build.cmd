@echo off
rem Build audiodiag.exe - user-mode x64 console app for Windows XP x64.
rem
rem Targets WNET/amd64 because WNET is the Server 2003 SP2 build environment,
rem which is exactly the kernel and CRT vintage XP x64 runs. Links /MT against
rem the WDK's static CRT so the resulting .exe depends on nothing but the OS -
rem there is no Visual C++ redistributable on the target machine.
rem
rem /GS- avoids needing BufferOverflowU.lib; there is no untrusted input here.
rem /SUBSYSTEM:CONSOLE,5.02 stamps the PE for Server 2003 / XP x64 so the
rem loader will accept it (the default subsystem version is too new).

setlocal
set DDK=C:\WinDDK\7600.16385.1

call "%DDK%\bin\setenv.bat" %DDK% fre x64 WNET no_oacr
if errorlevel 1 goto :fail

cd /d "%~dp0"
if exist audiodiag.exe del audiodiag.exe
if exist audiodiag.obj del audiodiag.obj

cl /nologo /W3 /O2 /MT /GS- /D_CRT_SECURE_NO_DEPRECATE /DDIRECTSOUND_VERSION=0x0900 ^
   /I"%DDK%\inc\api" /I"%DDK%\inc\crt" ^
   audiodiag.c ^
   /link /SUBSYSTEM:CONSOLE,5.02 /MACHINE:AMD64 /NODEFAULTLIB:libc.lib ^
   /LIBPATH:"%DDK%\lib\wnet\amd64" /LIBPATH:"%DDK%\lib\crt\amd64" ^
   dsound.lib winmm.lib ole32.lib user32.lib kernel32.lib

if errorlevel 1 goto :fail
if not exist audiodiag.exe goto :fail

echo.
echo ==== BUILD OK ====
dir audiodiag.exe | findstr /i audiodiag
endlocal
exit /b 0

:fail
echo.
echo ==== BUILD FAILED ====
endlocal
exit /b 1
