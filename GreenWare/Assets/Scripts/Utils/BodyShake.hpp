/// @file    BodyShake.hpp
/// @brief   モデルの拡縮だけで震わせる。ポーズ (アニメーション) には一切触らない
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// @note ボーンのローカル姿勢は AnimatorSystem が Phase::LateUpdate で毎フレーム書き直すため
///       Script から触っても消える。Player 本体を拡縮すると衝突カプセルも脈打つため、
///       スキンドメッシュを描く描画ノード (objData.world = go.transform.GetWorldMatrix()、
///       Player 直下の素の子) のスケールだけを操作する。原点はモデル原点 (足元)。
#pragma once

#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox::shake {

class BodyShake {
public:
    /// OnStart で 1 度呼ぶ。揺らす描画ノードと、その素のスケールを覚える。
    void Ensure(Script& owner);

    /// 毎フレーム呼ぶ。
    /// @param amount  縦の伸び幅 (素の大きさに対する比)。0 で止まる
    /// @param lateral 横の締まり。縦に対する比 (0 で幅を一切変えない)
    void Update(Script& owner, float amount, float lateral, float frequency);

    /// 素のスケールへ戻す。
    void Stop(Script& owner);

private:
    /// 1 つの描画ノードと、震え始める前のスケール。
    struct Node {
        EntityRef ref{};
        Vector3   base = Vector3::ONE;
    };

    std::vector<Node> m_nodes;
    float m_phase   = 0.0f;
    bool  m_shaking = false;
};


inline void BodyShake::Ensure(Script& owner)
{
    m_nodes.clear();
    m_phase   = 0.0f;
    m_shaking = false;

    GameObject* self = owner.scene.Self();
    if (!self) return;

    /// @note 直下の子だけを見る。描画ノードは持ち主の直下に材質ごと 1 つ並ぶため、孫まで拾うと
    ///       ボーン配下の武器・エフェクトのノードまで掴んで震えが二重に掛かる。
    const int count = self->GetChildCount();
    for (int i = 0; i < count; ++i) {
        GameObject* child = self->GetChild(i);
        if (!child || !child->GetComponent<SkinnedMeshRenderer>()) continue;
        m_nodes.push_back(Node{ EntityRef{ child->GetID() }, child->transform.scale });
    }
}

inline void BodyShake::Stop(Script& owner)
{
    if (!m_shaking) return;
    m_shaking = false;

    for (const Node& node : m_nodes)
        if (GameObject* object = node.ref.Resolve(owner.scene))
            object->transform.scale = node.base;
}

inline void BodyShake::Update(Script& owner, float amount, float lateral, float frequency)
{
    if (amount <= 0.0001f || frequency <= 0.0f) {
        Stop(owner);
        return;
    }
    /// @note DLL リロードや Play 直後で拾えていなければ、その場で拾い直す。
    if (m_nodes.empty()) Ensure(owner);

    m_shaking = true;
    m_phase += Max(Time::deltaTime, 0.0f) * frequency;

    /// @note 単一の正弦は往復が読めて «脈打つ» に見えるため、倍数比でない速い波を重ねて
    ///       戻る位置を毎回わずかにずらす。
    const float wave = std::sin(m_phase * TWO_PI)
                     + std::sin(m_phase * TWO_PI * 2.37f) * 0.35f;

    /// @note 全軸を同じ比で振ると «近づいたり遠ざかったり» に見える。縦に伸びた分だけ横を
    ///       締めれば嵩を変えずに «力んでいる» だけが出る (横 0 なら幅は不変のまま震える)。
    const float stretch = wave * amount;
    const Vector3 factor{ 1.0f - stretch * lateral,
                          1.0f + stretch,
                          1.0f - stretch * lateral };

    for (const Node& node : m_nodes) {
        GameObject* object = node.ref.Resolve(owner.scene);
        if (!object) continue;
        object->transform.scale = { node.base.x * factor.x,
                                    node.base.y * factor.y,
                                    node.base.z * factor.z };
    }
}

} // namespace sandbox::shake
