# usb-debug.ps1 - why does XHC0 (the USB-C DWC3 at 0x23000000) fail with code 10?
# Run from an ADMIN prompt via usb-debug.cmd, with a USB device in the USB-C port.
# Everything goes to <stick>:\woa-debug\usb-out\<timestamp>\ on the stick.

$ErrorActionPreference = 'Continue'
$out = Join-Path $PSScriptRoot ("usb-out\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $out | Out-Null
Start-Transcript -Path (Join-Path $out 'transcript.txt') | Out-Null
function Step($t) { Write-Host ""; Write-Host "==== $t ====" -ForegroundColor Cyan }

function Dump($tag) {
    foreach ($d in Get-PnpDevice | Where-Object { $_.InstanceId -like 'ACPI\PNP0D10\*' }) {
        $p = Get-PnpDeviceProperty -InstanceId $d.InstanceId
        $code = ($p | Where-Object KeyName -eq 'DEVPKEY_Device_ProblemCode').Data
        $st = ($p | Where-Object KeyName -eq 'DEVPKEY_Device_ProblemStatus').Data
        Write-Host ("{0,-24} {1,-6} problem={2} status=0x{3:X8}" -f $d.InstanceId, $d.Status, $code, $st)
        $p | Format-Table -AutoSize KeyName,Data | Out-File (Join-Path $out ("props-$tag-" + ($d.InstanceId -replace '[\\&]','_') + '.txt')) -Width 400
    }
}

Step 'xHCI controllers, before'
Dump 'before'
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\*' } | Format-Table -AutoSize Status,Class,FriendlyName,InstanceId | Out-File (Join-Path $out 'usb-devices-before.txt') -Width 400

Step 'start tracing (USBXHCI, UCX, USBHUB3, PnP)'
$providers = @(
    '{30E1D284-5D88-459C-83FD-6345B39B19EC}',  # Microsoft-Windows-USB-USBXHCI
    '{36DA592D-E43A-4E28-AF6F-4BC57C5A11E8}',  # Microsoft-Windows-USB-UCX
    '{AC52AD17-CC01-4F85-8DF5-4DCE4333C99B}',  # Microsoft-Windows-USB-USBHUB3
    '{9C205A39-1250-487D-ABD7-E831C6290539}'   # Microsoft-Windows-Kernel-PnP
)
$pf = Join-Path $out 'providers.txt'
$providers | ForEach-Object { "$_ 0xFFFFFFFFFFFFFFFF 5" } | Set-Content -Encoding Ascii $pf
logman stop usbdbg -ets 2>$null | Out-Null
logman start usbdbg -pf $pf -ets -o (Join-Path $out 'usb.etl') -bs 256 -nb 64 256

Step 'restart XHC0 (traced)'
$x = Get-PnpDevice | Where-Object { $_.InstanceId -like 'ACPI\PNP0D10\0' }
if ($x) {
    Disable-PnpDevice -InstanceId $x.InstanceId -Confirm:$false
    Start-Sleep 3
    Enable-PnpDevice -InstanceId $x.InstanceId -Confirm:$false
    Start-Sleep 15
} else { Write-Host 'ACPI\PNP0D10\0 not found' }

Step 'stop tracing'
logman stop usbdbg -ets
tracerpt (Join-Path $out 'usb.etl') -o (Join-Path $out 'usb.xml') -of XML -y | Out-Null
tracerpt (Join-Path $out 'usb.etl') -o (Join-Path $out 'usb.csv') -of CSV -y | Out-Null

Step 'xHCI controllers, after'
Dump 'after'
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\*' } | Format-Table -AutoSize Status,Class,FriendlyName,InstanceId | Out-File (Join-Path $out 'usb-devices-after.txt') -Width 400

Step 'event logs'
wevtutil qe System /c:200 /rd:true /f:text | Out-File (Join-Path $out 'system-events.txt')
foreach ($l in 'Microsoft-Windows-USB-USBXHCI-Operational','Microsoft-Windows-Kernel-PnP/Configuration') {
    Get-WinEvent -LogName $l -MaxEvents 100 -ErrorAction SilentlyContinue | Format-List TimeCreated,Id,LevelDisplayName,Message | Out-File (Join-Path $out (($l -replace '[/\\]','_') + '.txt'))
}

Step 'done'
Write-Host "Results: $out"
Stop-Transcript | Out-Null
