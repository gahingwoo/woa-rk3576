# regress.ps1 - the RK3576 Windows regression suite: one command per board,
# one PASS / FAIL / SKIP / N-A row per check.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File regress.ps1
#       [-Board auto|tspi3m|cm5io|rock4d] [-Quick] [-Stress [-StressSeconds N]]
#       [-Out DIR] [-ToolsDir DIR] [-User NAME] [-ExpectFirmware REGEX]
#       [-Require nvme,sd,...] [-FetchStreams] [-DryRun -Mock FILE]
#
# Run on the board as an administrator (an SSH session of an administrator
# is elevated). Reads only: it installs nothing, changes no driver, firmware
# or boot setting and never restarts the board. What it writes: the results
# folder, a temporary file on the system disk for the storage check (deleted
# afterwards), and, for the checks that need the desktop (display mode,
# -Stress), a one-off scheduled task for the signed-in user that it deletes
# again. -Stress (FurMark through GpuTest) is the only heavy check and runs
# only when asked; it stops GpuTest as soon as any temperature reaches 90 C.
#
# Output: DIR\results.json and DIR\results.md (default
# C:\RK3576-regress\<board>-<time>). Exit code: 0 no FAIL, 1 at least one
# FAIL, 2 the suite could not run (not elevated, unknown board).
#
# -DryRun -Mock tests\mock-<board>.json evaluates every check against
# recorded facts instead of the machine (CI runs it on windows-latest).
#
# Windows PowerShell 5.1 syntax only. Saved as UTF-8 with a BOM: the GpuTest
# default path below is not ASCII.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

[CmdletBinding()]
param(
    [ValidateSet('auto', 'tspi3m', 'cm5io', 'rock4d')]
    [string]$Board = 'auto',
    [switch]$Quick,
    [switch]$Stress,
    [int]$StressSeconds = 300,
    [string]$Out = '',
    [string]$ToolsDir = '',
    [string]$User = '',
    [string]$ExpectFirmware = '',
    [string[]]$Require = @(),
    [switch]$FetchStreams,
    [string]$GpuTest = 'C:\图吧工具箱\图吧工具箱202608\tools\显卡工具\GpuTest_Windows x64\GpuTest.exe',
    [switch]$DryRun,
    [string]$Mock = ''
)

$ErrorActionPreference = 'Continue'
Set-StrictMode -Off
$script:Start = Get-Date
$script:Results = New-Object System.Collections.Generic.List[object]
$script:Here = $PSScriptRoot
$script:ThermalAbortC = 90

# ------------------------------------------------------------------ helpers

function Add-Result {
    param([string]$Id, [string]$Name, [string]$Status, $Value = '', $Expected = '', [string]$Reason = '')
    $o = [pscustomobject][ordered]@{
        id       = $Id
        name     = $Name
        status   = $Status
        value    = [string]$Value
        expected = [string]$Expected
        reason   = $Reason
    }
    $script:Results.Add($o)
    $c = 'Gray'
    if ($Status -eq 'PASS') { $c = 'Green' } elseif ($Status -eq 'FAIL') { $c = 'Red' } elseif ($Status -eq 'SKIP') { $c = 'Yellow' }
    $line = '{0,-4} {1,-26} {2}' -f $Status, $Id, $Value
    if ($Reason) { $line += "  ($Reason)" }
    Write-Host $line -ForegroundColor $c
}

# A capability from the board manifest: 'yes', 'no' or 'optional'.
# -Require turns an optional one into yes for this run.
function Cap {
    param($Value, [string]$Name = '')
    if ($Name -and ($script:RequireList -contains $Name)) { return 'yes' }
    if ($Value -is [bool]) { if ($Value) { return 'yes' } else { return 'no' } }
    if ($null -eq $Value) { return 'no' }
    $s = [string]$Value
    if ($s -eq 'optional') { return 'optional' }
    if ($s -match '^(true|yes)$') { return 'yes' }
    return 'no'
}

# A fact about the machine. Live: the script block's result; -DryRun: the
# mock file's entry of that name ($null when the mock has none).
function Fact {
    param([string]$Name, [scriptblock]$Collect)
    if ($script:DryRunMode) {
        $f = $script:MockData.facts
        if ($f -and ($f.PSObject.Properties.Name -contains $Name)) { return , $f.$Name }
        return $null
    }
    try { $r = & $Collect; return , $r }
    catch {
        Write-Verbose "fact $Name : $($_.Exception.Message)"
        return $null
    }
}

function Find-Tool {
    param([string]$Name)
    if ($script:DryRunMode) {
        $t = $script:MockData.tools
        if ($t) { foreach ($p in $t.PSObject.Properties) { if ($p.Name -like "$Name *" -or $p.Name -eq $Name) { return "mock:$Name" } } }
        return $null
    }
    $dirs = @()
    if ($ToolsDir) { $dirs += $ToolsDir }
    $dirs += (Join-Path $script:Here 'bin')
    if ($env:ProgramFiles) {
        $dirs += (Join-Path $env:ProgramFiles 'RK3576')
        $dirs += (Join-Path $env:ProgramFiles 'RK3576\npu')
    }
    foreach ($d in $dirs) {
        $p = Join-Path $d "$Name.exe"
        if (Test-Path -LiteralPath $p) { return $p }
    }
    $c = Get-Command "$Name.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    return $null
}

# Run a console tool with a timeout. Key names the run in a mock file.
function Run-Tool {
    param([string]$Key, [string]$Exe, [string[]]$Arguments = @(), [int]$TimeoutSec = 120)
    if ($script:DryRunMode) {
        $t = $script:MockData.tools
        if ($t -and ($t.PSObject.Properties.Name -contains $Key)) {
            $m = $t.$Key
            return [pscustomobject]@{ ExitCode = [int]$m.exit; Output = [string]$m.output; TimedOut = [bool]$m.timedOut }
        }
        return [pscustomobject]@{ ExitCode = -1; Output = "no mock for '$Key'"; TimedOut = $false }
    }
    $o = Join-Path $script:OutDir ('tool-' + ($Key -replace '[^A-Za-z0-9_.-]', '_') + '.txt')
    $e = "$o.err"
    $quoted = @()
    foreach ($a in $Arguments) { if ($a -match '\s') { $quoted += '"' + $a + '"' } else { $quoted += $a } }
    $sp = @{ FilePath = $Exe; RedirectStandardOutput = $o; RedirectStandardError = $e; PassThru = $true; NoNewWindow = $true }
    if ($quoted.Count) { $sp.ArgumentList = $quoted }
    $p = Start-Process @sp
    $null = $p.Handle  # keeps the exit code readable after the process ends
    $timedOut = -not $p.WaitForExit($TimeoutSec * 1000)
    if ($timedOut) { try { $p.Kill() } catch {} }
    $text = ''
    if (Test-Path $o) { $text = Get-Content -Raw $o }
    if ((Test-Path $e) -and (Get-Item $e).Length) { $text += "`n" + (Get-Content -Raw $e) }
    Remove-Item $e -ErrorAction SilentlyContinue
    $code = -1
    if (-not $timedOut) { $code = $p.ExitCode }
    return [pscustomobject]@{ ExitCode = $code; Output = [string]$text; TimedOut = $timedOut }
}

# The native helpers: English-name PDH counters (the names do not change
# with the display language, as rk-monitor reads them), the display modes
# (used in the user session) and the ACPI table header.
$script:NativeSrc = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class RkRegressNative {
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] static extern uint PdhOpenQueryW(string src, IntPtr user, out IntPtr q);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] static extern uint PdhAddEnglishCounterW(IntPtr q, string path, IntPtr user, out IntPtr c);
    [DllImport("pdh.dll")] static extern uint PdhCollectQueryData(IntPtr q);
    [DllImport("pdh.dll")] static extern uint PdhCloseQuery(IntPtr q);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] static extern uint PdhGetFormattedCounterArrayW(IntPtr c, uint fmt, ref uint bytes, ref uint count, IntPtr items);
    [DllImport("kernel32.dll")] static extern uint GetSystemFirmwareTable(uint provider, uint id, byte[] buf, uint size);

    // instance name -> value; throws with the PDH status when the counter is not there
    public static Dictionary<string, double> Counter(string path, int sampleMs) {
        IntPtr q, c;
        uint st = PdhOpenQueryW(null, IntPtr.Zero, out q);
        if (st != 0) throw new Exception("PdhOpenQuery 0x" + st.ToString("X8"));
        try {
            st = PdhAddEnglishCounterW(q, path, IntPtr.Zero, out c);
            if (st != 0) throw new Exception("counter not found (PDH 0x" + st.ToString("X8") + ")");
            PdhCollectQueryData(q);
            System.Threading.Thread.Sleep(sampleMs);
            PdhCollectQueryData(q);
            uint bytes = 0, count = 0;
            const uint FMT = 0x200 | 0x8000; // PDH_FMT_DOUBLE | PDH_FMT_NOCAP100
            PdhGetFormattedCounterArrayW(c, FMT, ref bytes, ref count, IntPtr.Zero);
            var r = new Dictionary<string, double>();
            if (bytes == 0) return r;
            IntPtr buf = Marshal.AllocHGlobal((int)bytes);
            try {
                st = PdhGetFormattedCounterArrayW(c, FMT, ref bytes, ref count, buf);
                if (st != 0) throw new Exception("PdhGetFormattedCounterArray 0x" + st.ToString("X8"));
                int sz = IntPtr.Size == 8 ? 24 : 16;
                int voff = IntPtr.Size == 8 ? 16 : 8;
                for (int i = 0; i < count; i++) {
                    IntPtr item = new IntPtr(buf.ToInt64() + (long)i * sz);
                    string name = Marshal.PtrToStringUni(Marshal.ReadIntPtr(item));
                    int cst = Marshal.ReadInt32(item, IntPtr.Size);
                    double v = BitConverter.Int64BitsToDouble(Marshal.ReadInt64(item, voff));
                    if (cst == 0 || cst == 1) r[name ?? ""] = v;
                }
            } finally { Marshal.FreeHGlobal(buf); }
            return r;
        } finally { PdhCloseQuery(q); }
    }

    // OEM ID and OEM table ID of an ACPI table ("DSDT", "FACP", ...)
    public static string[] AcpiOem(string sig) {
        uint id = (uint)(sig[0] | (sig[1] << 8) | (sig[2] << 16) | (sig[3] << 24));
        const uint ACPI = 0x41435049;
        uint n = GetSystemFirmwareTable(ACPI, id, null, 0);
        if (n < 36) return null;
        byte[] b = new byte[n];
        GetSystemFirmwareTable(ACPI, id, b, n);
        return new string[] {
            System.Text.Encoding.ASCII.GetString(b, 10, 6).TrimEnd(' ', '\0'),
            System.Text.Encoding.ASCII.GetString(b, 16, 8).TrimEnd(' ', '\0'),
            BitConverter.ToUInt32(b, 24).ToString()
        };
    }
}
'@

function Load-Native {
    if ($script:NativeLoaded) { return $true }
    try {
        Add-Type -TypeDefinition $script:NativeSrc -Language CSharp -ErrorAction Stop
        $script:NativeLoaded = $true
    } catch {
        if ('RkRegressNative' -as [type]) { $script:NativeLoaded = $true } else { Write-Verbose "Add-Type: $_" }
    }
    return $script:NativeLoaded
}

function Read-Counter {
    param([string]$Path, [int]$SampleMs = 300)
    if (-not (Load-Native)) { throw 'native helper did not compile' }
    return [RkRegressNative]::Counter($Path, $SampleMs)
}

# The one value of a single-instance counter, or $null with $script:CounterError set.
function Counter-Value {
    param([string]$Path)
    try {
        $d = Read-Counter $Path
        foreach ($k in $d.Keys) { return [double]$d[$k] }
        $script:CounterError = 'no instance'
    } catch { $script:CounterError = $_.Exception.Message }
    return $null
}

# The parents of a PnP device up to its ACPI node.
function Pnp-Ancestors {
    param([string]$InstanceId)
    $chain = @()
    $id = $InstanceId
    for ($i = 0; $i -lt 10 -and $id; $i++) {
        $p = (Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data
        if (-not $p) { break }
        $chain += $p
        if ($p -like 'ACPI\*') { break }
        $id = $p
    }
    return $chain
}

function Pnp-Service {
    param([string]$InstanceId)
    return (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
}

# One ACPI device by _HID: present, status, problem code, service.
function Acpi-Device {
    param([string]$Hid)
    if (-not $script:PnpCache) { $script:PnpCache = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue) }
    $d = $script:PnpCache | Where-Object { $_.InstanceId -like "ACPI\$Hid\*" } | Select-Object -First 1
    if (-not $d) { return [pscustomobject]@{ present = $false } }
    return [pscustomobject]@{
        present    = $true
        instanceId = $d.InstanceId
        name       = $d.FriendlyName
        status     = [string]$d.Status
        problem    = [int]$d.ConfigManagerErrorCode
        service    = (Pnp-Service $d.InstanceId)
    }
}

function Temps-Now {
    # \Thermal Zone Information(*)\High Precision Temperature, in tenths of a
    # kelvin, as rk-monitor reads it (not MSAcpi_ThermalZoneTemperature, which
    # returns a stale cached value on these boards); WMI's copy of the same
    # counter set when PDH is not available
    $z = @()
    try {
        $d = Read-Counter '\Thermal Zone Information(*)\High Precision Temperature' 100
        foreach ($k in ($d.Keys | Sort-Object)) { $z += [pscustomobject]@{ name = $k; c = [math]::Round($d[$k] / 10.0 - 273.15, 1) } }
        if ($z.Count) { return $z }
    } catch {}
    foreach ($t in (Get-CimInstance -ClassName Win32_PerfFormattedData_Counters_ThermalZoneInformation -ErrorAction SilentlyContinue)) {
        $k = [double]$t.HighPrecisionTemperature / 10.0
        if ($k -le 0) { $k = [double]$t.Temperature }
        $z += [pscustomobject]@{ name = $t.Name; c = [math]::Round($k - 273.15, 1) }
    }
    return $z
}

# The signed-in user, for the checks that need the desktop.
function Session-User {
    if ($User) { return $User }
    $u = (Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).UserName
    if ($u) { return $u }
    $ex = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($ex) {
        $o = Invoke-CimMethod -InputObject $ex -MethodName GetOwner -ErrorAction SilentlyContinue
        if ($o -and $o.User) { if ($o.Domain) { return "$($o.Domain)\$($o.User)" } else { return $o.User } }
    }
    return $null
}

# Run a PowerShell script in the signed-in user's session: an SSH session is
# session 0 and has no desktop. A one-off task for that user, interactive
# (/it), started through conhost --headless so no window appears, deleted
# afterwards. Returns $null when nobody is signed in or the task never ran.
function Invoke-UserSession {
    param([string]$Name, [string]$Script, [int]$TimeoutSec = 60, [scriptblock]$WhileRunning = $null)
    $u = Session-User
    if (-not $u) { $script:SessionError = 'nobody is signed in on the desktop (autologon off?)'; return $null }
    $dir = Join-Path $env:ProgramData 'RK3576-regress\session'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    & icacls.exe $dir /grant '*S-1-5-32-545:(OI)(CI)M' /q | Out-Null
    $ps1 = Join-Path $dir "$Name.ps1"
    $cmd = Join-Path $dir "$Name.cmd"
    $done = Join-Path $dir "$Name.done"
    $res = Join-Path $dir "$Name.json"
    Remove-Item $done, $res -ErrorAction SilentlyContinue
    # UTF-8 with a BOM, which Windows PowerShell 5.1 needs for non-ASCII text
    $enc = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllText($ps1, $Script, $enc)
    Set-Content -Path $cmd -Encoding ASCII -Value @(
        '@echo off',
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ps1 -Out $res",
        "echo %ERRORLEVEL% > $done"
    )
    $tn = "RK3576-regress-$Name-$PID"
    $tr = "conhost.exe --headless cmd.exe /c $cmd"
    $o = & schtasks.exe /create /tn $tn /tr $tr /sc once /st 00:00 /ru $u /it /f 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { $script:SessionError = "schtasks /create: $($o.Trim())"; return $null }
    try {
        $o = & schtasks.exe /run /tn $tn 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { $script:SessionError = "schtasks /run: $($o.Trim())"; return $null }
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while (-not (Test-Path $done) -and (Get-Date) -lt $deadline) {
            if ($WhileRunning) {
                $stop = & $WhileRunning
                if ($stop) { break }
            } else { Start-Sleep -Milliseconds 500 }
        }
        if (-not (Test-Path $done)) { $script:SessionError = "the task in $u's session did not finish in $TimeoutSec s"; return $null }
        if (-not (Test-Path $res)) { $script:SessionError = 'the task wrote no result'; return $null }
        return (Get-Content -Raw $res | ConvertFrom-Json)
    } finally {
        & schtasks.exe /delete /tn $tn /f 2>&1 | Out-Null
    }
}

$script:DisplayProbe = @'
param([string]$Out)
$src = @"
using System;
using System.Runtime.InteropServices;
public static class RkModes {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool EnumDisplaySettingsW(string dev, int mode, ref DEVMODE dm);
    public static int[] Mode(int i) {
        DEVMODE d = new DEVMODE(); d.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        if (!EnumDisplaySettingsW(null, i, ref d)) return null;
        return new int[] { d.dmPelsWidth, d.dmPelsHeight, d.dmDisplayFrequency, d.dmBitsPerPel };
    }
}
"@
Add-Type -TypeDefinition $src
$cur = [RkModes]::Mode(-1)
$modes = @()
for ($i = 0; $i -lt 4096; $i++) {
    $m = [RkModes]::Mode($i)
    if (-not $m) { break }
    $modes += ('{0}x{1}@{2}' -f $m[0], $m[1], $m[2])
}
$r = [pscustomobject]@{
    current = $(if ($cur) { '{0}x{1}@{2}' -f $cur[0], $cur[1], $cur[2] } else { '' })
    modes   = @($modes | Sort-Object -Unique)
}
$r | ConvertTo-Json -Depth 3 | Set-Content -Path $Out -Encoding UTF8
'@

# ------------------------------------------------------------------ setup

$script:DryRunMode = [bool]$DryRun
$script:MockData = $null
if ($DryRun) {
    if (-not $Mock -or -not (Test-Path $Mock)) { Write-Host "-DryRun needs -Mock FILE"; exit 2 }
    $script:MockData = Get-Content -Raw -Encoding UTF8 $Mock | ConvertFrom-Json
} else {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host 'Run this as an administrator (an administrator SSH session is elevated).'
        exit 2
    }
}
$script:RequireList = @()
foreach ($r in $Require) { $script:RequireList += ($r -split ',') | ForEach-Object { $_.Trim().ToLower() } | Where-Object { $_ } }

$smbios = Fact 'smbios' {
    $cs = Get-CimInstance Win32_ComputerSystem
    $bios = Get-CimInstance Win32_BIOS
    $bb = Get-CimInstance Win32_BaseBoard -ErrorAction SilentlyContinue
    $os = Get-CimInstance Win32_OperatingSystem
    [pscustomobject]@{
        manufacturer = $cs.Manufacturer
        product      = $cs.Model
        baseboard    = $(if ($bb) { $bb.Product } else { '' })
        biosVersion  = $bios.SMBIOSBIOSVersion
        biosDate     = $(if ($bios.ReleaseDate) { $bios.ReleaseDate.ToString('yyyy-MM-dd') } else { '' })
        windows      = "$($os.Caption) $($os.Version) build $($os.BuildNumber)"
        computer     = $env:COMPUTERNAME
    }
}

$manifests = @{}
foreach ($f in (Get-ChildItem (Join-Path $script:Here 'boards') -Filter *.json)) {
    $mf = Get-Content -Raw -Encoding UTF8 $f.FullName | ConvertFrom-Json
    $manifests[$mf.id] = $mf
}
$boardId = $Board
$detectedBy = 'command line'
if ($Board -eq 'auto') {
    $boardId = $null
    foreach ($k in $manifests.Keys) {
        $s = $manifests[$k].smbios
        if ($smbios -and (($smbios.product -eq $s.product) -or ([string]$smbios.biosVersion -like "rk3576-$($s.platformName)-*"))) {
            $boardId = $k; $detectedBy = 'SMBIOS'; break
        }
    }
    if (-not $boardId) {
        Write-Host "Unknown board: SMBIOS says '$($smbios.manufacturer)' '$($smbios.product)', BIOS '$($smbios.biosVersion)'. Pass -Board."
        exit 2
    }
}
$M = $manifests[$boardId]

if (-not $Out) { $Out = Join-Path 'C:\RK3576-regress' ("{0}-{1}" -f $boardId, $script:Start.ToString('yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$script:OutDir = (Resolve-Path $Out).Path

$suiteVersion = 'unknown'
$vf = Join-Path $script:Here 'VERSION'
if (Test-Path $vf) { $suiteVersion = (Get-Content $vf -TotalCount 1).Trim() }

Write-Host "RK3576 regression suite ${suiteVersion}: $($M.name) ($boardId, by $detectedBy)$(if ($DryRun) { ', DRY RUN' })"
Write-Host "results: $script:OutDir"
Write-Host ''

# ------------------------------------------------------------------ checks

# Firmware -----------------------------------------------------------------
$bv = [string]$smbios.biosVersion
$want = "rk3576-$($M.smbios.platformName)-*"
if (-not $bv) {
    Add-Result 'fw.banner' 'Firmware version string (SMBIOS type 0)' 'FAIL' '' $want 'no SMBIOS BIOS version'
} elseif ($bv -notlike $want) {
    Add-Result 'fw.banner' 'Firmware version string (SMBIOS type 0)' 'FAIL' $bv $want 'not a build of this board'
} elseif ($ExpectFirmware -and ($bv -notmatch $ExpectFirmware)) {
    Add-Result 'fw.banner' 'Firmware version string (SMBIOS type 0)' 'FAIL' $bv $ExpectFirmware 'not the expected build'
} else {
    $note = ''
    if ($bv -like '*-dirty') { $note = 'built from a tree with uncommitted changes' }
    $exp = $want
    if ($ExpectFirmware) { $exp = $ExpectFirmware }
    Add-Result 'fw.banner' 'Firmware version string (SMBIOS type 0)' 'PASS' "$bv ($($smbios.biosDate))" $exp $note
}

$acpi = Fact 'acpi' {
    if (-not (Load-Native)) { throw 'no native helper' }
    $d = [RkRegressNative]::AcpiOem('DSDT')
    if ($d) { [pscustomobject]@{ oemId = $d[0]; oemTableId = $d[1]; revision = $d[2] } }
}
if (-not $acpi) {
    Add-Result 'fw.acpi' 'ACPI tables (DSDT OEM ID)' 'FAIL' '' 'RKCP / RK3576' 'no DSDT read (device tree mode, or the native helper failed)'
} elseif ($acpi.oemId -eq 'RKCP' -and $acpi.oemTableId -eq 'RK3576') {
    Add-Result 'fw.acpi' 'ACPI tables (DSDT OEM ID)' 'PASS' "$($acpi.oemId) / $($acpi.oemTableId)" 'RKCP / RK3576'
} else {
    Add-Result 'fw.acpi' 'ACPI tables (DSDT OEM ID)' 'FAIL' "$($acpi.oemId) / $($acpi.oemTableId)" 'RKCP / RK3576'
}

$bstat = 'PASS'; $breason = ''
if ($smbios.manufacturer -ne $M.smbios.manufacturer -or $smbios.product -ne $M.smbios.product) {
    $bstat = 'FAIL'; $breason = "SMBIOS is not this board's (selected by $detectedBy)"
}
Add-Result 'board.smbios' 'Board identity (SMBIOS type 1)' $bstat "$($smbios.manufacturer) $($smbios.product)" "$($M.smbios.manufacturer) $($M.smbios.product)" $breason

# CPU ----------------------------------------------------------------------
$cpu = Fact 'cpu' {
    $n = [int](Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
    $reg = @()
    foreach ($k in (Get-ChildItem 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor' -ErrorAction SilentlyContinue)) {
        $reg += [int](Get-ItemProperty $k.PSPath).'~MHz'
    }
    $pf = @()
    foreach ($p in (Get-CimInstance Win32_PerfFormattedData_Counters_ProcessorInformation -ErrorAction SilentlyContinue)) {
        if ($p.Name -notmatch '_Total') { $pf += [int]$p.ProcessorFrequency }
    }
    [pscustomobject]@{ count = $n; registryMHz = $reg; currentMHz = $pf }
}
if (-not $cpu) {
    Add-Result 'cpu.count' 'CPUs online' 'FAIL' '' $M.cpu.count 'could not read'
} else {
    $st = 'PASS'; if ([int]$cpu.count -ne [int]$M.cpu.count) { $st = 'FAIL' }
    Add-Result 'cpu.count' 'CPUs online' $st $cpu.count $M.cpu.count
    $all = @($cpu.registryMHz) + @($cpu.currentMHz) | Where-Object { $_ -gt 0 }
    if (-not $all) {
        Add-Result 'cpu.freq' 'CPU maximum frequency' 'FAIL' '' ">= $($M.cpu.minMHz) MHz" 'no frequency reported'
    } else {
        $max = ($all | Measure-Object -Maximum).Maximum
        $st = 'PASS'; if ($max -lt [int]$M.cpu.minMHz) { $st = 'FAIL' }
        $v = "$max MHz (registry $((@($cpu.registryMHz) | Sort-Object -Unique) -join '/'), now $((@($cpu.currentMHz) | Sort-Object -Unique) -join '/'))"
        Add-Result 'cpu.freq' 'CPU maximum frequency' $st $v ">= $($M.cpu.minMHz) MHz" $M.cpu.note
    }
}

# Device Manager (the Maskrom Studio self-test's rule) ----------------------
$pnpBad = Fact 'pnpProblems' {
    $list = @()
    foreach ($d in (Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object {
                ($_.ConfigManagerErrorCode -ne 0 -and $_.ConfigManagerErrorCode -ne 22) -or $_.Status -eq 'Error' -or $_.Status -eq 'Degraded' })) {
        $list += [pscustomobject]@{ instanceId = $d.InstanceId; name = $d.FriendlyName; code = [int]$d.ConfigManagerErrorCode }
    }
    , $list
}
if ($null -eq $pnpBad) {
    Add-Result 'devmgr' 'Device Manager: no problem devices' 'FAIL' '' '0 outside the whitelist' 'Get-PnpDevice failed'
} else {
    $left = @(); $allowedSeen = @()
    foreach ($d in @($pnpBad)) {
        # cut after the hardware ID: the rest can hold serial numbers
        $hid = (([string]$d.instanceId -split '\\')[0..1]) -join '\'
        $ok = $false
        foreach ($a in @($M.devmgrAllow)) { if ($a -and $hid -like "$($a.id)*") { $ok = $true } }
        if ($ok) { $allowedSeen += $hid } else { $left += "$($d.name) [$hid] code $($d.code)" }
    }
    $exp = '0'
    if (@($M.devmgrAllow).Count) { $exp = '0 except ' + ((@($M.devmgrAllow) | ForEach-Object { $_.id }) -join ', ') }
    if ($left.Count) {
        Add-Result 'devmgr' 'Device Manager: no problem devices' 'FAIL' ($left -join '; ') $exp "$($left.Count) device(s)"
    } else {
        $note = ''
        if ($allowedSeen.Count) { $note = 'whitelisted: ' + (($allowedSeen | Sort-Object -Unique) -join ', ') }
        Add-Result 'devmgr' 'Device Manager: no problem devices' 'PASS' '0' $exp $note
    }
}

# GPU ----------------------------------------------------------------------
$gpu = Fact 'gpu' {
    $a = Acpi-Device 'RKCP7402'
    $hz = $null
    if ($a.present) {
        $key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($a.instanceId)\Device Parameters\maliwddm"
        $hz = (Get-ItemProperty -Path $key -Name GpuClkHz -ErrorAction SilentlyContinue).GpuClkHz
    }
    $a | Add-Member -NotePropertyName gpuClkHz -NotePropertyValue $hz -PassThru
}
$gc = Fact 'gpuCounters' {
    $r = [ordered]@{}
    foreach ($n in 'WDDM version', 'Clock (MHz)', 'Temperature (C)', 'Throttled', 'Throttled reason', 'WDDM fallback reason') {
        $v = Counter-Value "\RK3576 GPU(*)\$n"
        if ($null -eq $v) { $r['error'] = $script:CounterError } else { $r[$n] = $v }
    }
    [pscustomobject]$r
}
function GC-Value([string]$n) { if ($gc -and ($gc.PSObject.Properties.Name -contains $n)) { return $gc.$n } return $null }

if (-not $gpu -or -not $gpu.present) {
    Add-Result 'gpu.driver' 'GPU: maliwddm bound to the Mali-G52' 'FAIL' 'no ACPI\RKCP7402' 'maliwddm, OK'
} else {
    $st = 'PASS'; $why = ''
    if ($gpu.service -ne 'maliwddm') { $st = 'FAIL'; $why = "bound to '$($gpu.service)'" }
    elseif ($gpu.status -ne 'OK') { $st = 'FAIL'; $why = "status $($gpu.status), code $($gpu.problem)" }
    Add-Result 'gpu.driver' 'GPU: maliwddm bound to the Mali-G52' $st "$($gpu.service), $($gpu.status)" 'maliwddm, OK' $why
}
$wv = GC-Value 'WDDM version'
$gcErr = ''
if ($gc -and ($gc.PSObject.Properties.Name -contains 'error')) { $gcErr = $gc.error }
if ($null -eq $wv) {
    Add-Result 'gpu.wddm' 'GPU: WDDM version (counter)' 'FAIL' '' $M.gpu.wddmVersion "RK3576 GPU counters not readable: $gcErr (are the RK3576 tools installed?)"
} else {
    $st = 'PASS'; $why = ''
    if ([int]$wv -ne [int]$M.gpu.wddmVersion) { $st = 'FAIL'; $why = "fallback reason $(GC-Value 'WDDM fallback reason')" }
    Add-Result 'gpu.wddm' 'GPU: WDDM version (counter)' $st ([int]$wv) $M.gpu.wddmVersion $why
}
$clk = GC-Value 'Clock (MHz)'
$clkSrc = 'counter'
if ($null -eq $clk -and $gpu -and $gpu.gpuClkHz) { $clk = [math]::Round([double]$gpu.gpuClkHz / 1e6); $clkSrc = 'GpuClkHz' }
if ($null -eq $clk) {
    Add-Result 'gpu.clock' 'GPU: clock' 'FAIL' '' "$($M.gpu.clockMHz) MHz" 'neither the counter nor GpuClkHz is there'
} else {
    $thr = GC-Value 'Throttled'
    $st = 'PASS'; $why = $M.gpu.note
    if ([math]::Abs([double]$clk - [double]$M.gpu.clockMHz) -gt 2) {
        $st = 'FAIL'
        if ($thr) { $why = "throttled (reason $(GC-Value 'Throttled reason'))" }
    }
    Add-Result 'gpu.clock' 'GPU: clock' $st "$([math]::Round([double]$clk)) MHz ($clkSrc)" "$($M.gpu.clockMHz) MHz" $why
}
$gt = GC-Value 'Temperature (C)'
if ($null -eq $gt) {
    Add-Result 'gpu.temp' 'GPU: temperature counter' 'FAIL' '' "10..$($M.gpu.tempMaxC) C" 'counter not readable'
} else {
    $st = 'PASS'; if ($gt -le 10 -or $gt -ge $M.gpu.tempMaxC) { $st = 'FAIL' }
    Add-Result 'gpu.temp' 'GPU: temperature counter' $st "$([math]::Round($gt,1)) C" "10..$($M.gpu.tempMaxC) C"
}

# Display ------------------------------------------------------------------
$disp = Fact 'display' {
    $r = Invoke-UserSession -Name 'display' -Script $script:DisplayProbe -TimeoutSec 60
    if (-not $r) {
        $vc = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Select-Object -First 1
        $r = [pscustomobject]@{ error = $script:SessionError; current = $(if ($vc) { '{0}x{1}@{2}' -f $vc.CurrentHorizontalResolution, $vc.CurrentVerticalResolution, $vc.CurrentRefreshRate } else { '' }); modes = @() }
    }
    $r
}
function Parse-Mode([string]$s) {
    if ($s -match '^(\d+)x(\d+)@(\d+)') { return [pscustomobject]@{ w = [int]$Matches[1]; h = [int]$Matches[2]; hz = [int]$Matches[3]; text = $s } }
    return $null
}
$want = "largest mode at $($M.display.refreshHz) Hz"
if (-not $disp -or (-not @($disp.modes).Count)) {
    $why = 'no mode list'
    if ($disp -and $disp.error) { $why = "no mode list: $($disp.error)" }
    $cur = ''; if ($disp) { $cur = $disp.current }
    Add-Result 'display.mode' 'Display: largest mode at 60 Hz' 'SKIP' $cur $want $why
} else {
    $cur = Parse-Mode $disp.current
    $hzWant = [int]$M.display.refreshHz
    $hzOk = { param($mode) $mode.hz -ge ($hzWant - 1) -and $mode.hz -le $hzWant }
    $c60 = @(@($disp.modes) | ForEach-Object { Parse-Mode $_ } | Where-Object { $_ -and (& $hzOk $_) })
    if (-not $c60.Count) {
        Add-Result 'display.mode' 'Display: largest mode at 60 Hz' 'FAIL' $disp.current $want 'no 60 Hz mode offered'
    } else {
        $best = $c60 | Sort-Object { $_.w * $_.h } -Descending | Select-Object -First 1
        $st = 'PASS'; $why = ''
        if (-not $cur) { $st = 'FAIL'; $why = 'no current mode' }
        elseif ($cur.w -ne $best.w -or $cur.h -ne $best.h) { $st = 'FAIL'; $why = "the largest at $($M.display.refreshHz) Hz is $($best.w)x$($best.h)" }
        elseif (-not (& $hzOk $cur)) { $st = 'FAIL'; $why = "refresh $($cur.hz) Hz" }
        Add-Result 'display.mode' 'Display: largest mode at 60 Hz' $st $disp.current "$($best.w)x$($best.h)@$($M.display.refreshHz)" $why
    }
}

# Audio --------------------------------------------------------------------
$audio = Fact 'audio' {
    $l = @()
    foreach ($e in (Get-PnpDevice -PresentOnly -Class AudioEndpoint -ErrorAction SilentlyContinue)) {
        $l += [pscustomobject]@{ name = $e.FriendlyName; status = [string]$e.Status }
    }
    , $l
}
function Audio-Check([string]$Id, [string]$Name, $CapValue, [string]$CapName, [string]$Pattern, [string]$Hint) {
    $c = Cap $CapValue $CapName
    if ($c -eq 'no') { Add-Result $Id $Name 'N-A' '' '' 'not on this board'; return }
    $hit = @(@($audio) | Where-Object { $_ -and $_.name -match $Pattern })
    $okHit = @($hit | Where-Object { $_.status -eq 'OK' })
    if ($okHit.Count) { Add-Result $Id $Name 'PASS' (($okHit | ForEach-Object { $_.name }) -join '; ') "an endpoint matching '$Pattern'"; return }
    $v = (($hit | ForEach-Object { "$($_.name) ($($_.status))" }) -join '; ')
    if ($c -eq 'optional') { Add-Result $Id $Name 'SKIP' $v "an endpoint matching '$Pattern'" "optional here: $Hint"; return }
    Add-Result $Id $Name 'FAIL' $v "an endpoint matching '$Pattern'" $Hint
}
Audio-Check 'audio.hdmi' 'Audio: HDMI endpoint' $M.audio.hdmi 'hdmiaudio' 'HDMI' 'needs a monitor with speakers on HDMI'
Audio-Check 'audio.analog' 'Audio: analog (headphone) endpoint' $M.audio.analog 'analog' 'Headphone' 'ES8388 endpoint'

# Network ------------------------------------------------------------------
$net = Fact 'network' {
    $l = @()
    foreach ($a in (Get-NetAdapter -Physical -ErrorAction SilentlyContinue)) {
        $ip = @(Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notlike '169.254.*' } | ForEach-Object { $_.IPAddress })
        $gw = @(Get-NetRoute -InterfaceIndex $a.ifIndex -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | ForEach-Object { $_.NextHop })
        $l += [pscustomobject]@{
            name = $a.Name; description = $a.InterfaceDescription; pnp = $a.PnPDeviceID; status = [string]$a.Status
            media = [string]$a.PhysicalMediaType; speed = [string]$a.LinkSpeed; ipv4 = $ip; gateway = $gw
        }
    }
    , $l
}
$eth = @(@($net) | Where-Object { $_ -and $_.pnp -like 'ACPI\RKCP6543*' })
if ((Cap $M.network.ethernet) -eq 'no') {
    Add-Result 'net.ethernet' 'Network: Ethernet up with an IPv4 address' 'N-A' '' '' 'no Ethernet'
} elseif (-not $eth.Count) {
    Add-Result 'net.ethernet' 'Network: Ethernet up with an IPv4 address' 'FAIL' 'no adapter on ACPI\RKCP6543' 'dwc_eqos adapter Up'
} else {
    $e = $eth[0]
    $st = 'PASS'; $why = ''
    if ($e.status -ne 'Up') { $st = 'FAIL'; $why = "status $($e.status) (cable?)" }
    elseif (-not @($e.ipv4).Count) { $st = 'FAIL'; $why = 'no IPv4 address (DHCP?)' }
    Add-Result 'net.ethernet' 'Network: Ethernet up with an IPv4 address' $st "$($e.status) $($e.speed) $(@($e.ipv4) -join ',')" 'Up, IPv4' $why
    $gw = @($e.gateway | Where-Object { $_ -and $_ -ne '0.0.0.0' }) | Select-Object -First 1
    if ($st -ne 'PASS' -or -not $gw) {
        Add-Result 'net.gateway' 'Network: ping the gateway' 'SKIP' '' 'replies' 'no gateway on the Ethernet adapter'
    } else {
        $ping = Fact 'ping' {
            $r = @(Test-Connection -ComputerName $gw -Count 3 -ErrorAction SilentlyContinue)
            [pscustomobject]@{ replies = $r.Count; ms = $(if ($r.Count) { [math]::Round(($r | Measure-Object -Property ResponseTime -Average).Average, 1) } else { $null }) }
        }
        $st = 'FAIL'; if ($ping -and $ping.replies -gt 0) { $st = 'PASS' }
        Add-Result 'net.gateway' 'Network: ping the gateway' $st "$gw : $($ping.replies)/3 replies, $($ping.ms) ms" '>= 1 of 3 replies'
    }
}
$cw = Cap $M.network.wifi 'wifi'
if ($cw -eq 'no') {
    Add-Result 'net.wifi' 'Network: Wi-Fi adapter' 'N-A' '' '' 'no Windows Wi-Fi driver for this board'
} else {
    $w = @(@($net) | Where-Object { $_ -and $_.media -match '802\.11' })
    if ($w.Count) { Add-Result 'net.wifi' 'Network: Wi-Fi adapter' 'PASS' "$($w[0].description) $($w[0].status)" 'an 802.11 adapter' }
    elseif ($cw -eq 'optional') { Add-Result 'net.wifi' 'Network: Wi-Fi adapter' 'SKIP' '' 'an 802.11 adapter' 'no Wi-Fi driver installed (bcmwlsdio is on a branch)' }
    else { Add-Result 'net.wifi' 'Network: Wi-Fi adapter' 'FAIL' '' 'an 802.11 adapter' '' }
}
$cb = Cap $M.network.bluetooth 'bluetooth'
if ($cb -eq 'no') {
    Add-Result 'net.bluetooth' 'Bluetooth radio' 'N-A' '' '' 'no Windows Bluetooth driver for this board'
} else {
    $bt = Fact 'bluetooth' {
        $a = Acpi-Device 'RKCP4345'
        $radios = @(Get-PnpDevice -PresentOnly -Class Bluetooth -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'OK' })
        $a | Add-Member -NotePropertyName radios -NotePropertyValue $radios.Count -PassThru
    }
    if ($bt -and $bt.present -and $bt.status -eq 'OK' -and $bt.radios -gt 0) {
        Add-Result 'net.bluetooth' 'Bluetooth radio' 'PASS' "$($bt.service), $($bt.radios) Bluetooth device(s) OK" 'RKCP4345 OK, a radio'
    } else {
        $v = ''; if ($bt) { $v = "present $($bt.present), status $($bt.status), radios $($bt.radios)" }
        $st = 'FAIL'; if ($cb -eq 'optional') { $st = 'SKIP' }
        Add-Result 'net.bluetooth' 'Bluetooth radio' $st $v 'RKCP4345 OK, a radio' ''
    }
}

# USB ----------------------------------------------------------------------
$usb = Fact 'usb' {
    $all = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue)
    $hc = @($all | Where-Object { $_.InstanceId -like 'ACPI\PNP0D10\*' })
    $dev = @($all | Where-Object { $_.InstanceId -like 'USB\VID_*' -and $_.InstanceId -notmatch '&MI_' })
    [pscustomobject]@{
        controllers   = $hc.Count
        controllersOk = @($hc | Where-Object { $_.Status -eq 'OK' }).Count
        devices       = @($dev | ForEach-Object { "$($_.FriendlyName) [$((($_.InstanceId -split '\\')[0..1]) -join '\')]" })
        hubs          = @($dev | Where-Object { $_.Class -eq 'USB' -and $_.FriendlyName -match 'Hub' }).Count
    }
}
if (-not $usb) {
    Add-Result 'usb.controllers' 'USB: xHCI controllers' 'FAIL' '' $M.usb.controllers 'could not read'
} else {
    $st = 'PASS'; if ($usb.controllersOk -lt [int]$M.usb.controllers) { $st = 'FAIL' }
    Add-Result 'usb.controllers' 'USB: xHCI controllers' $st "$($usb.controllersOk) OK of $($usb.controllers)" ">= $($M.usb.controllers) OK"
    $n = @($usb.devices).Count
    $st = 'PASS'; $why = ''
    if ((Cap $M.usb.hub) -eq 'yes' -and $usb.hubs -lt 1) { $st = 'FAIL'; $why = 'the onboard hub did not enumerate' }
    $v = "$n device(s)"
    if ($n) { $v += ': ' + (@($usb.devices) -join '; ') }
    $exp = 'enumerates'
    if ((Cap $M.usb.hub) -eq 'yes') { $exp = 'the onboard hub and what is behind it' }
    Add-Result 'usb.devices' 'USB: devices enumerated' $st $v $exp $why
}

# Storage ------------------------------------------------------------------
$disks = Fact 'disks' {
    $l = @()
    foreach ($d in (Get-CimInstance Win32_DiskDrive -ErrorAction SilentlyContinue)) {
        $chain = Pnp-Ancestors $d.PNPDeviceID
        $kind = 'other'
        if ($chain | Where-Object { $_ -like 'ACPI\RKCP0D40*' }) { $kind = 'emmc' }
        elseif ($chain | Where-Object { $_ -like 'ACPI\RKCPFE2C*' }) { $kind = 'sd' }
        elseif ($d.PNPDeviceID -like 'SCSI\*NVME*' -or ($chain | Where-Object { $_ -like 'PCI\*' })) { $kind = 'nvme' }
        elseif ($d.PNPDeviceID -like 'USBSTOR\*' -or ($chain | Where-Object { $_ -like 'USB\*' })) { $kind = 'usb' }
        $pd = Get-Disk -Number $d.Index -ErrorAction SilentlyContinue
        $l += [pscustomobject]@{
            kind = $kind; index = [int]$d.Index; model = $d.Model; sizeGB = [math]::Round([double]$d.Size / 1e9, 1)
            health = $(if ($pd) { [string]$pd.HealthStatus } else { '' }); status = $(if ($pd) { [string]$pd.OperationalStatus } else { [string]$d.Status })
            system = $(if ($pd) { [bool]$pd.IsSystem -or [bool]$pd.IsBoot } else { $false })
        }
    }
    , $l
}
foreach ($k in 'emmc', 'sd', 'nvme') {
    $c = Cap $M.storage.$k $k
    $label = @{ emmc = 'eMMC'; sd = 'SD card'; nvme = 'NVMe' }[$k]
    if ($c -eq 'no') { Add-Result "storage.$k" "Storage: $label" 'N-A' '' '' "no $label on this board"; continue }
    $d = @(@($disks) | Where-Object { $_ -and $_.kind -eq $k })
    if (-not $d.Count) {
        $st = 'FAIL'; $why = 'not found'
        if ($c -eq 'optional') { $st = 'SKIP'; $why = 'not fitted (optional here; -Require ' + $k + ' to insist)' }
        Add-Result "storage.$k" "Storage: $label" $st '' 'present, Healthy' $why
        continue
    }
    $bad = @($d | Where-Object { $_.health -and $_.health -ne 'Healthy' })
    $st = 'PASS'; if ($bad.Count) { $st = 'FAIL' }
    $v = (($d | ForEach-Object { "disk $($_.index) $($_.model) $($_.sizeGB) GB $($_.health)$(if ($_.system) { ' (system)' })" }) -join '; ')
    Add-Result "storage.$k" "Storage: $label" $st $v 'present, Healthy'
}
$mib = 64; if ($Quick) { $mib = 16 }
$rw = Fact 'storageRw' {
    $f = Join-Path $env:SystemDrive ("rk3576-regress-{0}.bin" -f $PID)
    $data = New-Object byte[] ($mib * 1MB)
    (New-Object System.Random).NextBytes($data)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $s = New-Object System.IO.FileStream($f, 'Create', 'Write', 'None', 1MB, [System.IO.FileOptions]::WriteThrough)
        $s.Write($data, 0, $data.Length); $s.Flush($true); $s.Close()
        $wms = $sw.ElapsedMilliseconds
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $want = [BitConverter]::ToString($sha.ComputeHash($data))
        $got = [BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes($f)))
        [pscustomobject]@{ ok = ($want -eq $got); mib = $mib; writeMs = $wms; error = '' }
    } catch {
        [pscustomobject]@{ ok = $false; mib = $mib; writeMs = 0; error = $_.Exception.Message }
    } finally { Remove-Item $f -Force -ErrorAction SilentlyContinue }
}
if (-not $rw) {
    Add-Result 'storage.rw' 'Storage: write, read back, compare (system disk)' 'FAIL' '' 'SHA-256 equal' 'did not run'
} else {
    $st = 'FAIL'; if ($rw.ok) { $st = 'PASS' }
    $v = "$($rw.mib) MiB"
    if ($rw.writeMs -gt 0) { $v += ", write-through $([math]::Round($rw.mib * 1000.0 / $rw.writeMs, 1)) MiB/s" }
    Add-Result 'storage.rw' 'Storage: write, read back, compare (system disk)' $st $v 'SHA-256 equal' $rw.error
}

# NPU ----------------------------------------------------------------------
$cn = Cap $M.npu 'npu'
if ($cn -eq 'no') {
    Add-Result 'npu.info' 'NPU: rknputest info' 'N-A' '' '' 'no NPU option on this board'
} else {
    $nd = Fact 'npuDevice' { Acpi-Device 'RKCP3576' }
    if (-not $nd -or -not $nd.present) {
        Add-Result 'npu.info' 'NPU: rknputest info' 'SKIP' '' '' 'the NPU setup option is off (no ACPI\RKCP3576)'
    } else {
        $exe = Find-Tool 'rknputest'
        if ($nd.service -ne 'rknpu' -or $nd.status -ne 'OK') {
            Add-Result 'npu.info' 'NPU: rknputest info' 'FAIL' "$($nd.service), $($nd.status), code $($nd.problem)" 'rknpu, OK' 'driver not bound'
        } elseif (-not $exe) {
            Add-Result 'npu.info' 'NPU: rknputest info' 'SKIP' 'rknpu OK' 'exit 0' 'rknputest.exe not found (RK3576 tools, -ToolsDir)'
        } else {
            $r = Run-Tool 'rknputest info' $exe @('info') 60
            $st = 'FAIL'; if ($r.ExitCode -eq 0) { $st = 'PASS' }
            Add-Result 'npu.info' 'NPU: rknputest info' $st "exit $($r.ExitCode)" 'exit 0' $(if ($r.TimedOut) { 'timed out' } else { '' })
            if ($st -eq 'PASS') {
                $r = Run-Tool 'rknputest m3' $exe @('m3', '--core', 'both', '--iters', '1') 120
                $st = 'FAIL'; if ($r.ExitCode -eq 0) { $st = 'PASS' }
                $why = @{ 1 = 'device error'; 3 = 'a result was wrong'; 4 = 'a job did not complete' }[[int]$r.ExitCode]
                Add-Result 'npu.m3' 'NPU: m3 int8 matmuls, both cores, 1 iteration' $st "exit $($r.ExitCode)" 'exit 0, bit-exact' $why
            }
        }
    }
}

# Video decoder ------------------------------------------------------------
$cv = Cap $M.vdec 'vdec'
if ($cv -eq 'no') {
    Add-Result 'vdec.warmup' 'Video decoder: warm-up decode' 'N-A' '' '' 'no video decoder'
} else {
    $vd = Fact 'vdecDevice' { Acpi-Device 'RKCP0383' }
    $exe = Find-Tool 'vdpu383test'
    if (-not $vd -or -not $vd.present) {
        Add-Result 'vdec.warmup' 'Video decoder: warm-up decode' 'SKIP' '' '' 'the video decoder setup option is off (no ACPI\RKCP0383)'
    } elseif ($vd.service -ne 'vdpu383' -or $vd.status -ne 'OK') {
        Add-Result 'vdec.warmup' 'Video decoder: warm-up decode' 'FAIL' "$($vd.service), $($vd.status), code $($vd.problem)" 'vdpu383, OK' 'driver not bound'
    } elseif (-not $exe) {
        Add-Result 'vdec.warmup' 'Video decoder: warm-up decode' 'SKIP' 'vdpu383 OK' 'exit 0' 'vdpu383test.exe not found (-ToolsDir or bin\)'
    } else {
        $r = Run-Tool 'vdpu383test warmup' $exe @('warmup') 60
        $st = 'FAIL'; if ($r.ExitCode -eq 0) { $st = 'PASS' }
        Add-Result 'vdec.warmup' 'Video decoder: warm-up decode' $st "exit $($r.ExitCode)" 'exit 0' $(if ($r.TimedOut) { 'timed out' } else { '' })
        $sdir = Join-Path $script:Here 'streams'
        $streamLines = @(Get-Content (Join-Path $script:Here 'streams.txt') | Where-Object { $_.Trim() -and -not $_.StartsWith('#') })
        if ($Quick) { Add-Result 'vdec.streams' 'Video decode: conformance streams' 'SKIP' '' "$($streamLines.Count) streams, md5 equal" '-Quick'; $streamLines = @() }
        foreach ($line in $streamLines) {
            if (-not $line.Trim() -or $line.StartsWith('#')) { continue }
            $f = $line -split '\s+'
            $codec = $f[0]; $name = $f[1]; $path = $f[2]; $sha = $f[3]; $frames = [int]$f[4]; $md5 = $f[5]
            $id = "vdec.$codec.$name"
            $title = "Video decode: $codec $name"
            $file = Join-Path $sdir (Split-Path $path -Leaf)
            if (-not $script:DryRunMode) {
                if (-not (Test-Path $file) -and $FetchStreams) {
                    New-Item -ItemType Directory -Force $sdir | Out-Null
                    try { Invoke-WebRequest -UseBasicParsing -Uri "https://fate-suite.ffmpeg.org/$path" -OutFile $file } catch { Remove-Item $file -ErrorAction SilentlyContinue }
                }
                if (-not (Test-Path $file)) { Add-Result $id $title 'SKIP' '' "$frames frames, md5 $md5" 'stream not here: run fetch-streams.sh on the host or pass -FetchStreams'; continue }
                if ((Get-FileHash -Algorithm SHA256 $file).Hash -ne $sha.ToUpper()) { Add-Result $id $title 'FAIL' '' "sha256 $sha" 'the stream file is not the listed one'; continue }
            }
            $r = Run-Tool "vdpu383test decode $name" $exe @('decode', $file) 120
            $t = $r.Output
            $gotMd5 = ''; if ($t -match 'output md5 ([0-9a-f]{32})') { $gotMd5 = $Matches[1] }
            $gotFrames = -1; if ($t -match 'frames out (\d+)') { $gotFrames = [int]$Matches[1] }
            $hwErr = -1; if ($t -match 'hardware errors (\d+)') { $hwErr = [int]$Matches[1] }
            $st = 'FAIL'; if ($gotMd5 -eq $md5 -and $gotFrames -eq $frames -and $hwErr -eq 0) { $st = 'PASS' }
            $why = ''; if ($r.TimedOut) { $why = 'timed out' } elseif ($st -eq 'FAIL') { $why = "exit $($r.ExitCode)" }
            Add-Result $id $title $st "$gotFrames frames, hw errors $hwErr, md5 $gotMd5" "$frames frames, md5 $md5" $why
        }
    }
}

# Thermal ------------------------------------------------------------------
$tz = Fact 'thermal' { , @(Temps-Now) }
$zones = @(@($tz) | Where-Object { $_ })
if (-not $zones.Count) {
    Add-Result 'thermal.zones' 'Thermal zones readable' 'FAIL' '0 zones' "$($M.thermal.zones) zones" 'no thermal zone counters'
} else {
    $hot = @($zones | Where-Object { $_.c -le 0 -or $_.c -ge $M.thermal.maxC })
    $st = 'PASS'; $why = ''
    if ($zones.Count -lt [int]$M.thermal.zones) { $st = 'FAIL'; $why = 'zones missing' }
    elseif ($hot.Count) { $st = 'FAIL'; $why = 'out of range' }
    $v = (($zones | ForEach-Object { "$($_.name) $($_.c) C" }) -join '; ')
    Add-Result 'thermal.zones' 'Thermal zones readable' $st $v ">= $($M.thermal.zones) zones, 0..$($M.thermal.maxC) C" $why
}

# Time ---------------------------------------------------------------------
$tm = Fact 'time' {
    $o = $null
    $s = & w32tm.exe /stripchart /computer:time.windows.com /samples:1 /dataonly 2>&1 | Out-String
    if ($s -match ',\s*([+-]?\d+(\.\d+)?)s') { $o = [double]$Matches[1] }
    $rtc = Acpi-Device 'HYMB0001'
    [pscustomobject]@{ now = (Get-Date).ToString('s'); year = (Get-Date).Year; offsetS = $o; rtcPresent = $rtc.present; rtcStatus = $rtc.status }
}
if (-not $tm) {
    Add-Result 'time.clock' 'Time: clock sane' 'FAIL' '' '' 'could not read'
} else {
    $st = 'PASS'; $why = ''
    if ($tm.year -lt 2026 -or $tm.year -gt 2040) { $st = 'FAIL'; $why = 'the year is wrong (RTC lost?)' }
    elseif ($null -ne $tm.offsetS -and [math]::Abs([double]$tm.offsetS) -gt 120) { $st = 'FAIL'; $why = "$($tm.offsetS) s from time.windows.com" }
    if ($st -eq 'PASS' -and $null -eq $tm.offsetS) { $why = 'no network time source to compare with' }
    if ($M.rtc.note) { if ($why) { $why += '; ' }; $why += $M.rtc.note }
    $v = "$($tm.now)"
    if ($null -ne $tm.offsetS) { $v += ", offset $($tm.offsetS) s" }
    Add-Result 'time.clock' 'Time: clock sane' $st $v 'year 2026..2040, within 120 s of NTP' $why
    $st = 'PASS'; if (-not $tm.rtcPresent -or $tm.rtcStatus -ne 'OK') { $st = 'FAIL' }
    Add-Result 'time.rtc' 'RTC device (HYMB0001)' $st "present $($tm.rtcPresent), $($tm.rtcStatus)" 'present, OK'
}

# Stress (opt-in) ----------------------------------------------------------
if (-not $Stress) {
    Add-Result 'stress.furmark' 'GPU stress: FurMark (GpuTest)' 'SKIP' '' '' 'opt-in: -Stress'
} else {
    $sr = Fact 'stress' {
        $r = [ordered]@{ ran = $false; aborted = $false; maxC = $null; score = ''; error = '' }
        if (-not (Test-Path -LiteralPath $GpuTest)) { $r.error = "GpuTest not found: $GpuTest"; return [pscustomobject]$r }
        $t0 = @(Temps-Now | ForEach-Object { $_.c })
        if ($t0.Count -and ($t0 | Measure-Object -Maximum).Maximum -ge 80) { $r.error = 'too hot to start (>= 80 C)'; return [pscustomobject]$r }
        $dir = Split-Path -Parent $GpuTest
        $log = Join-Path $dir '_geeks3d_gputest_log.txt'
        $logLen = 0; if (Test-Path -LiteralPath $log) { $logLen = (Get-Item -LiteralPath $log).Length }
        $ms = $StressSeconds * 1000
        $launch = @"
param([string]`$Out)
`$p = Start-Process -FilePath '$($GpuTest -replace "'", "''")' -WorkingDirectory '$($dir -replace "'", "''")' -ArgumentList '/test=fur /width=1024 /height=640 /benchmark /benchmark_duration_ms=$ms /no_scorebox' -PassThru -Wait
@{ exit = `$p.ExitCode } | ConvertTo-Json | Set-Content -Path `$Out -Encoding UTF8
"@
        $script:StressMax = 0.0
        $script:StressAborted = $false
        $watch = {
            Start-Sleep -Seconds 2
            $t = @(Temps-Now | ForEach-Object { $_.c })
            $g = Counter-Value '\RK3576 GPU(*)\Temperature (C)'
            if ($null -ne $g) { $t += $g }
            if ($t.Count) {
                $mx = ($t | Measure-Object -Maximum).Maximum
                if ($mx -gt $script:StressMax) { $script:StressMax = $mx }
                if ($mx -ge $script:ThermalAbortC) {
                    Get-Process -Name GpuTest -ErrorAction SilentlyContinue | Stop-Process -Force
                    $script:StressAborted = $true
                    return $true
                }
            }
            return $false
        }
        $res = Invoke-UserSession -Name 'gputest' -Script $launch -TimeoutSec ($StressSeconds + 120) -WhileRunning $watch
        $r.ran = $true
        $r.aborted = $script:StressAborted
        $r.maxC = $script:StressMax
        if (-not $res -and -not $script:StressAborted) { $r.error = $script:SessionError; $r.ran = $false }
        if (Test-Path -LiteralPath $log) {
            $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
            try { $fs.Seek($logLen, 'Begin') | Out-Null; $tail = (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
            $sc = [regex]::Matches($tail, 'Score:\s*\d+[^\r\n]*')
            if ($sc.Count) { $r.score = $sc[$sc.Count - 1].Value.Trim() }
        }
        [pscustomobject]$r
    }
    if (-not $sr -or -not $sr.ran) {
        $why = 'did not run'; if ($sr -and $sr.error) { $why = $sr.error }
        Add-Result 'stress.furmark' 'GPU stress: FurMark (GpuTest)' 'SKIP' '' "$StressSeconds s below $($script:ThermalAbortC) C" $why
    } else {
        $st = 'PASS'; $why = ''
        if ($sr.aborted) { $st = 'FAIL'; $why = "stopped: a temperature reached $($script:ThermalAbortC) C" }
        $v = "max $($sr.maxC) C"
        if ($sr.score) { $v += ", $($sr.score)" }
        Add-Result 'stress.furmark' 'GPU stress: FurMark (GpuTest)' $st $v "$StressSeconds s below $($script:ThermalAbortC) C" $why
        $after = Fact 'gpuAfterStress' { $script:PnpCache = $null; Acpi-Device 'RKCP7402' }
        $st = 'PASS'; if (-not $after -or $after.status -ne 'OK') { $st = 'FAIL' }
        Add-Result 'stress.gpu-after' 'GPU still OK after the stress run' $st $(if ($after) { $after.status } else { '' }) 'OK'
    }
}

# Display driver resets during the run (last, so the run itself is covered) --
$tdr = Fact 'tdrEvents' {
    , @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display'; Id = 4101; StartTime = $script:Start } -ErrorAction SilentlyContinue |
        ForEach-Object { $_.TimeCreated.ToString('s') })
}
$n = @(@($tdr) | Where-Object { $_ }).Count
$st = 'PASS'; if ($n) { $st = 'FAIL' }
Add-Result 'display.tdr' 'No Display 4101 (driver reset) events during the run' $st $n 0 $(if ($n) { (@($tdr) -join ', ') } else { '' })

# ------------------------------------------------------------------ report

$counts = [ordered]@{ PASS = 0; FAIL = 0; SKIP = 0; 'N-A' = 0 }
foreach ($r in $script:Results) { $counts[$r.status]++ }
$summary = [pscustomobject][ordered]@{
    suite      = $suiteVersion
    board      = $boardId
    boardName  = $M.name
    detectedBy = $detectedBy
    firmware   = $smbios.biosVersion
    windows    = $smbios.windows
    started    = $script:Start.ToString('s')
    finished   = (Get-Date).ToString('s')
    quick      = [bool]$Quick
    stress     = [bool]$Stress
    dryRun     = [bool]$DryRun
    counts     = [pscustomobject]$counts
    results    = $script:Results.ToArray()
}
$json = $summary | ConvertTo-Json -Depth 6
$enc = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $script:OutDir 'results.json'), $json, $enc)

function Format-Cell([string]$s) { return ($s -replace '\|', '\|' -replace "`r?`n", ' ') }
$md = New-Object System.Collections.Generic.List[string]
$md.Add("# RK3576 regression: $($M.name)")
$md.Add('')
$md.Add("| | |")
$md.Add("|---|---|")
$md.Add("| Board | $boardId (detected by $detectedBy) |")
$md.Add("| Firmware | $(Format-Cell $smbios.biosVersion) |")
$md.Add("| Windows | $(Format-Cell $smbios.windows) |")
$md.Add("| Suite | $suiteVersion$(if ($Quick) { ', -Quick' })$(if ($Stress) { ', -Stress' })$(if ($DryRun) { ', DRY RUN' }) |")
$md.Add("| Run | $($script:Start.ToString('s')) to $((Get-Date).ToString('s')) |")
$md.Add("| Result | PASS $($counts.PASS), FAIL $($counts.FAIL), SKIP $($counts.SKIP), N-A $($counts['N-A']) |")
$md.Add('')
$md.Add('| id | check | result | measured | expected | reason |')
$md.Add('|---|---|---|---|---|---|')
foreach ($r in $script:Results) {
    $md.Add("| $($r.id) | $(Format-Cell $r.name) | **$($r.status)** | $(Format-Cell $r.value) | $(Format-Cell $r.expected) | $(Format-Cell $r.reason) |")
}
[System.IO.File]::WriteAllLines((Join-Path $script:OutDir 'results.md'), $md, $enc)

Write-Host ''
Write-Host ("PASS {0}  FAIL {1}  SKIP {2}  N-A {3}   -> {4}" -f $counts.PASS, $counts.FAIL, $counts.SKIP, $counts['N-A'], $script:OutDir)
if ($counts.FAIL) { exit 1 }
exit 0
