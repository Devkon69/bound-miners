@echo off
cd /d "%~dp0"
rem misura quanti hash in volo rendono di piu' su questa scheda (circa 15 s) e li scrive in bound-gpuminer.log
bound-gpuminer.exe --tune --bench 10 %*
pause
