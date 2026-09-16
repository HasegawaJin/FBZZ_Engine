/// @file    ComponentReflectionCodec.hpp
/// @brief   コンポーネントの TOML 保存・復元を Reflect() 1 本から導く。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// WHY 手書きをやめるか:
///   SceneSerializer は 57 種類のコンポーネントを手書きで読み書きしており、
///   その大半 (63/67) は既に Reflect() を持っている。同じフィールド表が
///   «保存側・読み込み側・Reflect()» の 3 か所に分かれている状態で、
///   ParticleEmitter では実際に 44 フィールドがずれていた。
///   ずれても保存はできてしまい、AI バスと汎用 Inspector からだけ見えなくなる
///   ── エラーも警告も出ないので、存在自体に気付けない壊れ方をする。
///
/// WHY 「先勝ち」で書くか:
///   既存のシーン直列化は toml++ の insert 既定 (先勝ち) を前提に組まれている。
///   後勝ちに変えると、BeginField の戻し忘れがあったときの保存結果が変わる。
///   移行中は «既存ファイルと同じ結果» が最優先なので、揃えておく。
#pragma once
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <toml++/toml.hpp>

namespace fbzz::scene {

/// コンポーネント 1 個を Reflect() 経由で TOML テーブルへ書き出す。
template<class T>
[[nodiscard]] toml::table SerializeReflected(T& component)
{
    toml::table table;
    util::TomlWriteReflector reflector(table, /*overwriteDuplicates=*/false);
    component.Reflect(reflector);
    return table;
}

/// 逆。キーが欠けているフィールドは outComponent の値をそのまま残すので、
/// 新しい項目を足した後も古いシーンが «その項目だけ既定値» で開く。
template<class T>
void DeserializeReflected(const toml::table& table, T& outComponent)
{
    util::TomlReadReflector reflector(table);
    outComponent.Reflect(reflector);
}

/// GameObject に付いていれば goTable[key] へ書き出す。
template<class T>
void WriteComponentReflected(GameObject& go, toml::table& goTable, const char* key)
{
    if (auto* component = go.GetComponent<T>())
        goTable.insert(key, SerializeReflected(*component));
}

/// goTable[key] があれば AddComponent して読み込む。
/// @note 既定値は T{} から始まる。手書き経路が value_or(...) に書いていた既定と
///       食い違わないこと ── そこがずれると «キーの無い古いシーン» だけ挙動が変わる。
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
