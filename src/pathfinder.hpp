#pragma once
#include <Geode/Geode.hpp>
#include <map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include "state.hpp"

namespace pf {

enum class Phase {
    Idle,
    Search, // depth-first search over input toggles, rewinding with snapshots
    Verify, // replay the found path from a clean start, no rewinds
};

// What the update loop has to do before it can simulate the next tick.
enum class Pending {
    None,
    Restore, // rewind to m_restoreTick (snapshot or full reset)
    Died,
    Won,
};

class Pathfinder {
public:
    static Pathfinder& get();

    bool active() const { return m_phase != Phase::Idle; }
    Phase phase() const { return m_phase; }

    void start(PlayLayer* pl);
    void stop(PlayLayer* pl, std::string const& reason = "");
    void toggle(PlayLayer* pl);

    // Called from the hooks.
    // GJBaseGameLayer::update runs: while (beginTick()) { original update(1/240); afterUpdate(); }
    bool beginTick(PlayLayer* pl, std::chrono::steady_clock::time_point frameStart);
    void afterUpdate(PlayLayer* pl);
    void afterStep(PlayLayer* pl, bool isLastTick);   // GJBaseGameLayer::processCommands
    void onPlayerDestroyed() { m_died = true; }       // PlayLayer::destroyPlayer
    void updateLabel(PlayLayer* pl);

    void setInDriver(bool v) { m_inDriver = v; }
    bool inDriver() const { return m_inDriver; }
    bool blockLevelComplete() const { return active() || m_blockComplete; }
    void clearCompleteBlock() { m_blockComplete = false; }
    static constexpr float tickDt() { return 1.f / 240.f; }

private:
    struct Frame {
        size_t depth; // toggles[0..depth) is this node's path
        int base;     // tick of the node's last toggle (0 for the root)
        int death;    // tick the node's path dies at with no further toggles
        int nextT;    // next toggle tick to try (counts down)
        int low;      // lowest toggle tick in the current window
        bool widened = false;
    };

    Phase m_phase = Phase::Idle;
    Pending m_pending = Pending::None;
    bool m_inDriver = false;
    bool m_died = false;
    bool m_blockComplete = false;
    bool m_runIsChild = false; // current run tries a new toggle (vs. root / verify)

    // settings (read at start)
    int m_window = 60;
    int m_step = 1;
    int m_snapEvery = 4;
    int m_maxSnaps = 400;
    int m_budgetMs = 14;
    int m_maxRepairs = 20;
    int m_seed = 1;
    bool m_exact = false;
    bool m_dedupe = true;

    // current path: ascending ticks at which the jump button flips
    std::vector<int> m_toggles;
    size_t m_nextToggle = 0;
    bool m_held = false;

    std::vector<Frame> m_stack;
    std::map<int, Snapshot> m_snaps;
    std::unordered_set<uint64_t> m_deathSigs;
    int m_restoreTick = 0;

    // stats
    uint64_t m_attempts = 0;
    int m_bestTick = 0;
    float m_bestPercent = 0.f;
    int m_repairs = 0;
    int m_lastTick = 0;
    int m_stuckLoops = 0;
    std::chrono::steady_clock::time_point m_startTime;

    geode::Ref<cocos2d::CCLabelBMFont> m_label;

    int tick(PlayLayer* pl) const;
    void readSettings();
    void fullReset(PlayLayer* pl);
    void takeSnapshot(PlayLayer* pl, int t);
    void thinSnapshots();
    void restoreTo(PlayLayer* pl, int t);
    void applyInputsAt(PlayLayer* pl, int t);
    void onDeath(PlayLayer* pl, int d);
    void onWin(PlayLayer* pl, int t);
    bool advance(); // pick the next candidate; false = search exhausted
    uint64_t deathSignature(PlayLayer* pl, int d) const;
    float percentOf(PlayLayer* pl) const;
    void exportReplay(PlayLayer* pl, int endTick);
    void removeLabel();
};

} // namespace pf
