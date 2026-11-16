$ErrorActionPreference = 'Stop'
$flowRoot = Split-Path -Parent $PSScriptRoot
$compilerPath = 'D:\mingw64\bin\gcc.exe'
if (-not (Test-Path -LiteralPath $compilerPath)) {
    $compilerPath = (Get-Command gcc -ErrorAction Stop).Source
}
Push-Location -LiteralPath $flowRoot
try {
    [xml]$project = Get-Content -Raw 'MDK-ARM\LDC1614_F103_Margin_STLink.uvprojx'
    $target = @($project.Project.Targets.Target)
    if ($target.Count -ne 1 -or $target[0].TargetName -ne 'LDC1614_F103_MARGIN_SW') { throw 'Wrong margin target' }
    $sources = @($target[0].Groups.Group.Files.File | Where-Object { $_.FileType -in '1','2' } |
        ForEach-Object { $_.FilePath })
    $expected = @('../../Startup/startup_stm32c011xx.s','../../CMSIS/system_stm32c0xx.c',
        '../App/main.c','../App/f103_flow.c','../Driver/margin_i2c.c','../../test/Driver/test_board.c',
        '../Device/ldc1614.c','../Driver/f103_i2c_adapter.c')
    if (Compare-Object $sources $expected) { throw 'Unexpected margin source list' }
    foreach ($source in $sources) {
        if (-not (Test-Path -LiteralPath (Join-Path MDK-ARM $source))) { throw "Missing $source" }
    }
    if ($target[0].TargetOption.TargetArmAds.Cads.VariousControls.Define -notmatch 'MARGIN_I2C=1,ACK_COMPARE_PROFILE=1,') {
        throw 'Missing margin build define'
    }
    [xml]$options = Get-Content -Raw 'MDK-ARM\LDC1614_F103_Margin_STLink.uvoptx'
    if ($options.ProjectOpt.Target.TargetName -ne 'LDC1614_F103_MARGIN_SW' -or
        $options.ProjectOpt.Target.TargetOption.DebugOpt.nTsel -ne '5' -or
        $options.ProjectOpt.Target.TargetOption.DebugOpt.pMon -ne 'STLink\ST-LINKIII-KEIL_SWO.dll' -or
        $target[0].TargetOption.Utilities.Flash2 -ne 'STLink\ST-LINKIII-KEIL_SWO.dll') { throw 'Expected ST-Link debugger/download' }
    New-Item -ItemType Directory -Force -Path 'MDK-ARM\HostTests' | Out-Null
    & $compilerPath -std=c99 -O2 -Wall -Wextra -Werror -DMARGIN_HOST=1 -DMARGIN_I2C=1 -DACK_COMPARE_PROFILE=1 `
        -IApp -IDevice -IDriver -I../ack_compare/Driver -I../test/Driver `
        tests/test_margin.c Driver/margin_i2c.c Driver/f103_i2c_adapter.c Device/ldc1614.c App/f103_flow.c `
        -o MDK-ARM/HostTests/margin_edges.exe
    if ($LASTEXITCODE -ne 0) { throw 'Margin test compilation failed' }
    foreach ($scenario in @('edges','flow_ok') + (0..14 | ForEach-Object { "nack_$_" })) {
        & .\MDK-ARM\HostTests\margin_edges.exe $scenario
        if ($LASTEXITCODE -ne 0) { throw "Margin test failed: $scenario" }
    }
    & $compilerPath -std=c99 -O2 -Wall -Wextra -Werror -Itests/margin_stub -IDriver `
        tests/test_margin_port.c -o MDK-ARM/HostTests/margin_port.exe
    if ($LASTEXITCODE -ne 0) { throw 'Margin port test compilation failed' }
    & .\MDK-ARM\HostTests\margin_port.exe
    if ($LASTEXITCODE -ne 0) { throw 'Margin port test failed' }
} finally { Pop-Location }
