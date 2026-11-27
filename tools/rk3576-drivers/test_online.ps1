# test_online.ps1 - the follow-up check of install.cmd /online against
# made-up devices, on any Windows (CI: a windows-2022 runner). Changes
# nothing on the machine: install-online.ps1 runs with -Plan -FakeDevices,
# verify-bound.ps1 with -Simulate.
#
#   test_online.ps1 [-Package <rk3576-drivers folder>] [-Live]
#
# Without -Package it makes a small package from the scripts next to it,
# with one made-up INF per driver it needs. With it (the real package) the
# same checks run against the real INFs.
#
# 1. Plan with five fake devices: a GPU on an older maliwddm and an SAI bus
#    with no driver are marked; the SD host and the Ethernet adapter on old
#    drivers and a codec on a newer one are not. expected.json names the
#    two marked ones with the DriverVer of their package INF.
# 2. The 2026-11-27 case: after the restart the GPU is still on the old
#    package. verify-bound.ps1 re-marks the GPU and nothing else (not the
#    SD host or the Ethernet adapter, even when expected.json names them),
#    and with a user signed in tells them instead of restarting.
# 3. The same at the next boot: the retry is spent, it gives up and
#    removes its task.
# 4. Everything bound (one device on a newer package than expected): removes
#    its task, marks nothing.
# 5. Nobody signed in: restarts by itself.
# 6. -Live only, for a throwaway machine (CI): CHANGES IT. Sets up the
#    follow-up check with install-online.ps1's own functions in
#    %ProgramData%\RK3576\drivers for one present device made to look stale
#    and one absent device, checks the folder's ACL and the task, runs the
#    task as SYSTEM twice: the first run sets the device's reinstall bit and
#    leaves a sign-in notice (the runner's account counts as signed in),
#    the second gives up and removes both tasks.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param([string]$Package, [switch]$Live)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('rk3576-online-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$fails = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:fails++ }
}
function Write-Json($o, [string]$path) { ConvertTo-Json -InputObject $o -Depth 6 | Out-File -FilePath $path -Encoding utf8 }

$GPU = 'ACPI\RKCP7402\0'
$SAI = 'ACPI\RKCP3004\0'
$SD = 'ACPI\RKCPFE2C\0'
$ETH = 'ACPI\RKCP6543\0'
$CODEC = 'ACPI\RKCP8388\0'

if ($Package) {
    $pkg = (Resolve-Path $Package).Path
    $board = 'tspi3m'
} else {
    $pkg = Join-Path $tmp 'pkg'
    $board = 'test'
    New-Item -ItemType Directory -Path (Join-Path $pkg 'boards') | Out-Null
    foreach ($f in 'install-online.ps1', 'mark-reinstall.ps1', 'verify-bound.ps1') { Copy-Item (Join-Path $here $f) $pkg }
    'ci-test' | Out-File (Join-Path $pkg 'VERSION.txt') -Encoding ascii
    $infs = [ordered]@{
        'maliwddm'   = @('ACPI\RKCP7402', '11/27/2026,7.11.32.717')
        'dwcmshc'    = @('ACPI\RKCPFE2C', '11/27/2026,1.0.40.0')
        'dwc_eqos'   = @('ACPI\RKCP6543', '11/27/2026,1.0.40.0')
        'rk3xi2sbus' = @('ACPI\RKCP3004', '11/27/2026,1.0.40.0')
        'es8323'     = @('ACPI\RKCP8388', '11/27/2026,1.0.40.0')
    }
    foreach ($d in $infs.Keys) {
        $dir = Join-Path $pkg "drivers\$d"
        New-Item -ItemType Directory -Path $dir | Out-Null
        @('[Version]', "DriverVer = $($infs[$d][1]) ; stamped", '[Manufacturer]', '%M% = Models, NTarm64',
          '[Models.NTarm64]', "%D% = Inst, $($infs[$d][0])") | Out-File (Join-Path $dir "$d.inf") -Encoding ascii
    }
    @($infs.Keys) | Out-File (Join-Path $pkg "boards\$board.txt") -Encoding ascii
}

# 1. The plan.
$old = @{ Inf = 'oem71.inf'; Date = '2020-01-01'; Version = '1.0.0.0' }
$fake = @(
    @{ Id = $GPU; Name = 'GPU'; Ids = @('ACPI\RKCP7402', '*RKCP7402'); Driver = $old },
    @{ Id = $SAI; Name = 'SAI'; Ids = @('ACPI\RKCP3004'); Driver = $null },
    @{ Id = $SD; Name = 'SD'; Ids = @('ACPI\RKCPFE2C'); Driver = $old },
    @{ Id = $ETH; Name = 'Ethernet'; Ids = @('ACPI\RKCP6543'); Driver = $old },
    @{ Id = $CODEC; Name = 'Codec'; Ids = @('ACPI\RKCP8388'); Driver = @{ Inf = 'oem9.inf'; Date = '2099-01-01'; Version = '9.0.0.0' } }
)
Write-Json $fake (Join-Path $tmp 'fake.json')
$expPath = Join-Path $tmp 'expected.json'
$o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'install-online.ps1') -Board $board -Plan `
    -FakeDevices (Join-Path $tmp 'fake.json') -ExpectedOut $expPath
$rc = $LASTEXITCODE
$o | ForEach-Object { Write-Host "  | $_" }
Check ($rc -eq 0) "plan exits 0 (got $rc)"
Check ([bool]($o | Where-Object { $_ -like '2 device(s) would be marked*' })) 'plan marks 2 devices'
Check ([bool]($o | Where-Object { $_ -like 'Follow-up check (would be set up): *rk3576-drivers-verify*' })) 'plan prints the follow-up line'
$exp = Get-Content -LiteralPath $expPath -Raw | ConvertFrom-Json
$ids = @($exp.devices | ForEach-Object { $_.id } | Sort-Object)
$want = (@($SAI, $GPU) | Sort-Object) -join ' '
Check (($ids -join ' ') -eq $want) "expected.json names the GPU and SAI only (got $($ids -join ' '))"
$g = $exp.devices | Where-Object { $_.id -eq $GPU }
$line = $o | Where-Object { $_ -like 'maliwddm: would stage *' } | Select-Object -First 1
Check ($g.dir -eq 'maliwddm' -and $line -like "*$($g.inf) (*") "the GPU expects maliwddm's INF ($($g.inf))"
$dv = ([DateTime]$g.date).ToString('MM/dd/yyyy') + ',' + $g.version
Check ($line -like "*($dv;*") "the GPU expects its package DriverVer ($dv; $line)"

# 2 to 5. The boot check, simulated.
$stDir = Join-Path $tmp 'state'
function Reset-State([int]$retries) {
    if (Test-Path $stDir) { Remove-Item -Recurse -Force $stDir }
    New-Item -ItemType Directory -Path $stDir | Out-Null
    Copy-Item (Join-Path $pkg 'verify-bound.ps1') $stDir
    Copy-Item (Join-Path $pkg 'mark-reinstall.ps1') $stDir
    # expected.json naming the SD host and the Ethernet adapter as well: the
    # boot check must not mark them whatever it says.
    $e = Get-Content -LiteralPath $expPath -Raw | ConvertFrom-Json
    $e.devices = @($e.devices) + @(
        [pscustomobject]@{ id = $SD; name = 'SD'; dir = 'dwcmshc'; inf = 'dwcmshc.inf'; date = '2026-11-27'; version = '1.0.40.0' },
        [pscustomobject]@{ id = $ETH; name = 'Ethernet'; dir = 'dwc_eqos'; inf = 'dwc_eqos.inf'; date = '2026-11-27'; version = '1.0.40.0' })
    Write-Json $e (Join-Path $stDir 'expected.json')
    "{ `"retries`": $retries, `"max`": 1 }" | Out-File (Join-Path $stDir 'state.json') -Encoding ascii
}
function Invoke-Verify($gpuBound, [string[]]$owners) {
    $s = $exp.devices | Where-Object { $_.id -eq $SAI }
    $devs = [ordered]@{}
    $devs[$GPU] = $gpuBound
    $devs[$SAI] = @{ inf = 'oem90.inf'; date = $s.date; version = $s.version; problem = 0 }
    $devs[$SD] = @{ inf = 'oem3.inf'; date = '2020-01-01'; version = '1.0.0.0'; problem = 0 }
    $devs[$ETH] = @{ inf = 'oem4.inf'; date = '2020-01-01'; version = '1.0.0.0'; problem = 0 }
    Write-Json @{ devices = $devs; owners = $owners } (Join-Path $tmp 'sim.json')
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $stDir 'verify-bound.ps1') -Simulate (Join-Path $tmp 'sim.json')
    $script:vrc = $LASTEXITCODE
    $r | ForEach-Object { Write-Host "  | $_" }
    $r
}
function Has($out, [string]$pat) { [bool]($out | Where-Object { $_ -like $pat }) }
$stale = @{ inf = 'oem71.inf'; date = '2026-11-27'; version = '5.59.40.53'; problem = 0 }
$user = @('S-1-5-18', 'S-1-5-90-0-1', 'S-1-5-21-111-222-333-1001')
$nobody = @('S-1-5-18', 'S-1-5-19', 'S-1-5-20', 'S-1-5-90-0-1', 'S-1-5-96-0-0')

Write-Host '--- 2. the GPU kept its old package, a user is signed in'
Reset-State 0
$r = Invoke-Verify $stale $user
Check ($vrc -eq 0) "exits 0 (got $vrc)"
Check (Has $r "WOULD mark for reinstall: $GPU") 're-marks the GPU'
$rt = @(Get-Content (Join-Path $stDir 'reinstall.txt') | Where-Object { $_.Trim() })
Check (($rt -join ' ') -eq $GPU) "reinstall.txt is the GPU only (got $($rt -join ' '))"
Check (Has $r "$SD (dwcmshc):*not re-marked") 'the SD host is not re-marked'
Check (Has $r "$ETH (dwc_eqos):*not re-marked") 'the Ethernet adapter is not re-marked'
Check (Has $r "$SAI (rk3xi2sbus): done*") 'the SAI bus is done'
Check (Has $r 'WOULD register task rk3576-drivers-restart-notice*') 'tells the user at sign-in'
Check (Has $r 'WOULD tell the signed-in users now') 'tells the signed-in user now'
Check (-not (Has $r 'WOULD restart*')) 'does not restart'
Check (-not (Has $r 'WOULD delete task rk3576-drivers-verify')) 'keeps its task'
Check ([int](Get-Content (Join-Path $stDir 'state.json') -Raw | ConvertFrom-Json).retries -eq 1) 'counts the retry'

Write-Host '--- 3. the next boot, still not bound'
$r = Invoke-Verify $stale $user
Check (Has $r 'giving up:*') 'gives up'
Check (-not (Has $r 'WOULD mark*')) 'marks nothing'
Check (Has $r 'WOULD delete task rk3576-drivers-verify') 'removes its task'
Check (Has $r 'WOULD delete task rk3576-drivers-restart-notice') 'removes the sign-in notice'

Write-Host '--- 4. everything bound, the GPU on a newer package than expected'
Reset-State 0
$r = Invoke-Verify @{ inf = 'oem99.inf'; date = '2026-11-28'; version = '7.0.0.1'; problem = 0 } $user
Check (Has $r 'every device took its new driver') 'all done'
Check (-not (Has $r 'WOULD mark*')) 'marks nothing'
Check (Has $r 'WOULD delete task rk3576-drivers-verify') 'removes its task'

Write-Host '--- 5. the GPU kept its old package, nobody signed in'
Reset-State 0
$r = Invoke-Verify $stale $nobody
Check (Has $r "WOULD mark for reinstall: $GPU") 're-marks the GPU'
Check (Has $r 'WOULD restart in 60 seconds') 'restarts by itself'
Check (-not (Has $r 'WOULD register task rk3576-drivers-restart-notice*')) 'no sign-in notice'

function Invoke-Live {
    $here = $pkg
    $src = Join-Path $pkg 'install-online.ps1'
    $tok = $null; $err = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($src, [ref]$tok, [ref]$err)
    $want = 'Test-Newer', 'Get-Expected', 'Set-AdminOnly', 'Register-Followup'
    foreach ($f in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
        if ($want -contains $f.Name) { Invoke-Expression $f.Extent.Text }
    }
    $state = Join-Path $env:ProgramData 'RK3576\drivers'
    $VerifyTask = 'rk3576-drivers-verify'
    $pkgVer = 'live-test'
    $Board = 'test'
    $dev = Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'ROOT\*' -and (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
    } | Select-Object -First 1
    Check ([bool]$dev) "a present device with a driver ($($dev.InstanceId))"
    $absent = 'ACPI\RKCP9999\0'
    $mark = New-Object System.Collections.ArrayList
    [void]$mark.Add($dev.InstanceId); [void]$mark.Add($absent)
    $expect = @{}
    $expect[$dev.InstanceId] = [pscustomobject]@{ Id = $dev.InstanceId; Name = 'stale'; Dir = 'livetest'; Inf = 'livetest.inf'; Date = (Get-Date '2099-01-01'); Version = [version]'1.0.0.0' }
    $expect[$absent] = [pscustomobject]@{ Id = $absent; Name = 'absent'; Dir = 'livetest'; Inf = 'livetest.inf'; Date = (Get-Date '2099-01-01'); Version = [version]'1.0.0.0' }
    Register-Followup

    $acl = Get-Acl -LiteralPath $state
    $sids = @($acl.Access | ForEach-Object { $_.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]).Value } | Sort-Object -Unique)
    Check ($acl.AreAccessRulesProtected) 'the folder does not inherit'
    Check (($sids -join ' ') -eq 'S-1-5-18 S-1-5-32-544 S-1-5-32-545') "the folder grants SYSTEM, Administrators, Users only (got $($sids -join ' '))"
    $users = @($acl.Access | Where-Object { $_.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]).Value -eq 'S-1-5-32-545' })
    Check (-not ($users | Where-Object { $_.FileSystemRights -band [System.Security.AccessControl.FileSystemRights]'WriteData, AppendData, Delete' })) 'Users may not write'
    Check ($acl.GetOwner([System.Security.Principal.SecurityIdentifier]).Value -eq 'S-1-5-32-544') 'Administrators own it'
    foreach ($f in 'verify-bound.ps1', 'mark-reinstall.ps1', 'expected.json', 'state.json') { Check (Test-Path (Join-Path $state $f)) "$f is in it" }
    $t = Get-ScheduledTask -TaskName $VerifyTask
    Check ($t.Principal.UserId -match 'SYSTEM') "the task runs as SYSTEM ($($t.Principal.UserId))"
    Check ($t.Triggers[0].CimClass.CimClassName -eq 'MSFT_TaskBootTrigger' -and $t.Triggers[0].Delay -eq 'PT2M') "it starts 2 minutes after boot ($($t.Triggers[0].CimClass.CimClassName) $($t.Triggers[0].Delay))"

    function Start-Verify {
        $l = Join-Path $state 'verify-bound.log'
        $before = if (Test-Path $l) { @(Get-Content $l).Count } else { 0 }
        Start-ScheduledTask -TaskName $VerifyTask
        for ($n = 0; $n -lt 300; $n++) {
            Start-Sleep -Seconds 2
            if ((Test-Path $l) -and @(Get-Content $l).Count -gt $before -and (Get-ScheduledTask -TaskName $VerifyTask -ErrorAction SilentlyContinue).State -ne 'Running') { break }
        }
        $r = @(Get-Content $l | Select-Object -Skip $before)
        $r | ForEach-Object { Write-Host "  | $_" }
        $r
    }
    Write-Host '--- 6a. live, first boot check'
    $r = Start-Verify
    Check (Has $r "$($dev.InstanceId) (livetest):*NOT BOUND") 'the stale device is not bound'
    Check (Has $r "$absent (livetest): absent*") 'the absent device is skipped'
    Check (Has $r "  $($dev.InstanceId)  ConfigFlags*") 'mark-reinstall.ps1 ran as SYSTEM'
    $cf = [int](Get-ItemProperty -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)" -Name ConfigFlags).ConfigFlags
    Check ([bool]($cf -band 0x20)) ('the reinstall bit is set (0x{0:x8})' -f $cf)
    Check (Has $r 'a user is signed in*') 'the runner account counts as signed in'
    Check (-not (Has $r '*failed:*')) 'no step failed'
    $nt = Get-ScheduledTask -TaskName 'rk3576-drivers-restart-notice' -ErrorAction SilentlyContinue
    Check ($nt -and $nt.Principal.GroupId -match 'S-1-5-32-545|Users') "the sign-in notice is for Users ($($nt.Principal.GroupId))"
    Check ([bool](Get-ScheduledTask -TaskName $VerifyTask -ErrorAction SilentlyContinue)) 'the task stays for the next boot'
    $ev = Get-EventLog -LogName Application -Source 'RK3576-drivers' -Newest 1 -ErrorAction SilentlyContinue
    Check ($ev -and $ev.Message -like '*marked again*') 'the Application log has it'
    Write-Host '--- 6b. live, second boot check'
    $r = Start-Verify
    Check (Has $r 'giving up:*') 'gives up'
    Check (-not (Get-ScheduledTask -TaskName $VerifyTask -ErrorAction SilentlyContinue)) 'the task is gone'
    Check (-not (Get-ScheduledTask -TaskName 'rk3576-drivers-restart-notice' -ErrorAction SilentlyContinue)) 'the sign-in notice is gone'
}
if ($Live) { Invoke-Live }

Remove-Item -Recurse -Force $tmp
if ($fails) { Write-Host "$fails check(s) failed."; exit 1 }
Write-Host 'all checks passed'
exit 0
