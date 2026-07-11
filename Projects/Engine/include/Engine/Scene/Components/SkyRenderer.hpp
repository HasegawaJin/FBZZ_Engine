// FBZZ Engine
// SkyRenderer.hpp | fbzz::scene
// 大気散乱スカイドーム設定コンポーネント
// 太陽方向・散乱係数など、空描画に必要な値を Scene に保持する。
// 描画順やシェーダー実体は RenderSystem / Renderer が扱う。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct SkyRenderer {
    // Rayleigh 散乱係数 (m⁻¹)。波長ごとに異なり、空が青い理由となる。
    // デフォルト値は地球の標準大気に基づく一般的な近似値。
    math::Vector3 rayleighScattering = { 5.8e-3f, 13.5e-3f, 33.1e-3f };
    float         mieScattering      = 21.0e-4f;
    float         sunIntensity       = 20.0f;
    // WHAT: シェーダーは km 単位の半径を使う。地球以外のスケールでも空を調整できるよう Scene に保持する。
    float         planetRadius       = 6371.0f;
    float         atmosphereRadius   = 6471.0f;
    // Henyey-Greenstein 位相関数の非対称パラメーター (0=等方散乱, 1=完全前方散乱)。
    // 0.76 は大気中のエアロゾルに典型的な値で、太陽周辺のグローを再現する。
    float         mieG               = 0.76f;
    bool          enabled            = true;

    // 太陽/月ディスクの描画設定は SunMoonRenderer が所有する (本構造体は持たない)。

    // ── 昼夜の色/強度カーブ ───────────────────────────────────────────────────
    // WHY: 太陽の「向き」は DirectionalLight の transform が唯一のソース。dayNightEnabled のときは
    //      その光源の仰角 (太陽の高さ) から色・強度の昼夜遷移だけを駆動する。ライトを回せば
    //      空・月 (アンチ太陽)・ライティング・空連動 IBL がまとめて動く。
    bool          dayNightEnabled = false;
    math::Vector3 dayColor        = { 1.0f, 0.98f, 0.95f }; // 日中の太陽光色
    math::Vector3 sunsetColor     = { 1.0f, 0.5f, 0.2f };   // 日の出/日没の暖色
    math::Vector3 nightColor      = { 0.1f, 0.15f, 0.3f };  // 夜の薄明 (月光相当)
    float         dayIntensity    = 1.5f;                   // 日中のライト強度
    float         nightIntensity  = 0.1f;                   // 夜のライト強度

    // ── 雲シャドウ (Phase C: CloudShadow) ─────────────────────────────────────
    // WHY: 雲密度を地表へ投影した「まだら影」を全 Lit シェーダーの影係数に乗算する。
    //      専用 CloudShadowMap を焼かず、ワールド XZ で手続き型 FBM を評価する解析版
    //      (低コスト・パス追加不要)。視認上の空の雲とは別系統で、地表の光のゆらぎを演出する。
    float         cloudShadowStrength = 0.0f;   // 0=無効。0.5〜0.8 で自然なまだら影
    float         cloudShadowCoverage = 0.5f;   // 雲量 (大きいほど影が広い) [0,1]
    float         cloudShadowScale    = 0.02f;  // world→ノイズ UV スケール (小さいほど大きな雲影)
    float         cloudShadowSpeed    = 1.0f;   // 流れる速さ

    const char* GetTypeName() const { return "Sky Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("rayleighScattering", rayleighScattering);
        r.Field("mieScattering", mieScattering);
        r.Field("sunIntensity", sunIntensity);
        r.Field("planetRadius", planetRadius);
        r.Field("atmosphereRadius", atmosphereRadius);
        r.Field("mieG", mieG);
        r.Field("dayNightEnabled", dayNightEnabled);
        r.Field("dayColor", dayColor);
        r.Field("sunsetColor", sunsetColor);
        r.Field("nightColor", nightColor);
        r.Field("dayIntensity", dayIntensity);
        r.Field("nightIntensity", nightIntensity);
        r.Field("cloudShadowStrength", cloudShadowStrength);
        r.Field("cloudShadowCoverage", cloudShadowCoverage);
        r.Field("cloudShadowScale", cloudShadowScale);
        r.Field("cloudShadowSpeed", cloudShadowSpeed);
    }
};

} // namespace fbzz::scene
