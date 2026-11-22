@echo off
cd /d "%~dp0"
rem metti il tuo indirizzo BOUND al posto di TUO_INDIRIZZO; pc1 = nome di questo PC
set UTENTE=TUO_INDIRIZZO.pc1
set POOL=bound.ariabrain.com:3373
:loop
bound-gpuminer.exe --user %UTENTE% --pool %POOL% %*
echo il miner si e' fermato: riparto tra 10 secondi (Ctrl-C per uscire)
timeout /t 10
goto loop
