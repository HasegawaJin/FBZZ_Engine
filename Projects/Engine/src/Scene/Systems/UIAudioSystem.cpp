/// @file    UIAudioSystem.cpp
/// @brief   UIButton の hoverSound / clickSound を鳴らす。
/// @author  Hasegawa Jin
/// @date    2026-08-27
#include "Engine/Scene/Systems/UIAudioSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/AudioSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/UIButton.hpp"
#include "Engine/Audio/AudioManager.hpp"

namespace fbzz::scene {
namespace {

// UI の音は空間に置かれていないので、残響を通さない専用のバスへ流す。
// 名前が無いレイアウトでは Master へ落ちる (FindBus が kInvalidBus を返す)。
audio::BusIndex ResolveUIBus(audio::AudioManager& audioManager)
{
    const audio::BusIndex bus = audioManager.FindBus("UI");
    return bus == audio::kInvalidBus ? audio::kMasterBus : bus;
}

} // namespace

ComponentAccess UIAudioSystem::GetAccess() const
{
    // 鳴らすだけで UIButton は書き換えない。フラグを消すのは次の UISystem。
    return ComponentAccess{}.Reads<UIButton>();
}

OrderingHints UIAudioSystem::GetOrder() const
{
    // AudioSystem より前に鳴らし始めれば、同じフレームのミキサー更新に乗る。
    return OrderingHints{}.Before<AudioSystem>();
}

void UIAudioSystem::Update(SystemContext& ctx)
{
    if (!ctx.audioManager) return;
    audio::AudioManager& audioManager = *ctx.audioManager;
    const audio::BusIndex bus = ResolveUIBus(audioManager);

    for (EntityID id : ctx.scene.GetEntities<UIButton>()) {
        const auto* button = ctx.scene.GetComponent<UIButton>(id);
        const auto* go = ctx.scene.GetGameObject(id);
        if (!button || !button->enabled || !go || !go->activeInHierarchy()) continue;
        // 押せないボタンは触れても押せても無音。鳴ると「押せた」と誤解させる。
        if (!button->isInteractable) continue;

        // WHY 優先度を上げるか: 効果音の同時数が上限に当たったとき、
        //     UI の返事が消えると「押せていない」と受け取られる。
        //     爆発音 1 発より、押した音のほうが操作には要る。
        constexpr audio::AudioManager::PlayParams kUIParams{ /*priority*/ 8, 0.0f };

        if (button->onEnter && !button->hoverSound.empty())
            (void)audioManager.PlayVoice(button->hoverSound, false, bus, kUIParams);
        if (button->onClick && !button->clickSound.empty())
            (void)audioManager.PlayVoice(button->clickSound, false, bus, kUIParams);
    }
}

} // namespace fbzz::scene
