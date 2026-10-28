/// @file    ManagedServicesBridge.hpp
/// @brief   型付き managed facade から既存 native ScriptProxy を呼ぶ検証境界。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace fbzz::scene {
class Scene;
class Script;
class GameObject;
namespace managed_detail {

/// @note object は呼出中だけ有効な非所有参照。Runtime が opaque ID と GUID へ変換する。
struct ManagedServiceResult {
    std::vector<float> values;
    std::vector<std::string> text;
    /// @note SaveStore の既存 string だけを同期 reply encoding まで借用する。fallback/一時文字列は text が所有する。
    std::string_view borrowedText;
    bool hasBorrowedText = false;
    /// @note 同期 read 応答だけが Animator の既存配列を借用する。Runtime が復号を終えるまで callback を配送しない。
    std::span<const std::pair<std::string, float>> blendWeights;
    int64_t integer = 0;
    GameObject* object = nullptr;
};

/// @note native サービスの実装を再利用する。main thread・live Play scope・Scene/target の検証は Runtime が担当する。
class ManagedServicesBridge {
public:
    ManagedServicesBridge();
    ~ManagedServicesBridge();
    ManagedServicesBridge(const ManagedServicesBridge&) = delete;
    ManagedServicesBridge& operator=(const ManagedServicesBridge&) = delete;
    /// @note input は relatedID:u64, integer:i64, floatCount:u32, stringCount:u32, float32[], length+UTF8[]。
    /// @note flags bit0 は読み取り。selector の分類と一致しない要求を拒否し、変更要求を再送しない。
    /// @return 検証失敗は result と対象を未変更。Component 不在は NOT_FOUND。
    ManagedBridgeStatus Run(Scene& scene, Script& caller, GameObject& target, GameObject* related,
        const ManagedBridgeRequest& request, ManagedServiceResult& result, bool fixedUpdate, bool collisionCallback = false);
    /// @note AudioManager 生存中の明示的な owner 解放で呼ぶ。caller は実 owner Script の非所有参照。
    void CancelOwner(uint64_t ownerId, Script* caller = nullptr);
    void CancelCursorOwner(uint64_t ownerId);
    /// @return Cursor 要求の実所有 Script ID。malformed packet は false。
    static bool CursorOwner(const ManagedBridgeRequest& request, uint64_t& ownerId);
    /// @pre その世代の live adapter がすべて退役済みで、AudioManager が生存していること。
    void RetireGeneration(uint64_t generation);
    /// @note 同期 callback 終了時に未 commit の受信値だけを廃棄する。
    void EndCall(uint64_t call);
    ManagedBridgeStatus ReadMaterialPacket(Scene& scene, Script& caller, GameObject& target,
        const ManagedBridgeRequest& request, ManagedBridgeReply& reply);
    ManagedBridgeStatus RunJointPacket(Scene& scene, GameObject& target, const ManagedBridgeRequest& request,
        ManagedServiceResult& result, ManagedBridgeStatus (*resolver)(void*, uint64_t, GameObject*&), void* context, bool fixed);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
