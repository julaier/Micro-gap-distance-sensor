$ErrorActionPreference = 'Stop'
$testRoot = Split-Path -Parent $PSScriptRoot
foreach ($profile in 'A','B','C') {
    $name = switch ($profile) {
        A { 'LDC1614_ACK_A_LEGACY' }
        B { 'LDC1614_ACK_B_OD3' }
        C { 'LDC1614_ACK_C_EDGE' }
    }
    $marker = switch ($profile) {
        A { 'A_LEGACY_SINGLE' }
        B { 'B_OD3_INDEPENDENT' }
        C { 'C_EDGE_EARLY' }
    }
    $path = Join-Path $testRoot "MDK-ARM\Objects_$profile\$name.hex"
    $data = [System.Collections.Generic.SortedDictionary[uint32,byte]]::new()
    [uint32]$baseAddress = 0
    $eof = $false
    foreach ($line in [System.IO.File]::ReadAllLines($path)) {
        if ($eof) { throw 'Data after EOF' }
        if ($line -notmatch '^:([0-9A-Fa-f]{2})+$') { throw 'Invalid HEX syntax' }
        $record = [byte[]]::new(($line.Length - 1) / 2)
        $sum = 0
        for ($i = 0; $i -lt $record.Length; ++$i) {
            $record[$i] = [Convert]::ToByte($line.Substring(1 + 2 * $i, 2), 16)
            $sum += $record[$i]
        }
        $length = [int]$record[0]
        if (($sum -band 255) -ne 0 -or $record.Length -ne $length + 5) {
            throw 'HEX checksum/length error'
        }
        $offset = [int]$record[1] * 256 + $record[2]
        switch ($record[3]) {
            0 {
                for ($i = 0; $i -lt $length; ++$i) {
                    [uint32]$address = $baseAddress + $offset + $i
                    if ($address -lt 0x08000000 -or $address -ge 0x08007800) {
                        throw 'HEX outside reserved application region'
                    }
                    if ($data.ContainsKey($address)) { throw 'Overlapping HEX bytes' }
                    $data.Add($address, $record[4 + $i])
                }
            }
            1 {
                if ($length -ne 0 -or $offset -ne 0) { throw 'Invalid EOF' }
                $eof = $true
            }
            4 {
                if ($length -ne 2 -or $offset -ne 0) { throw 'Invalid extended address' }
                $baseAddress = ([uint32]$record[4] * 256 + $record[5]) * 65536
            }
            5 { if ($length -ne 4 -or $offset -ne 0) { throw 'Invalid entry record' } }
            default { throw 'Unexpected HEX record type' }
        }
    }
    if (-not $eof -or $data.Count -eq 0) { throw 'Missing EOF or firmware data' }
    $vector = [byte[]]::new(8)
    for ($i = 0; $i -lt 8; ++$i) {
        [uint32]$address = 0x08000000 + $i
        if (-not $data.ContainsKey($address)) { throw 'Missing vector table' }
        $vector[$i] = $data[$address]
    }
    $sp = [BitConverter]::ToUInt32($vector, 0)
    $reset = [BitConverter]::ToUInt32($vector, 4)
    if ($sp -le 0x20000000 -or $sp -gt 0x20001800 -or ($sp -band 7) -ne 0) {
        throw 'Invalid initial stack pointer'
    }
    if (($reset -band 1) -ne 1 -or -not $data.ContainsKey([uint32]($reset - 1))) {
        throw 'Invalid reset vector'
    }
    $ascii = [System.Text.Encoding]::ASCII.GetString([byte[]]@($data.Values))
    $firmware = if ($profile -eq 'C') { 'C011_LDC_ACK_EDGE_V7_F103' } else { 'C011_LDC_ACK_COMPARE_V6_F103' }
    foreach ($text in $firmware, $marker, 'F103_ORIGINAL,SLEEP,2A01,WAKE,0A01') {
        if (-not $ascii.Contains($text)) { throw "Missing firmware marker: $text" }
    }
    foreach ($otherMarker in 'A_LEGACY_SINGLE','B_OD3_INDEPENDENT','C_EDGE_EARLY') {
        if ($otherMarker -ne $marker -and $ascii.Contains($otherMarker)) {
            throw 'Conflicting profile markers'
        }
    }
    $endAddress = [uint32](@($data.Keys)[-1] + 1)
    Write-Output ('PASS: {0}; {1} data bytes, end=0x{2:X8}, SP=0x{3:X8}, SHA256={4}' -f
        $name, $data.Count, $endAddress, $sp, (Get-FileHash -LiteralPath $path).Hash)
}
