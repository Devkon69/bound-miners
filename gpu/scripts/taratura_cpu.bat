@echo off
cd /d "%~dp0"
rem Prova quante combinazioni di thread CPU e hash per thread rendono di piu' (circa 40 s), poi misura
rem GPU + CPU insieme per 30 s. Scrive tutto in bound-gpuminer.log. Chiudi gli altri programmi pesanti prima.
bound-gpuminer.exe --tune-cpu --bench 30 %*
pause
