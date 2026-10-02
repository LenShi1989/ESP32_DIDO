<#
.SYNOPSIS
    把 data/ 打包成 spiffs.bin，可選擇直接用序列埠燒錄。

.DESCRIPTION
    產生的 spiffs.bin 可以：
      1. 從網頁「OTA 更新」→ 更新對象選「檔案系統」上傳，或
      2. 加上 -Port 參數由本腳本直接用 esptool 燒進去。

    工具與分區參數都從 Arduino ESP32 core 自動抓，不必手動填路徑。

.PARAMETER Scheme
    分區配置，需與 Arduino IDE 的 Partition Scheme 一致。預設 min_spiffs
    （對應選單的 Minimal SPIFFS (Large APPS with OTA)）。

.PARAMETER Port
    指定後直接燒錄，例如 COM8。未指定則只產生檔案。

.PARAMETER Baud
    燒錄鮑率，預設 115200（本板 921600 會失敗）。

.EXAMPLE
    .\tools\make-spiffs.ps1
    只產生 build\spiffs.bin，供網頁 OTA 上傳。

.EXAMPLE
    .\tools\make-spiffs.ps1 -Port COM8
    產生後直接燒錄到裝置。
#>
[CmdletBinding()]
param(
    [string]$Scheme = 'min_spiffs',
    [string]$Port,
    [int]$Baud = 115200,
    [string]$DataDir = 'data',
    [string]$OutFile = 'build\spiffs.bin'
)

$ErrorActionPreference = 'Stop'

# 以腳本所在位置的上一層當專案根目錄，從任何路徑呼叫都能運作
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    # ---- 找 ESP32 core ----
    $pkg = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32'
    if (-not (Test-Path $pkg)) { throw "找不到 ESP32 core：$pkg" }

    $core = Get-ChildItem "$pkg\hardware\esp32" -Directory |
            Sort-Object { [version]$_.Name } -Descending |
            Select-Object -First 1
    if (-not $core) { throw "找不到 esp32 hardware 目錄" }

    $mkspiffs = Get-ChildItem "$pkg\tools\mkspiffs" -Recurse -Filter 'mkspiffs.exe' |
                Select-Object -First 1
    if (-not $mkspiffs) { throw "找不到 mkspiffs.exe" }

    # ---- 從分區表讀出 SPIFFS 的位移與大小 ----
    $csv = Join-Path $core.FullName "tools\partitions\$Scheme.csv"
    if (-not (Test-Path $csv)) { throw "找不到分區表：$csv" }

    $row = Get-Content $csv |
           Where-Object { $_ -match '^\s*spiffs\s*,' } |
           Select-Object -First 1
    if (-not $row) { throw "$Scheme.csv 裡沒有 spiffs 分區" }

    $cols   = $row -split ',' | ForEach-Object { $_.Trim() }
    $offset = $cols[3]
    $size   = [Convert]::ToInt32($cols[4], 16)

    # ---- 檢查來源 ----
    if (-not (Test-Path $DataDir)) { throw "找不到資料夾：$DataDir" }
    $srcBytes = (Get-ChildItem $DataDir -Recurse -File | Measure-Object Length -Sum).Sum
    if ($srcBytes -ge $size) {
        throw ("$DataDir 共 {0:N0} bytes，超過 {1} 的 SPIFFS 分區 {2:N0} bytes" -f $srcBytes, $Scheme, $size)
    }

    $outDir = Split-Path -Parent $OutFile
    if ($outDir -and -not (Test-Path $outDir)) {
        New-Item -ItemType Directory -Path $outDir | Out-Null
    }

    Write-Host "core      : $($core.Name)"
    Write-Host "分區      : $Scheme  offset $offset  size $('{0:N0}' -f $size) bytes"
    Write-Host "來源      : $DataDir  $('{0:N0}' -f $srcBytes) bytes"

    # ---- 打包 ----
    & $mkspiffs.FullName -c $DataDir -b 4096 -p 256 -s $size $OutFile
    if ($LASTEXITCODE -ne 0) { throw "mkspiffs 失敗（exit $LASTEXITCODE）" }

    $made = (Get-Item $OutFile).Length
    Write-Host "`n已產生 $OutFile（$('{0:N0}' -f $made) bytes，使用率 $('{0:P1}' -f ($srcBytes / $size))）" -ForegroundColor Green

    # ---- 可選：直接燒錄 ----
    if (-not $Port) {
        Write-Host "`n接著可從網頁 OTA 更新 → 更新對象選「檔案系統」上傳這個檔案。"
        return
    }

    $esptool = Get-ChildItem "$pkg\tools\esptool_py" -Recurse -Filter 'esptool.exe' |
               Select-Object -First 1
    if (-not $esptool) { throw "找不到 esptool.exe" }

    Write-Host "`n燒錄到 $Port @ $Baud ..." -ForegroundColor Cyan
    & $esptool.FullName --chip esp32 --port $Port --baud $Baud `
        write_flash -z $offset $OutFile
    if ($LASTEXITCODE -ne 0) { throw "esptool 失敗（exit $LASTEXITCODE）" }
    Write-Host "燒錄完成" -ForegroundColor Green
}
finally {
    Pop-Location
}
