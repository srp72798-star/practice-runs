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
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;

namespace {
    constexpr int kMaxBatch = 1000;
    constexpr char const* kBridgeVersion = "0.3.1";

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

    struct ObjectSpec {
        int id = 0;
        double x = 0, y = 0;
        double rotation = 0, scale = 1;
        bool flipX = false, flipY = false;
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

        auto list = matjson::Value::array();
        int matched = 0;
        for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
            float x = obj->getPositionX(), y = obj->getPositionY();
            if (x < x1 || x > x2 || y < y1 || y > y2) continue;
            if (idFilter && obj->m_objectID != *idFilter) continue;
            if (matched < limit) list.push(describe(obj));
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
        struct Plan { GameObject* obj; std::string original; std::string modified; double x, y; };
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
            plans.push_back({obj, original, joinObjectString(kv), x, y});
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
        int misplaced = 0;
        size_t i = 0;
        for (auto obj : CCArrayExt<GameObject*>(created)) {
            entry.created.emplace_back(obj);
            remapHistory(history, plans[i].obj, obj);
            bool ok = std::abs(obj->getPositionX() - plans[i].x) < 0.5 && std::abs(obj->getPositionY() - plans[i].y) < 0.5;
            if (!ok) misplaced++;
            auto d = describe(obj);
            d["oldUid"] = plans[i].obj->m_uniqueID;
            d["positionOk"] = ok;
            list.push(d);
            i++;
        }
        pushHistory(editor, std::move(entry));
        log::info("[AI] Modificados {} objetos (lote {})", plans.size(), batchId);

        return reply(matjson::makeObject({
            {"ok", misplaced == 0},
            {"modified", static_cast<int>(plans.size())},
            {"misplaced", misplaced},
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
            list.push(matjson::makeObject({
                {"uid", obj->m_uniqueID},
                {"id", obj->m_objectID},
                {"x", obj->getPositionX()},
                {"y", obj->getPositionY()},
                {"inLevel", present},
                {"positionOk", placed},
            }));
            index++;
        }
        int after = static_cast<int>(editor->m_objects->count());

        log::info("[AI] Lote: pedidos {}, creados {}, en el nivel {} ({} -> {} objetos)",
            specs.size(), created->count(), inLevel, before, after);

        return reply(matjson::makeObject({
            {"ok", inLevel == static_cast<int>(specs.size()) && misplaced == 0},
            {"requested", static_cast<int>(specs.size())},
            {"created", static_cast<int>(created->count())},
            {"verifiedInLevel", inLevel},
            {"misplaced", misplaced},
            {"objectCountBefore", before},
            {"objectCountAfter", after},
            {"objects", list},
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
