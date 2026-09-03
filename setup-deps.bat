@echo off
setlocal
cd /d "%~dp0"

set "VCPKG_EXE="
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\vcpkg.exe" set "VCPKG_EXE=%VCPKG_ROOT%\vcpkg.exe"
if not defined VCPKG_EXE for %%P in (vcpkg.exe) do if not "%%~$PATH:P"=="" set "VCPKG_EXE=%%~$PATH:P"
if not defined VCPKG_EXE if exist "C:\vcpkg\vcpkg.exe" set "VCPKG_EXE=C:\vcpkg\vcpkg.exe"
if not defined VCPKG_EXE if exist "C:\dev\vcpkg\vcpkg.exe" set "VCPKG_EXE=C:\dev\vcpkg\vcpkg.exe"
if not defined VCPKG_EXE if exist "%USERPROFILE%\vcpkg\vcpkg.exe" set "VCPKG_EXE=%USERPROFILE%\vcpkg\vcpkg.exe"

if not defined VCPKG_EXE (
  echo ERROR: could not find vcpkg.exe.
  echo Set VCPKG_ROOT to your vcpkg folder, or run this from a shell where vcpkg is on PATH.
  exit /b 1
)

echo Using %VCPKG_EXE%
"%VCPKG_EXE%" install --triplet x86-windows-static
if errorlevel 1 (
  echo.
  echo vcpkg install FAILED - see the log paths above.
  exit /b 1
)

echo.
echo Done. Headers/libs are in vcpkg_installed\x86-windows-static\
dir /b vcpkg_installed\x86-windows-static\include 2>nul
endlocal
