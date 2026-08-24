// FBZZ Engine
// DocumentOperators.cpp | fbzz::editor
// アセットを「開く」操作 (エディター面の切り替え) と Prefab 編集モードの出入り
//
// WHY: AI は .animcontroller の中身も .vfx のグラフも .behaviortree の木も
//      編集できるのに、**どれ一つ「開く」ことができなかった**。開くのは
//      AssetBrowser のダブルクリック (HandleEntryDoubleClick) だけが持つ経路で、
//      拡張子ごとの振り分けもそこに閉じていた。実害は 2 つある:
//        1. 人と一緒に作業できない。AI が直した Animator を人へ見せるのに
//           「Assets を辿ってダブルクリックしてください」と言うしかない。
//        2. パネルが開いていることを前提にした操作 (bt.auto_layout は
//           BehaviorTreePanel がワンショット要求を消費して初めて動く) が、
//           AI からは「呼んだのに何も起きない」になる。開く手段が無いので
//           前提を自分で満たせない。
//
//      Prefab 編集モードも同じ形で、入る手段が AssetBrowser の Alt+ダブルクリックと
//      右クリックメニューにしかなかった。prefab.close だけを足しても、
//      入れないのだから対称にならない。
//
// NOTE: .prefab のシーンへの配置 (インスタンス化) はここでは扱わない。
//       AI には prefab_instantiate があり、Undo の積み方まで含めて別実装になる。
//       同じ操作を 2 つ用意すると、どちらを直したかで挙動が分かれる。
//
// 設計: Docs/design/editor-operator-model.md §7
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
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

// 絶対パス / Assets 起点パスのどちらで来ても実ファイルを指すように解決する。
// WHY 両方受けるか: AssetBrowser とコマンドパレットは絶対パスを持ち、
//     AI と保存されたアセット参照は Assets 起点パスを持つ。片方しか受けないと、
//     同じ操作なのに呼び出し元によって「ファイルが無い」と言われる。
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
    // ── アセットを開く ──────────────────────────────────────────────────────
    {
        EditorOperator op;
        op.id       = "asset.open";
        op.label    = "Open Asset";
        op.category = "File";
        op.desc     = "アセットを対応するエディター面で開く。"
                      ".scene はシーンを切り替え、.animcontroller は Animation Graph、"
                      ".behaviortree は Behavior Tree、.vfx は VFX Editor へ渡す。"
                      "それ以外は Inspector の表示対象にする。";
        op.caution  = ".scene を開くと現在のシーンを閉じる (未保存の変更は確認モーダルになる)。"
                      "AI からモーダル無しで切り替えたい場合は scene_open を使う。";
        op.kind     = OpKind::Action;
        op.params   = { PathParam("絶対パスまたは Assets 起点のアセットパス") };

        op.poll = [](const OpContext& context, const OpArgs& args) {
            if (!args.Has("path")) return true;   // 引数なしの評価 (パレット) では可否を伏せない
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
                // 未保存確認は requestOpenScene (人の導線) が持つ。
                if (!ctx.requestOpenScene)
                    return OpResult::Err("NO_HANDLER", "シーンを開く経路が未結線です");
                ctx.requestOpenScene(path);
                result.message = "シーンを開きます";
            } else if (ext == ".animcontroller") {
                // 開くドキュメントを先に渡してから窓を出す (AssetBrowser と同じ順)。
                // 逆にすると、パネルは前回のドキュメントを 1 フレーム描いてしまう。
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
            } else if (ext == ".vfx") {
                // VFX Editor は独立プロセス。選択パスを渡してから起動要求を立てる。
                ctx.selectedAssetPath    = path;
                ctx.requestOpenVFXEditor = true;
                result.message = "VFX Editor で開きます";
            } else if (ext == ".prefab") {
                return OpResult::Err("USE_PREFAB_EDIT",
                                     ".prefab は prefab.edit (中身を編集) または "
                                     "prefab_instantiate (シーンへ配置) を使います");
            } else {
                // 既定は Inspector の表示対象にする (.mat / .physmat / テクスチャなど)。
                ctx.selectedAssetPath = path;
                result.message = "Inspector に表示します";
            }
            return result;
        };
        registry.Register(std::move(op));
    }

    // ── Prefab 編集モードへ入る ─────────────────────────────────────────────
    // WHY 専用の操作にするか: asset.open の一分岐にすると、.prefab を「開く」が
    //     配置なのか中身の編集なのかを拡張子から決めることになる。
    //     AssetBrowser がこれを Alt 修飾で分けているのと同じ理由で、別の操作にする。
    {
        EditorOperator op;
        op.id       = "prefab.edit";
        op.label    = "Edit Prefab";
        op.category = "File";
        op.desc     = "編集中のシーンを一時退避して .prefab の中身だけを開く。"
                      "シーンに 1 個も置いていない Prefab もこれで直せる。";
        op.caution  = "現在のシーンは一時退避される。抜けるには prefab.close (保存は scene.save)。";
        op.kind     = OpKind::Action;
        op.params   = { PathParam(".prefab の絶対パスまたは Assets 起点パス") };

        op.poll = [](const OpContext& context, const OpArgs& args) {
            // Prefab 編集中の入れ子は扱えない (退避先が 1 つしかない)。
            if (context.ctx.InPrefabEditMode()) return false;
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
            if (util::StringUtils::ToLower(util::FileSystem::GetExtension(path)) != ".prefab")
                return OpResult::Err("BAD_ARG", "prefab.edit が開けるのは .prefab だけです: " + path);

            // 要求だけを立てる。実際の差し替えは ProcessPrefabEditRequests が
            // フレーム先頭で行う (パネル描画の途中でシーンを入れ替えると、
            // 以降のパネルが破棄済みの GameObject を掴む)。
            context.ctx.requestOpenPrefabEdit = NormalizeAssetPath(path);
            return OpResult::Ok();
        };
        registry.Register(std::move(op));
    }

    // ── AI Command Bus ──────────────────────────────────────────────────────
    // WHY operator にするか: 待受の開始・停止は AI Settings パネルにしか無く、
    //     メニューは状態表示だけ (押せない項目) だった。パレットからも
    //     ホットキーからも届かないので、パネルを探し当てるまで再接続できない。
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
