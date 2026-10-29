@echo off
rem Builds the fake game (32 bit) into this folder. Needs Visual Studio with C++ (x86).
rem Usage: build.bat   then   fake_game.exe 20
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDEV="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDEV=%%i\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEV%" (echo Visual Studio not found & exit /b 1)
call "%VSDEV%" -arch=x86 -no_logo || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /LD fake_d2common.cpp /Fe:D2COMMON.dll /link /NOLOGO || exit /b 1
cl /nologo /O2 /EHsc fake_game.cpp /Fe:fake_game.exe /link /NOLOGO user32.lib gdi32.lib || exit /b 1
del *.obj *.exp *.lib 2>nul
echo Built fake_game.exe + D2COMMON.dll
