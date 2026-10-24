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


class ObjectChange(BaseModel):
    uid: Annotated[int, Field(description="uid del objeto (lo devuelven gd_add_objects_batch y gd_get_objects_in_area)")]
    x: Annotated[float, Field(ge=-1000, le=1_000_000)] | None = None
    y: Annotated[float, Field(ge=-3000, le=30_000)] | None = None
    dx: Annotated[float, Field(ge=-100_000, le=100_000, description="desplazamiento relativo en X")] | None = None
    dy: Annotated[float, Field(ge=-30_000, le=30_000, description="desplazamiento relativo en Y")] | None = None
    rotation: Annotated[float, Field(ge=-360, le=360)] | None = None
    scale: Annotated[float, Field(ge=0.1, le=10)] | None = None
    flipX: bool | None = None
    flipY: bool | None = None


class Area(BaseModel):
    x1: float
    x2: float
    y1: float
    y2: float


@mcp.tool()
def gd_get_object_catalog(
    query: Annotated[str, Field(max_length=60, description="Texto a buscar en el nombre de imagen del objeto, p. ej. 'spike', 'portal', 'square'")] = "",
    limit: Annotated[int, Field(ge=1, le=500)] = 100,
    offset: Annotated[int, Field(ge=0)] = 0,
) -> dict:
    """Lista de objetos que existen en el juego (id + nombre de su imagen),
    leida del propio Geometry Dash. Usala para encontrar IDs validos en vez
    de inventarlos."""
    return _safe("get_object_catalog", {"query": query, "limit": limit, "offset": offset})


@mcp.tool()
def gd_get_objects_in_area(
    x1: float = -1e9, x2: float = 1e9, y1: float = -1e9, y2: float = 1e9,
    id: Annotated[int, Field(ge=1, le=10000)] | None = None,
    limit: Annotated[int, Field(ge=1, le=2000)] = 500,
) -> dict:
    """Objetos del nivel abierto dentro de un rectangulo (coordenadas del
    editor). Devuelve uid, id, x, y, rotation, scale y flips. Sirve para
    revisar lo creado y conseguir los uid que piden modificar/borrar."""
    payload = {"x1": x1, "x2": x2, "y1": y1, "y2": y2, "limit": limit}
    if id is not None:
        payload["id"] = id
    return _safe("get_objects_in_area", payload)


@mcp.tool()
def gd_modify_objects_batch(
    changes: Annotated[list[ObjectChange], Field(min_length=1, max_length=MAX_BATCH)],
    batch_id: str | None = None,
) -> dict:
    """Modifica varios objetos a la vez (posicion absoluta x/y o relativa
    dx/dy, rotacion, escala, volteo). Se conservan el resto de propiedades.
    Importante: cada objeto modificado recibe un uid NUEVO (ver oldUid -> uid
    en la respuesta). Se puede revertir con gd_undo_last_batch."""
    key = batch_id or f"modify-{uuid.uuid4()}"
    payload = {"changes": [c.model_dump(exclude_none=True) for c in changes]}
    result = _safe("modify_objects_batch", payload, key)
    result.setdefault("batchId", key)
    return result


@mcp.tool()
def gd_delete_objects_batch(
    uids: Annotated[list[int], Field(min_length=1, max_length=2000)] | None = None,
    area: Area | None = None,
    confirm: bool = False,
    batch_id: str | None = None,
) -> dict:
    """Borra objetos por uid o por area. Borrar mas de 200 objetos exige
    confirm=true (pidele permiso al usuario antes). Se puede revertir con
    gd_undo_last_batch."""
    if (uids is None) == (area is None):
        return {"ok": False, "error": "invalid_params", "message": "Indica 'uids' o 'area' (solo uno)"}
    key = batch_id or f"delete-{uuid.uuid4()}"
    payload: dict = {"confirm": confirm}
    if uids is not None:
        payload["uids"] = uids
    else:
        payload["area"] = area.model_dump()
    result = _safe("delete_objects_batch", payload, key)
    result.setdefault("batchId", key)
    return result


@mcp.tool()
def gd_undo_last_batch() -> dict:
    """Deshace el ultimo lote hecho por la IA en este nivel (anadir,
    modificar o borrar). Se pueden deshacer hasta 50 lotes seguidos."""
    # Clave unica: si la conexion falla y se reintenta, no se deshacen dos lotes
    return _safe("undo_last_batch", {}, f"undo-{uuid.uuid4()}")


def _check() -> int:
    """Comprobacion para el instalador: dependencias + conexion con el mod."""
    import sys
    print(f"Python {sys.version.split()[0]} OK, SDK mcp OK")
    print(f"Fichero del puente: {BRIDGE_FILE}")
    try:
        state = _call("get_editor_state")
        print("Conexion con el mod OK:", json.dumps(state, ensure_ascii=False))
    except BridgeError as e:
        print("Aviso (no es grave si el juego esta cerrado):", e)
    return 0


if __name__ == "__main__":
    import sys
    if "--check" in sys.argv:
        raise SystemExit(_check())
    mcp.run()
