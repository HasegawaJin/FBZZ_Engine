/// @file    RenderOperators.cpp
/// @brief   デバッグ表示とビューモードの Operator。
/// @author  Hasegawa Jin
/// @date    2026-08-22

/// @note 移行前は Debug メニューにしか無く、AI は viewport を撮れても NavMesh/Collider/overdraw 等の診断表示を選べなかった。
/// @note 表示の切り替えはシーンの内容を変えないので Undo には載せない (Action)。
/// @see Docs/design/editor-operator-model.md
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>

#include <string>
#include <utility>

namespace fbzz::editor {

namespace {

/// @brief 真偽値 1 つを切り替える / 明示設定する操作の共通部分 (enabled 省略で反転、指定でその値)。
/// @note 反転専用だと、読み取り手段の無い項目で AI が ON を保証するには 2 回撮って比べるしかない。
EditorOperator MakeToggleBase(const char* id, const char* label, const char* desc)
{
    EditorOperator op;
    op.id       = id;
    op.label    = label;
    op.category = "Render";
    op.desc     = desc;
    op.kind     = OpKind::Action;

    OpParam enabledParam;
    enabledParam.name     = "enabled";
    enabledParam.type     = OpParamType::Bool;
    enabledParam.desc     = "省略すると現在値を反転する";
    enabledParam.required = false;
    op.params = { enabledParam };
    return op;
}

OpResult ApplyToggle(bool& value, const OpArgs& args)
{
    value = args.Has("enabled") ? args.GetBool("enabled") : !value;
    OpResult result;
    result.message = value ? "有効にしました" : "無効にしました";
    return result;
}

/// @brief ProjectSettings 側が権威の診断フラグ。
EditorOperator MakeRenderToggle(const char* id, const char* label, const char* desc,
                                bool renderer::RenderSettings::*field)
{
    EditorOperator op = MakeToggleBase(id, label, desc);
    op.exec = [field](OpContext& c, const OpArgs& args) -> OpResult {
        return ApplyToggle(c.ctx.projectSettings.render.*field, args);
    };
    /// @note メニューのチェックと op.list の checked が同じ式から出る。
    op.checked = [field](const OpContext& c, const OpArgs&) {
        return c.ctx.projectSettings.render.*field;
    };
    return op;
}

EditorOperator MakeShadowToggle()
{
    EditorOperator op = MakeToggleBase("render.shadow_enabled", "Shadows",
        "シャドウマップの有効・無効。アセット割当時の品質設定は Inspector で編集する。");
    const auto playing = [](const OpContext& context) {
        return context.ctx.playMode && !context.ctx.playMode->IsInEditor();
    };
    op.poll = [playing](const OpContext& context, const OpArgs&) {
        if (playing(context)) return core::Application::Get().GetActiveRenderSettings() != nullptr;
        renderer::RenderSettings resolved;
        return !asset::ResolveRenderPipelineSettings(context.ctx.projectSettings.render,
            context.ctx.projectSettings.renderPipelineAssetPath, resolved);
    };
    op.exec = [playing](OpContext& context, const OpArgs& args) -> OpResult {
        if (playing(context)) {
            auto* runtime = core::Application::Get().GetActiveRenderSettings();
            if (!runtime) return OpResult::Err("NO_RUNTIME_SETTINGS", "実行中の描画設定がありません");
            return ApplyToggle(runtime->shadowEnabled, args);
        }
        renderer::RenderSettings resolved;
        if (asset::ResolveRenderPipelineSettings(context.ctx.projectSettings.render,
            context.ctx.projectSettings.renderPipelineAssetPath, resolved))
            return OpResult::Err("PIPELINE_ASSET_OWNS_SETTING", "影の品質設定は Render Pipeline Asset の Inspector で編集してください");
        return ApplyToggle(context.ctx.projectSettings.render.shadowEnabled, args);
    };
    op.checked = [playing](const OpContext& context, const OpArgs&) {
        if (playing(context)) {
            const auto* runtime = core::Application::Get().GetActiveRenderSettings();
            return runtime && runtime->shadowEnabled;
        }
        renderer::RenderSettings resolved;
        (void)asset::ResolveRenderPipelineSettings(context.ctx.projectSettings.render,
            context.ctx.projectSettings.renderPipelineAssetPath, resolved);
        return resolved.shadowEnabled;
    };
    return op;
}

/// @brief EditorContext 側が権威のフラグ。
/// @note Grid/Skeleton/LightRange/VFXGizmos は EditorContext が正本で、毎フレーム sceneRenderSettings へ複写される (EditorApp::RenderSceneView)。RenderSettings 側へ書くと次フレームで上書きされる。
EditorOperator MakeContextToggle(const char* id, const char* label, const char* desc,
                                 bool EditorContext::*field)
{
    EditorOperator op = MakeToggleBase(id, label, desc);
    op.exec = [field](OpContext& c, const OpArgs& args) -> OpResult {
        return ApplyToggle(c.ctx.*field, args);
    };
    op.checked = [field](const OpContext& c, const OpArgs&) { return c.ctx.*field; };
    return op;
}

} /// @note namespace

void RegisterRenderOperators(OperatorRegistry& registry)
{
    /// @name EditorContext が正本のもの
    registry.Register(MakeContextToggle(
        "render.show_grid", "Show Grid",
        "Scene View のグリッド表示。", &EditorContext::showGrid));

    registry.Register(MakeContextToggle(
        "render.show_skeleton", "Show Skeleton",
        "スケルトンのボーンを描く。animation_get_pose の数値と絵を突き合わせる。",
        &EditorContext::showSkeleton));

    registry.Register(MakeContextToggle(
        "render.show_light_range", "Show Light Range",
        "ライトの影響範囲を描く。「暗い」の原因が range か intensity かを分ける。",
        &EditorContext::showLightRange));

    registry.Register(MakeContextToggle(
        "render.show_vfx_gizmos", "Show VFX Gizmos",
        "エミッターの発生形状と初速を描く。流れの場は render.show_flow_fields。",
        &EditorContext::showVFXGizmos));

    registry.Register(MakeContextToggle(
        "render.show_flow_fields", "Show Flow Fields",
        "FlowField の効く範囲 (球 / Baked の箱)・影響度 50% の破線球・流れの向きを描く。"
        "channels を絞った場は破線、速度場 PNG が読めない Baked は赤い対角線。",
        &EditorContext::showFlowFields));

    registry.Register(MakeContextToggle(
        "render.show_flow_samples", "Show Flow Samples",
        "格子点で実効流速 (環境流・重ねた場の合計) を矢印で描く。色は速さ (青→赤で 0〜10 m/s)。"
        "範囲は選択中の FlowField、無ければカメラ前方。",
        &EditorContext::showFlowSamples));

    registry.Register(MakeContextToggle(
        "render.show_physics_volumes", "Show Physics Volumes",
        "VolumeComponent のトリガー形状と効果 (重力・渦・爆風・時間・磁場) の向き、duration の残りを描く。"
        "トリガーが無い・判定できない形状の Volume は赤で出る。",
        &EditorContext::showPhysicsVolumes));

    registry.Register(MakeContextToggle(
        "render.show_water_flow", "Show Water Flow",
        "全 WaterComponent の範囲・水流 (波面上の矢印)・渦・浮力の届く深さ (破線) を描く。"
        "選択中だけの水面ギズモと違い、選択に依らず出る。",
        &EditorContext::showWaterFlow));

    registry.Register(MakeContextToggle(
        "render.show_ragdoll", "Show Ragdoll",
        "ラグドールの剛体・関節の可動域・接触点を描く。"
        "トルク上限に張り付いた関節が赤くなるので、どこが力負けしたかが位置で分かる。",
        &EditorContext::showRagdoll));

    registry.Register(MakeContextToggle(
        "render.show_script_gizmos", "Show Script Gizmos",
        "スクリプトの OnDrawGizmos / OnDrawGizmosSelected と debug.Draw* を Scene View に描く。"
        "Game View と配布ビルドには出ない。", &EditorContext::showScriptGizmos));

    registry.Register(MakeContextToggle(
        "render.skeleton_selected_only", "Skeleton: Selected Only",
        "スケルトン表示を選択中のキャラクターだけに絞る。", &EditorContext::skeletonSelectedOnly));

    registry.Register(MakeContextToggle(
        "render.show_constraints", "Show Constraints",
        "物理拘束を描く。編集中は JointComponent の設定値 (アンカー・軸・可動域) から描く。",
        &EditorContext::showConstraints));

    registry.Register(MakeContextToggle(
        "render.show_rigid_bodies", "Show Rigid Bodies",
        "選択中の剛体の速度 (1 秒後の到達点)・角速度・重心を描く。", &EditorContext::showRigidBodies));

    registry.Register(MakeContextToggle(
        "render.show_ik", "Show IK Chains",
        "IKSolver のチェーン・ターゲット・ポールを描く。", &EditorContext::showIK));

    registry.Register(MakeContextToggle(
        "render.show_spring_bones", "Show Spring Bones",
        "SpringBone の揺れ骨とコライダーを描く。", &EditorContext::showSpringBones));

    registry.Register(MakeContextToggle(
        "render.show_attachments", "Show Attachments",
        "SocketAttachment / TransformConstraint の追従先への線を描く。", &EditorContext::showAttachments));

    registry.Register(MakeContextToggle(
        "render.show_vfx_paths", "Show VFX Paths",
        "選択中の Trail / MeshTrail / LineRenderer / VFXLine / VFXBeam の経路を描く。", &EditorContext::showVFXPaths));

    registry.Register(MakeContextToggle(
        "render.show_terrain_bounds", "Show Terrain Bounds",
        "Terrain の範囲を箱で描く。", &EditorContext::showTerrainBounds));

    registry.Register(MakeContextToggle(
        "render.show_lod_bounds", "Show LOD Bounds",
        "LODGroup の判定球を現在の LOD 段の色で描く。", &EditorContext::showLODBounds));

    /// @note Debug メニューにはあるが operator が無く、Grid/Skeleton と同じ列なのに AI からだけ触れなかった。Stats は FPS/draw call を Game View へ焼き込むので、viewport_capture の絵に性能値を一緒に写せる。
    registry.Register(MakeContextToggle(
        "render.show_stats", "Show Stats",
        "Game Viewport へ FPS / draw call のオーバーレイを出す。",
        &EditorContext::showStats));

    registry.Register(MakeContextToggle(
        "render.show_scene_icons", "Show Scene Icons",
        "Scene View のコンポーネントアイコン (ライト・カメラ・音源など)。消すとアイコンのクリック選択も止まる。",
        &EditorContext::showSceneIcons));

    registry.Register(MakeContextToggle(
        "render.scene_view_occlusion_culling", "Scene View Occlusion Culling",
        "Scene View で CPU オクルージョンカリングを効かせる。既定は OFF。"
        "遮蔽者はメッシュ実体ではなくバウンディング球の近似なので、"
        "有効にすると見えているものが消えることがある。"
        "「消えた原因がカリングか」を切り分けるときに入れ切りする。"
        "Scene View はデバッグカメラで描くため CameraComponent の設定は効かず、"
        "編集ビューでの有効・無効はここでしか切り替えられない。",
        &EditorContext::sceneViewOcclusionCulling));

    /// @note 描画でなくスクリプト DLL の監視のため Render でなく Tools。Play 前に自動リロードを止めたい場面 (途中の再構築で NodeId が無効化) があるので切り替え手段を出す。
    {
        EditorOperator op = MakeContextToggle(
            "debug.hot_reload", "Hot Reload",
            "スクリプト DLL の自動ホットリロード監視。OFF にすると、"
            "編集中に勝手な再ロードでシーンが作り直されるのを防げる。",
            &EditorContext::hotReloadEnabled);
        op.category = "Tools";
        registry.Register(std::move(op));
    }
    {
        EditorOperator op = MakeContextToggle(
            "debug.hot_reload_sound", "Hot Reload Sound",
            "スクリプト / シェーダーのホットリロードが終わったとき (成功・失敗) に音を鳴らす。"
            "エディターを見ていなくても完了に気づける。",
            &EditorContext::hotReloadSound);
        op.category = "Tools";
        registry.Register(std::move(op));
    }

    /// @name ProjectSettings が正本のもの
    registry.Register(MakeRenderToggle(
        "render.show_colliders", "Show Colliders",
        "Collider の形状をワイヤーで描く。"
        "「当たらない」の原因が形状かレイヤーかを切り分ける最初の一手。"
        "緑 = 静的 / 黄 = 動く剛体 / 暗い黄 = 眠り / 紫 = トリガー。",
        &renderer::RenderSettings::showColliders));

    registry.Register(MakeRenderToggle(
        "render.show_terrain_collision", "Show Terrain Collision",
        "地形のコリジョン形状を描く。", &renderer::RenderSettings::showTerrainCollision));

    registry.Register(MakeRenderToggle(
        "render.show_navmesh", "Show NavMesh",
        "NavMesh の歩行可能面を描く。navmesh_find_path が found=false を返したとき、"
        "穴がどこにあるのかは絵でしか判らない。Play 中の Scene View でも描ける。",
        &renderer::RenderSettings::showNavMesh));

    registry.Register(MakeRenderToggle(
        "render.show_nav_sensors", "Show AI Sensors",
        "NavMeshSensor の視界・聴覚範囲を描く。"
        "「敵が気づかない」の原因がセンサー範囲か BT の条件かを切り分ける。",
        &renderer::RenderSettings::showNavSensors));

    registry.Register(MakeRenderToggle(
        "render.show_decal_bounds", "Show Decal Bounds",
        "デカールの投影ボックスを描く。", &renderer::RenderSettings::showDecalBounds));

    registry.Register(MakeShadowToggle());

    registry.Register(MakeRenderToggle(
        "render.particle_overdraw_view", "Particle Overdraw View",
        "パーティクルの重なり枚数をヒートマップで上書きする。"
        "パーティクルの実コストは粒子数ではなく塗った画素数で決まるが、"
        "重なりは通常の絵からは読めない。",
        &renderer::RenderSettings::particleOverdrawView));

    /// @name ビューモード
    /// @note 排他選択と現在値の判定を同じ操作へ集約する。
    {
        EditorOperator op;
        op.id       = "render.set_view_mode";
        op.label    = "Set View Mode";
        op.category = "Render";
        op.desc     = "通常描画とレイ交差の診断表示を切り替える。";
        op.kind     = OpKind::Action;

        OpParam modeParam;
        modeParam.name = "mode";
        modeParam.type = OpParamType::String;
        modeParam.desc = "描画モード";
        /// @note AI バスの候補提示と入力検証もこの宣言を使う。
        modeParam.enumValues = { "lit", "unlit", "wireframe_lit", "wireframe_unlit",
            "ray_hit_distance", "ray_geometric_normal", "ray_instance_id" };
        op.params = { modeParam };

        /// @note exec と checked は同じ文字列と enum の対応を使う。
        const auto toViewMode = [](const std::string& mode) {
            if (mode == "unlit")           return renderer::ViewMode::Unlit;
            if (mode == "wireframe_lit")   return renderer::ViewMode::WireframeLit;
            if (mode == "wireframe_unlit") return renderer::ViewMode::WireframeUnlit;
            if (mode == "ray_hit_distance") return renderer::ViewMode::RayHitDistance;
            if (mode == "ray_geometric_normal") return renderer::ViewMode::RayGeometricNormal;
            if (mode == "ray_instance_id") return renderer::ViewMode::RayInstanceId;
            return renderer::ViewMode::Lit;
        };

        op.exec = [toViewMode](OpContext& c, const OpArgs& args) -> OpResult {
            /// @note 未知の綴りは ValidateArgs (enumValues) が入口で弾く。
            c.ctx.projectSettings.render.viewMode = toViewMode(args.GetString("mode"));
            return OpResult::Ok();
        };
        op.checked = [toViewMode](const OpContext& c, const OpArgs& args) {
            if (!args.Has("mode")) return false;
            return c.ctx.projectSettings.render.viewMode == toViewMode(args.GetString("mode"));
        };
        registry.Register(std::move(op));
    }

    /// @note NavMesh の可視性と診断表示の種類は独立して指定する。
    {
        EditorOperator op;
        op.id       = "render.set_navmesh_draw_mode";
        op.label    = "Set NavMesh Draw Mode";
        op.category = "Render";
        op.desc     = "NavMesh オーバーレイの描き方を切り替える。"
                      "areas は NavMesh Modifier の areaType 塗り分けを、"
                      "portals はポリゴン同士の接続を、"
                      "voxels はベイクのセル判定 (急斜面 / 段差 / 障害物 / 半径不足) を出す。"
                      "navmesh_find_path が found=false のとき、経路が通らない理由を絵で分ける。";
        op.kind     = OpKind::Action;

        OpParam modeParam;
        modeParam.name       = "mode";
        modeParam.type       = OpParamType::String;
        modeParam.desc       = "描き方";
        modeParam.enumValues = { "solid", "transparent", "areas", "portals", "voxels" };
        op.params = { modeParam };

        const auto toDrawMode = [](const std::string& mode) {
            if (mode == "transparent") return renderer::NavMeshDrawMode::Transparent;
            if (mode == "areas")       return renderer::NavMeshDrawMode::Areas;
            if (mode == "portals")     return renderer::NavMeshDrawMode::Portals;
            if (mode == "voxels")      return renderer::NavMeshDrawMode::Voxels;
            return renderer::NavMeshDrawMode::Solid;
        };
        op.exec = [toDrawMode](OpContext& c, const OpArgs& args) -> OpResult {
            c.ctx.projectSettings.render.navMeshDrawMode = toDrawMode(args.GetString("mode"));
            /// @note 描き方だけ変えても表示が消えていれば何も起きないので、同時に点ける。
            c.ctx.projectSettings.render.showNavMesh = true;
            return OpResult::Ok();
        };
        op.checked = [toDrawMode](const OpContext& c, const OpArgs& args) {
            if (!args.Has("mode")) return false;
            return c.ctx.projectSettings.render.navMeshDrawMode == toDrawMode(args.GetString("mode"));
        };
        registry.Register(std::move(op));
    }

    /// @note NavMesh のベイクは描画ではないが、登録先を分けると専用グループと EditorApp 登録列の二重管理が要る。debug.hot_reload と同じく category だけ Tools に寄せる。
    {
        EditorOperator op;
        op.id       = "navmesh.bake_all";
        op.label    = "Bake All NavMesh";
        op.category = "Tools";
        op.desc     = "シーン内の有効な NavMesh Surface をすべて再ベイクする。"
                      "地形を彫った後・コライダーを動かした後は、古い NavMesh のまま経路が引かれる。"
                      "ベイクはバックグラウンドで走るため、この操作の完了は開始の完了でしかない。";
        op.kind     = OpKind::Action;
        op.poll = [](const OpContext& c, const OpArgs&) { return c.ctx.activeScene != nullptr; };
        op.exec = [](OpContext& c, const OpArgs&) -> OpResult {
            scene::Scene* activeScene = c.ctx.activeScene;
            if (!activeScene) return OpResult::Err("NO_SCENE", "シーンが開かれていません");
            int queued = 0;
            for (scene::EntityID eid : activeScene->GetEntities<scene::NavMeshSurfaceComponent>()) {
                auto* surface = activeScene->GetComponent<scene::NavMeshSurfaceComponent>(eid);
                if (!surface || !surface->enabled) continue;
                surface->needsBake = true;
                ++queued;
            }
            if (queued == 0)
                return OpResult::Err("NO_NAVMESH_SURFACE",
                                     "有効な NavMesh Surface がシーンにありません");
            OpResult result;
            result.message = std::to_string(queued) + " 個の Surface のベイクを開始しました";
            return result;
        };
        registry.Register(std::move(op));
    }
}

} /// @note namespace fbzz::editor
