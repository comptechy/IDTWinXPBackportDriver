@echo off
call C:\WinDDK\7600.16385.1\bin\setenv.bat C:\WinDDK\7600.16385.1 fre x64 WNET no_oacr
cd /d "%~dp0"
cl /nologo /W3 /O2 /MT /GS- /D_CRT_SECURE_NO_DEPRECATE /DDIRECTSOUND_VERSION=0x0900 /IC:/WinDDK/7600.16385.1\inc\api /IC:/WinDDK/7600.16385.1\inc\crt /Fewmpdiag64.exe /Fowmpdiag64.obj wmpdiag.c /link /NODEFAULTLIB:libc.lib /SUBSYSTEM:CONSOLE,5.02 /MACHINE:AMD64 /LIBPATH:C:/WinDDK/7600.16385.1\lib\wnet\amd64 /LIBPATH:C:/WinDDK/7600.16385.1\lib\crt\amd64 dsound.lib winmm.lib ole32.lib advapi32.lib user32.lib kernel32.lib
