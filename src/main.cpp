#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include "pathfinder.hpp"

using namespace geode::prelude;
using pf::Pathfinder;

static bool isThePlayLayer(GJBaseGameLayer* layer) {
    auto pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
}

class $modify(PFGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        auto& p = Pathfinder::get();
        if (!p.active() || p.inDriver() || !isThePlayLayer(this))
            return GJBaseGameLayer::update(dt);

        auto pl = PlayLayer::get();
        auto frameStart = std::chrono::steady_clock::now();
        // run as many 240 TPS ticks as fit in this frame's time budget
        while (p.beginTick(pl, frameStart)) {
            p.setInDriver(true);
            GJBaseGameLayer::update(Pathfinder::tickDt());
            p.setInDriver(false);
            p.afterUpdate(pl);
        }
        if (p.active()) p.updateLabel(pl);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        auto& p = Pathfinder::get();
        if (p.active() && p.inDriver() && isThePlayLayer(this))
            p.afterStep(PlayLayer::get(), isLastTick);
    }
};

class $modify(PFPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& p = Pathfinder::get();
        if (!p.active() || object == m_anticheatSpike || (player != m_player1 && player != m_player2))
            return PlayLayer::destroyPlayer(player, object);
        // while searching a death only marks the run as failed; it gets rewound
        p.onPlayerDestroyed();
    }

    void levelComplete() {
        if (Pathfinder::get().blockLevelComplete()) return;
        PlayLayer::levelComplete();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto& p = Pathfinder::get();
        if (!p.active()) p.clearCompleteBlock();
    }

    void onQuit() {
        auto& p = Pathfinder::get();
        if (p.active()) p.stop(nullptr);
        p.clearCompleteBlock();
        PlayLayer::onQuit();
    }
};

class $modify(PFPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        bool running = Pathfinder::get().active();
        auto spr = ButtonSprite::create(running ? "Stop Pathfinder" : "Pathfind", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PFPauseLayer::onPathfinder));

        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({spr->getScaledContentSize().width / 2.f + 10.f, 24.f});
        menu->setID("pathfinder-menu"_spr);
        this->addChild(menu, 10);
    }

    void onPathfinder(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        Pathfinder::get().toggle(pl);
        this->onResume(nullptr);
    }
};

$execute {
    geode::listenForKeybindSettingPresses("toggle_pathfinder", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;
        if (auto pl = PlayLayer::get()) Pathfinder::get().toggle(pl);
    });
}
