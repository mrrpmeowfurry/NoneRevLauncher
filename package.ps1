param(
    [Parameter(Position = 0)][string]$Label = "",
    [string]$Version = "",
    [string]$SiteRoot = "C:\xampp\htdocs",
    [string]$BuildRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$Webhook = "",
    [string]$Apk = "",
    [switch]$NoAndroid,
    [string]$LinuxTar = "",
    [switch]$NoLinux,
    [switch]$NoUpload,
    [string]$R2AccountId = "",
    [string]$R2AccessKey = "",
    [string]$R2SecretKey = "",
    [string]$R2Bucket = "nonerev"
)

$ErrorActionPreference = "Stop"
$client = Join-Path $BuildRoot "WindowsClient\Win32\Release"
$content = Join-Path $BuildRoot "content"
$launcher = Join-Path $BuildRoot "NoneRevLauncher\Win32\Release\NoneRevLauncher.exe"
$setup = Join-Path $SiteRoot "uploads\setup"
$stage = Join-Path $env:TEMP ("nonerev-pack-" + [guid]::NewGuid().ToString("n"))
if ([string]::IsNullOrWhiteSpace($Apk)) { $Apk = Join-Path $BuildRoot "Android\NativeShell\build\outputs\apk\NativeShell-dev-debug.apk" }
if ([string]::IsNullOrWhiteSpace($LinuxTar)) { $LinuxTar = Join-Path $BuildRoot "LinuxClient\NoneRevPlayer-linux-x86_64.tar.gz" }

# package.ps1 -> 2026-09-30-1432, package.ps1 optism-test -> 2026-09-30-optism-test
if ([string]::IsNullOrWhiteSpace($Version)) {
    if ([string]::IsNullOrWhiteSpace($Label)) {
        $Version = Get-Date -Format "yyyy-MM-dd-HHmm"
    } else {
        if ($Label -notmatch '^[A-Za-z0-9._-]{1,29}$') { throw "label can only have letters, digits, . _ - and be at most 29 long" }
        $Version = (Get-Date -Format "yyyy-MM-dd") + "-" + $Label
    }
}
if ($Version -notmatch '^[A-Za-z0-9._-]{1,40}$') { throw "version can only have letters, digits, . _ -" }
if (-not (Test-Path (Join-Path $client "RobloxPlayerBeta.exe"))) { throw "no RobloxPlayerBeta.exe in $client, build the client first" }

# r2 creds: params, then NONEREV_R2_* env vars, then C:\xampp\r2_credentials.txt (account id, key id, secret)
function Get-R2Credentials {
    $id = $R2AccountId; $key = $R2AccessKey; $secret = $R2SecretKey
    if ([string]::IsNullOrWhiteSpace($id))     { $id = $env:NONEREV_R2_ACCOUNT_ID }
    if ([string]::IsNullOrWhiteSpace($key))    { $key = $env:NONEREV_R2_ACCESS_KEY }
    if ([string]::IsNullOrWhiteSpace($secret)) { $secret = $env:NONEREV_R2_SECRET_KEY }
    if ([string]::IsNullOrWhiteSpace($id) -or [string]::IsNullOrWhiteSpace($key) -or [string]::IsNullOrWhiteSpace($secret)) {
        $credFile = "C:\xampp\r2_credentials.txt"
        if (Test-Path $credFile) {
            $lines = @(Get-Content $credFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" -and -not $_.StartsWith("#") })
            if ($lines.Count -ge 3) { $id = $lines[0]; $key = $lines[1]; $secret = $lines[2] }
        }
    }
    if ([string]::IsNullOrWhiteSpace($id) -or [string]::IsNullOrWhiteSpace($key) -or [string]::IsNullOrWhiteSpace($secret)) { return $null }
    return @{ AccountId = $id; AccessKey = $key; SecretKey = $secret }
}

function Get-HexSha256([byte[]]$bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace "-", "").ToLower() } finally { $sha.Dispose() }
}

function Get-HmacSha256([byte[]]$key, [string]$data) {
    $h = New-Object Security.Cryptography.HMACSHA256
    $h.Key = $key
    try { return $h.ComputeHash([Text.Encoding]::UTF8.GetBytes($data)) } finally { $h.Dispose() }
}

# sigv4, $headers needs host, x-amz-date and x-amz-content-sha256
function Get-SigV4Authorization([string]$method, [string]$path, [string]$query, [hashtable]$headers, [string]$payloadHash,
                                [string]$accessKey, [string]$secretKey, [string]$region, [string]$service, [string]$amzDate) {
    $names = @($headers.Keys | ForEach-Object { $_.ToLower() } | Sort-Object)
    $canonicalHeaders = ($names | ForEach-Object {
        $k = $_
        $v = ($headers.GetEnumerator() | Where-Object { $_.Key.ToLower() -eq $k } | Select-Object -First 1).Value
        "${k}:" + (($v -replace "\s+", " ").Trim())
    }) -join "`n"
    $signedHeaders = $names -join ";"
    $canonicalRequest = "$method`n$path`n$query`n$canonicalHeaders`n`n$signedHeaders`n$payloadHash"
    $dateStamp = $amzDate.Substring(0, 8)
    $scope = "$dateStamp/$region/$service/aws4_request"
    $stringToSign = "AWS4-HMAC-SHA256`n$amzDate`n$scope`n" + (Get-HexSha256 ([Text.Encoding]::UTF8.GetBytes($canonicalRequest)))
    $kDate    = Get-HmacSha256 ([Text.Encoding]::UTF8.GetBytes("AWS4$secretKey")) $dateStamp
    $kRegion  = Get-HmacSha256 $kDate $region
    $kService = Get-HmacSha256 $kRegion $service
    $kSigning = Get-HmacSha256 $kService "aws4_request"
    $signature = ([BitConverter]::ToString((Get-HmacSha256 $kSigning $stringToSign)) -replace "-", "").ToLower()
    return "AWS4-HMAC-SHA256 Credential=$accessKey/$scope, SignedHeaders=$signedHeaders, Signature=$signature"
}

function Get-ContentType([string]$file) {
    switch ([IO.Path]::GetExtension($file).ToLower()) {
        ".zip" { return "application/zip" }
        ".apk" { return "application/vnd.android.package-archive" }
        ".gz"  { return "application/gzip" }
        ".txt" { return "text/plain" }
        default { return "application/octet-stream" }
    }
}

function Send-ToR2([hashtable]$cred, [string]$bucket, [string]$key, [string]$file) {
    $hostName = "$($cred.AccountId).r2.cloudflarestorage.com"
    $path = "/" + ((($bucket + "/" + $key).Split("/") | ForEach-Object { [Uri]::EscapeDataString($_) }) -join "/")
    $amzDate = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
    $payloadHash = "UNSIGNED-PAYLOAD"
    $headers = @{ "host" = $hostName; "x-amz-date" = $amzDate; "x-amz-content-sha256" = $payloadHash }
    $auth = Get-SigV4Authorization "PUT" $path "" $headers $payloadHash $cred.AccessKey $cred.SecretKey "auto" "s3" $amzDate

    $length = (Get-Item $file).Length
    $req = [Net.HttpWebRequest]::Create("https://$hostName$path")
    $req.Method = "PUT"
    $req.ContentType = Get-ContentType $file
    $req.ContentLength = $length
    $req.AllowWriteStreamBuffering = $false
    $req.Timeout = 3600000
    $req.ReadWriteTimeout = 3600000
    $req.Headers.Add("x-amz-date", $amzDate)
    $req.Headers.Add("x-amz-content-sha256", $payloadHash)
    $req.Headers.Add("Authorization", $auth)

    $in = [IO.File]::OpenRead($file)
    try {
        $out = $req.GetRequestStream()
        try {
            $buf = New-Object byte[] (4MB)
            $sent = [long]0; $lastPct = -1
            while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                $out.Write($buf, 0, $n)
                $sent += $n
                $pct = [int](100 * $sent / [math]::Max($length, 1))
                if ($length -gt 20MB -and $pct -ne $lastPct -and $pct % 10 -eq 0) { Write-Host "    $pct%"; $lastPct = $pct }
            }
        } finally { $out.Dispose() }
        $resp = $req.GetResponse()
        try {
            if ([int]$resp.StatusCode -ne 200) { throw "R2 answered $([int]$resp.StatusCode) for $key" }
        } finally { $resp.Dispose() }
    } catch [Net.WebException] {
        $body = ""
        if ($_.Exception.Response) {
            $rs = $_.Exception.Response.GetResponseStream()
            $body = (New-Object IO.StreamReader($rs)).ReadToEnd()
        }
        throw "upload of $key failed: $($_.Exception.Message) $body"
    } finally { $in.Dispose() }
}

New-Item -ItemType Directory -Force $setup | Out-Null
New-Item -ItemType Directory -Force $stage | Out-Null

$files = @("RobloxPlayerBeta.exe", "Log.dll", "SDL2.dll", "boost.dll", "d3dcompiler_47.dll", "fmod.dll", "openvr_api.dll", "VMProtectSDK32.dll", "ReflectionMetadata.xml")
foreach ($f in $files) {
    $src = Join-Path $client $f
    if (Test-Path $src) { Copy-Item $src $stage } else { Write-Warning "skipping $f (not in $client)" }
}
if (Test-Path (Join-Path $client "ClientSettings")) { Copy-Item (Join-Path $client "ClientSettings") (Join-Path $stage "ClientSettings") -Recurse }

$contentDest = Join-Path $stage "content"
robocopy $content $contentDest /E /XF *.rbxl *.rbxlx /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed copying content" }

$platform = Join-Path $BuildRoot "PlatformContent\pc"
if (-not (Test-Path $platform)) { throw "no PlatformContent\pc in $BuildRoot" }
robocopy $platform (Join-Path $stage "PlatformContent\pc") /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed copying PlatformContent" }

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
if (Test-Path $launcher) {
    Copy-Item $launcher (Join-Path $setup "NoneRevLauncher.exe")
} else {
    Write-Warning "launcher exe not found at $launcher, build NoneRevLauncher in Release and run this again (or copy it to $setup yourself)"
}

$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "done. version $Version, $size MB. /setup/version now says $Version"

$android = Join-Path $setup "android"
$apkVersioned = $null
$apkSize = 0
if (-not $NoAndroid) {
    if (Test-Path $Apk) {
        New-Item -ItemType Directory -Force $android | Out-Null
        $apkVersioned = Join-Path $android "$Version-NoneRevPlayer.apk"
        Copy-Item $Apk $apkVersioned -Force
        Copy-Item $Apk (Join-Path $android "NoneRevPlayer.apk") -Force
        [IO.File]::WriteAllText((Join-Path $android "version.txt"), $Version)
        $apkSize = [math]::Round((Get-Item $apkVersioned).Length / 1MB, 1)
        Write-Host "android: $apkVersioned ($apkSize MB)"
    } else {
        Write-Warning "no apk at $Apk, android client left out. Build it with AndroidTools\build_android.cmd or pass -Apk <path>"
    }
}

$linux = Join-Path $setup "linux"
$linuxVersioned = $null
$linuxSize = 0
if (-not $NoLinux) {
    if (Test-Path $LinuxTar) {
        if ((Get-Item $LinuxTar).LastWriteTime -lt (Get-Item (Join-Path $client "RobloxPlayerBeta.exe")).LastWriteTime) {
            Write-Warning "$LinuxTar is older than the windows build, rebuild it in wsl if the code changed"
        }
        New-Item -ItemType Directory -Force $linux | Out-Null
        $linuxVersioned = Join-Path $linux "$Version-NoneRevPlayer-linux-x86_64.tar.gz"
        Copy-Item $LinuxTar $linuxVersioned -Force
        Copy-Item $LinuxTar (Join-Path $linux "NoneRevPlayer-linux-x86_64.tar.gz") -Force
        [IO.File]::WriteAllText((Join-Path $linux "version.txt"), $Version)
        $linuxSize = [math]::Round((Get-Item $linuxVersioned).Length / 1MB, 1)
        Write-Host "linux: $linuxVersioned ($linuxSize MB)"
    } else {
        Write-Warning "no linux tarball at $LinuxTar, linux client left out. Pass -LinuxTar <path> or -NoLinux"
    }
}

$uploaded = $false
if ($NoUpload) {
    Write-Host "-NoUpload given, not touching R2."
} else {
    $cred = Get-R2Credentials
    if ($null -eq $cred) {
        Write-Warning "no R2 credentials (pass -R2AccountId/-R2AccessKey/-R2SecretKey, set NONEREV_R2_ACCOUNT_ID/NONEREV_R2_ACCESS_KEY/NONEREV_R2_SECRET_KEY, or create C:\xampp\r2_credentials.txt). Files are only in $setup"
    } else {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $uploads = @(
            @{ Key = "setup/$Version-NoneRevPlayer.zip"; File = $zip }
        )
        if (Test-Path (Join-Path $setup "NoneRevLauncher.exe")) { $uploads += @{ Key = "setup/NoneRevLauncher.exe"; File = (Join-Path $setup "NoneRevLauncher.exe") } }
        if ($apkVersioned) {
            $uploads += @{ Key = "setup/android/$Version-NoneRevPlayer.apk"; File = $apkVersioned }
            $uploads += @{ Key = "setup/android/NoneRevPlayer.apk";         File = (Join-Path $android "NoneRevPlayer.apk") }
            $uploads += @{ Key = "setup/android/version.txt";               File = (Join-Path $android "version.txt") }
        }
        if ($linuxVersioned) {
            $uploads += @{ Key = "setup/linux/$Version-NoneRevPlayer-linux-x86_64.tar.gz"; File = $linuxVersioned }
            $uploads += @{ Key = "setup/linux/NoneRevPlayer-linux-x86_64.tar.gz";         File = (Join-Path $linux "NoneRevPlayer-linux-x86_64.tar.gz") }
            $uploads += @{ Key = "setup/linux/version.txt";                               File = (Join-Path $linux "version.txt") }
        }
        # version.txt last so nobody sees the version before the zip
        $uploads += @{ Key = "setup/version.txt"; File = (Join-Path $setup "version.txt") }

        Write-Host "uploading to r2://$R2Bucket ..."
        foreach ($u in $uploads) {
            $mb = [math]::Round((Get-Item $u.File).Length / 1MB, 1)
            Write-Host "  $($u.Key) ($mb MB)"
            Send-ToR2 $cred $R2Bucket $u.Key $u.File
        }
        $uploaded = $true
        Write-Host "uploaded $($uploads.Count) files to r2://$R2Bucket/setup"
    }
}

$hook = $Webhook
if ([string]::IsNullOrWhiteSpace($hook)) { $hook = $env:NONEREV_DISCORD_WEBHOOK }
if ([string]::IsNullOrWhiteSpace($hook)) {
    $hookFile = "C:\xampp\discord_webhook.txt"
    if (Test-Path $hookFile) { $hook = (Get-Content -Raw $hookFile).Trim() }
}

if (-not [string]::IsNullOrWhiteSpace($hook)) {
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

        $zipName = Split-Path -Leaf $zip
        $fields = @(
            @{ name = "Version";  value = "``$Version``";        inline = $true }
            @{ name = "Size";     value = "$size MB";            inline = $true }
            @{ name = "File";     value = "``$zipName``";        inline = $true }
        )
        if ($apkVersioned) { $fields += @{ name = "Android"; value = "``$(Split-Path -Leaf $apkVersioned)`` ($apkSize MB)"; inline = $true } }
        if ($linuxVersioned) { $fields += @{ name = "Linux"; value = "``$(Split-Path -Leaf $linuxVersioned)`` ($linuxSize MB)"; inline = $true } }
        $where = "local only"
        if ($uploaded) { $where = "r2://$R2Bucket/setup" }
        $fields += @{ name = "Uploaded to"; value = $where; inline = $true }
        $fields += @{ name = "Packed by"; value = "$env:USERNAME@$env:COMPUTERNAME"; inline = $false }
        $embed = @{
            title       = "NoneRev client packaged"
            description = "``$Version`` is now available. The bootstrapper will pull it on next launch/install."
            color       = 15417396
            fields      = $fields
            timestamp   = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
            footer      = @{ text = "NoneRev packager" }
        }
        $payload = @{ username = "NoneRev Builds"; embeds = @($embed) } | ConvertTo-Json -Depth 6

        $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
        Invoke-RestMethod -Uri $hook -Method Post -ContentType "application/json; charset=utf-8" -Body $bytes | Out-Null
        Write-Host "announced $Version on discord."
    } catch {
        Write-Warning "discord announcement failed (packaging still succeeded): $($_.Exception.Message)"
    }
} else {
    Write-Host "no discord webhook set (pass -Webhook, set NONEREV_DISCORD_WEBHOOK, or create C:\xampp\discord_webhook.txt), skipping announcement."
}
