/// @file    DocumentOperators.cpp
/// @brief   アセットを「開く」操作 (エディター面の切り替え) と Prefab 編集モードの出入り。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// AI は .animcontroller / .prefab / .behaviortree の中身を編集できても、開く手段は
/// AssetBrowser のダブルクリックにしか無く、パネルが開いている前提の操作 (bt.auto_layout 等) が
/// 無反応になっていた。「開く = 編集面を出す」「置く = prefab_instantiate」で全拡張子を通す。
/// @see Docs/design/editor-operator-model.md §7
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/FluidAssetWriters.hpp>
#include <Editor/Util/FluidBakeService.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <string>
#include <utility>

namespace fbzz::editor {

namespace {

OpParam PathParam(const char* description)
{
    OpParam param;
    param.name     = "path";
    param.type     = OpParamType::String;
    param.desc     = description;
    param.required = true;
    return param;
}

/// @brief 絶対パス / Assets 起点パスのどちらで来ても実ファイルを指すように解決する。
/// @note AssetBrowser とコマンドパレットは絶対パスを、AI と保存済みアセット参照は Assets 起点パスを
///       持つ。片方しか受けないと、呼び出し元によって「ファイルが無い」と言われてしまう。
std::string ResolveExistingPath(const EditorContext& context, const std::string& path)
{
    if (util::FileSystem::Exists(path)) return path;

    const std::string assetPath = NormalizeAssetPath(path);
    const std::string diskPath  = ToProjectAssetDiskPath(context.projectRoot, assetPath);
    if (util::FileSystem::Exists(diskPath)) return diskPath;

    return {};
}

} // namespace

void RegisterDocumentOperators(OperatorRegistry& registry)
{
    {
        EditorOperator op;
        op.id = "fluid.effect_template.create";
        op.label = "Create Fluid Effect Template";
        op.category = "VFX";
        op.desc = "素材レシピを新規フォルダへ作成し、順にベイクして時間差のある複数層の VFX を作る。";
        op.kind = OpKind::Action;
        OpParam preset;
        preset.name = "preset";
        preset.desc = "演出テンプレート";
        preset.enumValues = { "landing", "charge_release", "magic_eruption" };
        op.params = { preset, PathParam("Assets 配下の未作成フォルダ。例: Assets/VFX/MyLanding") };
        op.poll = [](const OpContext& context, const OpArgs&) { return context.ctx.fluidBake != nullptr; };
        op.exec = [](OpContext& context, const OpArgs& args) -> OpResult {
            if (context.ctx.fluidBake == nullptr) return OpResult::Err("NO_SERVICE", "FluidBakeService がありません");
            const std::string name = args.GetString("preset");
            const auto preset = name == "landing" ? FluidEffectTemplate::LANDING
                : name == "charge_release" ? FluidEffectTemplate::CHARGE_RELEASE
                : name == "magic_eruption" ? FluidEffectTemplate::MAGIC_ERUPTION : FluidEffectTemplate::COUNT;
            FluidJobError error;
            const auto id = context.ctx.fluidBake->EnqueueEffectTemplate(context.ctx, preset, args.GetString("path"), error);
            if (id == 0) return OpResult::Err(error.code, error.message);
            context.ctx.requestAssetBrowserRefresh = true;
            OpResult result;
            result.message = "素材と演出テンプレートを作成しています";
            result.data.Set("jobId", static_cast<int>(id));
            return result;
        };
        registry.Register(std::move(op));
    }

    /// @name アセットを開く
    {
        EditorOperator op;
        op.id       = "asset.open";
        op.label    = "Open Asset";
        op.category = "File";
        op.desc     = "アセットを対応するエディター面で開く。"
                      ".scene はシーンを切り替え、.animcontroller は Animation Graph、"
                      ".behaviortree は Behavior Tree、.synth は SFX Editor、"
                      ".sequence は Sequence、.fluid は Fluid Editor、"
                      ".prefab / .vfx は Prefab 編集モードへ渡す。"
                      "それ以外は Inspector の表示対象にする。"
                      "シーンへ «置く» のはこの操作ではない (prefab_instantiate を使う)。";
        op.caution  = ".scene を開くと現在のシーンを閉じる (未保存の変更は確認モーダルになる)。"
                      "AI からモーダル無しで切り替えたい場合は scene_open を使う。"
                      ".prefab / .vfx は現在のシーンを一時退避する (抜けるには prefab.close)。";
        op.kind     = OpKind::Action;
        op.params   = { PathParam("絶対パスまたは Assets 起点のアセットパス") };

        op.poll = [](const OpContext& context, const OpArgs& args) {
            /// @note 引数なしの評価 (パレット) では可否を伏せない
            if (!args.Has("path")) return true;
            return !ResolveExistingPath(context.ctx, args.GetString("path")).empty();
        };

        op.exec = [](OpContext& context, const OpArgs& args) -> OpResult {
            EditorContext&    ctx  = context.ctx;
            const std::string path = ResolveExistingPath(ctx, args.GetString("path"));
            if (path.empty()) {
                return OpResult::Err("ASSET_NOT_FOUND",
                                     "アセットが見つかりません: " + args.GetString("path"));
            }

            const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));

            OpResult result;
            if (ext == ".scene") {
                /// @note 未保存確認は requestOpenScene (人の導線) が持つ。
                if (!ctx.requestOpenScene)
                    return OpResult::Err("NO_HANDLER", "シーンを開く経路が未結線です");
                ctx.requestOpenScene(path);
                result.message = "シーンを開きます";
            } else if (ext == ".animcontroller") {
                /// @note 開くドキュメントを先に渡してから窓を出す (AssetBrowser と同じ順)。
                ///       逆にすると、パネルは前回のドキュメントを 1 フレーム描いてしまう。
                if (ctx.openAnimationGraph) ctx.openAnimationGraph(path);
                ctx.requestOpenAnimationGraph = true;
                result.message = "Animation Graph で開きます";
            } else if (ext == ".behaviortree") {
                if (ctx.openBehaviorTree) ctx.openBehaviorTree(path);
                ctx.requestOpenBehaviorTree = true;
                result.message = "Behavior Tree で開きます";
            } else if (ext == ".synth") {
                if (ctx.openSfxEditor) ctx.openSfxEditor(path);
                ctx.requestOpenSfxEditor = true;
                result.message = "SFX Editor で開きます";
            } else if (ext == ".sequence") {
                if (ctx.openSequence) ctx.openSequence(path);
                ctx.requestOpenSequence = true;
                result.message = "Sequence で開きます";
            } else if (ext == ".fluid") {
                /// @note 読んで消すのは Fluid Editor パネル (未保存の確認もパネルが持つ)。閉じていれば EditorApp が開く。
                ctx.requestOpenFluidEditor = path;
                result.message = "Fluid Editor で開きます";
            } else if (ext == ".vfx" || ext == ".prefab") {
                /// @note どちらも同じプレハブ形式。中身は Prefab 編集モードで開く (Hierarchy / Inspector /
                ///       ギズモがそのまま使える)。
                /// @note .prefab だけ «シーンへ置く» にすると拡張子ごとに操作の意味が変わるため «配置»
                ///       にはしない。全拡張子を通して «編集面で開く» に統一し、配置は prefab_instantiate が持つ。
                /// @note AssetBrowser のダブルクリックは別既定 (シーン編集中の .prefab = 配置 /
                ///       Alt+ダブルクリック = 編集) を持つ。使われ方の違いによる意図した非対称。
                /// @note 編集中でも受け付ける。EnterPrefabEditMode は編集中なら保存して閉じてから開き
                ///       直すため (EditorApp_Prefab.cpp:64)、弾くと «プレハブ間を移る» 手段が無くなる。
                ctx.requestOpenPrefabEdit = NormalizeAssetPath(path);
                result.message = ctx.InPrefabEditMode()
                    ? "今のプレハブを保存して切り替えます"
                    : "Prefab 編集モードで開きます";
            } else {
                /// @note 既定は Inspector の表示対象にする (.mat / .physmat / テクスチャなど)。
                SelectAsset(ctx, path);
                result.message = "Inspector に表示します";
            }
            return result;
        };
        registry.Register(std::move(op));
    }

    /// @name Prefab 編集モードへ入る
    /// @note asset.open は「拡張子から開き方を決める」汎用入口で、.scene かもしれないパスをそのまま渡す。
    ///       こちらは «プレハブを編集する» という意図そのもので、他拡張子は弾く。asset.open は
    ///       .prefab / .vfx をここへ委譲する。
    {
        EditorOperator op;
        op.id       = "prefab.edit";
        op.label    = "Edit Prefab";
        op.category = "File";
        op.desc     = "編集中のシーンを一時退避して .prefab の中身だけを開く。"
                      "シーンに 1 個も置いていない Prefab もこれで直せる。";
        op.caution  = "現在のシーンは一時退避される。抜けるには prefab.close (保存は scene.save)。"
                      "既に編集中のときは、そのプレハブを保存して閉じてから切り替える。";
        op.kind     = OpKind::Action;
        op.params   = { PathParam(".prefab の絶対パスまたは Assets 起点パス") };

        op.poll = [](const OpContext& context, const OpArgs& args) {
            /// @note 編集中でも呼べる。EnterPrefabEditMode が保存して閉じてから開き直す。
            if (context.ctx.activeScene == nullptr) return false;
            if (!args.Has("path")) return true;
            return !ResolveExistingPath(context.ctx, args.GetString("path")).empty();
        };

        op.exec = [](OpContext& context, const OpArgs& args) -> OpResult {
            const std::string path = ResolveExistingPath(context.ctx, args.GetString("path"));
            if (path.empty()) {
                return OpResult::Err("ASSET_NOT_FOUND",
                                     "アセットが見つかりません: " + args.GetString("path"));
            }
            /// @note .vfx もプレハブ形式なので同じ経路で開ける (旧 DAG 形式は除く)。
            const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
            if (ext != ".prefab" && ext != ".vfx")
                return OpResult::Err("BAD_ARG",
                                     "prefab.edit が開けるのは .prefab / .vfx だけです: " + path);

            /// @note 要求だけを立てる。実際の差し替えは ProcessPrefabEditRequests が
            ///       フレーム先頭で行う (パネル描画の途中でシーンを入れ替えると、
            ///       以降のパネルが破棄済みの GameObject を掴む)。
            context.ctx.requestOpenPrefabEdit = NormalizeAssetPath(path);
            return OpResult::Ok();
        };
        registry.Register(std::move(op));
    }

    /// @name AI Command Bus
    /// @note 待受の開始・停止は AI Settings パネルにしか無く、メニューは状態表示だけ (押せない項目)
    ///       だった。パレットからもホットキーからも届かず、パネルを探し当てるまで再接続できなかった。
    {
        EditorOperator op;
        op.id       = "ai.command_bus";
        op.label    = "AI Command Bus";
        op.category = "Tools";
        op.desc     = "AI (MCP) からの接続待受を開始 / 停止する。enabled を省略すると反転する。";
        op.caution  = "停止すると AI からの接続は切れる (この操作自体を AI から呼ぶと、"
                      "以降の要求は届かない)。";
        op.kind     = OpKind::Action;

        OpParam enabledParam;
        enabledParam.name     = "enabled";
        enabledParam.type     = OpParamType::Bool;
        enabledParam.desc     = "省略すると現在の待受状態を反転する";
        enabledParam.required = false;
        op.params = { enabledParam };

        op.poll = [](const OpContext& context, const OpArgs&) {
            return context.ctx.startAiCommandBus && context.ctx.stopAiCommandBus;
        };
        op.checked = [](const OpContext& context, const OpArgs&) {
            return context.ctx.aiCommandBusRunning;
        };
        op.exec = [](OpContext& context, const OpArgs& args) -> OpResult {
            EditorContext& ctx  = context.ctx;
            const bool     next = args.Has("enabled") ? args.GetBool("enabled")
                                                      : !ctx.aiCommandBusRunning;
            if (next == ctx.aiCommandBusRunning) {
                OpResult result;
                result.noChange = true;
                return result;
            }

            OpResult result;
            if (next) {
                if (!ctx.startAiCommandBus())
                    return OpResult::Err("BUS_START_FAILED",
                                         "待受を開始できませんでした (ログを確認してください)");
                result.message = "待受を開始しました";
            } else {
                ctx.stopAiCommandBus();
                result.message = "待受を停止しました";
            }
            ctx.aiCommandBusEnabled = next;
            return result;
        };
        registry.Register(std::move(op));
    }
}

} // namespace fbzz::editor
