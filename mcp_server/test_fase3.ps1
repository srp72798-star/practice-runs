# Prueba de la Fase 3: catalogo, leer area, modificar, borrar y deshacer.
# Al final el nivel debe quedar EXACTAMENTE con los objetos que tenia al empezar.
# Resultado en test_fase3_resultado.txt

$ErrorActionPreference = "Stop"
$out = Join-Path $PSScriptRoot "test_fase3_resultado.txt"
"=== Prueba Fase 3 $(Get-Date -Format s) ===" | Out-File $out -Encoding utf8
function Log($t) { $t | Out-File $out -Append -Encoding utf8; Write-Host $t }
function J($o) { $o | ConvertTo-Json -Compress -Depth 6 }

try {
    $bridge = Get-Content (Join-Path $env:LOCALAPPDATA "GeometryDash\geode\mods\grupo.practice-runs\ai-bridge.json") -Raw | ConvertFrom-Json
    $base = "http://127.0.0.1:$($bridge.port)/api"
    function Call($tool, $body) {
        $h = @{ Authorization = "Bearer $($bridge.token)"; "Idempotency-Key" = "$tool-" + [guid]::NewGuid() }
        try {
            return Invoke-RestMethod -Method Post -Uri "$base/$tool" -Headers $h -Body (J $body) -ContentType "application/json"
        } catch {
            $r = $_.Exception.Response
            if ($r) { $sr = New-Object IO.StreamReader($r.GetResponseStream()); return ($sr.ReadToEnd() | ConvertFrom-Json) }
            throw
        }
    }

    $s0 = Call "get_editor_state" @{}
    Log ("1 estado inicial: " + (J $s0))
    if (-not $s0.inEditor) { Log "Abre un nivel en el editor y repite."; exit 0 }
    $start = $s0.objectCount

    $cat = Call "get_object_catalog" @{ query = "spike"; limit = 5 }
    Log ("2 catalogo 'spike': total=$($cat.total) primeros=" + (J $cat.items))

    # Zona de pruebas lejos del principio para no tocar nada tuyo: x 3015..3075
    $add = Call "add_objects_batch" @{ objects = @(
        @{ id = 1; x = 3015; y = 105 }, @{ id = 8; x = 3045; y = 105 }, @{ id = 1; x = 3075; y = 135 }) }
    Log ("3 anadir: ok=$($add.ok) creados=$($add.created) misplaced=$($add.misplaced)")
    $uids = $add.objects | ForEach-Object { $_.uid }

    $area = Call "get_objects_in_area" @{ x1 = 3000; x2 = 3090; y1 = 90; y2 = 150 }
    Log ("4 leer area: total=$($area.total) " + (J ($area.objects | Select-Object uid,id,x,y)))

    $mod = Call "modify_objects_batch" @{ changes = @(
        @{ uid = $uids[1]; dx = 30 },
        @{ uid = $uids[0]; rotation = 90; scale = 1.5 }) }
    Log ("5 modificar: ok=$($mod.ok) modificados=$($mod.modified) misplaced=$($mod.misplaced) " + (J ($mod.objects | Select-Object oldUid,uid,x,y,rotation,scale)))
    $spikeNew = ($mod.objects | Where-Object { $_.oldUid -eq $uids[1] }).uid

    $del = Call "delete_objects_batch" @{ uids = @($spikeNew) }
    Log ("6 borrar pincho: ok=$($del.ok) borrados=$($del.deleted) total=$($del.objectCountAfter)")

    $u1 = Call "undo_last_batch" @{}
    Log ("7 deshacer borrado: ok=$($u1.ok) tipo=$($u1.type) restaurados=$($u1.restored) total=$($u1.objectCountAfter)")

    $u2 = Call "undo_last_batch" @{}
    Log ("8 deshacer modificacion: ok=$($u2.ok) tipo=$($u2.type) quitados=$($u2.removed) restaurados=$($u2.restored)")
    $check = Call "get_objects_in_area" @{ x1 = 3000; x2 = 3090; y1 = 90; y2 = 150 }
    Log ("   tras deshacer: " + (J ($check.objects | Select-Object id,x,y,rotation,scale)))

    $u3 = Call "undo_last_batch" @{}
    Log ("9 deshacer anadido: ok=$($u3.ok) tipo=$($u3.type) quitados=$($u3.removed) total=$($u3.objectCountAfter)")

    $u4 = Call "undo_last_batch" @{}
    Log ("10 deshacer sin nada (debe fallar): error=$($u4.error)")

    $big = Call "delete_objects_batch" @{ area = @{ x1 = -1e9; x2 = 1e9; y1 = -1e9; y2 = 1e9 } }
    Log ("11 borrado masivo sin confirmar: error=$($big.error) deleted=$($big.deleted)")

    $s1 = Call "get_editor_state" @{}
    if ($s1.objectCount -eq $start) { Log "RESULTADO: OK, el nivel ha vuelto a $start objetos" }
    else { Log "RESULTADO: FALLO, empezo con $start y acaba con $($s1.objectCount)" }
    Log "=== FIN ==="
} catch {
    Log ("ERROR: " + $_.Exception.Message)
}
