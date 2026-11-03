<#
.SYNOPSIS
  Updates the boot files on a TouhouOS USB stick in place, without reflashing it: the HUMANVILLAGE volume (homes, TOUHOU.LOG) stays.

.DESCRIPTION
  Run it after `python tools\build.py`, from an elevated PowerShell (giving the stick's EFI System Partition a drive letter needs
  administrator rights; copying the logs does not):

      powershell -ExecutionPolicy Bypass -File tools\update-stick.ps1                  update the stick, keep its cmdline.txt
      powershell -ExecutionPolicy Bypass -File tools\update-stick.ps1 -GetLog          also copy TOUHOU.LOG / TOUHOU.PRV to stick-logs\
      powershell -ExecutionPolicy Bypass -File tools\update-stick.ps1 -LogsOnly        only copy the logs (no administrator rights needed)
      powershell -ExecutionPolicy Bypass -File tools\update-stick.ps1 -Cmdline "video=native blk.ro tz=EST5EDT,M3.2.0,M11.1.0"
      powershell -ExecutionPolicy Bypass -File tools\update-stick.ps1 -DryRun          say what would be done, change nothing

  Which disk: only USB disks that are neither the system nor the boot disk and that look like a TouhouOS stick (a small EFI System
  Partition and a FAT32 volume labelled HUMANVILLAGE). It shows the one it found and asks before it writes (-Yes skips the question). If
  there are several, name the disk with -Disk N (Disk Management shows the numbers).

  What is written: BOOTX64.EFI, kernel.elf and initrd.img on the stick's ESP (and cmdline.txt when -Cmdline is given), taken from
  build\esp. Each file is read back and compared (SHA-256). The ESP gets a drive letter for the moment only and loses it again. Nothing
  else on the stick, and no other disk, is touched.
#>
[CmdletBinding()]
param(
    [string]$Cmdline,
    [string]$Source,
    [int]$Disk = -1,
    [switch]$GetLog,
    [switch]$LogsOnly,
    [switch]$DryRun,
    [switch]$Yes
)
$ErrorActionPreference = 'Stop'
if (-not $Source) { $Source = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\build\esp' }
$EspType = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-Sticks {
    foreach ($d in @(Get-Disk | Where-Object { $_.BusType -eq 'USB' -and -not $_.IsSystem -and -not $_.IsBoot })) {
        if ($Disk -ge 0 -and $d.Number -ne $Disk) { continue }
        $parts = @(Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue)
        $esp = $parts | Where-Object { $_.GptType -eq $EspType -and $_.Size -le 512MB } | Select-Object -First 1
        $hv = $null
        foreach ($p in $parts) {
            $v = Get-Volume -Partition $p -ErrorAction SilentlyContinue
            if ($v -and $v.FileSystemLabel -like 'HUMANVILLA*') { $hv = [pscustomobject]@{ Part = $p; Vol = $v }; break }
        }
        if ($esp -and $hv) { [pscustomobject]@{ Disk = $d; Esp = $esp; Hv = $hv } }
    }
}

function Invoke-Diskpart([string[]]$Lines) {
    $f = Join-Path $env:TEMP ('touhou-diskpart-' + [guid]::NewGuid().ToString('N') + '.txt')
    try {
        Set-Content -Path $f -Value $Lines -Encoding ASCII
        $out = & diskpart.exe /s $f 2>&1
        if ($LASTEXITCODE -ne 0) { throw "diskpart failed:`n$($out -join "`n")" }
        return $out
    } finally {
        Remove-Item -Path $f -ErrorAction SilentlyContinue
    }
}

function Get-FileSha([string]$Path) { (Get-FileHash -Algorithm SHA256 -Path $Path).Hash }

$sticks = @(Find-Sticks)
if ($sticks.Count -eq 0) {
    Write-Host 'No TouhouOS stick found: plug it into this computer (a USB disk with a small EFI System Partition and a FAT32 volume labelled HUMANVILLAGE).'
    exit 1
}
if ($sticks.Count -gt 1) {
    Write-Host 'More than one disk looks like a TouhouOS stick; name the one you want with -Disk N:'
    foreach ($s in $sticks) { Write-Host ("  Disk {0}: {1}, {2:N1} GB" -f $s.Disk.Number, $s.Disk.FriendlyName, ($s.Disk.Size / 1GB)) }
    exit 1
}
$stick = $sticks[0]
$d = $stick.Disk
Write-Host ("Found the stick: Disk {0}, {1}, {2:N1} GB" -f $d.Number, $d.FriendlyName, ($d.Size / 1GB))
Write-Host ("  boot partition: partition {0}, {1} MB (EFI System Partition)" -f $stick.Esp.PartitionNumber, [int]($stick.Esp.Size / 1MB))
$hvLetter = $stick.Hv.Part.DriveLetter
Write-Host ("  HUMANVILLAGE:   partition {0}, {1:N0} MB{2}" -f $stick.Hv.Part.PartitionNumber, ($stick.Hv.Part.Size / 1MB), $(if ($hvLetter) { ", drive ${hvLetter}:" } else { ", no drive letter" }))

if ($GetLog -or $LogsOnly) {
    if (-not $hvLetter) { Write-Host 'HUMANVILLAGE has no drive letter, so the logs cannot be copied (give it one in Disk Management).' }
    else {
        $dest = Join-Path (Get-Location) ('stick-logs\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        if ($DryRun) { Write-Host "[dry run] would copy TOUHOU.LOG and TOUHOU.PRV from ${hvLetter}:\ to $dest" }
        else {
            New-Item -ItemType Directory -Force -Path $dest | Out-Null
            foreach ($n in 'TOUHOU.LOG', 'TOUHOU.PRV', 'README.TXT') {
                $p = "${hvLetter}:\$n"
                if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $dest; Write-Host "  copied $n -> $dest" }
            }
        }
    }
    if ($LogsOnly) { exit 0 }
}

$files = [ordered]@{ 'EFI\BOOT\BOOTX64.EFI' = $null; 'kernel.elf' = $null; 'initrd.img' = $null }
foreach ($k in @($files.Keys)) {
    $src = Join-Path $Source $k
    if (-not (Test-Path -LiteralPath $src)) { throw "$src is missing: run python tools\build.py first" }
    $files[$k] = (Resolve-Path -LiteralPath $src).Path
}
Write-Host 'Will write to the stick''s boot partition:'
foreach ($k in $files.Keys) { Write-Host ("  {0}  <-  {1}  ({2:N0} bytes)" -f $k, $files[$k], (Get-Item -LiteralPath $files[$k]).Length) }
if ($PSBoundParameters.ContainsKey('Cmdline')) { Write-Host "  cmdline.txt  <-  `"$Cmdline`"" } else { Write-Host '  cmdline.txt stays as it is on the stick.' }
if ($DryRun) { Write-Host '[dry run] nothing was changed.'; exit 0 }
if (-not (Test-Admin)) { throw 'Run this from an elevated PowerShell (right-click Windows PowerShell -> Run as administrator): the boot partition needs a drive letter for a moment.' }
if (-not $Yes) {
    $answer = Read-Host "Update Disk $($d.Number) ($($d.FriendlyName)) now? Type yes"
    if ($answer -ne 'yes') { Write-Host 'Nothing was changed.'; exit 1 }
}

# Give the ESP a drive letter for the moment (Windows hides ESPs on its own); remember whether we added it.
$espNum = $stick.Esp.PartitionNumber
$letter = (Get-Partition -DiskNumber $d.Number -PartitionNumber $espNum).DriveLetter
$added = $false
if (-not $letter) {
    Invoke-Diskpart @("select disk $($d.Number)", "select partition $espNum", 'assign') | Out-Null
    $added = $true
    for ($i = 0; $i -lt 20 -and -not $letter; $i++) {
        Start-Sleep -Milliseconds 500
        $letter = (Get-Partition -DiskNumber $d.Number -PartitionNumber $espNum).DriveLetter
    }
    if (-not $letter) { throw 'The boot partition did not get a drive letter; nothing was written.' }
}
$root = "${letter}:\"
try {
    if (-not (Test-Path -LiteralPath (Join-Path $root 'kernel.elf')) -or -not (Test-Path -LiteralPath (Join-Path $root 'EFI\BOOT'))) {
        throw "${root} does not look like a TouhouOS boot partition (no kernel.elf and EFI\BOOT); nothing was written."
    }
    foreach ($k in $files.Keys) {
        $dst = Join-Path $root $k
        Copy-Item -LiteralPath $files[$k] -Destination $dst -Force
        if ((Get-FileSha $dst) -ne (Get-FileSha $files[$k])) { throw "$k was written but reads back differently: do not boot this stick; run the script again." }
        Write-Host "  wrote and verified $k"
    }
    if ($PSBoundParameters.ContainsKey('Cmdline')) {
        $dst = Join-Path $root 'cmdline.txt'
        [IO.File]::WriteAllText($dst, $Cmdline + "`r`n", [Text.Encoding]::ASCII)
        Write-Host '  wrote cmdline.txt'
    }
    Write-VolumeCache -DriveLetter $letter -ErrorAction SilentlyContinue
} finally {
    if ($added) {
        try { Invoke-Diskpart @("select disk $($d.Number)", "select partition $espNum", 'remove') | Out-Null }
        catch { Write-Warning "The temporary drive letter ${letter}: stays; remove it in Disk Management if you like." }
    }
}
Write-Host 'Done. Eject the stick as usual (the notification area, "Safely remove") before pulling it.'
