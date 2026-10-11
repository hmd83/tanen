# Upload the landing page (site/) to https://tanen.eu/ over FTPS.
#
#   .\scripts\deploy_site.ps1                               # every file in site/
#   .\scripts\deploy_site.ps1 index.html media/x.jpg        # just these
#
# Same steps as deploy_web.ps1: password from repo-root secrets.pw, each file
# backed up from the server first, then uploaded, downloaded again and
# compared. Paths are relative to site/ and keep their folders (media/...).
#
# Check before uploading: the live page can be ahead of site/ - diff
# https://tanen.eu/ against site/index.html, or this overwrites it.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Files)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$pwFile = Join-Path $repo 'secrets.pw'
if (-not (Test-Path $pwFile)) { throw "secrets.pw missing in $repo" }
$cred = 'tanen@tanen.eu:' + (Get-Content $pwFile -Raw).Trim()
$base = 'ftp://ftp.tanen.eu/public_html'
$site = Join-Path $repo 'site'
if (-not $Files) {
    $Files = Get-ChildItem $site -File -Recurse |
        ForEach-Object { $_.FullName.Substring($site.Length + 1).Replace('\', '/') }
}

$backup = Join-Path ([IO.Path]::GetTempPath()) ('tanen-site-' + (Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $backup | Out-Null

foreach ($f in $Files) {
    $f = $f.Replace('\', '/')
    $local = Join-Path $site $f
    if (-not (Test-Path $local)) { throw "no such file: site/$f" }

    & curl.exe -s -k --ssl-reqd --create-dirs -u $cred -o (Join-Path $backup $f) "$base/$f"
    if ($LASTEXITCODE -eq 67) { throw 'FTP login refused - put the current password in secrets.pw' }
    if ($LASTEXITCODE -eq 78) { Write-Host "new  $f (not on the server yet)" }
    elseif ($LASTEXITCODE) { throw "backup of $f failed (curl $LASTEXITCODE)" }

    & curl.exe -sS -k --ssl-reqd --ftp-create-dirs -u $cred -T $local "$base/$f"
    if ($LASTEXITCODE) { throw "upload of $f failed (curl $LASTEXITCODE)" }

    $check = Join-Path $backup "$f.check"
    & curl.exe -sS -k --ssl-reqd --create-dirs -u $cred -o $check "$base/$f"
    if ($LASTEXITCODE -or (Get-FileHash $local).Hash -ne (Get-FileHash $check).Hash) {
        throw "$f does not match on the server after upload"
    }
    Write-Host "OK   $f"
}
Write-Host "Live: https://tanen.eu/  (previous copies: $backup)"
Write-Host 'The proxy cache may serve the old page for a minute or two.'
