// {{PROJECT_NAME}}
// GameMain.cpp | {{CPP_NAMESPACE}}
// Game script registration — add your game scripts here.
//
// WHAT: RegisterScripts() is called at startup in both editor and standalone modes.
//       Every ScriptComponent subclass used in scenes must be registered here.
//       The entry point (main) lives in AppMain.cpp — do not add one here.
#include "{{TARGET_NAME}}/ProjectAPI.hpp"
#include <Engine/Scene/ScriptFactory.hpp>

// #include "Scripts/MyScript.hpp"

namespace {{CPP_NAMESPACE}} {

void RegisterScripts()
{
    // scene::ScriptFactory::Register<MyScript>();
}

} // namespace {{CPP_NAMESPACE}}
