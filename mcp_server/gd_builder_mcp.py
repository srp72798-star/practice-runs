"""
Servidor MCP del AI Level Builder para Geometry Dash.

Claude Desktop lanza este script (stdio). El script habla con el mod
"Practice Runs" por HTTP en 127.0.0.1, usando el puerto y el token que el mod
escribe en ai-bridge.json al arrancar el juego.

Solo usa la libreria estandar + el SDK oficial "mcp".
"""

from __future__ import annotations

import json
import logging
import os
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path
from typing import Annotated

from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel, Field

# ---------------------------------------------------------------- config

MOD_ID = "grupo.practice-runs"
DEFAULT_BRIDGE_FILE = (
    Path(os.environ.get("LOCALAPPDATA", ""))
    / "GeometryDash" / "geode" / "mods" / MOD_ID / "ai-bridge.json"
)
BRIDGE_FILE = Path(os.environ.get("GD_BRIDGE_FILE", DEFAULT_BRIDGE_FILE))
LOG_FILE = Path(__file__).with_name("actions.log")
TIMEOUT_SECONDS = 15
MAX_BATCH = 1000

logging.basicConfig(
    filename=LOG_FILE,
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("gd-builder")

mcp = FastMCP("geometry-dash-builder")


# ---------------------------------------------------------------- transporte

class BridgeError(Exception):
    pass


def _read_bridge() -> tuple[int, str]:
    if not BRIDGE_FILE.exists():
        raise BridgeError(
            f"No encuentro {BRIDGE_FILE}. Abre Geometry Dash con la opcion "
            "'AI Level Builder' activada en los ajustes del mod (y reinicia el juego)."
        )
    data = json.loads(BRIDGE_FILE.read_text(encoding="utf-8"))
    return int(data["port"]), str(data["token"])


def _call(tool: str, payload: dict | None = None, idempotency_key: str | None = None) -> dict:
    """POST /api/<tool> al mod. Reintenta una vez si se corta la conexion,
    con la misma clave de idempotencia para no duplicar operaciones."""
    port, token = _read_bridge()
    body = json.dumps(payload or {}).encode("utf-8")
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
    }
    if idempotency_key:
        headers["Idempotency-Key"] = idempotency_key

    last_error: Exception | None = None
    for attempt in range(2):
        req = urllib.request.Request(
            f"http://127.0.0.1:{port}/api/{tool}", data=body, headers=headers, method="POST"
        )
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT_SECONDS) as resp:
                result = json.loads(resp.read().decode("utf-8"))
                log.info("tool=%s key=%s ok=%s", tool, idempotency_key, result.get("ok"))
                return result
        except urllib.error.HTTPError as e:
            # El mod contesto con un error: no se reintenta
            try:
                result = json.loads(e.read().decode("utf-8"))
            except Exception:
                result = {"ok": False, "error": f"http_{e.code}", "message": str(e)}
            log.warning("tool=%s key=%s http=%s result=%s", tool, idempotency_key, e.code, result)
            return result
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            last_error = e
            log.warning("tool=%s intento=%d sin conexion: %s", tool, attempt + 1, e)
            # Sin clave de idempotencia no es seguro reintentar una escritura
            if not idempotency_key and tool != "get_editor_state" and tool != "ping":
                break
            time.sleep(0.5)
    raise BridgeError(
        f"No se pudo conectar con el mod ({last_error}). "
        "Comprueba que Geometry Dash esta abierto y la opcion 'AI Level Builder' activada."
    )


def _safe(tool: str, payload: dict | None = None, key: str | None = None) -> dict:
    try:
        return _call(tool, payload, key)
    except BridgeError as e:
        return {"ok": False, "error": "bridge_unavailable", "message": str(e)}


# ---------------------------------------------------------------- esquemas

class ObjectSpec(BaseModel):
    id: Annotated[int, Field(ge=1, le=10000, description="ID de objeto de Geometry Dash (1 = bloque basico, 8 = pincho)")]
    x: Annotated[float, Field(ge=-1000, le=1_000_000, description="X en unidades del editor (30 = un bloque)")]
    y: Annotated[float, Field(ge=-3000, le=30_000, description="Y en unidades del editor (105 = primera fila sobre el suelo)")]
    rotation: Annotated[float, Field(ge=-360, le=360)] = 0
    scale: Annotated[float, Field(ge=0.1, le=10)] = 1
    flipX: bool = False
    flipY: bool = False


# ---------------------------------------------------------------- herramientas

@mcp.tool()
def gd_get_editor_state() -> dict:
    """Estado del editor de Geometry Dash: si hay un nivel abierto en el
    editor, su nombre y cuantos objetos tiene. Llamalo antes de editar."""
    return _safe("get_editor_state")


@mcp.tool()
def gd_add_objects_batch(
    objects: Annotated[list[ObjectSpec], Field(min_length=1, max_length=MAX_BATCH)],
    batch_id: str | None = None,
) -> dict:
    """Crea varios objetos a la vez en el nivel abierto en el editor.

    Coordenadas: 30 unidades = 1 bloque. El suelo esta en y=90, asi que un
    bloque apoyado en el suelo va en y=105 (centro). x=15 es la primera columna.
    Todo el lote se deshace con un solo Ctrl+Z en el editor.

    La respuesta incluye los objetos leidos desde el juego despues de crearlos
    (verifiedInLevel) para confirmar que existen de verdad.
    Usa el mismo batch_id si repites la llamada: no se duplicara.
    """
    key = batch_id or f"batch-{uuid.uuid4()}"
    payload = {"objects": [o.model_dump() for o in objects]}
    result = _safe("add_objects_batch", payload, key)
    result.setdefault("batchId", key)
    return result


if __name__ == "__main__":
    mcp.run()
