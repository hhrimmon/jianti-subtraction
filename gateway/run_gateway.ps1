# run_gateway.ps1: start gateway.py only (bridge is owned by autorun.pyw, do not double-spawn).
# Used by scheduled task JianTiGatewayManual. ASCII only - PS5.1 reads BOM-less files as ANSI.
# Idempotent: kills any stale gateway.py first, then spawns via pythonw with file-redirected logs.
$env:JT_VLM_BACKEND = 'qianwen'
$dir = $PSScriptRoot
Add-Content (Join-Path $dir 'gateway_autorun.log') ("spawn run_gateway.ps1 backend=" + $env:JT_VLM_BACKEND)
Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'python*.exe' -and $_.CommandLine -match 'gateway\.py' -and $_.CommandLine -notmatch 'autorun' -and $_.CommandLine -notmatch 'qw_bridge' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 800
Start-Process -FilePath 'F:\Python\pythonw.exe' -ArgumentList '-u','gateway.py' -WorkingDirectory $dir -RedirectStandardOutput (Join-Path $dir 'gateway_manual_out.log') -RedirectStandardError (Join-Path $dir 'gateway_manual_err.log') -WindowStyle Hidden
