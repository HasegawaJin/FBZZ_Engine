// FBZZ Engine
// WaterAssetSerializer.cpp | fbzz::scene
// fzmat 移行により廃止。Save/Load は常に false を返すスタブ。
#include <Engine/Scene/WaterAssetSerializer.hpp>

namespace fbzz::scene {

bool WaterAssetSerializer::Save(const WaterComponent& /*component*/, const std::string& /*path*/)
{
    return false;
}

bool WaterAssetSerializer::Load(const std::string& /*path*/, WaterComponent& /*component*/)
{
    return false;
}

} // namespace fbzz::scene
