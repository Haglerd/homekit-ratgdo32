# Register-LogAuditTask.ps1
# Run ONCE as Administrator to install the scheduled task that runs
# /log-audit-and-fix on a recurring schedule for homekit-ratgdo32.
#
# Usage:
#   pwsh -NoProfile -ExecutionPolicy Bypass -File .\Register-LogAuditTask.ps1
#
# Default schedule: every 6 hours, starting at 06:00. Customize via params.
# To install but keep DISABLED until ready, pass -Disabled.

[CmdletBinding()]
param(
    [int]$IntervalHours = 6,
    [string]$StartTime = '06:00',
    [string]$TaskName = 'Claude-LogAuditAndFix-homekit-ratgdo32',
    [switch]$Disabled
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "Re-run this script in an elevated PowerShell window. Scheduled-task registration requires admin."
    exit 1
}

$ProjectRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$RunScript = Join-Path $ProjectRoot '.claude\scripts\Run-LogAuditAndFix.ps1'

if (-not (Test-Path $RunScript)) {
    Write-Error "Run script not found at: $RunScript"
    exit 1
}

$Trigger = New-ScheduledTaskTrigger -Daily -At $StartTime
$Trigger.Repetition = (New-ScheduledTaskTrigger -Once -At $StartTime -RepetitionInterval (New-TimeSpan -Hours $IntervalHours) -RepetitionDuration ([TimeSpan]::FromDays(365 * 10))).Repetition

$Action = New-ScheduledTaskAction `
    -Execute 'pwsh.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$RunScript`""

$Settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 35) `
    -MultipleInstances IgnoreNew

$Principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive

$Task = New-ScheduledTask -Action $Action -Trigger $Trigger -Settings $Settings -Principal $Principal `
    -Description "Runs /log-audit-and-fix for homekit-ratgdo32 every $IntervalHours hours. Pulls device logs, queues new findings, runs auto-fix pipeline through PR. Reviews wait for human approval."

Register-ScheduledTask -TaskName $TaskName -InputObject $Task -Force | Out-Null

if ($Disabled) {
    Disable-ScheduledTask -TaskName $TaskName | Out-Null
    Write-Host ""
    Write-Host "Registered (DISABLED): $TaskName"
    Write-Host "  Trigger:    every $IntervalHours hours starting $StartTime"
    Write-Host "  Run script: $RunScript"
    Write-Host "  Logs:       $env:USERPROFILE\.claude\log-audit-history\"
    Write-Host ""
    Write-Host "Task is INSTALLED BUT NOT FIRING. To enable when ready:"
    Write-Host "  Enable-ScheduledTask -TaskName '$TaskName'"
}
else {
    Write-Host ""
    Write-Host "Registered + ENABLED: $TaskName"
    Write-Host "  Trigger:    every $IntervalHours hours starting $StartTime"
    Write-Host "  Run script: $RunScript"
    Write-Host "  Logs:       $env:USERPROFILE\.claude\log-audit-history\"
}

Write-Host ""
Write-Host "Inspect:  Get-ScheduledTask -TaskName '$TaskName'"
Write-Host "Run now:  Start-ScheduledTask -TaskName '$TaskName'"
Write-Host "Disable:  Disable-ScheduledTask -TaskName '$TaskName'"
Write-Host "Remove:   Unregister-ScheduledTask -TaskName '$TaskName' -Confirm:`$false"
