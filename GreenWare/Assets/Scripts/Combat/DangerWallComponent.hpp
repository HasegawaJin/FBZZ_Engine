/// @file    DangerWallComponent.hpp
/// @brief   帯の予兆を «立てる»。直進攻撃の通り道を高さのある回廊として見せる
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// @note 床デカールだけでは不足。ボス1は10m 距離で画面高さの71%を占め (実測)、足元の帯が本体の陰に隠れる。
/// @note 面で塞がず両脇2枚にする。1枚で塞ぐと «避ける先» (帯の外) が壁の向こうに隠れる。
/// @note 毎フレーム組み直す。ビームは薙ぎ突進は向きが変わるため、固定だと壁外なのに当たる (`BossTelegraph.hpp` と同じ理由)。
#pragma once

#include <Engine/Scene/MeshBuilder.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class DangerWallComponent : public Script {
    FBZZ_SCRIPT(DangerWallComponent)

public:
    FBZZ_GROUP("Wall")
    FBZZ_FIELD_RANGE(float, height, 3.2f, "高さ", 0.2f, 20.0f)
    FBZZ_TOOLTIP("壁の高さ [m]。プレイヤー 2.5m が «越えられない» と読める程度に")
    FBZZ_FIELD_RANGE(float, ribScrollHz, 0.8f, "Rib Scroll", -4.0f, 4.0f)
    FBZZ_FIELD_RANGE_INT(int, segments, 24, "分割数", 2, 96)
    FBZZ_FIELD_RANGE(float, warnVolume, 0.75f, "警告の音量", 0.0f, 2.0f)
    FBZZ_TOOLTIP("危険線が引かれた瞬間に 1 度だけ鳴る。0 で無音 (見ていない方向の線に気付けなくなる)")
    FBZZ_TOOLTIP("長さ方向の分割数。少ないと桟の流れがカクつく")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Hidden", "状態")

    /// 今フレームの予兆を受け取る。帯でなければ壁を畳む。
    ///
    /// @note 自分で AI を探さない。渡す側 (`BossTelegraphComponent`) が既に集めているため、
    ///       探し直すと同じ分岐が 2 箇所に増える。
    void Submit(const BossTelegraph& telegraph)
    {
        m_telegraph = telegraph;
        m_submitted = true;
    }

    void OnLateUpdate() override
    {
        const bool show = m_submitted
                       && m_telegraph.shape == BossTelegraphShape::Line
                       && m_telegraph.length > 0.01f
                       && m_telegraph.radius > 0.001f;
        m_submitted = false;

        if (!show) {
            if (m_visible) {
                m_builder.Clear();
                mesh.Apply(m_builder);
                m_visible = false;
            }
            debugState = "Hidden";
            return;
        }

        /// @note 出た瞬間に 1 度だけ警告。画面外でも線は引かれるため音でしか気付けない。
        ///       出続けている間は鳴らさない。ボス側の充填音と重なると «来るぞ» が 2 つに聞こえる。
        if (!m_visible) se::Play(audio, se::kEnvHazardWarn, warnVolume);

        Build();
        mesh.Apply(m_builder);
        m_visible = true;
        debugState = "Shown";

        const MaterialInstance instance = material.Instance(0);
        if (instance.IsValid() && instance.HasProperty(kProgressId)) {
            (void)instance.SetFloat(kProgressId, Clamp01(m_telegraph.progress));
            (void)instance.SetFloat(kRibScrollId, Time::time * ribScrollHz);
        }
    }

private:
    static constexpr MaterialPropertyId kProgressId{ "progress" };
    static constexpr MaterialPropertyId kRibScrollId{ "ribScroll" };

    /// 通り道の両脇に板を 1 枚ずつ立てる。
    ///
    /// @note ワールド座標のまま積む。この GameObject は原点固定・予兆もワールド座標で来るため、
    ///       ローカルへ落とすと «誰の原点か» を渡す側と合わせ続けることになる (`BladeTrail` と同じ方針)。
    void Build()
    {
        m_builder.Clear();

        const Vector3 dir = m_telegraph.direction.NormalizedOr(Vector3{ 0.0f, 0.0f, 1.0f });
        const Vector3 side =
            Vector3::Cross(Vector3::UP, dir).NormalizedOr(Vector3{ 1.0f, 0.0f, 0.0f });
        const float   half = std::max(m_telegraph.radius, 0.001f);
        const float   len  = std::max(m_telegraph.length, 0.01f);
        const int     count = std::max(segments, 2);
        const float   top  = std::max(height, 0.05f);

        /// @note 左右で法線を外向きにする。両面表示だが、法線が内向きだと
        ///       面光源やフォグの寄与が裏返る。
        AddPanel(dir, side,  half, len, count, top,  side);
        AddPanel(dir, side, -half, len, count, top, -side);
    }

    void AddPanel(const Vector3& dir, const Vector3& side, float offset,
                  float len, int count, float top, const Vector3& normal)
    {
        const uint32_t base = static_cast<uint32_t>(m_builder.Vertices().size());
        const Vector3  root = m_telegraph.origin + side * offset;

        for (int i = 0; i < count; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(count - 1);
            const Vector3 at = root + dir * (len * u);
            m_builder.AddVertex(at, normal, Vector2{ u, 0.0f });
            m_builder.AddVertex(at + Vector3::UP * top, normal, Vector2{ u, 1.0f });
        }

        for (int i = 0; i + 1 < count; ++i) {
            const uint32_t q = base + static_cast<uint32_t>(i) * 2u;
            m_builder.AddQuad(q, q + 1u, q + 3u, q + 2u);
        }
    }

    MeshBuilder    m_builder;
    BossTelegraph  m_telegraph;
    bool           m_submitted = false;
    bool           m_visible   = false;
};

FBZZ_REFLECT(DangerWallComponent)

} // namespace sandbox
