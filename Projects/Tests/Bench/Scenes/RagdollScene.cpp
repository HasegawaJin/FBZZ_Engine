/// @file    RagdollScene.cpp
/// @brief   アクティブラグドールが «クリップからどれだけ離れるか» を目で見る場面。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// RagdollRigTests は «捕獲して書き戻したら元に戻る» «力負けしたら飽和数が増える» を
/// 数値で固定している。しかしラグドールの良し悪しは数値では決まらない ──
/// 「押されたときの粘り」「脱力したときの崩れ方」「可動域に当たった関節の暴れ方」は
/// 動きを見ないと判断できない。
///
/// ここでは骨を FK で動かす手続きクリップを回し、そのクリップ (細い灰線) と
/// 物理が出した姿勢 (太線) を重ねて描く。**2 つがどれだけ離れるか**が
/// アクティブラグドールの調整そのものになる。
#include "Scenes.hpp"

#include <Engine/Scene/Ragdoll/RagdollRig.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::bench {

namespace {

/// バインドポーズ 1 本。位置は «親からの相対» で持つ ── ワールド位置を直に持つと、
/// 骨を回したときに子の位置を動かし忘れて «骨格が伸び縮みするクリップ» になる
/// (RagdollRigTests で実際にやった失敗)。
struct RestBone {
    int         parent;
    const char* name;
    float       offsetX;
    float       offsetY;
    /// クリップがこの骨を Z 軸まわりに振る量 [rad]。
    float       swing;
    /// 振りの位相 [rad]。左右で反転させて «歩いている» ように見せる。
    float       phase;
};

/// 側面図で全身が読める人型。名前は RagdollProfile の規則に当たるよう付けてある
/// (Hand だけはどの規則にも当たらない ── その状態が UI に出ることも確認したい)。
constexpr RestBone kSkeleton[] = {
    { -1, "Hips",       0.00f,  1.00f, 0.10f, 0.0f },
    {  0, "Spine",      0.00f,  0.30f, 0.12f, 0.6f },
    {  1, "Chest",      0.00f,  0.25f, 0.10f, 1.2f },
    {  2, "Head",       0.00f,  0.30f, 0.15f, 1.8f },
    {  2, "UpperArm_L", -0.30f, -0.05f, 0.55f, 0.0f },
    {  4, "Forearm_L",  -0.30f, -0.25f, 0.45f, 1.0f },
    {  5, "Hand_L",     -0.25f, -0.25f, 0.20f, 1.6f },
    {  2, "UpperArm_R",  0.30f, -0.05f, 0.55f, 3.14f },
    {  7, "Forearm_R",   0.30f, -0.25f, 0.45f, 4.14f },
    {  8, "Hand_R",      0.25f, -0.25f, 0.20f, 4.74f },
    {  0, "Thigh_L",    -0.15f, -0.05f, 0.40f, 3.14f },
    { 10, "Shin_L",     -0.03f, -0.45f, 0.30f, 3.90f },
    { 11, "Foot_L",     -0.02f, -0.44f, 0.15f, 4.40f },
    {  0, "Thigh_R",     0.15f, -0.05f, 0.40f, 0.0f },
    { 13, "Shin_R",      0.03f, -0.45f, 0.30f, 0.76f },
    { 14, "Foot_R",      0.02f, -0.44f, 0.15f, 1.26f },
};

constexpr std::size_t kBoneCount = sizeof(kSkeleton) / sizeof(kSkeleton[0]);

/// 床の貫通と見なす深さ。XPBD は 1 刻みぶん沈んでから戻るので、そこは異常にしない。
constexpr float kGroundTolerance = 0.02f;
/// 骨の伸縮に許す割合と、短い骨で割合が効かなくなる分の下駄。
constexpr float kSpanTolerance = 0.05f;
constexpr float kSpanFloor     = 1.0e-3f;

ImU32 ColorOf(scene::RagdollDebugLine::Kind kind)
{
    switch (kind) {
    case scene::RagdollDebugLine::Kind::Joint:          return colors::kNormal;
    case scene::RagdollDebugLine::Kind::JointSaturated: return colors::kBodyAlt;
    case scene::RagdollDebugLine::Kind::Contact:        return colors::kContact;
    default:                                            return colors::kBody;
    }
}

class RagdollScene final : public BenchScene {
public:
    RagdollScene() { Reset(); }

    const char* Name() const override { return "ラグドール: クリップと物理の差"; }

    const char* WhatToLookFor() const override
    {
        return "細い灰線がクリップの姿勢、太い青線が物理が出した剛体。緑が可動域に余裕のある関節、\n"
               "橙がトルク上限に張り付いた関節、赤が接触。\n"
               "・ドライブを強くすると青線が灰線へ吸い付き、«ずれ» が 0 に近づくこと\n"
               "・ドライブを切ると全身が崩れて床に積み上がり、床を貫通しないこと\n"
               "・押したとき、橙 (力負け) が根ではなく末端から増えること。根から負けるなら\n"
               "  falloff の向きが逆で、体幹が末端より弱いということになる\n"
               "・可動域制限を «学習» に切り替えると、クリップを再生しても限界に食い込む関節\n"
               "  (limited) が 0 になること。学習前に 0 なら、そもそも可動域が広すぎる\n"
               "・骨が伸び縮みしないこと。灰線の長さが変わるならクリップ側 (FK) の作り方が誤り\n"
               "・«規則に当たらない骨» に Hand が並ぶこと。ここが増えていたら骨名が合っていない";
    }

    void Reset() override
    {
        m_humanoid    = true;
        m_driveOn     = true;
        m_driveScale  = 1.0f;
        m_driveFalloff = 0.85f;
        m_driveDamping = 1.0f;
        m_anchorScale = 3.0f;
        m_gravity     = 9.81f;
        m_substeps    = 8;
        m_clipAmount  = 1.0f;
        m_clipSpeed   = 1.0f;
        m_useGround   = true;
        m_learnLimits = false;
        m_time        = 0.0f;
        Rebuild();
    }

    void Simulate(float dt) override
    {
        if (!m_rig) return;

        m_time += dt * m_clipSpeed;
        Animate();

        // 実行時の RagdollSystem と同じ順序。順を入れ替えると «押し返しが 1 フレーム遅れる»
        // という形でしか症状が出ないので、ここでも同じ並びにしておく。
        if (m_driveOn) m_rig->UpdateDriveTargets(m_clip);
        m_rig->RefreshContacts(nullptr);
        m_rig->Step(dt);
        m_rig->ApplyContactReactions();
    }

    /// 姿勢の書き戻しとデバッグ幾何は «最後の 1 回» しか絵に出ない。刻みごとに作らない。
    void Present() override
    {
        if (!m_rig) return;
        m_rig->WritePose(m_clip, m_positions, m_rotations);
        m_deviation = m_rig->MeasureDeviation(m_clip);
        m_rig->BuildDebugLines(m_lines);
    }

    void DetectAnomalies(AnomalyLog& log) override
    {
        if (!m_rig) return;

        if (!log.CheckFinite("重心", m_rig->CenterOfMass(), 1.0e3f)) return;
        if (!log.CheckFinite("クリップとのずれ", m_deviation, 1.0e3f)) return;

        // 床の判定は剛体のデバッグ幾何で見る。骨の位置で見ると、剛体を持たない骨
        // (規則に当たらなかった骨) が親に引かれて沈むだけで嘘の貫通が出る。
        if (m_useGround) {
            float deepest = 0.0f;
            for (const scene::RagdollDebugLine& line : m_lines) {
                if (line.kind != scene::RagdollDebugLine::Kind::Body) continue;
                deepest = (std::min)(deepest, (std::min)(line.from.y, line.to.y));
            }
            log.ReportIf(deepest < -kGroundTolerance, Severity::Error,
                         "剛体が床を %.3f m 貫通している", -deepest);
        }

        for (std::size_t i = 0; i < m_positions.size() && i < kBoneCount; ++i) {
            const RestBone& rest = kSkeleton[i];
            if (!log.CheckFinite(rest.name, m_positions[i], 1.0e3f)) return;
            if (rest.parent < 0) continue;

            // 骨は伸び縮みしない。ここが動くのはソケットが解けているとき。
            const float bind = math::Vector3(rest.offsetX, rest.offsetY, 0.0f).Length();
            const float span = (m_positions[i]
                                - m_positions[static_cast<std::size_t>(rest.parent)]).Length();
            log.ReportIf(std::fabs(span - bind) > bind * kSpanTolerance + kSpanFloor,
                         Severity::Error, "%s の骨が %.3f m (バインド %.3f m から伸縮)",
                         rest.name, span, bind);
        }
    }

    void DrawControls() override
    {
        if (ImGui::Checkbox("人型プロファイル (切ると重機)", &m_humanoid)) Rebuild();
        if (ImGui::Button("組み直す")) Rebuild();
        ImGui::SameLine();
        if (ImGui::Button("クリップ姿勢へ戻す") && m_rig) {
            Animate();
            m_rig->Capture(m_clip);
        }
        ImGui::SameLine();
        if (ImGui::Button("押す") && m_rig)
            m_rig->ApplyImpulse({ -2.0f, 1.4f, 0.0f }, { 6.0f, 1.0f, 0.0f }, 1.2f);

        ImGui::Separator();
        bool drive = false;
        drive |= ImGui::Checkbox("ドライブ (切ると脱力)", &m_driveOn);
        drive |= ImGui::SliderFloat("トルク倍率", &m_driveScale, 0.0f, 4.0f, "%.2f");
        drive |= ImGui::SliderFloat("根からの減衰 (falloff)", &m_driveFalloff, 0.3f, 1.0f, "%.2f");
        drive |= ImGui::SliderFloat("減衰倍率", &m_driveDamping, 0.0f, 2.0f, "%.2f");
        drive |= ImGui::SliderFloat("根の繋ぎ止め", &m_anchorScale, 0.0f, 6.0f, "%.2f");
        if (drive) ApplyDrive();

        ImGui::Separator();
        if (ImGui::SliderFloat("重力", &m_gravity, 0.0f, 30.0f, "%.2f m/s^2") && m_rig)
            m_rig->SetGravity({ 0.0f, -m_gravity, 0.0f });
        if (ImGui::SliderInt("サブステップ", &m_substeps, 1, 24) && m_rig)
            m_rig->SetSubsteps(m_substeps);
        if (ImGui::Checkbox("床", &m_useGround)) ApplyGround();
        if (ImGui::Checkbox("可動域を学習", &m_learnLimits) && m_rig)
            m_rig->SetLimitLearning(m_learnLimits, 0.09f);

        ImGui::Separator();
        ImGui::SliderFloat("クリップの振り幅", &m_clipAmount, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("クリップの速さ", &m_clipSpeed, 0.0f, 3.0f, "%.2f");

        // 3 つの骨格が重なると、どれがどれか読めなくなる。1 つずつ消して見る口。
        ImGui::Separator();
        ImGui::TextDisabled("表示");
        ImGui::Checkbox("クリップ (灰)", &m_showClip);
        ImGui::SameLine();
        ImGui::Checkbox("剛体 (青)", &m_showBodies);
        ImGui::Checkbox("書き戻した骨 (黄)", &m_showBones);
        ImGui::SameLine();
        ImGui::Checkbox("可動域と接触", &m_showJoints);
        ImGui::SameLine();
        ImGui::Checkbox("重心", &m_showCom);

        ImGui::Separator();
        if (!m_rig) return;
        ImGui::Text("剛体 %d / 関節 %d / 接触 %d",
                    m_rig->GetBodyCount(), m_rig->GetJointCount(), m_rig->GetContactCount());
        ImGui::Text("クリップとのずれ %.4f m", m_deviation);

        const int saturated = m_rig->CountSaturatedJoints();
        const int limited   = m_rig->CountLimitedJoints();
        if (saturated > 0)
            ImGui::TextColored({ 1.0f, 0.72f, 0.36f, 1.0f }, "力負けした関節 %d", saturated);
        else
            ImGui::TextColored({ 0.74f, 0.77f, 0.81f, 1.0f }, "力負けした関節 0");
        if (limited > 0)
            ImGui::TextColored({ 1.0f, 0.72f, 0.36f, 1.0f }, "可動域に食い込んだ関節 %d", limited);
        else
            ImGui::TextColored({ 0.74f, 0.77f, 0.81f, 1.0f }, "可動域に食い込んだ関節 0");

        // «規則に当たらない骨» は、画面では «なんとなく柔らかい» としか見えない失敗。
        const std::vector<std::string>& unmatched = m_rig->UnmatchedBones();
        ImGui::Text("規則に当たらない骨 %zu", unmatched.size());
        for (const std::string& name : unmatched) ImGui::BulletText("%s", name.c_str());
    }

    void Draw(Viewport2D& view) override
    {
        view.DrawGrid(0.5f);
        if (m_useGround) view.DrawGroundLine(0.0f, colors::kGround);

        // クリップの骨格。物理を切っても必ずここへ戻るべき «正解»。
        if (m_showClip) {
            for (std::size_t i = 0; i < kBoneCount; ++i) {
                const int parent = kSkeleton[i].parent;
                if (parent < 0) continue;
                view.DrawLine(m_clip[static_cast<std::size_t>(parent)].position, m_clip[i].position,
                              colors::kHint, 1.0f);
            }
        }

        // 物理が出した剛体・可動域・接触。エンジン自身のデバッグ幾何をそのまま描く。
        for (const scene::RagdollDebugLine& line : m_lines) {
            const bool isBody = line.kind == scene::RagdollDebugLine::Kind::Body;
            if (isBody ? !m_showBodies : !m_showJoints) continue;
            view.DrawLine(line.from, line.to, ColorOf(line.kind), isBody ? 3.0f : 1.5f);
        }

        // 書き戻した骨。ここが灰線に重なっていれば «物理が付いてきている»。
        if (m_showBones) {
            for (std::size_t i = 0; i < m_positions.size() && i < kBoneCount; ++i) {
                const int parent = kSkeleton[i].parent;
                if (parent < 0) continue;
                view.DrawLine(m_positions[static_cast<std::size_t>(parent)], m_positions[i],
                              colors::kQuery, 1.0f);
            }
        }

        if (m_rig && m_showCom) view.DrawPoint(m_rig->CenterOfMass(), colors::kContact, 5.0f);
    }

    void ConfigureView(Viewport2D& view) override
    {
        view.SetPlane(ViewPlane::XY);
        view.SetFocus({ 0.0f, 1.0f, 0.0f }, 150.0f);
    }

private:
    /// バインドポーズ (振り幅 0) をワールド姿勢で作る。Build の基準はここ。
    std::vector<scene::RagdollBonePose> BindPose()
    {
        const float saved = m_clipAmount;
        m_clipAmount      = 0.0f;
        Animate();
        m_clipAmount = saved;
        return m_clip;
    }

    /// 局所回転 → ワールド姿勢の FK。骨の «長さ» は localOffset が持つので、
    /// どれだけ振っても骨格は伸び縮みしない。
    void Animate()
    {
        m_clip.resize(kBoneCount);
        for (std::size_t i = 0; i < kBoneCount; ++i) {
            const RestBone& rest = kSkeleton[i];

            const float angle = std::sin(m_time * 2.0f + rest.phase) * rest.swing * m_clipAmount;
            const math::Quaternion local =
                math::Quaternion::FromAxisAngle({ 0.0f, 0.0f, 1.0f }, angle);
            const math::Vector3 offset{ rest.offsetX, rest.offsetY, 0.0f };

            scene::RagdollBonePose& bone = m_clip[i];
            bone.parent = rest.parent;
            bone.name   = rest.name;
            if (rest.parent < 0) {
                bone.rotation = local;
                bone.position = offset;
            } else {
                const scene::RagdollBonePose& parent = m_clip[static_cast<std::size_t>(rest.parent)];
                bone.rotation = parent.rotation * local;
                bone.position = parent.position + parent.rotation * offset;
            }
        }
    }

    void ApplyDrive()
    {
        if (!m_rig) return;
        m_rig->SetDrive(m_driveOn, m_driveScale, m_driveFalloff, m_driveDamping);
        // 脱力させたのに根だけ吊られていると «宙に浮いた死体» になる。
        m_rig->SetRootAnchor(m_driveOn ? m_anchorScale : 0.0f, 0.04f, 0.05f);
    }

    void ApplyGround()
    {
        if (!m_rig) return;
        if (m_useGround) m_rig->SetGround(0.0f, 0.9f);
        else             m_rig->DisableGround();
    }

    void Rebuild()
    {
        m_time = 0.0f;

        const std::vector<scene::RagdollBonePose> bind = BindPose();

        m_rig = std::make_unique<scene::RagdollRig>();
        m_rig->Build(bind, m_humanoid ? scene::RagdollProfile::Humanoid()
                                      : scene::RagdollProfile::Mech());
        m_rig->SetLimitLearning(m_learnLimits, 0.09f);
        m_rig->SetGravity({ 0.0f, -m_gravity, 0.0f });
        m_rig->SetSubsteps(m_substeps);
        ApplyDrive();
        ApplyGround();

        Animate();
        m_rig->Capture(m_clip);
        m_rig->WritePose(m_clip, m_positions, m_rotations);
        m_rig->BuildDebugLines(m_lines);
        m_deviation = 0.0f;
    }

    std::unique_ptr<scene::RagdollRig>       m_rig;
    std::vector<scene::RagdollBonePose>      m_clip;
    std::vector<math::Vector3>               m_positions;
    std::vector<math::Quaternion>            m_rotations;
    std::vector<scene::RagdollDebugLine>     m_lines;

    bool  m_humanoid     = true;
    bool  m_driveOn      = true;
    float m_driveScale   = 1.0f;
    float m_driveFalloff = 0.85f;
    float m_driveDamping = 1.0f;
    float m_anchorScale  = 3.0f;
    float m_gravity      = 9.81f;
    int   m_substeps     = 8;
    float m_clipAmount   = 1.0f;
    float m_clipSpeed    = 1.0f;
    bool  m_useGround    = true;
    bool  m_learnLimits  = false;
    float m_time         = 0.0f;
    float m_deviation    = 0.0f;

    bool  m_showClip     = true;
    bool  m_showBodies   = true;
    bool  m_showBones    = true;
    bool  m_showJoints   = true;
    bool  m_showCom      = true;
};

} // namespace

std::unique_ptr<BenchScene> MakeRagdollScene() { return std::make_unique<RagdollScene>(); }

} // namespace fbzz::bench
