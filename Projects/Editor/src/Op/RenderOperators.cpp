/// @file    RenderOperators.cpp
/// @brief   デバッグ表示とビューモードの Operator。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY: 移行前、これらは Debug メニュー (ImGui / ネイティブ) からしか切り替えられなかった。
/// つまり **AI は viewport を撮れるのに、診断用の表示を出せなかった**。
/// 「敵がここへ来ない」を調べるのに NavMesh を可視化できず、
/// 「当たらない」を調べるのに Collider を出せず、
/// 粒子の重なりを疑っても overdraw ビューへ切り替えられない。
/// AI が持っていたのは「絵を撮る」手段だけで、「何を写すか」を選べなかった。
///
/// 表示の切り替えはシーンの内容を変えないので Undo には載せない (Action)。
/// Docs/design/editor-operator-model.md
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>

#include <string>
#include <utility>

namespace fbzz::editor {

namespace {

// 真偽値 1 つを切り替える / 明示設定する操作の共通部分。
// enabled 引数を省略すると反転、指定すればその値にする。
// WHY 両対応か: 人はメニューから「切り替え」たいが、AI は「必ず ON にしてから撮る」
//     という決め方をしたい。反転しか無いと、AI は現在値を読んでから分岐する必要があり、
//     読み取り手段が無い項目では 2 回撮って比べるしかなくなる。
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

// ProjectSettings 側が権威のフラグ (Collider / NavMesh / 影 など)。
EditorOperator MakeRenderToggle(const char* id, const char* label, const char* desc,
                                bool renderer::RenderSettings::*field)
{
    EditorOperator op = MakeToggleBase(id, label, desc);
    op.exec = [field](OpContext& c, const OpArgs& args) -> OpResult {
        return ApplyToggle(c.ctx.projectSettings.render.*field, args);
    };
    // メニューのチェックと op.list の checked が同じ式から出る。
    op.checked = [field](const OpContext& c, const OpArgs&) {
        return c.ctx.projectSettings.render.*field;
    };
    return op;
}

// EditorContext 側が権威のフラグ。
// WHY 書き込み先を分けるか: Grid / Skeleton / LightRange / VFXGizmos は
//     EditorContext が正本で、毎フレーム sceneRenderSettings へ複写される
//     (EditorApp::RenderSceneView)。RenderSettings 側へ書いても次のフレームで
//     上書きされ、「設定したのに何も変わらない」という形でしか現れない。
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

} // namespace

void RegisterRenderOperators(OperatorRegistry& registry)
{
    // ── EditorContext が正本のもの ──────────────────────────────────────────
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
        "パーティクル力場の影響半径・向きと、エミッターの発生形状を描く。"
        "どちらも「見えない体積」なので、値の違いを粒子の挙動から逆算するしかなかった。",
        &EditorContext::showVFXGizmos));

    // WHY 追加したか: Debug メニューにありながら operator が無く、Grid や Skeleton と
    //     同じ列に並んでいるのに AI からだけ触れない項目だった。Stats は FPS と
    //     draw call を Game View へ焼き込むので、viewport_capture の絵に
    //     性能値を一緒に写せる (profiler_get_snapshot と時刻を合わせる必要がない)。
    registry.Register(MakeContextToggle(
        "render.show_stats", "Show Stats",
        "Game Viewport へ FPS / draw call のオーバーレイを出す。",
        &EditorContext::showStats));

    registry.Register(MakeContextToggle(
        "render.scene_view_occlusion_culling", "Scene View Occlusion Culling",
        "Scene View で CPU オクルージョンカリングを効かせる。既定は OFF。"
        "遮蔽者はメッシュ実体ではなくバウンディング球の近似なので、"
        "有効にすると見えているものが消えることがある。"
        "「消えた原因がカリングか」を切り分けるときに入れ切りする。"
        "Scene View はデバッグカメラで描くため CameraComponent の設定は効かず、"
        "編集ビューでの有効・無効はここでしか切り替えられない。",
        &EditorContext::sceneViewOcclusionCulling));

    // WHY Render カテゴリに置かないか: 描画ではなくスクリプト DLL の監視。
    //     AI が Play 前に自動リロードを止めたい場面 (途中でシーンが再構築されると
    //     掴んでいた NodeId が無効になる) があるので、切り替え手段を出す。
    {
        EditorOperator op = MakeContextToggle(
            "debug.hot_reload", "Hot Reload",
            "スクリプト DLL の自動ホットリロード監視。OFF にすると、"
            "編集中に勝手な再ロードでシーンが作り直されるのを防げる。",
            &EditorContext::hotReloadEnabled);
        op.category = "Tools";
        registry.Register(std::move(op));
    }

    // ── ProjectSettings が正本のもの ────────────────────────────────────────
    // NOTE: showConstraints は operator にしない。毎フレーム showColliders から
    //       導出される (RenderSceneView) ため、設定しても次のフレームで戻る。
    registry.Register(MakeRenderToggle(
        "render.show_colliders", "Show Colliders",
        "Collider の形状をワイヤーで描く。"
        "「当たらない」の原因が形状かレイヤーかを切り分ける最初の一手。"
        "物理コンストレイントの表示もこれに追従する。",
        &renderer::RenderSettings::showColliders));

    registry.Register(MakeRenderToggle(
        "render.show_terrain_collision", "Show Terrain Collision",
        "地形のコリジョン形状を描く。", &renderer::RenderSettings::showTerrainCollision));

    registry.Register(MakeRenderToggle(
        "render.show_navmesh", "Show NavMesh",
        "NavMesh の歩行可能面を描く。navmesh_find_path が found=false を返したとき、"
        "穴がどこにあるのかは絵でしか判らない。"
        "Play 中の Scene View では強制的に非表示になる (実行中の描画を邪魔しないため)。",
        &renderer::RenderSettings::showNavMesh));

    registry.Register(MakeRenderToggle(
        "render.show_nav_sensors", "Show AI Sensors",
        "NavMeshSensor の視界・聴覚範囲を描く。"
        "「敵が気づかない」の原因がセンサー範囲か BT の条件かを切り分ける。",
        &renderer::RenderSettings::showNavSensors));

    registry.Register(MakeRenderToggle(
        "render.show_decal_bounds", "Show Decal Bounds",
        "デカールの投影ボックスを描く。", &renderer::RenderSettings::showDecalBounds));

    registry.Register(MakeRenderToggle(
        "render.shadow_enabled", "Shadows",
        "シャドウマップの有効・無効。影が原因で暗いのかを切り分けるのに使う。",
        &renderer::RenderSettings::shadowEnabled));

    registry.Register(MakeRenderToggle(
        "render.particle_overdraw_view", "Particle Overdraw View",
        "パーティクルの重なり枚数をヒートマップで上書きする。"
        "パーティクルの実コストは粒子数ではなく塗った画素数で決まるが、"
        "重なりは通常の絵からは読めない。",
        &renderer::RenderSettings::particleOverdrawView));

    // ── ビューモード ────────────────────────────────────────────────────────
    // WHY 個別の operator に割らないか: 排他選択なので、4 つの Action を並べるより
    //     1 つの引数で受けるほうが「今どれか」を取り違えない。
    {
        EditorOperator op;
        op.id       = "render.set_view_mode";
        op.label    = "Set View Mode";
        op.category = "Render";
        op.desc     = "描画モードを切り替える (lit / unlit / wireframe_lit / wireframe_unlit)。"
                      "unlit はライティングを外してアルベドだけを見るので、"
                      "「暗い」の原因がマテリアルか光かを一発で分けられる。";
        op.kind     = OpKind::Action;

        OpParam modeParam;
        modeParam.name = "mode";
        modeParam.type = OpParamType::String;
        modeParam.desc = "描画モード";
        // 取りうる値は宣言する。以前は exec の中で 4 つの文字列と比較し、
        // 独自のエラー文を返していた — 候補が op.list に出ないので、AI は
        // desc の文章から綴りを起こすしかなかった。
        modeParam.enumValues = { "lit", "unlit", "wireframe_lit", "wireframe_unlit" };
        op.params = { modeParam };

        // 文字列 → enum の対応表。exec と checked が同じ表を読むので、
        // 「設定はできるのに現在値の判定だけ綴りが違う」が起きない。
        const auto toViewMode = [](const std::string& mode) {
            if (mode == "unlit")           return renderer::ViewMode::Unlit;
            if (mode == "wireframe_lit")   return renderer::ViewMode::WireframeLit;
            if (mode == "wireframe_unlit") return renderer::ViewMode::WireframeUnlit;
            return renderer::ViewMode::Lit;
        };

        op.exec = [toViewMode](OpContext& c, const OpArgs& args) -> OpResult {
            // 未知の綴りは ValidateArgs (enumValues) が入口で弾く。
            c.ctx.projectSettings.render.viewMode = toViewMode(args.GetString("mode"));
            return OpResult::Ok();
        };
        op.checked = [toViewMode](const OpContext& c, const OpArgs& args) {
            if (!args.Has("mode")) return false;
            return c.ctx.projectSettings.render.viewMode == toViewMode(args.GetString("mode"));
        };
        registry.Register(std::move(op));
    }

    // NavMesh オーバーレイの描き方。show_navmesh が「出す/出さない」だけを持ち、
    // 「何を出すか」がどこにも無かったため、穴の位置も areaType の塗り分けも
    // 同じ 1 枚の青い面からは読み取れなかった。
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
            // 描き方だけ変えても表示が消えていれば何も起きないので、同時に点ける。
            c.ctx.projectSettings.render.showNavMesh = true;
            return OpResult::Ok();
        };
        op.checked = [toDrawMode](const OpContext& c, const OpArgs& args) {
            if (!args.Has("mode")) return false;
            return c.ctx.projectSettings.render.navMeshDrawMode == toDrawMode(args.GetString("mode"));
        };
        registry.Register(std::move(op));
    }

    // WHY Render グループに置くか: NavMesh のベイクは描画ではないが、登録先を分けるには
    //     専用グループを 1 つ増やして EditorApp の登録列にも 1 行足す必要があり、
    //     操作 1 つのために二重管理を作ることになる。debug.hot_reload と同じ扱いで
    //     category だけ Tools に寄せる。
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

} // namespace fbzz::editor
