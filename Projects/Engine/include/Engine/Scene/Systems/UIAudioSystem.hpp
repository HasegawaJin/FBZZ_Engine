/// @file    UIAudioSystem.hpp
/// @brief   UIButton の状態変化から効果音を鳴らす Scene System。
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// WHY UISystem の中で鳴らさないか:
///   UISystem はレンダーパスから呼ばれる自由関数で、AudioManager を知らない。
///   知らせるには全 Viewport の呼び出しへ引数を足すことになり、Editor の
///   Scene ビューを描いただけでクリック音が鳴る経路まで生まれる。
///   鳴らすのは「ゲームの 1 フレームに 1 回だけ回るシステム」の仕事にする。
///
/// NOTE: UI のフラグは描画パスで立つため、鳴るのは 1 フレーム後になる。
///       ScriptProxy が UIButton を読むときと同じ遅れで、体感には出ない。
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
