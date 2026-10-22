// Practice Runs - mod de Geode para Geometry Dash
//
// 1) Musica normal en modo practica: desbloquea la opcion oficial
//    "Practice Music Sync" (normalmente requiere el Music Unlocker).
// 2) Tramos: desde el menu de pausa escribes "65-90". El juego avanza
//    rapido (con noclip) hasta el 65%, se congela, eliges la altura del
//    checkpoint con el raton y practicas hasta el 90%. Al llegar te pregunta
//    si repetir el tramo o jugar el nivel normal.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <cstdio>

using namespace geode::prelude;

// ------------------------------------------------------------
// Estado global del tramo
// ------------------------------------------------------------

enum class RunState {
    Idle,       // no hay tramo activo
    Skipping,   // avanzando rapido hasta el inicio
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

static void setTimeScale(float scale) {
    CCDirector::get()->getScheduler()->setTimeScale(scale);
}

static bool allowRunCheckpoints() {
    return Mod::get()->getSavedValue<bool>("allow-checkpoints", false);
}

// Se pueden poner checkpoints ahora?
static bool checkpointsBlocked() {
    if (g_run.placingOwn) return false;
    switch (g_run.state) {
        case RunState::Idle: return false;
        case RunState::Running: return !allowRunCheckpoints();
        default: return true;  // avance rapido, eligiendo altura, pregunta final
    }
}

static void pauseMusic() {
    FMODAudioEngine::get()->pauseAllMusic(true);
}

static void resetRun() {
    g_run.state = RunState::Idle;
    g_run.frozen = false;
    g_run.placingOwn = false;
    g_run.syncFrames = 0;
    setTimeScale(1.f);
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
    setTimeScale(1.f);
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
                return;
            }
            if (repeat) {
                g_run.state = RunState::Running;
                pl->resetLevel();  // vuelve al checkpoint del tramo
            } else {
                resetRun();
                pl->removeAllCheckpoints();
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

    g_run.state = RunState::Skipping;
    g_run.frozen = false;
    pl->resetLevelFromStart();

    auto speed = Mod::get()->getSettingValue<int64_t>("skip-speed");
    setTimeScale(static_cast<float>(speed));
    pauseMusic();  // sin musica durante el avance rapido

    Notification::create(fmt::format("Avanzando hasta el {:g}%...", g_run.start))->show();
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

        if (g_run.syncFrames > 0 && --g_run.syncFrames == 0 && g_run.state == RunState::Running) {
            this->syncMusic();
        }

        if (g_run.state == RunState::Skipping) {
            float percent = this->getCurrentPercent();
            // Frenar un poco antes para no pasarse del inicio
            if (percent >= g_run.start - 2.f) setTimeScale(1.f);
            if (percent >= g_run.start) beginPicking();
        } else if (g_run.state == RunState::Running) {
            if (this->getCurrentPercent() >= g_run.end) showEndPrompt();
        }
    }

    // Noclip solo durante el avance rapido
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_run.state == RunState::Skipping && object != m_anticheatSpike) {
            return;
        }
        PlayLayer::destroyPlayer(player, object);
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
        }
        PlayLayer::togglePracticeMode(practiceMode);
    }

    void onQuit() {
        resetRun();
        PlatformToolbox::showCursor();
        PlayLayer::onQuit();
    }
};
