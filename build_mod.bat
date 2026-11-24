@echo off
setlocal
cd /d "%~dp0"
where node.exe >nul 2>nul
if errorlevel 1 goto node_missing
node cloudflare\build-config.mjs %*
if errorlevel 1 exit /b 1
where msbuild.exe >nul 2>nul
if not errorlevel 1 goto build
set "MW_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%MW_VSWHERE%" goto missing
for /f "usebackq delims=" %%I in (`"%MW_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "MW_VS=%%I"
if not defined MW_VS goto missing
call "%MW_VS%\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=amd64
if errorlevel 1 exit /b 1
:build
msbuild MWLeaderboard.sln /m /p:Configuration=Release /p:Platform=Win32
if errorlevel 1 exit /b 1
if not exist player-release mkdir player-release
copy /y bin\MWLeaderboard.asi player-release\MWLeaderboard.asi >nul
if errorlevel 1 exit /b 1
exit /b %errorlevel%
:node_missing
echo Install Node.js 22 or newer to prepare the embedded server address.
exit /b 1
:missing
echo Install the Desktop development with C++ workload in Visual Studio.
exit /b 1
