# sign-marketlab-dev.ps1 - LOCAL DEVELOPMENT Authenticode signing for
# MarketLab-owned Windows binaries.
#
# LOCAL TRUST ONLY. This signs with a self-signed certificate installed in the
# current user's Root and TrustedPublisher stores on this machine. It does NOT
# establish public trust and does NOT guarantee Smart App Control acceptance:
# SAC accepts RSA certificates from trusted providers, so a locally trusted
# self-signed certificate may still be blocked. Never use this certificate or
# this script for release artifacts.
#
# Only files without an existing valid signature are signed. Valid Qt,
# Microsoft or other third-party signatures are preserved. MarketLab-owned
# files are selected by name; qgeoview.dll is an explicitly approved bundled
# third-party exception ($BundledThirdPartyExceptions). Other bundled
# third-party files are signed only with -IncludeBundledThirdParty, after an
# actual Smart App Control block.
#
# Usage:
#   powershell -File packaging/windows/sign-marketlab-dev.ps1 -Path build/win-dev
#   powershell -File packaging/windows/sign-marketlab-dev.ps1 -Path <installer.exe> -Path <app-dir>
#
# The certificate's private key stays in the current user's certificate store;
# no .pfx is written and no secret material enters Git.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]]$Path,

    [string]$CertSubject = "CN=MarketLab Development Code Signing (LOCAL ONLY)",
    [string]$Thumbprint,
    [string]$TimestampUrl = "",
    [switch]$IncludeBundledThirdParty,
    [switch]$Recurse,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$MarketLabOwnedNames = @(
    "MarketLabTerminal.exe",
    "MarketLabMaintenanceTool.exe"
)

# Explicitly approved bundled third-party exception: QGeoView ships inside the
# MarketLab package, is not MarketLab-owned, and was the confirmed Smart App
# Control launch blocker. It is signed only because of that exception. Do not
# add other bundled third-party files here; pass -IncludeBundledThirdParty
# only after an actual SAC block and with that file explicitly named.
$BundledThirdPartyExceptions = @(
    "qgeoview.dll"
)

function Get-SignTool {
    $candidates = @()
    if ($env:WindowsSdkVerBinPath) {
        $candidates += Join-Path $env:WindowsSdkVerBinPath "x64\signtool.exe"
    }
    $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    if (Test-Path $kits) {
        $candidates += Get-ChildItem -Path (Join-Path $kits "*\x64\signtool.exe") -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            ForEach-Object { $_.FullName }
    }
    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $found) {
        throw "signtool.exe not found (Windows SDK required)"
    }
    return $found
}

function Get-DevSigningCert {
    $certs = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object { $_.HasPrivateKey }
    if ($Thumbprint) {
        $cert = $certs | Where-Object { $_.Thumbprint -eq $Thumbprint } | Select-Object -First 1
    } else {
        $cert = $certs | Where-Object { $_.Subject -eq $CertSubject } |
            Sort-Object NotAfter -Descending | Select-Object -First 1
    }
    if (-not $cert) {
        throw "No local development signing certificate found (subject '$CertSubject', thumbprint '$Thumbprint')"
    }
    return $cert
}

function Repair-DanglingCertificateTable {
    # Qt IFW generates the maintenance tool from the installer's PE header, so
    # installing from an Authenticode-signed setup leaves the tool with a
    # certificate-table pointer that points past EOF (signtool then fails with
    # 0x800700C1). Restore that invalid entry to "no signature" before signing.
    param([string]$FilePath)
    $bytes = [IO.File]::ReadAllBytes($FilePath)
    if ($bytes.Length -lt 0x40) { return $false }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -le 0 -or ($pe + 24 + 160) -gt $bytes.Length) { return $false }
    $magic = [BitConverter]::ToUInt16($bytes, $pe + 24)
    if ($magic -eq 0x20B) {
        $certOffset = $pe + 24 + 112 + 32
    } else {
        $certOffset = $pe + 24 + 96 + 32
    }
    if (($certOffset + 8) -gt $bytes.Length) { return $false }
    $certVa = [BitConverter]::ToUInt32($bytes, $certOffset)
    $certSize = [BitConverter]::ToUInt32($bytes, $certOffset + 4)
    if ($certVa -eq 0 -and $certSize -eq 0) { return $false }
    if (([int64]$certVa + [int64]$certSize) -le $bytes.Length) { return $false }
    for ($i = 0; $i -lt 8; $i++) { $bytes[$certOffset + $i] = 0 }
    [IO.File]::WriteAllBytes($FilePath, $bytes)
    return $true
}

function Resolve-TargetFiles {
    $files = @()
    foreach ($item in $Path) {
        if (Test-Path -LiteralPath $item -PathType Container) {
            $files += Get-ChildItem -LiteralPath $item -File -Recurse:$Recurse |
                Where-Object { $_.Extension -in ".exe", ".dll" } |
                ForEach-Object { [PSCustomObject]@{ File = $_; Explicit = $false } }
        } elseif (Test-Path -LiteralPath $item -PathType Leaf) {
            $files += [PSCustomObject]@{ File = Get-Item -LiteralPath $item; Explicit = $true }
        } else {
            throw "Path not found: $item"
        }
    }
    return $files
}

$signTool = Get-SignTool
$cert = Get-DevSigningCert

Write-Host "MarketLab development signing - LOCAL TRUST ONLY (not public trust)" -ForegroundColor Yellow
Write-Host ("  Certificate : {0}" -f $cert.Subject)
Write-Host ("  Thumbprint  : {0}" -f $cert.Thumbprint)
Write-Host ("  Valid until : {0}" -f $cert.NotAfter)
Write-Host ("  signtool    : {0}" -f $signTool)
Write-Host ""

$results = @()
$failed = $false

foreach ($entry in Resolve-TargetFiles) {
    $file = $entry.File
    $name = $file.Name
    $sig = Get-AuthenticodeSignature -LiteralPath $file.FullName
    $isOwned = $MarketLabOwnedNames -contains $name
    $isBundledException = $BundledThirdPartyExceptions -contains $name
    $eligible = $entry.Explicit -or $isOwned -or $isBundledException -or $IncludeBundledThirdParty
    if ($entry.Explicit) {
        $class = "explicit"
    } elseif ($isOwned) {
        $class = "marketlab-owned"
    } elseif ($isBundledException) {
        $class = "bundled-exception"
    } else {
        $class = "third-party"
    }

    $action = $null
    if ($sig.Status -eq "Valid" -and $sig.SignerCertificate -and $sig.SignerCertificate.Thumbprint -eq $cert.Thumbprint) {
        $action = "skip-already-signed"
    } elseif ($sig.Status -eq "Valid") {
        $action = "skip-third-party-signed"
    } elseif (-not $eligible) {
        $action = "skip-not-owned-or-approved"
    } elseif ($sig.Status -eq "Invalid" -and -not $Force) {
        $action = "skip-invalid-use-force"
        $failed = $true
    } else {
        $action = "sign"
    }

    if ($action -eq "sign") {
        if (Repair-DanglingCertificateTable -FilePath $file.FullName) {
            Write-Host ("  repaired dangling certificate-table pointer in {0}" -f $name) -ForegroundColor Yellow
        }
        $signArgs = @("sign", "/fd", "SHA256", "/sha1", $cert.Thumbprint, "/v")
        if ($TimestampUrl) {
            $signArgs += @("/tr", $TimestampUrl, "/td", "SHA256")
        }
        $signArgs += $file.FullName
        & $signTool @signArgs | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $failed = $true
            $results += [PSCustomObject]@{ Class = $class; File = $file.FullName; Before = $sig.Status; Action = "SIGN FAILED"; After = ""; Signer = "" }
            continue
        }
        $after = Get-AuthenticodeSignature -LiteralPath $file.FullName
        $ok = $after.Status -eq "Valid" -and $after.SignerCertificate -and
              $after.SignerCertificate.Thumbprint -eq $cert.Thumbprint
        if (-not $ok) {
            $failed = $true
        }
        $results += [PSCustomObject]@{
            Class = $class
            File = $file.FullName
            Before = $sig.Status
            Action = if ($ok) { "signed" } else { "VERIFY FAILED" }
            After = $after.Status
            Signer = if ($after.SignerCertificate) { $after.SignerCertificate.Subject } else { "" }
        }
    } else {
        $results += [PSCustomObject]@{
            Class = $class
            File = $file.FullName
            Before = $sig.Status
            Action = $action
            After = $sig.Status
            Signer = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { "" }
        }
    }
}

$results | Format-Table Class, File, Before, Action, After -AutoSize | Out-String -Width 240 | Write-Host

$signedCount = ($results | Where-Object { $_.Action -eq "signed" }).Count
if ($failed) {
    Write-Host ("Signed {0} file(s); one or more failures occurred." -f $signedCount) -ForegroundColor Red
    exit 1
}
Write-Host ("Signed {0} file(s); no failures." -f $signedCount) -ForegroundColor Green
