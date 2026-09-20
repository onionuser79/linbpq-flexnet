# Targeted bugcheck + pre-crash context probe.

$ErrorActionPreference = 'Continue'

Write-Output "=== BugCheck events (ID 1001, source BugCheck) — last 5 ==="
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-WER-SystemErrorReporting'} `
    -MaxEvents 5 -ErrorAction SilentlyContinue |
    ForEach-Object {
        Write-Output "--- $($_.TimeCreated) ID=$($_.Id) ---"
        Write-Output $_.Message
        Write-Output ""
    }

Write-Output "=== Alt source: BugCheck provider — last 5 ==="
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='BugCheck'} `
    -MaxEvents 5 -ErrorAction SilentlyContinue |
    ForEach-Object {
        Write-Output "--- $($_.TimeCreated) ID=$($_.Id) ---"
        Write-Output $_.Message
        Write-Output ""
    }

Write-Output "=== System log — last 50 events BEFORE first crash (between 09:30 and 10:01 UTC=11:30-12:01 local) ==="
$winStart = Get-Date "2026-05-25 11:30:00"
$winEnd   = Get-Date "2026-05-25 12:02:00"
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$winStart; EndTime=$winEnd} -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    Select-Object TimeCreated, Id, LevelDisplayName, ProviderName,
        @{n='Msg';e={ (($_.Message -split "`r`n")[0]).Substring(0,[Math]::Min(120,($_.Message -split "`r`n")[0].Length)) }} |
    Format-Table -AutoSize

Write-Output "=== System log — last 50 events BEFORE second crash (between 13:55 and 14:22 local) ==="
$winStart2 = Get-Date "2026-05-25 13:55:00"
$winEnd2   = Get-Date "2026-05-25 14:22:00"
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$winStart2; EndTime=$winEnd2} -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    Select-Object TimeCreated, Id, LevelDisplayName, ProviderName,
        @{n='Msg';e={ (($_.Message -split "`r`n")[0]).Substring(0,[Math]::Min(120,($_.Message -split "`r`n")[0].Length)) }} |
    Format-Table -AutoSize

Write-Output "=== Application log: errors in last 4h ==="
$since4 = (Get-Date).AddHours(-4)
Get-WinEvent -FilterHashtable @{LogName='Application'; Level=2; StartTime=$since4} -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    Select-Object TimeCreated, Id, ProviderName,
        @{n='Msg';e={ (($_.Message -split "`r`n")[0]).Substring(0,[Math]::Min(140,($_.Message -split "`r`n")[0].Length)) }} |
    Format-Table -AutoSize

Write-Output "=== installed Windows Updates — last 10 ==="
Get-HotFix -ErrorAction SilentlyContinue |
    Sort-Object InstalledOn -Descending |
    Select-Object -First 10 HotFixID, Description, InstalledOn |
    Format-Table -AutoSize

Write-Output "=== Setup log (update install events) — last 4h ==="
Get-WinEvent -LogName Setup -MaxEvents 50 -ErrorAction SilentlyContinue |
    Where-Object { $_.TimeCreated -ge $since4 } |
    Sort-Object TimeCreated |
    Select-Object TimeCreated, Id, LevelDisplayName,
        @{n='Msg';e={ (($_.Message -split "`r`n")[0]).Substring(0,[Math]::Min(140,($_.Message -split "`r`n")[0].Length)) }} |
    Format-Table -AutoSize

Write-Output "=== SMART (Win32_DiskDrive Status) ==="
Get-CimInstance Win32_DiskDrive -ErrorAction SilentlyContinue |
    Select-Object Model, Status, Size, MediaType |
    Format-Table -AutoSize

Write-Output "=== minidump folder listing with hashes ==="
$dumpDir = 'C:\Windows\Minidump'
Get-ChildItem $dumpDir -Filter *.dmp -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 5 LastWriteTime, Name, Length |
    Format-Table -AutoSize
