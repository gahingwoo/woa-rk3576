<#
.SYNOPSIS
Reads or sets the Quectel EC20's USB network mode (AT+QCFG="usbnet") on
Windows, and brings up mobile data in RNDIS mode.

.DESCRIPTION
The mode is stored in the module and survives power cycles, so Linux on the
same board sees it too (docs/dev/ec20.md).

  -Mode rndis  usbnet 3: an RNDIS Ethernet function, in-box driver
               (wceisvista.inf). The module dials and runs NAT; Windows
               gets an address by DHCP. No Cellular page.
  -Mode mbim   usbnet 2: CDC MBIM, in-box cxwmbclass and the Cellular
               page. Does not work on EC20CEFILGR08A03M1G (the module never
               answers MBIM OPEN).
  -Mode qmi    usbnet 0, the factory default. No Windows driver.
  -Revert      the same as -Mode qmi.
  -Status      identity, firmware, mode and the USB functions with their
               drivers. Changes nothing. Also the default with no switch.
  -Connect     RNDIS mode only: SIM PIN (-Pin), APN (-Apn), registration,
               the dial, then waits for the adapter's DHCP address.

The AT port: the module renumbers its interfaces with the mode (AT is MI_02
in QMI and MBIM mode, MI_04 in RNDIS mode), so the script sends "AT" to
every COM port of USB VID 2C7C bound to usbser.sys and takes the first that
answers OK, in interface order (NMEA comes before AT and does not answer).
Those COM ports need ec20-ports.inf 2.0 from gahingwoo/ec20-woa, which binds
the in-box usbser.sys by class (FF/00/00) in every mode; 1.0 bound MI_00 to
MI_03 by number and takes the RNDIS function away from its driver.

A switch reads the mode, asks, sets it and reads it back (this firmware
answers ERROR to usbnet 2 and 3 and stores them anyway: the read-back
decides), resets the module (AT+CFUN=1,1: it leaves USB for 20 to 40 s),
waits for an AT port to answer again, reads the mode once more and checks
the network function: RNDIS bound to the in-box driver with a "Remote NDIS"
adapter, or MBIM bound to cxwmbclass with a mobile broadband interface.

Exit codes: 0 done and checked; 1 cancelled or an AT command failed;
2 the read-back did not match; 3 no AT COM port; 4 the module did not come
back in time; 5 the mode is set but the network function is missing or has
no driver; 6 the MBIM function is bound but the module never answers MBIM
OPEN; 7 the SIM needs a PIN or PUK, or the PIN was refused; 8 not registered
on the network in time; 9 no DHCP address on the RNDIS adapter in time.

.PARAMETER Mode
rndis, mbim or qmi.

.PARAMETER Connect
Bring up data in RNDIS mode. Can follow a switch in the same run.

.PARAMETER Pin
The SIM PIN, used only if the SIM asks for one (AT+CPIN? = SIM PIN). It is
replaced with **** in all output. The script refuses to try it when only
one attempt is left.

.PARAMETER Apn
Set this APN on context 1 (the default bearer) if it differs. Without it
the context the module has is used.

.PARAMETER Port
The AT port, for example COM6, instead of probing.

.PARAMETER NoReset
Set the mode but do not reset the module; it takes effect on the next
power cycle.

.EXAMPLE
.\ec20-mode.ps1 -Status
.\ec20-mode.ps1 -Mode rndis -Connect -Pin 1234
.\ec20-mode.ps1 -Connect -Pin 1234 -Apn cmnet
.\ec20-mode.ps1 -Mode qmi
.\ec20-mode.ps1 -Mode rndis -Port COM8 -Confirm:$false

.NOTES
Windows PowerShell 5.1 compatible, ASCII only. Elevation is needed only for
the DHCP renew that -Connect tries; without it the script waits for Windows'
own DHCP retry.
Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
SPDX-License-Identifier: GPL-2.0-only
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [ValidateSet('rndis', 'mbim', 'qmi')][string]$Mode,
    [switch]$Status,
    [switch]$Revert,
    [switch]$Connect,
    [string]$Pin,
    [string]$Apn,
    [switch]$NoReset,
    [string]$Port,
    [int]$WaitSeconds = 120
)
$ErrorActionPreference = 'Stop'
$Vid = '2C7C'; $Pid_ = '0125'
$ModeName = @{ 0 = 'QMI (rmnet)'; 1 = 'ECM'; 2 = 'MBIM'; 3 = 'RNDIS' }
$ModeCode = @{ qmi = 0; mbim = 2; rndis = 3 }
if ($Revert) { $Mode = 'qmi' }
if ($Status -and ($Mode -or $Connect)) { Write-Host '-Status changes nothing; drop -Mode and -Connect, or -Status.'; exit 1 }
$Secret = @(); if ($Pin) { $Secret = @($Pin) }

function Hide([string]$s) {
    foreach ($x in $Secret) { if ($x) { $s = $s.Replace($x, '****') } }
    return $s
}

function Get-Ec20Function {
    # One row per interface of the module that Windows enumerated, with the
    # USB class from its compatible IDs and the driver service bound to it.
    # Roles by class: the FF/00/00 interfaces in order are NMEA, AT, modem;
    # the first FF/FF/FF one is DM.
    $devs = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like "USB\VID_$Vid&PID_$Pid_&MI_*" })
    $rows = @()
    foreach ($d in ($devs | Sort-Object InstanceId)) {
        $mi = ''; if ($d.InstanceId -match '&MI_([0-9A-F]{2})') { $mi = $Matches[1] }
        $cls = ''
        try {
            $cids = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_CompatibleIds -ErrorAction Stop).Data
            foreach ($c in @($cids)) {
                if ($c -match '^USB\\Class_([0-9A-F]{2})&SubClass_([0-9A-F]{2})&Prot_([0-9A-F]{2})$') { $cls = ("$($Matches[1])/$($Matches[2])/$($Matches[3])").ToUpperInvariant(); break }
            }
        } catch {}
        $svc = ''
        try { $svc = [string](Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data } catch {}
        $inf = ''
        try { $inf = [string](Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction Stop).Data } catch {}
        $com = ''
        try { $com = [string](Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters" -Name PortName -ErrorAction Stop).PortName } catch {}
        $rows += [pscustomobject]@{ MI = $mi; Class = $cls; Role = ''; Status = [string]$d.Status; Problem = [int]$d.ConfigManagerErrorCode; Service = $svc; Inf = $inf; Port = $com; Name = $d.FriendlyName; InstanceId = $d.InstanceId }
    }
    $serial = @('NMEA', 'AT', 'modem/AT'); $n = 0; $dm = $false
    foreach ($r in $rows) {
        switch -Regex ($r.Class) {
            '^FF/00/00$' { if ($n -lt 3) { $r.Role = $serial[$n] }; $n++ }
            '^FF/FF/FF$' { if (-not $dm) { $r.Role = 'DM'; $dm = $true } else { $r.Role = 'QMI network' } }
            '^02/0E/'    { $r.Role = 'MBIM' }
            '^E0/01/03$' { $r.Role = 'RNDIS' }
            '^02/06/'    { $r.Role = 'ECM' }
            '^0A/'       { $r.Role = 'net data' }
            '^01/'       { $r.Role = 'audio (UAC)' }
        }
    }
    return , $rows
}

function Show-Functions {
    $f = Get-Ec20Function
    if (-not $f.Count) { Write-Host "No interface of USB ${Vid}:$Pid_ is present." }
    else { $f | Format-Table MI, Class, Role, Status, Problem, Service, Inf, Port -AutoSize | Out-String -Width 200 | Write-Host }
    return , $f
}

function Invoke-At {
    # Sends one command and reads until its final result code (OK, ERROR,
    # +CME ERROR, +CMS ERROR, NO CARRIER) or the timeout. Returns an object:
    # Final = that result line ($null on timeout), Lines = the other lines.
    # The echo, blank lines and URCs that arrive in between (+QIND, +QUSIM,
    # +CPIN, RDY, ...) do not end the read; a caller matches its own lines.
    param([string]$Cmd, [int]$TimeoutMs = 10000, [switch]$NoReply, [string]$On = $script:AtPort)
    $sp = New-Object System.IO.Ports.SerialPort $On, 115200, 'None', 8, 'One'
    $sp.DtrEnable = $true; $sp.RtsEnable = $true
    $sp.ReadTimeout = 200; $sp.WriteTimeout = 2000
    $sp.Open()
    try {
        Start-Sleep -Milliseconds 100
        $sp.DiscardInBuffer()
        $sp.Write("$Cmd`r")
        if ($NoReply) { Start-Sleep -Milliseconds 300; return [pscustomobject]@{ Final = $null; Lines = @() } }
        $buf = ''; $lines = New-Object System.Collections.ArrayList; $final = $null
        $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
        while ($null -eq $final -and [DateTime]::UtcNow -lt $deadline) {
            try { $buf += $sp.ReadExisting() } catch {}
            # take complete lines only; a partial line stays in $buf
            while ($buf -match '^([^\r\n]*)[\r\n]+') {
                $line = $Matches[1].Trim(); $buf = $buf.Substring($Matches[0].Length)
                if (-not $line -or $line -eq $Cmd) { continue }
                if ($line -match '^(OK|ERROR|NO CARRIER|\+CME ERROR:.*|\+CMS ERROR:.*)$') { $final = $line; break }
                [void]$lines.Add($line)
            }
            if ($null -eq $final) { Start-Sleep -Milliseconds 50 }
        }
        return [pscustomobject]@{ Final = $final; Lines = @($lines) }
    } finally { $sp.Close(); $sp.Dispose() }
}

function Test-Ok { param($Reply) return ($Reply.Final -eq 'OK') }

function Show-Reply {
    param([string]$Cmd, $Reply)
    Write-Host ('> ' + (Hide $Cmd))
    foreach ($l in @($Reply.Lines) + @($Reply.Final)) { if ($l) { Write-Host ('  ' + (Hide $l)) } }
    if ($null -eq $Reply.Final) { Write-Host '  (no final result code)' }
}

function Find-AtPort {
    # Every COM port of VID 2C7C on usbser, probed with AT in interface
    # order; the first OK wins. The NMEA port comes first and stays silent.
    $cand = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like "USB\VID_$Vid&*" -and $_.Status -eq 'OK' } | Sort-Object InstanceId)
    foreach ($d in $cand) {
        $svc = ''
        try { $svc = [string](Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data } catch {}
        if ($svc -ne 'usbser') { continue }
        $com = ''
        try { $com = [string](Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters" -Name PortName -ErrorAction Stop).PortName } catch {}
        if (-not $com) { continue }
        try { if (Test-Ok (Invoke-At 'AT' -TimeoutMs 1500 -On $com)) { return $com } } catch {}
    }
    return $null
}

function Read-UsbNet {
    $r = Invoke-At 'AT+QCFG="usbnet"'
    foreach ($l in $r.Lines) { if ($l -match '^\+QCFG:\s*"usbnet",\s*(\d+)') { return [int]$Matches[1] } }
    Write-Host ('AT+QCFG="usbnet" gave: ' + (@($r.Lines) + @($r.Final) -join ' | '))
    return $null
}

function Get-MbnInterface {
    # The WWAN service's view: $true once it created a mobile broadband
    # interface, which needs the module to have answered MBIM OPEN.
    $o = (netsh mbn show interfaces) -join "`n"
    return ($o -match '(?m)^\s*Name\s*:')
}

function Get-RndisAdapter {
    # The network adapter on the module's RNDIS function: the adapter's
    # PnP device is the RNDIS interface itself.
    param($Functions)
    $r = @($Functions | Where-Object { $_.Role -eq 'RNDIS' })
    if (-not $r.Count) { return $null }
    return @(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object { $_.PnPDeviceID -eq $r[0].InstanceId })[0]
}

function Test-RndisFunction {
    # $null when the RNDIS function is up on the in-box driver with an
    # adapter, otherwise the reason. Prints what it found.
    param($Functions)
    $r = @($Functions | Where-Object { $_.Role -eq 'RNDIS' })
    if (-not $r.Count) { return 'No RNDIS (E0/01/03) function is present.' }
    $r = $r[0]
    if ($r.Service -eq 'usbser') {
        return ('MI_' + $r.MI + ' (RNDIS) is bound to usbser: the old ec20-ports.inf 1.0 (by hardware ID) is still in the driver store. ' +
            'Remove it (pnputil /enum-drivers, then pnputil /delete-driver oemNN.inf /uninstall) and install ec20-ports.inf 2.0.')
    }
    if ($r.Status -ne 'OK' -or $r.Inf -ne 'wceisvista.inf') {
        return ('The RNDIS function is ' + $r.Status + ' (problem ' + $r.Problem + ') with "' + $r.Inf + '" / "' + $r.Service + '", not OK on the in-box wceisvista.inf.')
    }
    $a = Get-RndisAdapter $Functions
    if (-not $a) { return 'The RNDIS function is bound, but no network adapter belongs to it.' }
    Write-Host ('RNDIS: ' + $a.InterfaceDescription + ', adapter "' + $a.Name + '" (ifIndex ' + $a.ifIndex + '), ' + $a.Status + ', service ' + $r.Service + ', ' + $r.Inf)
    return $null
}

function Get-Registered {
    # 1 home or 5 roaming on LTE (+CEREG) or 2G/3G (+CREG)
    foreach ($c in 'AT+CEREG?', 'AT+CREG?') {
        $r = Invoke-At $c -TimeoutMs 3000
        foreach ($l in $r.Lines) { if ($l -match '^\+C(E)?REG:\s*\d+\s*,\s*(\d+)' -and ($Matches[2] -eq '1' -or $Matches[2] -eq '5')) { return $true } }
    }
    return $false
}

function Get-CpinState {
    $r = Invoke-At 'AT+CPIN?' -TimeoutMs 5000
    foreach ($l in $r.Lines) { if ($l -match '^\+CPIN:\s*(.+)$') { return $Matches[1].Trim() } }
    return ('(' + (@($r.Lines) + @($r.Final) -join ' | ') + ')')
}

function Invoke-Connect {
    # --- SIM ---
    $st = Get-CpinState
    Write-Host "SIM: $st"
    if ($st -match 'PUK') { Write-Host 'The SIM wants its PUK. Not handled here; use a phone.'; return 7 }
    if ($st -eq 'SIM PIN') {
        if (-not $Pin) { Write-Host 'The SIM needs its PIN: run again with -Pin <PIN>.'; return 7 }
        $left = $null
        foreach ($l in (Invoke-At 'AT+QPINC="SC"').Lines) { if ($l -match '^\+QPINC:\s*"SC",\s*(\d+)') { $left = [int]$Matches[1] } }
        Write-Host ("PIN attempts left: $left")
        if ($null -ne $left -and $left -le 1) { Write-Host 'Only one PIN attempt left: not trying. Enter it on a phone if you are sure.'; return 7 }
        $r = Invoke-At ('AT+CPIN="' + $Pin + '"') -TimeoutMs 10000
        Show-Reply ('AT+CPIN="' + $Pin + '"') $r
        if (-not (Test-Ok $r)) { Write-Host 'The PIN was refused.'; return 7 }
        $deadline = (Get-Date).AddSeconds(20)
        do { Start-Sleep -Seconds 2; $st = Get-CpinState } while ($st -ne 'READY' -and (Get-Date) -lt $deadline)
        Write-Host "SIM: $st"
    }
    if ($st -ne 'READY') { Write-Host 'The SIM is not ready (no SIM, or still initialising).'; return 7 }

    # --- APN on context 1 ---
    $r = Invoke-At 'AT+CGDCONT?'; Show-Reply 'AT+CGDCONT?' $r
    $type = 'IPV4V6'; $have = $null
    foreach ($l in $r.Lines) { if ($l -match '^\+CGDCONT:\s*1\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"') { $type = $Matches[1]; $have = $Matches[2] } }
    if ($Apn -and $Apn -ne $have) {
        $cmd = 'AT+CGDCONT=1,"' + $type + '","' + $Apn + '"'
        $r = Invoke-At $cmd; Show-Reply $cmd $r
        if (-not (Test-Ok $r)) { return 1 }
        # Context 1 is the LTE attach APN: re-attach with the radio off and on.
        Write-Host 'APN changed: radio off and on (AT+CFUN=4, AT+CFUN=1) to attach with it.'
        [void](Invoke-At 'AT+CFUN=4' -TimeoutMs 15000)
        Start-Sleep -Seconds 2
        [void](Invoke-At 'AT+CFUN=1' -TimeoutMs 15000)
    } elseif (-not $have) {
        Write-Host 'Context 1 has no APN; the network assigns one at attach. If data does not come up, pass -Apn.'
    }

    # --- registration ---
    $deadline = (Get-Date).AddSeconds($WaitSeconds); $reg = $false
    while ((Get-Date) -lt $deadline) { if (Get-Registered) { $reg = $true; break }; Start-Sleep -Seconds 3 }
    foreach ($c in 'AT+COPS?', 'AT+CSQ', 'AT+QNWINFO') { Show-Reply $c (Invoke-At $c -TimeoutMs 5000) }
    if (-not $reg) { Write-Host "Not registered within $WaitSeconds s (+CEREG/+CREG)."; return 8 }

    # --- the dial ---
    # AT+QNETDEVCTL=<op>,<cid>,<urc_en> (op 1 = connect) is Quectel's ECM/
    # RNDIS dial on the EC200 and later lines; Qualcomm-based EC2x firmware
    # mostly lacks it and dials by itself after registration. Ask first.
    $t = Invoke-At 'AT+QNETDEVCTL=?' -TimeoutMs 3000
    if (Test-Ok $t) {
        Show-Reply 'AT+QNETDEVCTL=?' $t
        $r = Invoke-At 'AT+QNETDEVCTL=1,1,1' -TimeoutMs 30000; Show-Reply 'AT+QNETDEVCTL=1,1,1' $r
        if (-not (Test-Ok $r)) { Write-Host 'The dial was refused; waiting for the module to dial by itself.' }
    } else {
        Write-Host ('AT+QNETDEVCTL is not supported (' + $t.Final + '): this firmware dials by itself in RNDIS mode.')
    }
    foreach ($c in 'AT+CGACT?', 'AT+CGPADDR=1') { Show-Reply $c (Invoke-At $c -TimeoutMs 5000) }

    # --- DHCP on the adapter ---
    $f = Get-Ec20Function
    $why = Test-RndisFunction $f
    if ($why) { Write-Host $why; return 5 }
    $a = Get-RndisAdapter $f
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Write-Host ('Waiting for a DHCP address on "' + $a.Name + '" (up to ' + $WaitSeconds + ' s)...')
    $deadline = (Get-Date).AddSeconds($WaitSeconds); $ip = $null; $renewAt = (Get-Date)
    while ((Get-Date) -lt $deadline) {
        $ip = @(Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -notlike '169.254.*' })[0]
        if ($ip) { break }
        # The link may have come up before the module dialled, and Windows
        # then retries DHCP only every few minutes. Renew needs elevation.
        if ($admin -and (Get-Date) -ge $renewAt) {
            Write-Host 'ipconfig /renew on the adapter'
            & ipconfig.exe /renew $a.Name | Out-Null
            $renewAt = (Get-Date).AddSeconds(30)
        }
        Start-Sleep -Seconds 3
    }
    if (-not $ip) {
        Write-Host 'No DHCP address. Adapter:'
        Get-NetIPConfiguration -InterfaceIndex $a.ifIndex -ErrorAction SilentlyContinue | Out-String | Write-Host
        if (-not $admin) { Write-Host ('Not elevated, so no renew was tried: run elevated, or ipconfig /renew "' + $a.Name + '".') }
        return 9
    }
    $cfg = Get-NetIPConfiguration -InterfaceIndex $a.ifIndex -ErrorAction SilentlyContinue
    Write-Host ('Connected: ' + $ip.IPAddress + '/' + $ip.PrefixLength + ' on "' + $a.Name + '"')
    if ($cfg) {
        Write-Host ('Gateway: ' + (@($cfg.IPv4DefaultGateway | ForEach-Object { $_.NextHop }) -join ', '))
        Write-Host ('DNS: ' + (@($cfg.DNSServer | Where-Object { $_.AddressFamily -eq 2 } | ForEach-Object { $_.ServerAddresses }) -join ', '))
    }
    Write-Host ('Check: ping -S ' + $ip.IPAddress + ' 1.1.1.1')
    return 0
}

# --- find the AT port ---------------------------------------------------
$script:AtPort = $Port
if (-not $script:AtPort) { $script:AtPort = Find-AtPort }
if (-not $script:AtPort) {
    Write-Host 'No COM port on USB VID 2C7C answers AT (probed every usbser port of the module).'
    [void](Show-Functions)
    Write-Host 'The serial interfaces are vendor class, so Windows binds nothing to them by itself. Install'
    Write-Host 'ec20-ports.inf 2.0 from https://github.com/gahingwoo/ec20-woa (in-box usbser.sys, by class):'
    Write-Host '  .\tools\Install-Ec20Driver.ps1 -Variant Usbser -ImportCertificate'
    Write-Host 'or switch the mode from Linux (tools/ec20/ec20-mode.sh in this repository).'
    exit 3
}
Write-Host "AT port: $script:AtPort"

$cur = Read-UsbNet
if ($null -eq $cur) { Write-Host 'The module did not report its usbnet mode.'; exit 1 }
$rev = @((Invoke-At 'AT+QGMR').Lines | Where-Object { $_ -match '^EC|^EG|^EM|R\d\dA' })[0]
Write-Host ("Firmware: $rev")
Write-Host ("usbnet: $cur, " + $ModeName[$cur])

if (-not $Mode -and -not $Connect) {
    $f = Show-Functions
    $m = @($f | Where-Object { $_.Role -eq 'MBIM' })
    if ($m.Count) {
        Write-Host ("MBIM function: " + $m[0].Status + " (problem " + $m[0].Problem + "), service " + $m[0].Service)
        if (Get-MbnInterface) { Write-Host 'Mobile broadband interface: present (netsh mbn show interfaces)' }
        else { Write-Host 'Mobile broadband interface: none. The WWAN service creates it only after the module answers MBIM OPEN.' }
    }
    if (@($f | Where-Object { $_.Role -eq 'RNDIS' }).Count) {
        $why = Test-RndisFunction $f
        if ($why) { Write-Host $why }
    }
    exit 0
}

if ($Mode) {
    $want = $ModeCode[$Mode]
    if ($cur -eq $want) {
        Write-Host ("Already in " + $ModeName[$want] + " mode; nothing to set.")
    } else {
        if (-not $PSCmdlet.ShouldProcess("EC20 on $script:AtPort", "set usbnet $want (" + $ModeName[$want] + "), stored in the module, then reset it")) { exit 1 }
        $r = Invoke-At "AT+QCFG=`"usbnet`",$want" -TimeoutMs 15000
        # The read-back decides, not the reply: EC20CEFILGR08A03M1G answers
        # ERROR to usbnet 2 and 3 and stores the value (TaishanPi, 2026-11-26).
        $rb = Read-UsbNet
        if ($rb -ne $want) {
            if ($r.Final -and $r.Final -ne 'OK') { Write-Host ("The module refused the mode: " + $r.Final + ", read-back $rb"); exit 1 }
            Write-Host "Read-back before the reset: $rb, expected $want. This firmware may not support it."; exit 2
        }
        if ($r.Final -ne 'OK') { Write-Host ("The set gave '" + $r.Final + "' but the read-back shows $rb; going on.") }
        Write-Host "Set and read back: $rb"
        if ($NoReset) { Write-Host 'Not reset (-NoReset): the mode takes effect at the next power cycle.'; exit 0 }

        Write-Host 'Resetting the module (AT+CFUN=1,1); it leaves USB for 20 to 40 s.'
        try { [void](Invoke-At 'AT+CFUN=1,1' -NoReply) } catch {}
        Start-Sleep -Seconds 10
        $deadline = (Get-Date).AddSeconds($WaitSeconds); $back = $false
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 3
            # the AT port moves to another interface (and COM number) with the mode
            if (-not $Port) { $p = Find-AtPort; if (-not $p) { continue }; $script:AtPort = $p; $back = $true; break }
            try { if (Test-Ok (Invoke-At 'AT' -TimeoutMs 1500)) { $back = $true; break } } catch {}
        }
        if (-not $back) { Write-Host "No AT port answered within $WaitSeconds s."; [void](Show-Functions); exit 4 }
        Write-Host "AT port: $script:AtPort"
        $after = Read-UsbNet
        Write-Host "After the reset: usbnet $after"
        if ($after -ne $want) { exit 2 }
        # PnP needs a few seconds to install the new network function
        Start-Sleep -Seconds 10
    }

    $f = Show-Functions
    if ($want -eq 3) {
        $why = Test-RndisFunction $f
        if ($why) { Write-Host $why; exit 5 }
    } elseif ($want -eq 2) {
        $m = @($f | Where-Object { $_.Role -eq 'MBIM' })
        $d = @($f | Where-Object { $_.Role -eq 'net data' })
        if (-not $m.Count) { Write-Host 'No MBIM (02/0E) function is present.'; exit 5 }
        if ($d.Count) {
            Write-Host 'The MBIM data interface is a separate function: the module declares no IAD and Windows'
            Write-Host 'cannot group the two MBIM interfaces. Switch back with -Mode qmi and report this.'
            exit 5
        }
        $mbimSilent = @('No mobile broadband interface: the module did not answer MBIM OPEN, and cxwmbclass',
            'stopped the device (code 43). Restarting the device only repeats the 80 s cycle.',
            'See docs/dev/ec20.md, "MBIM on this firmware". Try -Mode rndis.')
        if ($m[0].Service -match '^(cx)?wmbclass$' -and $m[0].Problem -eq 43) {
            $mbimSilent | ForEach-Object { Write-Host $_ }
            exit 6
        }
        if ($m[0].Status -ne 'OK' -or $m[0].Service -notmatch '^(cx)?wmbclass$') {
            Write-Host ('The MBIM function is ' + $m[0].Status + ' with service "' + $m[0].Service + '", not OK on the in-box MBIM driver.')
            exit 5
        }
        # Bound is not enough: cxwmbclass sends MBIM OPEN at start, retries at
        # 12 s and 36 s, sends CLOSE at 72 s and fails the device (code 43) at
        # about 80 s if the module never answers. Wait for one or the other.
        Write-Host 'Waiting for the WWAN service to create the mobile broadband interface (up to 100 s)...'
        $deadline = (Get-Date).AddSeconds(100); $ok = $false
        while ((Get-Date) -lt $deadline) {
            if (Get-MbnInterface) { $ok = $true; break }
            $dd = Get-PnpDevice -InstanceId $m[0].InstanceId -ErrorAction SilentlyContinue
            if ($dd -and $dd.ConfigManagerErrorCode -ne 0) { break }
            Start-Sleep -Seconds 3
        }
        if (-not $ok) { $mbimSilent | ForEach-Object { Write-Host $_ }; exit 6 }
        Write-Host ('MBIM is up on ' + $m[0].Service + '. Settings > Network & internet > Cellular should be there now.')
        Write-Host 'Check: netsh mbn show interfaces ; netsh mbn show readyinfo *'
    } else {
        Write-Host 'Back in QMI mode: MI_04 is again without a Windows driver (expected; regress whitelists it).'
    }
    $cur = $want
}

if ($Connect) {
    if ($cur -ne 3) { Write-Host ('-Connect is for RNDIS mode; the module is in ' + $ModeName[$cur] + '. Add -Mode rndis.'); exit 1 }
    $rc = @(Invoke-Connect)
    exit ([int]$rc[-1])
}
exit 0
