# Weighted Path Fabric - full validation of one source tree.
# Copyright 2026 Summon Software Labs.
#
# Runs the Release and Debug builds, the AddressSanitizer build where the
# toolchain supports it, the MSVC static analyzer, the examples, the benchmarks,
# the install, and an independent find_package consumer built outside the source
# tree. No step uses a timeout: a hang is a defect and must be diagnosed.
[CmdletBinding()]
param(
  [string]$SourceDir = '',
  [string]$WorkDir = '',
  [switch]$SkipAsan,
  [switch]$SkipAnalyze,
  [switch]$SkipDebug
)

$ErrorActionPreference = 'Stop'
$script:Failures = New-Object System.Collections.Generic.List[string]

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrEmpty($SourceDir)) {
  $SourceDir = (Resolve-Path (Join-Path $ScriptDir '..')).Path
}
if ([string]::IsNullOrEmpty($WorkDir)) {
  $WorkDir = Join-Path $env:TEMP ('wpf-validate-' + [guid]::NewGuid().ToString('N'))
}

function Write-Step([string]$text) {
  Write-Host ''
  Write-Host ('=== ' + $text + ' ===') -ForegroundColor Cyan
}

function Invoke-Step([string]$name, [scriptblock]$body) {
  try {
    & $body
    Write-Host ('PASS ' + $name) -ForegroundColor Green
  } catch {
    Write-Host ('FAIL ' + $name + ' : ' + $_) -ForegroundColor Red
    $script:Failures.Add($name + ' : ' + $_)
  }
}

# Runs a native command and fails the step when it exits non-zero. Arguments are
# passed as an array so no shell quoting is involved.
function Invoke-Native([string]$exe, [string[]]$arguments) {
  $output = & $exe @arguments 2>&1
  if ($LASTEXITCODE -ne 0) {
    $tail = ($output | Select-Object -Last 25) -join [Environment]::NewLine
    throw ($exe + ' failed with exit code ' + $LASTEXITCODE + [Environment]::NewLine + $tail)
  }
  return $output
}

# Import the Visual Studio developer environment into this process once, instead
# of wrapping every command in a cmd shell.
$programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
$vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe was not found' }
$vsRoot = (& $vswhere -latest -products * -property installationPath).Trim()
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw ('vcvars64.bat was not found under ' + $vsRoot) }
$environmentLines = cmd /c ('"' + $vcvars + '" && set')
foreach ($line in $environmentLines) {
  if ($line -match '^([^=]+)=(.*)$') {
    Set-Item -Path ('env:' + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
  }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'cmake was not found on PATH' }
$asanDllDir = Get-ChildItem (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory |
  Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64' } |
  Where-Object { Test-Path (Join-Path $_ 'clang_rt.asan_dynamic-x86_64.dll') } |
  Select-Object -First 1
$script:AsanSupported = [bool]$asanDllDir

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
Write-Host ('source  : ' + $SourceDir)
Write-Host ('workdir : ' + $WorkDir)
Write-Host ('asan    : ' + $script:AsanSupported)

function Configure-And-Build([string]$name, [string[]]$options) {
  $buildDir = Join-Path $WorkDir $name
  $configuration = @('-S', $SourceDir, '-B', $buildDir, '-G', 'Ninja') + $options
  Invoke-Native 'cmake' $configuration | Out-Null
  Invoke-Native 'cmake' @('--build', $buildDir) | Out-Null
  return $buildDir
}

function Run-Ctest([string]$buildDir) {
  $output = Invoke-Native 'ctest' @('--test-dir', $buildDir, '--output-on-failure')
  $summary = ($output | Select-String -Pattern 'tests passed' | Select-Object -Last 1)
  Write-Host $summary
  if (-not ($summary -match '100% tests passed')) { throw 'ctest did not report a fully passing run' }
  return $output
}

# --- Release -----------------------------------------------------------------
Write-Step 'Release configure, build and test'
$releaseDir = $null
Invoke-Step 'release' {
  $script:releaseDir = Configure-And-Build 'release' @(
    '-DCMAKE_BUILD_TYPE=Release',
    ('-DCMAKE_INSTALL_PREFIX=' + (Join-Path $WorkDir 'prefix')))
  Run-Ctest $script:releaseDir | Out-Null
}

# --- Install and independent consumer ----------------------------------------
Write-Step 'Install and find_package consumer'
Invoke-Step 'install' {
  Invoke-Native 'cmake' @('--install', $script:releaseDir) | Out-Null
  $prefix = Join-Path $WorkDir 'prefix'
  $config = Join-Path $prefix 'lib\cmake\WeightedPathFabric\WeightedPathFabricConfig.cmake'
  if (-not (Test-Path $config)) { throw ('the package config was not installed at ' + $config) }
  $version = Join-Path $prefix 'lib\cmake\WeightedPathFabric\WeightedPathFabricConfigVersion.cmake'
  if (-not (Test-Path $version)) { throw 'the package version file was not installed' }
  $targets = Join-Path $prefix 'lib\cmake\WeightedPathFabric\WeightedPathFabricTargets.cmake'
  if (-not (Test-Path $targets)) { throw 'the exported targets file was not installed' }
  $header = Join-Path $prefix 'include\wpf\engine.hpp'
  if (-not (Test-Path $header)) { throw 'the public headers were not installed' }

  # The consumer is copied out of the source tree so that no source-tree include
  # can leak into it.
  $consumerSource = Join-Path $WorkDir 'consumer-src'
  if (Test-Path $consumerSource) { Remove-Item -Recurse -Force $consumerSource }
  Copy-Item -Recurse (Join-Path $SourceDir 'tests\consumer') $consumerSource
  $consumerBuild = Join-Path $WorkDir 'consumer-build'
  Invoke-Native 'cmake' @('-S', $consumerSource, '-B', $consumerBuild, '-G', 'Ninja',
                          '-DCMAKE_BUILD_TYPE=Release',
                          ('-DCMAKE_PREFIX_PATH=' + $prefix)) | Out-Null
  Invoke-Native 'cmake' @('--build', $consumerBuild) | Out-Null
  $consumerExe = Join-Path $consumerBuild 'wpf_consumer.exe'
  $consumerOut = Invoke-Native $consumerExe @()
  $consumerOut | ForEach-Object { Write-Host ('  ' + $_) }
  if ((($consumerOut -join ' ') -notmatch 'consumer OK')) { throw 'the consumer did not report success' }
}

# --- Debug -------------------------------------------------------------------
if (-not $SkipDebug) {
  Write-Step 'Debug configure, build and test'
  Invoke-Step 'debug' {
    $debugDir = Configure-And-Build 'debug' @('-DCMAKE_BUILD_TYPE=Debug')
    Run-Ctest $debugDir | Out-Null
  }
}

# --- AddressSanitizer --------------------------------------------------------
Write-Step 'AddressSanitizer'
if ($SkipAsan) {
  Write-Host 'skipped by request'
} elseif (-not $script:AsanSupported) {
  $script:Failures.Add('asan : the MSVC AddressSanitizer runtime is not installed')
} else {
  Invoke-Step 'asan' {
    $asanDir = Configure-And-Build 'asan' @(
      '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
      '-DWPF_ENABLE_ASAN=ON',
      '-DWPF_BUILD_BENCHMARKS=OFF')
    # The dynamic ASan runtime must be discoverable by every test process.
    $env:PATH = $asanDllDir + ';' + $env:PATH
    $output = Run-Ctest $asanDir
    $text = $output -join ' '
    if (($text -match 'ERROR: AddressSanitizer')) { throw 'AddressSanitizer reported a finding' }
  }
}

# --- MSVC static analysis ----------------------------------------------------
Write-Step 'MSVC static analyzer'
if ($SkipAnalyze) {
  Write-Host 'skipped by request'
} else {
  Invoke-Step 'analyze' {
    $analyzeDir = Join-Path $WorkDir 'analyze'
    Invoke-Native 'cmake' @('-S', $SourceDir, '-B', $analyzeDir, '-G', 'Ninja',
                            '-DCMAKE_BUILD_TYPE=Release', '-DWPF_ENABLE_ANALYZE=ON',
                            '-DWPF_BUILD_BENCHMARKS=OFF') | Out-Null
    $output = & cmake --build $analyzeDir 2>&1
    $code = $LASTEXITCODE
    $findings = $output | Select-String -Pattern 'warning C|error C'
    if ($code -ne 0) {
      throw ('the analyzer build failed' + [Environment]::NewLine + (($output | Select-Object -Last 25) -join [Environment]::NewLine))
    }
    if ($findings) {
      throw ('the analyzer reported first-party findings:' + [Environment]::NewLine + (($findings | Select-Object -First 20) -join [Environment]::NewLine))
    }
  }
}

# --- Result ------------------------------------------------------------------
Write-Step 'Result'
if ($script:Failures.Count -ne 0) {
  foreach ($failure in $script:Failures) { Write-Host ('FAILED ' + $failure) -ForegroundColor Red }
  exit 1
}
Write-Host 'all validation steps passed' -ForegroundColor Green
exit 0
