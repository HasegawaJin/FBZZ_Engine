/// @file    OwnedResources.hpp
/// @brief   所有権を移すテスト入力と非所有ハンドルによる観測を組み立てる。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Graphics/Renderer/OwnedResource.hpp>
#include <initializer_list>
#include <vector>

namespace fbzz::testkit {
template<class Tag>
std::vector<renderer::OwnedResource<Tag>> MakeOwnedResources(std::initializer_list<renderer::ResourceHandle<Tag>> handles)
{
    std::vector<renderer::OwnedResource<Tag>> result;
    result.reserve(handles.size());
    for (const auto handle : handles) result.emplace_back(handle);
    return result;
}
template<class Tag>
std::vector<renderer::ResourceHandle<Tag>> BorrowResources(const std::vector<renderer::OwnedResource<Tag>>& resources)
{
    std::vector<renderer::ResourceHandle<Tag>> result;
    result.reserve(resources.size());
    for (const auto& resource : resources) result.push_back(resource.Get());
    return result;
}
} /// @note namespace fbzz::testkit
