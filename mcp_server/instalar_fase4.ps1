# Fase 4: instala el servidor MCP del AI Level Builder y lo conecta a Claude Desktop.
# 1) Busca Python >= 3.10 (si no hay, ofrece instalarlo con winget)
# 2) Copia el servidor a %LOCALAPPDATA%\GDAIBuilder y crea un entorno virtual
# 3) Instala dependencias (mcp<2) y comprueba que arranca
# 4) Anade "gd-builder" a claude_desktop_config.json (con copia de seguridad)
# Resultado en instalar_resultado.txt

$ErrorActionPreference = "Stop"
$src = $PSScriptRoot
$out = Join-Path $src "instalar_resultado.txt"
"=== Instalacion Fase 4 $(Get-Date -Format s) ===" | Out-File $out -Encoding utf8
function Log($t) { "$t" | Out-File $out -Append -Encoding utf8; Write-Host $t }

function Find-Python {
    $candidates = @(
        @{ exe = "py"; args = @("-3") },
        @{ exe = "python"; args = @() },
        @{ exe = "python3"; args = @() }
    )
    foreach ($c in $candidates) {
        $cmd = Get-Command $c.exe -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        if ($cmd.Source -like "*\WindowsApps\*") { continue }  # acceso directo de la Store, no es Python real
        try {
            $info = & $c.exe @($c.args) -c "import sys;print(sys.executable);print(sys.version_info[0]*100+sys.version_info[1])" 2>$null
            if ($LASTEXITCODE -eq 0 -and $info.Count -ge 2 -and [int]$info[1] -ge 310) { return $info[0] }
        } catch { }
    }
    foreach ($dir in (Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) {
        $exe = Join-Path $dir.FullName "python.exe"
        if (Test-Path $exe) {
            $v = & $exe -c "import sys;print(sys.version_info[0]*100+sys.version_info[1])"
            if ([int]$v -ge 310) { return $exe }
        }
    }
    return $null
}

function Write-JsonNoBom($path, $obj) {
    $json = $obj | ConvertTo-Json -Depth 30
    [IO.File]::WriteAllText($path, $json, (New-Object Text.UTF8Encoding($false)))
}

try {
    # ---------- 1) Python
    $py = Find-Python
    if (-not $py) {
        Log "No he encontrado Python 3.10 o superior."
        $ans = Read-Host "Quieres instalar Python 3.12 oficial con winget ahora? (s/n)"
        if ($ans -notmatch '^[sSyY]') { Log "Instala Python desde https://www.python.org/downloads/ y vuelve a ejecutar."; exit 0 }
        winget install -e --id Python.Python.3.12 --scope user --accept-package-agreements --accept-source-agreements
        $py = Find-Python
        if (-not $py) { Log "Python instalado pero no lo encuentro aun. Cierra esta ventana y ejecuta INSTALAR_FASE4.bat otra vez."; exit 0 }
    }
    Log "Python: $py"

    # ---------- 2) Copiar servidor y crear venv
    $dest = Join-Path $env:LOCALAPPDATA "GDAIBuilder"
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item (Join-Path $src "gd_builder_mcp.py") $dest -Force
    Copy-Item (Join-Path $src "requirements.txt") $dest -Force
    $venvPy = Join-Path $dest "venv\Scripts\python.exe"
    if (-not (Test-Path $venvPy)) {
        Log "Creando entorno virtual..."
        & $py -m venv (Join-Path $dest "venv")
        if ($LASTEXITCODE -ne 0) { throw "No se pudo crear el entorno virtual" }
    }

    # ---------- 3) Dependencias + comprobacion
    Log "Instalando dependencias (puede tardar un minuto)..."
    & $venvPy -m pip install --disable-pip-version-check -q -r (Join-Path $dest "requirements.txt")
    if ($LASTEXITCODE -ne 0) { throw "Fallo pip install" }
    $check = & $venvPy (Join-Path $dest "gd_builder_mcp.py") --check
    $check | ForEach-Object { Log "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "El servidor MCP no arranca" }

    # ---------- 4) Config de Claude Desktop
    # La ruta buena es la que la propia app escribe en su log al arrancar
    $configs = @()
    $mainLog = Join-Path $env:LOCALAPPDATA "Claude\logs\main.log"
    if (Test-Path $mainLog) {
        $m = Select-String -Path $mainLog -Pattern 'Reading claude_desktop_config\.json from (.+claude_desktop_config\.json)' | Select-Object -Last 1
        if ($m) { $configs += $m.Matches[0].Groups[1].Value.Trim(); Log "La app lee la configuracion de: $($configs[0])" }
    }
    if ($configs.Count -eq 0) {
        $configs += Join-Path $env:APPDATA "Claude\claude_desktop_config.json"
        foreach ($pkg in (Get-ChildItem "$env:LOCALAPPDATA\Packages\*Claude_*" -Directory -ErrorAction SilentlyContinue)) {
            $configs += Join-Path $pkg.FullName "LocalCache\Roaming\Claude\claude_desktop_config.json"
        }
    }
    $entry = [pscustomobject]@{ command = $venvPy; args = @((Join-Path $dest "gd_builder_mcp.py")) }
    $done = 0
    foreach ($cfg in $configs) {
        $dir = Split-Path $cfg
        if (-not (Test-Path $dir)) { continue }
        if (Test-Path $cfg) {
            $raw = Get-Content $cfg -Raw
            Copy-Item $cfg "$cfg.backup-$(Get-Date -Format yyyyMMdd-HHmmss)"
            $obj = if ($raw.Trim()) { $raw | ConvertFrom-Json } else { [pscustomobject]@{} }
        } else {
            $obj = [pscustomobject]@{}
        }
        if (-not $obj.PSObject.Properties["mcpServers"]) { $obj | Add-Member -NotePropertyName mcpServers -NotePropertyValue ([pscustomobject]@{}) }
        $obj.mcpServers | Add-Member -NotePropertyName "gd-builder" -NotePropertyValue $entry -Force
        Write-JsonNoBom $cfg $obj
        Log "Configurado: $cfg"
        $done++
    }
    if ($done -eq 0) {
        Log "No encuentro la carpeta de Claude Desktop. Abre Claude Desktop una vez, cierralo y repite."
        exit 0
    }
    Log ""
    Log "LISTO. Ahora cierra Claude Desktop del todo (icono de la bandeja > Quit) y vuelve a abrirlo."
    Log "En un chat nuevo deberian aparecer las herramientas 'gd-builder'."
    Log "=== FIN ==="
} catch {
    Log ("ERROR: " + $_.Exception.Message)
}
