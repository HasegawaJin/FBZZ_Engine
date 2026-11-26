/// @file    GameObject.hpp
/// @brief   Unity ライクな OOP ラッパー。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note  Scene が unique_ptr で所有し、ComponentArray への入口を提供する。
/// @note  親子関係は EntityID で持ち、TransformSystem が world 値を更新する。
#pragma once
#include <Engine/Api.hpp>
#include "Engine/Scene/References/Entity.hpp"
#include "Engine/Scene/Hierarchy/Transform.hpp"
#include "ManagedBridgeAbi.hpp"
#include <bitset>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

class Scene;
namespace managed_detail { class ManagedObjectControls; }

class GameObject {
public:
    std::string name = "GameObject"; ///< 階層表示と名前検索に使う表示名。複数の個体で同じ名前を持てる。
    std::string tag = "Untagged"; ///< CompareTag と FindWithTag が使う分類名。
    std::string instanceId; ///< Scene が発行する個体の永続 UUID。改名後も IK などの参照解決に使う。
    /// @note 空なら非インスタンス。改名・移動時も同じ GUID の定義を Apply/Revert が参照する。
    std::string prefabAssetPath; ///< 出所プレファブのアセット参照。GUID が正本で、旧データは Assets 相対パスも受け付ける。
    std::string prefabSourceId; ///< 出所プレファブ内の元オブジェクトの instanceId。階層を照合して個別差分を計算する。
    /// @see Docs/design/prefab-safety.md
    std::string prefabSourceSnapshot; ///< ルートが最後に適用したプレファブ定義の TOML。個別差分を測る基準。
    int layer = 0; ///< 描画・照会でレイヤーマスクを適用するときに使うレイヤー番号。

    /// @brief システムが実行時に作った GO か (VFX Graph のノード実体・Water splash 等)。生成側が必ず立てる。
    /// @note 名前 "__" 接頭辞での判別をやめ意図を型で持たせた。SceneSerializer が保存対象から除外し、Hierarchy も既定で隠す。
    [[nodiscard]] bool IsRuntimeGenerated() const { return m_runtimeGenerated; }
    FBZZ_ENGINE_API void SetRuntimeGenerated(bool generated);

    Transform transform; ///< 個体が直接所有する位置・回転・スケール。階層同期に使う。

    /// @note  Unity: SetActive / activeSelf / activeInHierarchy
    FBZZ_ENGINE_API void SetActive(bool active);
    FBZZ_ENGINE_API bool activeSelf()        const;
    FBZZ_ENGINE_API bool activeInHierarchy() const;

    /// @note  Unity: CompareTag
    FBZZ_ENGINE_API bool CompareTag(const std::string& t) const;

    /// @note  Unity: AddComponent<T> / GetComponent<T>
    /// @note  template 本体は Scene.hpp の末尾で定義する (Scene が完全型である必要があるため)
    template<typename T> T& AddComponent(T component = {});
    template<typename T> T* GetComponent();
    template<typename T> void RemoveComponent();
    template<typename T> T* GetScript();

    /// @note  Unity: transform.SetParent / childCount / GetChild
    FBZZ_ENGINE_API void        SetParent(GameObject& parent);
    FBZZ_ENGINE_API bool        SetParent(GameObject* parent);
    FBZZ_ENGINE_API bool        ClearParent();
    FBZZ_ENGINE_API bool        IsDescendantOf(const GameObject& ancestor) const;
    FBZZ_ENGINE_API GameObject* GetParent()         const;
    FBZZ_ENGINE_API int         GetChildCount()     const;
    FBZZ_ENGINE_API GameObject* GetChild(int index) const;

    /// @note  自分を含む部分木を子の表示順に深さ優先で検索する。非アクティブも含む。
    /// @note  同名は最初の1件、未検出は nullptr。戻り値は Scene が所有する非所有参照。
    [[nodiscard]] FBZZ_ENGINE_API GameObject* FindInSubtree(std::string_view objectName);

    /// @brief Unity: transform.GetSiblingIndex / SetSiblingIndex
    /// @note 兄弟内の表示順 (Hierarchy の並び)。親がいない場合はルート同士の並び順を指す。Hierarchy パネルのドラッグ並べ替え (挿入ライン) に使う。
    FBZZ_ENGINE_API int  GetSiblingIndex() const;
    FBZZ_ENGINE_API bool SetSiblingIndex(int index);

    /// @note  Unity: GameObject.Find / FindWithTag / FindObjectsOfType (static)
    /// @note Unity と同じく既定では親ごと無効化された GameObject を返さない。FindByGuid は参照解決用なので無効な物も返す。
    FBZZ_ENGINE_API static GameObject*              Find(const std::string& n, bool includeInactive = false);
    FBZZ_ENGINE_API static GameObject*              FindByGuid(const std::string& guid);
    FBZZ_ENGINE_API static GameObject*              FindWithTag(const std::string& t, bool includeInactive = false);
    template<typename T>
    static std::vector<GameObject*> FindObjectsOfType();

    /// @note  Unity: Object.Destroy(go, delay)
    /// @note  delay=0 → 次フレーム末尾で削除 / delay>0 → 毎フレーム減算後に削除
    FBZZ_ENGINE_API static void Destroy(GameObject& go, float delay = 0.0f);

    [[nodiscard]] Scene* GetScene() const { return m_scene; }
    FBZZ_ENGINE_API bool     IsValid() const;
    EntityID GetID()   const { return m_id; }

private:
    EntityID m_id = EntityID::INVALID; ///< 所属 Scene 内の世代付き識別子。未所属なら無効。
    bool m_isActive = true; ///< 親の状態を含まない個体自身の有効フラグ。
    EntityID m_parent = EntityID::INVALID; ///< 親の Scene 内識別子。ルートなら無効。
    std::vector<EntityID> m_children; ///< 直下の子の Scene 内識別子。Hierarchy の表示順で保持する。
    Scene* m_scene = nullptr; ///< この個体を所有する Scene の非所有参照。未所属なら null。
    std::size_t m_sceneStorageIndex = 0; ///< Scene の所有位置。破棄時の全検索を避け、並べ替えと一括整理で更新する。
    bool m_destroying = false; ///< Scene による破棄処理が進行しているか。
    std::bitset<static_cast<std::size_t>(ManagedNativeComponentKind::COUNT)> m_pendingManagedComponentRemovals; ///< Cleanup で確定する Managed 側からの内蔵コンポーネント削除要求。

    bool m_runtimeGenerated = false; ///< 実行時生成物として保存と標準の階層表示から除外するか。
    friend class Scene;
    friend class managed_detail::ManagedObjectControls;
};

} // namespace fbzz::scene
