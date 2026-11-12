@echo off
rem Open the RP2040 combo web console with Edge or Chrome.
rem Double-click this file if the default browser does not support WebSerial.
setlocal
set "HTML=%~dp0combo_webserial.html"
if not exist "%HTML%" (
  echo combo_webserial.html not found in this folder.
  pause
  exit /b 1
)
set "EDGE=%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe"
if not exist "%EDGE%" set "EDGE=%ProgramFiles%\Microsoft\Edge\Application\msedge.exe"
if exist "%EDGE%" (
  start "" "%EDGE%" "%HTML%"
  exit /b 0
)
set "CHROME=%ProgramFiles%\Google\Chrome\Application\chrome.exe"
if not exist "%CHROME%" set "CHROME=%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe"
if exist "%CHROME%" (
  start "" "%CHROME%" "%HTML%"
  exit /b 0
)
start "" "%HTML%"
