# Prueba de la Fase 3 (version segura).
# - Solo trabaja en la zona x 30000..34000; si ahi hay objetos, NO hace nada.
# - Guarda una foto de TODO el nivel al empezar y al final la compara objeto
#   por objeto (id, x, y, rotacion, escala, volteo).
# Resultado en test_fase3_resultado.txt

$ErrorActionPreference = "Stop"
$out = Join-Path $PSScriptRoot "test_fase3_resultado.txt"
"=== Prueba Fase 3 (segura) $(Get-Date -Format s) ===" | Out-File $out -Encoding utf8
function Log($t) { $t | Out-File $out -Append -Encoding utf8; Write-Host $t }
function J($o) { $o | ConvertTo-Json -Compress -Depth 6 }

$bridge = Get-Content (Join-Path $env:LOCALAPPDATA "GeometryDash\geode\mods\grupo.practice-runs\ai-bridge.json") -Raw | ConvertFrom-Json
$base = "http://127.0.0.1:$($bridge.port)/api"
function Call($tool, $body) {
    $h = @{ Authorization = "Bearer $($bridge.token)"; "Idempotency-Key" = "$tool-" + [guid]::NewGuid() }
    try {
        return Invoke-RestMethod -Method Post -Uri "$base/$tool" -Headers $h -Body (J $body) -ContentType "application/json"
    } catch {
        if ($_.ErrorDetails -and $_.ErrorDetails.Message) { return ($_.ErrorDetails.Message | ConvertFrom-Json) }
        throw
    }
}
function Snapshot {
    $all = Call "get_objects_in_area" @{ limit = 2000 }
    if ($all.truncated) { throw "El nivel tiene mas de 2000 objetos; usa un nivel de prueba mas pequeno" }
    return ($all.objects | ForEach-Object {
        "{0}|{1}|{2}|{3}|{4}|{5}|{6}" -f $_.id, $_.x, $_.y, $_.rotation, $_.scale, $_.flipX, $_.flipY
    } | Sort-Object)
}
$fails = 0
function Check($cond, $text) {
    if ($cond) { Log "  OK   $text" } else { Log "  FALLO $text"; $script:fails++ }
}

try {
    $s0 = Call "get_editor_state" @{}
    if (-not $s0.inEditor) { Log "Abre un nivel en el editor y repite."; exit 0 }
    Log "Nivel '$($s0.levelName)' con $($s0.objectCount) objetos"

    $zone = Call "get_objects_in_area" @{ x1 = 29000; x2 = 35000 }
    if ($zone.total -gt 0) { Log "PARADO: hay $($zone.total) objetos en la zona de pruebas (x 29000-35000). No toco nada."; exit 0 }

    $before = Snapshot
    Log "Foto inicial: $($before.Count) objetos"

    # 1) Anadir 3 objetos
    $add = Call "add_objects_batch" @{ objects = @(
        @{ id = 1; x = 30015; y = 105 }, @{ id = 8; x = 30045; y = 105 }, @{ id = 1; x = 30075; y = 135 }) }
    Check ($add.ok -and $add.created -eq 3 -and $add.misplaced -eq 0) "anadir 3 objetos"
    $uids = @($add.objects | ForEach-Object { $_.uid })

    # 2) Leer la zona
    $area = Call "get_objects_in_area" @{ x1 = 29000; x2 = 35000 }
    Check ($area.total -eq 3) "leer zona: $($area.total) objetos"

    # 3) Modificar: mover pincho y girar/escalar bloque
    $mod = Call "modify_objects_batch" @{ changes = @(@{ uid = $uids[1]; dx = 30 }, @{ uid = $uids[0]; rotation = 90; scale = 1.5 }) }
    Check ($mod.ok -and $mod.modified -eq 2) "modificar 2 objetos"
    $spike = ($mod.objects | Where-Object { $_.oldUid -eq $uids[1] })
    Check ($spike.x -eq 30075 -and $spike.y -eq 105) "pincho movido a x=30075"

    # 4) Borrar el pincho
    $del = Call "delete_objects_batch" @{ uids = @($spike.uid) }
    Check ($del.ok -and $del.deleted -eq 1) "borrar pincho"

    # 5) Deshacer borrado
    $u1 = Call "undo_last_batch" @{}
    Check ($u1.ok -and $u1.type -eq "delete" -and $u1.restored -eq 1) "deshacer borrado"

    # 6) Deshacer modificacion: deben volver los originales y no quedar copias
    $u2 = Call "undo_last_batch" @{}
    Check ($u2.ok -and $u2.type -eq "modify" -and $u2.removed -eq 2 -and $u2.restored -eq 2) "deshacer modificacion (quitados=$($u2.removed) restaurados=$($u2.restored))"
    $z = Call "get_objects_in_area" @{ x1 = 29000; x2 = 35000 }
    $zs = ($z.objects | ForEach-Object { "{0}@{1},{2} r{3} s{4}" -f $_.id, $_.x, $_.y, $_.rotation, $_.scale } | Sort-Object) -join "; "
    Check ($z.total -eq 3 -and $zs -eq "1@30015,105 r0 s1; 1@30075,135 r0 s1; 8@30045,105 r0 s1") "zona tras deshacer: $zs"

    # 7) Deshacer anadido
    $u3 = Call "undo_last_batch" @{}
    Check ($u3.ok -and $u3.type -eq "add" -and $u3.removed -eq 3) "deshacer anadido (quitados=$($u3.removed))"

    # 8) Borrado grande sin confirmar: crear 101 objetos y pedir borrarlos sin confirm
    $many = @(); for ($i = 0; $i -lt 101; $i++) { $many += @{ id = 1; x = 31005 + 30 * ($i % 50); y = 105 + 30 * [math]::Floor($i / 50) } }
    $addMany = Call "add_objects_batch" @{ objects = $many }
    Check ($addMany.ok -and $addMany.created -eq 101) "anadir 101 objetos"
    $manyUids = @($addMany.objects | ForEach-Object { $_.uid })
    $noConfirm = Call "delete_objects_batch" @{ uids = $manyUids }
    Check ($noConfirm.error -eq "confirmation_required") "borrar 101 sin confirmar -> rechazado ($($noConfirm.error))"
    $withConfirm = Call "delete_objects_batch" @{ uids = $manyUids; confirm = $true }
    Check ($withConfirm.ok -and $withConfirm.deleted -eq 101) "borrar 101 con confirm=true"
    $u4 = Call "undo_last_batch" @{}
    Check ($u4.ok -and $u4.restored -eq 101) "deshacer borrado de 101"
    $u5 = Call "undo_last_batch" @{}
    Check ($u5.ok -and $u5.type -eq "add" -and $u5.removed -eq 101) "deshacer anadido de 101 (quitados=$($u5.removed))"

    # 9) Historial vacio
    $u6 = Call "undo_last_batch" @{}
    Check ($u6.error -eq "nothing_to_undo") "historial vacio -> $($u6.error)"

    # 10) Comparar el nivel completo con la foto inicial
    $after = Snapshot
    $diff = Compare-Object $before $after
    Check ($null -eq $diff) "nivel identico al inicial ($($before.Count) -> $($after.Count) objetos)"
    if ($diff) { Log ("  diferencias: " + (J $diff)) }

    if ($fails -eq 0) { Log "RESULTADO: TODO OK" } else { Log "RESULTADO: $fails FALLOS" }
    Log "=== FIN ==="
} catch {
    Log ("ERROR: " + $_.Exception.Message)
}
