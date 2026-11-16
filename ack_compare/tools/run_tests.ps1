$ErrorActionPreference = 'Stop'
$testRoot = Split-Path -Parent $PSScriptRoot
$compilerPath = 'D:\mingw64\bin\gcc.exe'
if (-not (Test-Path -LiteralPath $compilerPath)) {
    $compilerPath = (Get-Command gcc -ErrorAction Stop).Source
}
Push-Location -LiteralPath $testRoot
try {
    New-Item -ItemType Directory -Force -Path 'MDK-ARM\HostTests' | Out-Null
    [xml]$project = Get-Content -Raw 'MDK-ARM\LDC1614_ACK_Compare.uvprojx'
    $targets = @($project.Project.Targets.Target)
    if ($targets.Count -ne 2 -or $targets[0].TargetName -ne 'LDC1614_ACK_B_OD3') {
        throw 'Expected two targets, B first'
    }
    foreach ($target in $targets) {
        $sources = @($target.Groups.Group.Files.File |
            Where-Object { $_.FileType -in '1','2' } | ForEach-Object { $_.FilePath })
        $expected = @('../../Startup/startup_stm32c011xx.s', '../../CMSIS/system_stm32c0xx.c',
            '../App/main.c', '../App/wake_diag.c', '../Driver/wake_i2c.c',
            '../Driver/ack_port.c', '../../test/Driver/test_board.c')
        if (Compare-Object $sources $expected) { throw 'Unexpected Keil source list' }
        foreach ($source in $sources) {
            if (-not (Test-Path -LiteralPath (Join-Path 'MDK-ARM' $source))) {
                throw "Missing source: $source"
            }
        }
    }
    [xml]$edgeProject = Get-Content -Raw 'MDK-ARM\LDC1614_ACK_Edge.uvprojx'
    $edgeTargets = @($edgeProject.Project.Targets.Target)
    if ($edgeTargets.Count -ne 1 -or $edgeTargets[0].TargetName -ne 'LDC1614_ACK_C_EDGE' -or
        $edgeTargets[0].TargetOption.TargetArmAds.Cads.VariousControls.Define -notmatch 'ACK_COMPARE_PROFILE=2,') {
        throw 'Expected separate C early-edge target'
    }
    $edgeSources = @($edgeTargets[0].Groups.Group.Files.File |
        Where-Object { $_.FileType -in '1','2' } | ForEach-Object { $_.FilePath })
    if (Compare-Object $edgeSources $expected) { throw 'Unexpected C source list' }
    foreach ($profile in 0,1,2) {
        $testExe = "MDK-ARM/HostTests/ack_$profile.exe"
        & $compilerPath -std=c99 -O2 -Wall -Wextra -Werror "-DACK_COMPARE_PROFILE=$profile" -IApp -IDriver -I../test/Driver `
            tests/test_wake.c Driver/wake_i2c.c -o $testExe
        if ($LASTEXITCODE -ne 0) { throw "Host compilation failed: $LASTEXITCODE" }
        & ".\$testExe"
        if ($LASTEXITCODE -ne 0) { throw "Host test failed: $LASTEXITCODE" }
    }
    foreach ($profile in 1,2) {
        $portExe = "MDK-ARM/HostTests/ack_port_$profile.exe"
        & $compilerPath -std=c99 -O2 -Wall -Wextra -Werror "-DACK_COMPARE_PROFILE=$profile" -Itests/port_stub -IDriver -I../test/Driver `
            tests/test_port.c Driver/ack_port.c -o $portExe
        if ($LASTEXITCODE -ne 0) { throw "Port compilation failed: $LASTEXITCODE" }
        & ".\$portExe"
        if ($LASTEXITCODE -ne 0) { throw "Port test failed: $LASTEXITCODE" }
    }
    $f103 = Get-Content -Raw '..\..\new\Device\ldc1614.c'
    if ($f103 -notmatch 'v &= \(uint16_t\)~\(LDC1614_CONFIG_SLEEP_MODE \| LDC1614_CONFIG_REF_CLK_SRC\)' -or
        $f103 -notmatch 'v \|= LDC1614_CONFIG_REF_CLK_SRC') {
        throw 'F103 CONFIG arithmetic changed; review comparison baseline'
    }
} finally {
    Pop-Location
}
