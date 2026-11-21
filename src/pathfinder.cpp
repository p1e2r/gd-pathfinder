#include "pathfinder.hpp"
#include "gdr.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <asp/fs/std.hpp>

using namespace geode::prelude;

namespace pf {

static constexpr int kMaxTicks = 240 * 60 * 30; // 30 minutes of level time

Pathfinder& Pathfinder::get() {
    static Pathfinder inst;
    return inst;
}

int Pathfinder::tick(PlayLayer* pl) const {
    return static_cast<int>(pl->m_gameState.m_currentProgress);
}

void Pathfinder::readSettings() {
    auto mod = Mod::get();
    m_window = static_cast<int>(mod->getSettingValue<int64_t>("search_window"));
    m_step = static_cast<int>(mod->getSettingValue<int64_t>("search_step"));
    m_snapEvery = static_cast<int>(mod->getSettingValue<int64_t>("snapshot_interval"));
    m_maxSnaps = static_cast<int>(mod->getSettingValue<int64_t>("max_snapshots"));
    m_budgetMs = static_cast<int>(mod->getSettingValue<int64_t>("frame_budget_ms"));
    m_maxRepairs = static_cast<int>(mod->getSettingValue<int64_t>("max_repairs"));
    m_seed = static_cast<int>(mod->getSettingValue<int64_t>("seed"));
    m_exact = mod->getSettingValue<bool>("exact_mode");
    m_dedupe = mod->getSettingValue<bool>("dedupe_deaths");
    m_window = std::max(m_window, 2);
    m_step = std::max(m_step, 1);
    m_snapEvery = std::max(m_snapEvery, 1);
    m_maxSnaps = std::max(m_maxSnaps, 16);
    m_budgetMs = std::clamp(m_budgetMs, 1, 200);
}

void Pathfinder::toggle(PlayLayer* pl) {
    if (active()) stop(pl, "Stopped.");
    else start(pl);
}

void Pathfinder::start(PlayLayer* pl) {
    if (!pl || active()) return;

    if (pl->m_isPlatformer) {
        FLAlertLayer::create("Pathfinder", "Platformer levels aren't supported yet (classic mode only).", "OK")->show();
        return;
    }
    if (pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode) {
        Notification::create("2-player level: only player 1's button is searched", NotificationIcon::Warning)->show();
    }
    if (pl->m_isPracticeMode) pl->togglePracticeMode(false);

    readSettings();
    m_toggles.clear();
    m_stack.clear();
    m_snaps.clear();
    m_deathSigs.clear();
    m_attempts = 0;
    m_bestTick = 0;
    m_bestPercent = 0.f;
    m_repairs = 0;
    m_lastTick = 0;
    m_stuckLoops = 0;
    m_runIsChild = false;
    m_startTime = std::chrono::steady_clock::now();

    m_phase = Phase::Search;
    m_pending = Pending::None;
    fullReset(pl);

    auto label = CCLabelBMFont::create("Pathfinder", "bigFont.fnt");
    label->setScale(0.3f);
    label->setAnchorPoint({0.f, 1.f});
    label->setAlignment(kCCTextAlignmentLeft);
    label->setPosition({6.f, CCDirector::get()->getWinSize().height - 6.f});
    label->setOpacity(220);
    pl->addChild(label, 10000);
    m_label = label;
    updateLabel(pl);

    log::info("search started: window {} step {} snapshots every {} ({}), exact {}",
        m_window, m_step, m_snapEvery, m_maxSnaps, m_exact);
}

void Pathfinder::stop(PlayLayer* pl, std::string const& reason) {
    if (!active()) return;
    m_phase = Phase::Idle;
    m_pending = Pending::None;
    m_snaps.clear();
    m_stack.clear();
    m_deathSigs.clear();
    removeLabel();
    // the end animation may already be running; don't let it complete the level
    m_blockComplete = true;
    if (pl) {
        pl->m_player1->releaseAllButtons();
        pl->m_player2->releaseAllButtons();
        pl->resetLevelFromStart();
    }
    if (!reason.empty()) log::info("stopped: {}", reason);
}

void Pathfinder::fullReset(PlayLayer* pl) {
    pl->resetLevelFromStart();
    GameToolbox::fast_srand(static_cast<uint64_t>(m_seed));
    m_held = false;
    m_nextToggle = 0;
    m_died = false;
    m_lastTick = tick(pl);
    FMODAudioEngine::sharedEngine()->stopAllMusic(false);
}

void Pathfinder::takeSnapshot(PlayLayer* pl, int t) {
    Snapshot s;
    s.tick = t;
    s.checkpoint = pl->createCheckpoint();
    if (!s.checkpoint) return;
    s.p1.save(pl->m_player1);
    s.p2.save(pl->m_player2);
    s.seed = GameToolbox::getfast_srand();
    m_snaps[t] = std::move(s);
    thinSnapshots();
}

void Pathfinder::thinSnapshots() {
    if (static_cast<int>(m_snaps.size()) <= m_maxSnaps) return;
    // keep everything near the frontier, keep sparse ones further back
    int frontier = m_snaps.rbegin()->first;
    int keepRecent = frontier - 4 * m_window;
    for (auto it = m_snaps.begin(); it != m_snaps.end() && static_cast<int>(m_snaps.size()) > m_maxSnaps * 3 / 4;) {
        if (it->first != 0 && it->first < keepRecent && it->first % 240 != 0) it = m_snaps.erase(it);
        else ++it;
    }
    // still too many (very long level): drop the oldest sparse ones
    for (auto it = m_snaps.begin(); it != m_snaps.end() && static_cast<int>(m_snaps.size()) > m_maxSnaps;) {
        if (it->first != 0 && it->first < keepRecent) it = m_snaps.erase(it);
        else ++it;
    }
}

void Pathfinder::applyInputsAt(PlayLayer* pl, int t) {
    while (m_nextToggle < m_toggles.size() && m_toggles[m_nextToggle] <= t) {
        if (m_toggles[m_nextToggle] == t) {
            m_held = !m_held;
            pl->handleButton(m_held, 1, true);
        }
        ++m_nextToggle;
    }
}

void Pathfinder::restoreTo(PlayLayer* pl, int t) {
    // snapshots past t were made on the old path
    m_snaps.erase(m_snaps.upper_bound(t), m_snaps.end());

    auto it = m_snaps.upper_bound(t);
    if (m_exact || it == m_snaps.begin()) {
        m_snaps.clear();
        fullReset(pl);
        return;
    }
    --it;
    Snapshot const& s = it->second;

    pl->loadFromCheckpoint(s.checkpoint);
    s.p1.load(pl->m_player1);
    s.p2.load(pl->m_player2);
    GameToolbox::fast_srand(s.seed);
    pl->m_gameState.m_currentProgress = static_cast<unsigned int>(s.tick);
    pl->m_queuedButtons.clear();
    FMODAudioEngine::sharedEngine()->stopAllMusic(false);

    // the snapshot was taken before the inputs of its tick were applied
    size_t before = std::lower_bound(m_toggles.begin(), m_toggles.end(), s.tick) - m_toggles.begin();
    m_held = (before % 2) == 1;
    m_nextToggle = before;
    m_died = false;
    m_lastTick = s.tick;
    applyInputsAt(pl, s.tick);
}

void Pathfinder::afterStep(PlayLayer* pl, bool isLastTick) {
    if (!active() || m_pending != Pending::None) return;
    int t = tick(pl);
    if (m_died) {
        m_pending = Pending::Died;
        m_restoreTick = t; // reused as the death tick
        return;
    }
    if (pl->m_levelEndAnimationStarted) {
        m_pending = Pending::Won;
        m_restoreTick = t;
        return;
    }
    if (isLastTick && !m_exact && t > 0 && t % m_snapEvery == 0 && !m_snaps.contains(t))
        takeSnapshot(pl, t);
    applyInputsAt(pl, t);
}

bool Pathfinder::beginTick(PlayLayer* pl, std::chrono::steady_clock::time_point frameStart) {
    while (active()) {
        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        if (elapsed >= std::chrono::milliseconds(m_budgetMs)) return false;

        Pending p = m_pending;
        m_pending = Pending::None;
        switch (p) {
            case Pending::None: return true;
            case Pending::Died: onDeath(pl, m_restoreTick); break;
            case Pending::Won: onWin(pl, m_restoreTick); break;
            case Pending::Restore: restoreTo(pl, m_restoreTick); break;
        }
    }
    return false;
}

void Pathfinder::afterUpdate(PlayLayer* pl) {
    if (!active() || m_pending != Pending::None) return;
    int t = tick(pl);
    if (m_died) {
        m_pending = Pending::Died;
        m_restoreTick = t;
        return;
    }
    if (pl->m_levelEndAnimationStarted) {
        m_pending = Pending::Won;
        m_restoreTick = t;
        return;
    }
    if (t == m_lastTick) {
        if (++m_stuckLoops > 2000) stop(pl, "simulation stalled");
    } else {
        m_stuckLoops = 0;
    }
    m_lastTick = t;
    if (t > kMaxTicks) stop(pl, "level too long");
}

uint64_t Pathfinder::deathSignature(PlayLayer* pl, int d) const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](int64_t v) {
        h ^= static_cast<uint64_t>(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    };
    auto player = [&](PlayerObject* p) {
        mix(std::llround(p->m_positionX * 100.0));
        mix(std::llround(p->m_positionY * 100.0));
        mix(std::llround(p->m_yVelocity * 1000.0));
        int modes = p->m_isShip | p->m_isBird << 1 | p->m_isBall << 2 | p->m_isDart << 3 |
            p->m_isRobot << 4 | p->m_isSpider << 5 | p->m_isSwing << 6 | p->m_isUpsideDown << 7 |
            p->m_isOnGround << 8;
        mix(modes);
    };
    mix(d);
    mix(m_held);
    player(pl->m_player1);
    if (pl->m_gameState.m_isDualMode) player(pl->m_player2);
    return h;
}

float Pathfinder::percentOf(PlayLayer* pl) const {
    if (pl->m_levelLength <= 0.f) return 0.f;
    return std::clamp(pl->m_player1->getPositionX() / pl->m_levelLength * 100.f, 0.f, 100.f);
}

void Pathfinder::onDeath(PlayLayer* pl, int d) {
    ++m_attempts;
    if (d > m_bestTick) {
        m_bestTick = d;
        m_bestPercent = std::max(m_bestPercent, percentOf(pl));
    }

    if (m_phase == Phase::Verify) {
        // the path drifted when replayed from the start; search again from where it died,
        // using the snapshots the clean run just made
        if (++m_repairs > m_maxRepairs) {
            stop(pl);
            FLAlertLayer::create("Pathfinder",
                fmt::format("The found path didn't survive a clean replay after {} repairs. "
                    "Try <cy>Exact mode</c> in the mod settings.", m_maxRepairs), "OK")->show();
            return;
        }
        log::info("verify died at tick {}, repair {}", d, m_repairs);
        m_phase = Phase::Search;
        m_toggles.erase(std::lower_bound(m_toggles.begin(), m_toggles.end(), d), m_toggles.end());
        m_stack.clear();
        int base = m_toggles.empty() ? 0 : m_toggles.back();
        m_stack.push_back({m_toggles.size(), base, d, d - 1, std::max({base + 1, d - m_window, 1})});
        m_deathSigs.insert(deathSignature(pl, d));
    } else {
        uint64_t sig = deathSignature(pl, d);
        bool seen = !m_deathSigs.insert(sig).second;
        // a new toggle that ends in exactly the same death changed nothing; its children
        // would repeat the parent's own candidates
        if (!(m_runIsChild && m_dedupe && seen)) {
            int base = m_toggles.empty() ? 0 : m_toggles.back();
            m_stack.push_back({m_toggles.size(), base, d, d - 1, std::max({base + 1, d - m_window, 1})});
        }
    }

    if (!advance()) {
        stop(pl);
        FLAlertLayer::create("Pathfinder",
            fmt::format("No path found (best {:.1f}%). Try a bigger <cy>Search window</c> in the mod settings.",
                m_bestPercent), "OK")->show();
    }
}

bool Pathfinder::advance() {
    while (true) {
        if (m_stack.empty()) {
            if (m_toggles.empty()) return false;
            // after a repair the stack only covers the newest segment; to go further back,
            // reopen the decision that placed the last toggle
            int last = m_toggles.back();
            m_toggles.pop_back();
            int base = m_toggles.empty() ? 0 : m_toggles.back();
            m_stack.push_back({m_toggles.size(), base, last + 1, last - 1, std::max({base + 1, last - m_window, 1})});
            continue;
        }

        Frame& f = m_stack.back();
        if (f.nextT < f.low) {
            if (!f.widened && f.low > f.base + 1) {
                f.widened = true;
                f.nextT = f.low - 1;
                f.low = std::max(f.base + 1, f.low - 3 * m_window);
                continue;
            }
            m_stack.pop_back();
            continue;
        }

        int t = f.nextT;
        f.nextT -= m_step;
        m_toggles.resize(f.depth);
        m_toggles.push_back(t);
        m_restoreTick = t;
        m_pending = Pending::Restore;
        m_runIsChild = true;
        return true;
    }
}

void Pathfinder::onWin(PlayLayer* pl, int t) {
    ++m_attempts;
    m_bestTick = std::max(m_bestTick, t);
    m_bestPercent = 100.f;

    if (m_phase == Phase::Search && !m_exact) {
        log::info("path found at tick {} ({} toggles), verifying", t, m_toggles.size());
        m_phase = Phase::Verify;
        m_stack.clear();
        m_snaps.clear(); // search snapshots came from rewound runs
        m_runIsChild = false;
        fullReset(pl);
        return;
    }

    exportReplay(pl, t);
}

void Pathfinder::exportReplay(PlayLayer* pl, int endTick) {
    gdr::Replay r;
    r.author = GameManager::get()->m_playerName;
    r.description = "Generated by Pathfinder";
    r.duration = static_cast<float>(endTick) / 240.f;
    r.seed = m_seed;
    r.botVersion = Mod::get()->getVersion().toVString();
    if (pl->m_level) {
        r.levelID = static_cast<uint32_t>(pl->m_level->m_levelID.value());
        r.levelName = pl->m_level->m_levelName;
    }

    bool down = false;
    for (int t : m_toggles) {
        if (t >= endTick) break;
        down = !down;
        r.inputs.push_back({t, 1, false, down});
    }
    if (down) r.inputs.push_back({endTick, 1, false, false});

    bool asJson = Mod::get()->getSettingValue<bool>("export_json");
    auto folder = Mod::get()->getSettingValue<asp::fs::Path>("export_folder").toStdPath().unwrapOrDefault();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);

    std::string name = r.levelName.empty() ? "level" : r.levelName;
    for (char& c : name) {
        if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 32) c = '_';
    }
    std::string ext = asJson ? ".gdr.json" : ".gdr";
    auto path = folder / (name + "_pathfinder" + ext);
    for (int n = 2; std::filesystem::exists(path, ec); ++n)
        path = folder / fmt::format("{}_pathfinder_{}{}", name, n, ext);

    bool ok = false;
    {
        std::ofstream out(path, std::ios::binary);
        if (out) {
            if (asJson) {
                auto s = r.toJson();
                out.write(s.data(), static_cast<std::streamsize>(s.size()));
            } else {
                auto bytes = r.toMsgpack();
                out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            ok = static_cast<bool>(out);
        }
    }

    auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_startTime).count();
    auto attempts = m_attempts;
    auto inputs = r.inputs.size();
    auto repairs = m_repairs;
    stop(pl);

    if (!ok) {
        FLAlertLayer::create("Pathfinder", fmt::format("Found a path but couldn't write <cy>{}</c>.", path.string()), "OK")->show();
        return;
    }
    log::info("saved {} ({} inputs, {} attempts, {}s)", path.string(), inputs, attempts, secs);
    FLAlertLayer::create("Pathfinder",
        fmt::format("Path found in {}s ({} attempts, {} repairs).\n{} inputs saved to\n<cg>{}</c>",
            secs, attempts, repairs, inputs, path.filename().string()), "OK")->show();
}

void Pathfinder::updateLabel(PlayLayer* pl) {
    if (!m_label) return;
    char const* what = m_phase == Phase::Verify ? "verifying" : "searching";
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_startTime).count();
    m_label->setString(fmt::format(
        "Pathfinder: {} ({}s)\nbest {:.2f}%  now {:.2f}%\ntries {}  depth {}  inputs {}  snaps {}",
        what, secs, m_bestPercent, percentOf(pl), m_attempts, m_stack.size(), m_toggles.size(), m_snaps.size()).c_str());
}

void Pathfinder::removeLabel() {
    if (m_label) m_label->removeFromParent();
    m_label = nullptr;
}

} // namespace pf
