param(
  [Parameter(Mandatory = $true)]
  [string]$BinDirectory
)

$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
  $PSNativeCommandUseErrorActionPreference = $false
}

$resolvedBin = (Resolve-Path -LiteralPath $BinDirectory).Path
$workboost = Join-Path $resolvedBin 'workboost.exe'
$helper = Join-Path $resolvedBin 'WorkBoostElevated.exe'
if (-not (Test-Path -LiteralPath $workboost) -or
    -not (Test-Path -LiteralPath $helper) -or
    -not (Test-Path -LiteralPath (Join-Path $resolvedBin 'config\diagnosis.json'))) {
  throw 'installed executable, helper, or adjacent config is missing'
}

function Assert-NoExternalCompilerRuntime {
  param([string]$Executable)

  $binaryText = [Text.Encoding]::ASCII.GetString(
      [IO.File]::ReadAllBytes($Executable))
  $forbiddenRuntime =
      '(?i)(libgcc_s_[^\x00]+\.dll|libstdc\+\+-6\.dll|' +
      'libwinpthread-1\.dll|vcruntime[0-9_a-z]*\.dll|' +
      'msvcp[0-9_a-z]*\.dll|concrt[0-9_a-z]*\.dll)'
  $match = [regex]::Match($binaryText, $forbiddenRuntime)
  if ($match.Success) {
    throw "installed executable depends on an external compiler runtime: $($match.Value)"
  }
}

Assert-NoExternalCompilerRuntime -Executable $workboost
Assert-NoExternalCompilerRuntime -Executable $helper

function Invoke-JsonCommand {
  param([string[]]$Arguments)

  $text = (& $workboost @Arguments) -join "`n"
  if ($LASTEXITCODE -ne 0) {
    throw "command failed: workboost $($Arguments -join ' ')"
  }
  $value = $text | ConvertFrom-Json
  if ($value.schema_version -ne 1) {
    throw "schema_version is missing or unsupported: workboost $($Arguments -join ' ')"
  }
  return $value
}

function Invoke-NativeCapture {
  param(
    [string]$Executable,
    [string[]]$Arguments = @()
  )

  $previousPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'Continue'
    $text = (& $Executable @Arguments 2>$null) -join "`n"
    $exitCode = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previousPreference
  }
  return [pscustomobject]@{ ExitCode = $exitCode; Text = $text }
}

$commands = @(
  @('status', '--json'),
  @('top', 'cpu', '--limit', '3', '--json'),
  @('top', 'mem', '--limit', '3', '--json'),
  @('top', 'io', '--limit', '3', '--json'),
  @('connections', '--json'),
  @('services', '--limit', '3', '--json'),
  @('serial', '--json'),
  @('startup', '--json'),
  @('protected', '--json'),
  @('profile', 'show', 'coding', '--json')
)
$commandResults = @{}
foreach ($arguments in $commands) {
  $commandResults[$arguments -join ' '] =
      Invoke-JsonCommand -Arguments $arguments
}

function Assert-BooleanProperty {
  param(
    [object]$Value,
    [string]$Name,
    [string]$Command
  )

  if ($Value.PSObject.Properties.Name -notcontains $Name -or
      $Value.$Name -isnot [bool]) {
    throw "$Name is missing or is not Boolean: workboost $Command"
  }
}

$status = $commandResults['status --json']
Assert-BooleanProperty $status 'process_inventory_complete' 'status --json'
Assert-BooleanProperty $status 'tcp_inventory_complete' 'status --json'
Assert-BooleanProperty $status 'diagnosis_window_complete' 'status --json'
if ($status.diagnosis_window_complete -or
    $status.diagnosis_sample_count -ne 1) {
  throw 'status must disclose that one sample is not a diagnosis window'
}
foreach ($command in @('top cpu --limit 3 --json',
                        'top mem --limit 3 --json',
                        'top io --limit 3 --json')) {
  Assert-BooleanProperty $commandResults[$command] `
      'process_inventory_complete' $command
}
foreach ($command in @('connections --json', 'protected --json')) {
  Assert-BooleanProperty $commandResults[$command] `
      'process_inventory_complete' $command
  Assert-BooleanProperty $commandResults[$command] `
      'tcp_inventory_complete' $command
}

$relativeReport = Join-Path (Get-Location).Path 'workboost-diagnose-smoke.json'
try {
  $output = (& $workboost diagnose --duration 1 --interval 250 --json `
      --output (Split-Path -Leaf $relativeReport)) -join "`n"
  if ($LASTEXITCODE -ne 0 -or $output -notmatch '^Report:') {
    throw 'diagnostic recording output smoke failed'
  }
  $diagnosis = Get-Content -Raw -Encoding utf8 $relativeReport |
      ConvertFrom-Json
  if ($diagnosis.schema_version -ne 1 -or
      $diagnosis.sample_interval_ms -ne 250 -or
      $diagnosis.sample_count -lt 3) {
    throw 'diagnostic recording evidence is incomplete'
  }
  foreach ($field in @('process_inventory_complete_samples',
                        'tcp_inventory_complete_samples',
                        'protection_inventory_complete_samples')) {
    if ($diagnosis.PSObject.Properties.Name -notcontains $field -or
        $diagnosis.$field -lt 0 -or
        $diagnosis.$field -gt $diagnosis.sample_count) {
      throw "diagnostic inventory coverage is invalid: $field"
    }
  }
} finally {
  Remove-Item -LiteralPath $relativeReport -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath ($relativeReport + '.tmp') -Force `
      -ErrorAction SilentlyContinue
}

$invalidInterval = Invoke-NativeCapture -Executable $workboost -Arguments @(
    'diagnose', '--duration', '1', '--interval', '200', '--json')
if ($invalidInterval.ExitCode -ne 64) {
  throw 'invalid diagnostic interval was not rejected'
}

$invalidPosition = Invoke-NativeCapture -Executable $workboost -Arguments @(
    'diagnose', 'unexpected')
if ($invalidPosition.ExitCode -ne 64) {
  throw 'unexpected diagnostic argument was not rejected'
}

$invalidBaseline = Invoke-NativeCapture -Executable $workboost -Arguments @(
    'coding', 'enter', '--dry-run', '--baseline-duration', '9')
if ($invalidBaseline.ExitCode -ne 64) {
  throw 'short Coding Mode baseline was not rejected'
}

$comparisonResult = Invoke-NativeCapture -Executable $workboost -Arguments @(
    'benchmark', 'compare', 'codex.exe', '--duration', '1', '--json')
if ($comparisonResult.ExitCode -ne 1) {
  throw 'incomplete startup comparison did not return exit code 1'
}
$comparison = $comparisonResult.Text | ConvertFrom-Json
if ($comparison.schema_version -ne 1 -or
    $comparison.baseline.requested_runs -ne 3 -or
    -not $comparison.negative_delta_is_improvement) {
  throw 'incomplete startup comparison schema is invalid'
}

$helperResult = Invoke-NativeCapture -Executable $helper
if ($helperResult.ExitCode -ne 64) {
  throw 'elevated helper invalid invocation was not rejected'
}

Write-Host ("Installed CLI smoke passed: standalone runtime, " +
    "$($commands.Count) JSON commands, diagnostic recording, comparison, " +
    "and helper validation.")
