# Prueba de la Fase 2 del AI Level Builder.
# Lee el token del mod, consulta el estado del editor e inserta 3 objetos.
# Guarda todo en test_resultado.txt junto a este script.

$ErrorActionPreference = "Stop"
$out = Join-Path $PSScriptRoot "test_resultado.txt"
"=== Prueba AI Level Builder $(Get-Date -Format s) ===" | Out-File $out -Encoding utf8

function Log($text) { $text | Out-File $out -Append -Encoding utf8; Write-Host $text }

try {
    $bridgeFile = Join-Path $env:LOCALAPPDATA "GeometryDash\geode\mods\grupo.practice-runs\ai-bridge.json"
    if (-not (Test-Path $bridgeFile)) {
        Log "ERROR: no existe $bridgeFile. Activa 'AI Level Builder' en los ajustes del mod y reinicia el juego."
        exit 1
    }
    $bridge = Get-Content $bridgeFile -Raw | ConvertFrom-Json
    Log "Puerto: $($bridge.port)  (token leido, $($bridge.token.Length) caracteres)"

    $base = "http://127.0.0.1:$($bridge.port)/api"
    $headers = @{ Authorization = "Bearer $($bridge.token)" }

    # 1) Sin token tiene que rechazarse
    try {
        Invoke-RestMethod -Method Post -Uri "$base/ping" -Body "{}" -ContentType "application/json" | Out-Null
        Log "FALLO: el servidor acepto una peticion sin token"
    } catch {
        Log "OK: peticion sin token rechazada ($($_.Exception.Response.StatusCode.value__))"
    }

    # 2) Ping
    $ping = Invoke-RestMethod -Method Post -Uri "$base/ping" -Headers $headers -Body "{}" -ContentType "application/json"
    Log ("ping: " + ($ping | ConvertTo-Json -Compress))

    # 3) Estado del editor
    $state = Invoke-RestMethod -Method Post -Uri "$base/get_editor_state" -Headers $headers -Body "{}" -ContentType "application/json"
    Log ("estado: " + ($state | ConvertTo-Json -Compress))
    if (-not $state.inEditor) {
        Log "AVISO: no hay ningun nivel abierto en el editor. Abre uno y repite la prueba."
        exit 0
    }

    # 4) Insertar 3 objetos: bloque, pincho y bloque (x=315..375, en el suelo)
    $body = @{ objects = @(
        @{ id = 1; x = 315; y = 105 },
        @{ id = 8; x = 345; y = 105 },
        @{ id = 1; x = 375; y = 135 }
    ) } | ConvertTo-Json -Depth 5
    $key = "test-" + [guid]::NewGuid().ToString()
    $h2 = $headers.Clone(); $h2["Idempotency-Key"] = $key
    $add = Invoke-RestMethod -Method Post -Uri "$base/add_objects_batch" -Headers $h2 -Body $body -ContentType "application/json"
    Log ("lote: " + ($add | ConvertTo-Json -Compress -Depth 5))

    # 5) Repetir con la misma clave: no debe crear objetos nuevos
    $again = Invoke-RestMethod -Method Post -Uri "$base/add_objects_batch" -Headers $h2 -Body $body -ContentType "application/json"
    Log ("repeticion (debe tener replayed=true): replayed=$($again.replayed)")

    $state2 = Invoke-RestMethod -Method Post -Uri "$base/get_editor_state" -Headers $headers -Body "{}" -ContentType "application/json"
    Log ("estado final: " + ($state2 | ConvertTo-Json -Compress))
    Log "=== FIN ==="
} catch {
    Log ("ERROR: " + $_.Exception.Message)
}
