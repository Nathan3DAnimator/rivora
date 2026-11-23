# Publishes the Real Engine's source to its public repository (the open source release, notas-dev/servicos.md "Código
# aberto"): the files of this repository's HEAD (git archive) in a clone of the public one beside this folder
# (..\realengine-public), without what is only the author's (CLAUDE.md: the AI's instructions) and with the home folder
# in paths made anonymous ("Users\Player" -> "Users\Player": the same length, so binary test files stay valid); one
# commit per release ("Real Engine <VERSION>"), pushed. The development history stays in the private repository.
#   powershell -ExecutionPolicy Bypass -File tools/publish_source.ps1 [-Message "..."] [-NoPush]
param([string]$Message,
      [string]$Repo = "Nathan3DAnimator/realengine",
      [switch]$NoPush)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$pub = Join-Path (Split-Path -Parent $root) "realengine-public"
$ver = (Get-Content (Join-Path $root "VERSION") -TotalCount 1).Trim()
if (-not $Message) { $Message = "Real Engine $ver" }

# 1. the public clone (made once)
if (-not (Test-Path (Join-Path $pub ".git"))) {
    git clone "https://github.com/$Repo.git" $pub
    if ($LASTEXITCODE -ne 0) { throw "could not clone $Repo" }
}
# the public commits under the account's GitHub no-reply address (the author's e-mail never goes public)
$who = gh api user --jq '.login + " " + (.id|tostring)'
if ($LASTEXITCODE -eq 0 -and $who) {
    $login, $id = $who.Trim().Split(" ")
    git -C $pub config user.name $login
    git -C $pub config user.email "$id+$login@users.noreply.github.com"
}
# 2. its files replaced by HEAD's (everything but .git removed first: deleted files go too)
Get-ChildItem $pub -Force | Where-Object { $_.Name -ne ".git" } | Remove-Item -Recurse -Force -Confirm:$false
$tar = Join-Path $env:TEMP "realengine_src.tar"
git -C $root archive --format=tar -o $tar HEAD
if ($LASTEXITCODE -ne 0) { throw "git archive failed" }
tar -xf $tar -C $pub
Remove-Item $tar -Force
# 3. what is only the author's
foreach ($f in "CLAUDE.md") { $p = Join-Path $pub $f; if (Test-Path $p) { Remove-Item $p -Force } }
# 4. the home folder in paths (bytes, same length: text and binary files alike)
$from1 = [Text.Encoding]::ASCII.GetBytes("Users\Player"); $to1 = [Text.Encoding]::ASCII.GetBytes("Users\Player")
$from2 = [Text.Encoding]::ASCII.GetBytes("Users/Player"); $to2 = [Text.Encoding]::ASCII.GetBytes("Users/Player")
function Replace-Bytes([byte[]]$b, [byte[]]$a, [byte[]]$c) {
    $n = 0
    for ($i = 0; $i -le $b.Length - $a.Length; $i++) {
        $hit = $true
        for ($k = 0; $k -lt $a.Length; $k++) { if ($b[$i + $k] -ne $a[$k]) { $hit = $false; break } }
        if ($hit) { [Array]::Copy($c, 0, $b, $i, $c.Length); $n++ }
    }
    return $n
}
$changed = 0
Get-ChildItem $pub -Recurse -File -Force | Where-Object { $_.FullName -notlike "*\.git\*" -and $_.Length -lt 64MB } | ForEach-Object {
    $b = [IO.File]::ReadAllBytes($_.FullName)
    $text = [Text.Encoding]::ASCII.GetString($b)
    if ($text.Contains("Users\Player") -or $text.Contains("Users/Player")) {
        $n = (Replace-Bytes $b $from1 $to1) + (Replace-Bytes $b $from2 $to2)
        if ($n) { [IO.File]::WriteAllBytes($_.FullName, $b); $changed++ }
    }
}
Write-Host "Anonymous paths in $changed file(s)"
# 5. one commit, pushed
git -C $pub add -A
git -C $pub diff --cached --quiet
if ($LASTEXITCODE -eq 0) { Write-Host "Nothing changed since the last publication."; exit 0 }
git -C $pub commit -q -m $Message
if ($LASTEXITCODE -ne 0) { throw "commit failed" }
if (-not $NoPush) {
    git -C $pub push -q origin HEAD
    if ($LASTEXITCODE -ne 0) { throw "push failed" }
    Write-Host "Published: https://github.com/$Repo ($Message)"
} else { Write-Host "Committed in $pub (not pushed)" }
