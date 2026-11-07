import { RegisterReliabilityTools } from './reliability/ReliabilityTools.js';
import { RegisterEditorQueryTools, RegisterEditorCommandTools } from './Tools/EditorTools.js';
import { RegisterSceneQueryTools, RegisterSceneCommandTools } from './Tools/SceneTools.js';
import { RegisterScriptQueryTools } from './Tools/ScriptTools.js';
import { RegisterAssetQueryTools, RegisterAssetCommandTools } from './Tools/AssetTools.js';
import { RegisterBehaviorTreeQueryTools, RegisterBehaviorTreeCommandTools } from './Tools/BehaviorTreeTools.js';
import { RegisterRenderingQueryTools, RegisterRenderingCommandTools } from './Tools/RenderingTools.js';
import { RegisterAnimationQueryTools, RegisterAnimationCommandTools } from './Tools/AnimationTools.js';
import { RegisterPhysicsQueryTools } from './Tools/PhysicsTools.js';
import { RegisterWorldQueryTools, RegisterWorldCommandTools } from './Tools/WorldTools.js';
import { RegisterBuildQueryTools, RegisterBuildCommandTools } from './Tools/BuildTools.js';
import { RegisterPlaytestQueryTools, RegisterPlaytestCommandTools } from './Tools/PlaytestTools.js';
import { RegisterFluidQueryTools, RegisterFluidCommandTools } from './Tools/FluidTools.js';
import { RegisterCSharpTools } from './Tools/CSharpTools.js';
import { RegisterSqlTools } from './Tools/SqlTools.js';
/// @note read 権限では Command の登録自体を行わず、各領域の状態は MCP セッション内に閉じる。
export function RegisterEditorTools(server, bus, permission) {
    RegisterEditorQueryTools(server, bus);
    RegisterSceneQueryTools(server, bus);
    RegisterScriptQueryTools(server, bus);
    RegisterAssetQueryTools(server, bus);
    RegisterBehaviorTreeQueryTools(server, bus);
    RegisterRenderingQueryTools(server, bus);
    RegisterAnimationQueryTools(server, bus);
    RegisterPhysicsQueryTools(server, bus);
    RegisterWorldQueryTools(server, bus);
    RegisterBuildQueryTools(server, bus);
    RegisterPlaytestQueryTools(server, bus);
    RegisterFluidQueryTools(server, bus);
    RegisterReliabilityTools(server, bus, permission);
    RegisterCSharpTools(server, bus, permission);
    RegisterSqlTools(server, bus);
    if (permission !== 'read') {
        RegisterEditorCommandTools(server, bus, permission);
        RegisterSceneCommandTools(server, bus, permission);
        RegisterAssetCommandTools(server, bus, permission);
        RegisterBehaviorTreeCommandTools(server, bus, permission);
        RegisterRenderingCommandTools(server, bus, permission);
        RegisterAnimationCommandTools(server, bus, permission);
        RegisterWorldCommandTools(server, bus, permission);
        RegisterBuildCommandTools(server, bus, permission);
        RegisterPlaytestCommandTools(server, bus, permission);
        RegisterFluidCommandTools(server, bus, permission);
    }
}
//# sourceMappingURL=tools.js.map