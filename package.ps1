# packs the built client into the zip the launcher downloads and publishes it on the site.
#
#   .\package.ps1 -Version 2016.09.08.1
#   .\package.ps1 -Version 2016.09.08.1 -SiteRoot D:\somewhere\htdocs
#
# makes <SiteRoot>\uploads\setup\<Version>-NoneRevPlayer.zip, writes version.txt (what
# /setup/version returns) and copies NoneRevLauncher.exe there for /setup/download.
# place files (*.rbxl) in content are NOT packed, only the engine content is.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$SiteRoot = "C:\xampp\htdocs",
    [string]$BuildRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = "Stop"
$client = Join-Path $BuildRoot "WindowsClient\Win32\Release"
$content = Join-Path $BuildRoot "content"
$launcher = Join-Path $BuildRoot "NoneRevLauncher\Win32\Release\NoneRevLauncher.exe"
$setup = Join-Path $SiteRoot "uploads\setup"
$stage = Join-Path $env:TEMP ("nonerev-pack-" + [guid]::NewGuid().ToString("n"))

if ($Version -notmatch '^[A-Za-z0-9._-]{1,40}$') { throw "version can only have letters, digits, . _ -" }
if (-not (Test-Path (Join-Path $client "RobloxPlayerBeta.exe"))) { throw "no RobloxPlayerBeta.exe in $client, build the client first" }

New-Item -ItemType Directory -Force $setup | Out-Null
New-Item -ItemType Directory -Force $stage | Out-Null

# the exe + the dlls it needs. AppSettings.xml is NOT included, the launcher writes its own
$files = @("RobloxPlayerBeta.exe", "Log.dll", "SDL2.dll", "boost.dll", "d3dcompiler_47.dll", "fmod.dll", "openvr_api.dll", "VMProtectSDK32.dll", "ReflectionMetadata.xml")
foreach ($f in $files) {
    $src = Join-Path $client $f
    if (Test-Path $src) { Copy-Item $src $stage } else { Write-Warning "skipping $f (not in $client)" }
}
if (Test-Path (Join-Path $client "ClientSettings")) { Copy-Item (Join-Path $client "ClientSettings") (Join-Path $stage "ClientSettings") -Recurse }

# engine content without the place files
$contentDest = Join-Path $stage "content"
robocopy $content $contentDest /E /XF *.rbxl *.rbxlx /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed copying content" }

# the engine also wants PlatformContent\pc next to content (content/../PlatformContent/pc/),
# thats where the pc textures and terrain live. without it the client dies on startup
$platform = Join-Path $BuildRoot "PlatformContent\pc"
if (-not (Test-Path $platform)) { throw "no PlatformContent\pc in $BuildRoot" }
robocopy $platform (Join-Path $stage "PlatformContent\pc") /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed copying PlatformContent" }

# compiled shaders, loaded from content/../shaders. without them the renderer cant init and the
# client shows the "graphics drivers seem to be too old" box. only the packs + json, not source/
$shaders = Join-Path $BuildRoot "shaders"
if (-not (Test-Path (Join-Path $shaders "shaders.json"))) { throw "no shaders\shaders.json in $BuildRoot" }
New-Item -ItemType Directory -Force (Join-Path $stage "shaders") | Out-Null
Copy-Item (Join-Path $shaders "shaders.json") (Join-Path $stage "shaders")
Copy-Item (Join-Path $shaders "shaders_*.pack") (Join-Path $stage "shaders")

$zip = Join-Path $setup ("$Version-NoneRevPlayer.zip")
if (Test-Path $zip) { Remove-Item $zip }
Write-Host "zipping to $zip (this takes a bit)..."
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item $stage -Recurse -Force

[IO.File]::WriteAllText((Join-Path $setup "version.txt"), $Version)
# linux launcher script, served by /setup/download?os=linux
Copy-Item (Join-Path $PSScriptRoot "linux/nonerev") (Join-Path $setup "nonerev")
if (Test-Path $launcher) {
    Copy-Item $launcher (Join-Path $setup "NoneRevLauncher.exe")
} else {
    Write-Warning "launcher exe not found at $launcher, build NoneRevLauncher in Release and run this again (or copy it to $setup yourself)"
}

$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "done. version $Version, $size MB. /setup/version now says $Version"
