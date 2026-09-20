# Identify the driver behind the recurring 49e4-offset bugcheck.

$ErrorActionPreference = 'Continue'

Write-Output "=== loaded kernel drivers — non-Microsoft (likely 3rd-party) ==="
Get-CimInstance Win32_SystemDriver -ErrorAction SilentlyContinue |
    Where-Object { $_.State -eq 'Running' } |
    ForEach-Object {
        $svc = $_
        $img = $svc.PathName -replace '"','' -replace '^\\\?\?\\','' -replace '^\\SystemRoot\\','C:\Windows\'
        $info = $null
        if ($img -and (Test-Path $img)) {
            try { $info = Get-Item $img | Get-ItemProperty | Select-Object -ExpandProperty VersionInfo } catch {}
        }
        [PSCustomObject]@{
            Name      = $svc.Name
            DisplayName = $svc.DisplayName
            Path      = $img
            Company   = if ($info) { $info.CompanyName } else { '?' }
            FileVer   = if ($info) { $info.FileVersion } else { '?' }
        }
    } |
    Where-Object { $_.Company -notmatch 'Microsoft' -and $_.Company -ne '?' } |
    Sort-Object Company, Name |
    Format-Table -AutoSize

Write-Output "=== ALL running kernel drivers (full list) ==="
Get-CimInstance Win32_SystemDriver -ErrorAction SilentlyContinue |
    Where-Object { $_.State -eq 'Running' } |
    Select-Object Name, DisplayName, StartMode |
    Sort-Object Name |
    Format-Table -AutoSize

Write-Output "=== OpenVPN / Tap / Wintun drivers ==="
Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue |
    Where-Object { $_.DeviceName -match 'TAP|Wintun|OpenVPN|VirtualBox|Hyper-V' -or $_.DriverProviderName -match 'OpenVPN|Wintun' } |
    Select-Object DeviceName, DriverProviderName, DriverVersion, DriverDate |
    Format-Table -AutoSize

Write-Output "=== Recent service crashes (App log Service Control Manager errors, last 4h) ==="
$since4 = (Get-Date).AddHours(-4)
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Service Control Manager'; Level=2; StartTime=$since4} -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    Select-Object TimeCreated, Id,
        @{n='Msg';e={ (($_.Message -split "`r`n")[0]).Substring(0,[Math]::Min(150,($_.Message -split "`r`n")[0].Length)) }} |
    Format-Table -AutoSize

Write-Output "=== events in 60 seconds BEFORE the 12:21 crash (14:20:30 -> 14:21:50 local) ==="
$winStart = Get-Date "2026-05-25 14:20:30"
$winEnd   = Get-Date "2026-05-25 14:21:50"
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$winStart; EndTime=$winEnd} -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    Select-Object TimeCreated, Id, LevelDisplayName, ProviderName,
        @{n='Msg';e={ (($_.Message -split "`r`n")[0]).Substring(0,[Math]::Min(140,($_.Message -split "`r`n")[0].Length)) }} |
    Format-Table -AutoSize

Write-Output "=== events in 60 seconds BEFORE the 10:01 crash (12:00:30 -> 12:01:30 local) ==="
$winStart = Get-Date "2026-05-25 12:00:30"
$winEnd   = Get-Date "2026-05-25 12:01:30"
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$winStart; EndTime=$winEnd} -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    Select-Object TimeCreated, Id, LevelDisplayName, ProviderName,
        @{n='Msg';e={ (($_.Message -split "`r`n")[0]).Substring(0,[Math]::Min(140,($_.Message -split "`r`n")[0].Length)) }} |
    Format-Table -AutoSize
