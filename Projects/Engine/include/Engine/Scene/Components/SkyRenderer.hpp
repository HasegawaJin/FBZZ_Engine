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
    }
};

} // namespace fbzz::scene
