/// @file    Ref.hpp
/// @brief   型安全なオブジェクト参照ハンドル。
/// @author  Hasegawa Jin
/// @date    2026-06-29
///
/// 設計意図 (WHY):
/// 従来スクリプトはオブジェクト参照を std::string 名で持ち、毎回 ResolveOwner() /
/// FindTarget() のような手書き解決 (scene.Find + 親階層ウォーク) を重複実装していた。
/// リネームで黙って壊れ、AI から見て「参照の正解」が一意でなかった。
///
/// Ref<T> は EntityRef (= シリアライズされる EntityID) を内包し、所有 Script 経由で
/// GameObject* / Script 派生型* / コンポーネント* を 1 ステップで解決する。
/// FBZZ_REF マクロで宣言すると Inspector にドラッグ&ドロップスロットが出る
/// (EntityID 用 UI を流用)。これにより参照は ID で保持され、リネーム耐性を持つ。
///
/// 使用例:
/// FBZZ_REF(GameObject, owner, "Owner")           // 任意の GameObject
/// FBZZ_REF(SwordTrailComponent, trail, "Trail")  // 型付きスクリプト参照
/// FBZZ_REF(LightComponent, lamp, "Lamp")         // コンポーネント参照
/// ...
/// if (trail) trail->PlayBloodSpray();            // 解決は内部で自動
/// if (lamp)  lamp->intensity = 3.0f;
///
/// T にコンポーネントを指定する場合、その型のヘッダーを include しておくこと
/// (解決は GetComponent<T> なので完全型が要る)。
#pragma once
#include <Engine/Scene/EntityRef.hpp>
#include <type_traits>

namespace fbzz::scene {

class Script;
class GameObject;

template<typename T = GameObject>
struct Ref {
    // シリアライズ対象。Inspector でアサインされた EntityID を保持する。
    EntityRef ref{};
    // 解決に使う所有 Script。FBZZ_REF が { this } で初期化する。シリアライズしない。
    Script* owner = nullptr;

    Ref() = default;
    explicit Ref(Script* o) : owner(o) {}

    // 参照先 GameObject を解決する。未アサイン / 破棄済みなら nullptr。
    GameObject* object() const;
    // 参照先を T* として解決する。T=GameObject はそのまま、T が Script 派生なら GetScript<T>。
    T* Get() const;

    T* operator->() const { return Get(); }
    T& operator*()  const { return *Get(); }
    explicit operator bool() const { return Get() != nullptr; }

    bool operator==(const Ref& o) const { return ref == o.ref; }

    // 明示的にアサインを解除する。
    void Clear() { ref = EntityRef{}; }
    bool IsAssigned() const { return ref.IsValid(); }
};

// メソッド実体は Script / ScriptSceneProxy が完全型になる Script.hpp 末尾で定義する
// (owner->scene へのアクセスと GetScript<T> のテンプレート実体化のため)。

} // namespace fbzz::scene
