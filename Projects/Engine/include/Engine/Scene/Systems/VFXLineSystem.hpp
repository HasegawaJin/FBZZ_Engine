/// @file    VFXLineSystem.hpp
/// @brief   VFXLineComponent の端点を解決し、雷・ビームの形を帯のメッシュへ焼く
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// メッシュは同じ GameObject の ProceduralMeshComponent (保存しない) へ書き、GPU への転送と
/// MeshRenderer への差し込みは RuntimeMeshSystem (Phase::LateUpdate) に任せる。
/// エディター停止中も動く (Inspector で値を触ると、その場で Scene ビューの形が変わる)。
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class VFXLineSystem final : public ISystem {
public:
    std::string_view Name() const override { return "VFXLineSystem"; }
    Phase GetPhase() const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
