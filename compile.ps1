# BeamMP Manager one-step compile script (MinGW / w64devkit).
# Works two ways:
#   1) Paste this whole thing into PowerShell.
#   2) Save as compile.ps1 and run:  powershell -ExecutionPolicy Bypass -File .\compile.ps1
# If you run the saved file from inside the beammp-launcher folder it uses that folder automatically;
# otherwise it uses the fallback path below.
& {
    $ErrorActionPreference = 'Stop'
    $fallbackRepo = "C:\Users\James\Downloads\BeamMP-Multi-Fork\beammp-launcher"
    $repo = if ($PSScriptRoot -and (Test-Path "$PSScriptRoot\manager\src\main.cpp")) { $PSScriptRoot } else { $fallbackRepo }

    # Find g++ (already on PATH, or in the usual w64devkit folders)
    $gpp = (Get-Command g++ -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    if (-not $gpp) {
        foreach ($d in 'D:\w64devkit\bin', 'D:\w64devkit\w64devkit\bin', 'E:\w64devkit\bin', 'F:\w64devkit\bin') {
            if (Test-Path "$d\g++.exe") { $gpp = "$d\g++.exe"; break }
        }
    }
    if (-not $gpp) {
        Write-Host "Could not find g++.exe. Edit the folder list in this script to where w64devkit is." -ForegroundColor Red
        return
    }
    $env:Path = (Split-Path $gpp) + ";" + $env:Path
    Write-Host "Using compiler: $gpp"

    $m = "$repo\manager"
    if (-not (Test-Path "$m\src\main.cpp")) {
        Write-Host "Can't find $m\src\main.cpp. Fix the fallbackRepo path at the top of this script." -ForegroundColor Red
        return
    }

    # Embed the UI into the program
    New-Item -ItemType Directory -Force "$m\generated" | Out-Null
    $sb = New-Object System.Text.StringBuilder
    foreach ($b in [System.IO.File]::ReadAllBytes("$m\ui\index.html")) { [void]$sb.Append(('0x{0:x2},' -f $b)) }
    [void]$sb.Append('0x00')
    [System.IO.File]::WriteAllText("$m\generated\ui_html.inc", $sb.ToString())

    $out = "$repo\BeamMP-Manager.exe"
    Write-Host "Compiling..."
    & g++ -std=c++17 -O2 -s -static -D_WIN32_WINNT=0x0A00 -DNOMINMAX -DWIN32_LEAN_AND_MEAN "-I$m\third_party" "-I$m\generated" "$m\src\main.cpp" -o $out -lws2_32 -lshell32
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build FAILED. Copy the red error lines above and send them to Claude." -ForegroundColor Red
        return
    }
    Write-Host "Built: $out" -ForegroundColor Green
}