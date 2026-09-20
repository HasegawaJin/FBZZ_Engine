/// @file    ScriptTrailProxy.hpp
/// @brief   Script から TrailComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-06
#pragma once

#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::renderer { enum class TrailAlignment : uint8_t; enum class TrailUVMode : uint8_t; enum class TrailWidthEasing : uint8_t; }
namespace fbzz::scene {

class Script;
using renderer::TrailAlignment;
using renderer::TrailUVMode;
using renderer::TrailWidthEasing;

struct ScriptTrailProxy {
    Script* script = nullptr;

    /// @brief Trail の記録と描画を切り替える。攻撃中だけ剣閃を出す、ダッシュ中だけ残像を出す、
    /// @note といった用途を Script から簡潔に書ける。
    void SetEnabled(bool enabled, bool clearWhenDisabled = true) const;

    /// @note Clear — 現在の制御点をすべて破棄し、次フレームから新しい軌跡として開始する。
    void Clear() const;

    /// @note SetDuration — 軌跡が残る秒数を設定する。
    void SetDuration(float seconds) const;

    /// @brief Trail が保持する制御点数を設定する。剣閃のように短時間で大きく動く用途では
    /// @note Script 側から密度を調整したい。
    void SetMaxPoints(int maxPoints) const;

    /// @note SetSampling — 制御点の追加頻度と最小移動距離を設定する。
    void SetSampling(float sampleInterval, float minVertexDist) const;

    /// @note SetWidth — 最新点と最古点の幅を設定する。
    void SetWidth(float start, float end) const;

    /// @note SetWidthEasing — 幅フェードの補間曲線を設定する。
    void SetWidthEasing(TrailWidthEasing easing) const;

    /// @note SetColor — 最新点と最古点の色を設定する。
    void SetColor(const math::Vector4& start, const math::Vector4& end) const;

    /// @note SetMaterial — Trail 用 .mat アセットを指定する。空文字で materialPath を解除する。
    void SetMaterial(std::string_view materialPath) const;

    /// @note SetUVMode — U 座標を Stretch / Tile のどちらで生成するかを設定する。
    void SetUVMode(TrailUVMode mode) const;

    /// @note SetAlignment — CameraFacing / WorldUp の向きを切り替える。
    void SetAlignment(TrailAlignment alignment) const;

    /// @note SetCameraFacing — カメラに向くリボン。剣閃・弾道・残像向け。
    void SetCameraFacing() const;

    /// @note SetWorldUp — ワールド上方向を基準にするリボン。タイヤ跡・地面エフェクト向け。
    void SetWorldUp() const;

    /// @note SetSmoothSubdivisions — Catmull-Rom 補間数を設定する。0 なら補間なし。
    void SetSmoothSubdivisions(int subdivisions) const;

    /// @note SetAttachBone — SkinnedMeshRenderer の指定ボーン位置を Trail のサンプル原点にする。
    void SetAttachBone(std::string_view boneName, const math::Vector3& offset = math::Vector3::ZERO) const;

    /// @note ClearAttachBone — ボーン追従を解除し、GameObject 原点から Trail を発生させる。
    void ClearAttachBone() const;

    /// @note SetEnabled(false, false) で記録だけ止めた場合、enabled は false のまま
    /// @note 制御点が duration 経過まで残る。剣閃が完全に消えたかは GetPointCount() で見る。
    [[nodiscard]] bool  IsEnabled() const;
    [[nodiscard]] float GetDuration() const;
    /// @note 現在保持している制御点数。0 なら描画するものが残っていない。
    [[nodiscard]] int   GetPointCount() const;
};

} /// @note namespace fbzz::scene
