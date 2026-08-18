// FBZZ Engine
// GameResultState.hpp | sandbox
// Main.scene から Result.scene へ受け渡す最小限の戦績
#pragma once

namespace sandbox {

struct GameResultState {
    static inline bool victory = false;
    static inline float clearSeconds = 0.0f;
    static inline int defeatedEnemies = 0;
    static inline int enemyImpacts = 0;
    static inline int anchorImpacts = 0;
};

} // namespace sandbox
