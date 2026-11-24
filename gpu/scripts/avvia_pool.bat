@echo off
cd /d "%~dp0"
rem solo GPU (tutte quelle trovate: NVIDIA, AMD, Intel). Metti il tuo indirizzo BOUND al posto di TUO_INDIRIZZO; pc1 = nome di questo PC.
rem Per minare anche con la CPU usa avvia_pool_gpu_cpu.bat.
set UTENTE=TUO_INDIRIZZO.pc1
set POOL=bound.ariabrain.com:3373
:loop
bound-gpuminer.exe --user %UTENTE% --pool %POOL% %*
echo il miner si e' fermato: riparto tra 10 secondi (Ctrl-C per uscire)
timeout /t 10
goto loop
