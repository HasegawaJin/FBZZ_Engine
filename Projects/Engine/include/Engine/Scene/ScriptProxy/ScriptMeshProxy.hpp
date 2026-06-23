// FBZZ Engine
// ScriptMeshProxy.hpp | fbzz::scene
// Script から MeshRenderer / SkinnedMeshRenderer を操作するショートハンド。
// MeshRenderer と SkinnedMeshRenderer のどちらが付いていても同一 API で有効/無効を切り替えられる。
#pragma once

#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptMeshProxy {
    Script* script = nullptr;

    // MeshRenderer または SkinnedMeshRenderer の enabled を切り替える。両方あれば両方を変更する。
    void SetEnabled(bool enabled) const;
    // どちらか一方でも enabled なら true を返す。
    bool IsEnabled() const;

    // --- MeshRenderer 専用 ---
    // "primitive:cube" / "models/foo.fbx:0" 形式のパスを設定し meshDirty を立てる。
    void SetMeshPath(std::string_view path) const;

    // --- SkinnedMeshRenderer 専用 ---
    // modelPath を変更する。変更後は RenderSystem が再ロードする。
    void SetModelPath(std::string_view path) const;
};

} // namespace fbzz::scene
