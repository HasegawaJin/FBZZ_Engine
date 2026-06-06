// FBZZ Engine
// ScriptTrailProxy.hpp | fbzz::scene
// Script から TrailComponent を操作するショートハンド
#pragma once

#include <Math/Vector4.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::scene {

class Script;
enum class TrailAlignment : uint8_t;

struct ScriptTrailProxy {
    Script* script = nullptr;

    // SetEnabled — Trail の記録と描画を切り替える。
    // WHY: 攻撃中だけ剣閃を出す、ダッシュ中だけ残像を出す、といった用途を Script から簡潔に書ける。
    void SetEnabled(bool enabled, bool clearWhenDisabled = true) const;

    // Clear — 現在の制御点をすべて破棄し、次フレームから新しい軌跡として開始する。
    void Clear() const;

    // SetDuration — 軌跡が残る秒数を設定する。
    void SetDuration(float seconds) const;

    // SetSampling — 制御点の追加頻度と最小移動距離を設定する。
    void SetSampling(float sampleInterval, float minVertexDist) const;

    // SetWidth — 最新点と最古点の幅を設定する。
    void SetWidth(float start, float end) const;

    // SetColor — 最新点と最古点の色を設定する。
    void SetColor(const math::Vector4& start, const math::Vector4& end) const;

    // SetTexture — Trail テクスチャと UV スクロールを設定する。
    void SetTexture(std::string_view texturePath, float uvTiling = 1.0f, float uvScrollSpeed = 0.0f) const;

    // SetAlignment — CameraFacing / WorldUp の向きを切り替える。
    void SetAlignment(TrailAlignment alignment) const;

    // SetCameraFacing — カメラに向くリボン。剣閃・弾道・残像向け。
    void SetCameraFacing() const;

    // SetWorldUp — ワールド上方向を基準にするリボン。タイヤ跡・地面エフェクト向け。
    void SetWorldUp() const;

    // SetSmoothSubdivisions — Catmull-Rom 補間数を設定する。0 なら補間なし。
    void SetSmoothSubdivisions(int subdivisions) const;
};

} // namespace fbzz::scene
