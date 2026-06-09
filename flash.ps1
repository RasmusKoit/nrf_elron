<#
  Flash the Elron firmware to the XIAO nRF52840 UF2 bootloader.

  1. Finds the board's COM port (USB VID 2FE3 / PID 0100, "Elron Display").
  2. Opens it at 1200 baud -> firmware reboots into the UF2 bootloader
     (no physical double-tap needed, once the touch-capable firmware is on).
  3. Waits for the bootloader's mass-storage drive (INFO_UF2.TXT) and copies
     the .uf2 onto it. Falls back to asking for a manual double-tap.

  Usage:  powershell -ExecutionPolicy Bypass -File flash.ps1 [-Uf2 <path>]
#>
param(
    [string]$Uf2 = (Join-Path $PSScriptRoot "elron_train.uf2")
)

if (-not (Test-Path $Uf2)) { Write-Error "UF2 not found: $Uf2"; exit 1 }
Write-Host "[flash] firmware: $Uf2"

function Find-Uf2Drive {
    Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | ForEach-Object {
        $info = Join-Path $_.Root "INFO_UF2.TXT"
        if (Test-Path $info) { return $_.Root }
    } | Select-Object -First 1
}

# 1) Find COM port + 2) 1200-baud touch to enter the bootloader
$pnp = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -match 'COM\d+' -and
    ($_.DeviceID -match 'VID_2FE3.*PID_0100' -or $_.Name -match 'Elron Display')
}
$com = if ($pnp) { [regex]::Match($pnp.Name, 'COM\d+').Value } else { $null }

if ($com) {
    Write-Host "[flash] 1200-baud touch on $com ..."
    try {
        # Force a baud *transition* down to 1200 so the host definitely sends
        # SET_LINE_CODING (some stacks skip it if the rate is unchanged).
        $warm = New-Object System.IO.Ports.SerialPort($com, 9600)
        $warm.Open(); Start-Sleep -Milliseconds 150; $warm.Close()
        Start-Sleep -Milliseconds 150
        $sp = New-Object System.IO.Ports.SerialPort($com, 1200)
        $sp.DtrEnable = $true
        $sp.Open(); Start-Sleep -Milliseconds 700; $sp.Close()
    } catch { Write-Host "[flash] touch failed ($_); will wait for manual double-tap" }
} else {
    Write-Host "[flash] board COM port not found; double-tap RST to enter the bootloader"
}

# 3) Wait for the UF2 drive, then copy
$root = $null
for ($i = 0; $i -lt 60; $i++) {
    $root = Find-Uf2Drive
    if ($root) { break }
    if ($i -eq 6) { Write-Host "[flash] no bootloader drive yet -> double-tap the RST button" }
    Start-Sleep -Milliseconds 500
}
if (-not $root) { Write-Error "[flash] UF2 bootloader drive never appeared"; exit 2 }

Write-Host "[flash] copying to $root"
Copy-Item -LiteralPath $Uf2 -Destination $root -Force
Write-Host "[flash] done - board will reboot into the new firmware"
