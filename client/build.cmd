@echo off
setlocal
if "%~3"=="" (
    echo Usage: build.cmd LIVEKIT_SDK HEADER_DEPENDENCIES BUILD_OUTPUT
    exit /b 1
)
if not exist "%~3" mkdir "%~3"
pushd "%~3"
rc /nologo /i "%~dp0." /fo TinySpeak.res "%~dp0TinySpeak.rc"
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /MD /O2 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DNOMINMAX ^
    /I"%~1\include" /I"%~2" "%~dp0main.cpp" "%~dp0network.cpp" "%~dp0ui.cpp" "%~dp0server_settings.cpp" "%~dp0voice.cpp" "%~dp0chat_view.cpp" "%~dp0chat_sounds.cpp" "%~dp0host_process.cpp" ^
    TinySpeak.res /Fe:TinySpeak.exe /link /SUBSYSTEM:WINDOWS,10.00 /MANIFEST:EMBED /MANIFESTINPUT:"%~dp0TinySpeak.manifest" /MANIFESTINPUT:"%~dp0standard-user.manifest" ^
    /LIBPATH:"%~1\lib" livekit.lib user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib ^
    uuid.lib advapi32.lib winhttp.lib bcrypt.lib dwmapi.lib uxtheme.lib winmm.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
copy /y "%~1\bin\livekit.dll" . >nul
copy /y "%~1\bin\livekit_ffi.dll" . >nul
cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 ^
    "%~dp0demo.cpp" TinySpeak.res /Fe:"Launch TinySpeak.exe" /link /SUBSYSTEM:WINDOWS,10.00 /MANIFEST:EMBED /MANIFESTINPUT:"%~dp0standard-user.manifest" shell32.lib user32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
popd
