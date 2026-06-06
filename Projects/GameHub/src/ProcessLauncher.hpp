// FBZZ Engine
// ProcessLauncher.hpp | fbzz::hub
// Editor process lookup and launch
#pragma once

#include "HubConfig.hpp"

#include <string>

namespace fbzz::hub {

class ProcessLauncher {
public:
    static bool OpenInEditor(const HubConfig& config, const std::string& projectPath, std::string& errorMessage);

private:
    static std::wstring ResolveEditorPath(const HubConfig& config);
};

} // namespace fbzz::hub
