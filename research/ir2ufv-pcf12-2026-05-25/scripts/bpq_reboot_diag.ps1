# Diagnostic harvest for unexpected reboots on iw2ohx-bpq.
# Runs read-only — events, dumps, last-boot info.

$ErrorActionPreference = 'Continue'

Write-Output "=== os + uptime ==="
$os = Get-CimInstance Win32_OperatingSystem
$os | Select-Object Caption, Version, LastBootUpTime, LocalDateTime | Format-List

Write-Output "=== boot history (last 10 boots, via Event ID 6005) ==="
Get-WinEvent -FilterHashtable @{LogName='System'; Id=6005} -MaxEvents 10 |
    Select-Object TimeCreated, ProviderName, Id |
    Format-Table -AutoSize

Write-Output "=== last 20 shutdown/reboot-related events (24h) ==="
$since = (Get-Date).AddHours(-24)
# 1074 = clean shutdown initiated by user/process
# 1076 = reason for previous unexpected shutdown (asked at next boot)
# 6005 = event log service started (= boot)
# 6006 = event log service stopped (= clean shutdown)
# 6008 = previous shutdown was unexpected
# 6013 = uptime announcement
# 41   = kernel-power: system rebooted without cleanly shutting down
# 109  = kernel-power: system entering sleep
# 137  = kernel-power: dirty shutdown
Get-WinEvent -FilterHashtable @{
    LogName='System';
    StartTime=$since;
    Id=@(1074,1076,6005,6006,6008,6013,41,109,137)
} -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    Select-Object TimeCreated, Id, ProviderName,
        @{n='Msg';e={ $_.Message -replace "`r`n", " | " -replace "\s+", " " }} |
    Format-Table -AutoSize -Wrap

Write-Output "=== Kernel-Power 41 (unexpected reboot) details — last 5 ==="
Get-WinEvent -FilterHashtable @{LogName='System'; Id=41; ProviderName='Microsoft-Windows-Kernel-Power'} `
    -MaxEvents 5 -ErrorAction SilentlyContinue |
    ForEach-Object {
        Write-Output "--- $($_.TimeCreated) ---"
        Write-Output $_.Message
    }

Write-Output "=== WHEA-Logger hardware errors (24h) ==="
Get-WinEvent -FilterHashtable @{
    LogName='System'; ProviderName='Microsoft-Windows-WHEA-Logger'; StartTime=$since
} -ErrorAction SilentlyContinue |
    Select-Object TimeCreated, Id, LevelDisplayName,
        @{n='Msg';e={ ($_.Message -split "`r`n")[0] }} |
    Format-Table -AutoSize

Write-Output "=== BSOD memory dumps ==="
$minidumpDir = 'C:\Windows\Minidump'
if (Test-Path $minidumpDir) {
    Get-ChildItem $minidumpDir -Filter *.dmp -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 10 LastWriteTime, Name, Length |
        Format-Table -AutoSize
} else {
    Write-Output "(no $minidumpDir folder)"
}
$memDmp = 'C:\Windows\MEMORY.DMP'
if (Test-Path $memDmp) {
    Get-Item $memDmp | Select-Object LastWriteTime, Length | Format-List
} else {
    Write-Output "(no $memDmp file)"
}

Write-Output "=== WER reports (recent) ==="
$wer = 'C:\ProgramData\Microsoft\Windows\WER\ReportArchive'
if (Test-Path $wer) {
    Get-ChildItem $wer -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 5 LastWriteTime, Name |
        Format-Table -AutoSize
} else {
    Write-Output "(no $wer folder)"
}

Write-Output "=== recent Windows Update install events (may explain auto-reboot) ==="
Get-WinEvent -LogName 'Setup' -MaxEvents 30 -ErrorAction SilentlyContinue |
    Where-Object { $_.TimeCreated -ge $since } |
    Select-Object TimeCreated, Id, LevelDisplayName,
        @{n='Msg';e={ ($_.Message -split "`r`n")[0] }} |
    Format-Table -AutoSize

Write-Output "=== thermal / disk SMART quick check ==="
# Disk SMART status (true = OK, false = failing)
Get-WmiObject -Namespace root\wmi -Class MSStorageDriver_FailurePredictStatus -ErrorAction SilentlyContinue |
    Select-Object InstanceName, PredictFailure, Reason |
    Format-Table -AutoSize

Write-Output "=== UPS / battery status (if any) ==="
Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue |
    Select-Object DeviceID, BatteryStatus, EstimatedChargeRemaining, EstimatedRunTime |
    Format-Table -AutoSize
Get-CimInstance Win32_PortableBattery -ErrorAction SilentlyContinue |
    Select-Object Name, BatteryStatus |
    Format-Table -AutoSize
