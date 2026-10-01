# net-debug.ps1 - one-shot Ethernet diagnostics for the installed Windows on CM5-IO.
# Run from an ADMIN prompt:  <stick>:\woa-debug\net-debug.cmd
# Everything goes to <stick>:\woa-debug\net-out\<timestamp>\ on the stick.

$ErrorActionPreference = 'Continue'
$stick = Split-Path -Qualifier $PSScriptRoot
$out = Join-Path $PSScriptRoot ("net-out\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $out | Out-Null
Start-Transcript -Path (Join-Path $out 'transcript.txt') | Out-Null

function Step($t) { Write-Host ""; Write-Host "==== $t ====" -ForegroundColor Cyan }
function Stats($tag) {
    Get-NetAdapterStatistics -Name $name | Format-List * | Out-File (Join-Path $out "stats-$tag.txt")
    Get-NetAdapterStatistics -Name $name | Format-List ReceivedBytes,ReceivedUnicastPackets,ReceivedBroadcastPackets,ReceivedMulticastPackets,ReceivedDiscardedPackets,ReceivedPacketErrors,SentBytes,SentUnicastPackets,SentBroadcastPackets,OutboundDiscardedPackets,OutboundPacketErrors
}

Step 'adapter'
$a = Get-NetAdapter -Physical | Where-Object { $_.InterfaceDescription -notmatch 'Wi-?Fi|Wireless|Bluetooth' } | Select-Object -First 1
if (-not $a) { Write-Host 'No physical Ethernet adapter found.'; Stop-Transcript | Out-Null; exit 1 }
$name = $a.Name
$a | Format-List * | Out-File (Join-Path $out 'adapter.txt')
Write-Host "adapter: $name / $($a.InterfaceDescription) / $($a.Status) / $($a.LinkSpeed) / MAC $($a.MacAddress)"
Get-NetAdapterAdvancedProperty -Name $name -AllProperties | Format-Table -AutoSize DisplayName,RegistryKeyword,RegistryValue | Out-File (Join-Path $out 'advanced.txt') -Width 300
Get-NetIPConfiguration -InterfaceAlias $name -Detailed | Out-File (Join-Path $out 'ipconfig-before.txt')
$pnp = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'ACPI\RKCP6543*' }
$pnp | Format-List * | Out-File (Join-Path $out 'pnp.txt')
if ($pnp) { Get-PnpDeviceProperty -InstanceId $pnp.InstanceId | Format-Table -AutoSize KeyName,Data | Out-File (Join-Path $out 'pnp-props.txt') -Width 400 }
sc.exe qc dwc_eqos | Out-File (Join-Path $out 'service.txt')
Stats 'before'

Step 'start tracing (driver ETW + pktmon)'
logman stop eqos -ets 2>$null | Out-Null
logman start eqos -p '{5d8331d3-70b3-5620-5664-db28f48a4b79}' 0xFF 5 -ets -o (Join-Path $out 'eqos.etl')
pktmon stop 2>$null | Out-Null
pktmon filter remove | Out-Null
pktmon start --capture --pkt-size 0 --comp all --file-size 64 --file-name (Join-Path $out 'pkt.etl')

Step 'restart the adapter (driver init + link up, traced)'
Disable-NetAdapter -Name $name -Confirm:$false
Start-Sleep 3
Enable-NetAdapter -Name $name -Confirm:$false
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep 1
    $s = Get-NetAdapter -Name $name
    if ($s.Status -eq 'Up') { break }
}
Write-Host "after enable: $($s.Status) $($s.LinkSpeed) (${i}s)"

Step 'DHCP (up to 40 s)'
Start-Sleep 5
$job = Start-Job { ipconfig /renew 2>&1 }
if (-not (Wait-Job $job -Timeout 40)) { Write-Host 'renew still running after 40 s, moving on' }
Receive-Job $job | Out-File (Join-Path $out 'renew.txt')
Remove-Job $job -Force
Get-NetIPAddress -InterfaceAlias $name -AddressFamily IPv4 | Format-Table IPAddress,PrefixOrigin,AddressState
Stats 'after-dhcp'

Step 'static-IP test: does anything answer ARP?'
# Linux on this board gets 192.168.2.x on this port. Borrow .250 briefly, then go back to DHCP.
New-NetIPAddress -InterfaceAlias $name -IPAddress 192.168.2.250 -PrefixLength 24 -ErrorAction SilentlyContinue | Out-Null
Start-Sleep 3
foreach ($t in '192.168.2.1','192.168.2.254') { ping -n 3 -w 1000 $t }
arp -a -N 192.168.2.250 | Tee-Object (Join-Path $out 'arp.txt')
Get-NetNeighbor -InterfaceAlias $name | Format-Table IPAddress,LinkLayerAddress,State | Out-File (Join-Path $out 'neighbors.txt')
Stats 'after-static'
Remove-NetIPAddress -InterfaceAlias $name -IPAddress 192.168.2.250 -Confirm:$false -ErrorAction SilentlyContinue
Set-NetIPInterface -InterfaceAlias $name -Dhcp Enabled

Step 'stop tracing'
pktmon counters | Tee-Object (Join-Path $out 'pktmon-counters.txt')
pktmon list --all | Out-File (Join-Path $out 'pktmon-components.txt')
pktmon stop
logman stop eqos -ets
pktmon etl2pcap (Join-Path $out 'pkt.etl') --out (Join-Path $out 'pkt.pcapng') | Out-Null
pktmon etl2txt (Join-Path $out 'pkt.etl') --out (Join-Path $out 'pkt.txt') | Out-Null
tracerpt (Join-Path $out 'eqos.etl') -o (Join-Path $out 'eqos.xml') -of XML -y | Out-Null

Step 'event logs'
wevtutil qe System /c:300 /rd:true /f:text | Out-File (Join-Path $out 'system-events.txt')
Get-WinEvent -LogName 'Microsoft-Windows-Dhcp-Client/Admin' -MaxEvents 50 -ErrorAction SilentlyContinue | Format-List TimeCreated,Id,Message | Out-File (Join-Path $out 'dhcp-events.txt')

Step 'shutdown fix (Fast Startup off, hiber bugcheck simulated)'
powercfg /h off
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager" /v PowerSimulateHiberBugcheck /t REG_DWORD /d 0x40 /f

Step 'done'
Write-Host "Results: $out"
Write-Host 'Shut down (not restart), then bring the stick back.'
Stop-Transcript | Out-Null
