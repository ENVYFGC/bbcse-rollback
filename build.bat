@echo off
:: Builds dinput8.dll (32-bit) for BlazBlue: Continuum Shift Extend (Steam).
:: Needs Visual Studio 2019/2022 with the "Desktop development with C++" workload.
:: BBCSE.exe is 32-bit: the x86 toolchain is required (an x64 DLL will not load).
::   build.bat          release build
::   build.bat dev      + developer tools (F2-F11 hotkeys: sync test, recorder, save/load state)
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo [FAIL] vswhere.exe not found: install Visual Studio & exit /b 1 )
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo [FAIL] no Visual Studio with C++ tools found & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || ( echo [FAIL] vcvars32 & exit /b 1 )

set "DEFS="
if /i "%~1"=="dev" set "DEFS=/DBBCSE_DEV_TOOLS"

if not exist "%~dp0build" mkdir "%~dp0build"
pushd "%~dp0build"
set "G=%~dp0ggpo"
set "I=%~dp0imgui"
cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /LD /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS /DDIRECTINPUT_VERSION=0x0800 %DEFS% ^
   /I"%G%" /I"%G%\include" /I"%G%\network" /I"%G%\backends" /I"%I%" ^
   "%~dp0src\dllmain.cpp" "%~dp0src\overlay.cpp" ^
   "%G%\main.cpp" "%G%\sync.cpp" "%G%\input_queue.cpp" "%G%\game_input.cpp" "%G%\bitvector.cpp" ^
   "%G%\log.cpp" "%G%\poll.cpp" "%G%\timesync.cpp" "%G%\platform_windows.cpp" ^
   "%G%\network\udp.cpp" "%G%\network\udp_proto.cpp" ^
   "%G%\backends\p2p.cpp" "%G%\backends\synctest.cpp" "%G%\backends\spectator.cpp" ^
   "%I%\imgui.cpp" "%I%\imgui_draw.cpp" "%I%\imgui_tables.cpp" "%I%\imgui_widgets.cpp" ^
   "%I%\imgui_impl_dx9.cpp" "%I%\imgui_impl_win32.cpp" ^
   /Fe:dinput8.dll ^
   /link /DEF:"%~dp0src\exports.def" /IMPLIB:proxy.lib /MACHINE:X86 dinput8.lib dxguid.lib user32.lib ws2_32.lib winmm.lib dwmapi.lib imm32.lib
set RC=%errorlevel%
popd
if %RC% neq 0 ( echo [FAIL] build failed rc=%RC% & exit /b %RC% )
echo [OK] built %~dp0build\dinput8.dll
