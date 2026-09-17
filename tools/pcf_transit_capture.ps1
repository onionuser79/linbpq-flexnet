<#
    pcf_transit_capture.ps1 — capture PC/Flexnet IW2OHX-12's AXIP traffic
    ON the transit node itself.

    Why here and not on iw2ohx-gw: gw is not in the path between the
    (X)Net nodes and IW2OHX-12 (verified — tcpdump on gw sees zero frames
    for either leg). IW2OHX-12 is the node that actually forwards for
    IW2OHX-4 <-> IW2OHX-14, so capturing on this host is the only place
    that sees a native transit's INPUT and OUTPUT together. That pairing
    is the whole point: FlexNet's L2/L3 forwarding is undocumented, so the
    only ground truth is watching a real router do it.

    flexkrnl.exe holds UDP 93/94/95, one per FlexNet peer link, so the
    filters below are the complete AXIP surface of this node.

    Uses pktmon: built into Windows, no Wireshark here. Requires
    elevation, which the iw2ohx account has.

    Usage (from iw2ohx-gw or macmini over ssh):
        powershell -NoProfile -File pcf_transit_capture.ps1 -Seconds 60
#>
param(
    [int]    $Seconds = 60,
    [string] $OutDir  = 'C:\temp',
    [string] $Tag     = ''
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ($Tag) { $stamp = "$stamp-$Tag" }
$etl   = Join-Path $OutDir "pcf-transit-$stamp.etl"
$pcap  = Join-Path $OutDir "pcf-transit-$stamp.pcapng"

Write-Output ">> clearing old filters"
& pktmon filter remove | Out-Null

# One filter per flexkrnl AXIP port. pktmon matches either direction, so
# three filters cover all six directions of the three peer links.
foreach ($p in 93, 94, 95) {
    Write-Output ">> filter: UDP port $p"
    & pktmon filter add "FlexAXIP$p" -t UDP -p $p | Out-Null
}
& pktmon filter list | Out-String | Write-Output

Write-Output ">> starting capture -> $etl"
# --pkt-size 0 keeps the WHOLE frame: the AX.25 header and any FlexNet
# L3/L4 envelope live past the default 128-byte snap, and truncating
# them would discard exactly what we are here to read.
& pktmon start --capture --pkt-size 0 --file-name $etl --file-size 64 | Out-String | Write-Output
if (-not (& pktmon status | Out-String).Contains('Active')) {
    Write-Output '>> WARNING: pktmon does not report Active — check the start output above'
}

Write-Output ">> capturing for ${Seconds}s (trigger the connect NOW)"
Start-Sleep -Seconds $Seconds

Write-Output ">> stopping"
& pktmon stop | Out-String | Write-Output

Write-Output ">> converting to pcapng"
& pktmon etl2pcap $etl -o $pcap | Out-String | Write-Output

& pktmon filter remove | Out-Null

if ((Test-Path $pcap) -and ((Get-Item $pcap).Length -gt 0)) {
    $sz = (Get-Item $pcap).Length
    Write-Output ">> PCAP $pcap ($sz bytes)"
    Write-Output "PCAPPATH=$pcap"
} else {
    Write-Output ">> conversion FAILED; raw etl kept at $etl"
    Write-Output "PCAPPATH=$etl"
}
