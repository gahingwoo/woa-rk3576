<#
.SYNOPSIS
Reads or sets the Quectel EC20's USB network mode (AT+QCFG="usbnet") on
Windows: MBIM for Windows' in-box mobile broadband, or back to the factory
QMI mode.

.DESCRIPTION
The mode is stored in the module and survives power cycles, so Linux on the
same board sees it too (docs/dev/ec20.md: Linux works in MBIM mode, which is
why this is a one-time switch and not a per-OS one).

The script talks to the module's AT port, interface 2 of USB 2C7C:0125. That
interface is vendor class (FF/00/00), so Windows binds no driver to it by
itself: it needs ec20-ports.inf from gahingwoo/ec20-woa, which puts the
in-box usbser.sys on interfaces 0 to 3 (measured on the TaishanPi 3M,
2026-11-14: four COM ports, AT answers). The script finds the COM port of
MI_02 (MI_03, the second AT-capable port, if MI_02 has none) by itself.

  -Status   identity, firmware, the usbnet mode, and the module's USB
            functions as Windows sees them. Changes nothing.
  (default) switch to MBIM (usbnet 2).
  -Revert   switch back to QMI (usbnet 0), the factory default.

A switch reads the mode, asks, sets it, reads it back, resets the module
(AT+CFUN=1,1: it drops off USB for 20 to 40 s), waits for the AT port to
answer again, reads the mode back once more and lists the USB functions.
For MBIM it then checks that a CDC MBIM function (02/0E) is bound to the
in-box driver (service cxwmbclass on 23H2, wmbclass on older builds).

Exit codes: 0 done and checked; 1 cancelled or an AT command failed;
2 the read-back did not match; 3 no AT COM port; 4 the module did not come
back in time; 5 the mode is set but the MBIM function is missing or has no
driver.

.PARAMETER Port
The AT port, for example COM6, instead of looking it up.

.PARAMETER NoReset
Set the mode but do not reset the module; it takes effect on the next
power cycle.

.EXAMPLE
.\ec20-mode.ps1 -Status
.\ec20-mode.ps1                 # to MBIM, asks first
.\ec20-mode.ps1 -Revert         # back to QMI
.\ec20-mode.ps1 -Port COM6 -Confirm:$false

.NOTES
Windows PowerShell 5.1 compatible, ASCII only. Run elevated is not needed.
Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
SPDX-License-Identifier: GPL-2.0-only
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High', DefaultParameterSetName = 'Set')]
param(
    [Parameter(ParameterSetName = 'Status')][switch]$Status,
    [Parameter(ParameterSetName = 'Set')][switch]$Revert,
    [Parameter(ParameterSetName = 'Set')][switch]$NoReset,
    [string]$Port,
    [int]$WaitSeconds = 120
)
$ErrorActionPreference = 'Stop'
$Vid = '2C7C'; $Pid_ = '0125'
$ModeName = @{ 0 = 'QMI (rmnet)'; 1 = 'ECM'; 2 = 'MBIM'; 3 = 'RNDIS' }

function Get-Ec20Function {
    # One row per interface of the module that Windows enumerated, with the
    # USB class from its compatible IDs and the driver service bound to it.
    $devs = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like "USB\VID_$Vid&PID_$Pid_&MI_*" })
    foreach ($d in ($devs | Sort-Object InstanceId)) {
        $mi = ''; if ($d.InstanceId -match '&MI_([0-9A-F]{2})') { $mi = $Matches[1] }
        $cls = ''
        try {
            $cids = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_CompatibleIds -ErrorAction Stop).Data
            foreach ($c in @($cids)) {
                if ($c -match '^USB\\Class_([0-9A-F]{2})&SubClass_([0-9A-F]{2})&Prot_([0-9A-F]{2})$') { $cls = "$($Matches[1])/$($Matches[2])/$($Matches[3])"; break }
            }
        } catch {}
        $svc = ''
        try { $svc = [string](Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data } catch {}
        $role = switch -Regex ($cls) {
            '^FF/FF/FF$' { if ($mi -eq '00') { 'DM' } else { 'QMI network' } }
            '^FF/00/00$' { @{ '01' = 'NMEA'; '02' = 'AT'; '03' = 'modem/AT' }[$mi] }
            '^02/0E/'    { 'MBIM' }
            '^0A/'       { 'MBIM data' }
            '^01/'       { 'audio (UAC)' }
            default      { '' }
        }
        $com = ''
        try { $com = [string](Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters" -Name PortName -ErrorAction Stop).PortName } catch {}
        [pscustomobject]@{ MI = $mi; Class = $cls; Role = $role; Status = [string]$d.Status; Problem = [int]$d.ConfigManagerErrorCode; Service = $svc; Port = $com; Name = $d.FriendlyName }
    }
}

function Find-AtPort {
    $f = @(Get-Ec20Function)
    foreach ($mi in '02', '03') {
        $p = @($f | Where-Object { $_.MI -eq $mi -and $_.Port -and $_.Status -eq 'OK' })
        if ($p.Count) { return $p[0].Port }
    }
    return $null
}

function Invoke-At {
    # Sends one command, returns its lines up to the final result code.
    param([string]$Cmd, [int]$TimeoutMs = 5000, [switch]$NoReply)
    $sp = New-Object System.IO.Ports.SerialPort $script:AtPort, 115200, 'None', 8, 'One'
    $sp.DtrEnable = $true; $sp.RtsEnable = $true
    $sp.ReadTimeout = 200; $sp.NewLine = "`r"
    $sp.Open()
    try {
        $sp.DiscardInBuffer()
        $sp.Write("$Cmd`r")
        if ($NoReply) { Start-Sleep -Milliseconds 300; return @() }
        $buf = ''; $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
        while ([DateTime]::UtcNow -lt $deadline) {
            try { $buf += $sp.ReadExisting() } catch {}
            if ($buf -match '(?m)^(OK|ERROR|\+CME ERROR:.*|\+CMS ERROR:.*)\r?$') { break }
            Start-Sleep -Milliseconds 50
        }
        $lines = @($buf -split "[`r`n]+" | Where-Object { $_ -and $_ -ne $Cmd })
        return $lines
    } finally { $sp.Close(); $sp.Dispose() }
}

function Test-Ok { param($Lines) return (@($Lines) -contains 'OK') }

function Read-UsbNet {
    $r = Invoke-At 'AT+QCFG="usbnet"'
    foreach ($l in $r) { if ($l -match '"usbnet",\s*(\d+)') { return [int]$Matches[1] } }
    Write-Host ('AT+QCFG="usbnet" gave: ' + ($r -join ' | '))
    return $null
}

function Show-Functions {
    $f = @(Get-Ec20Function)
    if (-not $f.Count) { Write-Host 'No interface of USB 2C7C:0125 is present.' }
    else { $f | Format-Table MI, Class, Role, Status, Problem, Service, Port -AutoSize | Out-String -Width 200 | Write-Host }
    return , $f
}

# --- find the AT port ---------------------------------------------------
$script:AtPort = $Port
if (-not $script:AtPort) { $script:AtPort = Find-AtPort }
if (-not $script:AtPort) {
    Write-Host 'No COM port on the EC20 AT interface (USB\VID_2C7C&PID_0125&MI_02 or MI_03).'
    [void](Show-Functions)
    Write-Host 'The interface is vendor class, so Windows binds nothing to it by itself. Install'
    Write-Host 'ec20-ports.inf from https://github.com/gahingwoo/ec20-woa (in-box usbser.sys, no binary):'
    Write-Host '  .\tools\Install-Ec20Driver.ps1 -Variant Usbser -ImportCertificate'
    Write-Host 'or switch the mode from Linux (tools/ec20/ec20-mode.sh in this repository).'
    exit 3
}
Write-Host "AT port: $script:AtPort"

$cur = Read-UsbNet
if ($null -eq $cur) { Write-Host 'The module did not report its usbnet mode.'; exit 1 }
$rev = (Invoke-At 'AT+QGMR') | Where-Object { $_ -ne 'OK' } | Select-Object -First 1
Write-Host ("Firmware: $rev")
Write-Host ("usbnet: $cur, " + $ModeName[$cur])

if ($Status) {
    $f = Show-Functions
    $m = @($f | Where-Object { $_.Role -eq 'MBIM' })
    if ($m.Count) { Write-Host ("MBIM function: " + $m[0].Status + ", service " + $m[0].Service) }
    exit 0
}

$want = 2; if ($Revert) { $want = 0 }
if ($cur -eq $want) {
    Write-Host ("Already in " + $ModeName[$want] + " mode; nothing to set.")
} else {
    if (-not $PSCmdlet.ShouldProcess("EC20 on $script:AtPort", "set usbnet $want (" + $ModeName[$want] + "), stored in the module, then reset it")) { exit 1 }
    if (-not (Test-Ok (Invoke-At "AT+QCFG=`"usbnet`",$want"))) { Write-Host 'The module refused the mode.'; exit 1 }
    $rb = Read-UsbNet
    if ($rb -ne $want) { Write-Host "Read-back before the reset: $rb, expected $want. This firmware may not support it."; exit 2 }
    Write-Host "Set and read back: $rb"
    if ($NoReset) { Write-Host 'Not reset (-NoReset): the mode takes effect at the next power cycle.'; exit 0 }
}

if (-not $NoReset -and $cur -ne $want) {
    Write-Host 'Resetting the module (AT+CFUN=1,1); it leaves USB for 20 to 40 s.'
    try { [void](Invoke-At 'AT+CFUN=1,1' -NoReply) } catch {}
    Start-Sleep -Seconds 10
    $deadline = (Get-Date).AddSeconds($WaitSeconds); $back = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        if (-not $Port) { $p = Find-AtPort; if (-not $p) { continue }; $script:AtPort = $p }
        try { if (Test-Ok (Invoke-At 'AT' -TimeoutMs 1500)) { $back = $true; break } } catch {}
    }
    if (-not $back) { Write-Host "The AT port did not answer within $WaitSeconds s."; [void](Show-Functions); exit 4 }
    $after = Read-UsbNet
    Write-Host "After the reset: usbnet $after"
    if ($after -ne $want) { exit 2 }
    # PnP needs a few seconds to install the new network function
    Start-Sleep -Seconds 10
}

$f = Show-Functions
if ($want -eq 2) {
    $m = @($f | Where-Object { $_.Role -eq 'MBIM' })
    $d = @($f | Where-Object { $_.Role -eq 'MBIM data' })
    if (-not $m.Count) { Write-Host 'No MBIM (02/0E) function is present.'; exit 5 }
    if ($d.Count) {
        Write-Host 'The MBIM data interface is a separate function: the module declares no IAD and Windows'
        Write-Host 'cannot group the two MBIM interfaces. Switch back with -Revert and report this.'
        exit 5
    }
    if ($m[0].Status -ne 'OK' -or $m[0].Service -notmatch '^(cx)?wmbclass$') {
        Write-Host ('The MBIM function is ' + $m[0].Status + ' with service "' + $m[0].Service + '", not OK on the in-box MBIM driver.')
        exit 5
    }
    Write-Host ('MBIM is bound to ' + $m[0].Service + '. Settings > Network & internet > Cellular should be there now.')
    Write-Host 'Check: netsh mbn show interfaces ; netsh mbn show readyinfo *'
} else {
    Write-Host 'Back in QMI mode: MI_04 is again without a Windows driver (expected; regress whitelists it).'
}
exit 0
