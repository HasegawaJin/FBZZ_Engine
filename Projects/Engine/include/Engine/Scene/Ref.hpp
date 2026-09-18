/// @file    Ref.hpp
/// @brief   型安全なオブジェクト参照ハンドル。
/// @author  Hasegawa Jin
/// @date    2026-06-29
///
/// @note Ref<T> は EntityRef (シリアライズされる EntityID) を保持し、所有 Script 経由で
///       GameObject* / Script 派生型* / コンポーネント* を解決する。FBZZ_REF で宣言すると
///       Inspector にドラッグ&ドロップスロットが出て、参照は ID 保持のためリネームに耐える。
/// @note T にコンポーネントを指定する場合、解決は GetComponent<T> のため完全型の include が要る。
#pragma once
#include <Engine/Scene/EntityRef.hpp>
#include <type_traits>

namespace fbzz::scene {

class Script;
class GameObject;

template<typename T = GameObject>
struct Ref {
    EntityRef ref{}; ///< シリアライズ対象。Inspector でアサインされた EntityID を保持する。
    Script* owner = nullptr; ///< 解決に使う所有 Script。FBZZ_REF が { this } で初期化する。シリアライズしない。

    Ref() = default;
    explicit Ref(Script* o) : owner(o) {}

    /// @brief 参照先 GameObject を解決する。
    /// @return 未アサイン / 破棄済みなら nullptr。
    GameObject* object() const;
    /// @brief 参照先を T* として解決する。T=GameObject はそのまま、T が Script 派生なら GetScript<T>。
    T* Get() const;

    T* operator->() const { return Get(); }
    T& operator*()  const { return *Get(); }
    explicit operator bool() const { return Get() != nullptr; }

    bool operator==(const Ref& o) const { return ref == o.ref; }

    /// @brief 明示的にアサインを解除する。
    void Clear() { ref = EntityRef{}; }
    bool IsAssigned() const { return ref.IsValid(); }
};

/// @note メソッド実体は Script / ScriptSceneProxy が完全型になる Script.hpp 末尾で定義する
///       (owner->scene へのアクセスと GetScript<T> のテンプレート実体化のため)。

} // namespace fbzz::scene
