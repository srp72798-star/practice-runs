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
from typing import Annotated, Literal

from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel, Field, field_validator

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

# Propiedades permitidas (clave del formato de nivel de GD). El mod vuelve a
# validarlas y comprueba despues que el juego las ha guardado.
ALLOWED_PROPS = {
    "7": "R del trigger de color (0-255)", "8": "G (0-255)", "9": "B (0-255)",
    "10": "duracion en segundos", "11": "touch triggered", "17": "blending",
    "21": "canal de color principal", "22": "canal de color de detalle",
    "23": "canal de color objetivo", "28": "move X", "29": "move Y",
    "30": "easing (0-18)", "35": "opacidad (0-1)", "51": "grupo objetivo",
    "62": "spawn triggered", "85": "easing rate", "87": "multi trigger",
}

GroupList = Annotated[list[Annotated[int, Field(ge=1, le=9999)]], Field(max_length=10)]
Props = dict[str, float | bool]


def _check_props(v: Props | None) -> Props | None:
    if v:
        bad = [k for k in v if k not in ALLOWED_PROPS]
        if bad:
            raise ValueError(f"propiedades no permitidas: {bad}. Permitidas: {ALLOWED_PROPS}")
    return v


class ObjectSpec(BaseModel):
    id: Annotated[int, Field(ge=1, le=10000, description="ID de objeto de Geometry Dash (1 = bloque basico, 8 = pincho)")]
    x: Annotated[float, Field(ge=-1000, le=1_000_000, description="X en unidades del editor (30 = un bloque)")]
    y: Annotated[float, Field(ge=-3000, le=30_000, description="Y en unidades del editor (105 = primera fila sobre el suelo)")]
    rotation: Annotated[float, Field(ge=-360, le=360)] = 0
    scale: Annotated[float, Field(ge=0.1, le=10)] = 1
    flipX: bool = False
    flipY: bool = False
    groups: Annotated[GroupList | None, Field(description="grupos del objeto (para moverlo/ocultarlo con triggers)")] = None
    props: Annotated[Props | None, Field(description="propiedades avanzadas por clave: " + ", ".join(f"{k}={d}" for k, d in ALLOWED_PROPS.items()))] = None

    _v = field_validator("props")(classmethod(lambda cls, v: _check_props(v)))


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
    payload = {"objects": [o.model_dump(exclude_none=True) for o in objects]}
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
    groups: Annotated[GroupList | None, Field(description="sustituye los grupos ([] = quitar todos)")] = None
    props: Annotated[Props | None, Field(description="propiedades avanzadas a cambiar (mismas claves que en gd_add_objects_batch)")] = None

    _v = field_validator("props")(classmethod(lambda cls, v: _check_props(v)))


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
    details: bool = False,
) -> dict:
    """Objetos del nivel abierto dentro de un rectangulo (coordenadas del
    editor). Devuelve uid, id, x, y, rotation, scale y flips. Con
    details=true tambien grupos y propiedades de trigger tal como las guarda
    el juego (maximo 500). Sirve para revisar lo creado y conseguir los uid."""
    payload = {"x1": x1, "x2": x2, "y1": y1, "y2": y2, "limit": limit, "details": details}
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
    """Borra objetos por uid o por area. Borrar mas de 100 objetos (o mas de
    la cuarta parte del nivel) exige confirm=true: pidele permiso al usuario
    antes. Se puede revertir con gd_undo_last_batch."""
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


# ---------------------------------------------------------------- fase 5

# IDs comprobados con el catalogo del juego (gd_get_object_catalog):
# portal_01..18, boost_01..05, ring_*, bump_*.
GAMEPLAY_IDS = {
    "block": 1, "spike": 8,
    "portal_cube": 12, "portal_ship": 13, "portal_ball": 47, "portal_ufo": 111,
    "portal_wave": 660, "portal_robot": 745, "portal_spider": 1331, "portal_swing": 1933,
    "gravity_normal": 10, "gravity_flip": 11, "mirror_on": 45, "mirror_off": 46,
    "size_normal": 99, "size_mini": 101, "dual_on": 286, "dual_off": 287,
    "speed_0.5x": 200, "speed_1x": 201, "speed_2x": 202, "speed_3x": 203, "speed_4x": 1334,
    "pad_yellow": 35, "pad_pink": 140, "pad_red": 1332, "pad_blue": 67,
    "orb_yellow": 36, "orb_pink": 141, "orb_red": 1333, "orb_blue": 84,
    "orb_green": 1022, "orb_black": 1330, "orb_dash_green": 1704, "orb_dash_pink": 1751,
}
GameplayKind = Literal[tuple(GAMEPLAY_IDS)]  # type: ignore[valid-type]


class GameplayItem(BaseModel):
    kind: Annotated[GameplayKind, Field(description="que objeto poner")]
    x: Annotated[float, Field(ge=-1000, le=1_000_000, description="X del centro; columnas en 15 + 30*n")]
    y: Annotated[float, Field(ge=-3000, le=30_000, description="Y del centro; 105 = fila del suelo. Portales: unos 135 a ras de suelo")] = 105
    rotation: Annotated[float, Field(ge=-360, le=360)] = 0
    flipY: bool = False
    groups: GroupList | None = None


@mcp.tool()
def gd_place_gameplay_objects(
    items: Annotated[list[GameplayItem], Field(min_length=1, max_length=MAX_BATCH)],
    batch_id: str | None = None,
) -> dict:
    """Coloca objetos de gameplay por nombre (portales de modo, gravedad,
    tamano, espejo, dual, velocidad, pads, orbes, bloque, pincho) sin tener
    que saber su id. Cuadricula: centros en x = 15 + 30*n; fila del suelo
    y = 105. Se deshace con gd_undo_last_batch."""
    objects = []
    for it in items:
        o = {"id": GAMEPLAY_IDS[it.kind], "x": it.x, "y": it.y, "rotation": it.rotation, "flipY": it.flipY}
        if it.groups:
            o["groups"] = it.groups
        objects.append(o)
    key = batch_id or f"gameplay-{uuid.uuid4()}"
    result = _safe("add_objects_batch", {"objects": objects}, key)
    result.setdefault("batchId", key)
    return result


COLOR_CHANNELS = {"bg": 1000, "ground": 1001, "line": 1002, "obj": 1004, "ground2": 1009}
# Unidades del move en el texto del nivel por cada bloque. Comprobado en el
# juego (Fase 5): 29=60 sube un bloque exactamente 2 bloques.
MOVE_UNITS_PER_BLOCK = 30


class TriggerSpec(BaseModel):
    type: Literal["color", "move", "alpha"]
    x: Annotated[float, Field(ge=-1000, le=1_000_000, description="se activa cuando el jugador pasa por esta X")]
    y: Annotated[float, Field(ge=-3000, le=30_000)] = 225
    duration: Annotated[float, Field(ge=0, le=600)] = 0.5
    touch_triggered: bool = False
    spawn_triggered: bool = False
    multi_trigger: bool = False
    groups: Annotated[GroupList | None, Field(description="grupos del propio trigger (para triggers spawn)")] = None
    # color
    channel: Annotated[int | Literal["bg", "ground", "line", "obj", "ground2"] | None,
                       Field(description="canal de color: numero 1-999 o bg/ground/line/obj/ground2")] = None
    r: Annotated[int, Field(ge=0, le=255)] = 255
    g: Annotated[int, Field(ge=0, le=255)] = 255
    b: Annotated[int, Field(ge=0, le=255)] = 255
    blending: bool = False
    # move / alpha
    target_group: Annotated[int, Field(ge=1, le=9999)] | None = None
    move_x_blocks: Annotated[float, Field(ge=-3000, le=3000)] = 0
    move_y_blocks: Annotated[float, Field(ge=-3000, le=3000)] = 0
    easing: Annotated[int, Field(ge=0, le=18)] = 0
    easing_rate: Annotated[float, Field(ge=0.1, le=20)] = 2
    opacity: Annotated[float, Field(ge=0, le=1)] = 1


TRIGGER_IDS = {"color": 899, "move": 901, "alpha": 1007}


def _trigger_to_object(t: TriggerSpec) -> dict:
    props: dict = {"10": t.duration}
    if t.touch_triggered:
        props["11"] = True
    if t.spawn_triggered:
        props["62"] = True
    if t.multi_trigger:
        props["87"] = True
    if t.type == "color":
        if t.channel is None:
            raise ValueError("el trigger de color necesita 'channel'")
        ch = COLOR_CHANNELS[t.channel] if isinstance(t.channel, str) else t.channel
        if not 1 <= ch <= 1101:
            raise ValueError("canal de color fuera de rango")
        props.update({"23": ch, "7": t.r, "8": t.g, "9": t.b, "35": t.opacity})
        if t.blending:
            props["17"] = True
    else:
        if t.target_group is None:
            raise ValueError(f"el trigger {t.type} necesita 'target_group'")
        props["51"] = t.target_group
        if t.type == "move":
            props["28"] = t.move_x_blocks * MOVE_UNITS_PER_BLOCK
            props["29"] = t.move_y_blocks * MOVE_UNITS_PER_BLOCK
            props["30"] = t.easing
            if t.easing:
                props["85"] = t.easing_rate
        else:
            props["35"] = t.opacity
    obj = {"id": TRIGGER_IDS[t.type], "x": t.x, "y": t.y, "props": props}
    if t.groups:
        obj["groups"] = t.groups
    return obj


@mcp.tool()
def gd_add_triggers(
    triggers: Annotated[list[TriggerSpec], Field(min_length=1, max_length=200)],
    batch_id: str | None = None,
) -> dict:
    """Crea triggers de color, move y alpha. Color: channel + r,g,b
    (+opacity, blending). Move: target_group + move_x_blocks/move_y_blocks
    (+easing). Alpha: target_group + opacity. Para que un objeto se vea
    afectado, ponle ese grupo con 'groups' al crearlo. La respuesta dice si
    el juego guardo cada propiedad (propsOk)."""
    try:
        objects = [_trigger_to_object(t) for t in triggers]
    except ValueError as e:
        return {"ok": False, "error": "invalid_params", "message": str(e)}
    key = batch_id or f"triggers-{uuid.uuid4()}"
    result = _safe("add_objects_batch", {"objects": objects}, key)
    result.setdefault("batchId", key)
    return result


@mcp.tool()
def gd_get_level_summary() -> dict:
    """Resumen del nivel abierto: numero de objetos, limites, longitud en
    bloques, ids mas usados, portales/velocidades/triggers ordenados por X,
    grupos usados y el siguiente grupo libre. Llamalo antes de construir."""
    return _safe("get_level_summary")


@mcp.tool()
def gd_save_level(confirm: bool = False) -> dict:
    """Guarda el nivel abierto (como el boton Save del editor). Antes guarda
    una copia de la version anterior. Exige confirm=true: pregunta SIEMPRE
    al usuario antes de guardar y no lo hagas por tu cuenta."""
    return _safe("save_level", {"confirm": confirm}, f"save-{uuid.uuid4()}")


# ---------------------------------------------------------------- fase 6: niveles completos
#
# Generador determinista de secciones jugables a velocidad 1x. Solo usa
# patrones sencillos (pinchos, bloques, escaleras, pasillos de nave) para no
# crear saltos imposibles. Cuadricula: columna c -> x = 30*c + 15;
# fila r -> y = 105 + 30*r (fila 0 = apoyada en el suelo).

import random

BLOCK, SPIKE = GAMEPLAY_IDS["block"], GAMEPLAY_IDS["spike"]
CUBE_PATTERNS = {
    # nombre: (dificultad minima, [(col, fila, id)])
    "pincho": ("easy", [(0, 0, SPIKE)]),
    "bloque": ("easy", [(0, 0, BLOCK)]),
    "escalera": ("easy", [(0, 0, BLOCK), (1, 0, BLOCK), (2, 0, BLOCK), (2, 1, BLOCK), (3, 1, BLOCK), (4, 1, BLOCK)]),
    "doble_pincho": ("normal", [(0, 0, SPIKE), (1, 0, SPIKE)]),
    "bloque_y_pinchos": ("normal", [(0, 0, BLOCK), (2, 0, SPIKE), (3, 0, SPIKE)]),
    "triple_pincho": ("hard", [(0, 0, SPIKE), (1, 0, SPIKE), (2, 0, SPIKE)]),
    "bloque_y_3_pinchos": ("hard", [(0, 0, BLOCK), (2, 0, SPIKE), (3, 0, SPIKE), (4, 0, SPIKE)]),
}
DIFF_ORDER = {"easy": 0, "normal": 1, "hard": 2}
CUBE_GAP = {"easy": 7, "normal": 5, "hard": 4}          # bloques libres entre patrones
SHIP_HEIGHT = {"easy": 7, "normal": 6, "hard": 5}       # alto del pasillo de nave
SHIP_OPENING = {"easy": 4, "normal": 3, "hard": 3}      # hueco minimo junto a un pilar
SHIP_SPACING = {"easy": 8, "normal": 6, "hard": 5}      # bloques entre pilares
SECTION_COLORS = [(40, 90, 200), (150, 40, 180), (30, 150, 90), (200, 90, 30), (180, 30, 60), (30, 140, 170)]


def _cell(col: int, row: int, oid: int, **extra) -> dict:
    o = {"id": oid, "x": col * 30 + 15, "y": 105 + row * 30}
    o.update(extra)
    return o


class SectionPlan(BaseModel):
    mode: Literal["cube", "ship"]
    length_blocks: Annotated[int, Field(ge=20, le=300)]
    difficulty: Literal["easy", "normal", "hard"] = "easy"


def _gen_cube(rng: random.Random, start: int, plan: SectionPlan, prev: dict | None) -> tuple[list, dict]:
    objs, used = [], []
    end = start + plan.length_blocks
    col = start + 2
    if prev and prev["mode"] != "cube":
        objs.append(_cell(start + 1, 0, GAMEPLAY_IDS["portal_cube"], y=prev["center_y"]))
        col = start + 8                      # sitio para caer al suelo
    else:
        col = start + (12 if prev is None else 4)
    allowed = [n for n, (d, _) in CUBE_PATTERNS.items() if DIFF_ORDER[d] <= DIFF_ORDER[plan.difficulty]]
    while True:
        name = rng.choice(allowed)
        cells = CUBE_PATTERNS[name][1]
        width = max(c for c, _, _ in cells) + 1
        if col + width > end - 4:
            break
        for c, r, oid in cells:
            objs.append(_cell(col + c, r, oid))
        used.append(name)
        col += width + CUBE_GAP[plan.difficulty] + rng.randint(0, 2)
    return objs, {"mode": "cube", "patterns": used, "center_y": 135}


def _gen_ship(rng: random.Random, start: int, plan: SectionPlan, prev: dict | None) -> tuple[list, dict]:
    objs = []
    end = start + plan.length_blocks
    h = SHIP_HEIGHT[plan.difficulty]
    center_y = 105 + (h - 1) * 30 / 2
    objs.append(_cell(start + 1, 0, GAMEPLAY_IDS["portal_ship"], y=center_y))
    for c in range(start + 4, end - 2):
        objs.append(_cell(c, h, BLOCK))                     # techo del pasillo
    pillars = 0
    top = rng.random() < 0.5
    pillar_h = h - SHIP_OPENING[plan.difficulty]
    col = start + 10
    while col <= end - 8 and pillar_h > 0:
        rows = range(h - pillar_h, h) if top else range(0, pillar_h)
        for r in rows:
            objs.append(_cell(col, r, BLOCK))
        pillars += 1
        top = not top
        col += SHIP_SPACING[plan.difficulty] + rng.randint(0, 2)
    return objs, {"mode": "ship", "pillars": pillars, "center_y": center_y, "height": h}


def generate_level(sections: list[SectionPlan], start_col: int, seed: int, colors: bool) -> tuple[list[list], list[dict]]:
    rng = random.Random(seed)
    out, infos, prev = [], [], None
    col = start_col
    for i, plan in enumerate(sections):
        if i == 0 and plan.mode != "cube":
            prev = {"mode": "cube", "center_y": 135}
        gen = _gen_cube if plan.mode == "cube" else _gen_ship
        objs, info = gen(rng, col, plan, prev)
        if colors:
            r, g, b = SECTION_COLORS[i % len(SECTION_COLORS)]
            objs.append({"id": 899, "x": col * 30 + 15, "y": 465,
                         "props": {"23": 1000, "7": r, "8": g, "9": b, "10": 1}})
        info.update({"index": i, "difficulty": plan.difficulty, "fromX": col * 30, "toX": (col + plan.length_blocks) * 30,
                     "objects": len(objs)})
        out.append(objs)
        infos.append(info)
        prev = info
        col += plan.length_blocks
    return out, infos


def validate_level(sections: list[list]) -> list[str]:
    """Comprobaciones basicas: sin objetos solapados y sin filas de mas de 3
    pinchos seguidos en el suelo."""
    issues, seen = [], set()
    ground_spikes = set()
    for objs in sections:
        for o in objs:
            if o["id"] in (899,):
                continue
            key = (round(o["x"]), round(o["y"]))
            if key in seen:
                issues.append(f"dos objetos en x={o['x']} y={o['y']}")
            seen.add(key)
            if o["id"] == SPIKE and o["y"] == 105:
                ground_spikes.add(round(o["x"]))
    for x in ground_spikes:
        if all(x + 30 * k in ground_spikes for k in range(4)):
            issues.append(f"mas de 3 pinchos seguidos desde x={x}")
    return issues


@mcp.tool()
def gd_build_level(
    sections: Annotated[list[SectionPlan], Field(min_length=1, max_length=20)],
    seed: int = 1,
    colors: bool = True,
    start_x: Annotated[float, Field(ge=0, le=900_000)] | None = None,
    dry_run: bool = False,
) -> dict:
    """Construye un nivel jugable (velocidad 1x) por secciones de cubo y
    nave, con dificultad easy/normal/hard. Cada seccion es un lote propio:
    gd_undo_last_batch deshace la ultima seccion. Con dry_run=true solo
    devuelve el plan sin tocar el juego. Por defecto empieza despues del
    ultimo objeto del nivel. Solo construye si la zona esta vacia. NO guarda:
    para eso usa gd_save_level y pide permiso al usuario."""
    summary = None
    if start_x is None:
        # Igual en dry_run, para que el plan ensene la X real (visto en la Fase 7)
        summary = _safe("get_level_summary")
        if not summary.get("ok"):
            if not dry_run:
                return summary
            start_col = 0
        else:
            start_col = 0 if summary["objectCount"] == 0 else int(summary["bounds"]["maxX"] // 30) + 10
    else:
        start_col = int(start_x // 30)
    built, infos = generate_level(sections, start_col, seed, colors)
    issues = validate_level(built)
    total = sum(len(o) for o in built)
    plan = {"startX": start_col * 30, "endX": infos[-1]["toX"], "totalObjects": total, "sections": infos, "issues": issues}
    if issues:
        return {"ok": False, "error": "invalid_plan", **plan}
    if total > 5000:
        return {"ok": False, "error": "too_big", "message": "Maximo 5000 objetos por llamada", **plan}
    if dry_run:
        return {"ok": True, "dryRun": True, **plan}

    zone = _safe("get_objects_in_area", {"x1": plan["startX"] - 15, "x2": plan["endX"] + 15, "limit": 1})
    if not zone.get("ok"):
        return zone
    if zone.get("total", 0) > 0:
        return {"ok": False, "error": "zone_not_empty",
                "message": f"Ya hay {zone['total']} objetos entre x={plan['startX']} y x={plan['endX']}; elige otro start_x", **plan}

    run = f"level-{uuid.uuid4()}"
    batches = []
    for info, objs in zip(infos, built):
        for part in range(0, len(objs), MAX_BATCH):
            key = f"{run}-s{info['index']}-{part // MAX_BATCH}"
            res = _safe("add_objects_batch", {"objects": objs[part:part + MAX_BATCH]}, key)
            batches.append({"section": info["index"], "batchId": key, "ok": res.get("ok"),
                            "created": res.get("created"), "propsWrong": res.get("propsWrong")})
            if not res.get("ok"):
                return {"ok": False, "error": "build_failed", "failedBatch": res, "batches": batches, **plan,
                        "message": "Se paro a mitad; los lotes ya creados se pueden deshacer con gd_undo_last_batch"}
    log.info("nivel construido %s: %d objetos en %d lotes", run, total, len(batches))
    return {"ok": True, "batches": batches, "undoHint": f"{len(batches)} llamadas a gd_undo_last_batch lo quitan todo", **plan}


@mcp.prompt()
def disenar_nivel(idea: str = "un nivel corto y facil de cubo y nave") -> str:
    """Guia para crear un nivel completo con el AI Level Builder."""
    return (
        f"Quiero un nivel de Geometry Dash: {idea}.\n"
        "Pasos: 1) gd_get_editor_state y gd_get_level_summary. "
        "2) Propon un plan de secciones (modo, longitud en bloques, dificultad) y ensenalo con gd_build_level dry_run=true. "
        "3) Si me parece bien, construyelo con gd_build_level. 4) Revisa con gd_get_level_summary. "
        "5) Pideme que lo pruebe en el juego. 6) Solo guarda con gd_save_level si yo lo confirmo."
    )


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
