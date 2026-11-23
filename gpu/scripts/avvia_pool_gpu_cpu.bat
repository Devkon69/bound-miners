@echo off
cd /d "%~dp0"
rem GPU NVIDIA + CPU insieme. Metti il tuo indirizzo BOUND al posto di TUO_INDIRIZZO; pc1 = nome di questo PC.
rem CPU: "--cpu auto" = tutti i thread logici meno uno (quello resta alla GPU). Dopo taratura_cpu.bat puoi
rem scrivere qui i valori che ti suggerisce, per esempio: set CPU=--cpu 31 --cpu-lanes 3
set UTENTE=TUO_INDIRIZZO.pc1
set POOL=bound.ariabrain.com:3373
set CPU=--cpu auto --cpu-lanes 3
:loop
bound-gpuminer.exe --user %UTENTE% --pool %POOL% %CPU% %*
echo il miner si e' fermato: riparto tra 10 secondi (Ctrl-C per uscire)
timeout /t 10
goto loop
