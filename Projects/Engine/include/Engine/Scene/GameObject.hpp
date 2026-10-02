/// @file    GameObject.hpp
/// @brief   Unity ライクな OOP ラッパー。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note  Scene が unique_ptr で所有し、ComponentArray への入口を提供する。
/// @note  親子関係は EntityID で持ち、TransformSystem が world 値を更新する。
#pragma once
#include "Entity.hpp"
#include "Transform.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

class Scene;

class GameObject {
public:
    /// @note  Unity: gameObject.name / .tag (直接変数)
    std::string name       = "GameObject";
    std::string tag        = "Untagged";
    /// @note  クロスオブジェクト参照 (IK Pole/Target 等) はリネームに耐えられるよう
    /// @note  UUID v4 を永続識別子として持つ。Scene::CreateGameObject で自動生成される。
    std::string instanceId;
    /// @brief Prefab インスタンスの出所参照。GUID を正本とし、旧 Assets 相対パスも読み込める。
    /// @note 空なら非インスタンス。改名・移動時も同じ GUID の定義を Apply/Revert が参照する。
    std::string prefabAssetPath;
    /// @brief この GO が .prefab 内のどのオブジェクトから作られたか (プレファブ側の instanceId)。空文字列 = プレファブ由来でない。
    /// @note instanceId はインスタンス化のたびに新規採番されるため、これで対応する子を突き合わせ、プロパティ単位の override を計算する。階層内の全 GO に入る。
    std::string prefabSourceId;
    /// @note ルートだけが最後に適用した元定義の TOML を保持する。個別変更はこの版との差分で判定する。
    /// @see Docs/design/prefab-safety.md
    std::string prefabSourceSnapshot;
    int layer = 0;

    /// @brief システムが実行時に作った GO か (VFX Graph のノード実体・Water splash 等)。生成側が必ず立てる。
    /// @note 名前 "__" 接頭辞での判別をやめ意図を型で持たせた。SceneSerializer が保存対象から除外し、Hierarchy も既定で隠す。
    bool runtimeGenerated = false;

    /// @note  Unity: gameObject.transform (常に存在。ComponentArray には入れない)
    Transform transform;

    /// @note  Unity: SetActive / activeSelf / activeInHierarchy
    void SetActive(bool active);
    bool activeSelf()        const;
    bool activeInHierarchy() const;

    /// @note  Unity: CompareTag
    bool CompareTag(const std::string& t) const;

    /// @note  Unity: AddComponent<T> / GetComponent<T>
    /// @note  template 本体は Scene.hpp の末尾で定義する (Scene が完全型である必要があるため)
    template<typename T> T& AddComponent(T component = {});
    template<typename T> T* GetComponent();
    template<typename T> void RemoveComponent();
    template<typename T, typename... Args> T& AddScript(Args&&... args);
    template<typename T> T* GetScript();

    /// @note  Unity: transform.SetParent / childCount / GetChild
    void        SetParent(GameObject& parent);
    bool        SetParent(GameObject* parent);
    bool        ClearParent();
    bool        IsDescendantOf(const GameObject& ancestor) const;
    GameObject* GetParent()         const;
    int         GetChildCount()     const;
    GameObject* GetChild(int index) const;

    /// @note  自分を含む部分木を子の表示順に深さ優先で検索する。非アクティブも含む。
    /// @note  同名は最初の1件、未検出は nullptr。戻り値は Scene が所有する非所有参照。
    [[nodiscard]] GameObject* FindInSubtree(std::string_view objectName);

    /// @brief Unity: transform.GetSiblingIndex / SetSiblingIndex
    /// @note 兄弟内の表示順 (Hierarchy の並び)。親がいない場合はルート同士の並び順を指す。Hierarchy パネルのドラッグ並べ替え (挿入ライン) に使う。
    int  GetSiblingIndex() const;
    bool SetSiblingIndex(int index);

    /// @note  Unity: GameObject.Find / FindWithTag / FindObjectsOfType (static)
    /// @note Unity と同じく既定では親ごと無効化された GameObject を返さない。FindByGuid は参照解決用なので無効な物も返す。
    static GameObject*              Find(const std::string& n, bool includeInactive = false);
    static GameObject*              FindByGuid(const std::string& guid);
    static GameObject*              FindWithTag(const std::string& t, bool includeInactive = false);
    template<typename T>
    static std::vector<GameObject*> FindObjectsOfType();

    /// @note  Unity: Object.Destroy(go, delay)
    /// @note  delay=0 → 次フレーム末尾で削除 / delay>0 → 毎フレーム減算後に削除
    static void Destroy(GameObject& go, float delay = 0.0f);

    [[nodiscard]] Scene* GetScene() const { return m_scene; }
    bool     IsValid() const;
    EntityID GetID()   const { return m_id; }

private:
    EntityID              m_id       = EntityID::INVALID;
    bool                  m_isActive = true;
    EntityID              m_parent   = EntityID::INVALID;
    std::vector<EntityID> m_children;
    Scene*                m_scene    = nullptr; ///< @note 非所有参照

    friend class Scene;
};

} /// @note namespace fbzz::scene
