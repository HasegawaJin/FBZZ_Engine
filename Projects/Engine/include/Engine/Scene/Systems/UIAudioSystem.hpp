/// @file    UIAudioSystem.hpp
/// @brief   UIButton の状態変化から効果音を鳴らす Scene System。
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// @note UISystem の中で鳴らさない理由: UISystem はレンダーパスから呼ばれる自由関数で
///       AudioManager を知らない。全 Viewport へ引数を足すと Scene ビューを描いただけで
///       クリック音が鳴る経路が生まれるため、«ゲームの 1 フレームに 1 回» の System にする。
/// @note UI のフラグは描画パスで立つため鳴るのは 1 フレーム後 (ScriptProxy と同じ遅れで体感には出ない)。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class UIAudioSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "UIAudioSystem"; }
    Phase            GetPhase()   const override { return Phase::LateScript; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
