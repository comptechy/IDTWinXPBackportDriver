@echo off
rem Stage 5bm: 32-bit kstest.  The x64 kstest opens our KS pin directly and
rem plays a tone, bypassing sysaudio AND dsound.  Building it x86 answers
rem "can a WOW64 process open and stream our KS pin at all?", which splits
rem "dsound.dll is at fault" from "WOW64 KS access to our filter is at fault".
call C:\WinDDK\7600.16385.1\bin\setenv.bat C:\WinDDK\7600.16385.1 fre x86 WNET no_oacr
cd /d "%~dp0"
cl /nologo /W3 /O2 /MT /GS- /D_CRT_SECURE_NO_DEPRECATE /IC:/WinDDK/7600.16385.1\inc\api /IC:/WinDDK/7600.16385.1\inc\crt /Fekstest32.exe /Fokstest32.obj kstest.c /link /NODEFAULTLIB:libc.lib /SUBSYSTEM:CONSOLE,5.01 /MACHINE:X86 /LIBPATH:C:/WinDDK/7600.16385.1\lib\wnet\i386 /LIBPATH:C:/WinDDK/7600.16385.1\lib\crt\i386 ksuser.lib setupapi.lib advapi32.lib winmm.lib ole32.lib user32.lib kernel32.lib
