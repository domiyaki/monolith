@echo off
setlocal
REM Build the x64 native proxy. Usage: build.bat [output-directory]
REM MONOLITH_BUILD_OUTPUT_DIRECTORY supplies the default output override.
REM MONOLITH_BUILD_INTERMEDIATE_DIRECTORY optionally separates compiler artifacts.
set "MONOLITH_SOURCE_DIR=%~dp0"
set "MONOLITH_OUTPUT_DIR=%MONOLITH_BUILD_OUTPUT_DIRECTORY%"
if not "%~1"=="" set "MONOLITH_OUTPUT_DIR=%~1"
if not defined MONOLITH_OUTPUT_DIR set "MONOLITH_OUTPUT_DIR=%~dp0..\..\Binaries"
for %%I in ("%MONOLITH_OUTPUT_DIR%") do set "MONOLITH_OUTPUT_DIR=%%~fI"
set "MONOLITH_INTERMEDIATE_DIR=%MONOLITH_BUILD_INTERMEDIATE_DIRECTORY%"
if not defined MONOLITH_INTERMEDIATE_DIR set "MONOLITH_INTERMEDIATE_DIR=%MONOLITH_OUTPUT_DIR%\MonolithProxyIntermediate"
for %%I in ("%MONOLITH_INTERMEDIATE_DIR%") do set "MONOLITH_INTERMEDIATE_DIR=%%~fI"

REM Discover Visual Studio instead of assuming an edition or installation drive.
set "MONOLITH_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%MONOLITH_VSWHERE%" goto :compiler_from_path
set "MONOLITH_VS_INSTALL="
for /f "usebackq delims=" %%I in (`"%MONOLITH_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "MONOLITH_VS_INSTALL=%%I"
if not defined MONOLITH_VS_INSTALL goto :compiler_from_path
call "%MONOLITH_VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

:compiler_from_path
where cl.exe >nul 2>&1 || goto :missing_compiler
if defined VSCMD_ARG_TGT_ARCH if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :wrong_architecture
if not exist "%MONOLITH_OUTPUT_DIR%" mkdir "%MONOLITH_OUTPUT_DIR%" || exit /b 1
if not exist "%MONOLITH_INTERMEDIATE_DIR%" mkdir "%MONOLITH_INTERMEDIATE_DIR%" || exit /b 1
pushd "%MONOLITH_INTERMEDIATE_DIR%" || exit /b 1
cl.exe /nologo /EHsc /std:c++17 /O2 /MT /utf-8 /I "%MONOLITH_SOURCE_DIR%ThirdParty" "%MONOLITH_SOURCE_DIR%monolith_proxy.cpp" /Fo"%MONOLITH_INTERMEDIATE_DIR%\monolith_proxy.obj" /Fd"%MONOLITH_INTERMEDIATE_DIR%\monolith_proxy.pdb" /Fe"%MONOLITH_OUTPUT_DIR%\monolith_proxy.exe" /link winhttp.lib user32.lib shell32.lib advapi32.lib bcrypt.lib || goto :build_failed
popd
echo Built: "%MONOLITH_OUTPUT_DIR%\monolith_proxy.exe"
exit /b 0

:build_failed
popd
echo ERROR: Native proxy compilation or linking failed. 1>&2
exit /b 1

:missing_compiler
echo ERROR: Install Visual Studio C++ x64 build tools, or run from an x64 Developer Command Prompt. 1>&2
exit /b 1

:wrong_architecture
echo ERROR: An x64 MSVC environment is required. 1>&2
exit /b 1
