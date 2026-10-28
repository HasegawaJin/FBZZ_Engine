/// @file    ManagedObjectControls.hpp
/// @brief   managed オブジェクト制御と反復階層検索を既存 Scene へ接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {
class Scene;
class GameObject;
namespace managed_detail {
class ManagedObjectControls final {
public:
    /// @note Runtime が live callback・Scene 寿命を検証する。read の text は未使用なら空にする。
    static ManagedBridgeStatus Read(GameObject& target, uint32_t selector, ManagedBridgeReply& reply, std::string& text);
    static ManagedBridgeStatus Write(GameObject& target, uint32_t selector, int64_t value, std::string_view text);
    /// @note pending removal は read から即時に隠し、同じ型の Add は Cleanup が除去するまで拒否する。
    static ManagedBridgeStatus Component(GameObject& target, uint32_t kind, ManagedBridgeOp op, bool& exists, uint64_t* lifetime = nullptr);
    static bool IsPendingComponentRemoval(const GameObject& target, uint32_t kind);
    /// @note local を維持した親変更を子孫の world pose まで検証し、失敗時は階層・Transform を未変更にする。
    static bool SetParent(GameObject& target, GameObject* parent);
    /// @note callback と physics 配送の終了後、Scene の破棄 queue を処理する前に呼ぶ。
    static void FlushScriptRemovals(Scene& scene);
    /// @note scope は 0=Scene、1=直下、2=子孫、3=Prefab source 部位（root 自身を含む）。順序は Hierarchy 深さ優先。
    /// @return 循環・無効リンクを全件検証し、失敗時の results は未変更。
    static ManagedBridgeStatus Query(Scene& scene, GameObject* root, uint32_t scope, std::string_view name,
        std::string_view tag, std::string_view part, bool includeInactive, std::vector<GameObject*>& results);
};
} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
