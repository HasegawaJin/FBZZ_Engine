# TASKS 遯ｶ繝ｻFBZZ Engine

闖ｴ諛茨ｽ･・ｭ鬮｢蜿･・ｧ蛹ｺ蜃ｾ邵ｺ・ｫ驕抵ｽｺ髫ｱ髦ｪ・邵ｲ竏ｽ・ｽ諛茨ｽ･・ｭ驍ｨ繧・ｽｺ繝ｻ蜃ｾ邵ｺ・ｫ隴厄ｽｴ隴・ｽｰ邵ｺ蜷ｶ・狗ｸｺ阮吮・邵ｲ繝ｻ
髫ｧ・ｳ驍擾ｽｰ髫ｪ・ｭ髫ｪ蛹ｻ繝ｻ `docs/` 闔会ｽ･闕ｳ荵昴・陷ｷ繝ｻDesign.md 郢ｧ雋樒崟霎｣・ｧ邵ｲ繝ｻ

---

## 霑ｴ・ｾ陜ｨ・ｨ陜ｨ・ｰ: Step 6 遯ｶ繝ｻImGui Editor

髫ｧ・ｳ驍擾ｽｰ: [docs/editor/Design.md](docs/editor/Design.md)

| # | 郢ｧ・ｿ郢ｧ・ｹ郢ｧ・ｯ | 霑･・ｶ隲ｷ繝ｻ|
|---|--------|------|
| 1 | Engine Util 隰ｨ・ｴ陋ｯ繝ｻ(ILogSink / FileSystem / StringUtils) | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 2 | ImGui 遶翫・IRenderer 驍ｨ・ｱ陷ｷ繝ｻ| 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 3 | EditorApp 鬯ｪ・ｨ隴ｬ・ｼ (DockSpace + MenuBar) | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 4 | Util 陷ｷ繝ｻ・ｨ・ｮ (MathConvert / ImGuiWidgets / UndoStack / HotkeyManager / ConsoleSink / ModalDialog / EditorSettings / FileDialog) | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 5 | SceneHierarchyPanel | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 6 | InspectorPanel 遯ｶ繝ｻTransform 驍ｱ・ｨ鬮ｮ繝ｻ| 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 7 | ViewportPanel + EditorCamera | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 8 | Gizmo (ImGuizmo) + MousePicking | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 9 | Light editing integrated into InspectorPanel | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 10 | ConsolePanel / AssetBrowserPanel / StatusBar | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 11 | SceneSerializer 遯ｶ繝ｻengine::scene::SceneSerializer Save/Load | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 12 | PlayModeController 遯ｶ繝ｻPlay/Pause/Stop 陜難ｽｺ隴幢ｽｬ陷咲ｩゑｽｽ繝ｻ| 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 13 | InspectorPanel 遯ｶ繝ｻMeshRenderer / LightComponent / CameraComponent / ParticleEmitter / AudioSource / RigidBody / SkyRenderer Component 驍ｱ・ｨ鬮ｮ繝ｻ| 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 14 | editor::SceneSerializer Load/Deserialize 遯ｶ繝ｻPlayMode Stop 隴弱ｅ繝ｻ郢ｧ・ｹ郢晉ｿｫ繝｣郢晏干縺咏ｹ晢ｽｧ郢昴・繝ｨ陟包ｽｩ陷医・| 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| 15 | MenuBar: File > Save Scene / Open Scene | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|

---

## Step 6.5 遯ｶ繝ｻSceneSerializer 驍ｨ・ｱ陷ｷ繝ｻ

髫ｧ・ｳ驍擾ｽｰ: [docs/scene/SceneSerializer.md](docs/scene/SceneSerializer.md)

| # | 郢ｧ・ｿ郢ｧ・ｹ郢ｧ・ｯ | 霑･・ｶ隲ｷ繝ｻ|
|---|--------|------|
| 1 | sandbox/main.cpp 郢ｧ繝ｻSceneSerializer::Load 邵ｺ・ｧ驕擾ｽｭ驍ｵ・ｮ (~80 髯ｦ讙主ｲｼ隶薙・ | 﨟槫｣ｲ 鬨ｾ・ｲ髯ｦ蠕｡・ｸ・ｭ (陋ｻ譎丞ｱ楢･搾ｽｷ陷崎ｼ斐・assets/scenes/*.fbzz 騾墓ｻ薙・陟募ｾ娯・驕擾ｽｭ驍ｵ・ｮ陷ｿ・ｯ) |
| 2 | .fbzz 郢ｧ・ｷ郢晢ｽｼ郢晢ｽｳ郢晁ｼ斐＜郢ｧ・､郢晢ｽｫ邵ｺ・ｮ闖ｴ諛医・ (霑ｴ・ｾ main.cpp 邵ｺ・ｮ郢ｧ・ｷ郢晢ｽｼ郢晢ｽｳ陞ｳ螟ゑｽｾ・ｩ郢ｧ蝣､・ｧ・ｻ隶繝ｻ | 﨟槫｣ｲ 鬨ｾ・ｲ髯ｦ蠕｡・ｸ・ｭ (陋ｻ譎丞ｱ楢･搾ｽｷ陷崎ｼ斐・assets/scenes/ 邵ｺ・ｫ髢ｾ・ｪ陷肴・蜃ｽ隰後・ |

---

## 郢ｧ・ｨ郢晢ｽｳ郢ｧ・ｸ郢晢ｽｳ 郢晢ｽ｢郢ｧ・ｸ郢晢ｽ･郢晢ｽｼ郢晢ｽｫ

髫ｧ・ｳ驍擾ｽｰ: [docs/engine/Design.md](docs/engine/Design.md)

| 郢晢ｽ｢郢ｧ・ｸ郢晢ｽ･郢晢ｽｼ郢晢ｽｫ | 霑･・ｶ隲ｷ繝ｻ| 陋ｯ蜻ｵﾂ繝ｻ|
|-----------|------|------|
| Core / Input / Renderer / Scene 陜難ｽｺ騾ｶ・､ | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ| |
| Asset / Audio / Util | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ| |
| LightComponent / CameraComponent / AudioSourceComponent | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ| docs 邵ｺ・ｮ邵ｲ譴ｧ謔ｴ陞ｳ貅ｯ・｣繝ｻﾂ蟠趣ｽ｡・ｨ髫ｪ蛟･繝ｻ髫ｱ・､郢ｧ繝ｻ|
| AudioSystem free function (scene::AudioSystem) | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ| docs 邵ｺ・ｮ邵ｲ譴ｧ謔ｴ陞ｳ貅ｯ・｣繝ｻﾂ蟠趣ｽ｡・ｨ髫ｪ蛟･繝ｻ髫ｱ・､郢ｧ繝ｻ|
| RenderSystem LightComponent 陷ｿ譛ｱ蟇・(LightSystem 陟大｢鍋・陷台ｼ∝求) | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ| docs 邵ｺ・ｮ邵ｲ譴ｧ謔ｴ陞ｳ貅ｯ・｣繝ｻﾂ蟠趣ｽ｡・ｨ髫ｪ蛟･繝ｻ髫ｱ・､郢ｧ繝ｻ|

---

## 霑夲ｽｩ騾・・縺顔ｹ晢ｽｳ郢ｧ・ｸ郢晢ｽｳ (Step 4 闔会ｽ･鬮ｯ繝ｻ

髫ｧ・ｳ驍擾ｽｰ: [docs/physics/Design.md](docs/physics/Design.md)

| 隶匁ｺｯ繝ｻ | 霑･・ｶ隲ｷ繝ｻ|
|------|------|
| CapsuleCollider | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| Unity-style ColliderComponent / VolumeComponent 陋ｹ繝ｻ| 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|
| Volume 驍会ｽｻ (6 驕橸ｽｮ) | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ(ColliderVolume + VolumeComponent 邵ｺ・ｫ驍ｨ・ｱ陷ｷ繝ｻ |
| Constraint 驍会ｽｻ (5 驕橸ｽｮ) | 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ|

---

## Step 7 闔会ｽ･鬮ｯ繝ｻ

| Step | 陷繝ｻ・ｮ・ｹ | 霑･・ｶ隲ｷ繝ｻ|
|------|------|------|
| 7 | DX12 驕假ｽｻ髯ｦ繝ｻ+ RenderGraph | 隨ｶ繝ｻ隴幢ｽｪ騾ｹﾂ隰・・|
| 7 | DXR (Ray Tracing) | 隨ｶ繝ｻ隴幢ｽｪ騾ｹﾂ隰・・|
| 遯ｶ繝ｻ| Script / Reflect 郢ｧ・ｷ郢ｧ・ｹ郢昴・ﾎ・| 隨ｨ繝ｻ陞ｳ蠕｡・ｺ繝ｻ([docs/scene/Script.md](docs/scene/Script.md)) |

---

## Script / Reflect

| # | Task | Status |
|---|------|--------|
| 1 | Script base / IReflector / ScriptComponent | Done |
| 2 | Scene registration / GameObject AddScript / GetScript | Done |
| 3 | ScriptSystem and SceneManager Update integration | Done |
| 4 | ImGuiReflector and InspectorPanel script section | Done |
| 5 | Sandbox PlayerController sample | Done |
| 6 | SceneSerializer v2 ScriptFactory / TOML reflector | Done |

---

## Step 6.5 Progress

| # | Task | Status |
|---|------|--------|
| 1 | sandbox/main.cpp loads a .fbzz scene through SceneSerializer | Done |
| 2 | Physics verification scene moved to assets/scenes/PhysicsTest.fbzz | Done |
| 3 | Runtime RigidBodyComponent bootstrap for PhysicsTest | Done |

---

## Panel Refactoring

| # | Task | Status |
|---|------|--------|
| 1 | IPanel template-method lifecycle (GetWindowName / OnRenderContent / OnInit / OnShutdown) | Done |
| 2 | InspectorPanel DrawComponentSection template refactor | Done |
| 3 | Engine Component GetTypeName / Reflect entry points | Done |
| 4 | EditorContext helper methods (HasActiveScene / GetSelectedGO) | Done |
| 5 | Scene::GetComponents<T>() query helper | Done |
| 6 | LightComponent editing remains inside the selected object's Inspector component section | Done |
| 7 | Standalone LightPanel removed | Done |

---

## Work Log

| Date | Task | Status |
|------|------|--------|
| 2026-05-22 | Split editor viewport into Scene view and Game view rendered from the scene MainCamera | Done |
| 2026-05-22 | Focus the Game viewport automatically when Play is pressed | Done |
| 2026-05-22 | Fix PlayMode snapshot restore dropping MeshRenderer primitive mesh and shader data | Done |
| 2026-05-22 | Add aspect ratio selection to the Game viewport | Done |
| 2026-05-22 | Add Unity-style Game viewport resolution presets including Full HD | Done |
| 2026-05-22 | Implement ResourceSystem handle-based renderer resource management | Done |
| 2026-05-22 | Fix particle vertex buffer leak and lazy-initialize Material params buffer | Done |
| 2026-05-22 | Fix MissingFeatures A-1/A-2 renderer API and AssetBrowser scene open bugs | Done |
| 2026-05-23 | Implement MissingFeatures D-7-1 OBB DebugDraw box overload | Done |
| 2026-05-23 | Connect MissingFeatures B-1/B-2 showColliders and showLightRange debug drawing | Done |
| 2026-05-23 | Fix Capsule-Capsule closest-point contact torque bias | Done |
| 2026-05-23 | Implement MissingFeatures D-9-1/D-9-2 collision and trigger script callbacks | Done |
| 2026-05-23 | Implement feature/layer-system GameObject layers, culling masks, physics filtering, serializer, and Inspector UI | Done |
| 2026-05-23 | Add Inspector Add Component menu and Hierarchy template object creation menu | Done |
| 2026-05-23 | Fix Hierarchy delete handling and add GameObject order movement operations | Done |
| 2026-05-23 | Add Hierarchy tree display, drag parent assignment, and Set As Root action | Done |
| 2026-05-23 | Guard Hierarchy recursion and route all parent assignment through cycle-safe GameObject API | Done |
| 2026-05-23 | Implement Phase 1 runtime UI components, UISystem, Inspector/Serializer integration, and Game viewport submission | Done |
| 2026-05-23 | Add UI viewport tab and UI-only render target preview | Done |
| 2026-05-23 | Fix UISprite canvas-space projection and add sandbox 3D+UI overlay verification scene | Done |
| 2026-05-23 | Fix Logger macros for MSVC builds without /Zc:preprocessor | Done |
| 2026-05-23 | Fix UI culling and sandbox Play/Stop stale button references | Done |
| 2026-05-23 | Add UIText debug rendering and remove sandbox UI debug logs | Done |
| 2026-05-23 | UI Phase 2: dynamic VB (persistent Map/Unmap), SDF font atlas (CPU-generated 24x24 glyphs, UIText.hlsl), Anchor/Pivot layout, texturePath Inspector, UILayoutGroup (H/V), UIAnimator (Color/Position Tween), World Space Canvas RenderMode, full Inspector and Serializer integration | Done |
| 2026-05-24 | Fix physics restitution warm-start caching and ConvexHull EPA/contact normal stability | Done |
| 2026-05-24 | Fix EPA cube normal refinement and restitution test contact-height sampling | Done |
| 2026-05-24 | Visualize Physics Tests Phase 1-11 in Sandbox main scene | Done |
| 2026-05-24 | Fix AABB contact point and lock AABB angular response | Done |
| 2026-05-24 | Fix Sandbox Play/Stop physics world constraint rebinding | Done |
| 2026-05-24 | Add ColliderDebugGeometry and wire it to View > Colliders debug rendering | Done |
| 2026-05-24 | Add ConstraintDebugGeometry and Sandbox constraint debug rendering | Done |
| 2026-05-24 | Implement Skeletal Animation v1 core: Assimp bone/clip import, AnimatorComponent, GPU skinning shaders, RenderSystem and SceneSerializer integration | Done |
| 2026-05-24 | Skeletal Animation v2: nodeGlobalTransforms cache, AnimatorDebugDrawSystem (bone line draw + View > Skeleton toggle), Inspector clip browser with Combo + ProgressBar | Done |
| 2026-05-24 | Fix per-object MaterialComponent synchronization and duplicate material isolation | Done |
| 2026-05-25 | Audit RenderSystem and shader compile/load paths; rewrite broken comments | Done |
