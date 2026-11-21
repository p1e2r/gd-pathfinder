# v0.2.0
- New default **Lookahead** strategy (same approach as camila314/pathfinder, separate implementation, running the real game): random click patterns ahead, commit the best third, roll back further when stuck, restart instead of giving up. The old search is still there as **Exhaustive**.
- Exhaustive: fix it getting stuck: it scans every timing in the window first and expands the ones that got furthest first, instead of diving into the first one that survived.
- Branches that go too many tries without a new best are abandoned (new *Stall budget* setting). If all of them run out, the search restarts with a 4x bigger budget instead of giving up.
- Label shows tries per second and the round; progress is logged every 1000 tries.

# v0.1.0
- First version: depth-first search over jump timings with snapshot rewinds, clean-start verification, GDR (.gdr / .gdr.json) export.
