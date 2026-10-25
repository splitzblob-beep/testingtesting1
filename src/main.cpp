// Predictive Input Buffer
//
// When you click, an invisible copy of your player (the "ghost") is simulated a
// short way into the future. If clicking now would kill you but clicking a few
// ticks later survives, your click is held back and delivered at that later tick.
//
// 1 tick = 1/240 s = a quarter of a frame at 60 fps.
//
// Controls: F6 in a level cycles the strength (OFF -> 1f -> 2f -> 4f -> 8f).
//
// Not buffered: ship mode, platformer levels, 2-player levels.
// Runs made with this mod are not legit for lists or leaderboards.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace pib {

    // ------------------------------------------------------------------
    // Strengths (F6 cycles through these)
    // ------------------------------------------------------------------

    struct Strength {
        char const* label;
        int maxTicks;      // biggest delay allowed in every mode except wave
        int maxWaveTicks;  // wave dies quickly, so it gets a smaller window
    };

    constexpr std::array<Strength, 5> STRENGTHS { {
        { "OFF", 0, 0 },
        { "1f", 4, 4 },
        { "2f", 8, 6 },
        { "4f", 16, 8 },
        { "8f", 32, 12 },
    } };

    // dt that PlayerObject::update receives for one 240 Hz physics tick.
    // The real value is recorded every tick; this is only the fallback.
    constexpr float FALLBACK_TICK_DT = 0.25f;

    // ------------------------------------------------------------------
    // Which objects the ghost is allowed to touch
    // ------------------------------------------------------------------

    constexpr uint64_t bit(GameObjectType type) {
        return 1ull << static_cast<unsigned>(type);
    }

    // Always collided with: these decide whether you live or die.
    constexpr uint64_t SOLID_MASK = bit(GameObjectType::Solid) | bit(GameObjectType::Hazard) |
        bit(GameObjectType::AnimatedHazard) | bit(GameObjectType::Slope);

    // Collided with only when orb prediction is on.
    constexpr uint64_t ORB_MASK = bit(GameObjectType::YellowJumpPad) | bit(GameObjectType::PinkJumpPad) |
        bit(GameObjectType::RedJumpPad) | bit(GameObjectType::GravityPad) | bit(GameObjectType::SpiderPad) |
        bit(GameObjectType::YellowJumpRing) | bit(GameObjectType::PinkJumpRing) | bit(GameObjectType::RedJumpRing) |
        bit(GameObjectType::GravityRing) | bit(GameObjectType::GreenRing) | bit(GameObjectType::DropRing) |
        bit(GameObjectType::CustomRing) | bit(GameObjectType::DashRing) | bit(GameObjectType::GravityDashRing) |
        bit(GameObjectType::SpiderOrb) | bit(GameObjectType::InverseGravityPortal) |
        bit(GameObjectType::NormalGravityPortal) | bit(GameObjectType::GravityTogglePortal);

    // Change too much to simulate. Reaching one ends the prediction and counts as "safe".
    constexpr uint64_t STOP_MASK = bit(GameObjectType::ShipPortal) | bit(GameObjectType::CubePortal) |
        bit(GameObjectType::BallPortal) | bit(GameObjectType::UfoPortal) | bit(GameObjectType::WavePortal) |
        bit(GameObjectType::RobotPortal) | bit(GameObjectType::SpiderPortal) | bit(GameObjectType::SwingPortal) |
        bit(GameObjectType::DualPortal) | bit(GameObjectType::SoloPortal) | bit(GameObjectType::RegularSizePortal) |
        bit(GameObjectType::MiniSizePortal) | bit(GameObjectType::InverseMirrorPortal) |
        bit(GameObjectType::NormalMirrorPortal) | bit(GameObjectType::TeleportPortal) |
        bit(GameObjectType::TeleportOrb);

    uint64_t typeBit(GameObject* object) {
        auto type = static_cast<unsigned>(object->m_objectType);
        return type < 64 ? (1ull << type) : 0;
    }

    // Speed portals are applied to the ghost by hand (they're not in any mask above).
    float speedPortalSpeed(int objectID) {
        switch (objectID) {
            case 200: return 0.7f;   // slow
            case 201: return 0.9f;   // normal
            case 202: return 1.1f;   // fast
            case 203: return 1.3f;   // faster
            case 1334: return 1.6f;  // fastest
            default: return 0.f;
        }
    }

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------

    struct Snapshot {
        GameObject* object;
        bool isActivated;
        EffectGameObject* effect;
        bool activated;
        bool byP1;
        bool byP2;
    };

    struct Pending {
        int ticksLeft;
        bool down;
    };

    struct State {
        // settings
        int strength = 2;
        bool orbPrediction = true;
        bool showLabel = true;
        double budgetMs = 4.0;

        // Ghosts and label are owned by the PlayLayer; these are just for quick access.
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        CCLabelBMFont* label = nullptr;

        // simulation
        bool simulating = false;
        bool ghostDied = false;
        bool ghostHitStop = false;
        std::unordered_set<GameObject*> touched;
        std::vector<Snapshot> snapshots;

        // input pipeline
        bool injecting = false;
        bool physicalDown = false;  // what your finger is doing
        bool appliedDown = false;   // what the game has been told
        bool isPlayer1 = true;
        std::deque<Pending> queue;
        int shift = 0;              // delay given to every input while the queue is busy
        float tickDt = FALLBACK_TICK_DT;

        // how long you usually hold a tap (median of the last few)
        int tick = 0;
        int pressedAt = -1;
        std::array<int, 8> holds {};
        int holdCount = 0;
        int holdNext = 0;
    };

    State g;

    // ------------------------------------------------------------------
    // Small helpers
    // ------------------------------------------------------------------

    void readSettings() {
        auto* mod = Mod::get();
        g.orbPrediction = mod->getSettingValue<bool>("orb-prediction");
        g.showLabel = mod->getSettingValue<bool>("show-label");
        g.budgetMs = mod->getSettingValue<double>("budget-ms");
    }

    void updateLabel(std::string const& extra = "") {
        if (!g.label) {
            return;
        }
        g.label->setVisible(g.showLabel);
        auto text = fmt::format("Buffer: {}", STRENGTHS[g.strength].label);
        if (!extra.empty()) {
            text += "  " + extra;
        }
        g.label->setString(text.c_str());
    }

    bool isGhost(PlayerObject* player) {
        return player && (player == g.ghost1 || player == g.ghost2);
    }

    bool isFlying(PlayerObject* player) {
        return player->m_isDart || player->m_isBird || player->m_isSwing;
    }

    void rememberHold(bool down) {
        if (down) {
            g.pressedAt = g.tick;
            return;
        }
        if (g.pressedAt < 0) {
            return;
        }
        g.holds[g.holdNext] = std::clamp(g.tick - g.pressedAt, 1, 240);
        g.holdNext = (g.holdNext + 1) % static_cast<int>(g.holds.size());
        g.holdCount = std::min(g.holdCount + 1, static_cast<int>(g.holds.size()));
        g.pressedAt = -1;
    }

    int holdEstimate() {
        if (g.holdCount == 0) {
            return 12;  // about 50 ms, a normal tap
        }
        auto copy = g.holds;
        auto mid = copy.begin() + g.holdCount / 2;
        std::nth_element(copy.begin(), mid, copy.begin() + g.holdCount);
        return *mid;
    }

    // ------------------------------------------------------------------
    // Sandbox: anything the ghost changes in the level is put back afterwards
    // ------------------------------------------------------------------

    void snapshot(GameObject* object) {
        if (!object || g.touched.contains(object)) {
            return;
        }
        g.touched.insert(object);
        Snapshot s { object, object->m_isActivated, nullptr, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            s.effect = effect;
            s.activated = effect->m_activated;
            s.byP1 = effect->m_activatedByPlayer1;
            s.byP2 = effect->m_activatedByPlayer2;
        }
        g.snapshots.push_back(s);
    }

    void restoreSandbox() {
        for (auto const& s : g.snapshots) {
            s.object->m_isActivated = s.isActivated;
            if (s.effect) {
                s.effect->m_activated = s.activated;
                s.effect->m_activatedByPlayer1 = s.byP1;
                s.effect->m_activatedByPlayer2 = s.byP2;
            }
        }
        g.snapshots.clear();
        g.touched.clear();
    }

    // ------------------------------------------------------------------
    // Ghost
    // ------------------------------------------------------------------

    // The ghost lives in a layer that is never added to the scene, so it is never
    // drawn and its effects never update between predictions.
    PlayerObject* makeGhost(PlayLayer* layer, CCLayer* sandbox) {
        auto* ghost = PlayerObject::create(1, 1, layer, sandbox, true);
        if (!ghost) {
            return nullptr;
        }
        ghost->setVisible(false);
        sandbox->addChild(ghost);
        return ghost;
    }

    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->resetTouchedRings(true);
        ghost->copyAttributes(real);
        ghost->setVisible(false);
        ghost->setPosition(real->getPosition());
        ghost->setRotation(real->getRotation());
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_yVelocity = real->m_yVelocity;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDashing = real->m_isDashing;
        ghost->m_lastActivatedPortal = real->m_lastActivatedPortal;

        // Button and jump-buffer state, so the ghost reacts to a press exactly like
        // the real player would instead of keeping leftovers from the last prediction.
        ghost->m_holdingButtons = real->m_holdingButtons;
        ghost->m_jumpBuffered = real->m_jumpBuffered;
        ghost->m_wasJumpBuffered = real->m_wasJumpBuffered;
        ghost->m_stateJumpBuffered = real->m_stateJumpBuffered;
        ghost->m_stateRingJump = real->m_stateRingJump;
        ghost->m_stateRingJump2 = real->m_stateRingJump2;
        ghost->m_wasRobotJump = real->m_wasRobotJump;
        ghost->m_touchedRing = real->m_touchedRing;
        ghost->m_touchedPad = real->m_touchedPad;
        ghost->m_touchedGravityPortal = real->m_touchedGravityPortal;
    }

    void pressGhost(PlayerObject* ghost, bool down) {
        if (down) {
            ghost->pushButton(PlayerButton::Jump);
        }
        else {
            ghost->releaseButton(PlayerButton::Jump);
        }
    }

    void stepGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        ghost->update(dt);
        ghost->updateRotation(dt);
        if (g.ghostDied) {
            return;
        }
        ghost->m_collisionLogTop->removeAllObjects();
        ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects();
        ghost->m_collisionLogRight->removeAllObjects();
        layer->checkCollisions(ghost, dt, false);
    }

    // ------------------------------------------------------------------
    // Prediction
    // ------------------------------------------------------------------

    struct SimResult {
        int survived = 0;     // ticks survived after the input fired
        bool safe = false;    // made it through the whole look-ahead
        bool doomed = false;  // died (or left the simulatable area) before the input
                              // even fired, so a longer delay can't help
    };

    // Simulates pressing (or releasing) `delay` ticks from now.
    // releaseAfter: ticks after the press when the ghost lets go (-1 = keep holding).
    SimResult simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, int delay, bool down, int horizon,
        int releaseAfter) {
        SimResult res;
        bool both = p2 != nullptr;
        if (!g.ghost1 || (both && !g.ghost2)) {
            res.safe = true;  // no ghost, never interfere
            return res;
        }

        copyState(g.ghost1, p1);
        if (both) {
            copyState(g.ghost2, p2);
        }

        g.simulating = true;
        g.ghostDied = false;
        g.ghostHitStop = false;

        float dt = (g.tickDt > 0.001f && g.tickDt < 2.f) ? g.tickDt : FALLBACK_TICK_DT;
        bool flying = isFlying(p1) || (both && isFlying(p2));
        int airTicks = 0;
        int groundTicks = 0;
        int total = delay + horizon;
        int t = 0;

        for (; t < total; ++t) {
            if (t == delay) {
                pressGhost(g.ghost1, down);
                if (both) {
                    pressGhost(g.ghost2, down);
                }
            }
            if (releaseAfter >= 0 && t == delay + releaseAfter) {
                pressGhost(g.ghost1, false);
                if (both) {
                    pressGhost(g.ghost2, false);
                }
            }

            stepGhost(layer, g.ghost1, dt);
            if (both && !g.ghostDied) {
                stepGhost(layer, g.ghost2, dt);
            }
            if (g.ghostDied) {
                break;
            }
            if (g.ghostHitStop) {
                res.safe = t >= delay;
                break;
            }
            if (t < delay) {
                continue;
            }

            ++res.survived;
            if (flying) {
                continue;
            }

            // Ground modes: if the ghost jumped and landed cleanly, that's good enough.
            bool grounded = g.ghost1->m_isOnGround && (!both || g.ghost2->m_isOnGround);
            if (!grounded) {
                ++airTicks;
                groundTicks = 0;
                continue;
            }
            if (airTicks >= 6 && ++groundTicks >= 3) {
                res.safe = true;
                break;
            }
        }

        if (!g.ghostDied && !g.ghostHitStop && res.survived >= horizon) {
            res.safe = true;
        }
        if ((g.ghostDied || g.ghostHitStop) && t < delay) {
            res.doomed = true;
            res.safe = false;
        }

        restoreSandbox();
        g.simulating = false;
        g.ghostDied = false;
        g.ghostHitStop = false;
        return res;
    }

    // Returns how many ticks to hold the input back, or 0 to send it right away.
    int findDelay(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool down, int horizon, int maxDelay,
        int releaseAfter) {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();

        auto now = simulate(layer, p1, p2, 0, down, horizon, releaseAfter);
        if (now.safe || now.doomed) {
            return 0;
        }

        int best = 0;
        int bestSurvived = now.survived;
        for (int k = 1; k <= maxDelay; ++k) {
            auto later = simulate(layer, p1, p2, k, down, horizon, releaseAfter);
            if (later.doomed) {
                break;
            }
            if (later.safe) {
                return k;
            }
            if (later.survived > bestSurvived) {
                best = k;
                bestSurvived = later.survived;
            }
            if (std::chrono::duration<double, std::milli>(clock::now() - start).count() > g.budgetMs) {
                break;
            }
        }

        // Nothing fully safe: only use a delay if it's clearly better than clicking now.
        return bestSurvived >= now.survived + 20 ? best : 0;
    }

    // ------------------------------------------------------------------
    // Input pipeline
    // ------------------------------------------------------------------

    void send(PlayLayer* layer, bool down) {
        g.injecting = true;
        g.appliedDown = down;
        layer->handleButton(down, 1, g.isPlayer1);
        g.injecting = false;
    }

    // Drop anything waiting and make the game match your finger again.
    void flush(PlayLayer* layer) {
        g.queue.clear();
        g.shift = 0;
        if (layer && g.appliedDown != g.physicalDown) {
            send(layer, g.physicalDown);
        }
    }

    void resetInput() {
        g.queue.clear();
        g.shift = 0;
        g.physicalDown = false;
        g.appliedDown = false;
        g.pressedAt = -1;
    }

    // Called once per physics tick, right before player 1 moves.
    void deliver(PlayLayer* layer) {
        if (g.queue.empty()) {
            return;
        }
        auto* p1 = layer->m_player1;
        auto* p2 = layer->m_gameState.m_isDualMode ? layer->m_player2 : nullptr;
        if (!p1 || p1->m_isShip || (p2 && p2->m_isShip)) {
            flush(layer);
            return;
        }

        // An input queued with N ticks fires right before the Nth tick from when it
        // was pressed, which is exactly what simulate(delay = N) tested.
        while (!g.queue.empty() && g.queue.front().ticksLeft <= 0) {
            bool down = g.queue.front().down;
            g.queue.pop_front();
            if (down != g.appliedDown) {
                send(layer, down);
            }
        }
        for (auto& pending : g.queue) {
            --pending.ticksLeft;
        }
        if (g.queue.empty()) {
            g.shift = 0;
        }
    }

    // Returns true if the input was taken (held back), false to let it through now.
    bool intercept(PlayLayer* layer, bool down, bool isPlayer1) {
        g.physicalDown = down;
        g.isPlayer1 = isPlayer1;

        auto const& strength = STRENGTHS[g.strength];
        auto* p1 = layer->m_player1;
        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;

        bool skip = strength.maxTicks <= 0 || !p1 || p1->m_isDead || p1->m_isPlatformer || p1->m_isShip ||
            (p2 && p2->m_isShip);
        if (skip) {
            g.queue.clear();
            g.shift = 0;
            g.appliedDown = down;
            return false;
        }

        // A click is already being held back: delay this one by the same amount,
        // so the gap between your inputs (tap length, double taps) stays the same.
        if (!g.queue.empty()) {
            g.queue.push_back({ std::max(g.shift, g.queue.back().ticksLeft), down });
            return true;
        }

        bool wave = p1->m_isDart || (p2 && p2->m_isDart);
        bool robot = p1->m_isRobot || (p2 && p2->m_isRobot);

        // Letting go only changes anything in wave and robot.
        if (!down && !wave && !robot) {
            g.appliedDown = down;
            return false;
        }

        bool mini = p1->m_vehicleSize < 1.f;
        int horizon = wave ? (mini ? 24 : 32) : 48;
        int maxDelay = wave ? strength.maxWaveTicks : strength.maxTicks;

        // Cube, ball, ufo, spider and swing re-trigger while held, so model a tap
        // the length you usually tap. Wave and robot hold until their own release.
        int releaseAfter = (down && !wave && !robot) ? holdEstimate() : -1;

        int delay = findDelay(layer, p1, p2, down, horizon, maxDelay, releaseAfter);
        if (delay <= 0) {
            g.appliedDown = down;
            return false;
        }

        g.shift = delay;
        g.queue.push_back({ delay, down });
        updateLabel(fmt::format("saved {} +{}t", down ? "click" : "release", delay));
        log::debug("Held {} back by {} ticks", down ? "press" : "release", delay);
        return true;
    }

    void cycleStrength(PlayLayer* layer) {
        g.strength = (g.strength + 1) % static_cast<int>(STRENGTHS.size());
        readSettings();
        flush(layer);
        updateLabel();
        log::info("Strength: {}", STRENGTHS[g.strength].label);
    }

    PlayLayer* currentLayer() {
        return GameManager::get()->m_playLayer;
    }
}

$on_mod(Loaded) {
    pib::readSettings();
    auto start = Mod::get()->getSettingValue<int64_t>("start-strength");
    pib::g.strength = static_cast<int>(std::clamp<int64_t>(start, 0, pib::STRENGTHS.size() - 1));
}

// ----------------------------------------------------------------------
// Hooks
// ----------------------------------------------------------------------

class $modify(PIBPlayLayer, PlayLayer) {
    struct Fields {
        Ref<CCLayer> sandbox;
        Ref<PlayerObject> ghost1;
        Ref<PlayerObject> ghost2;
    };

    static void onModify(auto& self) {
        // Run before noclip-style mods so ghost deaths are always seen.
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        pib::readSettings();
        pib::resetInput();

        auto* sandbox = CCLayer::create();
        m_fields->sandbox = sandbox;
        m_fields->ghost1 = pib::makeGhost(this, sandbox);
        m_fields->ghost2 = pib::makeGhost(this, sandbox);
        pib::g.ghost1 = m_fields->ghost1.data();
        pib::g.ghost2 = m_fields->ghost2.data();

        auto* label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(0.3f);
        label->setAnchorPoint({ 0.f, 0.f });
        label->setPosition({ 4.f, 4.f });
        label->setOpacity(160);
        this->addChild(label, 1000);
        pib::g.label = label;
        pib::updateLabel();
        return true;
    }

    void onQuit() {
        pib::g.ghost1 = nullptr;
        pib::g.ghost2 = nullptr;
        pib::g.label = nullptr;
        pib::resetInput();
        m_fields->ghost1 = nullptr;
        m_fields->ghost2 = nullptr;
        m_fields->sandbox = nullptr;
        PlayLayer::onQuit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (pib::g.simulating) {
            pib::g.ghostDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
        if (player && player->m_isDead) {
            pib::g.queue.clear();
            pib::g.shift = 0;
            pib::g.appliedDown = false;
        }
    }

    void resetLevel() {
        pib::resetInput();
        PlayLayer::resetLevel();
    }

    void levelComplete() {
        if (pib::g.simulating) {
            return;
        }
        PlayLayer::levelComplete();
    }

    void playEndAnimationToPos(CCPoint position) {
        if (pib::g.simulating) {
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(PIBGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarly);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::flipGravity", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::gameEventTriggered", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::playerWillSwitchMode", Priority::First);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (pib::g.simulating || pib::g.injecting || button != 1) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        auto* layer = pib::currentLayer();
        if (!layer || static_cast<GJBaseGameLayer*>(layer) != this || (m_level && m_level->m_twoPlayerMode)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        pib::rememberHold(down);
        if (pib::intercept(layer, down, isPlayer1)) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // While predicting, the ghost only sees what can kill it (and orbs/pads if enabled).
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!pib::g.simulating || !objects) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        static gd::vector<GameObject*> kept;
        kept.clear();

        uint64_t keep = pib::g.orbPrediction ? (pib::SOLID_MASK | pib::ORB_MASK) : pib::SOLID_MASK;
        bool haveRect = false;
        CCRect ghostRect;
        int count = std::min(objectCount, static_cast<int>(objects->size()));

        for (int i = 0; i < count; ++i) {
            auto* object = (*objects)[i];
            if (!object) {
                continue;
            }
            uint64_t type = pib::typeBit(object);
            if (keep & type) {
                kept.push_back(object);
                continue;
            }
            if (!pib::g.orbPrediction) {
                continue;
            }
            bool stop = (pib::STOP_MASK & type) != 0;
            float speed = stop ? 0.f : pib::speedPortalSpeed(object->m_objectID);
            if (!stop && speed <= 0.f) {
                continue;
            }
            if (!haveRect) {
                ghostRect = player->getObjectRect();
                haveRect = true;
            }
            if (ghostRect.intersectsRect(object->getObjectRect())) {
                if (stop) {
                    pib::g.ghostHitStop = true;
                }
                else {
                    player->m_playerSpeed = speed;
                }
            }
        }

        GJBaseGameLayer::collisionCheckObjects(player, &kept, static_cast<int>(kept.size()), dt);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (!pib::g.simulating) {
            return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
        }
        if (!pib::g.orbPrediction || !pib::isGhost(player) || !object || pib::g.touched.contains(object)) {
            return false;
        }
        // Already used by the real player, so it's not there for the ghost either.
        if (object->m_activatedByPlayer1 || object->m_activatedByPlayer2) {
            return false;
        }
        pib::snapshot(object);
        return true;
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (pib::g.simulating) {
            if (!pib::g.orbPrediction || !pib::isGhost(player)) {
                return;
            }
            pib::snapshot(ring);
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }

    // Gravity portals, blue pads and blue orbs: flip only the ghost, with no effects.
    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (pib::g.simulating) {
            if (pib::g.orbPrediction && pib::isGhost(player)) {
                player->flipGravity(flip, true);
            }
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    // Never let the ghost fire event triggers in the real level.
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (pib::g.simulating) {
            return;
        }
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    // Safety net if a mode portal slips through: end the prediction, change nothing.
    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (pib::g.simulating) {
            pib::g.ghostHitStop = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }
};

class $modify(PIBPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::ringJump", Priority::First);
        (void)self.setHookPriority("PlayerObject::incrementJumps", Priority::First);
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", Priority::First);
    }

    // Runs once per physics tick for each player.
    void update(float dt) {
        if (!pib::g.simulating) {
            auto* layer = pib::currentLayer();
            if (layer && this == layer->m_player1) {
                if (dt > 0.f) {
                    pib::g.tickDt = dt;
                }
                ++pib::g.tick;
                pib::deliver(layer);
            }
        }
        PlayerObject::update(dt);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (pib::g.simulating) {
            if (!pib::g.orbPrediction || !pib::isGhost(this)) {
                return;
            }
            pib::snapshot(ring);
        }
        PlayerObject::ringJump(ring, skipCheck);
    }

    // Keeps the ghost from adding to your jump count.
    void incrementJumps() {
        if (pib::g.simulating) {
            return;
        }
        PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (pib::g.simulating) {
            return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }
};

class $modify(PIBEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (pib::g.simulating) {
            return;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(PIBGameObject, GameObject) {
    void playShineEffect() {
        if (pib::g.simulating) {
            return;
        }
        GameObject::playShineEffect();
    }
};

class $modify(PIBHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        if (pib::g.simulating) {
            return;
        }
        HardStreak::addPoint(point);
    }
};

// F6 through the game's own keyboard events, so it only works while GD is focused.
class $modify(PIBKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat && key == KEY_F6) {
            if (auto* layer = pib::currentLayer()) {
                pib::cycleStrength(layer);
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
