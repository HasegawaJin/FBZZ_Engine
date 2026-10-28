<!-- @file    project-code-simplification.md
@brief   機能ごとの重複候補数と既存契約を維持する共通化の記録。
@author  Hasegawa Jin
@date    2026-10-10 -->
# プロジェクト全体の処理共通化

2026-10-10 に code-simplifier の方針で全体を走査した。対象の重複処理 **54 種類、167 実装箇所を 54 個の共通処理へ集約**した。113 個の重複実装を共通の計算へ寄せ、型付き入口・Shader entrypoint・所有者の制御は維持する。実行時間・FPS の改善率はこの集計から推定しない。

## 集計方法

最初の走査は 2,638 ソース、23,141 個の関数本体候補。コメントと空白を除いて48字句以上の本体が完全一致する **89 群・223 箇所**を抽出し、引数型・外部関数・精度・所有権・依存方向を実ソースで確認した。字句一致は意味上の同一性の証明ではなく、全関数やあらゆる類似処理を数える AST 解析でもない。短い処理（Hash21など）は関連箇所を確認する際に追加で扱う。

C++ / C# / TypeScript / JavaScript / HLSL を本体比較対象にした。PowerShell と CMake はファイル数のみ。さらに 16 個の CMakeLists / Python ファイルを索引として確認した（Python の関数定義候補は19個）。ThirdParty、build、Library、bin、obj、node_modules、旧 GreenWare、SDK 0.9.1 と既に生成された配布物は対象外。

再走査は 2,683 ソース、36 群・74 箇所。この時点の全体スナップショットには並行作業の増減も含むため、全体差分を本作業だけの削減量とは扱わない。以下の共通化台帳を本作業の件数とする。

## プロジェクト別の最初の候補数

| 機能 | ファイル | 関数本体候補 | 重複群に含まれた箇所 |
|---|---:|---:|---:|
| CMake | 16 | 0 | 0 |
| Core | 33 | 217 | 0 |
| DevTools | 11 | 29 | 0 |
| Editor | 456 | 4898 | 39 |
| EditorLauncher | 5 | 13 | 0 |
| EditorMcp | 13 | 7 | 0 |
| Engine | 809 | 6472 | 53 |
| Fluid | 18 | 155 | 2 |
| GameHub | 30 | 44 | 12 |
| Graphics | 280 | 1741 | 3 |
| Managed | 165 | 1696 | 0 |
| Math | 25 | 106 | 1 |
| Physics | 83 | 544 | 2 |
| Sandbox | 4 | 6 | 0 |
| Shaders | 282 | 907 | 64 |
| Tests | 404 | 6306 | 47 |
| Tools | 4 | 0 | 0 |

## 共通化した処理の領域別カウント

| 機能 | 処理の種類 | 前 | 後 | 削減箇所 |
|---|---:|---:|---:|---:|
| Editor | 12 | 25 | 12 | 13 |
| Engine | 20 | 53 | 20 | 33 |
| Fluid | 1 | 2 | 1 | 1 |
| Core | 3 | 10 | 3 | 7 |
| Shaders | 15 | 66 | 15 | 51 |
| Graphics | 1 | 2 | 1 | 1 |
| Math | 1 | 2 | 1 | 1 |
| Tests | 1 | 7 | 1 | 6 |

## 実装した共通化

表の前後は計算本体の実装箇所数。既存 helper の利用と、型付き入口を残した委譲を含む。

| 機能 | 処理 | 前 | 後 | 共通化先 |
|---|---|---:|---:|---|
| Editor/Import | Assimp 行列転記 | 2 | 1 | `src/Import/AssimpNodeExport.hpp` |
| Editor/Import | Assimp ノード走査 | 2 | 1 | `src/Import/AssimpNodeExport.hpp` |
| Editor/Util | ローカル Transform 編集値の厳密比較 | 3 | 1 | `Editor/Util/TransformAuthoringValues.hpp` |
| Editor/Util | マテリアル保存名 | 2 | 1 | `Editor/Util/AssetPath.hpp` |
| Editor/Util | Prefab / VFX 保存名 | 2 | 1 | `Editor/Util/AssetPath.hpp` |
| Editor/Util | 親ディレクトリの末尾区切り除去 | 2 | 1 | `Editor/Util/AssetPath.hpp` |
| Editor/Util | アセットパスのドラッグデータ読み取り | 2 | 1 | `Editor/Util/AssetDragPayload.hpp` |
| Engine/Scene/Systems | スキン描画コンポーネント探索 | 3 | 1 | `src/Scene/Systems/BonePoseHelpers.hpp` |
| Engine/Scene/Systems | SpringBone / Ragdoll のボーン名正規化 | 2 | 1 | `src/Scene/Systems/BonePoseHelpers.hpp` |
| Engine/Scene/Systems | ボーン行列再計算 | 3 | 1 | `src/Scene/Systems/BonePoseHelpers.cpp` |
| Engine/Scene/Systems | ボーン姿勢の書き込み | 3 | 1 | `src/Scene/Systems/BonePoseHelpers.hpp` |
| Engine/Scene/Systems | ボーン GameObject 取得 | 2 | 1 | `src/Scene/Systems/BonePoseHelpers.hpp` |
| Engine/Util | 保存浮動小数点の丸め | 3 | 1 | `src/Util/TomlFloatNormalization.cpp` |
| Engine/Util | TOML の再帰的数値正規化 | 3 | 1 | `src/Util/TomlFloatNormalization.hpp` |
| Fluid | ソースマスク読み込み | 2 | 1 | `Fluid/FluidSourceMask.hpp` |
| Engine/Util | 単位円内のサンプリング | 2 | 1 | `src/Util/Random.cpp` |
| Engine/Util | 単位円上のサンプリング | 2 | 1 | `src/Util/Random.cpp` |
| Engine/Util | 単位球内のサンプリング | 2 | 1 | `src/Util/Random.cpp` |
| Engine/Util | 単位球上のサンプリング | 2 | 1 | `src/Util/Random.cpp` |
| Core/Util | 大文字小文字を無視する文字列比較 | 3 | 1 | `Core/Util/StringUtils.hpp` |
| Core/Util | 文字列の小文字化 | 4 | 1 | `Core/Util/StringUtils.hpp` |
| Engine/Util | 符号・整数幅を維持する全消費整数解析 | 5 | 1 | `Engine/Util/NumberParsing.hpp` |
| Shaders | 4 本のボーンパレット混合 | 21 | 1 | `Common/Skinning.hlsli` |
| Shaders | normalize 系スキン材質頂点変換 | 9 | 1 | `Common/MaterialVertex.hlsli` |
| Shaders | 標準表面材質頂点変換 | 10 | 1 | `Common/MaterialVertex.hlsli` |
| Shaders | SafeNormalize 系スキン材質頂点変換 | 3 | 1 | `Common/MaterialVertex.hlsli` |
| Shaders | Fluid 障害物の距離数学 | 2 | 1 | `Bake/Fluid/FluidColliderMath.hlsli` |
| Shaders | CurlNoise3D | 2 | 1 | `Rendering/ParticleNoise.hlsli` |
| Shaders | Sky の回転・Reversed-Z 投影 | 3 | 1 | `Common/Space.hlsli` |
| Shaders | 雲 / 雲影の値ノイズ | 2 | 1 | `Common/Random.hlsli` |
| Shaders | Dissolve 固有の値ノイズ | 2 | 1 | `Rendering/DissolveNoise.hlsli` |
| Shaders | Dissolve 固有の Hash21 | 2 | 1 | `Rendering/DissolveNoise.hlsli` |
| Shaders | Velocity のシーン遮蔽 | 2 | 1 | `Common/Space.hlsli` |
| Shaders | Mask のスキン位置変換 | 2 | 1 | `Common/Skinning.hlsli` |
| Shaders | Fullscreen 三角形頂点変換 | 2 | 1 | `Common/Fullscreen.hlsli（既存）` |
| Shaders | DissolveMask のノイズ積和 | 2 | 1 | `Rendering/DissolveNoise.hlsli` |
| Shaders | atlas UV を直接渡す UI 頂点変換 | 2 | 1 | `UI/UICommon.hlsli` |
| Core/Util | 区切り末尾の契約を保持するパス比較 | 3 | 1 | `Core/Util/FileSystem.hpp` |
| Engine/Scene/Managed | packet の境界検査と POD 読み取り | 4 | 1 | `src/Scene/Managed/ManagedPacketRead.hpp` |
| Graphics/Passes/Geometry | ForwardDraw | 2 | 1 | `Projects/Graphics/src/Passes/Geometry/RenderSceneDraw.cpp` |
| Engine/Scene/Systems | WriteWorldPoseToTransform | 2 | 1 | `Projects/Engine/src/Scene/Systems/WorldPoseHelpers.cpp` |
| Engine/Scene/Systems | FindLoadedClip / FindClipBySource | 2 | 1 | `Projects/Engine/src/Scene/Systems/AnimationClipHelpers.cpp` |
| Engine/Scene | ExecuteProfiledCallback (typed argument) | 5 | 1 | `Projects/Engine/src/Scene/Script.cpp::ExecuteProfiledCallbackWithArgument` |
| Engine/Scene/Systems | TryAddTerrainHeightSample | 2 | 1 | `Projects/Engine/src/Scene/Systems/TerrainHeightHelpers.cpp` |
| Editor/Fluid | EvaluateRamp | 2 | 1 | `Projects/Editor/include/Editor/Util/FluidRecipeWidgets.hpp : fluidui::EvaluateRamp` |
| Editor/Fluid | PartCount / ListCount | 2 | 1 | `Projects/Editor/include/Editor/Util/FluidDocument.hpp : CountFluidParts` |
| Math/UI/Particles | LerpV4 / LerpVec4 | 2 | 1 | `Projects/Math/include/Math/Vector4.hpp : Vector4::Lerp` |
| Editor/Status | LocalClockNow | 2 | 1 | `Projects/Editor/include/Editor/Util/LocalClock.hpp : LocalClockNow` |
| Engine/UI + Editor/Viewport | Rotate2D / RotateUIVector | 2 | 1 | `Projects/Engine/include/Engine/Scene/Components/UIRect.hpp : RotateUIVector` |
| Engine/UI + Editor/Viewport | ExtractZRotation / ExtractUIZRotation | 2 | 1 | `Projects/Engine/include/Engine/Scene/Components/UIRect.hpp : ExtractUIZRotation` |
| Editor/Panels/Inspector | Mesh/Hull Collider Undo apply | 2 | 1 | `Projects/Editor/src/Panels/Inspector/InspectorPhysics.cpp::ApplyMeshBasedColliderValue` |
| Editor/Panels/Inspector | Capsule/Cylinder Collider draw | 2 | 1 | `Projects/Editor/src/Panels/Inspector/InspectorPhysics.cpp::DrawAxialCollider` |
| Engine/Scene | Mesh/Hull Collider field read | 2 | 1 | `Projects/Engine/src/Scene/SceneSerializer.cpp::ReadMeshColliderFields` |
| Tests/Graphics | Shader source path resolution | 7 | 1 | `Projects/Tests/TestKit/include/TestKit/Graphics/ShaderPath.hpp::ResolveShaderPath` |

## 維持する契約

- Bone の名前変換は IK の `|` / namespace 規則を SpringBone・Ragdoll の規則と混ぜない。骨パレットの上限や空の root 名の扱いも各呼び出し元で維持する。
- 保存は小数6桁への丸め、負のゼロ除去、配列・table の再帰と整数値の保持を維持する。
- 文字列比較は unsigned byte を使う既存の locale 契約を維持し、Unicode 正規化や実ファイル解決を追加しない。既存の StringUtils シグネチャは保持する。
- Random / RandomStream は乱数源の所有を分け、標本の消費順・棄却条件・数式を共有する。
- Shader は `normalize` / `SafeNormalize`、Dissolve 固有 hash、Cloud の FBM 係数、液体と気体の Collider packing、Mask の `xyz → w=1` 再構成順を変えない。
- 配布コピーはビルドから更新される出力で、過去 SDK や生成物を原本として手編集しない。

## 全機能の細分化したカウント

重複箇所数は群が跨ぐ機能ごとに所属箇所を数えるため、群数とは異なる。0件はこの字句比較で検出されなかった意味であり、全ての意味上の重複がないことを示さない。

| 機能 | 初回ファイル | 関数本体候補 | 初回重複箇所 | 再走査重複箇所 |
|---|---:|---:|---:|---:|
| CMake | 16 | 0 | 0 | 0 |
| Core/Memory | 18 | 92 | 0 | 0 |
| Core/Profiler | 6 | 46 | 0 | 0 |
| Core/Root | 4 | 15 | 0 | 0 |
| Core/Util | 5 | 64 | 0 | 0 |
| DevTools/AgentLint | 2 | 26 | 0 | 0 |
| DevTools/BenchReport | 9 | 3 | 0 | 0 |
| Editor/Ai | 32 | 570 | 2 | 2 |
| Editor/GraphEditor | 12 | 55 | 0 | 0 |
| Editor/Import | 24 | 120 | 5 | 0 |
| Editor/Op | 32 | 386 | 0 | 0 |
| Editor/Panels/AiSettings | 2 | 7 | 0 | 0 |
| Editor/Panels/Analysis | 2 | 58 | 0 | 0 |
| Editor/Panels/AnimationGraph | 2 | 106 | 0 | 0 |
| Editor/Panels/AnimationGraphInspector | 1 | 0 | 0 | 0 |
| Editor/Panels/AnimationMaskPreview | 4 | 10 | 0 | 0 |
| Editor/Panels/AnimationPreview | 5 | 101 | 0 | 0 |
| Editor/Panels/AssetBrowser | 7 | 162 | 3 | 0 |
| Editor/Panels/AssetMaintenance | 2 | 10 | 0 | 0 |
| Editor/Panels/BehaviorTree | 2 | 31 | 0 | 0 |
| Editor/Panels/BuildOutput | 2 | 11 | 0 | 0 |
| Editor/Panels/BuildSettings | 2 | 23 | 0 | 0 |
| Editor/Panels/Console | 2 | 31 | 0 | 0 |
| Editor/Panels/CurveAssetPreview | 2 | 4 | 0 | 0 |
| Editor/Panels/DependencyView | 2 | 7 | 0 | 0 |
| Editor/Panels/EditorTool | 1 | 3 | 0 | 0 |
| Editor/Panels/FluidEditor | 10 | 214 | 0 | 0 |
| Editor/Panels/FontPreview | 2 | 6 | 0 | 0 |
| Editor/Panels/HotkeyEditor | 2 | 4 | 0 | 0 |
| Editor/Panels/I | 2 | 18 | 0 | 0 |
| Editor/Panels/IPreview | 1 | 0 | 0 | 0 |
| Editor/Panels/IblBake | 2 | 7 | 0 | 0 |
| Editor/Panels/Inspector | 30 | 268 | 5 | 0 |
| Editor/Panels/MapEditor | 2 | 18 | 0 | 0 |
| Editor/Panels/MapToolCommon | 1 | 2 | 0 | 0 |
| Editor/Panels/MaterialPreview | 2 | 21 | 0 | 0 |
| Editor/Panels/MaterialPreviewCore | 2 | 63 | 0 | 0 |
| Editor/Panels/MemoryDebug | 2 | 5 | 0 | 0 |
| Editor/Panels/Navigation | 2 | 11 | 0 | 0 |
| Editor/Panels/PerformanceProfiler | 2 | 7 | 0 | 0 |
| Editor/Panels/Preview | 2 | 10 | 0 | 0 |
| Editor/Panels/PreviewPanelRenderers | 1 | 0 | 0 | 0 |
| Editor/Panels/ProjectSettings | 2 | 59 | 1 | 0 |
| Editor/Panels/RenderPassViewer | 2 | 50 | 0 | 0 |
| Editor/Panels/SceneHierarchy | 2 | 24 | 2 | 0 |
| Editor/Panels/ScriptProfiler | 2 | 9 | 0 | 0 |
| Editor/Panels/ScriptRequirements | 2 | 5 | 0 | 0 |
| Editor/Panels/SearchEverything | 2 | 6 | 0 | 0 |
| Editor/Panels/Sequence | 2 | 49 | 0 | 0 |
| Editor/Panels/SfxEditor | 2 | 10 | 0 | 0 |
| Editor/Panels/SpriteEditor | 2 | 23 | 0 | 0 |
| Editor/Panels/StatusBar | 2 | 8 | 0 | 0 |
| Editor/Panels/TexturePreview | 2 | 2 | 0 | 0 |
| Editor/Panels/UndoHistory | 2 | 4 | 0 | 0 |
| Editor/Panels/VFXTimeline | 2 | 9 | 0 | 0 |
| Editor/Panels/Viewport | 11 | 144 | 5 | 0 |
| Editor/Panels/VolumeFlipbookBake | 2 | 32 | 0 | 0 |
| Editor/Panels/VolumeFlipbookComparePreview | 2 | 13 | 0 | 0 |
| Editor/Playtest | 8 | 44 | 0 | 0 |
| Editor/Profiler | 4 | 43 | 0 | 0 |
| Editor/Root | 33 | 419 | 4 | 2 |
| Editor/Tools | 12 | 185 | 0 | 0 |
| Editor/Util | 155 | 1411 | 12 | 6 |
| EditorLauncher/Root | 5 | 13 | 0 | 0 |
| EditorMcp/Root | 7 | 2 | 0 | 0 |
| EditorMcp/tests | 6 | 5 | 0 | 0 |
| Engine/AI | 10 | 76 | 0 | 0 |
| Engine/Asset | 170 | 1157 | 5 | 2 |
| Engine/Audio | 15 | 127 | 2 | 0 |
| Engine/Core | 42 | 228 | 0 | 0 |
| Engine/Format | 3 | 1 | 0 | 0 |
| Engine/Input | 10 | 103 | 0 | 0 |
| Engine/Profiler | 9 | 27 | 0 | 0 |
| Engine/Reflection | 1 | 14 | 0 | 0 |
| Engine/Renderer | 54 | 11 | 0 | 0 |
| Engine/Root | 5 | 23 | 1 | 0 |
| Engine/Scene | 271 | 3015 | 13 | 5 |
| Engine/Scene/Managed | 56 | 392 | 8 | 0 |
| Engine/Scene/Systems | 143 | 1131 | 20 | 0 |
| Engine/Util | 20 | 167 | 4 | 0 |
| Fluid/Root | 18 | 155 | 2 | 0 |
| GameHub/GameHub | 11 | 17 | 0 | 0 |
| GameHub/Root | 5 | 0 | 0 | 0 |
| GameHub/Templates | 2 | 14 | 12 | 12 |
| GameHub/main | 6 | 0 | 0 | 0 |
| GameHub/shared | 3 | 1 | 0 | 0 |
| GameHub/ui | 3 | 12 | 0 | 0 |
| Graphics/Effects | 17 | 24 | 0 | 0 |
| Graphics/Graphics | 1 | 1 | 0 | 0 |
| Graphics/Passes/Geometry | 33 | 256 | 1 | 0 |
| Graphics/Passes/PostProcess | 19 | 80 | 0 | 0 |
| Graphics/Passes/RayTracing | 10 | 26 | 0 | 0 |
| Graphics/Pipeline | 31 | 196 | 0 | 0 |
| Graphics/RayTracing | 23 | 97 | 0 | 0 |
| Graphics/Renderer | 106 | 658 | 2 | 2 |
| Graphics/Renderer/Platform | 40 | 403 | 0 | 0 |
| Managed/Fbzz.ManagedBuildTasks | 2 | 16 | 0 | 0 |
| Managed/Fbzz.ManagedDependencyFixtures | 1 | 1 | 0 | 0 |
| Managed/Fbzz.ManagedFixtureUtility | 1 | 0 | 0 | 0 |
| Managed/Fbzz.ManagedFixtures | 29 | 122 | 0 | 0 |
| Managed/Fbzz.ManagedHost | 56 | 616 | 0 | 0 |
| Managed/Fbzz.ManagedInvalidFixtures | 1 | 1 | 0 | 0 |
| Managed/Fbzz.ManagedTests | 34 | 443 | 0 | 0 |
| Managed/Fbzz.ScriptApi | 32 | 381 | 0 | 0 |
| Managed/Fbzz.ScriptGenerator | 5 | 57 | 0 | 0 |
| Managed/Fbzz.ScriptGeneratorTests | 3 | 58 | 0 | 0 |
| Managed/Templates | 1 | 1 | 0 | 0 |
| Math/Root | 25 | 106 | 1 | 1 |
| Physics/Cloth | 2 | 41 | 1 | 1 |
| Physics/Root | 81 | 503 | 1 | 1 |
| Sandbox/Root | 3 | 6 | 0 | 0 |
| Sandbox/Sandbox | 1 | 0 | 0 | 0 |
| Shaders | 282 | 907 | 64 | 0 |
| Tests/Bench | 22 | 202 | 1 | 1 |
| Tests/Core | 30 | 268 | 0 | 0 |
| Tests/Editor | 64 | 1181 | 0 | 0 |
| Tests/Engine | 144 | 2500 | 17 | 17 |
| Tests/Fluid | 3 | 65 | 2 | 2 |
| Tests/Graphics | 65 | 1090 | 21 | 14 |
| Tests/Math | 13 | 193 | 0 | 0 |
| Tests/Physics | 40 | 740 | 4 | 4 |
| Tests/TestKit | 23 | 67 | 2 | 2 |
| Tools | 4 | 0 | 0 | 0 |

## 初回89群の処理判断

IDは初回に本体字句の重複量順で固定した番号。型別の短い入口や独立したテスト期待値を単一の汎用フレームワークへ置換しない。

| ID | 候補関数 | 箇所 | 判断 |
|---:|---|---:|---|
| 1 | `BlendSkinMatrix / GBufferBlendSkinMatrix / MeshTrailBlendSkinMatrix` (Shaders) | 21 | 共通化済み |
| 2 | `VSMain` (Shaders) | 9 | 共通化済み |
| 3 | `VSMain` (Shaders) | 10 | 共通化済み |
| 4 | `VSMain` (Shaders) | 3 | 共通化済み |
| 5 | `ForwardDraw` (Engine/Scene/Systems, Graphics/Passes/Geometry) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 6 | `ExecuteProfiledCallback` (Engine/Scene) | 4 | 共通化済み（型付きの薄い入口は維持） |
| 7 | `TraverseNodes` (Editor/Import) | 2 | 共通化済み |
| 8 | `LiquidColliderDistance / FluidColliderDistance` (Shaders) | 2 | 共通化済み |
| 9 | `Run` (GameHub/Templates) | 2 | 独立配布テンプレートを維持 |
| 10 | `TransformEquals / GizmoTransformEquals / UITransformEquals` (Editor/Panels/Inspector, Editor/Panels/Viewport) | 3 | 共通化済み |
| 11 | `LiquidCurlNoise / FluidCurlNoise` (Shaders) | 2 | 共通化済み |
| 12 | `NormalizeTomlFloats` (Engine/Scene, Engine/Root) | 3 | 共通化済み |
| 13 | `Field` (Tests/Engine) | 3 | テスト内補助の共用候補（本番 Util へは移さない） |
| 14 | `WriteWorldPoseToTransform` (Engine/Scene/Systems) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 15 | `CubePoints` (Tests/Physics) | 3 | テストの独立した入力・期待値・観測を維持 |
| 16 | `EvaluateRamp` (Editor/Util) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 17 | `FindSkinnedMeshRenderer` (Engine/Scene/Systems) | 3 | 共通化済み |
| 18 | `CopyMatrix` (Editor/Import) | 2 | 共通化済み |
| 19 | `RunStandalone` (GameHub/Templates) | 2 | 独立配布テンプレートを維持 |
| 20 | `RecalcBoneMatrix` (Engine/Scene/Systems) | 2 | 共通化済み |
| 21 | `CommitBoneWorldPose` (Engine/Scene/Systems) | 2 | 共通化済み |
| 22 | `ParseProjectPath` (GameHub/Templates) | 2 | 独立配布テンプレートを維持 |
| 23 | `Read` (Tests/Engine) | 2 | テスト内補助の共用候補（本番 Util へは移さない） |
| 24 | `VSMain` (Shaders) | 3 | 共通化済み |
| 25 | `Attach / Add` (Tests/Engine) | 2 | テスト内補助の共用候補（本番 Util へは移さない） |
| 26 | `Field` (Tests/Engine) | 2 | テスト内補助の共用候補（本番 Util へは移さない） |
| 27 | `CloudShadowValueNoise / ValueNoise2D` (Shaders) | 2 | 共通化済み |
| 28 | `FindLoadedClip / FindClipBySource` (Engine/Scene/Systems) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 29 | `Integer / Number` (Engine/Scene/Managed) | 3 | 共通化済み |
| 30 | `Surface` (Tests/Graphics) | 2 | テストの独立した入力・期待値・観測を維持 |
| 31 | `Read` (Tests/Engine) | 2 | テスト内補助の共用候補（本番 Util へは移さない） |
| 32 | `ApplyMeshColliderValue / ApplyConvexHullValue` (Editor/Panels/Inspector) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 33 | `SamePathCI / SameFilePathCI` (Editor/Root, Engine/Asset) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 34 | `ResolveWaterShaderFile / ResolveWaterMediumShaderPath / ResolveProbeShader` (Tests/Graphics) | 3 | 共通化済み（型付きの薄い入口は維持） |
| 35 | `OnLog` (GameHub/Templates) | 2 | 独立配布テンプレートを維持 |
| 36 | `CanonicalBoneName` (Engine/Scene/Systems) | 2 | 共通化済み |
| 37 | `OpenGameLog` (GameHub/Templates) | 2 | 独立配布テンプレートを維持 |
| 38 | `OnLog` (Tests/Graphics) | 2 | テスト内補助の共用候補（本番 Util へは移さない） |
| 39 | `ValueNoise` (Shaders) | 2 | 共通化済み |
| 40 | `SanitizeMaterialFileName` (Editor/Util, Editor/Import) | 2 | 共通化済み |
| 41 | `LoadSourceMasks` (Fluid/Root) | 2 | 共通化済み |
| 42 | `TearDown` (Tests/Graphics) | 2 | Fixture の所有解放順序を維持 |
| 43 | `TryAddTerrainHeightSample` (Engine/Scene/Systems) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 44 | `BeginObjectElement` (Engine/Scene, Editor/Util) | 2 | 反射の保存形式・参照・stack 契約を分離して維持 |
| 45 | `UnloadFromStore / Unload` (Engine/Asset) | 2 | 型・所有元の薄い入口を維持 |
| 46 | `CubePoints / UnitCubePoints` (Tests/Bench, Tests/Physics) | 2 | テストの独立した入力・期待値・観測を維持 |
| 47 | `TearDown` (Tests/Graphics) | 2 | Fixture の所有解放順序を維持 |
| 48 | `EqualsIgnoreCase` (Engine/Audio) | 2 | 共通化済み |
| 49 | `TearDown` (Tests/Graphics) | 2 | Fixture の所有解放順序を維持 |
| 50 | `DiscardIfOccludedByScene` (Shaders) | 2 | 共通化済み |
| 51 | `Field` (Editor/Ai) | 2 | 型・所有元の薄い入口を維持 |
| 52 | `PartCount / ListCount` (Editor/Util) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 53 | `LerpV4 / LerpVec4` (Engine/Scene/Systems) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 54 | `SanitizeEntityName / SanitizeAssetName` (Editor/Panels/AssetBrowser, Editor/Panels/SceneHierarchy) | 2 | 共通化済み |
| 55 | `VSMain` (Shaders) | 2 | 共通化済み |
| 56 | `VSMain` (Shaders) | 2 | 共通化済み |
| 57 | `InsideUnitSphere` (Engine/Util) | 2 | 共通化済み |
| 58 | `OnUnitSphere` (Engine/Util) | 2 | 共通化済み |
| 59 | `Number / Offset` (Editor/Util, Engine/Scene/Managed) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 60 | `FindDefaultProjectPath` (GameHub/Templates) | 2 | 独立配布テンプレートを維持 |
| 61 | `BeginObjectElement` (Engine/Scene, Editor/Util) | 2 | 反射の保存形式・参照・stack 契約を分離して維持 |
| 62 | `MeanX` (Tests/Fluid, Tests/Engine) | 2 | テストの独立した入力・期待値・観測を維持 |
| 63 | `ArrToVec4 / ArrToQuat` (Engine/Scene) | 2 | 型・所有元の薄い入口を維持 |
| 64 | `ReadMeshCollider / ReadConvexHullCollider` (Engine/Scene) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 65 | `Field` (Editor/Util) | 2 | 型・所有元の薄い入口を維持 |
| 66 | `SyncInput / SyncGeometryInput` (Tests/Graphics) | 2 | テストの独立した入力・期待値・観測を維持 |
| 67 | `ReadTransfer / Read` (Engine/Scene/Managed) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 68 | `Read` (Engine/Scene/Managed) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 69 | `SameWave` (Tests/Fluid, Tests/Engine) | 2 | テストの独立した入力・期待値・観測を維持 |
| 70 | `QuatToArr` (Physics/Root, Editor/Util) | 2 | Physics の Math-only 依存境界を維持 |
| 71 | `CompareMatrix3 / CompareMatrix4` (Tests/TestKit) | 2 | 核心は共有済み、型別の薄い比較入口を維持 |
| 72 | `Find / FindMutable` (Editor/Root) | 2 | 型・所有元の薄い入口を維持 |
| 73 | `LocalClockNow` (Editor/Root, Editor/Panels/ProjectSettings) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 74 | `BeginObject` (Engine/Scene, Editor/Util) | 2 | 反射の保存形式・参照・stack 契約を分離して維持 |
| 75 | `Sync` (Tests/Engine) | 2 | テストの独立した入力・期待値・観測を維持 |
| 76 | `DrawCapsuleCollider / DrawCylinderCollider` (Editor/Panels/Inspector) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 77 | `DissolveMask` (Shaders) | 2 | 共通化済み |
| 78 | `Cross` (Physics/Cloth, Math/Root) | 2 | double の Cloth と float の Math を維持 |
| 79 | `TearDown` (Tests/Engine) | 2 | Fixture の所有解放順序を維持 |
| 80 | `DirectoryOf / DirectoryOfPath` (Editor/Panels/AssetBrowser) | 2 | 共通化済み |
| 81 | `ReadAssetPayload` (Editor/Panels/SceneHierarchy, Editor/Panels/Viewport) | 2 | 共通化済み |
| 82 | `VSMain` (Shaders) | 2 | 共通化済み |
| 83 | `LowerCopy` (Engine/Asset) | 2 | 共通化済み |
| 84 | `Rotate2D / RotateUIVector` (Engine/Scene/Systems, Editor/Panels/Viewport) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 85 | `MakeDarkLight / Dark` (Tests/Graphics) | 2 | テストの独立した入力・期待値・観測を維持 |
| 86 | `BucketFor` (Graphics/Renderer) | 2 | 異なる Buffer 型・UAV 検証と所有規則を維持 |
| 87 | `ResolveQualityShader / ResolveUnderwaterShaderFile` (Tests/Graphics) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 88 | `ExtractZRotation / ExtractUIZRotation` (Engine/Scene/Systems, Editor/Panels/Viewport) | 2 | 共通化済み（型付きの薄い入口は維持） |
| 89 | `ResolveTerrainInstanceShader / ResolveMeshletTestShader` (Tests/Graphics) | 2 | 共通化済み（型付きの薄い入口は維持） |

## 検証

<!-- @file    ProjectSimplifierValidation.md
@brief   共通化作業の検証結果を設計記録へ渡す。
@author  Hasegawa Jin
@date    2026-10-10 -->
- Release のコンパイル確認は最終対象74 CPP。73 CPPをまとめた `check-20261010-181711-39008.log` と、追加fixtureの `check-20261010-191844-56192.log` / `check-20261010-194047-19264.log` はいずれもエラー0・警告0。
- `FBZZTestsCoreAuto` / `FluidAuto` / `MathAuto` / `EngineAuto` / `EditorAuto` / `GraphicsAuto` / `GraphicsStandalone` と `FBZZEditorLauncher` の8ターゲットをリンクまで確認。`build-20261010-182113-20888.log` はエラー0・警告0。fixture最終版のStandaloneリンクも `build-20261010-194116-15944.log` で合格。
- `CompileEditorLauncherShaders` は208 HLSL・355 entrypoint jobsの依存キャッシュを確認し、stale=0で合格（`build-20261010-180136-41004.log`）。この実行で355件を新規コンパイルしたという意味ではない。DX12の実デバイステストでは本番Shaderの読み込み・描画結果も確認する。
- CPUの選択回帰は **1,045 / 1,045 合格**。Core / Editor / Engine / Fluid / Math / Physicsの806件は `test-20261010-185217-36336.log`、Graphics CPUの239件は `test-20261010-190047-40588.log`。2つのフィルターは重複しない。
- DX12実デバイスの選択回帰は **172 / 172 合格**、失敗0・実行時間93.356秒。既存のdisabled 1件は未実行として分ける。最終JUnit XMLの `tests=173 / failures=0 / disabled=1 / errors=0` とプロセス終了コード0を確認した。
- リサイズ時の露出履歴はAutoExposureを有効にした設定で検証し、無効化時の履歴破棄とは分けた。ReflectionProbe fixtureは空の一時プロジェクトをAssetManagerで初期化し、共有Engine Assetsを読み取り専用で参照し、終了時にUnloadAllする。共有AssetPathServiceの上書き・解除による後続テストへの影響を除去した。
- GPU検証はプロセス起動前に `FBZZ_ENGINE_ASSET_ROOT=<repository>/Assets` を指定する。GraphicsStandaloneは `OPT_IN` としてCTestから除外されるため、`CMake/FBZZTests.cmake` の契約に従って専用exeを使う。通常CTestから選択される同名CPU fixtureの14件をGPU検証件数へ加算しない。
- AgentLintは変更152ファイルでエラー0・警告0。`git diff --check` と変更対象を絞った差分検査も合格。

検証ログは `build/agent/`、GPUのログとJUnit XMLは `Scratch/ProjectSimplifierGpu.log` / `.xml` に保存した。並行作業で更新された実装の性能改善は、本台帳の共通化へ帰属させない。
