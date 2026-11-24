@echo off
cd /d "%~dp0"
rem tutte le GPU e niente CPU; per aggiungere la CPU: bench.bat --cpu auto; una sola scheda: bench.bat --gpu 0
bound-gpuminer.exe --bench 20 %*
pause
