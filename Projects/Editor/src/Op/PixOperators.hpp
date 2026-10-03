/// @file    PixOperators.hpp
/// @brief   Private PIX operator services for deterministic non-launching tests.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once

#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Core/PixCapture.hpp>

#include <filesystem>
#include <functional>

namespace fbzz::editor {

/// @note Tests replace both boot-state reading and launch; an absent launch callback never falls back to external execution.
struct PixOperatorServices {
    std::function<core::PixCaptureStatus()> readCaptureStatus;
    std::filesystem::path defaultInstalledDirectory;
    std::function<bool(const std::filesystem::path&, std::string&)> launchUi;
};

void RegisterPixOperators(OperatorRegistry& registry, PixOperatorServices services);

} /// @note namespace fbzz::editor
