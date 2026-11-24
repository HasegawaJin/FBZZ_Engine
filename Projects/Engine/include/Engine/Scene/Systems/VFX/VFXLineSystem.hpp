/// @file    VFXLineSystem.hpp
/// @brief   VFXLineComponent の端点を解決し、雷・ビームの形を帯のメッシュへ焼く
/// @author  Hasegawa Jin
/// @date    2026-09-12

/// @note メッシュは同じ GameObject の ProceduralMeshComponent (保存しない) へ書き、GPU への転送と
/// @note MeshRenderer への差し込みは RuntimeMeshSystem (Phase::LateUpdate) に任せる。
/// @note エディター停止中も動く (Inspector で値を触ると、その場で Scene ビューの形が変わる)。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class VFXLineSystem final : public ISystem {
public:
    std::string_view Name() const override { return "VFXLineSystem"; }
    Phase GetPhase() const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API OrderingHints GetOrder() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
