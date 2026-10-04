# Сборка libmcgen_cuda для Windows (PowerShell): оболочка над build.bat (нужен nvcc и MSVC в PATH — «x64 Native Tools»)
param([string]$Arch = "89", [string]$PtxArch = "75")
$env:CUDA_ARCH = $Arch; $env:CUDA_PTX_ARCH = $PtxArch
& "$PSScriptRoot\build.bat"
