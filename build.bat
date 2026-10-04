@echo off
rem ============================================================================
rem Build ConquerDX9.Hook.sln (Release x86 -> Release\D3DX9_43.dll)
rem Finds MSBuild via vswhere so it works in any Visual Studio/BuildTools
rem install that has the "Desktop development with C++" workload.
rem Automatically builds using the v143 toolset and pushes output over RDP.
rem Usage:  build.bat  [Configuration] [Platform]   (defaults: Release x86)
rem ============================================================================
setlocal

set CONFIG=%1
set PLATFORM=%2
if "%CONFIG%"=="" set CONFIG=Release
if "%PLATFORM%"=="" set PLATFORM=x86

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo ERROR: vswhere not found - is the Visual Studio Installer present?
    exit /b 1
)

rem Prefer a full VS install, fall back to Build Tools.
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
    echo ERROR: no Visual Studio with the C++ toolset found.
    echo Install the "Desktop development with C++" workload, or use the VS
    echo Developer Command Prompt instead.
    exit /b 1
)

set MSBUILD=%VSDIR%\MSBuild\Current\Bin\MSBuild.exe
if not exist "%MSBUILD%" (
    echo ERROR: MSBuild.exe not found under %VSDIR%
    exit /b 1
)

echo Using: %MSBUILD%
rem Pin the v143 toolset so it matches the PlatformToolset declared in the
rem .vcxproj files (VS2022). Change this if you install a newer toolset.
"%MSBUILD%" "%~dp0ConquerDX9.Hook.sln" /m /nologo /verbosity:minimal /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /p:PlatformToolset=v143

if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    exit /b 1
)

rem ============================================================================
rem Assert the Gadget loader actually made it into the linked image.
rem
rem A forwarder-only build of this DLL is perfectly healthy: it loads, the game
rem runs, every D3DX call works. It just never starts Frida. So if the build
rem tree is stale, the mistake is completely silent - which is exactly how it
rem bit us once. src\gadget_loader.cpp carries the marker string
rem DX9HOOK_GADGET_LOADER_V1 and emits it via OutputDebugStringA; grep for it.
rem ============================================================================
set "BUILT_DLL=%~dp0%CONFIG%\D3DX9_43.dll"

findstr /M /C:"DX9HOOK_GADGET_LOADER_V1" "%BUILT_DLL%" >nul 2>&1
if errorlevel 1 (
    echo.
    echo BUILD PRODUCED A PROXY WITHOUT THE GADGET LOADER.
    echo   %BUILT_DLL%
    echo   does not contain the DX9HOOK_GADGET_LOADER_V1 marker.
    echo.
    echo src\gadget_loader.cpp was not compiled in. Check that it is present in
    echo the source tree this script builds, and that ConquerDX9Hook.vcxproj
    echo lists it. If Visual Studio has the solution open, reload it so the new
    echo file is picked up, then build again.
    exit /b 1
)

echo.
echo BUILD OK - output: %BUILT_DLL%  ^(with the debug pdb if enabled^)
echo Verified: the Gadget loader is linked in.

rem ============================================================================
rem Push compiled DLL to physical local machine via RDP
rem ============================================================================
set "SOURCE_DLL=%~dp0Release\D3DX9_43.dll"
set "DEST_DIR=\\tsclient\H\client\Env_DX9"

echo.
echo Pushing DLL to local machine (%DEST_DIR%)...

if not exist "%DEST_DIR%" (
    echo WARNING: Destination "%DEST_DIR%" not found. 
    echo Ensure your Local Drives are shared in your Remote Desktop Connection settings.
) else (
    rem The /Y flag suppresses the overwrite prompt and forces the overwrite
    copy /Y "%SOURCE_DLL%" "%DEST_DIR%\"
    
    if errorlevel 1 (
        echo ERROR: Failed to copy the DLL. Make sure the target file is not currently in use/locked.
    ) else (
        echo SUCCESS: Overwrote D3DX9_43.dll on local machine.
    )

    rem Deploy the Gadget sidecar too, when it has been fetched. The proxy works
    rem without it; with it the game exposes a Frida listener. See USAGE.md.
    if exist "%~dp0deploy\D3DX9_43_44.dll" (
        copy /Y "%~dp0deploy\D3DX9_43_44.dll" "%DEST_DIR%\" >nul
        copy /Y "%~dp0deploy\D3DX9_43_44.config" "%DEST_DIR%\" >nul
        echo SUCCESS: Deployed the Frida Gadget sidecar ^(D3DX9_43_44.dll + .config^).
    ) else (
        echo NOTE: deploy\D3DX9_43_44.dll not found - the proxy will ship alone.
        echo       Run "python third_party\frida\fetch-gadget.py" to add the Gadget.
    )
)

endlocal
