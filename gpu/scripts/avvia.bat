@echo off
cd /d "%~dp0"
rem metti il tuo indirizzo BOUND al posto di TUO_INDIRIZZO; pc1 = nome di questo PC
set UTENTE=TUO_INDIRIZZO.pc1
set PONTE=127.0.0.1:7340
:loop
bound-gpuminer.exe --user %UTENTE% --pool %PONTE% %*
echo il miner si e' fermato: riparto tra 10 secondi (Ctrl-C per uscire)
timeout /t 10
goto loop
