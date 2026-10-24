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
#include <unordered_map>

using namespace geode::prelude;

namespace {
    constexpr int kMaxBatch = 1000;
    constexpr char const* kBridgeVersion = "0.2.1";

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

    ai::Response toolAddObjectsBatch(matjson::Value const& body) {
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
            res = onMainThread([params] { return toolAddObjectsBatch(params); });
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
