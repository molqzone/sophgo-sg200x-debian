[CmdletBinding()]
param(
  [ValidateSet('test', 'firmware', 'modules', 'image', 'verify', 'toolchain')]
  [string]$Target = 'verify',
  [string]$Board = 'maixcam',
  [ValidateSet('sd', 'emmc')]
  [string]$Storage = 'sd',
  [string]$Output = 'output'
)

$ErrorActionPreference = 'Stop'

function Test-DockerObject {
  param([string[]]$Arguments)
  $previousPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'SilentlyContinue'
    & docker @Arguments *> $null
    return $LASTEXITCODE -eq 0
  } finally {
    $ErrorActionPreference = $previousPreference
  }
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

if (-not (Test-DockerObject -Arguments @('image', 'inspect', $image))) {
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
  $buildArgs = @('build', '--build-arg', "HTTP_PROXY=$containerHttpProxy") + $buildArgs[1..($buildArgs.Count - 1)]
}
if ($containerAptHttpProxy) {
  $buildArgs = @('build', '--build-arg', "http_proxy=$containerAptHttpProxy") + $buildArgs[1..($buildArgs.Count - 1)]
}
if ($containerHttpsProxy) {
  $buildArgs = @('build', '--build-arg', "HTTPS_PROXY=$containerHttpsProxy", '--build-arg', "https_proxy=$containerHttpsProxy") + $buildArgs[1..($buildArgs.Count - 1)]
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

$outputPath = Join-Path $repoRoot $Output
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$runArgs = @(
  'run', '--rm', '--privileged',
  '-e', 'IN_CONTAINER=1',
  '-e', 'CCACHE_DIR=/ccache',
  '-e', "HTTP_PROXY=$containerHttpProxy",
  '-e', "HTTPS_PROXY=$containerHttpsProxy",
  '-e', "http_proxy=$containerAptHttpProxy",
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
& docker @runArgs
if ($LASTEXITCODE -ne 0) { throw "Local build target '$Target' failed with exit code $LASTEXITCODE" }
