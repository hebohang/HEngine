[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Report,

    [string]$Why = '',
    [string]$Referrers = '',
    [string]$Without = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:MaximumReportBytes = 512MB
$script:TopContributorCount = 10
$script:OrdinalIgnoreCase = [StringComparer]::OrdinalIgnoreCase
$script:Ordinal = [StringComparer]::Ordinal
$script:JsonText = $null
$script:JsonOffset = 0
$script:JsonLength = 0

function Throw-JsonSyntaxError {
    param([string]$Message)

    throw "CookReport JSON syntax error at character $script:JsonOffset: $Message"
}

function Skip-JsonWhitespace {
    while ($script:JsonOffset -lt $script:JsonLength) {
        $character = $script:JsonText[$script:JsonOffset]
        if ($character -ne ' ' -and $character -ne "`t" -and
            $character -ne "`r" -and $character -ne "`n") {
            break
        }
        ++$script:JsonOffset
    }
}

function Read-JsonHexQuad {
    if ($script:JsonOffset + 4 -gt $script:JsonLength) {
        Throw-JsonSyntaxError -Message 'incomplete Unicode escape.'
    }
    $value = 0
    for ($index = 0; $index -lt 4; ++$index) {
        $character = $script:JsonText[$script:JsonOffset++]
        if ($character -ge '0' -and $character -le '9') {
            $digit = [int]$character - [int][char]'0'
        }
        elseif ($character -ge 'a' -and $character -le 'f') {
            $digit = 10 + [int]$character - [int][char]'a'
        }
        elseif ($character -ge 'A' -and $character -le 'F') {
            $digit = 10 + [int]$character - [int][char]'A'
        }
        else {
            Throw-JsonSyntaxError -Message 'invalid Unicode escape.'
        }
        $value = ($value * 16) + $digit
    }
    return $value
}

function Read-StrictJsonString {
    param([switch]$Decode)

    if ($script:JsonOffset -ge $script:JsonLength -or
        $script:JsonText[$script:JsonOffset] -ne '"') {
        Throw-JsonSyntaxError -Message 'expected a JSON string.'
    }
    ++$script:JsonOffset
    $builder = if ($Decode) { New-Object Text.StringBuilder } else { $null }
    while ($script:JsonOffset -lt $script:JsonLength) {
        $character = $script:JsonText[$script:JsonOffset++]
        if ($character -eq '"') {
            if ($Decode) {
                return $builder.ToString()
            }
            return
        }
        if ([int]$character -lt 32) {
            Throw-JsonSyntaxError -Message 'unescaped control character in string.'
        }
        if ($character -ne '\') {
            if ($Decode) {
                [void]$builder.Append($character)
            }
            continue
        }

        if ($script:JsonOffset -ge $script:JsonLength) {
            Throw-JsonSyntaxError -Message 'incomplete string escape.'
        }
        $escape = $script:JsonText[$script:JsonOffset++]
        switch ($escape) {
            '"' { if ($Decode) { [void]$builder.Append('"') } }
            '\' { if ($Decode) { [void]$builder.Append('\') } }
            '/' { if ($Decode) { [void]$builder.Append('/') } }
            'b' { if ($Decode) { [void]$builder.Append([char]0x08) } }
            'f' { if ($Decode) { [void]$builder.Append([char]0x0C) } }
            'n' { if ($Decode) { [void]$builder.Append([char]0x0A) } }
            'r' { if ($Decode) { [void]$builder.Append([char]0x0D) } }
            't' { if ($Decode) { [void]$builder.Append([char]0x09) } }
            'u' {
                $codeUnit = Read-JsonHexQuad
                if ($codeUnit -ge 0xD800 -and $codeUnit -le 0xDBFF) {
                    if ($script:JsonOffset + 6 -gt $script:JsonLength -or
                        $script:JsonText[$script:JsonOffset] -ne '\' -or
                        $script:JsonText[$script:JsonOffset + 1] -ne 'u') {
                        Throw-JsonSyntaxError -Message 'high surrogate is not followed by a low surrogate.'
                    }
                    $script:JsonOffset += 2
                    $lowSurrogate = Read-JsonHexQuad
                    if ($lowSurrogate -lt 0xDC00 -or $lowSurrogate -gt 0xDFFF) {
                        Throw-JsonSyntaxError -Message 'high surrogate is not followed by a low surrogate.'
                    }
                    if ($Decode) {
                        [void]$builder.Append([char]$codeUnit)
                        [void]$builder.Append([char]$lowSurrogate)
                    }
                }
                elseif ($codeUnit -ge 0xDC00 -and $codeUnit -le 0xDFFF) {
                    Throw-JsonSyntaxError -Message 'unpaired low surrogate.'
                }
                elseif ($Decode) {
                    [void]$builder.Append([char]$codeUnit)
                }
            }
            default { Throw-JsonSyntaxError -Message "invalid string escape '\$escape'." }
        }
    }
    Throw-JsonSyntaxError -Message 'unterminated JSON string.'
}

function Read-StrictJsonNumber {
    if ($script:JsonText[$script:JsonOffset] -eq '-') {
        ++$script:JsonOffset
        if ($script:JsonOffset -ge $script:JsonLength) {
            Throw-JsonSyntaxError -Message 'incomplete number.'
        }
    }

    if ($script:JsonText[$script:JsonOffset] -eq '0') {
        ++$script:JsonOffset
        if ($script:JsonOffset -lt $script:JsonLength -and
            $script:JsonText[$script:JsonOffset] -ge '0' -and
            $script:JsonText[$script:JsonOffset] -le '9') {
            Throw-JsonSyntaxError -Message 'leading zero in number.'
        }
    }
    elseif ($script:JsonText[$script:JsonOffset] -ge '1' -and
        $script:JsonText[$script:JsonOffset] -le '9') {
        do {
            ++$script:JsonOffset
        } while ($script:JsonOffset -lt $script:JsonLength -and
            $script:JsonText[$script:JsonOffset] -ge '0' -and
            $script:JsonText[$script:JsonOffset] -le '9')
    }
    else {
        Throw-JsonSyntaxError -Message 'invalid number.'
    }

    if ($script:JsonOffset -lt $script:JsonLength -and
        $script:JsonText[$script:JsonOffset] -eq '.') {
        ++$script:JsonOffset
        $fractionStart = $script:JsonOffset
        while ($script:JsonOffset -lt $script:JsonLength -and
            $script:JsonText[$script:JsonOffset] -ge '0' -and
            $script:JsonText[$script:JsonOffset] -le '9') {
            ++$script:JsonOffset
        }
        if ($script:JsonOffset -eq $fractionStart) {
            Throw-JsonSyntaxError -Message 'fraction requires a digit.'
        }
    }

    if ($script:JsonOffset -lt $script:JsonLength -and
        ($script:JsonText[$script:JsonOffset] -eq 'e' -or
            $script:JsonText[$script:JsonOffset] -eq 'E')) {
        ++$script:JsonOffset
        if ($script:JsonOffset -lt $script:JsonLength -and
            ($script:JsonText[$script:JsonOffset] -eq '+' -or
                $script:JsonText[$script:JsonOffset] -eq '-')) {
            ++$script:JsonOffset
        }
        $exponentStart = $script:JsonOffset
        while ($script:JsonOffset -lt $script:JsonLength -and
            $script:JsonText[$script:JsonOffset] -ge '0' -and
            $script:JsonText[$script:JsonOffset] -le '9') {
            ++$script:JsonOffset
        }
        if ($script:JsonOffset -eq $exponentStart) {
            Throw-JsonSyntaxError -Message 'exponent requires a digit.'
        }
    }
}

function Read-StrictJsonValue {
    param([int]$Depth)

    if ($Depth -gt 256) {
        Throw-JsonSyntaxError -Message 'nesting exceeds 256 levels.'
    }
    Skip-JsonWhitespace
    if ($script:JsonOffset -ge $script:JsonLength) {
        Throw-JsonSyntaxError -Message 'expected a JSON value.'
    }

    $character = $script:JsonText[$script:JsonOffset]
    if ($character -eq '{') {
        ++$script:JsonOffset
        Skip-JsonWhitespace
        $exactNames = New-Object 'System.Collections.Generic.HashSet[string]' ($script:Ordinal)
        $foldedNames = New-Object 'System.Collections.Generic.HashSet[string]' ($script:OrdinalIgnoreCase)
        if ($script:JsonOffset -lt $script:JsonLength -and
            $script:JsonText[$script:JsonOffset] -eq '}') {
            ++$script:JsonOffset
            return
        }
        while ($true) {
            $propertyName = Read-StrictJsonString -Decode
            if (-not $exactNames.Add($propertyName)) {
                Throw-JsonSyntaxError -Message "duplicate object property '$propertyName'."
            }
            if (-not $foldedNames.Add($propertyName)) {
                Throw-JsonSyntaxError -Message "case-ambiguous object property '$propertyName'."
            }
            Skip-JsonWhitespace
            if ($script:JsonOffset -ge $script:JsonLength -or
                $script:JsonText[$script:JsonOffset] -ne ':') {
                Throw-JsonSyntaxError -Message "expected ':' after object property."
            }
            ++$script:JsonOffset
            Read-StrictJsonValue -Depth ($Depth + 1)
            Skip-JsonWhitespace
            if ($script:JsonOffset -ge $script:JsonLength) {
                Throw-JsonSyntaxError -Message 'unterminated object.'
            }
            if ($script:JsonText[$script:JsonOffset] -eq '}') {
                ++$script:JsonOffset
                return
            }
            if ($script:JsonText[$script:JsonOffset] -ne ',') {
                Throw-JsonSyntaxError -Message "expected ',' or '}' in object."
            }
            ++$script:JsonOffset
            Skip-JsonWhitespace
            if ($script:JsonOffset -ge $script:JsonLength -or
                $script:JsonText[$script:JsonOffset] -ne '"') {
                Throw-JsonSyntaxError -Message 'object comma must be followed by a property.'
            }
        }
    }
    if ($character -eq '[') {
        ++$script:JsonOffset
        Skip-JsonWhitespace
        if ($script:JsonOffset -lt $script:JsonLength -and
            $script:JsonText[$script:JsonOffset] -eq ']') {
            ++$script:JsonOffset
            return
        }
        while ($true) {
            Read-StrictJsonValue -Depth ($Depth + 1)
            Skip-JsonWhitespace
            if ($script:JsonOffset -ge $script:JsonLength) {
                Throw-JsonSyntaxError -Message 'unterminated array.'
            }
            if ($script:JsonText[$script:JsonOffset] -eq ']') {
                ++$script:JsonOffset
                return
            }
            if ($script:JsonText[$script:JsonOffset] -ne ',') {
                Throw-JsonSyntaxError -Message "expected ',' or ']' in array."
            }
            ++$script:JsonOffset
            Skip-JsonWhitespace
            if ($script:JsonOffset -lt $script:JsonLength -and
                $script:JsonText[$script:JsonOffset] -eq ']') {
                Throw-JsonSyntaxError -Message 'trailing comma in array.'
            }
        }
    }
    if ($character -eq '"') {
        Read-StrictJsonString
        return
    }
    if ($character -eq '-' -or ($character -ge '0' -and $character -le '9')) {
        Read-StrictJsonNumber
        return
    }
    foreach ($literal in @('true', 'false', 'null')) {
        if ($script:JsonOffset + $literal.Length -le $script:JsonLength -and
            $script:JsonText.Substring($script:JsonOffset, $literal.Length) -ceq $literal) {
            $script:JsonOffset += $literal.Length
            return
        }
    }
    Throw-JsonSyntaxError -Message 'invalid JSON value.'
}

function Assert-StrictJsonText {
    param([string]$Value)

    $script:JsonText = $Value
    $script:JsonOffset = 0
    $script:JsonLength = $Value.Length
    try {
        Read-StrictJsonValue -Depth 0
        Skip-JsonWhitespace
        if ($script:JsonOffset -ne $script:JsonLength) {
            Throw-JsonSyntaxError -Message 'unexpected content after the root value.'
        }
    }
    finally {
        $script:JsonText = $null
        $script:JsonOffset = 0
        $script:JsonLength = 0
    }
}

function Assert-JsonObject {
    param([object]$Value, [string]$Context)

    if ($null -eq $Value -or $Value -isnot [pscustomobject]) {
        throw "$Context must be a JSON object."
    }
}

function Assert-JsonArray {
    param([object]$Value, [string]$Context)

    if ($null -eq $Value -or $Value -isnot [array]) {
        throw "$Context must be a JSON array."
    }
}

function Get-ExactProperty {
    param(
        [object]$Object,
        [string]$Name,
        [string]$Context,
        [switch]$Optional
    )

    Assert-JsonObject -Value $Object -Context $Context
    $matches = @($Object.PSObject.Properties | Where-Object { $_.Name -ceq $Name })
    if ($matches.Count -eq 0) {
        if ($Optional) {
            return $null
        }
        throw "$Context is missing required property '$Name'."
    }
    if ($matches.Count -ne 1) {
        throw "$Context contains duplicate property '$Name'."
    }
    return $matches[0]
}

function Test-ExactProperty {
    param([object]$Object, [string]$Name)

    if ($null -eq $Object -or $Object -isnot [pscustomobject]) {
        return $false
    }
    return @($Object.PSObject.Properties | Where-Object { $_.Name -ceq $Name }).Count -eq 1
}

function Assert-UnambiguousPropertyNames {
    param([object]$Object, [string]$Context)

    Assert-JsonObject -Value $Object -Context $Context
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ($script:OrdinalIgnoreCase)
    foreach ($property in $Object.PSObject.Properties) {
        if (-not $seen.Add($property.Name)) {
            throw "$Context contains property names that differ only by case: '$($property.Name)'."
        }
    }
}

function Assert-NonNegativeIntegerValue {
    param([object]$Value, [string]$Context)

    $integerTypes = @(
        [byte], [sbyte], [int16], [uint16], [int32], [uint32],
        [int64], [uint64], [decimal], [double], [single]
    )
    $bNumeric = $false
    foreach ($type in $integerTypes) {
        if ($Value -is $type) {
            $bNumeric = $true
            break
        }
    }
    if (-not $bNumeric) {
        throw "$Context must be a non-negative JSON integer."
    }

    try {
        $number = [decimal]$Value
    }
    catch {
        throw "$Context is outside the supported integer range."
    }
    if ($number -lt 0 -or [decimal]::Truncate($number) -ne $number) {
        throw "$Context must be a non-negative JSON integer."
    }
}

function Get-RequiredNonNegativeInteger {
    param([object]$Object, [string]$Name, [string]$Context)

    $property = Get-ExactProperty -Object $Object -Name $Name -Context $Context
    Assert-NonNegativeIntegerValue -Value $property.Value -Context "$Context.$Name"
    return $property.Value
}

function Assert-OptionalNonNegativeInteger {
    param([object]$Object, [string]$Name, [string]$Context)

    $property = Get-ExactProperty -Object $Object -Name $Name -Context $Context -Optional
    if ($null -ne $property) {
        Assert-NonNegativeIntegerValue -Value $property.Value -Context "$Context.$Name"
    }
}

function Get-RequiredString {
    param([object]$Object, [string]$Name, [string]$Context)

    $property = Get-ExactProperty -Object $Object -Name $Name -Context $Context
    if ($property.Value -isnot [string] -or [string]::IsNullOrWhiteSpace($property.Value)) {
        throw "$Context.$Name must be a non-empty JSON string."
    }
    if ($property.Value -ne $property.Value.Trim() -or
        $property.Value.IndexOfAny(@([char]0x0D, [char]0x0A, [char]0x00)) -ge 0) {
        throw "$Context.$Name contains unsupported whitespace or control characters."
    }
    return [string]$property.Value
}

function Assert-OptionalBoolean {
    param([object]$Object, [string]$Name, [string]$Context)

    $property = Get-ExactProperty -Object $Object -Name $Name -Context $Context -Optional
    if ($null -ne $property -and $property.Value -isnot [bool]) {
        throw "$Context.$Name must be a JSON boolean."
    }
}

function Assert-OptionalArray {
    param([object]$Object, [string]$Name, [string]$Context)

    $property = Get-ExactProperty -Object $Object -Name $Name -Context $Context -Optional
    if ($null -ne $property) {
        Assert-JsonArray -Value $property.Value -Context "$Context.$Name"
    }
}

function ConvertTo-NormalizedQueryPath {
    param([string]$Value, [string]$Context)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "$Context must not be empty."
    }
    $normalized = $Value.Trim().Replace('\', '/')
    if ($normalized.Length -gt 4096 -or
        $normalized.StartsWith('/') -or
        $normalized -match '^[A-Za-z]:' -or
        $normalized.StartsWith('//')) {
        throw "$Context must be a relative logical path."
    }

    $reservedNames = '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\..*)?$'
    $segments = $normalized.Split('/')
    foreach ($segment in $segments) {
        if ([string]::IsNullOrEmpty($segment) -or
            $segment -eq '.' -or
            $segment -eq '..' -or
            $segment.Length -gt 255 -or
            $segment.EndsWith('.') -or
            $segment.EndsWith(' ') -or
            $segment -match '[<>:"|?*]' -or
            $segment -match $reservedNames) {
            throw "$Context contains an unsafe logical path segment: '$segment'."
        }
        foreach ($character in $segment.ToCharArray()) {
            if ([int]$character -lt 32) {
                throw "$Context contains a control character."
            }
        }
    }
    return $normalized
}

function Assert-CanonicalReportPath {
    param([object]$Value, [string]$Context)

    if ($Value -isnot [string]) {
        throw "$Context must be a logical-path string."
    }
    $normalized = ConvertTo-NormalizedQueryPath -Value $Value -Context $Context
    if ($normalized -cne $Value) {
        throw "$Context must use canonical forward slashes without surrounding whitespace."
    }
    return $normalized
}

function Assert-CountMap {
    param([object]$Value, [string]$Context)

    Assert-UnambiguousPropertyNames -Object $Value -Context $Context
    foreach ($property in $Value.PSObject.Properties) {
        if ([string]::IsNullOrWhiteSpace($property.Name)) {
            throw "$Context contains an empty key."
        }
        Assert-NonNegativeIntegerValue -Value $property.Value -Context "$Context.$($property.Name)"
    }
}

function Assert-DetailArray {
    param([object]$Value, [string]$Context, [switch]$Paths)

    Assert-JsonArray -Value $Value -Context $Context
    for ($index = 0; $index -lt $Value.Count; ++$index) {
        $item = $Value[$index]
        if ($item -is [string]) {
            if ($Paths) {
                [void](Assert-CanonicalReportPath -Value $item -Context "$Context[$index]")
            }
            continue
        }
        if ($null -eq $item -or $item -isnot [pscustomobject]) {
            throw "$Context[$index] must be a string or JSON object."
        }
        Assert-UnambiguousPropertyNames -Object $item -Context "$Context[$index]"
    }
}

function Assert-RecordArray {
    param([object]$ReportObject, [string]$Name)

    $property = Get-ExactProperty -Object $ReportObject -Name $Name -Context 'CookReport' -Optional
    if ($null -eq $property) {
        return
    }
    Assert-JsonArray -Value $property.Value -Context "CookReport.$Name"
    for ($index = 0; $index -lt $property.Value.Count; ++$index) {
        $item = $property.Value[$index]
        Assert-UnambiguousPropertyNames -Object $item -Context "CookReport.$Name[$index]"
    }
}

function Assert-OptionalString {
    param([object]$Object, [string]$Name, [string]$Context, [switch]$AllowEmpty)

    $property = Get-ExactProperty -Object $Object -Name $Name -Context $Context -Optional
    if ($null -eq $property) {
        return
    }
    if ($property.Value -isnot [string] -or
        (-not $AllowEmpty -and [string]::IsNullOrWhiteSpace($property.Value))) {
        throw "$Context.$Name must be a JSON string$($(if ($AllowEmpty) { '' } else { ' with content' }))."
    }
    if ($property.Value.IndexOfAny(@([char]0x0D, [char]0x0A, [char]0x00)) -ge 0) {
        throw "$Context.$Name contains unsupported control characters."
    }
}

function Assert-ByteSummary {
    param([object]$Value, [string]$Context)

    Assert-UnambiguousPropertyNames -Object $Value -Context $Context
    foreach ($name in @('FileCount', 'SourceBytes', 'CookedBytes', 'CompressedBytes')) {
        Assert-OptionalNonNegativeInteger -Object $Value -Name $name -Context $Context
    }
}

function Assert-ContributionRecords {
    param([object]$ReportObject, [string]$Name)

    $property = Get-ExactProperty -Object $ReportObject -Name $Name -Context 'CookReport' -Optional
    if ($null -eq $property) {
        return
    }
    Assert-JsonArray -Value $property.Value -Context "CookReport.$Name"
    $ids = New-Object 'System.Collections.Generic.HashSet[string]' ($script:OrdinalIgnoreCase)
    for ($index = 0; $index -lt $property.Value.Count; ++$index) {
        $item = $property.Value[$index]
        $context = "CookReport.$Name[$index]"
        Assert-UnambiguousPropertyNames -Object $item -Context $context
        $idProperty = Get-ExactProperty -Object $item -Name 'Id' -Context $context -Optional
        if ($null -ne $idProperty) {
            if ($idProperty.Value -isnot [string] -or [string]::IsNullOrWhiteSpace($idProperty.Value)) {
                throw "$context.Id must be a non-empty JSON string."
            }
            if (-not $ids.Add($idProperty.Value)) {
                throw "CookReport.$Name contains a duplicate or case-ambiguous Id '$($idProperty.Value)'."
            }
        }
        foreach ($stringName in @('SeedSource', 'Origin', 'Name')) {
            Assert-OptionalString -Object $item -Name $stringName -Context $context
        }
        Assert-OptionalString -Object $item -Name 'Group' -Context $context -AllowEmpty
        Assert-OptionalNonNegativeInteger -Object $item -Name 'SeedCount' -Context $context
        foreach ($summaryName in @('Reachable', 'Exclusive')) {
            $summaryProperty = Get-ExactProperty -Object $item -Name $summaryName -Context $context -Optional
            if ($null -ne $summaryProperty) {
                Assert-ByteSummary -Value $summaryProperty.Value -Context "$context.$summaryName"
            }
        }
        $simulation = Get-ExactProperty -Object $item -Name 'RemovalSimulation' -Context $context -Optional
        if ($null -ne $simulation) {
            Assert-UnambiguousPropertyNames -Object $simulation.Value -Context "$context.RemovalSimulation"
            foreach ($numberName in @('RemovedFileCount', 'SourceBytes', 'CookedBytes', 'CompressedBytes')) {
                Assert-OptionalNonNegativeInteger -Object $simulation.Value -Name $numberName -Context "$context.RemovalSimulation"
            }
            $files = Get-ExactProperty -Object $simulation.Value -Name 'Files' -Context "$context.RemovalSimulation" -Optional
            if ($null -ne $files) {
                Assert-DetailArray -Value $files.Value -Context "$context.RemovalSimulation.Files" -Paths
                $removedCount = Get-ExactProperty -Object $simulation.Value -Name 'RemovedFileCount' -Context "$context.RemovalSimulation" -Optional
                if ($null -ne $removedCount -and $files.Value.Count -ne [int64]$removedCount.Value) {
                    throw "$context.RemovalSimulation.Files count does not match RemovedFileCount."
                }
            }
        }
    }
}

function Assert-TopFileRecords {
    param([object]$ReportObject, [string]$Name)

    $property = Get-ExactProperty -Object $ReportObject -Name $Name -Context 'CookReport' -Optional
    if ($null -eq $property) {
        return
    }
    Assert-JsonArray -Value $property.Value -Context "CookReport.$Name"
    for ($index = 0; $index -lt $property.Value.Count; ++$index) {
        $item = $property.Value[$index]
        $context = "CookReport.$Name[$index]"
        Assert-UnambiguousPropertyNames -Object $item -Context $context
        $path = Get-RequiredString -Object $item -Name 'Path' -Context $context
        [void](Assert-CanonicalReportPath -Value $path -Context "$context.Path")
        [void](Get-RequiredString -Object $item -Name 'Type' -Context $context)
        [void](Get-RequiredNonNegativeInteger -Object $item -Name 'Bytes' -Context $context)
    }
}

function Assert-DuplicateContentGroups {
    param([object]$ReportObject)

    $property = Get-ExactProperty -Object $ReportObject -Name 'DuplicateContentGroups' -Context 'CookReport' -Optional
    if ($null -eq $property) {
        return
    }
    Assert-JsonArray -Value $property.Value -Context 'CookReport.DuplicateContentGroups'
    for ($index = 0; $index -lt $property.Value.Count; ++$index) {
        $item = $property.Value[$index]
        $context = "CookReport.DuplicateContentGroups[$index]"
        Assert-UnambiguousPropertyNames -Object $item -Context $context
        $hash = Get-RequiredString -Object $item -Name 'Hash' -Context $context
        if ($hash -notmatch '^[0-9A-Fa-f]{16}$') {
            throw "$context.Hash must be a 16-digit hexadecimal hash."
        }
        foreach ($name in @(
            'CookedBytesPerFile', 'PossibleSourceSavings',
            'PossibleCookedSavings', 'PossibleCompressedSavings')) {
            [void](Get-RequiredNonNegativeInteger -Object $item -Name $name -Context $context)
        }
        $paths = (Get-ExactProperty -Object $item -Name 'Paths' -Context $context).Value
        Assert-DetailArray -Value $paths -Context "$context.Paths" -Paths
        if ($paths.Count -lt 2) {
            throw "$context.Paths must contain at least two files."
        }
    }
}

function Assert-BaseSchema {
    param([object]$ReportObject)

    Assert-UnambiguousPropertyNames -Object $ReportObject -Context 'CookReport'
    $version = Get-RequiredNonNegativeInteger -Object $ReportObject -Name 'ReportVersion' -Context 'CookReport'
    if ($version -ne 1 -and $version -ne 2) {
        throw "Unsupported CookReport.ReportVersion '$version'; expected 1 or 2."
    }

    $succeededProperty = Get-ExactProperty -Object $ReportObject -Name 'Succeeded' -Context 'CookReport'
    if ($succeededProperty.Value -isnot [bool]) {
        throw 'CookReport.Succeeded must be a JSON boolean.'
    }

    if ($version -eq 2) {
        [void](Get-RequiredString -Object $ReportObject -Name 'RunId' -Context 'CookReport')
        $status = Get-RequiredString -Object $ReportObject -Name 'Status' -Context 'CookReport'
        if ($status -cnotin @('Running', 'Failed', 'Succeeded')) {
            throw 'CookReport.Status must be Running, Failed, or Succeeded.'
        }
        $outcomeScope = Get-RequiredString -Object $ReportObject -Name 'OutcomeScope' -Context 'CookReport'
        if ($outcomeScope -cne 'Cook') {
            throw "CookReport.OutcomeScope must be 'Cook'."
        }
        if (($status -ceq 'Succeeded') -ne [bool]$succeededProperty.Value) {
            throw 'CookReport.Status and Succeeded are inconsistent.'
        }
        Assert-OptionalString -Object $ReportObject -Name 'Error' -Context 'CookReport'
        if ($status -cne 'Succeeded') {
            return [int]$version
        }
    }
    [void](Get-RequiredString -Object $ReportObject -Name 'GameName' -Context 'CookReport')

    $indexedCount = Get-RequiredNonNegativeInteger -Object $ReportObject -Name 'IndexedFileCount' -Context 'CookReport'
    $includedCount = Get-RequiredNonNegativeInteger -Object $ReportObject -Name 'IncludedFileCount' -Context 'CookReport'
    $prunedCount = Get-RequiredNonNegativeInteger -Object $ReportObject -Name 'PrunedFileCount' -Context 'CookReport'
    [void](Get-RequiredNonNegativeInteger -Object $ReportObject -Name 'IncludedSourceBytes' -Context 'CookReport')
    [void](Get-RequiredNonNegativeInteger -Object $ReportObject -Name 'CookedPayloadBytes' -Context 'CookReport')
    [void](Get-RequiredNonNegativeInteger -Object $ReportObject -Name 'PakBytes' -Context 'CookReport')

    if ([decimal]$includedCount + [decimal]$prunedCount -ne [decimal]$indexedCount) {
        throw 'CookReport file counts are inconsistent: IncludedFileCount + PrunedFileCount must equal IndexedFileCount.'
    }

    foreach ($name in @(
        'CookedJsonFileCount', 'ExcludedByRuleCount', 'EstimatedCpuBytes',
        'EstimatedGpuBytes', 'PrunedBytes', 'WarmupTaggedFileCount')) {
        Assert-OptionalNonNegativeInteger -Object $ReportObject -Name $name -Context 'CookReport'
    }
    foreach ($name in @(
        'ResourceTypeCounts', 'DependencyKindCounts', 'DependencySourceCounts',
        'DependencyEvidenceSourceCounts', 'WarmupGroupFileCounts')) {
        $property = Get-ExactProperty -Object $ReportObject -Name $name -Context 'CookReport' -Optional
        if ($null -ne $property) {
            Assert-CountMap -Value $property.Value -Context "CookReport.$name"
        }
    }

    $unreferenced = Get-ExactProperty -Object $ReportObject -Name 'UnreferencedFiles' -Context 'CookReport' -Optional
    if ($null -ne $unreferenced) {
        Assert-DetailArray -Value $unreferenced.Value -Context 'CookReport.UnreferencedFiles' -Paths
        if ($unreferenced.Value.Count -ne [int64]$prunedCount) {
            throw 'CookReport.UnreferencedFiles count does not match PrunedFileCount.'
        }
    }

    $summary = Get-ExactProperty -Object $ReportObject -Name 'Summary' -Context 'CookReport' -Optional
    if ($null -ne $summary) {
        Assert-UnambiguousPropertyNames -Object $summary.Value -Context 'CookReport.Summary'
    }

    return [int]$version
}

function Assert-V1Schema {
    param([object]$ReportObject)

    $dependencies = Get-ExactProperty -Object $ReportObject -Name 'Dependencies' -Context 'CookReport' -Optional
    if ($null -eq $dependencies) {
        return
    }
    Assert-JsonArray -Value $dependencies.Value -Context 'CookReport.Dependencies'
    for ($index = 0; $index -lt $dependencies.Value.Count; ++$index) {
        $item = $dependencies.Value[$index]
        Assert-UnambiguousPropertyNames -Object $item -Context "CookReport.Dependencies[$index]"
        $path = Get-RequiredString -Object $item -Name 'Path' -Context "CookReport.Dependencies[$index]"
        [void](Assert-CanonicalReportPath -Value $path -Context "CookReport.Dependencies[$index].Path")
        Assert-OptionalArray -Object $item -Name 'Dependencies' -Context "CookReport.Dependencies[$index]"
        Assert-OptionalArray -Object $item -Name 'CatalogDependencies' -Context "CookReport.Dependencies[$index]"
    }
}

function Assert-V2Schema {
    param([object]$ReportObject)

    foreach ($name in @('ContentHash', 'RegistryHash')) {
        $hash = Get-RequiredString -Object $ReportObject -Name $name -Context 'CookReport'
        if ($hash -notmatch '^[0-9A-Fa-f]{16}$') {
            throw "CookReport.$name must be a 16-digit hexadecimal FNV-1a hash."
        }
    }

    $filesProperty = Get-ExactProperty -Object $ReportObject -Name 'Files' -Context 'CookReport'
    Assert-JsonArray -Value $filesProperty.Value -Context 'CookReport.Files'
    $includedCount = Get-RequiredNonNegativeInteger -Object $ReportObject -Name 'IncludedFileCount' -Context 'CookReport'
    if ($filesProperty.Value.Count -ne [int64]$includedCount) {
        throw 'CookReport.Files count does not match IncludedFileCount.'
    }

    $fileIndex = New-Object 'System.Collections.Generic.Dictionary[string,object]' ($script:OrdinalIgnoreCase)
    for ($index = 0; $index -lt $filesProperty.Value.Count; ++$index) {
        $file = $filesProperty.Value[$index]
        $context = "CookReport.Files[$index]"
        Assert-UnambiguousPropertyNames -Object $file -Context $context
        $path = Get-RequiredString -Object $file -Name 'Path' -Context $context
        $path = Assert-CanonicalReportPath -Value $path -Context "$context.Path"
        if ($fileIndex.ContainsKey($path)) {
            $existingPath = (Get-ExactProperty -Object $fileIndex[$path] -Name 'Path' -Context $context).Value
            throw "CookReport.Files contains case-ambiguous paths '$existingPath' and '$path'."
        }
        $fileIndex.Add($path, $file)

        foreach ($name in @('SourceSize', 'CookedSize', 'CompressedSize')) {
            Assert-OptionalNonNegativeInteger -Object $file -Name $name -Context $context
        }
        foreach ($name in @('Hash', 'Type')) {
            Assert-OptionalString -Object $file -Name $name -Context $context
        }

        foreach ($name in @('InclusionReasons', 'DirectReferrers')) {
            $property = Get-ExactProperty -Object $file -Name $name -Context $context -Optional
            if ($null -ne $property) {
                Assert-DetailArray -Value $property.Value -Context "$context.$name" -Paths:($name -eq 'DirectReferrers')
            }
        }
        $witness = Get-ExactProperty -Object $file -Name 'RootWitness' -Context $context -Optional
        if ($null -ne $witness) {
            if ($null -eq $witness.Value) {
                # A null witness is valid for a defensive diagnostic report.
            }
            elseif ($witness.Value -is [array]) {
                Assert-DetailArray -Value $witness.Value -Context "$context.RootWitness"
            }
            elseif ($witness.Value -is [pscustomobject]) {
                Assert-UnambiguousPropertyNames -Object $witness.Value -Context "$context.RootWitness"
                foreach ($name in @('SeedId', 'SeedSource', 'Origin')) {
                    Assert-OptionalString -Object $witness.Value -Name $name -Context "$context.RootWitness"
                }
                Assert-OptionalString -Object $witness.Value -Name 'Group' -Context "$context.RootWitness" -AllowEmpty
                $chain = Get-ExactProperty -Object $witness.Value -Name 'Chain' -Context "$context.RootWitness" -Optional
                if ($null -ne $chain) {
                    Assert-DetailArray -Value $chain.Value -Context "$context.RootWitness.Chain" -Paths
                }
            }
            else {
                throw "$context.RootWitness must be a JSON array or object."
            }
        }
        foreach ($name in @(
            'OnlyIncludedByDirectory', 'OnlyIncludedByExplicitRoot', 'HeuristicOnly')) {
            Assert-OptionalBoolean -Object $file -Name $name -Context $context
        }
    }

    $reverseDependencies = Get-ExactProperty -Object $ReportObject -Name 'ReverseDependencies' -Context 'CookReport' -Optional
    if ($null -ne $reverseDependencies) {
        if ($reverseDependencies.Value -is [array]) {
            $reversePaths = New-Object 'System.Collections.Generic.HashSet[string]' ($script:OrdinalIgnoreCase)
            for ($index = 0; $index -lt $reverseDependencies.Value.Count; ++$index) {
                $entry = $reverseDependencies.Value[$index]
                $context = "CookReport.ReverseDependencies[$index]"
                Assert-UnambiguousPropertyNames -Object $entry -Context $context
                $pathProperty = Get-ExactProperty -Object $entry -Name 'Path' -Context $context -Optional
                if ($null -ne $pathProperty) {
                    $reversePath = Assert-CanonicalReportPath -Value $pathProperty.Value -Context "$context.Path"
                    if (-not $reversePaths.Add($reversePath)) {
                        throw "CookReport.ReverseDependencies contains a duplicate or case-ambiguous Path '$reversePath'."
                    }
                }
                $referrerProperty = Get-ExactProperty -Object $entry -Name 'Referrers' -Context $context -Optional
                if ($null -ne $referrerProperty) {
                    Assert-DetailArray -Value $referrerProperty.Value -Context "$context.Referrers" -Paths
                }
            }
        }
        elseif ($reverseDependencies.Value -is [pscustomobject]) {
            Assert-UnambiguousPropertyNames -Object $reverseDependencies.Value -Context 'CookReport.ReverseDependencies'
            foreach ($property in $reverseDependencies.Value.PSObject.Properties) {
                [void](Assert-CanonicalReportPath -Value $property.Name -Context "CookReport.ReverseDependencies key '$($property.Name)'")
                if ($property.Value -isnot [array] -and $property.Value -isnot [pscustomobject]) {
                    throw "CookReport.ReverseDependencies.$($property.Name) must be a JSON array or object."
                }
            }
        }
        else {
            throw 'CookReport.ReverseDependencies must be a JSON array or object.'
        }
    }

    foreach ($name in @('RemovalSimulations', 'LargestPackageContributors')) {
        Assert-RecordArray -ReportObject $ReportObject -Name $name
    }
    foreach ($name in @('LargestSourceFiles', 'LargestCookedFiles', 'LargestCompressedFiles')) {
        Assert-TopFileRecords -ReportObject $ReportObject -Name $name
    }
    foreach ($name in @('RootContributions', 'IncludeDirectoryContributions')) {
        Assert-ContributionRecords -ReportObject $ReportObject -Name $name
    }
    foreach ($name in @('Edges', 'VirtualDependencies', 'UnresolvedCandidates')) {
        Assert-RecordArray -ReportObject $ReportObject -Name $name
    }
    Assert-DuplicateContentGroups -ReportObject $ReportObject
    $resourceTypeBytes = Get-ExactProperty -Object $ReportObject -Name 'ResourceTypeBytes' -Context 'CookReport' -Optional
    if ($null -ne $resourceTypeBytes) {
        Assert-UnambiguousPropertyNames -Object $resourceTypeBytes.Value -Context 'CookReport.ResourceTypeBytes'
        foreach ($property in $resourceTypeBytes.Value.PSObject.Properties) {
            if ($property.Value -is [pscustomobject]) {
                Assert-ByteSummary -Value $property.Value -Context "CookReport.ResourceTypeBytes.$($property.Name)"
            }
            else {
                Assert-NonNegativeIntegerValue -Value $property.Value -Context "CookReport.ResourceTypeBytes.$($property.Name)"
            }
        }
    }

    return ,$fileIndex
}

function ConvertTo-InvariantText {
    param([object]$Value)

    if ($null -eq $Value) {
        return 'null'
    }
    if ($Value -is [bool]) {
        return $(if ($Value) { 'true' } else { 'false' })
    }
    if ($Value -is [IFormattable]) {
        return $Value.ToString($null, [Globalization.CultureInfo]::InvariantCulture)
    }
    return [string]$Value
}

function Format-DetailValue {
    param([object]$Value)

    if ($Value -is [string]) {
        return [string]$Value
    }
    if ($null -eq $Value -or $Value -is [ValueType]) {
        return ConvertTo-InvariantText -Value $Value
    }
    return ConvertTo-Json -InputObject $Value -Compress -Depth 32
}

function Get-DetailLines {
    param([object[]]$Items, [switch]$PreserveOrder)

    $lines = @($Items | ForEach-Object { Format-DetailValue -Value $_ })
    if (-not $PreserveOrder -and $lines.Count -gt 1) {
        [array]::Sort($lines, $script:Ordinal)
    }
    Write-Output -NoEnumerate $lines
}

function Write-DetailSection {
    param(
        [string]$Name,
        [object]$Value,
        [switch]$Recorded,
        [switch]$PreserveOrder
    )

    if (-not $Recorded) {
        Write-Output "${Name}: <not recorded>"
        return
    }

    $items = @($Value)
    Write-Output ("{0} ({1}):" -f $Name, $items.Count)
    if ($items.Count -eq 0) {
        Write-Output '  (none)'
        return
    }
    $lines = @(Get-DetailLines -Items $items -PreserveOrder:$PreserveOrder)
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        Write-Output ("  [{0}] {1}" -f $index, $lines[$index])
    }
}

function Get-SummaryProperty {
    param([object]$ReportObject, [string]$Name)

    $summaryProperty = Get-ExactProperty -Object $ReportObject -Name 'Summary' -Context 'CookReport' -Optional
    if ($null -ne $summaryProperty -and (Test-ExactProperty -Object $summaryProperty.Value -Name $Name)) {
        return Get-ExactProperty -Object $summaryProperty.Value -Name $Name -Context 'CookReport.Summary'
    }
    return Get-ExactProperty -Object $ReportObject -Name $Name -Context 'CookReport' -Optional
}

function Write-ReportSummary {
    param([object]$ReportObject)

    Write-Output 'Summary:'
    foreach ($name in @('OutcomeScope', 'ContentHash', 'RegistryHash')) {
        $property = Get-SummaryProperty -ReportObject $ReportObject -Name $name
        if ($null -ne $property) {
            Write-Output ("  {0}={1}" -f $name, $property.Value)
        }
    }
    foreach ($name in @(
        'IndexedFileCount', 'IncludedFileCount', 'PrunedFileCount',
        'IncludedSourceBytes', 'CookedPayloadBytes', 'PakBytes',
        'EstimatedCpuBytes', 'EstimatedGpuBytes', 'CookedJsonFileCount',
        'ExcludedByRuleCount', 'PrunedBytes', 'WarmupTaggedFileCount')) {
        $property = Get-SummaryProperty -ReportObject $ReportObject -Name $name
        if ($null -ne $property) {
            Write-Output ("  {0}={1}" -f $name, (ConvertTo-InvariantText -Value $property.Value))
        }
    }
    foreach ($name in @(
        'ResourceTypeCounts', 'DependencyKindCounts', 'DependencySourceCounts',
        'DependencyEvidenceSourceCounts', 'WarmupGroupFileCounts', 'ResourceTypeBytes')) {
        $property = Get-SummaryProperty -ReportObject $ReportObject -Name $name
        if ($null -ne $property) {
            Write-Output ("  {0}={1}" -f $name, (Format-DetailValue -Value $property.Value))
        }
    }
}

function Get-NumericPropertyScore {
    param([object]$Item, [string[]]$Names)

    foreach ($name in $Names) {
        $value = $Item
        $bFound = $true
        foreach ($segment in $name.Split('.')) {
            $property = Get-ExactProperty -Object $value -Name $segment -Context 'contributor' -Optional
            if ($null -eq $property) {
                $bFound = $false
                break
            }
            $value = $property.Value
        }
        if ($bFound) {
            try {
                return [decimal]$value
            }
            catch {
                continue
            }
        }
    }
    return [decimal]0
}

function Format-TopContributorValue {
    param([object]$Value, [string]$CollectionName)

    if ($CollectionName -in @('RootContributions', 'IncludeDirectoryContributions')) {
        $parts = New-Object 'System.Collections.Generic.List[string]'
        foreach ($name in @('Id', 'SeedSource', 'Origin', 'Group', 'SeedCount')) {
            $property = Get-ExactProperty -Object $Value -Name $name -Context $CollectionName -Optional
            if ($null -ne $property -and
                -not ($name -eq 'Group' -and [string]::IsNullOrEmpty([string]$property.Value))) {
                $parts.Add("$name=$($property.Value)")
            }
        }
        foreach ($name in @('Reachable', 'Exclusive')) {
            $property = Get-ExactProperty -Object $Value -Name $name -Context $CollectionName -Optional
            if ($null -ne $property) {
                $parts.Add("$name=$(Format-DetailValue -Value $property.Value)")
            }
        }
        $simulation = Get-ExactProperty -Object $Value -Name 'RemovalSimulation' -Context $CollectionName -Optional
        if ($null -ne $simulation) {
            $summary = [ordered]@{}
            foreach ($name in @('RemovedFileCount', 'SourceBytes', 'CookedBytes', 'CompressedBytes')) {
                $property = Get-ExactProperty -Object $simulation.Value -Name $name -Context "$CollectionName.RemovalSimulation" -Optional
                if ($null -ne $property) {
                    $summary[$name] = $property.Value
                }
            }
            $parts.Add("RemovalSimulation=$(Format-DetailValue -Value ([pscustomobject]$summary))")
        }
        return $parts -join ' | '
    }

    if ($CollectionName -in @('LargestSourceFiles', 'LargestCookedFiles', 'LargestCompressedFiles')) {
        $parts = New-Object 'System.Collections.Generic.List[string]'
        foreach ($name in @('Path', 'Type', 'Bytes')) {
            $property = Get-ExactProperty -Object $Value -Name $name -Context $CollectionName -Optional
            if ($null -ne $property) {
                $parts.Add("$name=$($property.Value)")
            }
        }
        return $parts -join ' | '
    }

    if ($CollectionName -eq 'DuplicateContentGroups') {
        $parts = New-Object 'System.Collections.Generic.List[string]'
        foreach ($name in @(
            'Hash', 'CookedBytesPerFile', 'PossibleSourceSavings',
            'PossibleCookedSavings', 'PossibleCompressedSavings')) {
            $property = Get-ExactProperty -Object $Value -Name $name -Context $CollectionName -Optional
            if ($null -ne $property) {
                $parts.Add("$name=$($property.Value)")
            }
        }
        $paths = Get-ExactProperty -Object $Value -Name 'Paths' -Context $CollectionName -Optional
        if ($null -ne $paths) {
            $parts.Insert(1, "FileCount=$(@($paths.Value).Count)")
        }
        return $parts -join ' | '
    }

    return Format-DetailValue -Value $Value
}

function Get-TopContributorLines {
    param(
        [object[]]$Items,
        [string[]]$ScoreProperties,
        [string]$CollectionName
    )

    $entries = @($Items | ForEach-Object {
        [pscustomobject]@{
            Score = Get-NumericPropertyScore -Item $_ -Names $ScoreProperties
            Text = Format-TopContributorValue -Value $_ -CollectionName $CollectionName
        }
    })
    $entries = @($entries | Sort-Object -Property @(
        @{ Expression = { $_.Score }; Descending = $true },
        @{ Expression = { $_.Text }; Ascending = $true }
    ))
    return @($entries | Select-Object -First $script:TopContributorCount | ForEach-Object { $_.Text })
}

function Write-TopContributors {
    param([object]$ReportObject)

    $specifications = @(
        [pscustomobject]@{ Name = 'RootContributions'; Scores = @('RemovalSimulation.CompressedBytes', 'Exclusive.CompressedBytes', 'Exclusive.CookedBytes', 'Exclusive.SourceBytes', 'Reachable.CompressedBytes', 'Reachable.SourceBytes', 'Reachable.FileCount') },
        [pscustomobject]@{ Name = 'IncludeDirectoryContributions'; Scores = @('RemovalSimulation.CompressedBytes', 'Exclusive.CompressedBytes', 'Exclusive.CookedBytes', 'Exclusive.SourceBytes', 'Reachable.CompressedBytes', 'Reachable.SourceBytes', 'Reachable.FileCount') },
        [pscustomobject]@{ Name = 'LargestSourceFiles'; Scores = @('SourceBytes', 'SourceSize', 'Bytes') },
        [pscustomobject]@{ Name = 'LargestCookedFiles'; Scores = @('CookedBytes', 'CookedSize', 'StoredBytes', 'StoredSize', 'Bytes') },
        [pscustomobject]@{ Name = 'LargestCompressedFiles'; Scores = @('CompressedBytes', 'CompressedSize', 'PakBytes', 'Bytes') },
        [pscustomobject]@{ Name = 'LargestPackageContributors'; Scores = @('CompressedBytes', 'CompressedSize', 'PakBytes', 'CookedBytes', 'SourceBytes', 'Bytes') },
        [pscustomobject]@{ Name = 'DuplicateContentGroups'; Scores = @('PossibleCompressedSavings', 'PossibleCookedSavings', 'PossibleSourceSavings', 'Bytes') }
    )

    $bFound = $false
    foreach ($specification in $specifications) {
        $property = Get-ExactProperty -Object $ReportObject -Name $specification.Name -Context 'CookReport' -Optional
        if ($null -eq $property) {
            continue
        }
        $bFound = $true
        $lines = @(Get-TopContributorLines -Items @($property.Value) -ScoreProperties $specification.Scores -CollectionName $specification.Name)
        Write-Output ("{0} (top {1} of {2}):" -f $specification.Name, $lines.Count, @($property.Value).Count)
        if ($lines.Count -eq 0) {
            Write-Output '  (none)'
        }
        else {
            foreach ($line in $lines) {
                Write-Output "  $line"
            }
        }
    }
    if (-not $bFound) {
        Write-Output 'TopContributors: <not recorded>'
    }
}

function Resolve-FileRecord {
    param(
        [object]$FileIndex,
        [string]$Query,
        [string]$ParameterName
    )

    $normalized = ConvertTo-NormalizedQueryPath -Value $Query -Context $ParameterName
    if (-not $FileIndex.ContainsKey($normalized)) {
        throw "$ParameterName path is not present in CookReport.Files: '$normalized'."
    }
    return $FileIndex[$normalized]
}

function Write-WhyResult {
    param([object]$FileIndex, [string]$Query)

    $file = Resolve-FileRecord -FileIndex $FileIndex -Query $Query -ParameterName '-Why'
    $path = (Get-ExactProperty -Object $file -Name 'Path' -Context 'CookReport.Files item').Value
    Write-Output "Why: $path"

    $flags = New-Object 'System.Collections.Generic.List[string]'
    foreach ($name in @('OnlyIncludedByDirectory', 'OnlyIncludedByExplicitRoot', 'HeuristicOnly')) {
        $property = Get-ExactProperty -Object $file -Name $name -Context "CookReport.Files[$path]" -Optional
        if ($null -ne $property) {
            $flags.Add(("{0}={1}" -f $name, (ConvertTo-InvariantText -Value $property.Value)))
        }
    }
    Write-Output $(if ($flags.Count -eq 0) { 'Flags: <not recorded>' } else { 'Flags: ' + ($flags -join '; ') })

    $inclusionReasons = Get-ExactProperty -Object $file -Name 'InclusionReasons' -Context "CookReport.Files[$path]" -Optional
    if ($null -eq $inclusionReasons) {
        Write-DetailSection -Name 'InclusionReasons' -Value $null
    }
    else {
        Write-DetailSection -Name 'InclusionReasons' -Value $inclusionReasons.Value -Recorded
    }

    $rootWitness = Get-ExactProperty -Object $file -Name 'RootWitness' -Context "CookReport.Files[$path]" -Optional
    if ($null -eq $rootWitness) {
        Write-DetailSection -Name 'RootWitness' -Value $null -PreserveOrder
    }
    else {
        Write-DetailSection -Name 'RootWitness' -Value $rootWitness.Value -Recorded -PreserveOrder
    }

    $directReferrers = Get-ExactProperty -Object $file -Name 'DirectReferrers' -Context "CookReport.Files[$path]" -Optional
    if ($null -eq $directReferrers) {
        Write-DetailSection -Name 'DirectReferrers' -Value $null
    }
    else {
        Write-DetailSection -Name 'DirectReferrers' -Value $directReferrers.Value -Recorded
    }
}

function Add-DetailLines {
    param(
        [object]$Value,
        [object]$LineSet,
        [object]$Lines
    )

    $items = if ($Value -is [array]) { @($Value) } else { @($Value) }
    foreach ($item in $items) {
        $line = Format-DetailValue -Value $item
        if ($LineSet.Add($line)) {
            $Lines.Add($line)
        }
    }
}

function Write-ReferrersResult {
    param([object]$ReportObject, [object]$FileIndex, [string]$Query)

    $file = Resolve-FileRecord -FileIndex $FileIndex -Query $Query -ParameterName '-Referrers'
    $path = (Get-ExactProperty -Object $file -Name 'Path' -Context 'CookReport.Files item').Value
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lineSet = New-Object 'System.Collections.Generic.HashSet[string]' ($script:Ordinal)
    $bRecorded = $false

    $reverse = Get-ExactProperty -Object $ReportObject -Name 'ReverseDependencies' -Context 'CookReport' -Optional
    if ($null -eq $reverse) {
        $direct = Get-ExactProperty -Object $file -Name 'DirectReferrers' -Context "CookReport.Files[$path]" -Optional
        if ($null -ne $direct) {
            $bRecorded = $true
            Add-DetailLines -Value $direct.Value -LineSet $lineSet -Lines $lines
        }
    }
    else {
        $bRecorded = $true
        if ($reverse.Value -is [pscustomobject]) {
            $matches = @($reverse.Value.PSObject.Properties | Where-Object { $script:OrdinalIgnoreCase.Equals($_.Name, $path) })
            if ($matches.Count -gt 1) {
                throw "CookReport.ReverseDependencies contains case-ambiguous keys for '$path'."
            }
            if ($matches.Count -eq 1) {
                $value = $matches[0].Value
                if ($value -is [pscustomobject]) {
                    $referrerProperty = $null
                    foreach ($name in @('Referrers', 'DirectReferrers', 'ReverseDependencies')) {
                        $referrerProperty = Get-ExactProperty -Object $value -Name $name -Context "CookReport.ReverseDependencies[$path]" -Optional
                        if ($null -ne $referrerProperty) { break }
                    }
                    if ($null -ne $referrerProperty) {
                        Add-DetailLines -Value $referrerProperty.Value -LineSet $lineSet -Lines $lines
                    }
                    else {
                        Add-DetailLines -Value $value -LineSet $lineSet -Lines $lines
                    }
                }
                else {
                    Add-DetailLines -Value $value -LineSet $lineSet -Lines $lines
                }
            }
        }
        else {
            foreach ($entry in $reverse.Value) {
                $targetProperty = $null
                foreach ($name in @('Path', 'Target')) {
                    $targetProperty = Get-ExactProperty -Object $entry -Name $name -Context 'CookReport.ReverseDependencies item' -Optional
                    if ($null -ne $targetProperty) { break }
                }
                if ($null -eq $targetProperty -or $targetProperty.Value -isnot [string] -or
                    -not $script:OrdinalIgnoreCase.Equals($targetProperty.Value.Replace('\', '/'), $path)) {
                    continue
                }

                $referrerProperty = $null
                foreach ($name in @('Referrers', 'DirectReferrers', 'ReverseDependencies', 'Referrer', 'Owner')) {
                    $referrerProperty = Get-ExactProperty -Object $entry -Name $name -Context 'CookReport.ReverseDependencies item' -Optional
                    if ($null -ne $referrerProperty) { break }
                }
                if ($null -ne $referrerProperty) {
                    Add-DetailLines -Value $referrerProperty.Value -LineSet $lineSet -Lines $lines
                }
            }
        }
    }

    Write-Output "Referrers: $path"
    if (-not $bRecorded) {
        Write-Output '  <not recorded>'
        return
    }
    $sortedLines = $lines.ToArray()
    [array]::Sort($sortedLines, $script:Ordinal)
    if ($sortedLines.Count -eq 0) {
        Write-Output '  (none)'
        return
    }
    foreach ($line in $sortedLines) {
        Write-Output "  $line"
    }
}

function Test-IdentityMatch {
    param([object]$Record, [string]$Query)

    foreach ($name in @('Id', 'Origin', 'Name', 'SeedId', 'SeedOrigin')) {
        $property = Get-ExactProperty -Object $Record -Name $name -Context 'contribution record' -Optional
        if ($null -ne $property -and $property.Value -is [string]) {
            $candidate = $property.Value.Trim().Replace('\', '/')
            if ($script:OrdinalIgnoreCase.Equals($candidate, $Query)) {
                return $true
            }
        }
    }
    foreach ($name in @('Seed', 'RemovedSeed')) {
        $property = Get-ExactProperty -Object $Record -Name $name -Context 'contribution record' -Optional
        if ($null -ne $property -and $property.Value -is [pscustomobject] -and
            (Test-IdentityMatch -Record $property.Value -Query $Query)) {
            return $true
        }
    }
    return $false
}

function Get-IdentityMatches {
    param([object]$ReportObject, [string]$PropertyName, [string]$Query)

    $property = Get-ExactProperty -Object $ReportObject -Name $PropertyName -Context 'CookReport' -Optional
    if ($null -eq $property) {
        return @()
    }
    return @($property.Value | Where-Object { Test-IdentityMatch -Record $_ -Query $Query })
}

function Write-WithoutResult {
    param([object]$ReportObject, [string]$Query)

    if ([string]::IsNullOrWhiteSpace($Query) -or $Query.Length -gt 4096 -or
        $Query.IndexOfAny(@([char]0x0D, [char]0x0A, [char]0x00)) -ge 0) {
        throw '-Without must be a non-empty seed id, origin, or name without control characters.'
    }
    $normalized = $Query.Trim().Replace('\', '/')
    $sections = @('RemovalSimulations', 'RootContributions', 'IncludeDirectoryContributions')
    $matchesBySection = @{}
    $matchCount = 0
    foreach ($section in $sections) {
        $matches = @(Get-IdentityMatches -ReportObject $ReportObject -PropertyName $section -Query $normalized)
        $matchesBySection[$section] = $matches
        $matchCount += $matches.Count
    }
    if ($matchCount -eq 0) {
        throw "No RemovalSimulation or contribution matches -Without '$normalized' by Id, Origin, or Name."
    }

    Write-Output "Without: $normalized"
    $simulationLines = New-Object 'System.Collections.Generic.List[string]'
    $simulationLineSet = New-Object 'System.Collections.Generic.HashSet[string]' ($script:Ordinal)
    foreach ($match in $matchesBySection['RemovalSimulations']) {
        $line = Format-DetailValue -Value $match
        if ($simulationLineSet.Add($line)) {
            $simulationLines.Add($line)
        }
    }
    foreach ($section in @('RootContributions', 'IncludeDirectoryContributions')) {
        foreach ($match in $matchesBySection[$section]) {
            $simulation = Get-ExactProperty -Object $match -Name 'RemovalSimulation' -Context "CookReport.$section item" -Optional
            if ($null -eq $simulation) {
                continue
            }
            $identity = [ordered]@{}
            foreach ($name in @('Id', 'Origin', 'Name')) {
                $property = Get-ExactProperty -Object $match -Name $name -Context "CookReport.$section item" -Optional
                if ($null -ne $property) {
                    $identity[$name] = $property.Value
                }
            }
            $identity['RemovalSimulation'] = $simulation.Value
            $line = Format-DetailValue -Value ([pscustomobject]$identity)
            if ($simulationLineSet.Add($line)) {
                $simulationLines.Add($line)
            }
        }
    }
    $sortedSimulationLines = $simulationLines.ToArray()
    [array]::Sort($sortedSimulationLines, $script:Ordinal)
    Write-Output ("RemovalSimulations ({0}):" -f $sortedSimulationLines.Count)
    if ($sortedSimulationLines.Count -eq 0) {
        Write-Output '  <not recorded for this seed>'
    }
    else {
        foreach ($line in $sortedSimulationLines) {
            Write-Output "  $line"
        }
    }

    foreach ($section in @('RootContributions', 'IncludeDirectoryContributions')) {
        $property = Get-ExactProperty -Object $ReportObject -Name $section -Context 'CookReport' -Optional
        $matches = $matchesBySection[$section]
        if ($null -eq $property) {
            Write-Output "${section}: <not recorded>"
            continue
        }
        Write-Output ("{0} ({1}):" -f $section, $matches.Count)
        if ($matches.Count -eq 0) {
            Write-Output '  (no matching record)'
            continue
        }
        $lines = Get-DetailLines -Items $matches
        foreach ($line in $lines) {
            Write-Output "  $line"
        }
    }
}

$reportItem = Get-Item -LiteralPath $Report -Force -ErrorAction Stop
if ($reportItem -isnot [IO.FileInfo] -or $reportItem.PSIsContainer -or
    ($reportItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "-Report must name a regular, non-reparse-point file: $($reportItem.FullName)"
}
if ($reportItem.Length -eq 0 -or $reportItem.Length -gt $script:MaximumReportBytes) {
    throw "-Report must be between 1 byte and $script:MaximumReportBytes bytes: $($reportItem.FullName)"
}

$strictUtf8 = New-Object Text.UTF8Encoding($false, $true)
try {
    $reportText = [IO.File]::ReadAllText($reportItem.FullName, $strictUtf8)
    Assert-StrictJsonText -Value $reportText
    $reportObject = ConvertFrom-Json -InputObject $reportText
}
catch {
    throw "Cannot parse CookReport as strict UTF-8 JSON: $($_.Exception.Message)"
}
finally {
    $reportText = $null
}

$reportVersion = Assert-BaseSchema -ReportObject $reportObject
$fileIndex = $null
if ($reportVersion -eq 1) {
    Assert-V1Schema -ReportObject $reportObject
}
else {
    $status = (Get-ExactProperty -Object $reportObject -Name 'Status' -Context 'CookReport').Value
    if ($status -ceq 'Succeeded') {
        $fileIndex = Assert-V2Schema -ReportObject $reportObject
    }
}

$gameNameProperty = Get-ExactProperty -Object $reportObject -Name 'GameName' -Context 'CookReport' -Optional
$succeeded = (Get-ExactProperty -Object $reportObject -Name 'Succeeded' -Context 'CookReport').Value
$headerParts = New-Object 'System.Collections.Generic.List[string]'
$headerParts.Add("CookReport v$reportVersion")
if ($null -ne $gameNameProperty) {
    $headerParts.Add("Game=$($gameNameProperty.Value)")
}
$headerParts.Add("Succeeded=$(ConvertTo-InvariantText -Value $succeeded)")
foreach ($name in @('Status', 'RunId')) {
    $property = Get-ExactProperty -Object $reportObject -Name $name -Context 'CookReport' -Optional
    if ($null -ne $property) {
        $headerParts.Add("$name=$($property.Value)")
    }
}
Write-Output ($headerParts -join ' | ')

if ($reportVersion -eq 2 -and $status -cne 'Succeeded') {
    $errorProperty = Get-ExactProperty -Object $reportObject -Name 'Error' -Context 'CookReport' -Optional
    if ($null -ne $errorProperty) {
        Write-Output "Error: $($errorProperty.Value)"
    }
    Write-Output 'Notice: this CookReport records an incomplete or failed cook; summary and explain data are not available.'
    return
}

$bHasQuery = -not [string]::IsNullOrWhiteSpace($Why) -or
    -not [string]::IsNullOrWhiteSpace($Referrers) -or
    -not [string]::IsNullOrWhiteSpace($Without)
if (-not $bHasQuery -or $reportVersion -eq 1) {
    Write-ReportSummary -ReportObject $reportObject
}

if ($reportVersion -eq 1) {
    Write-Output 'Notice: ReportVersion 1 has no per-file explain, reverse-dependency, contribution, or removal-simulation data.'
    if ($bHasQuery) {
        Write-Output 'The requested explain query cannot be answered by this v1 report; summary data above remains valid.'
    }
    return
}

if (-not $bHasQuery) {
    Write-TopContributors -ReportObject $reportObject
    return
}

if (-not [string]::IsNullOrWhiteSpace($Why)) {
    Write-WhyResult -FileIndex $fileIndex -Query $Why
}
if (-not [string]::IsNullOrWhiteSpace($Referrers)) {
    Write-ReferrersResult -ReportObject $reportObject -FileIndex $fileIndex -Query $Referrers
}
if (-not [string]::IsNullOrWhiteSpace($Without)) {
    Write-WithoutResult -ReportObject $reportObject -Query $Without
}
