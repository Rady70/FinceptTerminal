# Windows development signing (local trust only)

This directory holds the local development signing helper for Windows builds.

It exists because Smart App Control (SAC), when enforcing, blocks unsigned
binaries. The blocker observed in practice was the bundled `qgeoview.dll`
loaded at process start, so the app could not launch even though the main EXE
itself was tolerated.

**This is not public-trust signing.** A self-signed certificate installed only
in the current user's stores does not make the binaries distributable, and it
does not guarantee SAC acceptance on other machines. Release artifacts require
a certificate from a provider trusted by Windows (for example Azure Artifact
Signing, formerly Trusted Signing) or an RSA code-signing certificate from a CA
in the Microsoft Trusted Root Program. SAC does not currently accept ECC
signatures.

## Certificate (per machine, never committed)

Create a local code-signing certificate in the current user's store:

```powershell
$cert = New-SelfSignedCertificate `
    -Subject "CN=MarketLab Development Code Signing (LOCAL ONLY)" `
    -Type CodeSigningCert -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
    -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(2) `
    -CertStoreLocation Cert:\CurrentUser\My
Export-Certificate -Cert $cert -FilePath "$env:LOCALAPPDATA\MarketLab\certs\marketlab-dev-codesign.cer" -Type CERT
certutil -user -addstore Root "$env:LOCALAPPDATA\MarketLab\certs\marketlab-dev-codesign.cer"
Import-Certificate -FilePath "$env:LOCALAPPDATA\MarketLab\certs\marketlab-dev-codesign.cer" -CertStoreLocation Cert:\CurrentUser\TrustedPublisher
```

The private key stays in the user profile and is non-exportable. The thumbprint
is runtime state: pass it with `-Thumbprint` or configure
`FINCEPT_WIN_SIGN_SHA1`. Do not write the thumbprint into tracked files.

## Signing a build or package

```powershell
& packaging/windows/sign-marketlab-dev.ps1 -Path build/win-dev
& packaging/windows/sign-marketlab-dev.ps1 -Path <setup.exe> -Path <app-dir>
```

Selection:

- explicit file arguments are always considered;
- directory scans sign only `MarketLabTerminal.exe`,
  `MarketLabMaintenanceTool.exe`, and the explicitly approved `qgeoview.dll`
  (QGeoView) bundled third-party exception;
- other bundled third-party files (`libcrypto`, `libssl`, `yt-dlp`, ...) are
  never touched by directory scans;
- a `Valid` signature is preserved; `NotSigned` files are signed only when
  approved; any other signature status fails the run unless `-Force` is given.

Each result row is classified as `marketlab-owned`, `bundled-exception`,
`explicit` or `third-party`.

For the CMake build tree, the opt-in `sign-windows` target signs the EXE and
the deployed `qgeoview.dll` when either `FINCEPT_WIN_SIGN_CERT` (+
`FINCEPT_WIN_SIGN_PASSWORD`) or `FINCEPT_WIN_SIGN_SHA1` is configured.

## Qt IFW maintenance tool repair

Installing from an Authenticode-signed setup makes Qt IFW generate
`MarketLabMaintenanceTool.exe` from the installer's PE header. The generated
tool then carries a certificate-table pointer that points past EOF, and
`signtool` refuses it with `0x800700C1`. `Repair-DanglingCertificateTable`
is called only for `MarketLabMaintenanceTool.exe`; it zeroes that entry only
when it is actually outside the file, then signing proceeds normally. Files
without a certificate table and files with a valid one are left byte-identical.

## Limitations

- Local trust only; not a distribution fix.
- SAC acceptance is machine-specific and not guaranteed.
- Never commit certificates, private keys, PFX files, signed binaries,
  installers, backups or machine-specific paths.
