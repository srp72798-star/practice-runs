# Prueba de la Fase 5: grupos, propiedades de triggers, resumen del nivel y guardado.
# - Solo trabaja en la zona x 40000..46000; si ahi hay objetos, NO hace nada.
# - Al final lo deshace todo y comprueba que el nivel queda igual que al empezar.
# - NO guarda el nivel (solo comprueba que guardar pide confirmacion).
# Resultado en test_fase5_resultado.txt

$ErrorActionPreference = "Stop"
$out = Join-Path $PSScriptRoot "test_fase5_resultado.txt"
"=== Prueba Fase 5 $(Get-Date -Format s) ===" | Out-File $out -Encoding utf8
function Log($t) { "$t" | Out-File $out -Append -Encoding utf8; Write-Host $t }
function J($o) { $o | ConvertTo-Json -Compress -Depth 8 }

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
    return @($all.objects | ForEach-Object { "{0}|{1}|{2}|{3}|{4}" -f $_.id, $_.x, $_.y, $_.rotation, $_.scale } | Sort-Object)
}
$fails = 0
function Check($cond, $text) { if ($cond) { Log "  OK   $text" } else { Log "  FALLO $text"; $script:fails++ } }

try {
    $ping = Call "ping" @{}
    Log "Puente $($ping.bridge), mod $($ping.mod)"
    $s0 = Call "get_editor_state" @{}
    if (-not $s0.inEditor) { Log "Abre un nivel en el editor y repite."; exit 0 }
    $zone = Call "get_objects_in_area" @{ x1 = 39000; x2 = 47000 }
    if ($zone.total -gt 0) { Log "PARADO: hay $($zone.total) objetos en la zona de pruebas (x 39000-47000). No toco nada."; exit 0 }
    $before = @(Snapshot)
    $sum0 = Call "get_level_summary" @{}
    Check ($sum0.ok) "resumen del nivel: $($sum0.objectCount) objetos, siguiente grupo libre $($sum0.nextFreeGroup)"
    $G = [int]$sum0.nextFreeGroup

    # 1) Bloque con grupo + portal nave + velocidad 2x
    $a1 = Call "add_objects_batch" @{ objects = @(
        @{ id = 1; x = 40005; y = 105; groups = @($G) },
        @{ id = 13; x = 40065; y = 135 },
        @{ id = 202; x = 40125; y = 105 }) }
    Check ($a1.ok -and $a1.created -eq 3 -and $a1.propsWrong -eq 0) "anadir bloque con grupo $G, portal nave y velocidad 2x"
    $blockUid = $a1.objects[0].uid

    # 2) Triggers con propiedades
    $a2 = Call "add_objects_batch" @{ objects = @(
        @{ id = 899; x = 40185; y = 225; props = @{ "23" = 1000; "7" = 10; "8" = 20; "9" = 30; "10" = 0.5 } },
        @{ id = 901; x = 40215; y = 225; props = @{ "51" = $G; "28" = 0; "29" = 60; "10" = 1; "30" = 1; "85" = 2 } },
        @{ id = 1007; x = 40245; y = 225; props = @{ "51" = $G; "35" = 0.5; "10" = 0.5 } }) }
    Check ($a2.ok -and $a2.created -eq 3 -and $a2.propsWrong -eq 0) "anadir triggers color/move/alpha (propsWrong=$($a2.propsWrong))"
    foreach ($o in $a2.objects) {
        Log ("     id {0}: propsOk={1} noGuardadas={2} mal={3}" -f $o.id, $o.propsOk, ($o.propsNotSaved -join ' '), ($o.propsWrong -join ' | '))
    }
    $colorUid = $a2.objects[0].uid

    # 3) Releer con detalles
    $d = Call "get_objects_in_area" @{ x1 = 39000; x2 = 47000; details = $true }
    $blk = $d.objects | Where-Object { $_.uid -eq $blockUid }
    Check (@($blk.groups) -contains $G) "el bloque tiene el grupo $G segun el juego"
    $col = $d.objects | Where-Object { $_.uid -eq $colorUid }
    Check ($col.props.'7' -eq 10 -and $col.props.'8' -eq 20 -and $col.props.'9' -eq 30 -and $col.props.'23' -eq 1000) "trigger color: RGB 10,20,30 canal 1000 segun el juego"
    $mv = $d.objects | Where-Object { $_.id -eq 901 }
    Log "     move segun el juego: $(J $mv.props)"

    # 4) Resumen ve portal y triggers
    $sum1 = Call "get_level_summary" @{}
    $names = @($sum1.portalsAndTriggers | Where-Object { $_.x -ge 39000 } | ForEach-Object { $_.name })
    Check ($names -contains "portal nave" -and $names -contains "velocidad 2x" -and $names -contains "trigger move") "resumen: $($names -join ', ')"
    Check (@($sum1.usedGroups) -contains $G) "resumen: grupo $G en uso"

    # 5) Modificar grupos y propiedad
    $m = Call "modify_objects_batch" @{ changes = @(
        @{ uid = $blockUid; groups = @($G, ($G + 1)) },
        @{ uid = $colorUid; props = @{ "7" = 200 } }) }
    Check ($m.ok -and $m.propsWrong -eq 0) "modificar grupos y color (propsWrong=$($m.propsWrong))"

    # 6) Validaciones
    $bad = Call "add_objects_batch" @{ objects = @(@{ id = 1; x = 40500; y = 105; props = @{ "999" = 1 } }) }
    Check ($bad.error -eq "invalid_params") "propiedad no permitida -> rechazada ($($bad.error))"
    $bad2 = Call "add_objects_batch" @{ objects = @(@{ id = 1; x = 40500; y = 105; groups = @(0) }) }
    Check ($bad2.error -eq "invalid_params") "grupo 0 -> rechazado ($($bad2.error))"
    $sv = Call "save_level" @{}
    Check ($sv.error -eq "confirmation_required") "guardar sin confirmar -> rechazado ($($sv.error))"

    # 7) Deshacer todo
    $u1 = Call "undo_last_batch" @{}; Check ($u1.ok -and $u1.type -eq "modify") "deshacer modificacion"
    $u2 = Call "undo_last_batch" @{}; Check ($u2.ok -and $u2.removed -eq 3) "deshacer triggers"
    $u3 = Call "undo_last_batch" @{}; Check ($u3.ok -and $u3.removed -eq 3) "deshacer bloque/portal/velocidad"
    $after = @(Snapshot)
    $same = (($before -join "`n") -eq ($after -join "`n"))
    Check $same "nivel identico al inicial ($($before.Count) -> $($after.Count) objetos)"

    if ($fails -eq 0) { Log "RESULTADO: TODO OK" } else { Log "RESULTADO: $fails FALLOS" }
    Log "=== FIN ==="
} catch {
    Log ("ERROR: " + $_.Exception.Message)
}
