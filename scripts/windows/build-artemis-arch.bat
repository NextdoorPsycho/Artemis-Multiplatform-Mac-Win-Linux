@echo off
setlocal enableDelayedExpansion

echo DEBUG: SCRIPT START - build-artemis-arch.bat called with parameters: %*
echo DEBUG: SCRIPT START - Current time: %date% %time%
echo DEBUG: SCRIPT START - Current directory: %cd%

rem Run from Qt command prompt with working directory set to root of repo
set SCRIPT_DIR=%~dp0
for %%I in ("%SCRIPT_DIR%..\..") do set SOURCE_ROOT=%%~fI

set BUILD_CONFIG=%1

rem Convert to lower case for windeployqt
if /I "%BUILD_CONFIG%"=="debug" (
    set BUILD_CONFIG=debug
    set WIX_MUMS=10
) else (
    if /I "%BUILD_CONFIG%"=="release" (
        set BUILD_CONFIG=release
        set WIX_MUMS=10
    ) else (
        if /I "%BUILD_CONFIG%"=="signed-release" (
            set BUILD_CONFIG=release
            set SIGN=1
            set MUST_DEPLOY_SYMBOLS=1

            rem Fail if there are unstaged changes
            git diff-index --quiet HEAD --
            if !ERRORLEVEL! NEQ 0 (
                echo Signed release builds must not have unstaged changes!
                exit /b 1
            )
        ) else (
            echo Invalid build configuration - expected 'debug' or 'release'
            echo Usage: scripts\windows\build-artemis-arch.bat ^(release^|debug^)
            exit /b 1
        )
    )
)


rem Locate qmake and determine if we're using qmake.exe, qmake.bat, or qmake6.bat
set QMAKE_CMD=
for %%Q in (qmake.bat qmake6.bat qmake.exe qmake6.exe) do (
    where %%Q >nul 2>&1
    if !ERRORLEVEL! EQU 0 if not defined QMAKE_CMD set QMAKE_CMD=%%Q
)
if not defined QMAKE_CMD (
    echo Unable to find QMake. Did you add Qt bins to your PATH?
    goto Error
)

rem Find Qt path to determine our architecture
set QT_PATH=
for %%Q in (qmake.bat qmake6.bat qmake.exe qmake6.exe) do (
    for /F %%i in ('where %%Q 2^>nul') do if not defined QT_PATH set QT_PATH=%%i
)

rem Strip the qmake filename off the end to get the Qt bin directory itself
set QT_PATH=%QT_PATH:\qmake.exe=%
set QT_PATH=%QT_PATH:\qmake.bat=%
set QT_PATH=%QT_PATH:\qmake.cmd=%
set QT_PATH=%QT_PATH:\qmake6.exe=%
set QT_PATH=%QT_PATH:\qmake6.bat=%

echo QT_PATH=%QT_PATH%
if not x%QT_PATH:_arm64=%==x%QT_PATH% (
    set ARCH=arm64

    rem Replace the _arm64 suffix with _64 to get the x64 bin path
    set HOSTBIN_PATH=%QT_PATH:_arm64=_64%
    echo HOSTBIN_PATH=!HOSTBIN_PATH!

    if exist %QT_PATH%\windeployqt.exe (
        echo Using windeployqt.exe from QT_PATH
        set WINDEPLOYQT_CMD=windeployqt.exe
    ) else (
        echo Using windeployqt.exe from HOSTBIN_PATH
        set WINDEPLOYQT_CMD=!HOSTBIN_PATH!\windeployqt.exe --qtpaths %QT_PATH%\qtpaths.bat
    )
) else (
    if not x%QT_PATH:_64=%==x%QT_PATH% (
        set ARCH=x64
        set WINDEPLOYQT_CMD=windeployqt.exe
    ) else (
        if not x%QT_PATH:msvc=%==x%QT_PATH% (
            set ARCH=x86
            set WINDEPLOYQT_CMD=windeployqt.exe
        ) else (
            echo Unable to determine Qt architecture
            goto Error
        )
    )
)

echo Detected target architecture: %ARCH%

set SIGNTOOL_PARAMS=sign /tr http://timestamp.digicert.com /td sha256 /fd sha256 /sha1 8b9d0d682ad9459e54f05a79694bc10f9876e297 /v

set BUILD_ROOT=%SOURCE_ROOT%\build
set BUILD_FOLDER=%BUILD_ROOT%\build-%ARCH%-%BUILD_CONFIG%
set DEPLOY_FOLDER=%BUILD_ROOT%\deploy-%ARCH%-%BUILD_CONFIG%
set INSTALLER_FOLDER=%BUILD_ROOT%\installer-%ARCH%-%BUILD_CONFIG%
set SYMBOLS_FOLDER=%BUILD_ROOT%\symbols-%ARCH%-%BUILD_CONFIG%
set /p VERSION=<%SOURCE_ROOT%\app\version.txt

rem Use the correct VC tools for the specified architecture
if /I "%ARCH%" EQU "x64" (
    rem x64 is a special case that doesn't match %PROCESSOR_ARCHITECTURE%
    set VC_ARCH=AMD64
) else (
    set VC_ARCH=%ARCH%
)

rem If we're not building for the current platform, use the cross compiling toolchain
if /I "%VC_ARCH%" NEQ "%PROCESSOR_ARCHITECTURE%" (
    set VC_ARCH=%PROCESSOR_ARCHITECTURE%_%VC_ARCH%
)

rem Find Visual Studio and set up environment variables directly
set VSWHERE="%SOURCE_ROOT%\scripts\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -property installationPath`) do (
    set "VS_INSTALL_PATH=%%i"
    for /f "delims=" %%j in ('dir /b "%%i\VC\Tools\MSVC"') do set "MSVC_VERSION=%%j"
)

rem For ARM64, don't call vcvarsall.bat manually - let Qt handle it
rem For other architectures, set up the environment
set "VCVARSALL=%VS_INSTALL_PATH%\VC\Auxiliary\Build\vcvarsall.bat"
if /I "%ARCH%" NEQ "arm64" (
    echo Setting up MSVC environment using vcvarsall for %VC_ARCH%
    if exist "%VCVARSALL%" (
        call "%VCVARSALL%" %VC_ARCH%
        if !ERRORLEVEL! NEQ 0 (
            echo ERROR: vcvarsall.bat failed
            goto Error
        )
    ) else (
        echo ERROR: vcvarsall.bat not found at %VCVARSALL%
        goto Error
    )
) else (
    echo Skipping manual vcvarsall.bat for ARM64 - Qt will handle environment setup
)

echo MSVC Environment Setup:
echo   VS Install Path: %VS_INSTALL_PATH%
echo   MSVC Version: %MSVC_VERSION%
echo   Target Arch: %VC_ARCH%
if /I "%ARCH%" NEQ "arm64" (
    echo   VCVARSALL: "%VCVARSALL%"
    echo   INCLUDE: "%INCLUDE%"
    echo   LIB: "%LIB%"
) else (
    echo   Environment: Qt will manage ARM64 cross-compilation environment
)

rem Find VC redistributable DLLs
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -find VC\Redist\MSVC\*\%ARCH%\Microsoft.VC*.CRT`) do set VC_REDIST_DLL_PATH=%%i

echo Cleaning output directories
rmdir /s /q %DEPLOY_FOLDER%
rmdir /s /q %BUILD_FOLDER%
rmdir /s /q %INSTALLER_FOLDER%
rmdir /s /q %SYMBOLS_FOLDER%
mkdir %BUILD_ROOT%
mkdir %DEPLOY_FOLDER%
mkdir %BUILD_FOLDER%
mkdir %INSTALLER_FOLDER%
mkdir %SYMBOLS_FOLDER%

echo Configuring the project
pushd %BUILD_FOLDER%
echo DEBUG: CRITICAL - Successfully changed to build directory: %CD%
echo DEBUG: CRITICAL - BUILD_FOLDER was: %BUILD_FOLDER%
echo Running qmake command: %QMAKE_CMD% %SOURCE_ROOT%\artemis.pro
echo Current directory: %CD%
echo Target architecture: %ARCH%
echo Qt path: %QT_PATH%

rem For ARM64 builds, we need to specify the target platform explicitly
if /I "%ARCH%" EQU "arm64" (
    echo Configuring for ARM64 cross-compilation
    call %QMAKE_CMD% %SOURCE_ROOT%\artemis.pro "CONFIG+=arm64"
    call :CheckQmakeResult
) else (
    call %QMAKE_CMD% %SOURCE_ROOT%\artemis.pro
    call :CheckQmakeResult
)
echo DEBUG: CRITICAL - qmake succeeded, continuing...

echo DEBUG: CRITICAL - After qmake error check, still executing
echo DEBUG: CRITICAL - Current variables: ARCH=%ARCH%, BUILD_CONFIG=%BUILD_CONFIG%
echo DEBUG: CRITICAL - Current directory: %CD%
echo DEBUG: CRITICAL - About to continue to next section...

echo DEBUG: qmake completed successfully, current directory: %CD%

echo DEBUG: CRITICAL - Before Makefile check section
echo DEBUG: About to check Makefile
rem Verify the generated Makefile has the correct architecture
if exist "Makefile" (
    echo Checking Makefile for architecture settings:
    echo DEBUG: Running first findstr command
    findstr /C:"arm64" Makefile
    if !ERRORLEVEL! NEQ 0 echo Warning: arm64 not found in Makefile
    echo DEBUG: Running second findstr command
    findstr /C:"ARCH" Makefile
    if !ERRORLEVEL! NEQ 0 echo Warning: ARCH not found in Makefile
) else (
    echo WARNING: No Makefile found after qmake
)

echo DEBUG: Makefile check complete
echo DEBUG: CRITICAL - After Makefile check section completed
echo DEBUG: CRITICAL - Script still running, about to check ARCH variable
echo DEBUG: SCRIPT FLOW - After Makefile check, ARCH=%ARCH%, BUILD_CONFIG=%BUILD_CONFIG%, CD=%CD%
echo DEBUG: ABOUT TO CHECK ARCH CONDITIONAL - ARCH is currently: %ARCH%
echo DEBUG: Comparing ARCH=%ARCH% with arm64

rem For ARM64 builds, we need to be very explicit about tool paths to avoid PATH issues
if /I "%ARCH%" EQU "arm64" (
    echo DEBUG: Entering ARM64 build section
    
echo DEBUG: Forcing entry into compilation section after qmake
    set "FOUND_CL="
    set "FOUND_NMAKE="
    set "FOUND_QMAKE=%QT_PATH%\qmake.bat"
    
    echo DEBUG: Looking for ARM64 tools...
    
    rem Look for cl.exe in the expected ARM64 cross-compile location
    rem and nmake.exe in the host x64 tools location (nmake is always x64)
    for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -property installationPath`) do (
        for /f "delims=" %%j in ('dir /b "%%i\VC\Tools\MSVC"') do (
            set "CL_PATH=%%i\VC\Tools\MSVC\%%j\bin\Hostx64\arm64\cl.exe"
            set "NMAKE_PATH=%%i\VC\Tools\MSVC\%%j\bin\Hostx64\x64\nmake.exe"
            if exist "!CL_PATH!" set "FOUND_CL=!CL_PATH!"
            if exist "!NMAKE_PATH!" set "FOUND_NMAKE=!NMAKE_PATH!"
        )
    )
    
    echo DEBUG: ARM64 Tools Found:
    echo   qmake: "%FOUND_QMAKE%"
    echo   cl.exe: "!FOUND_CL!"
    echo   nmake: "!FOUND_NMAKE!"
    
    if "!FOUND_CL!"=="" (
        echo ERROR: Could not find cl.exe for ARM64 cross-compilation
        echo VSWHERE path: %VSWHERE%
        echo VS Install path: %VS_INSTALL_PATH%
        echo MSVC Version: %MSVC_VERSION%
        goto Error
    )
    if "!FOUND_NMAKE!"=="" (
        echo ERROR: Could not find nmake.exe for ARM64 cross-compilation
        echo VSWHERE path: %VSWHERE%
        echo VS Install path: %VS_INSTALL_PATH%
        echo MSVC Version: %MSVC_VERSION%
        goto Error
    )
    
    rem Use the exact tool paths instead of relying on PATH
    echo DEBUG: About to run nmake with explicit path for ARM64...
    echo DEBUG: nmake command: "!FOUND_NMAKE!" %BUILD_CONFIG%
    echo DEBUG: Working directory: %CD%
    echo Running nmake with explicit path for ARM64...
    "!FOUND_NMAKE!" %BUILD_CONFIG% > nmake.log 2>&1
    echo DEBUG: nmake completed with exit code: !ERRORLEVEL!
    if !ERRORLEVEL! NEQ 0 (
        echo ERROR: nmake failed for ARM64! Check nmake.log for details.
        echo nmake.log contents:
        type nmake.log
        goto Error
    echo DEBUG: About to run nmake for ARM64...
    )
    echo nmake completed successfully for ARM64
    
) else (
    echo DEBUG: Entering x64/x86 build section
    echo DEBUG: nmake completed for ARM64 with exit code: !ERRORLEVEL!
    rem For x64 builds, use the simpler approach
    if exist "%SOURCE_ROOT%\scripts\jom.exe" (
        echo DEBUG: Using jom.exe for build
        %SOURCE_ROOT%\scripts\jom.exe %BUILD_CONFIG%
    ) else (
        echo DEBUG: Using nmake for build
        rem Capture nmake output to a log file for debugging
        nmake %BUILD_CONFIG% > nmake.log 2>&1
        echo DEBUG: nmake completed with exit code: !ERRORLEVEL!
        if !ERRORLEVEL! NEQ 0 (
            echo ERROR: nmake failed! Check nmake.log for details.
            type nmake.log
            goto Error
        )
    )
)

rem Verify the build actually produced something
echo Verifying build output...
echo DEBUG: Checking if Artemis.exe exists at app\%BUILD_CONFIG%\Artemis.exe
if exist "app\%BUILD_CONFIG%\Artemis.exe" (
    echo SUCCESS: Artemis.exe was built successfully
) else (
    echo ERROR: Artemis.exe was not found after build!
    echo Contents of app directory:
    dir app /s 2>nul
    echo Contents of current directory:
    dir 2>nul
    echo nmake.log contents if exists:
    if exist nmake.log type nmake.log
    goto Error
)

rem Debug: Check what was actually built
echo Checking build output:
dir "app\%BUILD_CONFIG%\*.exe" 2>nul
if !ERRORLEVEL! NEQ 0 echo No exe files found in app\%BUILD_CONFIG%
if exist "app\%BUILD_CONFIG%\Artemis.exe" (
    echo Artemis.exe found, checking architecture...
    file "app\%BUILD_CONFIG%\Artemis.exe" 2>nul
    if !ERRORLEVEL! NEQ 0 echo file command not available
    dumpbin /headers "app\%BUILD_CONFIG%\Artemis.exe" 2>nul | findstr "machine" 2>nul
    if !ERRORLEVEL! NEQ 0 echo dumpbin not available
) else (
    echo ERROR: Artemis.exe was not built!
    dir "app\*" /s 2>nul
    if !ERRORLEVEL! NEQ 0 echo No files in app directory
)
popd

echo Saving PDBs
for /r "%BUILD_FOLDER%" %%f in (*.pdb) do (
    copy "%%f" %SYMBOLS_FOLDER%
    if !ERRORLEVEL! NEQ 0 goto Error
)
copy %SOURCE_ROOT%\libs\windows\lib\%ARCH%\*.pdb %SYMBOLS_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error
7z a %SYMBOLS_FOLDER%\ArtemisDebuggingSymbols-%ARCH%-%VERSION%.zip %SYMBOLS_FOLDER%\*.pdb
if !ERRORLEVEL! NEQ 0 goto Error

echo Copying DLL dependencies
copy %SOURCE_ROOT%\libs\windows\lib\%ARCH%\*.dll %DEPLOY_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error

echo Copying AntiHooking.dll
copy %BUILD_FOLDER%\AntiHooking\%BUILD_CONFIG%\AntiHooking.dll %DEPLOY_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error

echo Copying GC mapping list
copy %SOURCE_ROOT%\app\SDL_GameControllerDB\gamecontrollerdb.txt %DEPLOY_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error

if not x%QT_PATH:\5.=%==x%QT_PATH% (
    echo Copying qt.conf for Qt 5
    copy %SOURCE_ROOT%\app\qt_qt5.conf %DEPLOY_FOLDER%\qt.conf
    if !ERRORLEVEL! NEQ 0 goto Error

    rem Qt 5.15
    set WINDEPLOYQT_ARGS=--no-qmltooling --no-virtualkeyboard
) else (
    rem Qt 6.5+
    set WINDEPLOYQT_ARGS=--no-system-d3d-compiler --no-system-dxc-compiler --skip-plugin-types qmltooling,generic --no-ffmpeg
    set WINDEPLOYQT_ARGS=!WINDEPLOYQT_ARGS! --no-quickcontrols2fusion --no-quickcontrols2imagine --no-quickcontrols2universal
    set WINDEPLOYQT_ARGS=!WINDEPLOYQT_ARGS! --no-quickcontrols2fusionstyleimpl --no-quickcontrols2imaginestyleimpl --no-quickcontrols2universalstyleimpl --no-quickcontrols2windowsstyleimpl
)

echo Deploying Qt dependencies
%WINDEPLOYQT_CMD% --dir %DEPLOY_FOLDER% --%BUILD_CONFIG% --qmldir %SOURCE_ROOT%\app\gui --no-opengl-sw --no-compiler-runtime --no-sql %WINDEPLOYQT_ARGS% %BUILD_FOLDER%\app\%BUILD_CONFIG%\Artemis.exe
if !ERRORLEVEL! NEQ 0 goto Error

echo Deleting unused styles
rem Qt 5.x directories
rmdir /s /q %DEPLOY_FOLDER%\QtQuick\Controls.2\Fusion
rmdir /s /q %DEPLOY_FOLDER%\QtQuick\Controls.2\Imagine
rmdir /s /q %DEPLOY_FOLDER%\QtQuick\Controls.2\Universal
rem Qt 6.5+ directories
rmdir /s /q %DEPLOY_FOLDER%\qml\QtQuick\Controls\Fusion
rmdir /s /q %DEPLOY_FOLDER%\qml\QtQuick\Controls\Imagine
rmdir /s /q %DEPLOY_FOLDER%\qml\QtQuick\Controls\Universal
rmdir /s /q %DEPLOY_FOLDER%\qml\QtQuick\Controls\Windows
rmdir /s /q %DEPLOY_FOLDER%\qml\QtQuick\NativeStyle

if "%SIGN%"=="1" (
    echo Signing deployed binaries
    set FILES_TO_SIGN=%BUILD_FOLDER%\app\%BUILD_CONFIG%\Artemis.exe
    for /r "%DEPLOY_FOLDER%" %%f in (*.dll *.exe) do (
        set FILES_TO_SIGN=!FILES_TO_SIGN! %%f
    )
    signtool %SIGNTOOL_PARAMS% !FILES_TO_SIGN!
    if !ERRORLEVEL! NEQ 0 goto Error
)

echo Building MSI
set DEPLOY_FOLDER=%DEPLOY_FOLDER%
set BUILD_FOLDER=%BUILD_FOLDER%
msbuild %SOURCE_ROOT%\wix\Artemis\Artemis.wixproj -Restore /p:Configuration=%BUILD_CONFIG% /p:Platform=%ARCH% /p:OutputPath=%INSTALLER_FOLDER%\ /p:DEPLOY_FOLDER=%DEPLOY_FOLDER% /p:BUILD_FOLDER=%BUILD_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error

echo Copying application binary to deployment directory
copy %BUILD_FOLDER%\app\%BUILD_CONFIG%\Artemis.exe %DEPLOY_FOLDER%\Artemis.exe
if !ERRORLEVEL! NEQ 0 goto Error

echo Building portable package
rem This must be done after WiX harvesting and signing, since the VCRT dlls are MS signed
rem and should not be harvested for inclusion in the full installer
copy "%VC_REDIST_DLL_PATH%\*.dll" %DEPLOY_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error
rem This file tells Artemis that it's a portable installation
echo. > %DEPLOY_FOLDER%\portable.dat
if !ERRORLEVEL! NEQ 0 goto Error
7z a %INSTALLER_FOLDER%\ArtemisPortable-%ARCH%-%VERSION%.zip %DEPLOY_FOLDER%\*
if !ERRORLEVEL! NEQ 0 goto Error

echo Build successful for Artemis v%VERSION% %ARCH% binaries!
exit /b 0

:Error
echo Build failed!
exit /b 1

:CheckQmakeResult
echo DEBUG: qmake returned with exit code: %ERRORLEVEL%
if %ERRORLEVEL% NEQ 0 (
    echo DEBUG: CRITICAL - qmake failed with exit code %ERRORLEVEL%, going to Error label
    goto Error
)
echo DEBUG: qmake succeeded with exit code: %ERRORLEVEL%
goto :eof

echo DEBUG: SCRIPT FLOW - End of script reached without entering ARM64 build section
