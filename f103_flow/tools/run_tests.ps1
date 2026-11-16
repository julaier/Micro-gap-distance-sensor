$ErrorActionPreference = 'Stop'
$flowRoot = Split-Path -Parent $PSScriptRoot
$compilerPath = 'D:\mingw64\bin\gcc.exe'
if (-not (Test-Path -LiteralPath $compilerPath)) {
    $compilerPath = (Get-Command gcc -ErrorAction Stop).Source
}
Push-Location -LiteralPath $flowRoot
try {
    # Verify the actual LDC implementation, not merely a manually copied table.
    $original = (Get-Content -Raw '..\..\new\Device\ldc1614.c').Replace("`r`n", "`n").TrimEnd()
    $current = (Get-Content -Raw 'Device\ldc1614.c').Replace("`r`n", "`n").TrimEnd()
    if ($current -cne $original) { throw 'LDC implementation differs from original F103' }
    [xml]$proj = Get-Content -Raw 'MDK-ARM\LDC1614_F103_Flow.uvprojx'
    $targets = @($proj.Project.Targets.Target)
    if ($targets.Count -ne 1 -or $targets[0].TargetName -ne 'LDC1614_F103_FLOW_SW') {
        throw 'Unexpected target'
    }
    $sourcePaths = @($targets[0].Groups.Group.Files.File | Where-Object { $_.FileType -in '1','2' } |
        ForEach-Object { $_.FilePath })
    $expected = @('../../Startup/startup_stm32c011xx.s', '../../CMSIS/system_stm32c0xx.c',
        '../../ack_compare/Driver/ack_port.c', '../App/main.c', '../App/f103_flow.c',
        '../../ack_compare/Driver/wake_i2c.c', '../../test/Driver/test_board.c',
        '../Device/ldc1614.c', '../Driver/f103_i2c_adapter.c')
    if (Compare-Object $sourcePaths $expected) { throw 'Unexpected source list' }
    foreach ($sourcePath in $sourcePaths) {
        if (-not (Test-Path -LiteralPath (Join-Path 'MDK-ARM' $sourcePath))) { throw "Missing $sourcePath" }
    }
    if ($targets[0].TargetOption.TargetArmAds.Cads.VariousControls.Define -notmatch 'ACK_COMPARE_PROFILE=1,') {
        throw 'Expected unchanged B software driver'
    }
    # A newly created project must not silently default to ULINK on an ST-Link board.
    [xml]$debugOptions = Get-Content -Raw 'MDK-ARM\LDC1614_F103_Flow.uvoptx'
    $debugTarget = $debugOptions.ProjectOpt.Target
    if ($debugTarget.TargetName -ne 'LDC1614_F103_FLOW_SW' -or
        $debugTarget.TargetOption.DebugOpt.uSim -ne '0' -or
        $debugTarget.TargetOption.DebugOpt.uTrg -ne '1' -or
        $debugTarget.TargetOption.DebugOpt.nTsel -ne '5' -or
        $debugTarget.TargetOption.DebugOpt.pMon -ne 'STLink\ST-LINKIII-KEIL_SWO.dll') {
        throw 'Expected ST-Link target debugger, not simulator or ULINK'
    }
    $stSettings = @($debugTarget.TargetOption.TargetDriverDllRegistry.SetRegEntry |
        Where-Object { $_.Key -eq 'ST-LINKIII-KEIL_SWO' })
    if ($stSettings.Count -ne 1 -or $stSettings[0].Name -notmatch 'STM32C0x_32\.FLM' -or
        $targets[0].TargetOption.Utilities.Flash1.UseTargetDll -ne '1' -or
        $targets[0].TargetOption.Utilities.bUseTDR -ne '1' -or
        $targets[0].TargetOption.Utilities.Flash2 -ne 'STLink\ST-LINKIII-KEIL_SWO.dll') {
        throw 'Expected ST-Link download driver with C011 32K flash algorithm'
    }
    New-Item -ItemType Directory -Force -Path 'MDK-ARM\HostTests' | Out-Null
    & $compilerPath -std=c99 -O2 -Wall -Wextra -Werror -DACK_COMPARE_PROFILE=1 `
        -IApp -IDevice -IDriver -I../ack_compare/Driver -I../test/Driver `
        tests/test_flow.c Driver/f103_i2c_adapter.c Device/ldc1614.c App/f103_flow.c `
        -o MDK-ARM/HostTests/f103_flow.exe
    if ($LASTEXITCODE -ne 0) { throw 'Flow test compilation failed' }
    foreach ($scenario in @('ok','wrong_id','wake_reset','commit_nack','init_bus','snapshot_nack') + (0..14)) {
        & .\MDK-ARM\HostTests\f103_flow.exe "$scenario"
        if ($LASTEXITCODE -ne 0) { throw "Flow test failed: $scenario" }
    }
} finally { Pop-Location }
