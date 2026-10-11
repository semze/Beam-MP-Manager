@echo off
rem Builds BeamMP-Manager.exe (the native-window app). Just double-click this file.
rem It needs the free portable compiler w64devkit (https://github.com/skeeto/w64devkit/releases).
rem If it can't find it, a normal "pick a folder" window opens so you can choose where you unzipped it.
setlocal
set "SAVED=%~dp0w64devkit-path.txt"
set "KIT=%~1"

rem 1) remembered folder from last time
if not defined KIT if exist "%SAVED%" set /p KIT=<"%SAVED%"

rem 2) usual places
if not defined KIT for %%D in ("%~dp0w64devkit" "%~dp0..\w64devkit" "C:\w64devkit" "D:\w64devkit" "%USERPROFILE%\w64devkit" "%USERPROFILE%\Downloads\w64devkit") do if exist "%%~D\bin\g++.exe" if not defined KIT set "KIT=%%~D"

rem 3) already on PATH?
if not defined KIT where g++ >nul 2>nul && goto build

:check
if defined KIT if not exist "%KIT%\bin\g++.exe" if exist "%KIT%\w64devkit\bin\g++.exe" set "KIT=%KIT%\w64devkit"
if defined KIT if exist "%KIT%\bin\g++.exe" goto found

rem 4) ask with a folder picker
echo Choose the folder where you unzipped w64devkit (the one that contains a "bin" folder)...
set "KIT="
for /f "usebackq delims=" %%P in (`powershell -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; $d=New-Object System.Windows.Forms.FolderBrowserDialog; $d.Description='Select the folder you unzipped w64devkit into'; $d.ShowNewFolderButton=$false; if($d.ShowDialog() -eq 'OK'){$d.SelectedPath}"`) do set "KIT=%%P"
if not defined KIT (
  echo No folder chosen. Download w64devkit-x64-*.zip from https://github.com/skeeto/w64devkit/releases and unzip it first.
  pause
  exit /b 1
)
if not exist "%KIT%\bin\g++.exe" if not exist "%KIT%\w64devkit\bin\g++.exe" (
  echo That folder does not contain bin\g++.exe. Please pick the unzipped w64devkit folder.
  set "KIT="
  pause
  goto check
)
goto check

:found
>"%SAVED%" echo %KIT%
set "PATH=%KIT%\bin;%PATH%"

:build
where g++ >nul 2>nul
if errorlevel 1 ( echo g++ still not found. & pause & exit /b 1 )
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0manager\build-mingw.ps1"
if errorlevel 1 ( pause & exit /b 1 )
echo.
echo Done. Run BeamMP-Manager.exe (keep it next to BeamMP-Launcher.exe).
pause
