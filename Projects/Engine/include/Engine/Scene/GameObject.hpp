// FBZZ Engine
// GameObject.hpp | fbzz::scene
// Unity ライクな OOP ラッパー
// Scene が unique_ptr で所有し、ComponentArray への入口を提供する。
// 親子関係は EntityID で持ち、TransformSystem が world 値を更新する。
#pragma once
#include "Entity.hpp"
#include "Transform.hpp"
#include <string>
#include <vector>

namespace fbzz::scene {

class Scene;

class GameObject {
public:
    // Unity: gameObject.name / .tag (直接変数)
    std::string name       = "GameObject";
    std::string tag        = "Untagged";
    // クロスオブジェクト参照 (IK Pole/Target 等) はリネームに耐えられるよう
    // UUID v4 を永続識別子として持つ。Scene::CreateGameObject で自動生成される。
    std::string instanceId;
    // Prefab インスタンスの出所アセットパス (Assets 起点の相対パス)。
    // WHY: Apply/Revert のために「このインスタンスがどのプレファブから生成されたか」を
    //      GO 自身に持たせる。空文字列 = Prefab 非インスタンス (通常の GO)。
    //      SceneSerializer が Save/Load で永続化し、PrefabSerializer::Instantiate が書き込む。
    std::string prefabAssetPath;
    // この GO が .prefab 内のどのオブジェクトから作られたか (プレファブ側の instanceId)。
    // WHY: インスタンス化のたびに instanceId は新規採番されるため、これが無いと
    //      「インスタンスの この子」と「プレファブの この子」を対応付けられない。
    //      対応が取れて初めて、プロパティ単位の差分 (override) を計算できる。
    //      ルートだけでなく階層内の全 GO に入る。空文字列 = プレファブ由来でない。
    std::string prefabSourceId;
    int layer = 0;

    // システムが実行時に作った GO (VFX Graph のノード実体・Water splash 等)。
    // WHY: これまでは名前を "__" で始めるという規約でシリアライズ除外を表現していたが、
    //      規約だと「見せる名前」と「保存するか」が同じ文字列に相乗りしてしまい、
    //      表示名を読みやすくした瞬間に保存対象へ戻るという壊れ方をする。
    //      意図を型で持たせることで、名前は純粋に表示のためだけに使えるようになる。
    // 効果は 2 つ:
    //   1. SceneSerializer が保存しない (ロード時のゾンビ GO 蓄積を防ぐ)
    //   2. Hierarchy が既定で隠す (1 エフェクト置くたびに十数行増えるのを防ぐ)
    // NOTE: 生成側が必ず立てる。
    bool runtimeGenerated = false;

    // Unity: gameObject.transform (常に存在。ComponentArray には入れない)
    Transform transform;

    // Unity: SetActive / activeSelf / activeInHierarchy
    void SetActive(bool active);
    bool activeSelf()        const;
    bool activeInHierarchy() const;

    // Unity: CompareTag
    bool CompareTag(const std::string& t) const;

    // Unity: AddComponent<T> / GetComponent<T>
    // template 本体は Scene.hpp の末尾で定義する (Scene が完全型である必要があるため)
    template<typename T> T& AddComponent(T component = {});
    template<typename T> T* GetComponent();
    template<typename T> void RemoveComponent();
    template<typename T, typename... Args> T& AddScript(Args&&... args);
    template<typename T> T* GetScript();

    // Unity: transform.SetParent / childCount / GetChild
    void        SetParent(GameObject& parent);
    bool        SetParent(GameObject* parent);
    bool        ClearParent();
    bool        IsDescendantOf(const GameObject& ancestor) const;
    GameObject* GetParent()         const;
    int         GetChildCount()     const;
    GameObject* GetChild(int index) const;

    // Unity: transform.GetSiblingIndex / SetSiblingIndex
    // 兄弟内の表示順 (Hierarchy の並び)。親がいない場合はルート同士の並び順を指す。
    // WHY: Hierarchy パネルのドラッグ並べ替え (挿入ライン) に必要。
    int  GetSiblingIndex() const;
    bool SetSiblingIndex(int index);

    // Unity: GameObject.Find / FindWithTag / FindObjectsOfType (static)
    static GameObject*              Find(const std::string& n);
    static GameObject*              FindByGuid(const std::string& guid);
    static GameObject*              FindWithTag(const std::string& t);
    template<typename T>
    static std::vector<GameObject*> FindObjectsOfType();

    // Unity: Object.Destroy(go, delay)
    // delay=0 → 次フレーム末尾で削除 / delay>0 → 毎フレーム減算後に削除
    static void Destroy(GameObject& go, float delay = 0.0f);

    bool     IsValid() const;
    EntityID GetID()   const { return m_id; }

private:
    EntityID              m_id       = EntityID::INVALID;
    bool                  m_isActive = true;
    EntityID              m_parent   = EntityID::INVALID;
    std::vector<EntityID> m_children;
    Scene*                m_scene    = nullptr; // 非所有参照

    friend class Scene;
};

} // namespace fbzz::scene
