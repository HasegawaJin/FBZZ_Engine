/// @file    ScriptSceneProxy.hpp
/// @brief   Script から Scene / GameObject 操作へ転送するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/PrefabRef.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

class GameObject;
class Script;

struct ScriptSceneProxy {
    Script* script = nullptr;

    /// @name 検索 (Unity の GameObject.Find / FindWithTag / FindObjectsOfType と同じ規則)
    /// @note 既定では親ごと無効化された GameObject を返さない。無効にしておいて後で有効化する物 (HUD・ポーズ画面など) を探すときは includeInactive = true を渡す。
    /// @{
    GameObject* Find(std::string_view name, bool includeInactive = false) const;
    GameObject* FindWithTag(std::string_view tag, bool includeInactive = false) const;
    template<typename T> GameObject* FindObjectOfType(bool includeInactive = false) const;
    template<typename T> std::vector<GameObject*> FindObjectsOfType(bool includeInactive = false) const;
    /// @}
    GameObject* Self() const;
    GameObject* GetGameObject(EntityID id) const;
    GameObject* GetMainCameraObject() const;
    GameObject& Create(std::string_view name = "GameObject") const;
    void Destroy(GameObject& go, float delay = 0.0f) const;
    void DestroySelf(float delay = 0.0f) const;
    template<typename T> T* GetScript() const;
    template<typename T> T* GetScript(GameObject& go) const;
    template<typename T> T* GetScript(GameObject* go) const;  ///< nullptr 安全なポインタ版
    /// EntityID から直接 Script を取得する。Inspector でアサインした参照に使う。
    template<typename T> T* GetScript(EntityID id) const;
    template<typename T> T* GetComponent() const;
    /// 別 GameObject のコンポーネント。GetScript の GameObject 版と対になる。部位ごとに
    /// GameObject が分かれた構成では «自分の» コンポーネントだけでは足りない。Ref<T> が
    /// コンポーネント型を解決するのにも使う。
    template<typename T> T* GetComponent(GameObject& go) const;
    template<typename T> T* GetComponent(GameObject* go) const;  ///< nullptr 安全なポインタ版
    template<typename T> T& GetOrAddComponent() const;
    template<typename T> T& RequireComponent() const;
    bool IsActiveAndEnabled() const;
    bool isActiveAndEnabled() const { return IsActiveAndEnabled(); }
    /// 遷移要求。未登録のシーン名なら false を返す (演出を巻き戻す判断に使う)。
    bool LoadScene(std::string_view name) const;
    std::string GetSceneName() const;
    float GetTerrainHeightAt(const math::Vector3& worldPos) const;
    math::Vector3 GetTerrainNormalAt(const math::Vector3& worldPos) const;
    float GetWaterSurfaceHeight(const math::Vector3& worldPos, float time) const;

    /// 自 GO の name / tag ショートハンドプロパティ
    std::string GetName() const;
    void        SetName(const std::string& n) const;
    std::string GetTag() const;
    void        SetTag(const std::string& t) const;

    __declspec(property(get=GetName, put=SetName)) std::string name;
    __declspec(property(get=GetTag,  put=SetTag )) std::string tag;

    /// Prefab をインスタンス化して最初のルート GO を返す。失敗時は nullptr。
    GameObject* Instantiate(const PrefabRef& prefab) const;
    GameObject* Instantiate(const std::string& prefabPath) const;
    /// init コールバック付きオーバーロード — 生成直後の GO に初期値を差し込むのに使う。
    GameObject* Instantiate(const PrefabRef& prefab,
                            std::function<void(GameObject&)> init) const;
    GameObject* Instantiate(const std::string& prefabPath,
                            std::function<void(GameObject&)> init) const;

    /// @name オブジェクトプール
    /// @{
    /// Instantiate / Destroy の代わりに使う「借りて返す」版。弾・ヒットエフェクト・ダメージ
    /// 数字のように高頻度で出し入れするものは必ずこちら。
    /// @note Instantiate は .prefab 読み込みから TOML 再構築、シーン全体の参照張り直しまで
    ///       走る重い経路で、GameObject 配列再確保により既存の GameObject* も無効化される。
    ///       Spawn は 2 回目以降この経路を通らない。
    GameObject* Spawn(const PrefabRef& prefab,
                      const math::Vector3& position,
                      const math::Quaternion& rotation) const;
    GameObject* Spawn(const std::string& prefabPath,
                      const math::Vector3& position,
                      const math::Quaternion& rotation) const;
    /// 自身の現在位置・回転で出す簡易版。
    GameObject* Spawn(const PrefabRef& prefab) const;

    /// プールへ返す。プレファブ由来でない GO は false を返すので、
    /// その場合は Destroy へフォールバックすること。
    bool Despawn(GameObject& gameObject) const;
    /// 自分自身を返す (弾スクリプトが寿命切れで自分を仕舞う用途)。
    bool DespawnSelf() const;

    /// ロード画面などで事前生成しておく。戻り値は実際に作れた数。
    int Prewarm(const PrefabRef& prefab, int count) const;
    int Prewarm(const std::string& prefabPath, int count) const;
    /// 待機中の数 (チューニング用)。
    [[nodiscard]] size_t PooledCount(const std::string& prefabPath) const;
    /// @}
};

} // namespace fbzz::scene
