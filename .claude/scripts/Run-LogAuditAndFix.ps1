# Run-LogAuditAndFix.ps1
# Invoked by Windows Task Scheduler to run unattended log-audit-and-fix
# against this project. Fires on whatever cadence the scheduled task is set to.
#
# Safe to run while interactive sessions are active — Claude Code spawns
# its own session for headless invocations.

$ErrorActionPreference = 'Stop'

# Project root (resolves to homekit-ratgdo32/)
$ProjectRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$LogDir = Join-Path $env:USERPROFILE '.claude\log-audit-history'
$LogFile = Join-Path $LogDir ("homekit-ratgdo32-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

# Ensure log dir
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

# Move into project (Claude Code uses cwd to find .claude/)
Set-Location $ProjectRoot

# Run the slash command non-interactively, capture output
"=== Run-LogAuditAndFix start: $(Get-Date -Format 'o') ===" | Out-File -FilePath $LogFile -Encoding utf8
"Project root: $ProjectRoot" | Out-File -FilePath $LogFile -Append -Encoding utf8
"" | Out-File -FilePath $LogFile -Append -Encoding utf8

# Headless Claude Code invocation. -p runs the prompt then exits.
# 30-minute timeout — generous for a full pipeline run including a PR.
$proc = Start-Process -FilePath 'claude' `
    -ArgumentList '-p', '/log-audit-and-fix' `
    -WorkingDirectory $ProjectRoot `
    -RedirectStandardOutput "$LogFile.stdout" `
    -RedirectStandardError "$LogFile.stderr" `
    -NoNewWindow `
    -PassThru

if (-not $proc.WaitForExit(30 * 60 * 1000)) {
    $proc.Kill()
    "ERROR: timed out after 30 minutes — killed process" | Out-File -FilePath $LogFile -Append -Encoding utf8
    exit 1
}

# Append captured output to main log
"=== STDOUT ===" | Out-File -FilePath $LogFile -Append -Encoding utf8
Get-Content "$LogFile.stdout" -ErrorAction SilentlyContinue | Out-File -FilePath $LogFile -Append -Encoding utf8
"=== STDERR ===" | Out-File -FilePath $LogFile -Append -Encoding utf8
Get-Content "$LogFile.stderr" -ErrorAction SilentlyContinue | Out-File -FilePath $LogFile -Append -Encoding utf8
"" | Out-File -FilePath $LogFile -Append -Encoding utf8
"=== Exit code: $($proc.ExitCode) ===" | Out-File -FilePath $LogFile -Append -Encoding utf8
"=== End: $(Get-Date -Format 'o') ===" | Out-File -FilePath $LogFile -Append -Encoding utf8

# Clean up split files
Remove-Item "$LogFile.stdout" -ErrorAction SilentlyContinue
Remove-Item "$LogFile.stderr" -ErrorAction SilentlyContinue

# Prune old log files (keep last 30)
Get-ChildItem -Path $LogDir -Filter "homekit-ratgdo32-*.log" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -Skip 30 |
    Remove-Item -Force -ErrorAction SilentlyContinue

exit $proc.ExitCode
