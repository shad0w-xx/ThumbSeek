@echo off
rem Builds ThumbSeek.dll (FFmpeg-based) and ThumbSeek-setup.exe with the Visual Studio Build Tools.
rem FFmpeg (headers, import libs, DLLs) comes from the FFmpeg-Builds lgpl-shared zip extracted in ffmpeg_dl\.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "delims=" %%i in ('call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS=%%i"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (echo Visual Studio Build Tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
for /d %%d in (ffmpeg_dl\ffmpeg-*-win64-lgpl-shared*) do set "FF=%%d"
if not exist "%FF%\include\libavcodec\avcodec.h" (echo FFmpeg not found in ffmpeg_dl & exit /b 1)

rc /nologo /fo ThumbSeek.res version.rc || exit /b 1
rc /nologo /dSETUP /fo setup.res version.rc || exit /b 1
cl /nologo /O2 /W3 /EHsc /MT /DUNICODE /D_UNICODE /I"%FF%\include" /LD ThumbSeek.cpp ThumbSeek.res /link /DEF:ThumbSeek.def ^
   /LIBPATH:"%FF%\lib" avformat.lib avcodec.lib avutil.lib swscale.lib ^
   shlwapi.lib ole32.lib gdi32.lib advapi32.lib shell32.lib || exit /b 1
cl /nologo /O2 /W3 /EHsc /MT /DUNICODE /D_UNICODE setup.cpp setup.res /Fe:ThumbSeek-setup.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED ^
   comctl32.lib shell32.lib ole32.lib advapi32.lib user32.lib gdi32.lib || exit /b 1
rem The FFmpeg DLLs ThumbSeek.dll needs, next to it so setup can install them together.
for %%f in (avformat avcodec avutil swscale swresample) do copy /y "%FF%\bin\%%f-*.dll" . >nul || exit /b 1
del /q *.obj *.exp *.lib *.res 2>nul
