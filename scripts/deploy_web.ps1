# Upload the setup web app (web/) to https://tanen.eu/setup/ over FTPS.
#
#   .\scripts\deploy_web.ps1              # every file in web/
#   .\scripts\deploy_web.ps1 index.html   # just these
#
# Password: repo-root secrets.pw (one line, gitignored). Each file is first
# backed up from the server, then uploaded, downloaded again and compared.
# The server's certificate does not match the host name, hence -k.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Files)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$pwFile = Join-Path $repo 'secrets.pw'
if (-not (Test-Path $pwFile)) { throw "secrets.pw missing in $repo" }
$cred = 'tanen@tanen.eu:' + (Get-Content $pwFile -Raw).Trim()
$base = 'ftp://ftp.tanen.eu/public_html/setup'
$web = Join-Path $repo 'web'
if (-not $Files) { $Files = Get-ChildItem $web -File | ForEach-Object Name }

$backup = Join-Path ([IO.Path]::GetTempPath()) ('tanen-setup-' + (Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $backup | Out-Null

foreach ($f in $Files) {
    $local = Join-Path $web $f
    if (-not (Test-Path $local)) { throw "no such file: web/$f" }

    & curl.exe -s -k --ssl-reqd -u $cred -o (Join-Path $backup $f) "$base/$f"
    if ($LASTEXITCODE -eq 67) { throw 'FTP login refused - put the current password in secrets.pw' }
    if ($LASTEXITCODE -eq 78) { Write-Host "new  $f (not on the server yet)" }
    elseif ($LASTEXITCODE) { throw "backup of $f failed (curl $LASTEXITCODE)" }

    & curl.exe -sS -k --ssl-reqd -u $cred -T $local "$base/$f"
    if ($LASTEXITCODE) { throw "upload of $f failed (curl $LASTEXITCODE)" }

    $check = Join-Path $backup "$f.check"
    & curl.exe -sS -k --ssl-reqd -u $cred -o $check "$base/$f"
    if ($LASTEXITCODE -or (Get-FileHash $local).Hash -ne (Get-FileHash $check).Hash) {
        throw "$f does not match on the server after upload"
    }
    Write-Host "OK   $f"
}
Write-Host "Live: https://tanen.eu/setup/  (previous copies: $backup)"
Write-Host 'The proxy cache may serve the old page for a minute or two.'
