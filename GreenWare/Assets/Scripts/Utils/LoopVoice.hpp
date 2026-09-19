/// @file    LoopVoice.hpp
/// @brief   鳴り続ける音を専用の子オブジェクトで持つ 1 本の声
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note 持ち主の AudioSource で鳴らさない。主 voice は 1 本だけで volume/pitch は
///       AudioSystem が PlayOneShot の一発ものにもそのまま掛けるため、移動音を絞ると
///       同じ体の被弾音・撃破音まで一緒に小さくなる。«鳴り続けるもの» と «起きたこと» は
///       別の口から出す。子オブジェクトは名前で拾い直す (ElectricArcBundle と同じ理由):
///       DLL リロードで持ち主が覚えていた EntityID は消えるが子は Scene に残るため、
///       拾い直せないとリロードのたびに音源が増え同じループが重なる。
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

    /// 出力先と広がりを変える。既定は «盤面のどこかで鳴っている» 3D の SE。
    /// @note 既定を 3D にするのは、この声の使い道の多くが敵や機械の定常音で方向が聞き分けの
    ///       手がかりになるため。2D は環境音のように «場所を持たない» ものだけ明示させる。
    /// 音源を作った後に呼んでも既に立っている声には効かない。Update() の前に 1 度呼ぶこと。
    void SetOutput(std::string bus, float spatialBlend)
    {
        m_bus     = std::move(bus);
        m_spatial = Clamp01(spatialBlend);
    }

    /// 毎フレーム呼ぶ。path が空か volume が 0 なら止める。
    void Update(Script& owner, std::string_view path, float volume, float pitch = 1.0f);

    /// 鳴らすのをやめる。音源そのものは残す (また鳴らすときに作り直さないため)。
    void Stop(const Script& owner);

private:
    [[nodiscard]] AudioSourceComponent* Acquire(Script& owner);

    std::string m_key     = "Loop";
    std::string m_bus     = "SE";
    float       m_spatial = 1.0f;
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
            /// @note Create がコンポーネント配列を伸ばしうるので、設定は ID から引き直す。
            object = owner.scene.GetGameObject(created);
            if (!object) return nullptr;
            object->runtimeGenerated = true;
            if (GameObject* self = owner.scene.Self()) object->SetParent(*self);
            /// @note 親の原点で鳴らす。体のどこから出ているかまでは聞き分けられない。
            object->transform.position = Vector3::ZERO;
        }
        m_id = object->GetID();
    }

    auto* source = object->GetComponent<AudioSourceComponent>();
    if (!source) {
        source = &object->AddComponent<AudioSourceComponent>();
        source->busName      = m_bus;
        source->spatialBlend = m_spatial;
        source->playOnAwake  = false;
        source->loop         = true;
        /// @note 既定の 50m は «盤面のどこに居ても全員ぶん鳴っている» になる。鳴り続ける音は
        ///       一発ものと違って重なったぶんだけ濁るので、近くの数体ぶんに閉じる。
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

    /// @note 掛け直すのは «まだ鳴らしていない» か «別のクリップになった» ときだけ。
    ///       毎フレーム要求を立てると AudioSystem が毎フレーム voice を作り直し、
    ///       ループが先頭で切り刻まれてノイズになる。
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
