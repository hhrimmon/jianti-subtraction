# install.ps1 - one-shot setup for jianti server side (Windows)
# Creates .venv, installs python deps, downloads the headless browser (Playwright Chromium).
# Usage: powershell -ExecutionPolicy Bypass -File install.ps1
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

python -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m playwright install chromium

# Resolve the chromium executable that Playwright just downloaded
$chrome = Get-ChildItem "$env:LOCALAPPDATA\ms-playwright" -Recurse -Filter chrome.exe -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -match "chromium" } | Select-Object -First 1
if ($chrome) {
  Write-Output ("CHROME_EXE=" + $chrome.FullName)
  Write-Output "Tip: set env QW_CHROME to the path above, or just use run.ps1 which auto-detects it."
} else {
  Write-Output "WARN: chromium not found under ms-playwright; set QW_CHROME to your chrome.exe"
}
Write-Output "INSTALL_OK"
