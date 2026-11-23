@echo off
setlocal
if "%~1"=="" (
    echo Usage: build-windows.cmd BUILD_OUTPUT
    exit /b 1
)
set "output=%~f1"
if not exist "%output%" mkdir "%output%"
pushd "%~dp0"
set "CGO_ENABLED=0"
set "GOOS=windows"
set "GOARCH=amd64"
set "GOAMD64=v1"
go build -buildvcs=false -trimpath -ldflags="-s -w" -o "%output%\kanal-relay.exe" .
if errorlevel 1 exit /b %errorlevel%
go build -buildvcs=false -trimpath -tags embedded_voice -ldflags="-s -w" -o "%output%\TinySpeak Host.exe" .
if errorlevel 1 exit /b %errorlevel%
go run ./internal/tools/fetch-link "%output%"
if errorlevel 1 exit /b %errorlevel%
for %%F in ("kanal-relay.exe" "TinySpeak Host.exe" "TinySpeak Link.exe") do (
    mt.exe -nologo -manifest "..\client\standard-user.manifest" "-outputresource:%output%\%%~F;#1"
    if errorlevel 1 exit /b 1
)
popd
