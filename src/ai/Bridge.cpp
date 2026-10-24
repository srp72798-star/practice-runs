// AI Level Builder - puente entre el servidor local y el editor de GD.
//
// Reglas:
//  - El hilo del servidor nunca toca objetos de cocos2d: cada herramienta se
//    ejecuta en el hilo principal (queueInMainThread) y el servidor espera el
//    resultado con un tiempo limite.
//  - Todo esta desactivado salvo que se active la opcion "ai-builder".
//  - Cada arranque genera un token nuevo y lo guarda en ai-bridge.json dentro
//    de la carpeta de guardado del mod; el servidor MCP lo lee de ahi.

#include <Geode/Geode.hpp>

#include "Server.hpp"

#include <chrono>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;

namespace {
    constexpr int kMaxBatch = 1000;
    constexpr char const* kBridgeVersion = "0.4.0";

    // ---------------- respuestas ----------------

    ai::Response reply(matjson::Value value, int status = 200) {
        return {status, value.dump(matjson::NO_INDENTATION)};
    }

    ai::Response fail(std::string const& code, std::string const& message, int status = 400) {
        return reply(matjson::makeObject({
            {"ok", false},
            {"error", code},
            {"message", message},
        }), status);
    }

    // ---------------- hilo principal ----------------

    ai::Response onMainThread(std::function<ai::Response()> fn) {
        auto promise = std::make_shared<std::promise<ai::Response>>();
        auto future = promise->get_future();
        Loader::get()->queueInMainThread([promise, fn = std::move(fn)] {
            try {
                promise->set_value(fn());
            } catch (std::exception const& e) {
                promise->set_value(fail("exception", e.what(), 500));
            }
        });
        if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            return fail("game_not_running",
                "El juego no proceso la peticion en 10 s. Geometry Dash tiene que estar en modo ventana "
                "y sin minimizar (en pantalla completa se pausa al perder el foco).", 504);
        }
        return future.get();
    }

    // ---------------- idempotencia ----------------
    // Si el servidor MCP reintenta una peticion con la misma clave, se
    // devuelve la respuesta guardada en vez de repetir la operacion.

    std::mutex g_idemMutex;
    std::unordered_map<std::string, ai::Response> g_idemResults;
    std::deque<std::string> g_idemOrder;

    std::optional<ai::Response> cachedResult(std::string const& key) {
        if (key.empty()) return std::nullopt;
        std::lock_guard lock(g_idemMutex);
        auto it = g_idemResults.find(key);
        if (it == g_idemResults.end()) return std::nullopt;
        return it->second;
    }

    void storeResult(std::string const& key, ai::Response const& res) {
        if (key.empty() || res.status != 200) return;
        std::lock_guard lock(g_idemMutex);
        if (g_idemResults.emplace(key, res).second) {
            g_idemOrder.push_back(key);
            while (g_idemOrder.size() > 200) {
                g_idemResults.erase(g_idemOrder.front());
                g_idemOrder.pop_front();
            }
        }
    }

    // ---------------- utilidades ----------------

    std::string randomToken() {
        std::random_device rd;
        std::uniform_int_distribution<int> dist(0, 15);
        std::string token;
        for (int i = 0; i < 48; i++) token += "0123456789abcdef"[dist(rd)];
        return token;
    }

    bool isValidObjectID(int id) {
        auto& keys = ObjectToolbox::sharedState()->m_allKeys;
        return keys.find(id) != keys.end();
    }

    std::string numberToString(double v) {
        auto s = fmt::format("{:.3f}", v);
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }

    // ---------------- herramientas ----------------

    ai::Response toolPing() {
        return reply(matjson::makeObject({
            {"ok", true},
            {"bridge", kBridgeVersion},
            {"mod", Mod::get()->getVersion().toVString()},
        }));
    }

    ai::Response toolGetEditorState() {
        auto editor = LevelEditorLayer::get();
        if (!editor) {
            return reply(matjson::makeObject({{"ok", true}, {"inEditor", false}}));
        }
        std::string name = editor->m_level ? std::string(editor->m_level->m_levelName.c_str()) : "";
        return reply(matjson::makeObject({
            {"ok", true},
            {"inEditor", true},
            {"levelName", name},
            {"objectCount", static_cast<int>(editor->m_objects->count())},
        }));
    }

    // Propiedades de objeto que la IA puede poner (clave del formato del
    // nivel). Las claves de triggers salen de los comentarios "property N"
    // de los bindings de Geode (EffectGameObject / GameObject); 7/8/9 (RGB del
    // trigger de color) se comprueban al releer el objeto del juego.
    struct PropRule { char const* key; double min; double max; char kind; }; // kind: i=entero, f=decimal, b=bool
    constexpr PropRule kPropRules[] = {
        {"7", 0, 255, 'i'}, {"8", 0, 255, 'i'}, {"9", 0, 255, 'i'},   // color del trigger de color
        {"10", 0, 600, 'f'},                                          // duracion
        {"11", 0, 1, 'b'},                                            // touch triggered
        {"17", 0, 1, 'b'},                                            // blending
        {"21", 0, 1101, 'i'}, {"22", 0, 1101, 'i'},                   // canal de color principal / detalle
        {"23", 1, 1101, 'i'},                                         // canal de color objetivo
        {"28", -100000, 100000, 'f'}, {"29", -100000, 100000, 'f'},   // desplazamiento del move
        {"30", 0, 18, 'i'},                                           // easing
        {"35", 0, 1, 'f'},                                            // opacidad
        {"51", 1, 9999, 'i'},                                         // grupo objetivo
        {"62", 0, 1, 'b'},                                            // spawn triggered
        {"85", 0.1, 20, 'f'},                                         // easing rate
        {"87", 0, 1, 'b'},                                            // multi trigger
    };

    PropRule const* findPropRule(std::string const& key) {
        for (auto const& r : kPropRules) if (key == r.key) return &r;
        return nullptr;
    }

    using KeyValueList = std::vector<std::pair<std::string, std::string>>;

    // "groups": [1, 2] y "props": {"10": 0.5, ...} -> pares clave/valor ya
    // validados. Grupos en la clave 57 ("1.2").
    std::optional<std::string> parseExtras(matjson::Value const& o, KeyValueList& out, bool& groupsGiven) {
        groupsGiven = false;
        if (o.contains("groups")) {
            groupsGiven = true;
            auto const& g = o["groups"];
            if (!g.isArray()) return std::string("'groups' debe ser una lista de numeros");
            auto const& items = g.asArray().unwrap();
            if (items.size() > 10) return std::string("Maximo 10 grupos por objeto");
            std::set<int> unique;
            for (auto const& item : items) {
                auto v = item.asInt();
                if (!v || v.unwrap() < 1 || v.unwrap() > 9999) return std::string("Los grupos van de 1 a 9999");
                unique.insert(static_cast<int>(v.unwrap()));
            }
            std::string joined;
            for (int id : unique) joined += (joined.empty() ? "" : ".") + std::to_string(id);
            out.emplace_back("57", joined);
        }
        if (o.contains("props")) {
            auto const& p = o["props"];
            if (!p.isObject()) return std::string("'props' debe ser un objeto {clave: valor}");
            for (auto const& [key, value] : p) {
                auto rule = findPropRule(key);
                if (!rule) return fmt::format("La propiedad '{}' no esta permitida", key);
                double v;
                if (value.isBool()) v = value.asBool().unwrap() ? 1 : 0;
                else if (auto d = value.asDouble()) v = d.unwrap();
                else return fmt::format("La propiedad '{}' necesita un numero", key);
                if (v < rule->min || v > rule->max) {
                    return fmt::format("La propiedad '{}' va de {} a {}", key, rule->min, rule->max);
                }
                if (rule->kind != 'f' && v != std::floor(v)) return fmt::format("La propiedad '{}' debe ser entera", key);
                out.emplace_back(key, numberToString(v));
            }
        }
        return std::nullopt;
    }

    struct ObjectSpec {
        int id = 0;
        double x = 0, y = 0;
        double rotation = 0, scale = 1;
        bool flipX = false, flipY = false;
        KeyValueList extra;  // grupos y propiedades validados
    };

    // Valida y convierte la lista de objetos del JSON. Devuelve el error en
    // texto si algo no es valido; en ese caso no se crea nada.
    std::optional<std::string> parseObjects(matjson::Value const& list, std::vector<ObjectSpec>& out) {
        if (!list.isArray()) return "'objects' debe ser una lista";
        auto const& items = list.asArray().unwrap();
        if (items.empty()) return "'objects' esta vacia";
        if (static_cast<int>(items.size()) > kMaxBatch) {
            return fmt::format("Maximo {} objetos por lote (llegaron {})", kMaxBatch, items.size());
        }
        for (size_t i = 0; i < items.size(); i++) {
            auto const& o = items[i];
            if (!o.isObject()) return fmt::format("objects[{}] no es un objeto", i);
            ObjectSpec s;
            auto id = o["id"].asInt();
            auto x = o["x"].asDouble();
            auto y = o["y"].asDouble();
            if (!id || !x || !y) return fmt::format("objects[{}] necesita id, x e y numericos", i);
            s.id = static_cast<int>(id.unwrap());
            s.x = x.unwrap();
            s.y = y.unwrap();
            if (o.contains("rotation")) s.rotation = o["rotation"].asDouble().unwrapOr(0);
            if (o.contains("scale")) s.scale = o["scale"].asDouble().unwrapOr(1);
            if (o.contains("flipX")) s.flipX = o["flipX"].asBool().unwrapOr(false);
            if (o.contains("flipY")) s.flipY = o["flipY"].asBool().unwrapOr(false);

            if (!isValidObjectID(s.id)) return fmt::format("objects[{}]: el id {} no existe en el juego", i, s.id);
            if (s.x < -1000 || s.x > 1000000) return fmt::format("objects[{}]: x fuera de rango", i);
            if (s.y < -3000 || s.y > 30000) return fmt::format("objects[{}]: y fuera de rango", i);
            if (s.rotation < -360 || s.rotation > 360) return fmt::format("objects[{}]: rotation fuera de rango", i);
            if (s.scale < 0.1 || s.scale > 10) return fmt::format("objects[{}]: scale fuera de rango (0.1-10)", i);
            bool groupsGiven = false;
            if (auto err = parseExtras(o, s.extra, groupsGiven)) return fmt::format("objects[{}]: {}", i, *err);
            out.push_back(s);
        }
        return std::nullopt;
    }

    // En el texto del nivel la Y se cuenta desde el suelo (y=90 del editor).
    // Comprobado en el juego: "3,105" acababa en y=195.
    constexpr double kGroundOffsetY = 90.0;

    // Formato de objetos del nivel: 1=id, 2=x, 3=y, 4=flipX, 5=flipY,
    // 6=rotacion, 128/129=escala X/Y
    std::string toObjectString(std::vector<ObjectSpec> const& specs) {
        std::string out;
        for (auto const& s : specs) {
            out += fmt::format("1,{},2,{},3,{}", s.id, numberToString(s.x), numberToString(s.y - kGroundOffsetY));
            if (s.flipX) out += ",4,1";
            if (s.flipY) out += ",5,1";
            if (s.rotation != 0) out += ",6," + numberToString(s.rotation);
            if (s.scale != 1) out += ",128," + numberToString(s.scale) + ",129," + numberToString(s.scale);
            for (auto const& [k, v] : s.extra) {
                if (k == "57" && v.empty()) continue;
                out += "," + k + "," + v;
            }
            out += ";";
        }
        return out;
    }

    // ---------------- historial de lotes (para undo_last_batch) ----------------
    // Cada lote guarda que objetos creo (para quitarlos al deshacer) y el texto
    // completo de los que quito (para volver a crearlos). Se usa el propio
    // serializador del juego (getSaveString), asi no se pierde ninguna
    // propiedad (grupos, colores, ajustes de triggers...).

    struct HistoryEntry {
        std::string batchId;
        std::string type;                          // add / modify / delete
        std::vector<Ref<GameObject>> created;      // quitar al deshacer
        std::vector<std::string> removed;          // recrear al deshacer
        std::vector<Ref<GameObject>> removedRefs;  // los objetos que quito (mismo orden que removed)
    };

    // Cuando el juego recrea un objeto (al modificar o al deshacer), el
    // objeto nuevo es otro distinto con otro uid. Los lotes anteriores del
    // historial tienen que apuntar al nuevo, o al deshacerlos no lo
    // encontrarian (fallo visto en la prueba de la Fase 3).
    void remapHistory(std::vector<HistoryEntry>& history, GameObject* from, GameObject* to) {
        if (!from || from == to) return;
        for (auto& entry : history) {
            for (auto& ref : entry.created) {
                if (ref.data() == from) ref = Ref<GameObject>(to);
            }
        }
    }

    LevelEditorLayer* g_historyEditor = nullptr;
    std::vector<HistoryEntry> g_history;
    constexpr size_t kMaxHistory = 50;

    std::vector<HistoryEntry>& historyFor(LevelEditorLayer* editor) {
        // Si se ha abierto otro nivel (u otra vez el editor), el historial
        // anterior ya no vale
        if (editor != g_historyEditor) {
            g_history.clear();
            g_historyEditor = editor;
        }
        return g_history;
    }

    void pushHistory(LevelEditorLayer* editor, HistoryEntry entry) {
        auto& history = historyFor(editor);
        history.push_back(std::move(entry));
        if (history.size() > kMaxHistory) history.erase(history.begin());
    }

    std::string saveStringOf(GameObject* obj, LevelEditorLayer* editor) {
        return std::string(obj->getSaveString(editor).c_str());
    }

    // Texto de objeto "k,v,k,v,..." <-> lista ordenada de pares
    using KeyValues = std::vector<std::pair<std::string, std::string>>;

    KeyValues splitObjectString(std::string const& str) {
        KeyValues out;
        size_t pos = 0;
        while (pos < str.size()) {
            auto comma1 = str.find(',', pos);
            if (comma1 == std::string::npos) break;
            auto comma2 = str.find(',', comma1 + 1);
            auto value = str.substr(comma1 + 1, comma2 == std::string::npos ? std::string::npos : comma2 - comma1 - 1);
            out.emplace_back(str.substr(pos, comma1 - pos), value);
            if (comma2 == std::string::npos) break;
            pos = comma2 + 1;
        }
        return out;
    }

    void setKey(KeyValues& kv, std::string const& key, std::string const& value) {
        for (auto& [k, v] : kv) {
            if (k == key) { v = value; return; }
        }
        kv.emplace_back(key, value);
    }

    void removeKey(KeyValues& kv, std::string const& key) {
        std::erase_if(kv, [&](auto const& p) { return p.first == key; });
    }

    std::string joinObjectString(KeyValues const& kv) {
        std::string out;
        for (auto const& [k, v] : kv) {
            if (!out.empty()) out += ",";
            out += k + "," + v;
        }
        return out;
    }

    std::unordered_map<int, GameObject*> objectsByUid(LevelEditorLayer* editor) {
        std::unordered_map<int, GameObject*> map;
        for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) map[obj->m_uniqueID] = obj;
        return map;
    }

    std::set<int> parseGroupList(std::string const& v) {
        std::set<int> out;
        size_t pos = 0;
        while (pos <= v.size()) {
            auto dot = v.find('.', pos);
            auto part = v.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
            if (!part.empty()) out.insert(std::atoi(part.c_str()));
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
        return out;
    }

    // Relee el objeto tal como lo guarda el juego y compara las propiedades
    // pedidas. "wrong" = el juego tiene otro valor; "absent" = el juego no la
    // escribe (suele pasar con valores por defecto).
    void verifyExtras(GameObject* obj, LevelEditorLayer* editor, KeyValueList const& extra,
                      std::vector<std::string>& wrong, std::vector<std::string>& absent) {
        if (extra.empty()) return;
        auto kv = splitObjectString(std::string(obj->getSaveString(editor).c_str()));
        for (auto const& [key, want] : extra) {
            auto it = std::find_if(kv.begin(), kv.end(), [&](auto const& p) { return p.first == key; });
            if (it == kv.end()) {
                bool emptyish = want.empty() || want == "0";
                if (!emptyish) absent.push_back(key);
                continue;
            }
            bool same;
            if (key == "57") same = parseGroupList(it->second) == parseGroupList(want);
            else {
                double a = std::atof(it->second.c_str()), b = std::atof(want.c_str());
                same = std::abs(a - b) <= 0.01 + std::abs(b) * 0.001;
            }
            if (!same) wrong.push_back(fmt::format("{}: pedido {} / juego {}", key, want, it->second));
        }
    }

    matjson::Value stringList(std::vector<std::string> const& v) {
        auto arr = matjson::Value::array();
        for (auto const& s : v) arr.push(s);
        return arr;
    }

    // Grupos y propiedades permitidas tal como las guarda el juego
    void addDetails(matjson::Value& d, GameObject* obj, LevelEditorLayer* editor) {
        auto kv = splitObjectString(std::string(obj->getSaveString(editor).c_str()));
        auto groups = matjson::Value::array();
        auto props = matjson::Value::object();
        for (auto const& [k, v] : kv) {
            if (k == "57") { for (int g : parseGroupList(v)) groups.push(g); }
            else if (findPropRule(k)) props[k] = std::atof(v.c_str());
        }
        d["groups"] = groups;
        d["props"] = props;
    }

    void deselectAll(LevelEditorLayer* editor) {
        if (editor->m_editorUI) editor->m_editorUI->deselectAll();
    }

    matjson::Value describe(GameObject* obj) {
        return matjson::makeObject({
            {"uid", obj->m_uniqueID},
            {"id", obj->m_objectID},
            {"x", obj->getPositionX()},
            {"y", obj->getPositionY()},
            {"rotation", obj->getRotation()},
            {"scale", obj->m_scaleX},
            {"flipX", obj->m_isFlipX},
            {"flipY", obj->m_isFlipY},
        });
    }

    std::optional<std::vector<int>> parseUidList(matjson::Value const& value, std::string& error, size_t max) {
        if (!value.isArray()) { error = "'uids' debe ser una lista de numeros"; return std::nullopt; }
        auto const& items = value.asArray().unwrap();
        if (items.empty()) { error = "'uids' esta vacia"; return std::nullopt; }
        if (items.size() > max) { error = fmt::format("Maximo {} uids por lote", max); return std::nullopt; }
        std::vector<int> uids;
        for (auto const& item : items) {
            auto v = item.asInt();
            if (!v) { error = "'uids' solo puede tener numeros"; return std::nullopt; }
            uids.push_back(static_cast<int>(v.unwrap()));
        }
        return uids;
    }

    ai::Response toolGetObjectCatalog(matjson::Value const& body) {
        std::string query = body.contains("query") ? body["query"].asString().unwrapOr(std::string()) : "";
        int limit = static_cast<int>(std::clamp<std::intmax_t>(body["limit"].asInt().unwrapOr(100), 1, 500));
        int offset = static_cast<int>(std::max<std::intmax_t>(body["offset"].asInt().unwrapOr(0), 0));
        for (auto& c : query) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        auto items = matjson::Value::array();
        int matched = 0;
        for (auto const& [id, frame] : ObjectToolbox::sharedState()->m_allKeys) {
            std::string name = frame.c_str();
            std::string lower = name;
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!query.empty() && lower.find(query) == std::string::npos) continue;
            if (matched >= offset && matched < offset + limit) {
                items.push(matjson::makeObject({{"id", id}, {"frame", name}}));
            }
            matched++;
        }
        return reply(matjson::makeObject({
            {"ok", true},
            {"total", matched},
            {"offset", offset},
            {"items", items},
        }));
    }

    ai::Response toolGetObjectsInArea(matjson::Value const& body) {
        auto editor = LevelEditorLayer::get();
        if (!editor) return fail("not_in_editor", "Abre un nivel en el editor primero", 409);
        double x1 = body["x1"].asDouble().unwrapOr(-1e9), x2 = body["x2"].asDouble().unwrapOr(1e9);
        double y1 = body["y1"].asDouble().unwrapOr(-1e9), y2 = body["y2"].asDouble().unwrapOr(1e9);
        int limit = static_cast<int>(std::clamp<std::intmax_t>(body["limit"].asInt().unwrapOr(500), 1, 2000));
        std::optional<int> idFilter;
        if (body.contains("id")) idFilter = static_cast<int>(body["id"].asInt().unwrapOr(-1));
        bool details = body["details"].asBool().unwrapOr(false);
        if (details) limit = std::min(limit, 500);

        auto list = matjson::Value::array();
        int matched = 0;
        for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
            float x = obj->getPositionX(), y = obj->getPositionY();
            if (x < x1 || x > x2 || y < y1 || y > y2) continue;
            if (idFilter && obj->m_objectID != *idFilter) continue;
            if (matched < limit) {
                auto d = describe(obj);
                if (details) addDetails(d, obj, editor);
                list.push(d);
            }
            matched++;
        }
        return reply(matjson::makeObject({
            {"ok", true},
            {"total", matched},
            {"truncated", matched > limit},
            {"objects", list},
        }));
    }

    ai::Response toolModifyObjectsBatch(matjson::Value const& body, std::string const& batchId) {
        auto editor = LevelEditorLayer::get();
        if (!editor) return fail("not_in_editor", "Abre un nivel en el editor primero", 409);
        if (!body["changes"].isArray()) return fail("invalid_params", "'changes' debe ser una lista");
        auto const& changes = body["changes"].asArray().unwrap();
        if (changes.empty()) return fail("invalid_params", "'changes' esta vacia");
        if (changes.size() > static_cast<size_t>(kMaxBatch)) return fail("invalid_params", fmt::format("Maximo {} cambios por lote", kMaxBatch));

        auto byUid = objectsByUid(editor);

        // 1) Validar TODO antes de tocar nada
        struct Plan { GameObject* obj; std::string original; std::string modified; double x, y; KeyValueList extra; };
        std::vector<Plan> plans;
        std::unordered_set<int> seen;
        for (size_t i = 0; i < changes.size(); i++) {
            auto const& c = changes[i];
            auto uid = c["uid"].asInt();
            if (!uid) return fail("invalid_params", fmt::format("changes[{}] necesita 'uid'", i));
            int u = static_cast<int>(uid.unwrap());
            if (!seen.insert(u).second) return fail("invalid_params", fmt::format("uid {} repetido", u));
            auto it = byUid.find(u);
            if (it == byUid.end()) return fail("not_found", fmt::format("No existe ningun objeto con uid {}", u), 404);
            auto obj = it->second;

            double x = obj->getPositionX(), y = obj->getPositionY();
            if (c.contains("x")) x = c["x"].asDouble().unwrapOr(x);
            if (c.contains("y")) y = c["y"].asDouble().unwrapOr(y);
            x += c["dx"].asDouble().unwrapOr(0);
            y += c["dy"].asDouble().unwrapOr(0);
            if (x < -1000 || x > 1000000 || y < -3000 || y > 30000) {
                return fail("invalid_params", fmt::format("changes[{}]: posicion fuera de rango", i));
            }

            auto original = saveStringOf(obj, editor);
            auto kv = splitObjectString(original);
            setKey(kv, "2", numberToString(x));
            setKey(kv, "3", numberToString(y - kGroundOffsetY));
            if (c.contains("rotation")) {
                double r = c["rotation"].asDouble().unwrapOr(0);
                if (r < -360 || r > 360) return fail("invalid_params", fmt::format("changes[{}]: rotation fuera de rango", i));
                setKey(kv, "6", numberToString(r));
            }
            if (c.contains("scale")) {
                double s = c["scale"].asDouble().unwrapOr(1);
                if (s < 0.1 || s > 10) return fail("invalid_params", fmt::format("changes[{}]: scale fuera de rango", i));
                removeKey(kv, "32");
                setKey(kv, "128", numberToString(s));
                setKey(kv, "129", numberToString(s));
            }
            if (c.contains("flipX")) setKey(kv, "4", c["flipX"].asBool().unwrapOr(false) ? "1" : "0");
            if (c.contains("flipY")) setKey(kv, "5", c["flipY"].asBool().unwrapOr(false) ? "1" : "0");
            KeyValueList extra;
            bool groupsGiven = false;
            if (auto err = parseExtras(c, extra, groupsGiven)) return fail("invalid_params", fmt::format("changes[{}]: {}", i, *err));
            for (auto const& [k, v] : extra) {
                if (k == "57" && v.empty()) removeKey(kv, "57");   // groups: [] quita todos los grupos
                else setKey(kv, k, v);
            }
            plans.push_back({obj, original, joinObjectString(kv), x, y, extra});
        }

        // 2) Aplicar: quitar el original y crear la version modificada
        deselectAll(editor);
        std::string newObjects;
        HistoryEntry entry{batchId, "modify", {}, {}, {}};
        for (auto const& p : plans) {
            entry.removed.push_back(p.original);
            entry.removedRefs.emplace_back(p.obj);
            editor->removeObject(p.obj, true);
            newObjects += p.modified + ";";
        }
        auto created = editor->createObjectsFromString(newObjects, true, true);
        if (!created || created->count() != plans.size()) {
            // Algo ha ido mal: devolver el nivel a como estaba
            if (created) for (auto obj : CCArrayExt<GameObject*>(created)) editor->removeObject(obj, true);
            std::string restore;
            for (auto const& s : entry.removed) restore += s + ";";
            editor->createObjectsFromString(restore, true, true);
            return fail("modify_failed", "El juego no recreo todos los objetos; se ha restaurado el estado anterior", 500);
        }

        // 3) Verificar (y que los lotes anteriores apunten a los objetos nuevos)
        auto& history = historyFor(editor);
        auto list = matjson::Value::array();
        int misplaced = 0, propsWrong = 0;
        size_t i = 0;
        for (auto obj : CCArrayExt<GameObject*>(created)) {
            entry.created.emplace_back(obj);
            remapHistory(history, plans[i].obj, obj);
            bool ok = std::abs(obj->getPositionX() - plans[i].x) < 0.5 && std::abs(obj->getPositionY() - plans[i].y) < 0.5;
            if (!ok) misplaced++;
            std::vector<std::string> wrong, absent;
            verifyExtras(obj, editor, plans[i].extra, wrong, absent);
            if (!wrong.empty()) propsWrong++;
            auto d = describe(obj);
            d["oldUid"] = plans[i].obj->m_uniqueID;
            d["positionOk"] = ok;
            if (!plans[i].extra.empty()) {
                d["propsOk"] = wrong.empty();
                if (!wrong.empty()) d["propsWrong"] = stringList(wrong);
                if (!absent.empty()) d["propsNotSaved"] = stringList(absent);
            }
            list.push(d);
            i++;
        }
        pushHistory(editor, std::move(entry));
        log::info("[AI] Modificados {} objetos (lote {})", plans.size(), batchId);

        return reply(matjson::makeObject({
            {"ok", misplaced == 0 && propsWrong == 0},
            {"modified", static_cast<int>(plans.size())},
            {"misplaced", misplaced},
            {"propsWrong", propsWrong},
            {"note", "Los objetos modificados tienen uid nuevo (ver oldUid -> uid)"},
            {"objects", list},
        }));
    }

    ai::Response toolDeleteObjectsBatch(matjson::Value const& body, std::string const& batchId) {
        auto editor = LevelEditorLayer::get();
        if (!editor) return fail("not_in_editor", "Abre un nivel en el editor primero", 409);

        std::vector<GameObject*> targets;
        if (body.contains("uids")) {
            std::string error;
            auto uids = parseUidList(body["uids"], error, 2000);
            if (!uids) return fail("invalid_params", error);
            auto byUid = objectsByUid(editor);
            for (int u : *uids) {
                auto it = byUid.find(u);
                if (it == byUid.end()) return fail("not_found", fmt::format("No existe ningun objeto con uid {}", u), 404);
                targets.push_back(it->second);
            }
        } else if (body.contains("area")) {
            auto const& a = body["area"];
            auto x1 = a["x1"].asDouble(), x2 = a["x2"].asDouble(), y1 = a["y1"].asDouble(), y2 = a["y2"].asDouble();
            if (!x1 || !x2 || !y1 || !y2) return fail("invalid_params", "'area' necesita x1, x2, y1 e y2");
            for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
                float x = obj->getPositionX(), y = obj->getPositionY();
                if (x >= x1.unwrap() && x <= x2.unwrap() && y >= y1.unwrap() && y <= y2.unwrap()) targets.push_back(obj);
            }
        } else {
            return fail("invalid_params", "Indica 'uids' o 'area'");
        }

        if (targets.empty()) return reply(matjson::makeObject({{"ok", true}, {"deleted", 0}}));
        // Proteccion: los borrados grandes necesitan confirmacion explicita.
        // "Grande" = mas de 100 objetos o mas de la cuarta parte del nivel
        // (antes era solo >200 y la prueba borro un nivel entero de 21).
        bool confirmed = body["confirm"].asBool().unwrapOr(false);
        size_t levelCount = editor->m_objects->count();
        bool big = targets.size() > 100 || (levelCount >= 4 && targets.size() * 4 > levelCount);
        if (big && !confirmed) {
            return fail("confirmation_required",
                fmt::format("Esto borraria {} de los {} objetos del nivel. Pide permiso al usuario y repite con confirm=true.",
                    targets.size(), levelCount), 409);
        }
        if (targets.size() > 5000) return fail("invalid_params", "Maximo 5000 objetos por borrado");

        deselectAll(editor);
        HistoryEntry entry{batchId, "delete", {}, {}, {}};
        for (auto obj : targets) {
            entry.removed.push_back(saveStringOf(obj, editor));
            entry.removedRefs.emplace_back(obj);
            editor->removeObject(obj, true);
        }
        int stillThere = 0;
        for (auto obj : targets) if (editor->m_objects->containsObject(obj)) stillThere++;
        int deleted = static_cast<int>(targets.size()) - stillThere;
        pushHistory(editor, std::move(entry));
        log::info("[AI] Borrados {} objetos (lote {})", deleted, batchId);

        return reply(matjson::makeObject({
            {"ok", stillThere == 0},
            {"deleted", deleted},
            {"objectCountAfter", static_cast<int>(editor->m_objects->count())},
        }));
    }

    ai::Response toolUndoLastBatch() {
        auto editor = LevelEditorLayer::get();
        if (!editor) return fail("not_in_editor", "Abre un nivel en el editor primero", 409);
        auto& history = historyFor(editor);
        if (history.empty()) return fail("nothing_to_undo", "No hay ningun lote de la IA para deshacer en este nivel", 409);

        auto entry = std::move(history.back());
        history.pop_back();
        deselectAll(editor);

        // Quitar lo que creo el lote (si sigue en el nivel)
        int removed = 0, missing = 0;
        for (auto& obj : entry.created) {
            if (editor->m_objects->containsObject(obj.data())) {
                editor->removeObject(obj.data(), true);
                removed++;
            } else {
                missing++;  // ya lo habia quitado alguien (p. ej. Ctrl+Z manual)
            }
        }
        // Volver a crear lo que quito, y que los lotes anteriores apunten a
        // los objetos restaurados (son objetos nuevos con otro uid)
        int restored = 0;
        if (!entry.removed.empty()) {
            std::string restore;
            for (auto const& s : entry.removed) restore += s + ";";
            auto created = editor->createObjectsFromString(restore, true, true);
            restored = created ? static_cast<int>(created->count()) : 0;
            if (created && created->count() == entry.removed.size()) {
                size_t i = 0;
                for (auto obj : CCArrayExt<GameObject*>(created)) {
                    if (i < entry.removedRefs.size()) remapHistory(history, entry.removedRefs[i].data(), obj);
                    // En una modificacion, los lotes anteriores apuntaban a la
                    // version modificada (created[i]); ahora vuelve el original
                    if (entry.type == "modify" && i < entry.created.size()) {
                        remapHistory(history, entry.created[i].data(), obj);
                    }
                    i++;
                }
            }
        }
        log::info("[AI] Deshecho lote {} ({}): quitados {}, restaurados {}", entry.batchId, entry.type, removed, restored);

        return reply(matjson::makeObject({
            {"ok", restored == static_cast<int>(entry.removed.size())},
            {"undoneBatch", entry.batchId},
            {"type", entry.type},
            {"removed", removed},
            {"alreadyMissing", missing},
            {"restored", restored},
            {"remainingInHistory", static_cast<int>(history.size())},
            {"objectCountAfter", static_cast<int>(editor->m_objects->count())},
        }));
    }

    ai::Response toolAddObjectsBatch(matjson::Value const& body, std::string const& batchId) {
        std::vector<ObjectSpec> specs;
        if (auto err = parseObjects(body["objects"], specs)) return fail("invalid_params", *err);

        auto editor = LevelEditorLayer::get();
        if (!editor) return fail("not_in_editor", "Abre un nivel en el editor primero", 409);

        int before = static_cast<int>(editor->m_objects->count());
        auto created = editor->createObjectsFromString(toObjectString(specs), true, true);
        if (!created || created->count() == 0) {
            return fail("create_failed", "El juego no creo ningun objeto", 500);
        }

        // Un solo paso de deshacer para todo el lote (Ctrl+Z del editor)
        editor->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);

        // Y en el historial propio (undo_last_batch)
        HistoryEntry entry{batchId, "add", {}, {}, {}};
        for (auto obj : CCArrayExt<GameObject*>(created)) entry.created.emplace_back(obj);
        pushHistory(editor, std::move(entry));

        // Verificacion real: leer los objetos creados desde el juego y
        // comprobar que estan en el nivel y donde se pidieron
        auto list = matjson::Value::array();
        int inLevel = 0;
        int misplaced = 0;
        int propsWrong = 0;
        size_t index = 0;
        for (auto obj : CCArrayExt<GameObject*>(created)) {
            bool present = editor->m_objects->containsObject(obj);
            if (present) inLevel++;
            bool placed = true;
            if (index < specs.size()) {
                placed = std::abs(obj->getPositionX() - specs[index].x) < 0.5 &&
                         std::abs(obj->getPositionY() - specs[index].y) < 0.5;
            }
            if (!placed) misplaced++;
            auto d = matjson::makeObject({
                {"uid", obj->m_uniqueID},
                {"id", obj->m_objectID},
                {"x", obj->getPositionX()},
                {"y", obj->getPositionY()},
                {"inLevel", present},
                {"positionOk", placed},
            });
            if (index < specs.size() && !specs[index].extra.empty()) {
                std::vector<std::string> wrong, absent;
                verifyExtras(obj, editor, specs[index].extra, wrong, absent);
                if (!wrong.empty()) propsWrong++;
                d["propsOk"] = wrong.empty();
                if (!wrong.empty()) d["propsWrong"] = stringList(wrong);
                if (!absent.empty()) d["propsNotSaved"] = stringList(absent);
            }
            list.push(d);
            index++;
        }
        int after = static_cast<int>(editor->m_objects->count());

        log::info("[AI] Lote: pedidos {}, creados {}, en el nivel {} ({} -> {} objetos)",
            specs.size(), created->count(), inLevel, before, after);

        return reply(matjson::makeObject({
            {"ok", inLevel == static_cast<int>(specs.size()) && misplaced == 0 && propsWrong == 0},
            {"requested", static_cast<int>(specs.size())},
            {"propsWrong", propsWrong},
            {"created", static_cast<int>(created->count())},
            {"verifiedInLevel", inLevel},
            {"misplaced", misplaced},
            {"objectCountBefore", before},
            {"objectCountAfter", after},
            {"objects", list},
        }));
    }

    // ---------------- resumen del nivel ----------------
    // IDs comprobados con el catalogo del juego (portal_NN / boost_NN /
    // edit_e*Btn). Solo sirven para poner nombre en el resumen.
    char const* specialName(int id) {
        switch (id) {
            case 10: return "gravedad normal";   case 11: return "gravedad invertida";
            case 12: return "portal cubo";       case 13: return "portal nave";
            case 45: return "espejo";            case 46: return "espejo normal";
            case 47: return "portal bola";       case 99: return "tamano normal";
            case 101: return "tamano mini";      case 111: return "portal ufo";
            case 286: return "dual";             case 287: return "dual fin";
            case 660: return "portal wave";      case 745: return "portal robot";
            case 747: return "teletransporte";   case 1331: return "portal arana";
            case 1933: return "portal swing";
            case 200: return "velocidad 0.5x";   case 201: return "velocidad 1x";
            case 202: return "velocidad 2x";     case 203: return "velocidad 3x";
            case 1334: return "velocidad 4x";
            case 31: return "start pos";
            case 899: return "trigger color";    case 901: return "trigger move";
            case 1006: return "trigger pulse";   case 1007: return "trigger alpha";
            case 1049: return "trigger toggle";  case 1268: return "trigger spawn";
            case 1346: return "trigger rotate";  case 1616: return "trigger stop";
            default: return nullptr;
        }
    }

    ai::Response toolGetLevelSummary() {
        auto editor = LevelEditorLayer::get();
        if (!editor) return fail("not_in_editor", "Abre un nivel en el editor primero", 409);
        int count = static_cast<int>(editor->m_objects->count());
        float minX = 0, maxX = 0, minY = 0, maxY = 0;
        bool first = true;
        std::unordered_map<int, int> byId;
        std::set<int> groups;
        auto special = matjson::Value::array();
        struct Special { float x, y; int id; int uid; };
        std::vector<Special> specials;
        for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
            float x = obj->getPositionX(), y = obj->getPositionY();
            if (first) { minX = maxX = x; minY = maxY = y; first = false; }
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
            byId[obj->m_objectID]++;
            if (obj->m_groups) {
                for (int i = 0; i < obj->m_groupCount && i < 10; i++) {
                    int g = (*obj->m_groups)[i];
                    if (g > 0) groups.insert(g);
                }
            }
            if (specialName(obj->m_objectID)) specials.push_back({x, y, obj->m_objectID, obj->m_uniqueID});
        }
        std::sort(specials.begin(), specials.end(), [](auto const& a, auto const& b) { return a.x < b.x; });
        for (size_t i = 0; i < specials.size() && i < 300; i++) {
            special.push(matjson::makeObject({
                {"x", specials[i].x}, {"y", specials[i].y}, {"id", specials[i].id},
                {"uid", specials[i].uid}, {"name", specialName(specials[i].id)},
            }));
        }
        std::vector<std::pair<int, int>> ids(byId.begin(), byId.end());
        std::sort(ids.begin(), ids.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        auto topIds = matjson::Value::array();
        for (size_t i = 0; i < ids.size() && i < 40; i++) {
            topIds.push(matjson::makeObject({{"id", ids[i].first}, {"count", ids[i].second}}));
        }
        auto usedGroups = matjson::Value::array();
        int n = 0;
        for (int g : groups) { if (n++ >= 300) break; usedGroups.push(g); }
        int nextFree = 1;
        while (groups.count(nextFree)) nextFree++;

        std::string name = editor->m_level ? std::string(editor->m_level->m_levelName.c_str()) : "";
        return reply(matjson::makeObject({
            {"ok", true},
            {"levelName", name},
            {"objectCount", count},
            {"bounds", matjson::makeObject({{"minX", minX}, {"maxX", maxX}, {"minY", minY}, {"maxY", maxY}})},
            {"lengthBlocks", first ? 0 : static_cast<int>(maxX / 30)},
            {"objectsById", topIds},
            {"portalsAndTriggers", special},
            {"portalsAndTriggersTotal", static_cast<int>(specials.size())},
            {"usedGroups", usedGroups},
            {"nextFreeGroup", nextFree},
            {"historyBatches", static_cast<int>(historyFor(editor).size())},
        }));
    }

    // ---------------- guardar ----------------

    std::string safeFileName(std::string const& name) {
        std::string out;
        for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
        if (out.empty()) out = "nivel";
        return out.substr(0, 40);
    }

    ai::Response toolSaveLevel(matjson::Value const& body) {
        auto editor = LevelEditorLayer::get();
        if (!editor || !editor->m_level) return fail("not_in_editor", "Abre un nivel en el editor primero", 409);
        std::string name = editor->m_level->m_levelName.c_str();
        if (!body["confirm"].asBool().unwrapOr(false)) {
            return fail("confirmation_required",
                fmt::format("Guardar sobrescribe el nivel '{}' ({} objetos). Pide permiso al usuario y repite con confirm=true.",
                    name, editor->m_objects->count()), 409);
        }

        // Copia recuperable de la version guardada anterior
        std::string previous = editor->m_level->m_levelString.c_str();
        auto dir = Mod::get()->getSaveDir() / "ai-backups";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        char stamp[32];
        std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        auto backup = dir / fmt::format("{}_{}.txt", safeFileName(name), stamp);
        {
            std::ofstream file(backup, std::ios::binary | std::ios::trunc);
            file << previous;
            if (!file) return fail("backup_failed", "No se pudo escribir la copia de seguridad; no se ha guardado nada", 500);
        }

        // Lo mismo que el boton "Save" del menu de pausa del editor
        auto pause = EditorPauseLayer::create(editor);
        if (!pause) return fail("save_failed", "No se pudo preparar el guardado", 500);
        pause->saveLevel();

        std::string after = editor->m_level->m_levelString.c_str();
        log::info("[AI] Nivel '{}' guardado ({} -> {} bytes), copia en {}", name, previous.size(), after.size(), backup.string());
        return reply(matjson::makeObject({
            {"ok", !after.empty()},
            {"levelName", name},
            {"objectCount", static_cast<int>(editor->m_objects->count())},
            {"savedBytes", static_cast<int>(after.size())},
            {"changed", after != previous},
            {"backupFile", backup.string()},
        }));
    }

    // ---------------- despacho ----------------

    ai::Response handle(std::string const& tool, std::string const& body, std::string const& key) {
        if (auto cached = cachedResult(key)) {
            auto res = *cached;
            // Marcar que es una repeticion
            if (auto parsed = matjson::parse(res.body)) {
                auto v = parsed.unwrap();
                v["replayed"] = true;
                res.body = v.dump(matjson::NO_INDENTATION);
            }
            return res;
        }

        matjson::Value params = matjson::Value::object();
        if (!body.empty()) {
            auto parsed = matjson::parse(body);
            if (!parsed) return fail("invalid_json", "El cuerpo no es JSON valido");
            params = parsed.unwrap();
            if (!params.isObject()) return fail("invalid_json", "El cuerpo debe ser un objeto JSON");
        }

        ai::Response res;
        if (tool == "ping") {
            res = toolPing();
        } else if (tool == "get_editor_state") {
            res = onMainThread(toolGetEditorState);
        } else if (tool == "add_objects_batch") {
            res = onMainThread([params, key] { return toolAddObjectsBatch(params, key); });
        } else if (tool == "modify_objects_batch") {
            res = onMainThread([params, key] { return toolModifyObjectsBatch(params, key); });
        } else if (tool == "delete_objects_batch") {
            res = onMainThread([params, key] { return toolDeleteObjectsBatch(params, key); });
        } else if (tool == "undo_last_batch") {
            res = onMainThread(toolUndoLastBatch);
        } else if (tool == "get_objects_in_area") {
            res = onMainThread([params] { return toolGetObjectsInArea(params); });
        } else if (tool == "get_level_summary") {
            res = onMainThread(toolGetLevelSummary);
        } else if (tool == "save_level") {
            res = onMainThread([params] { return toolSaveLevel(params); });
        } else if (tool == "get_object_catalog") {
            res = onMainThread([params] { return toolGetObjectCatalog(params); });
        } else {
            return fail("unknown_tool", "Herramienta desconocida: " + tool, 404);
        }
        storeResult(key, res);
        return res;
    }
}

$on_mod(Loaded) {
    if (!Mod::get()->getSettingValue<bool>("ai-builder")) {
        log::info("[AI] AI Level Builder desactivado (opcion ai-builder)");
        return;
    }

    int port = static_cast<int>(Mod::get()->getSettingValue<int64_t>("ai-port"));
    auto token = randomToken();

    // Guardar puerto y token para el servidor MCP (solo lectura local)
    auto path = Mod::get()->getSaveDir() / "ai-bridge.json";
    {
        std::ofstream file(path, std::ios::trunc);
        file << matjson::makeObject({
            {"port", port},
            {"token", token},
            {"bridge", kBridgeVersion},
        }).dump();
    }

    std::string error;
    if (ai::startServer(port, token, handle, error)) {
        log::info("[AI] Servidor escuchando en 127.0.0.1:{} (token en {})", port, path.string());
    } else {
        log::error("[AI] No se pudo arrancar el servidor: {}", error);
    }
}
