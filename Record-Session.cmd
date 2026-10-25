@echo off
rem Uncharted: The Nathan Drake Collection - session recorder. Start it before or after the game;
rem it records every game run (system, settings, timed log, metrics, exit code, F7-F10 marks) into
rem analysis\uncharted-user\sessions\. It changes nothing in the game. Extra options go to the
rem recorder, e.g. Record-Session.cmd --keys F7=texture,F8=geometry --no-engine-dump
title Uncharted session recorder
cd /d "%~dp0"
if exist "python\python.exe" (
    set "PY=python\python.exe"
) else (
    where py >nul 2>nul && (set "PY=py -3") || (set "PY=python")
)
echo Uncharted session recorder
echo   Hotkeys during the game: F7 = texture problem, F8 = geometry/vertices, F9 = flash/flicker, F10 = other
echo   (one beep = captured, two = engine dump done, low beep = failed)
echo   Sessions are saved in analysis\uncharted-user\sessions\
echo   Close this window or press Ctrl+C to stop.
echo.
%PY% "tools\session_recorder.py" %*
pause
