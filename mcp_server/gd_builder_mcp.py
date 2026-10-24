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
    "20": "capa del editor", "24": "capa Z (-5,-3,-1,0,1,3,5,7,9,11)", "25": "orden Z",
    "45": "pulse fade in", "46": "pulse hold", "47": "pulse fade out", "48": "pulse modo", "52": "pulse objetivo (0 canal, 1 grupo)",
    "371": "zoom de camara", "75": "shake fuerza", "84": "shake intervalo",
    "175": "shader speed", "176": "shader strength", "177": "shader time off", "179": "shader wave width",
    "180": "shader target X", "189": "shader target Y", "181": "shader fade in", "182": "shader fade out",
    "183": "shader inner", "191": "shader outer", "184": "shader invert", "188": "shader use X", "190": "shader use Y",
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


# ---------------------------------------------------------------- fase 6/8: niveles completos
#
# Generador determinista de niveles jugables a velocidad 1x con todos los
# modos menos swing. Cuadricula: columna c -> x = 30*c + 15; fila r ->
# y = 105 + 30*r (fila 0 = apoyada en el suelo).
#
# Reglas para que no haya fallos:
#  - Todos los portales de modo van a y=135 (fila 1). Antes de cada portal
#    los modos con pasillo tienen un "embudo" (techo bajado hasta la fila 3)
#    para que el jugador pase si o si por el portal.
#  - Bola y arana terminan con dos portales de gravedad normal apilados
#    (filas 1 y 3) bastante antes del embudo, para no salir boca abajo.
#  - Ningun obstaculo en los ultimos bloques de cada seccion.

import random

BLOCK, SPIKE = GAMEPLAY_IDS["block"], GAMEPLAY_IDS["spike"]
GRAVITY_NORMAL = GAMEPLAY_IDS["gravity_normal"]
DIFF_ORDER = {"easy": 0, "normal": 1, "hard": 2}

GROUND_PATTERNS = {
    # nombre: (dificultad minima, modos, [(col, fila, id)])
    "pincho": ("easy", ("cube", "robot"), [(0, 0, SPIKE)]),
    "bloque": ("easy", ("cube", "robot"), [(0, 0, BLOCK)]),
    "escalera": ("easy", ("cube",), [(0, 0, BLOCK), (1, 0, BLOCK), (2, 0, BLOCK), (2, 1, BLOCK), (3, 1, BLOCK), (4, 1, BLOCK)]),
    "doble_pincho": ("normal", ("cube", "robot"), [(0, 0, SPIKE), (1, 0, SPIKE)]),
    "bloque_y_pinchos": ("normal", ("cube",), [(0, 0, BLOCK), (2, 0, SPIKE), (3, 0, SPIKE)]),
    "pared_alta": ("easy", ("robot",), [(0, 0, BLOCK), (0, 1, BLOCK)]),
    "triple_pincho": ("hard", ("cube",), [(0, 0, SPIKE), (1, 0, SPIKE), (2, 0, SPIKE)]),
    "bloque_y_3_pinchos": ("hard", ("cube",), [(0, 0, BLOCK), (2, 0, SPIKE), (3, 0, SPIKE), (4, 0, SPIKE)]),
}
GROUND_GAP = {"easy": 7, "normal": 5, "hard": 4}

# modo: (portal, alto del pasillo, separacion por dificultad, invierte gravedad)
CORRIDOR_MODES = {
    "ship": ("portal_ship", 6, {"easy": 8, "normal": 6, "hard": 5}, False),
    "ball": ("portal_ball", 5, {"easy": 12, "normal": 10, "hard": 8}, True),
    "ufo": ("portal_ufo", 6, {"easy": 10, "normal": 8, "hard": 7}, False),
    "wave": ("portal_wave", 6, {"easy": 10, "normal": 9, "hard": 7}, False),
    "spider": ("portal_spider", 5, {"easy": 9, "normal": 7, "hard": 6}, True),
}
GROUND_MODES = {"cube": "portal_cube", "robot": "portal_robot"}
ALL_MODES = tuple(GROUND_MODES) + tuple(CORRIDOR_MODES)
PORTAL_Y = 135

# Paletas glow style por modo: (fondo oscuro, acento)
PALETTES = {
    "cube": ((12, 10, 40), (0, 200, 255)),
    "ship": ((30, 0, 40), (255, 60, 200)),
    "ball": ((0, 30, 25), (0, 255, 150)),
    "ufo": ((35, 25, 0), (255, 200, 0)),
    "wave": ((40, 0, 10), (255, 40, 60)),
    "robot": ((0, 15, 40), (60, 120, 255)),
    "spider": ((25, 0, 40), (190, 80, 255)),
}
CH_BLOCK, CH_GLOW = 10, 11   # canales de color propios del nivel


def _cell(col: int, row: int, oid: int, role: str = "solid", **extra) -> dict:
    o = {"id": oid, "x": col * 30 + 15, "y": 105 + row * 30, "_role": role}
    o.update(extra)
    return o


class SectionPlan(BaseModel):
    mode: Literal["cube", "robot", "ship", "ball", "ufo", "wave", "spider"]
    length_blocks: Annotated[int, Field(ge=30, le=300)]
    difficulty: Literal["easy", "normal", "hard"] = "easy"


def _gen_ground(rng: random.Random, start: int, plan: SectionPlan, prev: dict | None) -> tuple[list, dict]:
    objs, used = [], []
    end = start + plan.length_blocks
    if prev is None:
        col = start + 12
    else:
        if prev["mode"] != plan.mode:
            objs.append(_cell(start + 1, 0, GAMEPLAY_IDS[GROUND_MODES[plan.mode]], "portal", y=PORTAL_Y))
        col = start + 8
    allowed = [n for n, (d, modes, _) in GROUND_PATTERNS.items()
               if plan.mode in modes and DIFF_ORDER[d] <= DIFF_ORDER[plan.difficulty]]
    while True:
        name = rng.choice(allowed)
        cells = GROUND_PATTERNS[name][2]
        width = max(c for c, _, _ in cells) + 1
        if col + width > end - 6:
            break
        for c, r, oid in cells:
            objs.append(_cell(col + c, r, oid, "hazard" if oid == SPIKE else "solid"))
        used.append(name)
        col += width + GROUND_GAP[plan.difficulty] + rng.randint(0, 2)
    return objs, {"mode": plan.mode, "patterns": used}


def _gen_corridor(rng: random.Random, start: int, plan: SectionPlan, prev: dict | None) -> tuple[list, dict]:
    portal, h, spacing, flips = CORRIDOR_MODES[plan.mode]
    d = plan.difficulty
    end = start + plan.length_blocks
    objs = [_cell(start + 1, 0, GAMEPLAY_IDS[portal], "portal", y=PORTAL_Y)]
    funnel_from = end - 6
    for c in range(start + 4, end):
        objs.append(_cell(c, h, BLOCK, "ceiling"))
        if c >= funnel_from:                               # embudo hacia el siguiente portal
            for r in range(3, h):
                objs.append(_cell(c, r, BLOCK, "ceiling"))
    last_obstacle = end - (20 if flips else 14)
    col = start + 12
    count = 0
    while col <= last_obstacle:
        floor_turn = count % 2 == 0
        if plan.mode in ("ball", "spider"):
            k = 2 if d == "easy" else 3
            row = 0 if floor_turn else h - 1
            for i in range(k):
                objs.append(_cell(col + i, row, SPIKE, "hazard", **({} if floor_turn else {"rotation": 180})))
            col += k - 1
        else:
            if plan.mode == "ship":
                p = h - (4 if d == "easy" else 3)
            elif plan.mode == "ufo":
                p = 2
            else:  # wave
                p = 2 if d == "easy" else 3
            rows = range(0, p) if floor_turn else range(h - p, h)
            for r in rows:
                objs.append(_cell(col, r, BLOCK, "solid"))
        count += 1
        col += spacing[d] + rng.randint(0, 2)
    if flips:
        # vuelta a gravedad normal, con sitio para caer antes del embudo
        objs.append(_cell(end - 12, 1, GRAVITY_NORMAL, "portal"))
        objs.append(_cell(end - 12, 3, GRAVITY_NORMAL, "portal"))
    return objs, {"mode": plan.mode, "obstacles": count, "height": h}


def _decorate(i: int, start: int, plan: SectionPlan, objs: list, first: bool) -> list:
    """Glow style basico: colores por seccion (fondo oscuro, suelo, linea y
    bloques del color del modo, glow con blending) y un pulso al empezar."""
    bg, acc = PALETTES[plan.mode]
    dur = 0 if first else 0.6
    x0 = start * 30 + 15
    def col_trig(dy: int, channel: int, rgb, blending: bool = False) -> dict:
        p = {"23": channel, "7": rgb[0], "8": rgb[1], "9": rgb[2], "10": dur}
        if blending:
            p["17"] = True
        return {"id": 899, "x": x0, "y": 400 + dy * 30, "props": p, "_role": "trigger"}
    dark = tuple(max(0, int(v * 0.6)) for v in bg)
    block = tuple(int(v * 0.2) for v in acc)
    out = [
        col_trig(0, 1000, bg),
        col_trig(1, 1001, dark),
        col_trig(2, 1002, acc),
        col_trig(3, CH_BLOCK, block),
        col_trig(4, CH_GLOW, acc, blending=True),
    ]
    if not first:
        # pulso blanco corto del glow al entrar en cada seccion
        out.append({"id": 1006, "x": x0, "y": 400 + 5 * 30, "_role": "trigger",
                    "props": {"23": CH_GLOW, "52": 0, "7": 255, "8": 255, "9": 255, "45": 0, "46": 0.1, "47": 0.4}})
    for o in objs:
        if o["_role"] in ("solid", "ceiling") and o["id"] == BLOCK:
            o.setdefault("props", {})["21"] = CH_BLOCK
    return out


def generate_level(sections: list[SectionPlan], start_col: int, seed: int, colors: bool) -> tuple[list[list], list[dict]]:
    rng = random.Random(seed)
    out, infos, prev = [], [], None
    col = start_col
    for i, plan in enumerate(sections):
        if plan.mode in GROUND_MODES:
            objs, info = _gen_ground(rng, col, plan, prev)
        else:
            objs, info = _gen_corridor(rng, col, plan, prev)
        if colors:
            objs += _decorate(i, col, plan, objs, prev is None)
        info.update({"index": i, "difficulty": plan.difficulty, "fromX": col * 30, "toX": (col + plan.length_blocks) * 30,
                     "objects": len(objs)})
        out.append(objs)
        infos.append(info)
        prev = info
        col += plan.length_blocks
    return out, infos


def _strip(objs: list) -> list:
    return [{k: v for k, v in o.items() if not k.startswith("_")} for o in objs]


def validate_level(sections: list[list]) -> list[str]:
    """Sin objetos solapados, sin mas de 3 pinchos seguidos en el suelo."""
    issues, seen = [], {}
    ground_spikes = set()
    for objs in sections:
        for o in objs:
            if o.get("_role") == "trigger":
                continue
            key = (round(o["x"]), round(o["y"]))
            if key in seen and not (o["id"] == GRAVITY_NORMAL or seen[key] == GRAVITY_NORMAL):
                issues.append(f"dos objetos en x={o['x']} y={o['y']}")
            seen[key] = o["id"]
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
    """Construye un nivel jugable (velocidad 1x) por secciones. Modos: cube,
    robot, ship, ball, ufo, wave, spider; dificultad easy/normal/hard. Pone
    los portales con embudo y portales de gravedad donde hace falta. Con
    colors=true decora en glow style (colores por seccion, glow con
    blending, pulsos). Cada seccion es un lote propio: gd_undo_last_batch
    deshace la ultima. dry_run=true solo devuelve el plan. Solo construye si
    la zona esta vacia. NO guarda: usa gd_save_level con permiso."""
    summary = None
    if start_x is None:
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
    plan = {"startX": start_col * 30, "endX": infos[-1]["toX"], "totalObjects": total, "sections": infos, "issues": issues,
            "seconds": round((infos[-1]["toX"] - start_col * 30) / 311.58, 1)}
    if issues:
        return {"ok": False, "error": "invalid_plan", **plan}
    if total > 8000:
        return {"ok": False, "error": "too_big", "message": "Maximo 8000 objetos por llamada", **plan}
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
        clean = _strip(objs)
        for part in range(0, len(clean), MAX_BATCH):
            key = f"{run}-s{info['index']}-{part // MAX_BATCH}"
            res = _safe("add_objects_batch", {"objects": clean[part:part + MAX_BATCH]}, key)
            batches.append({"section": info["index"], "batchId": key, "ok": res.get("ok"),
                            "created": res.get("created"), "propsWrong": res.get("propsWrong")})
            if not res.get("ok"):
                return {"ok": False, "error": "build_failed", "failedBatch": res, "batches": batches, **plan,
                        "message": "Se paro a mitad; los lotes ya creados se pueden deshacer con gd_undo_last_batch"}
    log.info("nivel construido %s: %d objetos en %d lotes", run, total, len(batches))
    return {"ok": True, "batches": batches, "undoHint": f"{len(batches)} llamadas a gd_undo_last_batch lo quitan todo", **plan}


@mcp.tool()
def gd_set_level_song(
    song_id: Annotated[int, Field(ge=1, le=99_999_999, description="ID de Newgrounds o de la biblioteca de musica")] | None = None,
    official_track: Annotated[int, Field(ge=0, le=100, description="cancion oficial de RobTop (0 = Stereo Madness)")] | None = None,
) -> dict:
    """Pone la cancion del nivel abierto. Se guarda al guardar el nivel. Si
    no esta descargada, el usuario la descarga desde los ajustes del nivel."""
    if (song_id is None) == (official_track is None):
        return {"ok": False, "error": "invalid_params", "message": "Indica song_id u official_track (solo uno)"}
    payload = {"songID": song_id} if song_id is not None else {"officialTrack": official_track}
    return _safe("set_level_song", payload, f"song-{uuid.uuid4()}")


@mcp.tool()
def gd_set_editor_view(
    x: Annotated[float, Field(ge=-1000, le=1_000_000)],
    y: Annotated[float, Field(ge=-3000, le=30_000)] = 200,
    zoom: Annotated[float, Field(ge=0.1, le=4)] = 0.6,
) -> dict:
    """Mueve la camara del editor para centrarla en (x, y) con un zoom. Util
    para revisar con capturas de pantalla lo que se ha construido."""
    return _safe("set_editor_view", {"x": x, "y": y, "zoom": zoom})



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
