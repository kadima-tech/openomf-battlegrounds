@echo off
rem Double-click in File Explorer, or run launch.cmd from a terminal (Windows 10 or 11).
rem Rebuilds dist\Battlegrounds with MSYS2 when the sources changed, then opens the launcher.
setlocal
set "REPO=%~dp0"
set "REPO=%REPO:~0,-1%"
set "APP=%REPO%\dist\Battlegrounds\Battlegrounds.exe"
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
if not exist "%BASH%" (
    echo Building Battlegrounds needs MSYS2 ^(https://www.msys2.org^); see launcher\README.md.
    echo Set MSYS2_ROOT if it is installed somewhere other than C:\msys64.
    if exist "%APP%" (
        echo Starting the existing build without checking it is current.
        goto run
    )
    pause
    exit /b 1
)
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
pushd "%REPO%"
"%BASH%" -l "%REPO:\=/%/launcher/windows/launch.sh"
set "STATUS=%ERRORLEVEL%"
popd
if not "%STATUS%"=="0" (
    echo The build failed; see the messages above.
    pause
    exit /b %STATUS%
)
:run
set "OPENBG_PROJECT_ROOT=%REPO%"
start "" "%APP%" %*
