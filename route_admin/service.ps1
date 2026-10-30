# Gateway route admin - service control for Windows PowerShell 5.1
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File admin\service.ps1 -Action start
#   powershell -NoProfile -ExecutionPolicy Bypass -File admin\service.ps1 -Action stop
#   powershell -NoProfile -ExecutionPolicy Bypass -File admin\service.ps1 -Action status
#   powershell -NoProfile -ExecutionPolicy Bypass -File admin\service.ps1 -Action restart
#   powershell -NoProfile -ExecutionPolicy Bypass -File admin\service.ps1 -Action install
#   powershell -NoProfile -ExecutionPolicy Bypass -File admin\service.ps1 -Action uninstall
#   powershell -NoProfile -ExecutionPolicy Bypass -File admin\service.ps1 -Action autostart-on
#   powershell -NoProfile -ExecutionPolicy Bypass -File admin\service.ps1 -Action autostart-off
#   powershell -NoProfile -ExecutionPolicy Bypass -File admin\service.ps1 -Action autostart-status
#
# Binds 127.0.0.1 only. Never touches port 8000.

param(
    [ValidateSet('start', 'stop', 'status', 'restart',
        'install', 'uninstall',
        'autostart-on', 'autostart-off', 'autostart-status')]
    [string]$Action = 'status',
    # uninstall only: also delete data/ (password hash, DPAPI secrets, logs, pid)
    [switch]$PurgeData,
    # skip the interactive confirmation used by -PurgeData
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dataDir = Join-Path $here 'data'
$pidFile = Join-Path $dataDir 'admin.pid'
$outLog = Join-Path $dataDir 'admin.out.log'
$errLog = Join-Path $dataDir 'admin.err.log'
$autoCmd = Join-Path $dataDir 'autostart.cmd'
$profilesDir = Join-Path $here 'profiles'
$port = 8791
$bind = '127.0.0.1'
$url = "http://${bind}:${port}/"
$taskName = 'RouteAdminAutoStart'

if (-not (Test-Path $dataDir)) { New-Item -ItemType Directory -Path $dataDir | Out-Null }

function Get-Listener {
    # Get-NetTCPConnection throws ObjectNotFound when nothing listens; quiet it down.
    try {
        return (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction Stop | Select-Object -First 1)
    } catch {
        return $null
    }
}

function Get-Python {
    $w = Get-Command pythonw.exe -ErrorAction SilentlyContinue
    if ($w) { return $w.Source }
    $c = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

function Test-Up {
    try {
        $r = Invoke-WebRequest -Uri ($url + 'api/auth/status') -UseBasicParsing -TimeoutSec 3
        return ($r.StatusCode -eq 200)
    } catch {
        return $false
    }
}

function Get-SavedPid {
    if (-not (Test-Path $pidFile)) { return 0 }
    $txt = (Get-Content -Path $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    $n = 0
    if ([int]::TryParse([string]$txt, [ref]$n)) { return $n }
    return 0
}

# ---------------------------------------------------------------- autostart

# schtasks is a native tool: its stderr would be turned into a terminating error by
# $ErrorActionPreference='Stop'. Running it through cmd.exe with 2>&1 inside cmd
# keeps the text on stdout and the exit code checkable.
function Invoke-SchTasks {
    param([string]$ArgLine)
    $out = & cmd.exe /d /c ('schtasks ' + $ArgLine + ' 2>&1')
    return @{ code = $LASTEXITCODE; text = ($out -join "`n") }
}

function Test-Autostart {
    $r = Invoke-SchTasks ('/Query /TN "' + $taskName + '"')
    return ($r.code -eq 0)
}

function Enable-Autostart {
    # The task points at a tiny .cmd wrapper so we avoid nested-quote trouble
    # and keep /TR well under the 261 character limit.
    $lines = @(
        '@echo off',
        ('powershell -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + (Join-Path $here 'service.ps1') + '" -Action start')
    )
    Set-Content -Path $autoCmd -Value $lines -Encoding ASCII

    $r = Invoke-SchTasks ('/Create /TN "' + $taskName + '" /SC ONLOGON /DELAY 0000:20 /TR "' + $autoCmd + '" /F')
    if ($r.code -ne 0) {
        Write-Host "autostart install failed (schtasks rc=$($r.code)): $($r.text)"
        exit 1
    }
    if (-not (Test-Autostart)) {
        Write-Host 'autostart install failed: task not found after create'
        exit 1
    }
    Write-Host "autostart on: task=$taskName trigger=at logon (delay 20s)"
}

function Disable-Autostart {
    $had = Test-Autostart
    if ($had) {
        $r = Invoke-SchTasks ('/Delete /TN "' + $taskName + '" /F')
        if ($r.code -ne 0) {
            Write-Host "autostart remove failed (schtasks rc=$($r.code)): $($r.text)"
            exit 1
        }
    }
    Remove-Item -Path $autoCmd -ErrorAction SilentlyContinue
    if ($had) { Write-Host "autostart off: task=$taskName removed" } else { Write-Host 'autostart off: not installed' }
}

# ---------------------------------------------------------------- process

function Stop-ServiceBg {
    $stopped = $false
    $saved = Get-SavedPid
    if ($saved -gt 0) {
        $p = Get-Process -Id $saved -ErrorAction SilentlyContinue
        if ($p) { Stop-Process -Id $saved -Force; $stopped = $true }
    }
    $l = Get-Listener
    if ($l) {
        $owner = Get-Process -Id $l.OwningProcess -ErrorAction SilentlyContinue
        if ($owner) { Stop-Process -Id $owner.Id -Force; $stopped = $true }
    }
    Start-Sleep -Milliseconds 400
    Remove-Item -Path $pidFile -ErrorAction SilentlyContinue
    if ($stopped) { Write-Host 'stopped' } else { Write-Host 'not running' }
}

function Start-ServiceBg {
    if (Get-Listener) {
        Write-Host "already running: $url"
        return
    }
    $exe = Get-Python
    if (-not $exe) {
        Write-Host 'python / pythonw not found in PATH'
        exit 1
    }
    $app = Join-Path $here 'app.py'
    if (-not (Test-Path $app)) {
        Write-Host "app.py not found: $app"
        exit 1
    }
    # Start through WMI: the parent is WmiPrvSE, outside this terminal's job object,
    # so closing the window / ending the command will not kill the server.
    # cmd.exe /d /c only exists to append stdout+stderr into the log files.
    $inner = '""' + $exe + '" "' + $app + '" --port ' + $port + ' 1>>"' + $outLog + '" 2>>"' + $errLog + '""'
    $cmd = 'cmd.exe /d /c ' + $inner
    $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $here }
    if ($r.ReturnValue -ne 0) {
        Write-Host "WMI process create failed rc=$($r.ReturnValue)"
        Write-Host "cmdline: $cmd"
        exit 1
    }

    # Wait for the port, then record the PID that is really listening
    # (cmd.exe exits right away because pythonw.exe is a GUI-subsystem exe).
    $procId = 0
    for ($i = 0; $i -lt 25; $i++) {
        $l = Get-Listener
        if ($l) { $procId = [int]$l.OwningProcess; break }
        Start-Sleep -Milliseconds 200
    }
    if ($procId -le 0) {
        Write-Host "process created but port never came up; see $errLog"
        exit 1
    }
    Set-Content -Path $pidFile -Value $procId -Encoding ASCII

    if (Test-Up) {
        Write-Host "started pid=$procId url=$url"
        Write-Host "log: $errLog"
    } else {
        Write-Host "started pid=$procId but health check failed; see $errLog"
        exit 1
    }
}

switch ($Action) {
    'start' {
        Start-ServiceBg
    }
    'stop' {
        Stop-ServiceBg
    }
    'restart' {
        Stop-ServiceBg
        Start-Sleep -Milliseconds 300
        Start-ServiceBg
    }
    'status' {
        $l = Get-Listener
        $saved = Get-SavedPid
        if ($l) {
            $owner = Get-Process -Id $l.OwningProcess -ErrorAction SilentlyContinue
            $name = if ($owner) { $owner.ProcessName } else { '?' }
            Write-Host "running pid=$($l.OwningProcess) ($name) listening on ${bind}:${port}"
        } else {
            Write-Host 'not running'
        }
        if ($saved -gt 0) { Write-Host "pid file: $saved" }
        if (Test-Up) { Write-Host "health: ok - $url" } else { Write-Host 'health: down' }
        if (Test-Autostart) { Write-Host "autostart: on (task $taskName)" } else { Write-Host 'autostart: off' }
        Write-Host "log: $errLog"
    }
    'install' {
        Enable-Autostart
        Start-ServiceBg
        Write-Host "installed. next logon it starts by itself; now: $url"
    }
    'uninstall' {
        Disable-Autostart
        Stop-ServiceBg
        if ($PurgeData) {
            if (-not $Force) {
                $ans = Read-Host 'Delete data/ (password hash, encrypted keys, logs, pid)? type YES'
                if ($ans -ne 'YES') { Write-Host 'purge cancelled, data kept'; exit 0 }
            }
            Remove-Item -Path $dataDir -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item -Path $profilesDir -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host 'data purged'
        } else {
            Write-Host 'data kept (use -PurgeData to remove it too)'
        }
        Write-Host 'uninstalled'
    }
    'autostart-on' {
        Enable-Autostart
    }
    'autostart-off' {
        Disable-Autostart
    }
    'autostart-status' {
        # single line, machine friendly
        if (Test-Autostart) { Write-Host 'on' } else { Write-Host 'off' }
    }
}
