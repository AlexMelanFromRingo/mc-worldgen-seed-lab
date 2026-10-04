@echo off
rem Сборка libmcgen_cuda для Windows (nvcc + MSVC). Запускать в «x64 Native Tools Command Prompt for VS» ИЛИ в обычной консоли: тогда vcvars64.bat находится сам (vswhere).
rem   libmcgen\gpu\build.bat                      -> libmcgen\build\gpu\mcgen_cuda.dll  (архитектура — по nvidia-smi; не вышло — sm_75 sm_80 sm_86 sm_89; плюс PTX compute_75 вперёд)
rem   set CUDA_ARCH=75 80 86 89 && libmcgen\gpu\build.bat   (явный список архитектур; sm_120 — CUDA 12.8+)
rem   set OUT=C:\mcgen\gpu && libmcgen\gpu\build.bat       (другой каталог результата)
rem Бит-точность: --fmad=false, без fast-math; хост-код — /fp:strict. cudart и CRT слинкованы статически: нужен только драйвер NVIDIA (nvcuda.dll).
rem Положите результат рядом с mcgen.dll (blender\mcgen_addon\lib\windows-x64\) или в <кэш аддона>\gpu\ : libmcgen найдёт библиотеку сама.
rem Проще всё это делает кнопка «Build GPU library» в аддоне (Stats ▸ Compute).
setlocal enabledelayedexpansion
set HERE=%~dp0
where cl >nul 2>nul
if not errorlevel 1 goto have_cl
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" goto no_cl
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_cl
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 goto no_cl
:have_cl
if not "%CUDA_ARCH%"=="" goto have_arch
for /f "tokens=1,2 delims=." %%a in ('nvidia-smi --query-gpu^=compute_cap --format^=csv^,noheader 2^>nul') do set CUDA_ARCH=%%a%%b
if "%CUDA_ARCH%"=="" set CUDA_ARCH=75 80 86 89
:have_arch
if "%OUT%"=="" set OUT=%HERE%..\build\gpu
if not exist "%OUT%" mkdir "%OUT%"
if "%CUDA_PTX_ARCH%"=="" set CUDA_PTX_ARCH=75
set GEN=
for %%a in (%CUDA_ARCH%) do set GEN=!GEN! -gencode arch=compute_%%a,code=sm_%%a
set GEN=%GEN% -gencode arch=compute_%CUDA_PTX_ARCH%,code=compute_%CUDA_PTX_ARCH%
echo nvcc: архитектуры %CUDA_ARCH% + PTX %CUDA_PTX_ARCH%
nvcc -O3 -std=c++17 --shared %GEN% --fmad=false -prec-div=true -prec-sqrt=true -ftz=false -Xptxas -O3 -lineinfo ^
  -Xcompiler /fp:strict,/MT,/EHsc -I"%HERE%." -I"%HERE%..\..\engine" ^
  -o "%OUT%\mcgen_cuda.dll" "%HERE%mcgpu_core.cu" "%HERE%mcgpu_biome.cu" "%HERE%mcgpu_terrain.cu"
if errorlevel 1 exit /b 1
echo OK: %OUT%\mcgen_cuda.dll
exit /b 0
:no_cl
echo Не найден компилятор Microsoft C++ (cl.exe). Поставьте "Visual Studio Build Tools" с нагрузкой "Разработка классических приложений на C++" или запустите из "x64 Native Tools Command Prompt".
exit /b 2
