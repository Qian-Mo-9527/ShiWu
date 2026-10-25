@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT goto manualtools
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" goto manualtools
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
goto compile

:manualtools
rem Installer registration may be unavailable even while the tools remain installed.
for /d %%y in ("%ProgramFiles%\Microsoft Visual Studio\*") do for /d %%e in ("%%~fy\*") do for /f "delims=" %%v in ('dir /b /ad /o-n "%%~fe\VC\Tools\MSVC" 2^>nul') do if not defined VCTOOLS if exist "%%~fe\VC\Tools\MSVC\%%v\bin\Hostx64\x64\cl.exe" if exist "%%~fe\VC\Tools\MSVC\%%v\include\vcruntime.h" if exist "%%~fe\VC\Tools\MSVC\%%v\lib\x64\libcmt.lib" set "VCTOOLS=%%~fe\VC\Tools\MSVC\%%v"
set "SDKROOT=%ProgramFiles(x86)%\Windows Kits\10"
for /f "delims=" %%s in ('dir /b /ad /o-n "%SDKROOT%\Include" 2^>nul') do if not defined SDKVER if exist "%SDKROOT%\Include\%%s\um\Windows.h" if exist "%SDKROOT%\Lib\%%s\um\x64\kernel32.lib" if exist "%SDKROOT%\bin\%%s\x64\rc.exe" set "SDKVER=%%s"
if not defined VCTOOLS (
  echo 未找到本机可用的 Visual Studio C/C++ 编译工具。
  exit /b 1
)
if not defined SDKVER (
  echo 未找到本机可用的 Windows SDK。
  exit /b 1
)
set "INCLUDE=%VCTOOLS%\include;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\winrt"
set "LIB=%VCTOOLS%\lib\x64;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"
set "PATH=%VCTOOLS%\bin\Hostx64\x64;%SDKROOT%\bin\%SDKVER%\x64;%PATH%"

:compile
if not exist build mkdir build
rc /nologo /fo build\frontend.res src\frontend.rc
if errorlevel 1 exit /b 1
rc /nologo /fo build\backend.res src\backend.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /TC /utf-8 /DSQLITE_THREADSAFE=1 /DSQLITE_OMIT_LOAD_EXTENSION /c vendor\sqlite3.c /Fobuild\sqlite3.obj
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /TC /std:c17 /utf-8 /DUNICODE /D_UNICODE /c src\ui.c src\store.c /Fo"build\\"
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /TC /std:c17 /utf-8 /DUNICODE /D_UNICODE /c src\frontend.c /Fobuild\frontend.obj
if errorlevel 1 exit /b 1
link /nologo build\frontend.res build\frontend.obj build\ui.obj build\store.obj build\sqlite3.obj /OUT:"二手交易.exe" /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comdlg32.lib comctl32.lib ole32.lib windowscodecs.lib shlwapi.lib bcrypt.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /TC /std:c17 /utf-8 /DUNICODE /D_UNICODE /c src\backend.c /Fobuild\backend.obj
if errorlevel 1 exit /b 1
link /nologo build\backend.res build\backend.obj build\ui.obj build\store.obj build\sqlite3.obj /OUT:"本地数据管理.exe" /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comdlg32.lib comctl32.lib ole32.lib windowscodecs.lib shlwapi.lib bcrypt.lib uxtheme.lib
if errorlevel 1 exit /b 1
echo 两个程序编译成功。
