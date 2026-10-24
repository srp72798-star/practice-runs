# Anade gd-builder a la configuracion REAL de Claude Desktop (carpeta Claude-Data).
# Usa Python para editar el JSON sin tocar nada mas, y hace copia de seguridad.
$out = Join-Path $PSScriptRoot "conectar_resultado.txt"
"=== Conectar gd-builder $(Get-Date -Format s) ===" | Out-File $out -Encoding utf8
function Log($t) { "$t" | Out-File $out -Append -Encoding utf8; Write-Host $t }

$dest = Join-Path $env:LOCALAPPDATA "GDAIBuilder"
$venvPy = Join-Path $dest "venv\Scripts\python.exe"
$server = Join-Path $dest "gd_builder_mcp.py"
if (-not (Test-Path $venvPy)) { Log "Falta el entorno de Python: ejecuta antes INSTALAR_FASE4.bat"; exit 0 }

# Carpeta de datos que usa la app (la anota en main.log)
$dirs = @()
$mainLog = Join-Path $env:LOCALAPPDATA "Claude\logs\main.log"
if (Test-Path $mainLog) {
    $m = Select-String -Path $mainLog -Pattern 'token cache location: (.+)\\config\.json' | Select-Object -Last 1
    if ($m) { $dirs += $m.Matches[0].Groups[1].Value.Trim() }
}
$dirs += @(Get-ChildItem "$env:LOCALAPPDATA\Claude-Data\*" -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName)
$dirs = $dirs | Select-Object -Unique

$py = @'
import json, shutil, sys, time
path, cmd, script = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    with open(path, encoding="utf-8-sig") as f:
        data = json.load(f)
    shutil.copy2(path, path + ".backup-" + time.strftime("%Y%m%d-%H%M%S"))
except FileNotFoundError:
    data = {}
servers = data.setdefault("mcpServers", {})
servers["gd-builder"] = {"command": cmd, "args": [script]}
with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2, ensure_ascii=False)
print("servidores ahora:", ", ".join(servers))
'@
$tmp = Join-Path $env:TEMP "gd_merge_config.py"
[IO.File]::WriteAllText($tmp, $py, (New-Object Text.UTF8Encoding($false)))

$done = 0
foreach ($d in $dirs) {
    if (-not (Test-Path $d)) { continue }
    $cfg = Join-Path $d "claude_desktop_config.json"
    Log "Carpeta de Claude: $d"
    $r = & $venvPy $tmp $cfg $venvPy $server
    Log "  $r"
    if ($LASTEXITCODE -eq 0) { $done++ } else { Log "  ERROR al editar $cfg" }
}
if ($done -eq 0) { Log "No he encontrado la carpeta Claude-Data." }
else {
    Log ""
    Log "LISTO. Cierra Claude Desktop con Quit (icono de la bandeja) y vuelve a abrirlo."
}
Log "=== FIN ==="
