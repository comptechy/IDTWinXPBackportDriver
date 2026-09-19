@echo off
call C:\WinDDK\7600.16385.1\bin\setenv.bat C:\WinDDK\7600.16385.1 fre x86 WNET no_oacr
cd /d "%~dp0"
cl /nologo /W3 /O2 /MT /GS- /D_CRT_SECURE_NO_DEPRECATE /DDIRECTSOUND_VERSION=0x0900 /IC:/WinDDK/7600.16385.1\inc\api /IC:/WinDDK/7600.16385.1\inc\crt /Fewmpdiag32.exe /Fowmpdiag32.obj wmpdiag.c /link /NODEFAULTLIB:libc.lib /SUBSYSTEM:CONSOLE,5.01 /MACHINE:X86 /LIBPATH:C:/WinDDK/7600.16385.1\lib\wnet\i386 /LIBPATH:C:/WinDDK/7600.16385.1\lib\crt\i386 dsound.lib winmm.lib ole32.lib advapi32.lib user32.lib kernel32.lib
