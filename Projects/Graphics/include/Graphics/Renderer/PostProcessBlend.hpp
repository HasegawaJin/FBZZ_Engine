/// @file    PostProcessBlend.hpp
/// @brief   ボリューム合成の共通プリミティブと、解決結果を描画設定へ流し込むヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-08-12
/// @note 実際の合成は VolumeOverride の派生 27 種がそれぞれ行う。各クラスが自前で
/// @note       混ぜ方を決めると効果ごとに挙動が食い違うため、規則をここへ集約し全
/// @note       オーバーライドがこれを呼ぶ形にする。
#pragma once
#include <Graphics/Renderer/RenderSettings.hpp>

namespace fbzz::renderer {

/// @note t=0 で a、t=1 で b。
/// @note a + (b - a) * 1.0f は丸め誤差で b と厳密には一致しない。「ボリュームの中に完全に
/// @note       入っているのに設定が微妙に違う」という追跡困難な差異を防ぐため端点を明示的に返す。
[[nodiscard]] float BlendFloat(float a, float b, float t);

/// @note bool は連続量ではないため、重みが半分を超えた側を採用する。
/// @note Unity の Volume framework 相当のフィールド単位 override を入れると全パラメーターが
/// @note       ラッパー型になりシェーダー転送まで波及する。オーバーライドをリスト化したことで
/// @note       「触らない効果は素通しされる」ため、途中で ON/OFF が切り替わる限界の実害は小さい。
[[nodiscard]] bool BlendBool(bool a, bool b, float t);

/// @note 整数パラメーター (サンプル数・ステップ数) も閾値で切り替える。
/// @note steps 32 → 128 の途中で 51 のような半端な値を経由しても品質上の意味がなく、
/// @note       GPU コストだけが読めなくなるため丸めた線形補間にはしない。
[[nodiscard]] int BlendInt(int a, int b, float t);

/// @note float[3] (色) をまとめて補間する。
void BlendColor3(const float (&a)[3], const float (&b)[3], float t, float (&out)[3]);

/// @note 合成済みの VolumeSettings を RenderSettings の該当フィールドへ書き戻す。
/// @note 全 RenderPass が rs.ssr / rs.gtao のように参照しており、入れ子に変えると描画コード
/// @note       全体を触ることになる。「ボリュームで解決 → 描画用へ流し込む」の 1 方向に限定し、
/// @note       パス側は今までどおりフラットな設定を読み続けられるようにする。
void ApplyVolumeSettings(const VolumeSettings& volume, RenderSettings& out);

/// @note 球状ボリュームの距離ウェイト。
/// @note   distance <= radius - blendDistance : 1
/// @note   distance >= radius                 : 0
/// @note   その間                              : 線形に降下
[[nodiscard]] float PostProcessVolumeDistanceWeight(
    float distance, float radius, float blendDistance);

} /// @note namespace fbzz::renderer
