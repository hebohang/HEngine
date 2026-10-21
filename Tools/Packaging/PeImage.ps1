# Shared bounded PE parsing for packaging and Editor dependency audits.
function Assert-ByteRange {
    param(
        [byte[]]$Bytes,
        [long]$Offset,
        [long]$Count,
        [string]$Description
    )

    if ($Offset -lt 0 -or $Count -lt 0 -or
        $Offset -gt $Bytes.LongLength -or
        $Count -gt ($Bytes.LongLength - $Offset)) {
        throw "Invalid PE $Description range: offset=$Offset count=$Count size=$($Bytes.LongLength)"
    }
}

function Read-UInt16 {
    param([byte[]]$Bytes, [long]$Offset)
    Assert-ByteRange -Bytes $Bytes -Offset $Offset -Count 2 -Description 'UInt16'
    return [BitConverter]::ToUInt16($Bytes, [int]$Offset)
}

function Read-UInt32 {
    param([byte[]]$Bytes, [long]$Offset)
    Assert-ByteRange -Bytes $Bytes -Offset $Offset -Count 4 -Description 'UInt32'
    return [BitConverter]::ToUInt32($Bytes, [int]$Offset)
}

function Read-UInt64 {
    param([byte[]]$Bytes, [long]$Offset)
    Assert-ByteRange -Bytes $Bytes -Offset $Offset -Count 8 -Description 'UInt64'
    return [BitConverter]::ToUInt64($Bytes, [int]$Offset)
}

function Read-AsciiString {
    param(
        [byte[]]$Bytes,
        [long]$Offset,
        [int]$MaximumLength = 4096
    )

    Assert-ByteRange -Bytes $Bytes -Offset $Offset -Count 1 -Description 'string'
    $characters = New-Object System.Collections.Generic.List[byte]
    for ($index = 0; $index -lt $MaximumLength; ++$index) {
        Assert-ByteRange -Bytes $Bytes -Offset ($Offset + $index) -Count 1 -Description 'string'
        $value = $Bytes[$Offset + $index]
        if ($value -eq 0) {
            return [Text.Encoding]::ASCII.GetString($characters.ToArray())
        }
        [void]$characters.Add($value)
    }
    throw "Unterminated PE string at file offset $Offset"
}

function Get-PeImage {
    param([string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.LongLength -lt 64 -or
        $bytes[0] -ne [byte][char]'M' -or
        $bytes[1] -ne [byte][char]'Z') {
        throw "Not a PE image: $Path"
    }

    $peOffset = [long](Read-UInt32 -Bytes $bytes -Offset 0x3c)
    if ((Read-UInt32 -Bytes $bytes -Offset $peOffset) -ne 0x00004550) {
        throw "Invalid PE signature: $Path"
    }

    $coffOffset = $peOffset + 4
    $machine = Read-UInt16 -Bytes $bytes -Offset $coffOffset
    $sectionCount = Read-UInt16 -Bytes $bytes -Offset ($coffOffset + 2)
    $optionalHeaderSize = Read-UInt16 -Bytes $bytes -Offset ($coffOffset + 16)
    $optionalOffset = $coffOffset + 20
    $optionalMagic = Read-UInt16 -Bytes $bytes -Offset $optionalOffset
    if ($optionalMagic -eq 0x20b) {
        $imageBase = Read-UInt64 -Bytes $bytes -Offset ($optionalOffset + 24)
        $directoryCountOffset = $optionalOffset + 108
        $directoryOffset = $optionalOffset + 112
    }
    elseif ($optionalMagic -eq 0x10b) {
        $imageBase = [uint64](Read-UInt32 -Bytes $bytes -Offset ($optionalOffset + 28))
        $directoryCountOffset = $optionalOffset + 92
        $directoryOffset = $optionalOffset + 96
    }
    else {
        throw ('Unsupported PE optional-header magic 0x{0:x}: {1}' -f $optionalMagic, $Path)
    }

    $directoryCount = Read-UInt32 -Bytes $bytes -Offset $directoryCountOffset
    $sizeOfHeaders = Read-UInt32 -Bytes $bytes -Offset ($optionalOffset + 60)
    $sectionTableOffset = $optionalOffset + $optionalHeaderSize
    $sections = New-Object System.Collections.Generic.List[object]
    for ($sectionIndex = 0; $sectionIndex -lt $sectionCount; ++$sectionIndex) {
        $sectionOffset = $sectionTableOffset + ($sectionIndex * 40)
        Assert-ByteRange -Bytes $bytes -Offset $sectionOffset -Count 40 -Description 'section header'
        $nameBytes = $bytes[$sectionOffset..($sectionOffset + 7)]
        $nameLength = [Array]::IndexOf($nameBytes, [byte]0)
        if ($nameLength -lt 0) { $nameLength = 8 }
        $name = [Text.Encoding]::ASCII.GetString($nameBytes, 0, $nameLength)
        [void]$sections.Add([pscustomobject]@{
            Name = $name
            VirtualSize = Read-UInt32 -Bytes $bytes -Offset ($sectionOffset + 8)
            VirtualAddress = Read-UInt32 -Bytes $bytes -Offset ($sectionOffset + 12)
            RawSize = Read-UInt32 -Bytes $bytes -Offset ($sectionOffset + 16)
            RawOffset = Read-UInt32 -Bytes $bytes -Offset ($sectionOffset + 20)
        })
    }

    return [pscustomobject]@{
        Path = $Path
        Bytes = $bytes
        Machine = $machine
        OptionalMagic = $optionalMagic
        ImageBase = $imageBase
        DirectoryCount = $directoryCount
        DirectoryOffset = $directoryOffset
        SizeOfHeaders = $sizeOfHeaders
        Sections = $sections.ToArray()
    }
}

function Convert-RvaToFileOffset {
    param(
        [object]$Image,
        [uint32]$Rva
    )

    if ($Rva -lt $Image.SizeOfHeaders) {
        Assert-ByteRange -Bytes $Image.Bytes -Offset $Rva -Count 1 -Description 'header RVA'
        return [long]$Rva
    }

    foreach ($section in $Image.Sections) {
        $mappedSize = [Math]::Max([uint64]$section.VirtualSize, [uint64]$section.RawSize)
        $start = [uint64]$section.VirtualAddress
        $end = $start + $mappedSize
        if ([uint64]$Rva -ge $start -and [uint64]$Rva -lt $end) {
            $offset = [uint64]$section.RawOffset + ([uint64]$Rva - $start)
            Assert-ByteRange -Bytes $Image.Bytes -Offset $offset -Count 1 -Description 'section RVA'
            return [long]$offset
        }
    }
    throw ('RVA 0x{0:x} is outside the PE sections: {1}' -f $Rva, $Image.Path)
}

function Get-PeDataDirectory {
    param(
        [object]$Image,
        [int]$Index
    )

    if ($Index -lt 0 -or $Index -ge $Image.DirectoryCount) {
        return $null
    }
    $entryOffset = $Image.DirectoryOffset + ($Index * 8)
    $rva = Read-UInt32 -Bytes $Image.Bytes -Offset $entryOffset
    $size = Read-UInt32 -Bytes $Image.Bytes -Offset ($entryOffset + 4)
    if ($rva -eq 0 -or $size -eq 0) {
        return $null
    }
    return [pscustomobject]@{ Rva = $rva; Size = $size }
}

function Get-PeNormalImports {
    param([object]$Image)

    $imports = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $importDirectory = Get-PeDataDirectory -Image $Image -Index 1
    if ($null -ne $importDirectory) {
        $descriptorOffset = Convert-RvaToFileOffset -Image $Image -Rva $importDirectory.Rva
        for ($descriptorIndex = 0; $descriptorIndex -lt 4096; ++$descriptorIndex) {
            $currentOffset = $descriptorOffset + ($descriptorIndex * 20)
            Assert-ByteRange -Bytes $Image.Bytes -Offset $currentOffset -Count 20 -Description 'import descriptor'
            $originalThunk = Read-UInt32 -Bytes $Image.Bytes -Offset $currentOffset
            $timeDateStamp = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 4)
            $forwarderChain = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 8)
            $nameRva = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 12)
            $firstThunk = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 16)
            if (($originalThunk -bor $timeDateStamp -bor $forwarderChain -bor $nameRva -bor $firstThunk) -eq 0) {
                break
            }
            if ($nameRva -eq 0) {
                throw "Import descriptor has no DLL name: $($Image.Path)"
            }
            $nameOffset = Convert-RvaToFileOffset -Image $Image -Rva $nameRva
            [void]$imports.Add((Read-AsciiString -Bytes $Image.Bytes -Offset $nameOffset))
        }
    }

    return @($imports | Sort-Object)
}

function Get-PeDelayImports {
    param([object]$Image)

    $imports = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $delayDirectory = Get-PeDataDirectory -Image $Image -Index 13
    if ($null -ne $delayDirectory) {
        $descriptorOffset = Convert-RvaToFileOffset -Image $Image -Rva $delayDirectory.Rva
        for ($descriptorIndex = 0; $descriptorIndex -lt 4096; ++$descriptorIndex) {
            $currentOffset = $descriptorOffset + ($descriptorIndex * 32)
            Assert-ByteRange -Bytes $Image.Bytes -Offset $currentOffset -Count 32 -Description 'delay import descriptor'
            $attributes = Read-UInt32 -Bytes $Image.Bytes -Offset $currentOffset
            $nameValue = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 4)
            $moduleHandle = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 8)
            if (($attributes -bor $nameValue -bor $moduleHandle) -eq 0) {
                break
            }
            if (($attributes -band 1) -eq 0) {
                throw "Legacy VA-based delay imports are not supported: $($Image.Path)"
            }
            $nameOffset = Convert-RvaToFileOffset -Image $Image -Rva $nameValue
            [void]$imports.Add((Read-AsciiString -Bytes $Image.Bytes -Offset $nameOffset))
        }
    }

    return @($imports | Sort-Object)
}

function Get-PeImports {
    param([object]$Image)

    $imports = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in @(Get-PeNormalImports -Image $Image)) {
        [void]$imports.Add($name)
    }
    foreach ($name in @(Get-PeDelayImports -Image $Image)) {
        [void]$imports.Add($name)
    }
    return @($imports | Sort-Object)
}

function Get-PeExports {
    param([object]$Image)

    $exports = @{}
    $exportDirectory = Get-PeDataDirectory -Image $Image -Index 0
    if ($null -eq $exportDirectory) {
        return $exports
    }

    $directoryOffset = Convert-RvaToFileOffset -Image $Image -Rva $exportDirectory.Rva
    Assert-ByteRange -Bytes $Image.Bytes -Offset $directoryOffset -Count 40 -Description 'export directory'
    $functionCount = Read-UInt32 -Bytes $Image.Bytes -Offset ($directoryOffset + 20)
    $nameCount = Read-UInt32 -Bytes $Image.Bytes -Offset ($directoryOffset + 24)
    $functionsRva = Read-UInt32 -Bytes $Image.Bytes -Offset ($directoryOffset + 28)
    $namesRva = Read-UInt32 -Bytes $Image.Bytes -Offset ($directoryOffset + 32)
    $ordinalsRva = Read-UInt32 -Bytes $Image.Bytes -Offset ($directoryOffset + 36)
    if ($nameCount -gt $functionCount -or $nameCount -gt 100000) {
        throw "Invalid export counts: $($Image.Path)"
    }

    $functionsOffset = Convert-RvaToFileOffset -Image $Image -Rva $functionsRva
    $namesOffset = Convert-RvaToFileOffset -Image $Image -Rva $namesRva
    $ordinalsOffset = Convert-RvaToFileOffset -Image $Image -Rva $ordinalsRva
    for ($nameIndex = 0; $nameIndex -lt $nameCount; ++$nameIndex) {
        $nameRva = Read-UInt32 -Bytes $Image.Bytes -Offset ($namesOffset + ($nameIndex * 4))
        $nameOffset = Convert-RvaToFileOffset -Image $Image -Rva $nameRva
        $name = Read-AsciiString -Bytes $Image.Bytes -Offset $nameOffset
        $ordinalIndex = Read-UInt16 -Bytes $Image.Bytes -Offset ($ordinalsOffset + ($nameIndex * 2))
        if ($ordinalIndex -ge $functionCount) {
            throw "Invalid export ordinal for ${name}: $($Image.Path)"
        }
        $exports[$name] = Read-UInt32 -Bytes $Image.Bytes -Offset ($functionsOffset + ($ordinalIndex * 4))
    }
    return $exports
}

function Read-ExportedUInt32 {
    param([object]$Image, [hashtable]$Exports, [string]$Name)
    if (-not $Exports.ContainsKey($Name)) {
        throw "Required data export '$Name' is missing: $($Image.Path)"
    }
    $offset = Convert-RvaToFileOffset -Image $Image -Rva $Exports[$Name]
    return Read-UInt32 -Bytes $Image.Bytes -Offset $offset
}

function Read-ExportedStringPointer {
    param([object]$Image, [hashtable]$Exports, [string]$Name)
    if (-not $Exports.ContainsKey($Name)) {
        throw "Required data export '$Name' is missing: $($Image.Path)"
    }
    if ($Image.OptionalMagic -ne 0x20b) {
        throw "The Agility SDK export verifier requires a 64-bit PE image: $($Image.Path)"
    }
    $exportOffset = Convert-RvaToFileOffset -Image $Image -Rva $Exports[$Name]
    $pointerValue = Read-UInt64 -Bytes $Image.Bytes -Offset $exportOffset
    if ($pointerValue -ge $Image.ImageBase) {
        $stringRva64 = $pointerValue - $Image.ImageBase
    }
    else {
        # Accept an RVA-form pointer for toolchains that emit one before fixups.
        $stringRva64 = $pointerValue
    }
    if ($stringRva64 -gt [uint32]::MaxValue) {
        throw "Export '$Name' points outside the PE image: $($Image.Path)"
    }
    $stringOffset = Convert-RvaToFileOffset -Image $Image -Rva ([uint32]$stringRva64)
    return Read-AsciiString -Bytes $Image.Bytes -Offset $stringOffset
}
