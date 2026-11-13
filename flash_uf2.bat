@echo off
rem ---------------------------------------------------------------
rem  Flash the bundled UF2 into the RP2040 BOOTSEL drive (RPI-RP2)
rem  Usage: click "Enter bootloader" in the web console (or hold
rem         BOOTSEL while plugging USB), wait for the RPI-RP2 drive,
rem         then double-click this file.
rem ---------------------------------------------------------------
setlocal enabledelayedexpansion

set "HERE=%~dp0"
set "UF2="
for %%f in ("%HERE%firmware\*.uf2") do if exist "%%~ff" set "UF2=%%~ff"

if not defined UF2 (
  echo [x] No .uf2 found under "%HERE%firmware"
  echo     Download it from the release page first.
  pause
  exit /b 1
)

echo [*] Firmware : %UF2%

set "TARGET="
for %%d in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
  if exist "%%d:\INFO_UF2.TXT" set "TARGET=%%d:"
)

if not defined TARGET (
  echo [x] RPI-RP2 drive not found.
  echo     1^) In the web console click "Enter bootloader", or hold BOOTSEL while plugging USB.
  echo     2^) Wait until a USB drive named RPI-RP2 shows up.
  echo     3^) Run this script again.
  pause
  exit /b 1
)

echo [*] Target   : %TARGET%
copy /Y "%UF2%" "%TARGET%\" >nul 2>&1
if errorlevel 1 (
  echo [!] Copy returned an error - normal when the board reboots instantly.
)
echo [ok] Sent. The board reboots by itself.
echo      If the serial ports do not come back, unplug and replug USB.
pause
