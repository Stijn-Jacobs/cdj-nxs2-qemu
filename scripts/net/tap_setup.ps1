# SPDX-License-Identifier: GPL-2.0-or-later
# One-time Windows setup of the TAP adapter behind DJLINK=tap:CDJ-Link, which
# puts the emulated deck on a network your own PC is on, so rekordbox and
# Ableton Live can reach it.
#
# It creates an adapter named CDJ-Link, gives it the host address
# 192.168.50.1/24 (no gateway), caps its IP MTU at 1500, adds a permanent ARP
# entry for the deck (192.168.50.10, MAC 02-00-00-00-00-01) and one inbound
# firewall rule on that adapter for UDP 67, 20808 and 50000-50002. Safe to run
# again: it skips what already exists.
#
# Needs the OpenVPN project's TAP-Windows6 driver, which ships with OpenVPN
# (https://openvpn.net/community-downloads/; the installer's "TAP Virtual
# Ethernet Adapter" component is enough). The adapter is made with its
# tapctl.exe, expected at C:\Program Files\OpenVPN\bin\tapctl.exe; if OpenVPN
# is installed elsewhere, pass the path:
#   -TapCtl "D:\Tools\OpenVPN\bin\tapctl.exe"
#
# Run it from an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File scripts\net\tap_setup.ps1
param(
    [string]$TapCtl = 'C:\Program Files\OpenVPN\bin\tapctl.exe'
)

$ErrorActionPreference = 'Stop'
$name = 'CDJ-Link'

$elevated = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).
    IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    throw 'Run this from an elevated (Administrator) PowerShell.'
}

# A new adapter of its own, so any VPN adapters stay untouched.
if (-not (Get-NetAdapter -Name $name -ErrorAction SilentlyContinue)) {
    if (-not (Test-Path $TapCtl)) {
        throw "tapctl.exe not found at $TapCtl. Install OpenVPN (it carries the TAP-Windows6 driver) or pass -TapCtl <path to tapctl.exe>."
    }
    & $TapCtl create --name $name --hwid root\tap0901
    if ($LASTEXITCODE -ne 0) { throw "tapctl create failed (exit $LASTEXITCODE)." }
    Start-Sleep -Seconds 3
}
Get-NetAdapter -Name $name | Format-Table Name, InterfaceDescription, Status, MacAddress

# The host side of the deck's segment.
netsh interface ip set address name="$name" static 192.168.50.1 255.255.255.0

# TAP-Windows reports an IP MTU of 65500, so Windows sends rekordbox's 8 KB
# NFS replies as single oversized frames; the deck, like a real one on
# Ethernet, only takes 1500-byte packets (E-8309 on every LINK load).
netsh interface ipv4 set subinterface "$name" mtu=1500 store=persistent

# The rig's DHCP server leases the deck 192.168.50.10; Windows has to reach it
# before the deck can answer ARP.
Get-NetNeighbor -InterfaceAlias $name -IPAddress 192.168.50.10 -ErrorAction SilentlyContinue |
    Remove-NetNeighbor -Confirm:$false
New-NetNeighbor -InterfaceAlias $name -IPAddress 192.168.50.10 `
    -LinkLayerAddress 02-00-00-00-00-01 -State Permanent | Out-Null

# Inbound on this adapter only: DHCP to the rig (67), Ableton Link (20808),
# Pro DJ Link (50000-50002). Any profile, since a TAP network without a
# gateway is classed Public.
$rule = 'CDJ-Link deck segment (DHCP, Ableton Link, Pro DJ Link)'
if (-not (Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $rule -Direction Inbound -Protocol UDP `
        -LocalPort 67, 20808, 50000, 50001, 50002 -InterfaceAlias $name `
        -Profile Any -Action Allow | Out-Null
}

Get-NetIPAddress -InterfaceAlias $name -AddressFamily IPv4 | Format-Table IPAddress, PrefixLength
Get-NetNeighbor -InterfaceAlias $name -IPAddress 192.168.50.10 | Format-Table IPAddress, LinkLayerAddress, State
Get-NetFirewallRule -DisplayName $rule | Format-Table DisplayName, Enabled, Profile
Write-Host 'CDJ-Link is ready. The adapter shows Disconnected until QEMU opens it.'
