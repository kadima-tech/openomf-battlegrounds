@echo off
rem Double-click in File Explorer, or run launch.cmd from a terminal (Windows 10 or 11).
rem Rebuilds dist\Battlegrounds with MSYS2 when the sources changed, then opens the launcher.
rem Missing build tools (MSYS2 and its packages) are installed only after you confirm.
setlocal
set "REPO=%~dp0"
set "REPO=%REPO:~0,-1%"
set "APP=%REPO%\dist\Battlegrounds\Battlegrounds.exe"
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
if exist "%BASH%" goto build

echo Building Battlegrounds needs MSYS2 (https://www.msys2.org), which provides its compilers.
echo MSYS2 was not found in %MSYS2_ROOT%. Set MSYS2_ROOT if it is installed somewhere else.
if /i not "%MSYS2_ROOT%"=="C:\msys64" goto no_msys2
where winget >nul 2>nul
if errorlevel 1 goto no_winget
set "ANSWER="
set /p "ANSWER=Install MSYS2 now with winget? About 500 MB; Windows asks for administrator approval. [y/N] "
if /i not "%ANSWER:~0,1%"=="y" goto no_msys2
winget install --id MSYS2.MSYS2 --exact --source winget --accept-package-agreements --accept-source-agreements
if exist "%BASH%" goto build
echo MSYS2 did not install into %MSYS2_ROOT%.
goto no_msys2

:no_winget
echo winget, the Windows package manager, is not available. Install "App Installer" from the
echo Microsoft Store and run launch.cmd again, or install MSYS2 from https://www.msys2.org.

:no_msys2
if exist "%APP%" goto existing
pause
exit /b 1

:existing
echo Starting the existing build without checking it is current.
goto run

:build
rem launch.sh offers to install any missing MSYS2 packages, then builds when needed.
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
pushd "%REPO%"
"%BASH%" -l "%REPO:\=/%/launcher/windows/launch.sh"
set "STATUS=%ERRORLEVEL%"
popd
if "%STATUS%"=="0" goto run
echo.
echo The build did not finish; the messages above say why.
pause
exit /b %STATUS%

:run
set "OPENBG_PROJECT_ROOT=%REPO%"
start "" "%APP%" %*
