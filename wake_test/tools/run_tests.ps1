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
    [xml]$project = Get-Content -Raw 'MDK-ARM\LDC1614_Wake_Test.uvprojx'
    $sources = @($project.Project.Targets.Target.Groups.Group.Files.File |
        Where-Object { $_.FileType -in '1','2' } | ForEach-Object { $_.FilePath })
    $expected = @('../../Startup/startup_stm32c011xx.s', '../../CMSIS/system_stm32c0xx.c',
        '../App/main.c', '../App/wake_diag.c', '../Driver/wake_i2c.c', '../../test/Driver/test_board.c')
    if (Compare-Object $sources $expected) { throw 'Unexpected Keil source list' }
    foreach ($source in $sources) {
        $resolvedSource = Join-Path 'MDK-ARM' $source
        if (-not (Test-Path -LiteralPath $resolvedSource)) { throw "Missing source: $source" }
    }
    & $compilerPath -std=c99 -O2 -Wall -Wextra -Werror -IApp -IDriver -I../test/Driver `
        tests/test_wake.c Driver/wake_i2c.c -o MDK-ARM/Objects/wake_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Host compilation failed: $LASTEXITCODE" }
    & .\MDK-ARM\Objects\wake_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Host test failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
