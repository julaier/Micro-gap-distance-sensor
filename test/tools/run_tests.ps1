$ErrorActionPreference = 'Stop'
$testRoot = Split-Path -Parent $PSScriptRoot
$compilerPath = 'D:\mingw64\bin\gcc.exe'
if (-not (Test-Path -LiteralPath $compilerPath)) {
    $compilerPath = (Get-Command gcc -ErrorAction Stop).Source
}
Push-Location -LiteralPath $testRoot
try {
    if (-not (Test-Path -LiteralPath 'MDK-ARM\Objects')) {
        New-Item -ItemType Directory -Path 'MDK-ARM\Objects' | Out-Null
    }
    & $compilerPath -std=c99 -O2 -Wall -Wextra -Werror -IDriver `
        tests/test_ack_driver.c Driver/test_i2c.c -o MDK-ARM/Objects/ack_driver_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Host test compilation failed: $LASTEXITCODE" }
    & .\MDK-ARM\Objects\ack_driver_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Host test failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
