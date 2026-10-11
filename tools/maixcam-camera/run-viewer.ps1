[CmdletBinding()]
param(
    [string]$Url = 'rtsp://10.42.0.1:8554/live',
    [ValidateSet('Usb', 'Razver')]
    [string]$Transport = 'Usb',
    [string]$Python = 'python',
    [switch]$RefreshDependencies,
    [switch]$SetupOnly
)

# Creates a user-local virtual environment and starts the interactive viewer.

$ErrorActionPreference = 'Stop'

function Invoke-ViewerPython {
    param([string]$Executable, [string[]]$PythonArguments)

    # Start-Process joins ArgumentList into a Windows command line. Quote
    # every argument, including trailing backslashes and embedded quotes.
    $quotedArguments = foreach ($argument in $PythonArguments) {
        '"' + ($argument -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
    }
    $pythonProcess = Start-Process -FilePath $Executable `
        -ArgumentList ($quotedArguments -join ' ') -NoNewWindow -Wait -PassThru
    return $pythonProcess.ExitCode
}

$toolDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$requirements = Join-Path $toolDirectory 'requirements.txt'
$viewer = Join-Path $toolDirectory 'viewer.py'
$runtimeRoot = Join-Path $env:LOCALAPPDATA 'MaixCAM\camera-viewer'
$virtualEnvironment = Join-Path $runtimeRoot 'venv'
$venvPython = Join-Path $virtualEnvironment 'Scripts\python.exe'
$stamp = Join-Path $runtimeRoot 'requirements.sha256'
$requirementsHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $requirements).
    Hash.ToLowerInvariant()

New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null
if (-not (Test-Path -LiteralPath $venvPython)) {
    $basePython = (Get-Command $Python -ErrorAction Stop).Source
    $pythonExitCode = Invoke-ViewerPython $basePython @('-m', 'venv', $virtualEnvironment)
    if ($pythonExitCode -ne 0) {
        throw "failed to create viewer virtual environment: $pythonExitCode"
    }
}

$installedHash = if (Test-Path -LiteralPath $stamp) {
    (Get-Content -Raw -LiteralPath $stamp).Trim()
} else {
    ''
}
if ($RefreshDependencies -or $installedHash -ne $requirementsHash) {
    $pythonExitCode = Invoke-ViewerPython $venvPython @(
        '-m', 'pip', 'install', '--disable-pip-version-check', '--requirement', $requirements
    )
    if ($pythonExitCode -ne 0) {
        throw "failed to install viewer dependencies: $pythonExitCode"
    }
    [IO.File]::WriteAllText(
        $stamp,
        "$requirementsHash`n",
        [Text.UTF8Encoding]::new($false)
    )
}

if ($SetupOnly) {
    Write-Host "VIEWER_ENVIRONMENT=$virtualEnvironment"
    Write-Host "REQUIREMENTS_SHA256=$requirementsHash"
    exit 0
}

$pythonExitCode = Invoke-ViewerPython $venvPython @(
    $viewer, '--transport', $Transport.ToLowerInvariant(), '--url', $Url
)
exit $pythonExitCode
