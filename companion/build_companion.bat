@echo off
REM Builds the in-frame overlay companion DLL (mctde_overlay.dll).
REM Drop the result in DATA\mctde-Link-Chainload\ so mctde-Link chainloads it.
setlocal
set VCVARS=Z:\Visual Studio\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (
  echo vcvars32.bat not found at "%VCVARS%" - edit this path for your VS install.
  exit /b 1
)
call "%VCVARS%"
cd /d "%~dp0"
if not exist bin mkdir bin
cd bin
cl /nologo /O2 /MT /LD /EHsc "..\OverlayCompanion.cpp" /Fe:mctde_overlay.dll
set ERR=%ERRORLEVEL%
del /q *.obj *.exp *.lib 2>nul
if %ERR% NEQ 0 (echo BUILD FAILED & exit /b %ERR%)
echo.
echo Built: %~dp0bin\mctde_overlay.dll
echo Deploy to: DATA\mctde-Link-Chainload\mctde_overlay.dll
endlocal
