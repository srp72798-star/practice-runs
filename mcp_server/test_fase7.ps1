# Prueba de la Fase 7: seguridad del puente local. No cambia el nivel.
# Resultado en test_fase7_resultado.txt
$ErrorActionPreference = "Stop"
$out = Join-Path $PSScriptRoot "test_fase7_resultado.txt"
"=== Prueba Fase 7 (seguridad) $(Get-Date -Format s) ===" | Out-File $out -Encoding utf8
function Log($t) { "$t" | Out-File $out -Append -Encoding utf8; Write-Host $t }
$fails = 0
function Check($cond, $text) { if ($cond) { Log "  OK   $text" } else { Log "  FALLO $text"; $script:fails++ } }

$bridge = Get-Content (Join-Path $env:LOCALAPPDATA "GeometryDash\geode\mods\grupo.practice-runs\ai-bridge.json") -Raw | ConvertFrom-Json
$base = "http://127.0.0.1:$($bridge.port)/api"
function Raw($tool, $headers, $body) {
    try {
        $r = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$base/$tool" -Headers $headers -Body $body -ContentType "application/json"
        return @{ status = [int]$r.StatusCode; body = ($r.Content | ConvertFrom-Json) }
    } catch {
        $code = 0; if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        $b = $null; if ($_.ErrorDetails -and $_.ErrorDetails.Message) { try { $b = $_.ErrorDetails.Message | ConvertFrom-Json } catch {} }
        return @{ status = $code; body = $b }
    }
}
$auth = @{ Authorization = "Bearer $($bridge.token)" }

try {
    $r = Raw "get_editor_state" @{} "{}"
    Check ($r.status -eq 401) "sin token -> 401 ($($r.status))"
    $r = Raw "get_editor_state" @{ Authorization = "Bearer 0000" } "{}"
    Check ($r.status -eq 401) "token incorrecto -> 401 ($($r.status))"
    $r = Raw "get_editor_state" @{ Authorization = "Bearer $($bridge.token)"; Origin = "https://ejemplo.com" } "{}"
    Check ($r.status -eq 403) "peticion con Origin (navegador) -> 403 ($($r.status))"
    $r = Raw "get_editor_state" $auth "{no es json"
    Check ($r.status -eq 400) "JSON invalido -> 400 ($($r.status))"
    $r = Raw "borrar_todo" $auth "{}"
    Check ($r.status -eq 404) "herramienta desconocida -> 404 ($($r.status))"
    $r = Raw "add_objects_batch" $auth '{"objects":[{"id":99999,"x":100000,"y":105}]}'
    Check ($r.status -eq 400 -and $r.body.error -eq "invalid_params") "id de objeto inexistente -> rechazado"
    $r = Raw "add_objects_batch" $auth '{"objects":[{"id":1,"x":5000000,"y":105}]}'
    Check ($r.status -eq 400) "coordenada fuera de rango -> rechazada"
    $many = (1..1001 | ForEach-Object { '{"id":1,"x":100000,"y":105}' }) -join ","
    $r = Raw "add_objects_batch" $auth "{""objects"":[$many]}"
    Check ($r.status -eq 400) "lote de 1001 objetos -> rechazado"
    $key = "fase7-" + [guid]::NewGuid()
    $h = @{ Authorization = "Bearer $($bridge.token)"; "Idempotency-Key" = $key }
    $a = Raw "get_editor_state" $h "{}"; $b = Raw "get_editor_state" $h "{}"
    Check ($a.status -eq 200 -and $b.body.replayed -eq $true) "misma clave de idempotencia -> respuesta repetida, no se ejecuta otra vez"
    $listen = @(Get-NetTCPConnection -LocalPort $bridge.port -State Listen -ErrorAction SilentlyContinue)
    $addrs = ($listen | ForEach-Object LocalAddress | Sort-Object -Unique) -join ","
    Check ($listen.Count -gt 0 -and $addrs -eq "127.0.0.1") "el puerto $($bridge.port) solo escucha en 127.0.0.1 ($addrs)"
    $r = Raw "save_level" $auth "{}"
    if ($r.body.error -eq "not_in_editor") { Log "  --   guardar sin confirm: no comprobado (no estabas en el editor)" }
    else { Check ($r.status -eq 409 -and $r.body.error -eq "confirmation_required") "guardar sin confirm -> 409 ($($r.status) $($r.body.error))" }

    if ($fails -eq 0) { Log "RESULTADO: TODO OK" } else { Log "RESULTADO: $fails FALLOS" }
    Log "=== FIN ==="
} catch { Log ("ERROR: " + $_.Exception.Message) }
