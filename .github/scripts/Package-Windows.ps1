[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
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
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            "${ProjectRoot}/release/${ProductName}-*-windows-*.exe"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    # Build the Inno Setup installer next to the zip.
    $InstallerName = "${OutputName}-installer"
    $InstallerPath = "${ProjectRoot}/release/${InstallerName}.exe"

    Log-Group "Building installer for ${ProductName}..."

    $Iscc = Get-Command -Name iscc -ErrorAction SilentlyContinue
    if ( $null -eq $Iscc ) {
        $Candidates = @(
            "${Env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
            "${Env:ProgramFiles}\Inno Setup 6\ISCC.exe"
        ) | Where-Object { $_ -and ( Test-Path $_ ) }
        if ( $Candidates.Count -eq 0 ) {
            Write-Host 'Inno Setup not found, installing via Chocolatey...'
            choco install innosetup --version 6.4.3 --no-progress -y
            if ( $LASTEXITCODE -ne 0 ) { throw "choco install innosetup failed (${LASTEXITCODE})" }
            $Candidates = @(
                "${Env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
                "${Env:ProgramFiles}\Inno Setup 6\ISCC.exe"
            ) | Where-Object { $_ -and ( Test-Path $_ ) }
        }
        if ( $Candidates.Count -eq 0 ) { throw 'ISCC.exe not found after install' }
        $IsccPath = $Candidates[0]
    } else {
        $IsccPath = $Iscc.Source
    }

    $SourceDir = ( Resolve-Path -Path "${ProjectRoot}/release/${Configuration}/${ProductName}" ).Path
    $IsccArgs = @(
        "/DAppVersion=${ProductVersion}"
        "/DSourceDir=${SourceDir}"
        "/DOutputDir=${ProjectRoot}/release"
        "/DOutputName=${InstallerName}"
        "/DLicenseFile=${ProjectRoot}/LICENSE"
    )

    # Pass the branding graphics (icon + wizard BMPs) when present so the
    # installer is branded; the .iss gates all graphics on this define.
    $AssetsDir = Join-Path $ScriptHome 'assets'
    if ( Test-Path (Join-Path $AssetsDir 'pixelion.ico') ) {
        $IsccArgs += "/DAssetsDir=${AssetsDir}"
    } else {
        Write-Warning "Installer assets not found in ${AssetsDir}; building unbranded installer."
    }

    $IsccArgs += "${ScriptHome}/windows-installer.iss"
    & $IsccPath @IsccArgs
    if ( $LASTEXITCODE -ne 0 ) { throw "iscc failed (${LASTEXITCODE})" }
    if ( ! ( Test-Path $InstallerPath ) ) { throw "Installer not produced: ${InstallerPath}" }

    # Authenticode signing extension point. Signing is NOT implemented.
    # It is skipped when WINDOWS_SIGN_CERT and WINDOWS_SIGN_PASSWORD are absent,
    # which is the current state. To enable it, add a signtool call here, e.g.
    #   signtool sign /f <pfx> /p <password> /fd SHA256 /tr <timestamp-url> /td SHA256 $InstallerPath
    if ( $Env:WINDOWS_SIGN_CERT -and $Env:WINDOWS_SIGN_PASSWORD ) {
        Log-Warning "WINDOWS_SIGN_* is set, but Authenticode signing is not implemented. ${InstallerName}.exe is UNSIGNED."
    }
    Log-Group
}

Package
