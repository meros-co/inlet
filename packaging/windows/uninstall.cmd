@echo off
rem Removes Inlet, asking for administrator rights. Settings in C:\ProgramData\Inlet stay.
copy /y "%~dp0uninstall.ps1" "%TEMP%\inlet-uninstall.ps1" >nul
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile -ExecutionPolicy Bypass -NoExit -File ""%TEMP%\inlet-uninstall.ps1""'"
