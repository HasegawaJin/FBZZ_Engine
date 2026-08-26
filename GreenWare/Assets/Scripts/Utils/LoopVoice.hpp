/// @file    LoopVoice.hpp
/// @brief   鳴り続ける音を専用の子オブジェクトで持つ 1 本の声
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 持ち主の AudioSource で鳴らさないか:
///   AudioSourceComponent が持てる主 voice は 1 本だけで、その volume と pitch は
///   AudioSystem が PlayOneShot の一発ものにもそのまま掛ける。移動音を速さで絞ったり
///   高さを振ったりすると、同じ体から出る被弾音や撃破音まで一緒に小さくなる。
///   «鳴り続けているもの» と «起きたこと» は別の口から出す。
///
/// WHY 子オブジェクトを名前で拾い直すか:
///   スクリプト DLL をリロードすると持ち主の Script は作り直され、この声が覚えていた
///   EntityID は消える。一方で子は Scene 側に残っているため、拾い直せないと
///   リロードのたびに音源が増え、同じループが重なって鳴る。
///   (ElectricArcBundle が筋を拾い直すのと同じ理由)
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <string>
#include <string_view>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox::se {

class LoopVoice {
public:
    /// 子オブジェクト名に使う識別子。持ち主ごとに一意でないと音源を奪い合う。
    void SetKey(std::string key) { m_key = std::move(key); }

    /// 毎フレーム呼ぶ。path が空か volume が 0 なら止める。
    void Update(Script& owner, std::string_view path, float volume, float pitch = 1.0f);

    /// 鳴らすのをやめる。音源そのものは残す (また鳴らすときに作り直さないため)。
    void Stop(const Script& owner);

private:
    [[nodiscard]] AudioSourceComponent* Acquire(Script& owner);

    std::string m_key     = "Loop";
    EntityID    m_id      = EntityID::INVALID;
    bool        m_started = false;
};


inline AudioSourceComponent* LoopVoice::Acquire(Script& owner)
{
    GameObject* object = owner.scene.GetGameObject(m_id);
    if (!object) {
        const std::string name = "SFX_Loop_" + m_key;
        object = owner.scene.Find(name);
        if (!object) {
            const EntityID created = owner.scene.Create(name).GetID();
            // Create がコンポーネント配列を伸ばしうるので、設定は ID から引き直す。
            object = owner.scene.GetGameObject(created);
            if (!object) return nullptr;
            object->runtimeGenerated = true;
            if (GameObject* self = owner.scene.Self()) object->SetParent(*self);
            // 親の原点で鳴らす。体のどこから出ているかまでは聞き分けられない。
            object->transform.position = Vector3::ZERO;
        }
        m_id = object->GetID();
    }

    auto* source = object->GetComponent<AudioSourceComponent>();
    if (!source) {
        source = &object->AddComponent<AudioSourceComponent>();
        source->busName      = "SE";
        source->spatialBlend = 1.0f;
        source->playOnAwake  = false;
        source->loop         = true;
        // 既定の 50m は «盤面のどこに居ても全員ぶん鳴っている» になる。鳴り続ける音は
        // 一発ものと違って重なったぶんだけ濁るので、近くの数体ぶんに閉じる。
        source->minDistance  = 2.0f;
        source->maxDistance  = 22.0f;
    }
    return source;
}

inline void LoopVoice::Update(Script& owner, std::string_view path, float volume, float pitch)
{
    if (path.empty() || volume <= 0.001f) {
        Stop(owner);
        return;
    }

    AudioSourceComponent* source = Acquire(owner);
    if (!source) return;

    // 掛け直すのは «まだ鳴らしていない» か «別のクリップになった» ときだけ。
    // 毎フレーム要求を立てると AudioSystem が毎フレーム voice を作り直し、
    // ループが先頭で切り刻まれてノイズになる。
    if (!m_started || source->clipPath != path) {
        source->clipPath      = std::string(path);
        source->loop          = true;
        source->enabled       = true;
        source->m_pendingPlay = true;
        source->m_pendingStop = false;
        m_started             = true;
    }

    source->volume = Clamp01(volume);
    source->pitch  = Clamp(pitch, 0.01f, 4.0f);
}

inline void LoopVoice::Stop(const Script& owner)
{
    if (!m_started) return;
    m_started = false;

    GameObject* object = owner.scene.GetGameObject(m_id);
    if (!object) return;
    auto* source = object->GetComponent<AudioSourceComponent>();
    if (!source) return;
    source->m_pendingStop = true;
    source->m_pendingPlay = false;
}

} // namespace sandbox::se
