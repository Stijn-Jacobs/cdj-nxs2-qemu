# SPDX-License-Identifier: GPL-2.0-or-later
# Raise every rig QEMU above the desktop's own processes, so MAIN's vCPU thread
# is not kept off the CPU by other applications. Sets the priority class of each
# qemu-system-* process as it appears. Never RealTime: a spinning vCPU would
# starve the desktop's input.
#
#   usage: powershell -File scripts\run\raise_priority.ps1 [-Seconds 0] [-Class AboveNormal|High]
#   -Seconds 0 (rig.sh): run until no QEMU has been seen for 30 s after the
#   first appeared, or give up after 10 minutes if none ever does.
param([int]$Seconds = 0, [string]$Class = "AboveNormal")
if ($Class -notin @("AboveNormal", "High")) { Write-Output "prio: refusing class $Class"; exit 1 }
$start = Get-Date
$lastSeen = $null
$done = @{}
while ($true) {
    $now = Get-Date
    if ($Seconds -gt 0) {
        if (($now - $start).TotalSeconds -ge $Seconds) { break }
    } elseif ($lastSeen) {
        if (($now - $lastSeen).TotalSeconds -ge 30) { break }
    } elseif (($now - $start).TotalSeconds -ge 600) {
        break
    }
    $procs = @(Get-Process -Name "qemu-system-*" -ErrorAction SilentlyContinue)
    if ($procs.Count) { $lastSeen = $now }
    foreach ($p in $procs) {
        if (-not $done.ContainsKey($p.Id)) {
            try { $p.PriorityClass = $Class; $done[$p.Id] = 1; Write-Output "prio $Class $($p.ProcessName) $($p.Id)" } catch { }
        }
    }
    Start-Sleep -Seconds 2
}
