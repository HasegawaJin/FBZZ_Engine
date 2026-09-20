/// @file    BeatVoice.hpp
/// @brief   音程を変えて鳴らす一発ものの声。拍に乗った合図の «音階» を鳴らす
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note 持ち主の AudioSource では鳴らさない (LoopVoice と同じ理由) ─ pitch は一発ものにも
///       掛かるため、音程を上げると同じ体から出る斬撃音や被弾音まで一緒に高くなる。
///       主 voice を鳴らし直す単音にする ─ 拍の合図は 0.36 秒おきに上がる «旋律» で、
///       前の音が残って重なると和音になり段の上がり方が耳から消える。
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace sandbox::se {

class BeatVoice {
public:
    /// 子オブジェクト名に使う識別子。持ち主ごとに一意でないと音源を奪い合う。
    void SetKey(std::string key) { m_key = std::move(key); }
    /// 出力先のバス。音源を作る前に 1 度だけ効く。
    void SetOutput(std::string bus) { m_bus = std::move(bus); }

    /// path を pitch で 1 回鳴らす。鳴っている前の音は切れる。
    void Play(fbzz::scene::Script& owner, std::string_view path, float volume, float pitch);

private:
    [[nodiscard]] fbzz::scene::AudioSourceComponent* Acquire(fbzz::scene::Script& owner);

    std::string           m_key = "Beat";
    std::string           m_bus = "SE";
    fbzz::scene::EntityID m_id  = fbzz::scene::EntityID::INVALID;
};


inline fbzz::scene::AudioSourceComponent* BeatVoice::Acquire(fbzz::scene::Script& owner)
{
    using namespace fbzz::scene;

    GameObject* object = owner.scene.GetGameObject(m_id);
    if (!object) {
        /// @note DLL リロードで m_id は消えるが、子は Scene に残る。名前で拾い直す。
        const std::string name = "SFX_Beat_" + m_key;
        object = owner.scene.Find(name, true);
        if (!object) {
            const EntityID created = owner.scene.Create(name).GetID();
            /// @note Create がコンポーネント配列を伸ばしうるので、設定は ID から引き直す。
            object = owner.scene.GetGameObject(created);
            if (!object) return nullptr;
            object->runtimeGenerated = true;
            if (GameObject* self = owner.scene.Self()) object->SetParent(*self);
            object->transform.position = fbzz::math::Vector3::ZERO;
        }
        m_id = object->GetID();
    }

    auto* source = object->GetComponent<AudioSourceComponent>();
    if (!source) {
        source = &object->AddComponent<AudioSourceComponent>();
        source->busName      = m_bus;
        /// @note 合図は «画面の出来事» で場所を持たない。3D にすると振り向いた向きで旋律が偏る。
        source->spatialBlend = 0.0f;
        source->playOnAwake  = false;
        source->loop         = false;
    }
    return source;
}

inline void BeatVoice::Play(fbzz::scene::Script& owner, std::string_view path,
                            float volume, float pitch)
{
    if (path.empty() || volume <= 0.001f) return;

    fbzz::scene::AudioSourceComponent* source = Acquire(owner);
    if (!source) return;

    /// @note 再生要求は AudioSystem が «今の voice を止めて鳴らし直す» ので、同じクリップでも
    ///       毎回頭から鳴る。音程と音量は鳴らし直す瞬間の値が乗る。
    source->clipPath      = std::string(path);
    source->loop          = false;
    source->volume        = fbzz::math::Clamp01(volume);
    source->pitch         = fbzz::math::Clamp(pitch, 0.01f, 4.0f);
    source->enabled       = true;
    source->m_pendingPlay = true;
    source->m_pendingStop = false;
}

} // namespace sandbox::se
