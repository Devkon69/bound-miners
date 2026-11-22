@echo off
cd /d "%~dp0"
bound-gpubench.exe %*
echo.
echo Mandami il file bound-gpubench.log
pause
