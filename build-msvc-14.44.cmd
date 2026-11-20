@echo off
rem Assimp MSVC Ninja build pinned to VS2022 toolset 14.44.35207.
rem
rem Root cause of STL1001 ("expected MSVC 19.44 or newer"):
rem   cmake --build reused build-msvc configured with 14.38 cl.exe while
rem   INCLUDE pointed at 14.44 headers. cl and STL must be the same folder.
rem
rem Usage:
rem   build-msvc-14.44.cmd              reconfigure+build (keeps build dir)
rem   build-msvc-14.44.cmd clean        wipe build-msvc then configure+build
rem
rem Env: VCVARS_VER (default 14.44.35207), VSINSTALLDIR, BUILD_DIR
setlocal enabledelayedexpansion

set "CONFIG=Release"
if "%VCVARS_VER%"=="" set "VCVARS_VER=14.44.35207"
if "%VSINSTALLDIR%"=="" set "VSINSTALLDIR=D:\Tools\VS2022BuildTools"
if "%BUILD_DIR%"=="" set "BUILD_DIR=build-msvc"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%"

set "VCVARS=%VSINSTALLDIR%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo ERROR: missing vcvars64: %VCVARS%
  exit /b 1
)

set "MSVC_TOOLS=%VSINSTALLDIR%\VC\Tools\MSVC\%VCVARS_VER%"
if not exist "%MSVC_TOOLS%\bin\Hostx64\x64\cl.exe" (
  echo ERROR: missing %MSVC_TOOLS%\bin\Hostx64\x64\cl.exe
  echo Available under %VSINSTALLDIR%\VC\Tools\MSVC\:
  dir /b "%VSINSTALLDIR%\VC\Tools\MSVC%"
  exit /b 1
)

rem Always pin; do not reuse a Dev Shell that left 14.38 on PATH.
call "%VCVARS%" -vcvars_ver=%VCVARS_VER%
if errorlevel 1 (
  echo ERROR: vcvars64 failed for %VCVARS_VER%
  exit /b 1
)

set "CC="
set "CXX="
set "CFLAGS="
set "CXXFLAGS="
set "CPPFLAGS="

where cl >nul 2>nul
if errorlevel 1 (
  echo ERROR: cl.exe not on PATH after vcvars
  exit /b 1
)
for /f "delims=" %%A in ('where cl') do (
  set "CL_PATH=%%A"
  goto :got_cl
)
:got_cl
echo cl: %CL_PATH%
echo %CL_PATH% | findstr /i "\\%VCVARS_VER%\\bin\\Hostx64\\x64\\cl.exe" >nul
if errorlevel 1 (
  echo ERROR: cl is not %VCVARS_VER% Hostx64\x64 - refuse mismatched STL
  echo got: %CL_PATH%
  exit /b 1
)

if /i "%~1"=="clean" (
  echo Wiping %BUILD_DIR% ...
  if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

if exist "%BUILD_DIR%\CMakeCache.txt" (
  findstr /c:"MSVC\\%VCVARS_VER%\\bin\\Hostx64\\x64\\cl.exe" "%BUILD_DIR%\CMakeCache.txt" >nul
  if errorlevel 1 (
    echo ERROR: %BUILD_DIR% was configured with a different cl.exe
    echo Re-run with: build-msvc-14.44.cmd clean
    findstr /b "CMAKE_CXX_COMPILER:" "%BUILD_DIR%\CMakeCache.txt"
    exit /b 1
  )
)

rem If configure fails on kilogramsPerUnit, delete:
rem   %BUILD_DIR%\_deps\tinyusdz_repo-src
rem   %BUILD_DIR%\_deps\tinyusdz_repo-subbuild
cmake -G Ninja -S . -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%CONFIG% ^
  -DCMAKE_C_COMPILER="%MSVC_TOOLS%\bin\Hostx64\x64\cl.exe" ^
  -DCMAKE_CXX_COMPILER="%MSVC_TOOLS%\bin\Hostx64\x64\cl.exe" ^
  -DASSIMP_WARNINGS_AS_ERRORS=OFF ^
  -DASSIMP_BUILD_USD_IMPORTER=ON ^
  -DASSIMP_BUILD_TESTS=OFF ^
  -DASSIMP_BUILD_ASSIMP_TOOLS=ON ^
  -DASSIMP_BUILD_SAMPLES=OFF ^
  -DASSIMP_BUILD_DRACO_STATIC=ON ^
  -DBUILD_SHARED_LIBS=ON
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1

echo OK: %BUILD_DIR%\bin\assimp-vc143-mt.dll  ^(toolset %VCVARS_VER%^)
exit /b 0
