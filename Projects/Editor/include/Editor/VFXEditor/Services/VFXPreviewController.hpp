// FBZZ Engine
// VFXPreviewController.hpp | fbzz::editor
// 専用 Preview World に対する再生制御・環境設定・同時プレビュー複製
// WHY: 「何を見せているか」は Panel の描画都合ではなく、エフェクト制作の判断材料そのもの。
//      背景の明暗、同時発生数、スクラブ位置を独立したサービスとして持たせることで、
//      Graph モード / 単体 Emitter モードのどちらの View からも同じ制御を共有できる。
#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

// プレビューの見え方を決める環境設定。
// WHY: 背景が常に暗い固定色だと、暗所前提で作ったエフェクトが昼のマップで
//      「white にしか見えない」「輪郭が消える」といった形で破綻していても
//      制作中は気づけない。明所・暗所・室内を即座に切り替えて確認できるようにする。
// NOTE: 適用先は操作用 Preview World だけ。AI capture 用 World には一切影響させない
//       (キャプチャの見た目が担当者の設定で変わると、AI の視覚評価が再現しなくなる)。
struct VFXPreviewEnvironment {
    std::string name;
    math::Vector4 backgroundColor = { 0.018f, 0.021f, 0.028f, 1.0f };
    math::Vector3 lightDirection = { -0.4f, -1.0f, -0.35f };
    math::Vector3 lightColor = { 1.0f, 1.0f, 1.0f };
    float lightIntensity = 1.0f;
    math::Vector3 ambientColor = { 0.05f, 0.06f, 0.08f };
    bool showFloorGrid = false;
    // 寸法比較用の代理メッシュ ("" / "primitive:cube" / "primitive:sphere" / "primitive:plane")。
    // エフェクトの大きさは、比較対象が無いと画面上では判断できない。
    std::string proxyMeshPath;
    float proxyScale = 1.0f;
};

class VFXPreviewController {
public:
    // ── レンダーターゲット (所有者は VFXEditorApp / EditorApp。ここは表示サイズだけを決める) ──
    renderer::ResourceHandle<renderer::RenderTargetTag> renderTarget;
    float width = 640.0f;
    float height = 360.0f;
    // 直近フレームで実際に Preview を描いたカメラ (aspect 適用済み)。
    // WHY: プレビュー上へワールド座標のオーバーレイ (Transformギズモなど) を重ねるには、
    //      描画に使ったものと完全に同じ行列で投影しないと枠が対象からずれる。
    //      カメラの所有者は VFXEditorApp / EditorApp と 2 つあるため、
    //      ポインタで参照させず「描いたときの値」をここへ写して View が読む。
    // cameraValid が false の間は、まだ 1 度も描いていない (投影してはいけない)。
    renderer::Camera camera;
    bool cameraValid = false;

    // このフレームにプレビューを描いてほしいか / プレビュー上にマウスがあるか。
    bool renderRequested = false;
    bool hovered = false;

    // ── トランスポート ──
    bool paused = false;  // editorTimeScale = 0 を注入して止める
    float speed = 1.0f;   // 再生速度倍率
    bool restartRequested = false;

    // ── 表示補助 (操作用 Preview 限定。AI capture へは持ち込まない) ──
    VFXPreviewEnvironment environment;
    // 環境設定が変わった版数。App 側が「作り直すべきか」を判定するために見る。
    std::uint64_t environmentRevision = 1;
    int environmentPreset = 0;
    bool showFloorGrid = false;
    // 力場の影響半径・向きとエミッター発生形状のワイヤー表示。
    bool showGizmos = false;
    // パーティクルの重なり枚数を色で見せるヒートマップ。
    bool overdrawView = false;
    // Overdraw診断をParticleだけに限定するか、Preview Actor/AnimatedMeshも含めるか。
    bool includeModelsInOverdraw = false;
    // GPU同期を伴う数値readbackは、Debugメニュー/AIが要求した1フレームだけ実行する。
    bool overdrawReadbackRequested = false;
    // ループ継ぎ目の確認 (t=0 と t=duration を並べて描く)。
    bool showLoopSeam = false;

    // ── 同時プレビュー ──
    // WHY: 1 個だけ見て調整したエフェクトは、20 個同時に出た瞬間に画面が白飛びしたり
    //      GPU 時間が跳ねたりする。制作中に密度を上げて確認できないと気づけない。
    int instanceCount = 1;
    float instanceSpread = 3.0f;
    // ルート 1 個を除いた追加分の複製インスタンス。
    std::vector<scene::EntityID> copyEntities;
    float copySpread = -1.0f; // 最後に構築したときの配置半径 (負値 = 未構築)

    // ── Preview World 内の対象 ──
    scene::EntityID selectedEntity = scene::EntityID::INVALID;
    scene::EntityID graphEntity = scene::EntityID::INVALID;
    scene::EntityID actorEntity = scene::EntityID::INVALID;
    std::string actorModelPath;
    std::string actorControllerPath;
    std::string actorClipPath;
    std::string actorMaterialPath;
    std::string actorState;
    std::string selectedBone;
    std::vector<std::string> actorBones;
    float actorTime = 0.0f;
    float actorSpeed = 1.0f;
    bool actorPlaying = true;
    bool actorLoop = true;
    bool syncActorToVFX = true;
    bool attachGraphToBone = false;
    bool restorePending = false;

    // 選択エミッターから SubEmitter 名前参照と子 GameObject を辿った「1エフェクト」の集合。
    // トランスポート操作 (Pause / Restart / スクラブ) はこの集合全体へ適用する。
    [[nodiscard]] std::vector<scene::EntityID> BuildEffectGroup(EditorContext& ctx) const;

    // グループ全エミッターへスクラブを要求する (決定論的な再シミュレーション)。
    void RequestScrub(EditorContext& ctx, const std::vector<scene::EntityID>& group,
                      float targetTime);
    // FBXはPreview Actorを生成し、.anim/Controller/Materialは既存Actorへ差し替える。
    [[nodiscard]] bool LoadPreviewAsset(EditorContext& ctx, const std::string& path,
                                        std::string* outError = nullptr);
    void RefreshActorBones(EditorContext& ctx);
    void ApplyActorAttachment(EditorContext& ctx);

    // instanceCount / instanceSpread に合わせて複製インスタンスを作り直す。
    void SyncInstanceCopies(EditorContext& ctx, const std::string& assetPath);

    // 代表的な 4 種の環境 (Dark / Daylight / Interior / Neutral Gray) を適用する。
    void ApplyEnvironmentPreset(int presetIndex);
};

} // namespace fbzz::editor
