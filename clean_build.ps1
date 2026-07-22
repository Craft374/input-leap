
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

function Find-VisualStudio {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installation = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -format json | ConvertFrom-Json | Select-Object -First 1
        if ($null -ne $installation) {
            $major_version = [int]($installation.installationVersion -split '\.')[0]
            if ($major_version -eq 17) {
                return @{version='Visual Studio 17 2022'; path=(Join-Path $installation.installationPath 'Common7\Tools\VsDevCmd.bat')}
            }
            if ($major_version -eq 16) {
                return @{version='Visual Studio 16 2019'; path=(Join-Path $installation.installationPath 'Common7\Tools\VsDevCmd.bat')}
            }
        }
    }

    $locations = @(
        @{version='Visual Studio 17 2022'; path='C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat'},
        @{version='Visual Studio 17 2022'; path='C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'},
        @{version='Visual Studio 17 2022'; path='C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'},
        @{version='Visual Studio 16 2019'; path='C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\Common7\Tools\VsDevCmd.bat'},
        @{version='Visual Studio 16 2019'; path='C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat'},
        @{version='Visual Studio 16 2019'; path='C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat'}
    )
    foreach ($location in $locations) {
        $installation_path = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $location.path))
        $compiler = Get-Item -Path (Join-Path $installation_path 'VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe') -ErrorAction SilentlyContinue
        if ((Test-Path -LiteralPath $location.path) -and $null -ne $compiler) {
            return $location
        }
    }
    return $null
}

function Confirm-DependencyInstall($description) {
    $answer = Read-Host "$description is missing. Install it now? [Y/n]"
    return ($answer -eq '' -or $answer -match '^(?i:y(es)?)$')
}

$visual_studio = Find-VisualStudio
if ($null -eq $visual_studio) {
    if (-not (Confirm-DependencyInstall 'Visual Studio 2022 C++ Build Tools')) {
        throw 'Visual Studio 2022 C++ Build Tools are required.'
    }

    $installer = Join-Path $env:TEMP 'input-leap-vs_BuildTools.exe'
    Invoke-WebRequest 'https://aka.ms/vs/17/release/vs_BuildTools.exe' -OutFile $installer
    try {
        $process = Start-Process -FilePath $installer -Wait -PassThru -ArgumentList @(
            '--quiet', '--wait', '--norestart', '--nocache',
            '--add', 'Microsoft.VisualStudio.Workload.VCTools', '--includeRecommended'
        )
        if ($process.ExitCode -notin @(0, 3010)) {
            throw "Visual Studio Build Tools installation failed with exit code $($process.ExitCode)."
        }
    } finally {
        Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
    }
    $visual_studio = Find-VisualStudio
}

if ($null -eq $visual_studio) {
    throw 'Visual Studio Build Tools were installed but are not available yet. Restart Windows and run this file again.'
}
Write-Output "Using $($visual_studio.version) at $($visual_studio.path)"

$cmake_command = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
$cmake_path = if ($null -ne $cmake_command) { $cmake_command.Source } else { 'C:\Program Files\CMake\bin\cmake.exe' }
if (-not (Test-Path -LiteralPath $cmake_path)) {
    throw 'CMake is required. Install CMake and select "Add CMake to the system PATH".'
}

$build_type = 'Release';
if ($env:B_BUILD_TYPE -ne $null) {
    $build_type = $env:B_BUILD_TYPE;
}
$qt_major_version = '6';
if ($env:B_QT_MAJOR_VERSION -ne $null) {
    $qt_major_version = $env:B_QT_MAJOR_VERSION;
}
$qt_root = $null
if ($env:B_QT_ROOT -ne $null) {
    $qt_root = $env:B_QT_ROOT;
} else {
    $qt_root = Get-Item -Path ".\deps\Qt\$qt_major_version*\*", "C:\Qt\$qt_major_version*\*" -ErrorAction SilentlyContinue |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "lib\cmake\Qt$qt_major_version") } |
            Sort-Object -Property FullName |
            Select-Object -Last 1 -ExpandProperty FullName
}
if ($null -eq $qt_root) {
    if ($qt_major_version -ne '6' -or -not (Confirm-DependencyInstall 'Qt 6.6.3')) {
        throw 'Qt is required. Install it under C:\Qt or set B_QT_ROOT.'
    }

    $python_command = Get-Command python -CommandType Application -ErrorAction SilentlyContinue
    if ($null -eq $python_command) {
        throw 'Python is required to install Qt automatically.'
    }
    & $python_command.Source -m pip install --user --disable-pip-version-check 'aqtinstall==3.1.17'
    if ($LASTEXITCODE -ne 0) { throw 'Could not install aqtinstall.' }
    & $python_command.Source -m aqt install-qt windows desktop 6.6.3 win64_msvc2019_64 -O .\deps\Qt
    if ($LASTEXITCODE -ne 0) { throw 'Could not install Qt.' }
    $qt_root = Get-Item -Path '.\deps\Qt\6.6.3\*' |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'lib\cmake\Qt6') } |
        Select-Object -First 1 -ExpandProperty FullName
    if ($null -eq $qt_root) { throw 'Qt installation finished without a usable MSVC package.' }
}

Write-Output "Using Qt at $qt_root";

$bonjour_path = Join-Path $PSScriptRoot 'deps\BonjourSDKLike'
$bonjour_library = Join-Path $bonjour_path 'Lib\x64\dnssd.lib'
if (-not (Test-Path -LiteralPath $bonjour_library)) {
    New-Item -Force -ItemType Directory -Path .\deps | Out-Null
    $bonjour_archive = Join-Path $PSScriptRoot 'deps\BonjourSDKLike.zip'
    Invoke-WebRequest 'https://github.com/nelsonjchen/mDNSResponder/releases/download/v2019.05.08.1/x64_RelWithDebInfo.zip' -OutFile $bonjour_archive
    if (Test-Path -LiteralPath $bonjour_path) {
        Remove-Item -LiteralPath $bonjour_path -Recurse
    }
    Expand-Archive $bonjour_archive -DestinationPath $bonjour_path
    Remove-Item -LiteralPath $bonjour_archive
}

if (Test-Path -LiteralPath build) {
    Remove-Item -LiteralPath build -Recurse;
}
New-Item -Force -ItemType Directory -Path .\build | Out-Null
pushd build

try {
    $env:BONJOUR_SDK_HOME="$bonjour_path"
    & $cmake_path .. -G "$($visual_studio.version)" -A x64 `
        "-DCMAKE_BUILD_TYPE=$build_type" `
        "-DCMAKE_PREFIX_PATH=$qt_root" `
        "-DQT_DEFAULT_MAJOR_VERSION=$qt_major_version" `
        -DDNSSD_LIB="$bonjour_path\Lib\x64\dnssd.lib" `
        -DCMAKE_INSTALL_PREFIX=input-leap-install
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

    & $cmake_path --build . --parallel --config $build_type --target install
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

    $isccCommand = Get-Command ISCC -ErrorAction SilentlyContinue
    $isccPath = if ($null -ne $isccCommand) { $isccCommand.Source } else { $null }
    if ($null -eq $isccPath) {
        $defaultIscc = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
        if (Test-Path -LiteralPath $defaultIscc) {
            $isccPath = $defaultIscc
        }
    }
    if ($null -ne $isccPath) {
        & $isccPath /Qp installer-inno\input-leap.iss
        if ($LASTEXITCODE -ne 0) { throw 'Installer build failed.' }
    } else {
        Write-Warning 'Inno Setup was not found. The application was built without an installer.'
    }
} finally {
    popd
}
