// Practice Runs - mod de Geode para Geometry Dash
//
// 1) Musica normal en modo practica: desbloquea la opcion oficial
//    "Practice Music Sync" (normalmente requiere el Music Unlocker).
// 2) Tramos: desde el menu de pausa escribes "65-90". El mod te teletransporta
//    al 65% con un StartPos invisible que lleva el modo, la velocidad, la
//    gravedad, el tamano, el dual y el espejo correctos (mira los portales que
//    hay antes). El juego carga los triggers hasta ese punto, asi que los
//    objetos estan donde deben. Eliges la altura del checkpoint con el raton,
//    practicas hasta el 90% y eliges si repetir o jugar el nivel normal.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

using namespace geode::prelude;

// ------------------------------------------------------------
// Estado global del tramo
// ------------------------------------------------------------

enum class RunState {
    Idle,       // no hay tramo activo
    Picking,    // congelado, eligiendo altura con el raton
    Running,    // practicando el tramo
    EndPrompt   // llegado al final, esperando respuesta
};

struct RunData {
    RunState state = RunState::Idle;
    float start = 0.f;
    float end = 100.f;
    bool frozen = false;
    bool placingOwn = false;   // true mientras el mod pone su propio checkpoint
    int syncFrames = 0;        // frames pendientes para resincronizar la musica
};

static RunData g_run;
static Ref<StartPosObject> g_startPos = nullptr;  // StartPos invisible del tramo

static bool allowRunCheckpoints() {
    return Mod::get()->getSavedValue<bool>("allow-checkpoints", false);
}

// Se pueden poner checkpoints ahora?
static bool checkpointsBlocked() {
    if (g_run.placingOwn) return false;
    switch (g_run.state) {
        case RunState::Idle: return false;
        case RunState::Running: return !allowRunCheckpoints();
        default: return true;  // eligiendo altura o pregunta final
    }
}

static void pauseMusic() {
    FMODAudioEngine::get()->pauseAllMusic(true);
}

static void clearStartPos(PlayLayer* pl) {
    if (pl && g_startPos) pl->setStartPosObject(nullptr);
    g_startPos = nullptr;
}

static void resetRun() {
    g_run.state = RunState::Idle;
    g_run.frozen = false;
    g_run.placingOwn = false;
    g_run.syncFrames = 0;
    FMODAudioEngine::get()->resumeAllMusic();
}

static bool parseRange(std::string const& text, float& a, float& b) {
    if (std::sscanf(text.c_str(), "%f-%f", &a, &b) != 2) return false;
    return a >= 0.f && b <= 100.f && a < b;
}

// ------------------------------------------------------------
// 1) Musica normal en practica
// ------------------------------------------------------------

class $modify(PRStatsManager, GameStatsManager) {
    bool isItemUnlocked(UnlockType type, int id) {
        // GJItem 17 = Music Unlocker (Diamond Shop)
        if (type == UnlockType::GJItem && id == 17 &&
            Mod::get()->getSettingValue<bool>("practice-music")) {
            return true;
        }
        return GameStatsManager::isItemUnlocked(type, id);
    }
};

// ------------------------------------------------------------
// Estado del nivel en un punto: mira los portales anteriores
// ------------------------------------------------------------

struct LevelStateAt {
    int mode = 0;          // 0 cubo, 1 nave, 2 bola, 3 ufo, 4 wave, 5 robot, 6 arana, 7 swing
    Speed speed = Speed::Normal;
    float speedValue = 0.9f;
    bool flipped = false;  // gravedad invertida
    bool mini = false;
    bool dual = false;
    bool mirror = false;
    float timeWarp = 1.f;      // trigger Time Warp activo (1 = normal)
    float speedPortalX = 0.f;  // para el registro
    int speedPortalId = -1;
};

static float speedValueOf(Speed speed) {
    switch (speed) {
        case Speed::Slow:    return 0.7f;
        case Speed::Fast:    return 1.1f;
        case Speed::Faster:  return 1.3f;
        case Speed::Fastest: return 1.6f;
        default:             return 0.9f;
    }
}

static Speed speedEnumOf(float value) {
    if (value < 0.8f)  return Speed::Slow;
    if (value < 1.0f)  return Speed::Normal;
    if (value < 1.2f)  return Speed::Fast;
    if (value < 1.45f) return Speed::Faster;
    return Speed::Fastest;
}

// Para cada tipo de portal guarda el ultimo (el de mayor X) antes del punto
struct Latest {
    float x = -1e9f;
    int id = -1;
    void consider(float ox, int oid) {
        if (ox > x) { x = ox; id = oid; }
    }
};

static std::optional<float> detectPathY(PlayLayer* pl, float x);

// ------------------------------------------------------------
// Velocidad aprendida jugando: cada vez que el jugador cambia de velocidad
// de verdad (al cruzar un portal) se apunta la X y la velocidad, por nivel.
// Asi se sabe la velocidad real del camino, aunque haya portales trampa o
// de rutas secretas por los que no se pasa.
// ------------------------------------------------------------

struct SpeedTrack {
    std::map<int, float> points;  // X redondeada -> velocidad
    float maxX = 0.f;             // hasta donde se ha jugado de verdad
    bool dirty = false;
};

static std::map<std::string, SpeedTrack> g_tracks;
static bool g_applyingSpeed = false;  // true mientras el mod pone la velocidad

static std::string trackKey(PlayLayer* pl) {
    if (!pl || !pl->m_level) return "";
    return fmt::format("speeds-{}-{}", static_cast<int>(pl->m_level->m_levelType),
        pl->m_level->m_levelID.value());
}

static SpeedTrack& loadTrack(std::string const& key) {
    auto it = g_tracks.find(key);
    if (it != g_tracks.end()) return it->second;

    SpeedTrack track;
    // Formato: "maxX|x:v,x:v,..."
    auto saved = Mod::get()->getSavedValue<std::string>(key, "");
    auto bar = saved.find('|');
    if (bar != std::string::npos) {
        track.maxX = std::strtof(saved.substr(0, bar).c_str(), nullptr);
        std::istringstream rest(saved.substr(bar + 1));
        std::string item;
        while (std::getline(rest, item, ',')) {
            int x = 0;
            float v = 0.f;
            if (std::sscanf(item.c_str(), "%d:%f", &x, &v) == 2) track.points[x] = v;
        }
    }
    return g_tracks.emplace(key, std::move(track)).first->second;
}

static void saveTrack(std::string const& key) {
    auto it = g_tracks.find(key);
    if (it == g_tracks.end() || !it->second.dirty) return;
    std::string out = fmt::format("{}|", it->second.maxX);
    bool first = true;
    for (auto const& [x, v] : it->second.points) {
        if (!first) out += ",";
        out += fmt::format("{}:{}", x, v);
        first = false;
    }
    Mod::get()->setSavedValue<std::string>(key, out);
    it->second.dirty = false;
}

// Solo se aprende jugando normal: sin tramo activo y sin StartPos
static bool canRecord(PlayLayer* pl) {
    return pl && g_run.state == RunState::Idle && !pl->m_startPosObject;
}

// Velocidad aprendida en una X, si ya se ha jugado hasta ahi
static std::optional<float> learnedSpeedAt(PlayLayer* pl, float x) {
    auto key = trackKey(pl);
    if (key.empty()) return std::nullopt;
    auto& track = loadTrack(key);
    if (track.points.empty() || track.maxX < x) return std::nullopt;
    auto it = track.points.upper_bound(static_cast<int>(std::round(x)));
    if (it == track.points.begin()) return std::nullopt;
    --it;
    return it->second;
}

static LevelStateAt computeLevelStateAt(PlayLayer* pl, float targetX);

// Igual que computeLevelStateAt pero recuerda el ultimo resultado, porque al
// reaparecer se pide muchas veces para la misma X y recorrer el nivel es lento
static LevelStateAt levelStateAt(PlayLayer* pl, float targetX) {
    static PlayLayer* cachedLayer = nullptr;
    static int cachedX = INT_MIN;
    static LevelStateAt cached;
    int x = static_cast<int>(std::round(targetX));
    if (pl == cachedLayer && x == cachedX) return cached;
    cached = computeLevelStateAt(pl, targetX);
    cachedLayer = pl;
    cachedX = x;
    return cached;
}

static LevelStateAt computeLevelStateAt(PlayLayer* pl, float targetX) {
    LevelStateAt st;
    if (auto ls = pl->m_levelSettings) {
        st.mode = ls->m_startMode;
        st.speed = ls->m_startSpeed;
        st.flipped = ls->m_isFlipped;
        st.mini = ls->m_startMini;
        st.dual = ls->m_startDual;
        st.mirror = ls->m_mirrorMode;
    }

    Latest mode, speed, gravity, size, dual, mirror, warp;
    float warpValue = 1.f;
    std::vector<GameObject*> speedPortals;
    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        float ox = obj->getPositionX();
        if (ox > targetX) continue;
        int id = obj->m_objectID;
        switch (id) {
            // Trigger Time Warp (solo los que se activan al pasar por su X)
            case 1935: {
                auto eff = static_cast<EffectGameObject*>(obj);
                if (eff->m_isTouchTriggered || eff->m_isSpawnTriggered) break;
                if (ox > warp.x) {
                    warp.consider(ox, id);
                    warpValue = eff->m_timeWarpTimeMod;
                }
                break;
            }
            // Portales de modo
            case 12: case 13: case 47: case 111: case 660: case 745: case 1331: case 1933:
                mode.consider(ox, id); break;
            // Portales de velocidad: se guardan todos y se filtran despues
            case 200: case 201: case 202: case 203: case 1334:
                speedPortals.push_back(obj); break;
            // Gravedad: azul normal, amarillo invertida
            case 10: case 11:
                gravity.consider(ox, id); break;
            // Tamano: normal / mini
            case 99: case 101:
                size.consider(ox, id); break;
            // Dual on / off
            case 286: case 287:
                dual.consider(ox, id); break;
            // Espejo on / off
            case 45: case 46:
                mirror.consider(ox, id); break;
            default: break;
        }
    }

    switch (mode.id) {
        case 12:   st.mode = 0; break;
        case 13:   st.mode = 1; break;
        case 47:   st.mode = 2; break;
        case 111:  st.mode = 3; break;
        case 660:  st.mode = 4; break;
        case 745:  st.mode = 5; break;
        case 1331: st.mode = 6; break;
        case 1933: st.mode = 7; break;
        default: break;
    }
    // Portales de velocidad: ignorar los que estan lejos del camino (rutas
    // secretas, decoracion...). El camino se mide con los objetos jugables
    // cerca de cada portal.
    for (auto obj : speedPortals) {
        float ox = obj->getPositionX();
        if (ox <= speed.x) continue;
        auto pathY = detectPathY(pl, ox);
        if (pathY && std::abs(obj->getPositionY() - *pathY) > 300.f) {
            log::info("Portal de velocidad id {} en X={} Y={} ignorado (camino a Y={})",
                obj->m_objectID, ox, obj->getPositionY(), *pathY);
            continue;
        }
        speed.consider(ox, obj->m_objectID);
    }

    switch (speed.id) {
        case 200:  st.speed = Speed::Slow; break;
        case 201:  st.speed = Speed::Normal; break;
        case 202:  st.speed = Speed::Fast; break;
        case 203:  st.speed = Speed::Faster; break;
        case 1334: st.speed = Speed::Fastest; break;
        default: break;
    }
    if (gravity.id != -1) st.flipped = (gravity.id == 11);
    if (size.id != -1)    st.mini = (size.id == 101);
    if (dual.id != -1)    st.dual = (dual.id == 286);
    if (mirror.id != -1)  st.mirror = (mirror.id == 45);

    st.speedValue = speedValueOf(st.speed);
    st.timeWarp = warpValue > 0.f ? warpValue : 1.f;
    st.speedPortalX = speed.x;
    st.speedPortalId = speed.id;

    // Si ya se ha jugado hasta aqui, manda la velocidad real aprendida
    if (auto learned = learnedSpeedAt(pl, targetX)) {
        st.speedValue = *learned;
        st.speed = speedEnumOf(*learned);
        st.speedPortalId = 0;  // 0 = velocidad aprendida jugando
    }
    return st;
}

// Coloca el StartPos. Hay que poner las dos posiciones: la visible y la
// "de inicio" interna, que es la que usa el juego para la camara de los
// modos voladores (nave, ufo, wave, swing...).
static void placeStartPos(StartPosObject* sp, CCPoint pos) {
    sp->setPosition(pos);
    sp->setStartPos(pos);
}

// Crea el StartPos invisible en el punto del tramo
static StartPosObject* createRunStartPos(PlayLayer* pl, float targetX) {
    auto st = levelStateAt(pl, targetX);

    auto settings = LevelSettingsObject::create();
    if (auto ls = pl->m_levelSettings) {
        // Copiar los ajustes generales del nivel para que la fisica sea la misma
        settings->m_level = ls->m_level;
        settings->m_songOffset = ls->m_songOffset;
        settings->m_fadeIn = ls->m_fadeIn;
        settings->m_fadeOut = ls->m_fadeOut;
        settings->m_backgroundIndex = ls->m_backgroundIndex;
        settings->m_groundIndex = ls->m_groundIndex;
        settings->m_middleGroundIndex = ls->m_middleGroundIndex;
        settings->m_groundLineIndex = ls->m_groundLineIndex;
        settings->m_fontIndex = ls->m_fontIndex;
        settings->m_twoPlayerMode = ls->m_twoPlayerMode;
        settings->m_platformerMode = ls->m_platformerMode;
        settings->m_allowMultiRotation = ls->m_allowMultiRotation;
        settings->m_enablePlayerSqueeze = ls->m_enablePlayerSqueeze;
        settings->m_fixGravityBug = ls->m_fixGravityBug;
        settings->m_fixNegativeScale = ls->m_fixNegativeScale;
        settings->m_fixRobotJump = ls->m_fixRobotJump;
        settings->m_dynamicLevelHeight = ls->m_dynamicLevelHeight;
        settings->m_sortGroups = ls->m_sortGroups;
        settings->m_fixRadiusCollision = ls->m_fixRadiusCollision;
        settings->m_enable22Changes = ls->m_enable22Changes;
        settings->m_allowStaticRotate = ls->m_allowStaticRotate;
        settings->m_reverseSync = ls->m_reverseSync;
        settings->m_decreaseBoostSlide = ls->m_decreaseBoostSlide;
        settings->m_enableImpulseFix = ls->m_enableImpulseFix;
    }
    settings->m_startMode = st.mode;
    settings->m_startSpeed = st.speed;
    settings->m_isFlipped = st.flipped;
    settings->m_startMini = st.mini;
    settings->m_startDual = st.dual;
    settings->m_mirrorMode = st.mirror;
    settings->m_startsWithStartPos = true;

    auto sp = StartPosObject::create();
    sp->setSettings(settings);
    // 105 = altura normal de salida; la altura buena la eliges luego con el raton
    placeStartPos(sp, {targetX, 105.f});

    log::info("Tramo en X={}: modo {}, velocidad {} (portal id {} en X={}), time warp {}, gravedad {}, mini {}, dual {}, espejo {}",
        targetX, st.mode, st.speedValue, st.speedPortalId, st.speedPortalX, st.timeWarp,
        st.flipped, st.mini, st.dual, st.mirror);
    return sp;
}

// Detecta a que altura va el camino del nivel cerca de una X: mira los
// objetos jugables (bloques, pinchos, rampas, pads, orbes) que hay un poco
// antes y por delante, y devuelve la mediana de sus alturas.
// Se llama despues de cargar el nivel hasta ese punto, asi que los objetos
// movidos por triggers ya estan en su sitio.
static bool isGameplayObject(GameObject* obj) {
    switch (obj->m_objectType) {
        case GameObjectType::Solid:
        case GameObjectType::Hazard:
        case GameObjectType::Slope:
        case GameObjectType::Breakable:
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
            return true;
        default:
            return false;
    }
}

static std::optional<float> detectPathY(PlayLayer* pl, float x) {
    std::vector<float> ys;
    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        float ox = obj->getPositionX();
        if (ox < x - 60.f || ox > x + 300.f) continue;
        if (!isGameplayObject(obj)) continue;
        ys.push_back(obj->getPositionY());
    }
    if (ys.empty()) return std::nullopt;
    std::sort(ys.begin(), ys.end());
    return ys[ys.size() / 2];
}

// Fija la velocidad del jugador (portal) y del juego (Time Warp) a la que
// toca en su X
static void fixPlayerSpeed(PlayLayer* pl, char const* when) {
    float x = pl->m_player1->getPositionX();
    auto st = levelStateAt(pl, x);
    float before = pl->m_player1->m_playerSpeed;
    float warpBefore = pl->m_gameState.m_timeWarp;

    g_applyingSpeed = true;
    pl->m_player1->updateTimeMod(st.speedValue, true);
    if (pl->m_player2 && pl->m_player2->isVisible()) {
        pl->m_player2->updateTimeMod(st.speedValue, true);
    }
    g_applyingSpeed = false;
    if (std::abs(pl->m_gameState.m_timeWarp - st.timeWarp) > 0.001f) {
        pl->updateTimeWarp(st.timeWarp);
    }

    log::info("[{}] X={} velocidad {} -> {} (portal id {} en X={}), time warp {} -> {}",
        when, x, before, st.speedValue, st.speedPortalId, st.speedPortalX, warpBefore, st.timeWarp);
}

// ------------------------------------------------------------
// Capa para elegir la altura con el raton
// ------------------------------------------------------------

class HeightPicker : public CCLayer {
protected:
    CCLayerColor* m_line = nullptr;

    bool init() override {
        if (!CCLayer::init()) return false;

        auto win = CCDirector::get()->getWinSize();

        auto dim = CCLayerColor::create({0, 0, 0, 70});
        this->addChild(dim);

        m_line = CCLayerColor::create({0, 255, 120, 220}, win.width, 2.f);
        m_line->setPosition({0.f, win.height / 2.f});
        this->addChild(m_line);

        auto label = CCLabelBMFont::create("Haz clic para elegir la altura del checkpoint", "bigFont.fnt");
        label->setScale(0.45f);
        label->setPosition({win.width / 2.f, win.height - 30.f});
        this->addChild(label);

        this->setTouchEnabled(true);
        this->scheduleUpdate();
        return true;
    }

    void registerWithTouchDispatcher() override {
        CCTouchDispatcher::get()->addTargetedDelegate(this, -1000, true);
    }

    void update(float) override {
        // La linea sigue al raton
        m_line->setPositionY(getMousePos().y);
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        this->pick(touch->getLocation());
        return true;
    }

    void pick(CCPoint screenPos) {
        auto pl = PlayLayer::get();
        if (!pl) {
            resetRun();
            this->removeFromParent();
            return;
        }

        // Pasar de coordenadas de pantalla a coordenadas del nivel
        auto local = pl->m_objectLayer->convertToNodeSpace(screenPos);

        auto player = pl->m_player1;
        player->m_position = CCPoint(player->m_position.x, local.y);
        player->setPosition(player->m_position);
        player->m_yVelocity = 0.0;

        fixPlayerSpeed(pl, "elegir altura");

        g_run.placingOwn = true;
        pl->markCheckpoint();
        g_run.placingOwn = false;

        g_run.frozen = false;
        g_run.state = RunState::Running;
        PlatformToolbox::hideCursor();

        this->removeFromParent();

        // Reaparecer en el checkpoint para empezar el tramo limpio
        pl->resetLevel();
    }

public:
    static HeightPicker* create() {
        auto ret = new HeightPicker();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ------------------------------------------------------------
// Fases del tramo
// ------------------------------------------------------------

static void beginPicking() {
    g_run.state = RunState::Picking;
    g_run.frozen = true;
    pauseMusic();
    PlatformToolbox::showCursor();

    if (auto picker = HeightPicker::create()) {
        CCScene::get()->addChild(picker, 1000);
    }
}

static void showEndPrompt() {
    g_run.state = RunState::EndPrompt;
    g_run.frozen = true;
    pauseMusic();
    PlatformToolbox::showCursor();

    createQuickPopup(
        "Tramo completado",
        fmt::format("Has llegado al <cg>{:g}%</c>. Que quieres hacer?", g_run.end),
        "Jugar normal", "Repetir",
        [](auto, bool repeat) {
            auto pl = PlayLayer::get();
            g_run.frozen = false;
            PlatformToolbox::hideCursor();
            if (!pl) {
                resetRun();
                g_startPos = nullptr;
                return;
            }
            if (repeat) {
                g_run.state = RunState::Running;
                pl->resetLevel();  // vuelve al checkpoint del tramo
            } else {
                resetRun();
                pl->removeAllCheckpoints();
                clearStartPos(pl);
                pl->togglePracticeMode(false);
                pl->resetLevelFromStart();
            }
        }
    );
}

static void startRun() {
    auto pl = PlayLayer::get();
    if (!pl) return;

    if (!pl->m_isPracticeMode) {
        pl->togglePracticeMode(true);
    }
    pl->removeAllCheckpoints();

    float targetX = pl->m_levelLength * g_run.start / 100.f;
    g_startPos = createRunStartPos(pl, targetX);
    pl->setStartPosObject(g_startPos);

    // 1a carga: el juego carga el nivel desde el StartPos (triggers incluidos)
    g_run.state = RunState::Picking;
    g_run.frozen = false;
    pl->resetLevelFromStart();

    Loader::get()->queueInMainThread([targetX] {
        auto pl = PlayLayer::get();
        if (!pl || g_run.state != RunState::Picking || !g_startPos) return;

        // Con los objetos ya en su sitio, buscar a que altura va el camino
        auto pathY = detectPathY(pl, targetX);
        if (pathY && std::abs(*pathY - g_startPos->getPositionY()) > 30.f) {
            log::info("Camino detectado a Y={}, recolocando el StartPos", *pathY);
            placeStartPos(g_startPos, {targetX, *pathY});
            pl->setStartPosObject(g_startPos);
            pl->resetLevelFromStart();  // 2a carga, ya a la altura buena
        }

        // Congelar cuando el reinicio haya terminado
        Loader::get()->queueInMainThread([] {
            if (PlayLayer::get() && g_run.state == RunState::Picking) beginPicking();
        });
    });
}

// ------------------------------------------------------------
// Ventana para escribir el tramo
// ------------------------------------------------------------

class RunPopup : public geode::Popup {
protected:
    TextInput* m_input = nullptr;
    PauseLayer* m_pauseLayer = nullptr;

    bool init(PauseLayer* pauseLayer) {
        if (!Popup::init(280.f, 180.f)) return false;
        m_pauseLayer = pauseLayer;

        this->setTitle("Practicar un tramo");

        auto info = CCLabelBMFont::create("Escribe inicio-final (ej: 65-90)", "chatFont.fnt");
        info->setScale(0.8f);
        m_mainLayer->addChildAtPosition(info, Anchor::Center, {0.f, 28.f});

        m_input = TextInput::create(160.f, "65-90");
        m_input->setFilter("0123456789-.");
        m_input->setMaxCharCount(11);
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, {0.f, 4.f});

        // Opcion: permitir checkpoints dentro del tramo
        auto toggleMenu = CCMenu::create();
        auto toggler = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(RunPopup::onToggleCheckpoints), 0.6f
        );
        toggler->toggle(allowRunCheckpoints());
        toggler->setPosition({-80.f, 0.f});
        toggleMenu->addChild(toggler);
        auto toggleLabel = CCLabelBMFont::create("Permitir checkpoints en el tramo", "bigFont.fnt");
        toggleLabel->setScale(0.3f);
        toggleLabel->setAnchorPoint({0.f, 0.5f});
        toggleLabel->setPosition({-64.f, 0.f});
        toggleMenu->addChild(toggleLabel);
        m_mainLayer->addChildAtPosition(toggleMenu, Anchor::Center, {0.f, -26.f});

        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Empezar"), this, menu_selector(RunPopup::onStart)
        );
        auto menu = CCMenu::create();
        menu->addChild(btn);
        m_mainLayer->addChildAtPosition(menu, Anchor::Bottom, {0.f, 24.f});

        return true;
    }

    void onToggleCheckpoints(CCObject* sender) {
        // El toggler cambia de estado despues del callback
        auto toggler = static_cast<CCMenuItemToggler*>(sender);
        Mod::get()->setSavedValue<bool>("allow-checkpoints", !toggler->isToggled());
    }

    void onStart(CCObject*) {
        float a = 0.f, b = 0.f;
        if (!parseRange(m_input->getString(), a, b)) {
            Notification::create("Formato: inicio-final, ej. 65-90", NotificationIcon::Error)->show();
            return;
        }
        g_run.start = a;
        g_run.end = b;

        auto pauseLayer = m_pauseLayer;
        this->onClose(nullptr);
        if (pauseLayer) pauseLayer->onResume(nullptr);

        Loader::get()->queueInMainThread([] { startRun(); });
    }

public:
    static RunPopup* create(PauseLayer* pauseLayer) {
        auto ret = new RunPopup();
        if (ret->init(pauseLayer)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ------------------------------------------------------------
// Boton en el menu de pausa
// ------------------------------------------------------------

class $modify(PRPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = ButtonSprite::create("Tramo", "goldFont.fnt", "GJ_button_04.png", 0.8f);
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PRPauseLayer::onRunButton));
        btn->setID("run-button"_spr);

        if (auto menu = this->getChildByID("left-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto newMenu = CCMenu::create();
            newMenu->addChild(btn);
            newMenu->setPosition({50.f, 50.f});
            this->addChild(newMenu);
        }
    }

    void onRunButton(CCObject*) {
        if (auto popup = RunPopup::create(this)) popup->show();
    }
};

// ------------------------------------------------------------
// Congelar el juego mientras eliges altura o respondes
// ------------------------------------------------------------

class $modify(PRBaseGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        if (g_run.frozen && pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            return;
        }
        GJBaseGameLayer::update(dt);
    }
};

// ------------------------------------------------------------
// Hooks del nivel
// ------------------------------------------------------------

// Bloquear checkpoints automaticos durante el tramo
class $modify(PRPlayerObject, PlayerObject) {
    void tryPlaceCheckpoint() {
        if (checkpointsBlocked()) return;
        PlayerObject::tryPlaceCheckpoint();
    }

    // Aprender la velocidad real: cada cambio de velocidad del jugador 1
    // jugando normal se apunta con su X
    void updateTimeMod(float speed, bool noEffects) {
        PlayerObject::updateTimeMod(speed, noEffects);
        if (g_applyingSpeed) return;
        auto pl = PlayLayer::get();
        if (!pl || this != pl->m_player1 || !canRecord(pl)) return;
        auto key = trackKey(pl);
        if (key.empty()) return;
        auto& track = loadTrack(key);
        int x = static_cast<int>(std::round(this->getPositionX()));
        auto it = track.points.find(x);
        if (it == track.points.end() || std::abs(it->second - speed) > 0.001f) {
            track.points[x] = speed;
            track.dirty = true;
        }
    }
};

class $modify(PRPlayLayer, PlayLayer) {
    // Todos los checkpoints (automaticos y manuales) pasan por aqui
    void storeCheckpoint(CheckpointObject* checkpoint) {
        if (checkpointsBlocked()) return;
        PlayLayer::storeCheckpoint(checkpoint);
    }

    // Al reaparecer en un checkpoint durante el tramo, resincronizar musica
    void loadFromCheckpoint(CheckpointObject* object) {
        PlayLayer::loadFromCheckpoint(object);
        if (g_run.state == RunState::Running) {
            fixPlayerSpeed(this, "reaparecer");
            g_run.syncFrames = 2;  // esperar a que el juego termine el reinicio
        }
    }

    void syncMusic() {
        auto pos = m_player1->getPosition();
        float seconds = this->timeForPos(pos, 0, 0, true, 0);
        if (m_levelSettings) seconds += m_levelSettings->m_songOffset;
        if (seconds < 0.f) seconds = 0.f;

        auto engine = FMODAudioEngine::get();
        engine->resumeAllMusic();
        engine->setMusicTimeMS(static_cast<unsigned int>(seconds * 1000.f), true, 0);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        // Apuntar hasta donde se ha jugado de verdad (para fiarse de lo aprendido)
        if (canRecord(this) && m_player1 && !m_player1->m_isDead) {
            auto key = trackKey(this);
            if (!key.empty()) {
                auto& track = loadTrack(key);
                float x = m_player1->getPositionX();
                // Velocidad con la que empieza el nivel
                if (track.points.empty()) {
                    track.points[static_cast<int>(std::round(x))] = m_player1->m_playerSpeed;
                    track.dirty = true;
                }
                if (x > track.maxX) {
                    track.maxX = x;
                    track.dirty = true;
                }
            }
        }

        if (g_run.syncFrames > 0 && --g_run.syncFrames == 0 && g_run.state == RunState::Running) {
            this->syncMusic();
        }

        if (g_run.state == RunState::Running && this->getCurrentPercent() >= g_run.end) {
            showEndPrompt();
        }
    }

    // No dejar abrir la pausa mientras eliges altura o respondes
    void pauseGame(bool unfocused) {
        if (g_run.state == RunState::Picking || g_run.state == RunState::EndPrompt) {
            return;
        }
        PlayLayer::pauseGame(unfocused);
    }

    // Si quitas el modo practica a mano, se cancela el tramo
    void togglePracticeMode(bool practiceMode) {
        if (!practiceMode && g_run.state != RunState::Idle) {
            resetRun();
            clearStartPos(this);
        }
        PlayLayer::togglePracticeMode(practiceMode);
    }

    void levelComplete() {
        saveTrack(trackKey(this));
        PlayLayer::levelComplete();
    }

    void onQuit() {
        saveTrack(trackKey(this));
        resetRun();
        clearStartPos(this);
        PlatformToolbox::showCursor();
        PlayLayer::onQuit();
    }
};
