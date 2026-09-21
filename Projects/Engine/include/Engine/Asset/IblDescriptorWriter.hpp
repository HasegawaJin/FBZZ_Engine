/// @file    IblDescriptorWriter.hpp
/// @brief   GPU ベイク結果を Engine の IBL 記述子へ保存する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <filesystem>
#include <Graphics/Renderer/IIblBaker.hpp>
namespace fbzz::asset {
/// @return 書き込みに失敗した場合は false。
bool WriteIblDescriptor(const std::filesystem::path& path, const renderer::IblBakeOutput& output);
}
