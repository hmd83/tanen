<?php
// Same-origin relay for TanenBase release firmware, used by flash.html.
//
// GitHub release downloads send no CORS header, so the browser cannot fetch
// them directly. This fetches ONE asset of ONE repo, nothing else, and keeps a
// copy (releases are immutable) so GitHub is hit once per file.
//
//   fw.php?tag=v1.0.1&name=tanenbase-1.0.1.hex
//   fw.php?tag=v1.0.1&name=tanenbase-1.0.1.hex.sha256

const REPO = 'hmd83/tanen';
const MAX_BYTES = 4 * 1024 * 1024;

$tag  = $_GET['tag']  ?? '';
$name = $_GET['name'] ?? '';
if (!preg_match('/^v\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$/', $tag) ||
    !preg_match('/^tanenbase-[0-9A-Za-z.\-]+\.hex(\.sha256)?$/', $name)) {
    http_response_code(400);
    exit('bad request');
}

$dir = __DIR__ . '/fw-cache';
$file = "$dir/$tag-$name";

if (!is_file($file)) {
    $ch = curl_init('https://github.com/' . REPO . "/releases/download/$tag/$name");
    curl_setopt_array($ch, [
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_FOLLOWLOCATION => true,
        CURLOPT_MAXREDIRS => 5,
        CURLOPT_PROTOCOLS => CURLPROTO_HTTPS,
        CURLOPT_REDIR_PROTOCOLS => CURLPROTO_HTTPS,
        CURLOPT_TIMEOUT => 30,
        CURLOPT_USERAGENT => 'tanen-fw-relay',
    ]);
    $body = curl_exec($ch);
    $code = curl_getinfo($ch, CURLINFO_RESPONSE_CODE);
    curl_close($ch);
    if ($body === false || $code !== 200 || strlen($body) > MAX_BYTES) {
        http_response_code($code === 404 ? 404 : 502);
        exit('not available');
    }
    if (!is_dir($dir)) mkdir($dir, 0755);
    file_put_contents("$file.tmp", $body);
    rename("$file.tmp", $file);
}

header('Content-Type: text/plain; charset=us-ascii');
header('Cache-Control: public, max-age=31536000, immutable');
header('X-Content-Type-Options: nosniff');
readfile($file);
