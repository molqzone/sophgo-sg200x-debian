[CmdletBinding()]
param(
  [ValidateSet('test', 'firmware', 'modules', 'middleware', 'image', 'verify', 'toolchain')]
  [string]$Target = 'verify',
  [string]$Board = 'maixcam',
  [ValidateSet('sd', 'emmc')]
  [string]$Storage = 'sd',
  [string]$Output = 'output'
)

$ErrorActionPreference = 'Stop'

function Test-DockerObject {
  param(
    [string[]]$Arguments,
    [ValidateRange(1, 5)]
    [int]$Attempts = 3
  )
  for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
    $previousPreference = $ErrorActionPreference
    try {
      $ErrorActionPreference = 'SilentlyContinue'
      $global:LASTEXITCODE = $null
      & docker @Arguments *> $null
      $ok = $?
      $exitCode = if ($global:LASTEXITCODE -is [int]) {
        [int]$global:LASTEXITCODE
      } elseif ($ok) {
        0
      } else {
        1
      }
    } finally {
      $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -eq 0) { return $true }
    if ($attempt -lt $Attempts) { Start-Sleep -Milliseconds 250 }
  }
  return $false
}

function ConvertTo-ContainerProxy {
  param([AllowNull()][AllowEmptyString()][string]$Value)
  if (-not $Value) { return '' }
  return $Value -replace '://(127\.0\.0\.1|localhost)(:)', '://host.docker.internal$2'
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$toolchainPath = Join-Path $repoRoot 'toolchain.env'
$dockerfile = Join-Path $repoRoot 'scripts\Dockerfile'

$versions = @{}
foreach ($line in Get-Content -LiteralPath $toolchainPath) {
  $trimmed = $line.Trim()
  if (-not $trimmed -or $trimmed.StartsWith('#')) { continue }
  $parts = $trimmed.Split('=', 2)
  if ($parts.Count -eq 2) { $versions[$parts[0]] = $parts[1] }
}

$sha = [System.Security.Cryptography.SHA256]::Create()
try {
  $fileHashes = @($dockerfile, $toolchainPath) | ForEach-Object {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash.ToLowerInvariant()
  }
  $bytes = [System.Text.Encoding]::ASCII.GetBytes(($fileHashes -join "`n") + "`n")
  $hash = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant().Substring(0, 12)
} finally {
  $sha.Dispose()
}
$image = if ($env:BUILDER_IMAGE) { $env:BUILDER_IMAGE } else { "sg2002-toolchain:$hash" }
$containerHttpProxy = ConvertTo-ContainerProxy $env:HTTP_PROXY
$containerHttpsProxy = ConvertTo-ContainerProxy $env:HTTPS_PROXY
$containerAptHttpProxy = ConvertTo-ContainerProxy $env:APT_HTTP_PROXY
$containerAptHttpsProxy = ConvertTo-ContainerProxy $env:APT_HTTPS_PROXY
$debianMirror = if ($env:DEBIAN_MIRROR) { $env:DEBIAN_MIRROR } else { 'https://deb.debian.org/debian' }
$containerDebianMirror = ConvertTo-ContainerProxy $debianMirror

$imageCached = Test-DockerObject -Arguments @('image', 'inspect', $image)
Write-Host "Builder image: $image (cached=$($imageCached.ToString().ToLowerInvariant()))"
if (-not $imageCached) {
  $buildArgs = @(
    'build',
    '--build-arg', "BUILDER_BASE_IMAGE=$($versions.BUILDER_BASE_IMAGE)",
    '--build-arg', "DEBIAN_SNAPSHOT=$($versions.DEBIAN_SNAPSHOT)",
    '--build-arg', "HOST_TOOLS_REPO=$($versions.HOST_TOOLS_REPO)",
    '--build-arg', "HOST_TOOLS_COMMIT=$($versions.HOST_TOOLS_COMMIT)",
    '-t', $image,
    '-f', $dockerfile,
    $repoRoot
  )
  if ($containerHttpProxy) {
    $buildArgs = @(
      'build',
      '--build-arg', "HTTP_PROXY=$containerHttpProxy",
      '--build-arg', "http_proxy=$containerHttpProxy"
    ) + $buildArgs[1..($buildArgs.Count - 1)]
  }
  if ($containerHttpsProxy) {
    $buildArgs = @(
      'build',
      '--build-arg', "HTTPS_PROXY=$containerHttpsProxy",
      '--build-arg', "https_proxy=$containerHttpsProxy"
    ) + $buildArgs[1..($buildArgs.Count - 1)]
  }
  & docker @buildArgs
  if ($LASTEXITCODE -ne 0) { throw "Docker toolchain build failed with exit code $LASTEXITCODE" }
}

foreach ($volume in @('sg2002-sdk', 'sg2002-build', 'sg2002-ccache')) {
  if (-not (Test-DockerObject -Arguments @('volume', 'inspect', $volume))) {
    & docker volume create $volume | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Unable to create Docker volume $volume" }
  }
}

$outputPath = if ([System.IO.Path]::IsPathRooted($Output)) {
  [System.IO.Path]::GetFullPath($Output)
} else {
  [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Output))
}
if ($outputPath.TrimEnd('\', '/') -eq $repoRoot.TrimEnd('\', '/')) {
  throw 'Output directory cannot be the repository root'
}
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$sourceOutput = ''
$repoPrefix = $repoRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if ($outputPath.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
  $relativeOutput = $outputPath.Substring($repoPrefix.Length).Replace('\', '/')
  $sourceOutput = "/workspace/$relativeOutput"
}
$runArgs = @(
  'run', '--rm', '--privileged',
  '-e', 'IN_CONTAINER=1',
  '-e', 'TERM=xterm',
  '-e', 'CCACHE_DIR=/ccache',
  '-e', "HTTP_PROXY=$containerHttpProxy",
  '-e', "HTTPS_PROXY=$containerHttpsProxy",
  '-e', "APT_HTTP_PROXY=$containerAptHttpProxy",
  '-e', "APT_HTTPS_PROXY=$containerAptHttpsProxy",
  '-e', "DEBIAN_MIRROR=$containerDebianMirror",
  '-e', "http_proxy=$containerHttpProxy",
  '-e', "https_proxy=$containerHttpsProxy",
  '-v', "${repoRoot}:/workspace:ro",
  '-v', "${repoRoot}/scripts:/builder:ro",
  '-v', "${repoRoot}/configs:/configs:ro",
  '-v', "${outputPath}:/output",
  '-v', 'sg2002-sdk:/sdk-cache',
  '-v', 'sg2002-build:/build-cache',
  '-v', 'sg2002-ccache:/ccache',
  '-w', '/workspace',
  '--entrypoint', '/bin/bash',
  $image,
  '/workspace/scripts/ci/local-build.sh', $Target, '--inside',
  '--board', $Board, '--storage', $Storage, '--output', '/output'
)
if ($sourceOutput) {
  $runArgs += @('--source-output', $sourceOutput)
}
$previousPreference = $ErrorActionPreference
try {
  # Native tools routinely write progress and warnings to stderr. Their exit
  # status, rather than the PowerShell error stream, determines success.
  $ErrorActionPreference = 'Continue'
  & docker @runArgs
  $dockerExitCode = $LASTEXITCODE
} finally {
  $ErrorActionPreference = $previousPreference
}
if ($dockerExitCode -ne 0) { throw "Local build target '$Target' failed with exit code $dockerExitCode" }
