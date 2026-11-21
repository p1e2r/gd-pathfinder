#pragma once
#include <Geode/Geode.hpp>
#include <map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <random>
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
    Reached, // the run hit m_stopAtTick alive
};

enum class Strategy {
    Lookahead,  // random lookahead: try random click sets ahead, keep the best, commit a third
    Exhaustive, // depth-first over single click timings
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
    struct Candidate {
        int t;     // toggle tick
        int death; // where that toggle's run died
    };

    struct Frame {
        size_t depth; // toggles[0..depth) is this node's path
        int base;     // tick of the node's last toggle (0 for the root)
        int death;    // tick the node's path dies at with no further toggles
        int nextT;    // next toggle tick to scan (counts down)
        int low;      // lowest toggle tick in the current window
        bool widened = false;
        bool scanning = true;            // still scanning the window
        std::vector<Candidate> deferred; // scanned, no progress yet; expanded best-first later
        size_t expandIdx = 0;
        uint64_t spent = 0;  // tries in this subtree since the last new best
        uint64_t budget = 0; // pop the frame once spent passes this
    };

    enum class RunKind { Root, Scan, Expand };

    // --- random lookahead ---
    enum class LaRun { Try, Commit };
    Strategy m_strategy = Strategy::Lookahead;
    int m_laTicks = 1000;    // how far ahead each try plans
    int m_laTries = 300;     // tries per step
    int m_laClicks = 30;     // button flips per try
    std::mt19937 m_rng;
    LaRun m_laRun = LaRun::Try;
    int m_stopAtTick = -1;   // end the run alive at this tick (-1 = run until death)
    int m_anchor = 0;        // committed point; a snapshot exists here (or it's 0)
    int m_try = 0;
    int m_stepBest = 0;      // furthest tick reached by a try this step
    std::vector<int> m_stepBestSuffix;
    int m_farthest = 0;      // furthest committed point so far
    int m_backoff = 1;       // how far to roll back after a failed step
    int m_backoffCap = 1000; // never roll back further than m_farthest - this

    Phase m_phase = Phase::Idle;
    Pending m_pending = Pending::None;
    bool m_inDriver = false;
    bool m_died = false;
    bool m_blockComplete = false;
    RunKind m_runKind = RunKind::Root;
    int m_runToggle = 0;       // toggle tick tried by the current Scan run
    size_t m_bottomDepth = 0;  // depth of the bottom frame on the stack
    uint64_t m_budget = 300;   // current per-branch stall budget
    uint64_t m_baseBudget = 300;
    bool m_budgetPopped = false; // a frame was abandoned this round
    int m_rounds = 1;
    uint64_t m_lastLogAttempts = 0;

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
    bool advance(); // pick the next run; false = search exhausted
    void pushFrame(int d);
    void laBeginTry();
    void laEndTry(PlayLayer* pl, int end);
    void laFinishStep(PlayLayer* pl);
    void laMoveAnchor(PlayLayer* pl, int target); // committed toggles < target are kept
    void laOnReached(PlayLayer* pl, int t);
    void runToggle(size_t depth, int t, RunKind kind);
    uint64_t deathSignature(PlayLayer* pl, int d) const;
    float percentOf(PlayLayer* pl) const;
    void exportReplay(PlayLayer* pl, int endTick);
    void removeLabel();
};

} // namespace pf
