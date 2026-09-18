/// @file    CCDScene.cpp
/// @brief   高速な球が薄い壁をすり抜ける様子と、TOI がそれを止める様子を並べて見る場面。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// CCDTests は «この配置での TOI はいくつか» を数値で固定している。ここで見たいのは
/// その手前 ── 1 フレームの移動量が障害物の厚みを越えると、離散的な判定はどのフレームでも
/// 重ならないまま通り過ぎる。数値では «hit=false» としか出ないが、絵にすると
/// «サンプル点が壁を跨いでいる» という理由まで一目で分かる。
///
/// すり抜けは実機では «たまに起きる» ので再現させにくい。速度・厚み・フレームレートを
/// 連続的に動かして、どこから破綻するかを手で確かめるための場面。
#include "Scenes.hpp"

#include <Physics/CCDSolver.hpp>
#include <Physics/RigidBody.hpp>

#include <imgui.h>

#include <cmath>
#include <cstddef>
#include <vector>

namespace fbzz::bench {

namespace {

constexpr float       kStartX     = -6.0f;
constexpr float       kEndX       = 6.0f;
constexpr std::size_t kMaxSamples = 4000; ///< 低速 × 高フレームレートで発散しないための上限
/// 停止位置に許すずれ。TOI は解析解なので、二次方程式の丸め以上に外れたら式が違う。
constexpr float kTouchTolerance = 5.0e-3f;

/// 障害物の種類。CCDSolver の 2 つの経路にそれぞれ対応させる。
enum class Obstacle {
    Wall,  ///< SweptSpherePlane
    Sphere ///< SweptSphereSphere
};

class CCDScene final : public BenchScene {
public:
    CCDScene() { Reset(); }

    const char* Name() const override { return "CCD: すり抜けと TOI"; }

    const char* WhatToLookFor() const override
    {
        return "青が «離散判定が見る各フレームの位置»、緑が CCD が返した衝突位置 (TOI)。\n"
               "・速度を上げると青い点が障害物を跨ぎ、どの点も赤くならなくなること。これがすり抜け\n"
               "・そのときも緑の輪は障害物の手前に出続けること。消えたら CCD が効いていない\n"
               "・緑の輪が障害物と «接している» こと。めり込む/離れるなら TOI がずれている\n"
               "・緑の矢印 (法線) が障害物から球へ向いていること。逆なら押し戻しが内側へ働く\n"
               "・NeedsCCD が «不要» のままなのに青い点が跨ぐ速度があってはいけない。\n"
               "  そこが判定の穴で、実機では «たまにすり抜ける» という形で出る\n"
               "・厚み / 半径を上げると同じ速度でもすり抜けなくなること";
    }

    void Reset() override
    {
        m_obstacle    = Obstacle::Wall;
        m_speed       = 60.0f;
        m_wallHalf    = 0.15f;
        m_targetRadius = 0.4f;
        m_radius      = 0.25f;
        m_frameRate   = 60.0f;
        m_playing     = true;
        m_cursorTime  = 0.0f;
        Rebuild();
    }

    void Simulate(float dt) override
    {
        if (!m_playing || m_samples.empty()) return;

        /// @note 表示上のコマ送り。判定そのものは Rebuild が全フレームぶん済ませてある。
        m_cursorTime += dt * m_frameRate;
        const float count = static_cast<float>(m_samples.size());
        while (m_cursorTime >= count) m_cursorTime -= count;
    }

    void DetectAnomalies(AnomalyLog& log) override
    {
        /// @note 障害物は必ず進路上にある。返さないなら掃引そのものが解けていない。
        if (!m_hit.hit) {
            log.Report(Severity::Error, "進路上に障害物があるのに CCD がヒットを返していない");
            return;
        }

        log.ReportIf(m_hit.toi < 0.0f || m_hit.toi > 1.0f, Severity::Error,
                     "TOI が [0,1] の外 (%.5f)", m_hit.toi);
        if (!log.CheckFinite("停止位置", m_hitCenter, 1.0e3f)) return;
        if (!log.CheckFinite("法線", m_hit.normal, 1.0e2f)) return;

        const float length = m_hit.normal.Length();
        log.ReportIf(std::fabs(length - 1.0f) > 1.0e-3f, Severity::Error,
                     "法線が単位長でない (|n| = %.5f)", length);

        /// @note 弾は -X から来る。法線が +X を向くと押し戻しが障害物の内側へ働く。
        log.ReportIf(m_hit.normal.x > 0.0f, Severity::Error,
                     "法線が障害物の内側を向いている (%+.2f, %+.2f, %+.2f)",
                     m_hit.normal.x, m_hit.normal.y, m_hit.normal.z);

        /// @note 停止位置は «ちょうど接する» ところ。めり込んでも離れても TOI がずれている。
        const float gap = m_hitCenter.x - TouchingX();
        log.ReportIf(std::fabs(gap) > kTouchTolerance, Severity::Error,
                     "TOI の停止位置が接触面から %+.4f m ずれている", gap);

        /// @note «不要» と言われた速度ですり抜けている ── しきい値が緩すぎる状態。
        log.ReportIf(!NeedsCCD() && m_overlappingFrames == 0, Severity::Error,
                     "NeedsCCD が拾わない速度ですり抜けている (しきい値が緩い)");
    }

    void DrawControls() override
    {
        bool dirty = false;

        int obstacle = m_obstacle == Obstacle::Wall ? 0 : 1;
        if (ImGui::Combo("障害物", &obstacle, "薄い壁 (平面)\0静止した球\0")) {
            m_obstacle = obstacle == 0 ? Obstacle::Wall : Obstacle::Sphere;
            dirty      = true;
        }

        dirty |= ImGui::SliderFloat("速度 (m/s)", &m_speed, 1.0f, 400.0f, "%.0f");
        if (m_obstacle == Obstacle::Wall)
            dirty |= ImGui::SliderFloat("壁の厚み (半分)", &m_wallHalf, 0.02f, 1.5f, "%.3f m");
        else
            dirty |= ImGui::SliderFloat("的の半径", &m_targetRadius, 0.05f, 2.0f, "%.3f m");
        dirty |= ImGui::SliderFloat("弾の半径", &m_radius, 0.05f, 1.0f, "%.3f m");
        dirty |= ImGui::SliderFloat("フレームレート", &m_frameRate, 15.0f, 240.0f, "%.0f fps");

        ImGui::Checkbox("流す", &m_playing);
        if (dirty) Rebuild();

        ImGui::Separator();
        ImGui::Text("1 フレームの移動量 %.3f m", StepLength());
        ImGui::Text("越えるべき厚み %.3f m", CrossingThickness());
        ImGui::Text("サンプル %zu / 重なったフレーム %d",
                    m_samples.size(), m_overlappingFrames);

        /// @note NeedsCCD のしきい値そのもの。«不要» なのに跨ぐ速度があれば、そこが判定の穴。
        const bool needsCCD = NeedsCCD();
        if (needsCCD) ImGui::TextColored({1.0f, 0.72f, 0.36f, 1.0f}, "NeedsCCD: 必要");
        else          ImGui::TextColored({0.74f, 0.77f, 0.81f, 1.0f}, "NeedsCCD: 不要");

        ImGui::Separator();
        if (m_hit.hit) {
            ImGui::TextColored({0.48f, 0.90f, 0.55f, 1.0f},
                               "TOI %.4f (フレーム %d の途中)", m_hit.toi, m_hitFrame);
            ImGui::Text("停止位置 x = %+.3f", m_hitCenter.x);
            ImGui::Text("接触点   x = %+.3f", m_hit.contactPoint.x);
            ImGui::Text("法線     (%+.2f, %+.2f, %+.2f)",
                        m_hit.normal.x, m_hit.normal.y, m_hit.normal.z);
        } else {
            ImGui::TextColored({1.0f, 0.36f, 0.41f, 1.0f}, "CCD もヒットを返していない");
        }

        if (m_overlappingFrames == 0)
            ImGui::TextColored({1.0f, 0.36f, 0.41f, 1.0f},
                               "離散判定はすり抜ける (どのフレームでも重ならない)");
        else
            ImGui::TextColored({0.74f, 0.77f, 0.81f, 1.0f}, "離散判定でも捕まえられる");

        /// @note «不要» と言われたのに跨いでいる ── しきい値が緩すぎる状態。
        if (!needsCCD && m_overlappingFrames == 0)
            ImGui::TextColored({1.0f, 0.36f, 0.41f, 1.0f},
                               "NeedsCCD が拾わない速度ですり抜けている (しきい値が緩い)");
    }

    void Draw(Viewport2D& view) override
    {
        view.DrawGrid(0.5f);
        DrawObstacle(view);

        /// @note 各フレームの位置。障害物と重なったフレームだけ赤くする。
        ///       低速では点が数千個になるので間引くが、重なった点は «すり抜けたかどうか» の
        ///       判断そのものなので必ず描く。
        const std::size_t stride = 1 + m_samples.size() / 300;
        for (std::size_t index = 0; index < m_samples.size(); ++index) {
            const float x        = m_samples[index];
            const bool  overlaps = Overlaps(x);
            if (!overlaps && index % stride != 0) continue;
            view.DrawCircle({x, 0.0f, 0.0f}, m_radius,
                            overlaps ? colors::kContact : colors::kBody, false, 1.2f);
        }

        /// @note 今のコマ。どのフレームを見ているかを追えるようにする。
        if (!m_samples.empty()) {
            const std::size_t index = static_cast<std::size_t>(m_cursorTime) % m_samples.size();
            const math::Vector3 from{m_samples[index], 0.0f, 0.0f};
            const math::Vector3 to{m_samples[index] + StepLength(), 0.0f, 0.0f};
            view.DrawCircle(from, m_radius, colors::kBodyAlt, false, 2.5f);
            view.DrawArrow(from, to, colors::kBodyAlt, 2.0f);
        }

        /// @note CCD が返した «最初に当たる位置»。ここで止まれば貫通しない。
        if (m_hit.hit) {
            view.DrawCircle(m_hitCenter, m_radius, colors::kNormal, false, 2.5f);
            view.DrawPoint(m_hit.contactPoint, colors::kContact, 5.0f);
            view.DrawArrow(m_hit.contactPoint, m_hit.contactPoint + m_hit.normal * 0.8f,
                           colors::kNormal, 2.0f);
        }
    }

    void ConfigureView(Viewport2D& view) override
    {
        view.SetPlane(ViewPlane::XY);
        view.SetFocus({0.0f, 0.0f, 0.0f}, 70.0f);
    }

private:
    float StepLength() const { return m_speed / m_frameRate; }

    bool NeedsCCD() const
    {
        physics::RigidBody probe;
        probe.SetVelocity({m_speed, 0.0f, 0.0f});
        return physics::CCDSolver::NeedsCCD(probe, m_radius, 1.0f / m_frameRate);
    }

    /// 弾が障害物にちょうど接するときの中心 X。TOI の «正解» はここ 1 点に決まる。
    float TouchingX() const
    {
        const float half = m_obstacle == Obstacle::Wall ? m_wallHalf : m_targetRadius;
        return -(half + m_radius);
    }

    /// 離散判定が «越えてはいけない» 距離。ここより 1 フレームの移動量が大きいと穴が開く。
    float CrossingThickness() const
    {
        const float half = m_obstacle == Obstacle::Wall ? m_wallHalf : m_targetRadius;
        return (half + m_radius) * 2.0f;
    }

    /// 位置 x の弾が障害物と重なっているか。離散判定が «そのフレームで» 見るもの。
    bool Overlaps(float x) const
    {
        if (m_obstacle == Obstacle::Wall) return std::fabs(x) - m_wallHalf < m_radius;
        return std::fabs(x) < m_targetRadius + m_radius;
    }

    void DrawObstacle(Viewport2D& view)
    {
        if (m_obstacle == Obstacle::Wall) {
            view.DrawBox({0.0f, 0.0f, 0.0f}, {m_wallHalf, 1.6f, 0.0f}, colors::kGround, true);
            /// @note 判定に使う «手前側の面» そのもの。TOI はこの線で解いている。
            view.DrawLine({-m_wallHalf, -1.9f, 0.0f}, {-m_wallHalf, 1.9f, 0.0f},
                          colors::kHint, 1.0f);
        } else {
            view.DrawCircle({0.0f, 0.0f, 0.0f}, m_targetRadius, colors::kGround, true);
        }
    }

    /// フレームごとの位置を全部作り、CCD を «最初に当たるフレーム» まで前へ回す。
    /// 実行時の CCDPhase と同じく、1 フレームぶんの掃引を順に試すことになる。
    void Rebuild()
    {
        const float dt   = 1.0f / m_frameRate;
        const float step = StepLength();

        m_samples.clear();
        for (float x = kStartX; x <= kEndX && m_samples.size() < kMaxSamples; x += step)
            m_samples.push_back(x);

        m_overlappingFrames = 0;
        for (const float x : m_samples)
            if (Overlaps(x)) ++m_overlappingFrames;

        m_hit       = {};
        m_hitFrame  = -1;
        m_hitCenter = {kStartX, 0.0f, 0.0f};

        const math::Vector3 velocity{m_speed, 0.0f, 0.0f};
        for (std::size_t index = 0; index < m_samples.size(); ++index) {
            const math::Vector3 center{m_samples[index], 0.0f, 0.0f};

            const physics::CCDResult result =
                m_obstacle == Obstacle::Wall
                    /// @note 壁の手前側の面。法線は弾の側 (-X) を向く。
                    ? physics::CCDSolver::SweptSpherePlane(center, m_radius, velocity,
                                                           {-1.0f, 0.0f, 0.0f}, m_wallHalf, dt)
                    : physics::CCDSolver::SweptSphereSphere(center, m_radius, velocity,
                                                            {0.0f, 0.0f, 0.0f}, m_targetRadius, dt);
            if (!result.hit) continue;

            m_hit       = result;
            m_hitFrame  = static_cast<int>(index);
            m_hitCenter = center + velocity * dt * result.toi;
            break;
        }
    }

    Obstacle m_obstacle     = Obstacle::Wall;
    float    m_speed        = 60.0f;
    float    m_wallHalf     = 0.15f;
    float    m_targetRadius = 0.4f;
    float    m_radius       = 0.25f;
    float    m_frameRate    = 60.0f;
    bool     m_playing      = true;
    float    m_cursorTime   = 0.0f;

    std::vector<float>  m_samples;
    int                 m_overlappingFrames = 0;
    physics::CCDResult  m_hit;
    int                 m_hitFrame = -1;
    math::Vector3       m_hitCenter{kStartX, 0.0f, 0.0f};
};

} // namespace

std::unique_ptr<BenchScene> MakeCCDScene() { return std::make_unique<CCDScene>(); }

} // namespace fbzz::bench
