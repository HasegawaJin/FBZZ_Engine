/// @file    ComponentReflectionCodec.hpp
/// @brief   コンポーネントの TOML 保存・復元を Reflect() 1 本から導く。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note SceneSerializer は 57 種類のコンポーネントを手書きで読み書きし、うち 63/67 は既に Reflect() を持つ。«保存側・読み込み側・Reflect()» の 3 か所にフィールド表が分かれ、ParticleEmitter では実際に 44 フィールドがずれていた ── ずれても保存はできてしまい、エラーも警告も出ないまま AI バスと汎用 Inspector からだけ見えなくなる壊れ方をする。
/// @note toml++ の insert 既定 (先勝ち) に合わせて書く。後勝ちに変えると BeginField の戻し忘れがあったときの保存結果が変わるため、移行中は «既存ファイルと同じ結果» を優先する。
#pragma once
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <toml++/toml.hpp>

namespace fbzz::scene {

/// @brief コンポーネント 1 個を Reflect() 経由で TOML テーブルへ書き出す。
template<class T>
[[nodiscard]] toml::table SerializeReflected(T& component)
{
    toml::table table;
    util::TomlWriteReflector reflector(table, false);
    component.Reflect(reflector);
    return table;
}

/// @brief 逆。キーが欠けているフィールドは outComponent の値をそのまま残す。
/// @note 新しい項目を足した後も古いシーンが «その項目だけ既定値» で開く。
template<class T>
void DeserializeReflected(const toml::table& table, T& outComponent)
{
    util::TomlReadReflector reflector(table);
    outComponent.Reflect(reflector);
}

/// @brief GameObject に付いていれば goTable[key] へ書き出す。
template<class T>
void WriteComponentReflected(GameObject& go, toml::table& goTable, const char* key)
{
    if (auto* component = go.GetComponent<T>())
        goTable.insert(key, SerializeReflected(*component));
}

/// @brief goTable[key] があれば AddComponent して読み込む。
/// @note 既定値は T{} から始まる。手書き経路が value_or(...) に書いていた既定と食い違わないこと ── そこがずれると «キーの無い古いシーン» だけ挙動が変わる。
template<class T>
void ReadComponentReflected(GameObject& go, const toml::table& goTable, const char* key)
{
    const auto* table = goTable[key].as_table();
    if (table == nullptr) return;
    T component{};
    DeserializeReflected(*table, component);
    go.AddComponent<T>(component);
}

} // namespace fbzz::scene
