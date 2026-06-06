// FBZZ Engine
// SceneIO.hpp | fbzz::editor
// シーンを TOML 形式でシリアライズ/デシリアライズ
#pragma once
#include <string>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

class SceneIO {
public:
    static bool Save(const scene::Scene& scene, const std::string& path);
    static bool Load(scene::Scene& scene, const std::string& path);

    // PlayMode スナップショット用 (メモリ上の TOML 文字列)
    static std::string Serialize(const scene::Scene& scene);
    static bool        Deserialize(scene::Scene& scene, const std::string& toml);
};

} // namespace fbzz::editor
