# Deshace el ultimo lote de la IA (undo_last_batch) y muestra el resultado.
$ErrorActionPreference = "Stop"
$out = Join-Path $PSScriptRoot "deshacer_resultado.txt"
try {
    $bridge = Get-Content (Join-Path $env:LOCALAPPDATA "GeometryDash\geode\mods\grupo.practice-runs\ai-bridge.json") -Raw | ConvertFrom-Json
    $h = @{ Authorization = "Bearer $($bridge.token)"; "Idempotency-Key" = "undo-" + [guid]::NewGuid() }
    try {
        $r = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$($bridge.port)/api/undo_last_batch" -Headers $h -Body "{}" -ContentType "application/json"
        $text = $r | ConvertTo-Json -Compress
    } catch {
        $text = "ERROR: " + $_.ErrorDetails.Message
    }
} catch {
    $text = "ERROR: " + $_.Exception.Message
}
$text | Out-File $out -Encoding utf8
Write-Host $text
