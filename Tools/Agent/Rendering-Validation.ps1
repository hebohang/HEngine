function Assert-RenderingValidationLogs {
    param([Parameter(Mandatory = $true)][string[]]$Paths)
    $count = 0
    $examples = [Collections.Generic.List[string]]::new()
    foreach ($path in $Paths) {
        $reader = [IO.File]::OpenText($path)
        try {
            while ($null -ne ($line = $reader.ReadLine())) {
                if ($line -notmatch 'Vulkan validation: Validation Error:|NVRHI validation error:|DX12 Debug Layer:|NVRHI Vulkan: Device Removed|VK_ERROR_DEVICE_LOST|DXGI_ERROR_DEVICE_(REMOVED|HUNG|RESET)') { continue }
                $count++
                if ($examples.Count -lt 3) {
                    $examples.Add($line.Substring(0, [Math]::Min(400, $line.Length)))
                }
            }
        } finally { $reader.Dispose() }
    }
    if ($count -gt 0) {
        throw "Rendering validation reported $count error log entries. $($examples -join ' | ') Full logs: $($Paths -join ', ')"
    }
}
