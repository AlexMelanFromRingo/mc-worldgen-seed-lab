@echo off
rem Сборка libmcgen_cuda для Windows (nvcc + MSVC): запускать в «x64 Native Tools Command Prompt for VS» с установленным CUDA Toolkit.
rem   libmcgen\gpu\build.bat                      -> libmcgen\build\gpu\mcgen_cuda.dll  (sm_89 + PTX compute_75 для совместимости вперёд)
rem   set CUDA_ARCH=75 80 86 89 && libmcgen\gpu\build.bat   (релизная сборка для нескольких архитектур)
rem Бит-точность: --fmad=false, без fast-math; хост-код — /fp:strict. cudart и CRT слинкованы статически: нужен только драйвер NVIDIA (nvcuda.dll).
rem Положите результат рядом с mcgen.dll (blender\mcgen_addon\lib\windows-x64\): libmcgen найдёт библиотеку сама.
setlocal enabledelayedexpansion
set HERE=%~dp0
if "%OUT%"=="" set OUT=%HERE%..\build\gpu
if not exist "%OUT%" mkdir "%OUT%"
if "%CUDA_ARCH%"=="" set CUDA_ARCH=89
if "%CUDA_PTX_ARCH%"=="" set CUDA_PTX_ARCH=75
set GEN=
for %%a in (%CUDA_ARCH%) do set GEN=!GEN! -gencode arch=compute_%%a,code=sm_%%a
set GEN=%GEN% -gencode arch=compute_%CUDA_PTX_ARCH%,code=compute_%CUDA_PTX_ARCH%
nvcc -O3 -std=c++17 --shared %GEN% --fmad=false -prec-div=true -prec-sqrt=true -ftz=false -Xptxas -O3 -lineinfo ^
  -Xcompiler "/fp:strict /MT /EHsc" -I"%HERE%." -I"%HERE%..\..\engine" ^
  -o "%OUT%\mcgen_cuda.dll" "%HERE%mcgpu_core.cu" "%HERE%mcgpu_biome.cu" "%HERE%mcgpu_terrain.cu"
if errorlevel 1 exit /b 1
echo OK: %OUT%\mcgen_cuda.dll
