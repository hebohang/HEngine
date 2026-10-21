# Read-only, bounded Windows hardware telemetry. Launched and stopped by an owned
# Test-StreamingSoak process; never changes power policy or CPU affinity.
param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
$paths=@('Actual Frequency','Processor Frequency','% Processor Performance','% Processor Utility','Performance Limit Flags')|
 ForEach-Object {"\Processor Information(*)\$_"}
$writer=[IO.StreamWriter]::new([IO.Path]::GetFullPath($OutputPath),$false,[Text.UTF8Encoding]::new($false))
$writer.AutoFlush=$true
try {
 Get-Counter -Counter $paths -SampleInterval 1 -MaxSamples 3000 -ErrorAction Stop|ForEach-Object {
  $values=@($_.CounterSamples|ForEach-Object {@{instance=$_.InstanceName;counter=($_.Path -split '\\')[-1];status=$_.Status;value=$_.CookedValue}})
  $writer.WriteLine((@{utc=$_.Timestamp.ToUniversalTime().ToString('o');counters=$values}|ConvertTo-Json -Depth 5 -Compress))
 }
}finally {$writer.Dispose()}
