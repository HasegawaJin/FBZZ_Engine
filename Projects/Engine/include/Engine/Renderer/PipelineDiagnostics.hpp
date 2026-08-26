/// @file PipelineDiagnostics.hpp
/// @brief 現在のパイプラインでは効かない設定を列挙する
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// WHY 要るか: パイプラインを切り替えると、有効にしてある設定の一部が黙って無視される。
//     SSAO を ON にしたまま Forward を選ぶと、チェックは入っているのに絵は変わらない。
//     ユーザーから見ると「設定が壊れている」としか読めないので、
//     「効かないこと」と「なぜか」と「どうすれば効くか」を明示する。
//
// WHY 判定をここへ集約するか: 実際の分岐は RenderSystem のパス登録側にあり、UI からは
//     見えない。同じ条件を UI にも書くと片方だけ更新され、「警告が出ないのに効かない」
//     という最悪の状態になる。述語 (RenderSettings::UsesGBuffer 等) を 1 つ決めて、
//     パス登録とこのファイルの両方がそれを呼ぶ形にしてある。
#pragma once
#include <string_view>
#include <vector>

namespace fbzz::renderer {

struct RenderSettings;

/// 有効にしてあるのに現在のパイプラインでは無視される設定 1 件。
struct InertSetting {
    /// UI に出ている名前 (VolumeOverride の表示名や Project Settings の項目名)。
    const char* label = nullptr;
    /// なぜ効かないか。
    const char* reason = nullptr;
    /// どうすれば効くか。
    const char* remedy = nullptr;
};

/// 現在の設定から「有効なのに無視されるもの」を列挙する。
/// 何も無ければ空を返す。
[[nodiscard]] std::vector<InertSetting> CollectInertSettings(const RenderSettings& settings);

/// VolumeOverride 1 件が現在のパイプラインで効くかを名前で判定する。
/// @param typeName VolumeOverride::GetTypeName() の値 ("SSR" / "GTAO" など)
/// @return 効くなら nullptr。効かないなら理由の文字列 (表示用・静的寿命)。
[[nodiscard]] const char* DescribeInertOverride(
    const RenderSettings& settings, std::string_view typeName);

} // namespace fbzz::renderer
