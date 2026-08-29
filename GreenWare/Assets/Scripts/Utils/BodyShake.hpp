/// @file    BodyShake.hpp
/// @brief   モデルの拡縮だけで震わせる。ポーズ (アニメーション) には一切触らない
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// WHY ポーズを触らないか:
///   骨を動かす手 (IK) は «アニメーションそのものを書き換える» ことになる。溜めの震えは
///   «今のポーズのまま体が細かく振れている» であって、別のポーズを作りたいわけではない。
///   モデルを丸ごと拡縮すれば、どのクリップが再生されていてもその上から等しく掛かる。
///
/// WHY 描画ノードに掛けるか (Player 本体ではなく):
///   スキンドメッシュは «描画ノードのワールド行列 × ボーン行列» で描かれる
///   (DeferredPasses: objData.world = go.transform.GetWorldMatrix())。描画ノードは
///   Player 直下にぶら下がる素の子で、アニメーションもボーン伝播も触らない。
///   Player 本体を拡縮するとカプセルまで一緒に脈打ち、接地判定が毎フレーム揺れる。
///
/// WHY ボーンではないか:
///   ボーンのローカル姿勢 (位置・回転・スケール) は AnimatorSystem が Phase::LateUpdate で
///   毎フレーム丸ごと書き直す。Script フェーズから触っても同じフレームのうちに必ず消える。
///
/// 拡縮の原点はモデル原点 (足元)。縦に伸ばしても浮かず、原点から遠い手や頭ほど大きく動く。
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

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void BodyShake::Ensure(Script& owner)
{
    m_nodes.clear();
    m_phase   = 0.0f;
    m_shaking = false;

    GameObject* self = owner.scene.Self();
    if (!self) return;

    // WHY 直下の子だけ見るか: 描画ノードは «モデル 1 体ぶんの入れ物» で、必ず持ち主の
    //     直下に並ぶ (材質ごとに 1 つ)。孫まで拾うと、ボーン配下に付けた武器や
    //     エフェクトのノードまで掴んで、震えが装備品ごとに二重に掛かる。
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
    // DLL リロードや Play 直後で拾えていなければ、その場で拾い直す。
    if (m_nodes.empty()) Ensure(owner);

    m_shaking = true;
    m_phase += Max(Time::deltaTime, 0.0f) * frequency;

    // WHY 2 つ目の速い波を足すか: 単一の正弦は往復が読めてしまい «脈打っている» に
    //     なる。倍数にならない速さの小さい波を重ねると、戻る位置が毎回わずかに違う
    //     «震え» になる。
    const float wave = std::sin(m_phase * TWO_PI)
                     + std::sin(m_phase * TWO_PI * 2.37f) * 0.35f;

    // WHY 縦と横を逆向きに振るか: 全軸を同じ比で振ると «近づいたり遠ざかったり» に
    //     見えて、力をこらえている絵にならない。縦に伸びたぶん横が締まると、
    //     嵩が変わらないまま «力んでいる» だけが出る。
    //     横を 0 にすれば «幅は 1 mm も変わらないのに手と頭は震えている» になる
    //     ─ 原点 (足元) から遠いところほど大きく動くという、拡縮そのものの性質。
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
