# run.ps1 - start gateway (:8000) + bridge (:8001) in background, then health check
# Env knobs: QW_CHROME (browser exe, auto-detected from Playwright if unset),
#            QW_PROFILE (browser profile dir), JT_SOLVE_FAKE=1 (stub mode for smoke test)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$py = ".\.venv\Scripts\pythonw.exe"

if (-not $env:QW_CHROME) {
  $chrome = Get-ChildItem "$env:LOCALAPPDATA\ms-playwright" -Recurse -Filter chrome.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "chromium" } | Select-Object -First 1
  if ($chrome) { $env:QW_CHROME = $chrome.FullName }
}

Start-Process -FilePath $py -ArgumentList "-u gateway.py" -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
Start-Process -FilePath $py -ArgumentList "-u qw_bridge.py" -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
Start-Sleep -Seconds 3
try { Write-Output ("gateway: " + (Invoke-WebRequest -UseBasicParsing http://127.0.0.1:8000/health).Content) } catch { Write-Output "gateway: DOWN" }
try { Write-Output ("bridge : " + (Invoke-WebRequest -UseBasicParsing http://127.0.0.1:8001/health).Content) } catch { Write-Output "bridge : DOWN" }
