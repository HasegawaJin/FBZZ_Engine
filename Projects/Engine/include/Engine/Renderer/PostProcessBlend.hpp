// FBZZ Engine
// PostProcessBlend.hpp | fbzz::renderer
// ボリューム合成の共通プリミティブと、解決結果を描画設定へ流し込むヘルパー。
//
// WHY 補間規則をここに集約するか:
//   実際の合成は VolumeOverride の派生 27 種がそれぞれ行う。各クラスが
//   自前で「bool はどう混ぜるか」を決めると、効果ごとに挙動が食い違う。
//   規則を 1 か所に置き、全オーバーライドがこれを呼ぶ形にする。
#pragma once
#include <Engine/Renderer/RenderSettings.hpp>

namespace fbzz::renderer {

// t=0 で a、t=1 で b。
// WHY 端点を特別扱いするか: a + (b - a) * 1.0f は (b - a) の丸め誤差により
//     b と厳密には一致しない。「ボリュームの中に完全に入っているのに設定が
//     微妙に違う」という追跡困難な差異を防ぐため、端点を明示的に返す。
[[nodiscard]] float BlendFloat(float a, float b, float t);

// bool は連続量ではないため、重みが半分を超えた側を採用する。
// WHY 閾値方式か: Unity の Volume framework 相当のフィールド単位 override を
//     入れると、全パラメーターがラッパー型になりシェーダー転送まで波及する。
//     「ブレンド途中で ON/OFF が切り替わる」限界は残るが、オーバーライドを
//     リスト化したことで「そもそも触らない効果は素通しされる」ため実害は小さい。
[[nodiscard]] bool BlendBool(bool a, bool b, float t);

// 整数パラメーター (サンプル数・ステップ数) も閾値で切り替える。
// WHY 丸めた線形補間にしないか: steps 32 → 128 の途中で 51 のような
//     半端な値を経由しても品質上の意味がなく、GPU コストだけが読めなくなる。
[[nodiscard]] int BlendInt(int a, int b, float t);

// float[3] (色) をまとめて補間する。
void BlendColor3(const float (&a)[3], const float (&b)[3], float t, float (&out)[3]);

// 合成済みの VolumeSettings を RenderSettings の該当フィールドへ書き戻す。
// WHY RenderSettings が VolumeSettings を直接持たないか:
//      全 RenderPass が rs.ssr / rs.gtao のように参照しており、
//      入れ子に変えると描画コード全体を触ることになる。
//      「ボリュームで解決 → 描画用へ流し込む」の 1 方向だけに限定しておけば、
//      パス側は今までどおりのフラットな設定を読み続けられる。
void ApplyVolumeSettings(const VolumeSettings& volume, RenderSettings& out);

// 球状ボリュームの距離ウェイト。
//   distance <= radius - blendDistance : 1
//   distance >= radius                 : 0
//   その間                              : 線形に降下
[[nodiscard]] float PostProcessVolumeDistanceWeight(
    float distance, float radius, float blendDistance);

} // namespace fbzz::renderer
