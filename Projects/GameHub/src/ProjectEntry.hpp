// FBZZ Engine
// ProjectEntry.hpp | fbzz::hub
// Hub に表示するプロジェクト情報
#pragma once

#include <string>

namespace fbzz::hub {

struct ProjectEntry {
    std::string name;
    std::string projectId;
    std::string path;
    std::string engineVersion;
    std::string lastOpened;
    std::string thumbnailPath;

    bool engineVersionMismatch = false;
    bool migrationRequired = false;
    bool pathExists = false;
    bool projFileValid = false;
    bool apiHeaderExists = false;
    bool settingsExists = false;
    bool cmakeExists = false;
    bool layoutValid = false;
    bool generatedRootsExist = false;
    bool thumbnailExists = false;
};

} // namespace fbzz::hub
