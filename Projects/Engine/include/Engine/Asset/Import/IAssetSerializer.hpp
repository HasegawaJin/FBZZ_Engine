/// @file    IAssetSerializer.hpp
/// @brief   ロード・セーブ両方が必要なアセット型の round-trip インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// AssetManager 内部では使わない。Editor の Save ボタン・テンプレート生成から呼ばれる。
#pragma once
#include <string>
#include <string_view>

namespace fbzz::asset {

template<typename T>
class IAssetSerializer {
public:
    virtual ~IAssetSerializer() = default;

    [[nodiscard]] virtual bool Save(const T& asset, const std::string& absPath) const = 0;
    [[nodiscard]] virtual bool Load(const std::string& absPath, T& outAsset)    const = 0;

    virtual std::string_view Extension() const = 0;
};

} // namespace fbzz::asset
