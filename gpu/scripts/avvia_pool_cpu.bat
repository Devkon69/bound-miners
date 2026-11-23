@echo off
cd /d "%~dp0"
rem SOLO CPU (anche su un PC senza scheda NVIDIA). Metti il tuo indirizzo BOUND al posto di TUO_INDIRIZZO; pc1 = nome di questo PC.
rem Usa tutti i thread della CPU, 3 hash per thread. Dopo taratura_cpu.bat puoi scrivere qui i valori migliori,
rem per esempio: set CPU=--cpu 32 --cpu-lanes 3
set UTENTE=TUO_INDIRIZZO.pc1
set POOL=bound.ariabrain.com:3373
set CPU=--cpu auto --cpu-lanes 3
:loop
bound-gpuminer.exe --cpu-only --user %UTENTE% --pool %POOL% %CPU% %*
echo il miner si e' fermato: riparto tra 10 secondi (Ctrl-C per uscire)
timeout /t 10
goto loop
