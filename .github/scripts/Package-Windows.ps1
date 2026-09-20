[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    # 额外产出 NSIS 安装器(装进 OBS 的 per-plugin 目录,高版本安装时先卸旧版)
    [switch] $Package
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"
    $BuildDir = "${ProjectRoot}/build_${Target}"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip",
            "${ProjectRoot}/release/${ProductName}-*-windows-*-Installer.exe"
        )
    }

    Remove-Item @RemoveArgs

    if ( $Package ) {
        Log-Group "Building ${ProductName} NSIS installer..."
        # 手写 NSIS 安装器:自动检测 OBS 目录(注册表卸载项),双布局自适应;
        # makensis 为 CI runner 预装
        & makensis `
            "-DRELEASE_DIR=$(Resolve-Path "${ProjectRoot}/release/${Configuration}")" `
            "-DVERSION=${ProductVersion}" `
            "-DOUT_DIR=$(Resolve-Path "${ProjectRoot}/release")" `
            "${ProjectRoot}/installer.nsi"
        if ( $LASTEXITCODE -ne 0 ) {
            throw "makensis failed with exit code ${LASTEXITCODE}"
        }
        Log-Group
    }

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group
}

Package
