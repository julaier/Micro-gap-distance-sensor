$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$compilerPath = 'D:\mingw64\bin\gcc.exe'
if (-not (Test-Path -LiteralPath $compilerPath)) {
    $compilerPath = (Get-Command gcc -ErrorAction Stop).Source
}
Push-Location -LiteralPath $projectRoot
try {
    & $compilerPath -std=c99 -O2 -Wall -Wextra -Werror -IDriver -IDevice -Itest/Driver `
        tests/soft_i2c/test_ported_i2c.c Driver/hard_i2c.c Device/ldc1614.c `
        -o MDK-ARM/Objects/ported_i2c_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Production I2C test compilation failed: $LASTEXITCODE" }
    & .\MDK-ARM\Objects\ported_i2c_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Production I2C test failed: $LASTEXITCODE" }

    & $compilerPath -std=c99 -O2 -fwhole-program -Wall -Wextra -Werror `
        -ffunction-sections -fdata-sections -IApp -IDriver -IDevice -IService `
        tests/soft_i2c/test_app_scan.c '-Wl,--gc-sections' -o MDK-ARM/Objects/app_scan_test.exe
    if ($LASTEXITCODE -ne 0) { throw "App scanner test compilation failed: $LASTEXITCODE" }
    & .\MDK-ARM\Objects\app_scan_test.exe
    if ($LASTEXITCODE -ne 0) { throw "App scanner test failed: $LASTEXITCODE" }

    & $compilerPath -std=c99 -O2 -fwhole-program -Wall -Wextra -Werror `
        -ffunction-sections -fdata-sections -IApp -IDriver -IDevice -IService `
        tests/soft_i2c/test_app_startup.c '-Wl,--gc-sections' -o MDK-ARM/Objects/app_startup_test.exe
    if ($LASTEXITCODE -ne 0) { throw "App startup test compilation failed: $LASTEXITCODE" }
    & .\MDK-ARM\Objects\app_startup_test.exe
    if ($LASTEXITCODE -ne 0) { throw "App startup test failed: $LASTEXITCODE" }

    & $compilerPath -std=c99 -O2 -Wall -Wextra -Werror -IApp -IDriver -IDevice -IService -Itest/Driver `
        tests/soft_i2c/test_app_integration.c Driver/hard_i2c.c Device/ldc1614.c `
        Service/measure.c Service/protocol.c App/console.c -o MDK-ARM/Objects/app_integration_test.exe
    if ($LASTEXITCODE -ne 0) { throw "App integration test compilation failed: $LASTEXITCODE" }
    & .\MDK-ARM\Objects\app_integration_test.exe
    if ($LASTEXITCODE -ne 0) { throw "App integration test failed: $LASTEXITCODE" }

    & .\test\tools\run_tests.ps1
} finally {
    Pop-Location
}
