@echo off
cd /d "%~dp0"
rem elenca le GPU trovate (NVIDIA con CUDA, AMD e Intel con OpenCL) con il numero da usare in --gpu
bound-gpuminer.exe --list-gpus
pause
