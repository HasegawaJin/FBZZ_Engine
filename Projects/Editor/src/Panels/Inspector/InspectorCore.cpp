// FBZZ Engine
// InspectorCore.cpp | fbzz::editor
// Transform / Script の Inspector 描画
#include "InspectorCore.hpp"
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/ScriptSnapshot.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cfloat>
#include <charconv>
#include <imgui_internal.h>

namespace fbzz::editor {

namespace {

// スクリプトカードの帯色。Engine コンポーネントのどのカテゴリ色とも被らない色を当て、
// 「ここから下はユーザーコード」であることを一目で分かるようにする。
constexpr ImU32 kScriptAccent = IM_COL32(120, 190, 255, 255);

// スクリプトカードを Component の表示順リストへ載せるためのキー。
//
// WHY index ではなく型名か: キーは .meta へ保存され、次にシーンを開いたときに
//   同じカードを指し続けなければならない。index はスクリプトを 1 つ足しただけで
//   全部ずれる。型名なら足しても消しても他のカードのキーが動かない。
// WHY "Script:" を前置するか: エンジン Component のキーは表示名そのもの ("Mesh Renderer")
//   なので、ユーザーが同名のスクリプトを書いたときに衝突する。名前空間を分ける。
// WHY 同型が複数あるとき "#n" を足すか: 1 つの GameObject に同じスクリプトを 2 つ
//   付けられる (弾を 2 門ぶん等)。キーが重複すると 2 枚が同じ席を奪い合う。
std::string ScriptOrderKey(const scene::ScriptComponent& sc, int index)
{
    const auto typeNameOf = [](const scene::ScriptEntry& entry) -> std::string {
        if (entry.script) return entry.script->GetTypeName();
        if (entry.serialized) return entry.serialized->type;
        return "Unknown";
    };
    if (index < 0 || index >= static_cast<int>(sc.scripts.size())) return {};

    const std::string type = typeNameOf(sc.scripts[static_cast<std::size_t>(index)]);
    int sameTypeBefore = 0;
    for (int i = 0; i < index; ++i)
        if (typeNameOf(sc.scripts[static_cast<std::size_t>(i)]) == type) ++sameTypeBefore;

    std::string key = "Script:" + type;
    if (sameTypeBefore > 0) key += "#" + std::to_string(sameTypeBefore);
    return key;
}

// 1 フレームのあいだ全スクリプトカードで共有する状態。
//
// WHY ファイルスコープの static か: カードは Component と混ざって 1 枚ずつ別々の
//   コールバックから描かれるようになったため、以前のように 1 つの関数のローカル変数で
//   持てない。Inspector は 1 フレームに 1 GameObject しか描かないので単一で足りる。
struct ScriptInspectorFrame {
    // 編集開始時の値 (Undo の before)。index → スナップショット。
    std::vector<std::string> beforeSnapshots;
    ImGuiID activeBefore     = 0;
    int     editingIndex     = -1;   // この frame に編集が始まったカード
    int     removeIndex      = -1;   // ⋯ → Remove Component
    bool    canTrackUndo     = false;
    bool    active           = false; // Begin〜End の内側か
};
ScriptInspectorFrame s_scriptFrame;

// スクリプト 1 個ぶんの編集を 1 コマンドとして記録する追跡状態。
//
// WHY: 以前はシーン全体を TOML 化して before/after にしていた。スクリプトの実体は
//      DLL の向こうにあり、型を知らないエディタからは値を取り出せないというのが理由。
//      だが Script は Reflect() を実装しているので、IReflector を 1 つ用意すれば
//      型を知らないまま「そのスクリプトだけ」を読み書きできる。
//      これで Undo が全シーン再構築ではなく値の復元になり、EntityID も選択も維持される。
struct ScriptUndoTracker {
    ImGuiID     activeId = 0;
    std::string before;        // 編集開始時のスナップショット
    std::string instanceId;    // 対象 GameObject
    std::string typeName;      // 対象スクリプトの型 (index だけだと取り違える)
    int         scriptIndex = -1;
    bool        active = false;
};
ScriptUndoTracker s_scriptUndo;

// 表示順 (Component 順リスト) に合わせて sc.scripts を並べ替え、リスト側のキーも
// 並べ替え後の実体に合わせて書き直す。
//
// WHY 表示順を正にするか: スクリプトカードがエンジン Component と同じ 1 枚として
//   並ぶようになった以上、「上にあるカードほど先に動く」以外の対応付けは説明できない。
//   実行順 (ScriptSystem が回す順) を表示順から導出し、2 つの並びが食い違う状態を作らない。
//
// WHY キーを書き直すか (同型スクリプト対策): キーの "#n" は「配列の中で同じ型が何番目か」
//   なので、同じ型を 2 つ付けた GameObject で 2 枚を入れ替えると、実体と一緒にキーも
//   入れ替わる。書き直さないと「入れ替える → キーも入れ替わる → 次フレームまた入れ替える」
//   と毎フレーム反転し続ける。並べ替えた直後に、リスト上のスクリプト席へ配列順の
//   キーを埋め直して自己整合にする。
// @return 実際に並びが変わったら true
bool SyncScriptOrderToDisplay(scene::ScriptComponent& sc,
                              EditorContext& ctx,
                              const std::string& instanceId)
{
    const int count = static_cast<int>(sc.scripts.size());
    if (count < 2) return false;

    const std::vector<std::string> displayOrder =
        ctx.editorSceneState.GetComponentOrder(instanceId);

    // 表示順に現れるスクリプトキーを拾い、その順に並べたい index 列を作る。
    // 併せて「リストのどの席がスクリプトだったか」も覚えておく (後でキーを埋め直す)。
    std::vector<int>         desired;
    std::vector<std::size_t> scriptSlots;
    desired.reserve(static_cast<std::size_t>(count));
    for (std::size_t slot = 0; slot < displayOrder.size(); ++slot) {
        for (int i = 0; i < count; ++i) {
            if (ScriptOrderKey(sc, i) != displayOrder[slot]) continue;
            if (std::find(desired.begin(), desired.end(), i) == desired.end()) {
                desired.push_back(i);
                scriptSlots.push_back(slot);
            }
            break;
        }
    }
    // 表示順に載っていないカード (追加直後など) は現状の順で末尾へ回す。
    for (int i = 0; i < count; ++i)
        if (std::find(desired.begin(), desired.end(), i) == desired.end())
            desired.push_back(i);

    bool alreadySorted = true;
    for (int i = 0; i < count; ++i)
        if (desired[static_cast<std::size_t>(i)] != i) { alreadySorted = false; break; }
    if (alreadySorted) return false;

    std::vector<scene::ScriptEntry> reordered;
    reordered.reserve(static_cast<std::size_t>(count));
    for (const int index : desired)
        reordered.push_back(std::move(sc.scripts[static_cast<std::size_t>(index)]));
    sc.scripts = std::move(reordered);

    // 並べ替え後の配列順で振り直したキーを、元のスクリプト席へ順に埋め戻す。
    if (!scriptSlots.empty()) {
        std::vector<std::string> rewritten = displayOrder;
        for (std::size_t n = 0; n < scriptSlots.size(); ++n)
            rewritten[scriptSlots[n]] = ScriptOrderKey(sc, static_cast<int>(n));
        ctx.editorSceneState.SetComponentOrder(instanceId, std::move(rewritten));
    }
    return true;
}

// FBZZ_REQUIRE_COMPONENT の不足をスクリプトカードの先頭へ出す。
// 戻り値: Fix が押されて実際に追加が起きたら true (呼び出し側がシーンを dirty にする)。
//
// WHY 自動追加ではなく警告 + 明示的な Fix にするか:
//   スクリプトを付けた瞬間に黙ってコンポーネントが増えると、(1) シーンが自分の知らない
//   ところで書き換わり、(2) Undo の粒度が「スクリプト 1 件」からずれ、(3) そもそも
//   Animator は Controller 未設定なら足しても動かないので「揃っているのに動かない」
//   という一段深い迷子を作る。足りないことを名指しし、直すかどうかは人が決める。
bool DrawScriptRequirementBanner(scene::GameObject& go,
                                 const scene::Script& script,
                                 EditorContext& ctx)
{
    std::vector<scene::ScriptRequirementIssue> issues;
    // 任意コンポーネント (FBZZ_OPTIONAL_COMPONENT) も拾って情報として並べる。
    scene::CollectScriptRequirementIssues(go, script, issues, /*includeOptional=*/true);
    if (issues.empty()) return false;

    // 「足せば直る」必須のものだけを Fix の対象にする。
    // unknown (宣言側の綴り違い) と addable=false (内部型) はボタンを出しても直らない。
    std::vector<std::string> fixable;
    for (const auto& issue : issues)
        if (!issue.optional && !issue.unknown && issue.addable)
            fixable.push_back(issue.componentType);

    const bool hasError = std::any_of(issues.begin(), issues.end(),
        [](const scene::ScriptRequirementIssue& i) { return !i.optional; });
    const ImVec4 accent = EditorTheme::Color(hasError ? ThemeColor::Danger : ThemeColor::Warning);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::PushTextWrapPos(0.0f);
    for (const auto& issue : issues) {
        if (issue.unknown) {
            ImGui::Text("Unknown component type '%s' in FBZZ_REQUIRE_COMPONENT",
                        issue.componentType.c_str());
        } else if (issue.optional) {
            ImGui::Text("Optional: '%s' is not attached (this script degrades without it)",
                        issue.componentDisplay.c_str());
        } else if (!issue.addable) {
            ImGui::Text("Missing: '%s' (internal component — cannot be added by hand)",
                        issue.componentDisplay.c_str());
        } else {
            ImGui::Text("Missing required component: %s", issue.componentDisplay.c_str());
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    if (fixable.empty()) {
        ImGui::Spacing();
        return false;
    }

    bool added = false;
    if (ImGui::SmallButton("Fix")) {
        // fixable は「登録済み・追加可能・未アタッチ」だけを残した集合なので、
        // 押された時点で必ず 1 個以上増える。コマンドが null になるのは
        // Undo を記録できない文脈 (Play 中など) のときだけ。
        auto command = AddMissingComponentsWithUndo(go, ctx, fixable);
        if (command && ctx.undoStack) ctx.undoStack->Push(std::move(command));
        added = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add the missing components with their default setup");

    ImGui::Spacing();
    return added;
}

// UIImage の原寸 = 元画像の 1 ピクセルが画面の 1 ピクセルになる大きさ。
//
// WHY 素材の pixelsPerUnit と multiplier を両方掛けるか:
//     Sliced / Tiled は Border とタイルを「元画像のピクセル数 ÷ multiplier」で描く。
//     原寸も同じ換算にしておかないと、原寸に合わせた矩形なのに 9-slice の角だけ
//     大きさが合わない、という食い違いが起きる。
bool ResolveUIImageNativeSize(const scene::UIImage& image, EditorContext& ctx,
                              math::Vector2& outSize)
{
    // 今 Viewport に出ている絵に合わせる。ボタンの状態差し替え中はそちらが正。
    const std::string& source = image.EffectiveTexturePath();
    if (source.empty() || ctx.resources == nullptr) return false;

    std::string texturePath;
    std::string spriteId;
    (void)asset::ParseSpriteReference(source, texturePath, spriteId);
    const auto handle =
        ctx.resources->LoadTexture(asset::AssetManager::ResolveAssetPath(texturePath));
    const renderer::ITexture* texture = handle.IsValid() ? ctx.resources->Get(handle) : nullptr;
    if (texture == nullptr) return false;

    const asset::ResolvedSprite resolved = asset::ResolveSpriteReference(
        source,
        static_cast<float>(texture->GetWidth()),
        static_cast<float>(texture->GetHeight()));
    if (resolved.sizePixels.x <= 0.0f || resolved.sizePixels.y <= 0.0f) return false;

    const float pixelsPerUnit = resolved.pixelsPerUnit > 0.0f
        ? resolved.pixelsPerUnit : scene::kUIReferencePixelsPerUnit;
    const float scale =
        (scene::kUIReferencePixelsPerUnit / pixelsPerUnit) * image.UnitScale();
    outSize = { resolved.sizePixels.x * scale, resolved.sizePixels.y * scale };
    return true;
}

bool TransformEquals(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return lhs.position.x == rhs.position.x &&
           lhs.position.y == rhs.position.y &&
           lhs.position.z == rhs.position.z &&
           lhs.rotation.x == rhs.rotation.x &&
           lhs.rotation.y == rhs.rotation.y &&
           lhs.rotation.z == rhs.rotation.z &&
           lhs.rotation.w == rhs.rotation.w &&
           lhs.scale.x == rhs.scale.x &&
           lhs.scale.y == rhs.scale.y &&
           lhs.scale.z == rhs.scale.z;
}

// ImGui の連続ドラッグを1つの Transform コマンドへまとめる。
// WHY: 値が変化する各フレームを履歴へ積むと、1回のドラッグを戻すために
//      Ctrl+Z が何十回も必要になるため、Activated～Deactivated を1操作とする。
void TrackTransformEdit(scene::GameObject& go, EditorContext& ctx, const char* description)
{
    struct ActiveEdit {
        scene::EntityID id;
        scene::Transform before;
        bool active = false;
    };
    static ActiveEdit edit;

    if (ImGui::IsItemActivated()) {
        edit.id = go.GetID();
        edit.before = go.transform;
        edit.active = true;
    }

    if (!ImGui::IsItemDeactivatedAfterEdit() || !edit.active || edit.id != go.GetID())
        return;

    const scene::Transform before = edit.before;
    const scene::Transform after = go.transform;
    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto markDirty = ctx.markSceneDirty;

    if (ctx.undoStack && scene && !TransformEquals(before, after)) {
        auto apply = [scene, instanceId, markDirty](const scene::Transform& value) {
            if (auto* target = scene->FindByGuid(instanceId)) {
                target->transform = value;
                if (markDirty) markDirty();
            }
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            description,
            [apply, after]() { apply(after); },
            [apply, before]() { apply(before); }));
    }

    edit.active = false;
    if (!TransformEquals(before, after) && ctx.markSceneDirty) ctx.markSceneDirty();
}

// NOTE: Transform の値クリップボードは EditorContext::transformClipboard にある。
//       以前はこの翻訳単位の関数内 static だったため、AI (transform.copy /
//       transform.paste operator) から同じ器を触れなかった。別々に持つと
//       「人がコピーしたものを AI が貼れない」だけでなく、同じ操作名で中身が違う
//       という最も追いにくい食い違いになる。
//       コピー/貼り付け/リセットの実体は Editor/Op/InspectorOperators.cpp。

// Transform をメニュー操作で書き換えた際の Undo コマンドを積む (連続ドラッグ用の TrackTransformEdit とは別経路)。
//
// WHY: 以前はシーン全体を TOML 化して before/after にしていたが、戻すのが 1 つの
//      GameObject の Transform だけなのにシーン全体を Deserialize で再構築していた。
//      EntityID が振り直されるため選択・ロック・エディタ非表示が毎回消え、
//      大きなシーンでは Paste/Reset のたびに全文シリアライズ 2 回ぶんのヒッチが出ていた。
void PushTransformSnapshotUndo(scene::GameObject& go,
                               EditorContext& ctx,
                               const scene::Transform& before,
                               const char* desc)
{
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    if (!ctx.activeScene || !ctx.undoStack || !ctx.undoStack->IsRecordingEnabled()) return;

    const scene::Transform after = go.transform;
    if (TransformEquals(before, after)) return;

    scene::Scene*     scene      = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto        markDirty  = ctx.markSceneDirty;

    auto apply = [scene, instanceId, markDirty](const scene::Transform& value) {
        if (auto* target = scene->FindByGuid(instanceId)) {
            target->transform = value;
            if (markDirty) markDirty();
        }
    };
    ctx.undoStack->Push(std::make_unique<LambdaCommand>(desc,
        [apply, after]()  { apply(after); },
        [apply, before]() { apply(before); }));
}

// スケールの等比リンク状態。
// WHY: エディターの操作モードであってシーンのデータではないため、GameObject 側には
//      持たせずセッション内の 1 つのトグルとして扱う (シーンを汚さない)。
bool& UniformScaleLock() { static bool locked = false; return locked; }

// ラベルの右クリックで「この行だけ既定値へ戻す」メニューを出す。
// WHY: 「試しに動かしたが元に戻したい」は Inspector で最も多い後戻り操作。Ctrl+Z は
//      直前の他の編集まで巻き戻してしまうため、行単位で戻せる口を別に用意する。
// NOTE: ImGui の仕様上、直前に描いたアイテム (= ラベル) に紐づくので、値ウィジェットを
//       描く前に呼び、要求だけ受け取って値の適用は後で行う。
[[nodiscard]] bool RowResetRequested(const char* popupId)
{
    bool requested = false;
    if (ImGui::BeginPopupContextItem(popupId)) {
        if (ImGui::MenuItem("Reset")) requested = true;
        ImGui::EndPopup();
    }
    return requested;
}

// Transform ヘッダーの Copy / Paste / Reset メニュー (⋯ ボタン / ヘッダー右クリック)。
//
// WHY 自前で値を書き換えないか (Operator モデル Step 4):
//   以前はここが Transform の代入と Undo コマンドの生成を直接持っていた。
//   同じ操作を AI・コマンドパレットからも呼べるようにした結果、実装が 2 つになり、
//   「Inspector から貼ったときと AI から貼ったときで Undo ラベルが違う」
//   「片方だけ markSceneDirty を忘れる」といった食い違いが起きうる状態だった。
//   このメニューは operator の消費者に徹する — 実体は 1 つだけになる。
//   Docs/design/editor-operator-model.md
void DrawTransformHeaderMenu(scene::GameObject& go, EditorContext& ctx)
{
    if (!ImGui::BeginPopup("##transform_hdr_ctx")) return;

    // 対象は「今 Inspector が映しているノード」。選択と一致しない場合があるため明示する。
    OpArgs args;
    args.Set("node", go.instanceId);

    const auto item = [&ctx, &args](const char* operatorId, const char* label) {
        const bool enabled = CanInvokeOperator(ctx, operatorId, args);
        if (ImGui::MenuItem(label, nullptr, false, enabled))
            InvokeOperator(ctx, operatorId, args);
    };

    item("transform.copy", "Copy Transform");
    item("transform.paste", "Paste Transform");
    ImGui::Separator();
    item("transform.reset", "Reset Transform");
    ImGui::EndPopup();
}

} // namespace

std::string GetScriptOrderKey(const scene::ScriptComponent& component, int index)
{
    return ScriptOrderKey(component, index);
}

void DrawTransformInspectors(scene::GameObject* go, EditorContext& ctx)
{
    // Transform は常に有効なのでチェックボックスを持たない。帯はテーマのアクセント色にして、
    // 「必ず一番上にある基準のカード」であることを他のコンポーネントと区別する。
    const widgets::ComponentHeaderResult transformHeader =
        widgets::ComponentHeader("Transform", EditorTheme::ColorU32(ThemeColor::Accent), nullptr);
    // ⋯ / ヘッダー右クリック: Copy / Paste / Reset (開閉状態に関わらず有効)
    if (transformHeader.menuClicked)
        ImGui::OpenPopup("##transform_hdr_ctx");
    DrawTransformHeaderMenu(*go, ctx);

    widgets::ComponentBodyScope transformBody{};
    if (transformHeader.open) {
        transformBody = widgets::BeginComponentBody(transformHeader,
                                                    EditorTheme::ColorU32(ThemeColor::Accent));
        auto& t = go->transform;
        ImGui::Spacing();

        const bool isUI = go->GetComponent<scene::UIImage>() || go->GetComponent<scene::UIText>();

        if (isUI) {
            const float itemW = (ImGui::GetContentRegionAvail().x
                                 - ImGui::CalcTextSize("X").x * 2
                                 - ImGui::GetStyle().ItemSpacing.x * 3) * 0.5f;

            ImGui::Text("Pos");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(itemW);
            ImGui::DragFloat("##px", &t.position.x, 1.0f, 0.0f, 0.0f, "X %.0f");
            TrackTransformEdit(*go, ctx, "Change Position X");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(itemW);
            ImGui::DragFloat("##py", &t.position.y, 1.0f, 0.0f, 0.0f, "Y %.0f");
            TrackTransformEdit(*go, ctx, "Change Position Y");

            math::Vector3 euler = widgets::QuatToEulerDeg(t.rotation);
            float rotZ = euler.z;
            ImGui::Text("Rot");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##rz", &rotZ, 0.5f, -360.0f, 360.0f, "Z %.1f deg"))
                t.rotation = widgets::EulerDegToQuat({ euler.x, euler.y, rotZ });
            TrackTransformEdit(*go, ctx, "Change Rotation");

            if (auto* uiImage = go->GetComponent<scene::UIImage>()) {
                ImGui::Text("Size");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(itemW);
                ImGui::DragFloat("##sw", &t.scale.x, 1.0f, 1.0f, 0.0f, "W %.0f");
                TrackTransformEdit(*go, ctx, "Change Width");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(itemW);
                ImGui::DragFloat("##sh", &t.scale.y, 1.0f, 1.0f, 0.0f, "H %.0f");
                TrackTransformEdit(*go, ctx, "Change Height");

                // 絵を貼った直後にまずやりたいのは「元の絵の比率に戻す」で、
                // それを手計算させないための 1 手。解決できないうちは押させない。
                math::Vector2 nativeSize{};
                const bool hasNativeSize =
                    ResolveUIImageNativeSize(*uiImage, ctx, nativeSize);
                ImGui::BeginDisabled(!hasNativeSize);
                if (ImGui::SmallButton("Set Native Size")) {
                    const scene::Transform before = t;
                    t.scale.x = nativeSize.x;
                    t.scale.y = nativeSize.y;
                    PushTransformSnapshotUndo(*go, ctx, before, "Set Native Size");
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(hasNativeSize
                        ? "元画像の 1 ピクセルが画面の 1 ピクセルになる大きさへ合わせます"
                        : "テクスチャが未設定か、まだ読み込まれていません");
                }
            }
        } else {
            // ラベルを左・値を右にそろえ、成分は軸色付き (X 赤 / Y 緑 / Z 青) にする。
            // WHY: Transform だけ ImGui 既定の「値 → ラベル」順だったため、直下の
            //      コンポーネント行と値の左端が食い違い、Inspector 全体が不揃いに見えていた。
            //      軸色は「どの成分を掴んでいるか」を数え直さずに判別するためのもの。
            const float column = widgets::PropertyLabelColumnWidth();

            // 「地色 + ラベル + 右クリックのリセット要求」までを 1 か所にまとめる。
            // リセットは値ウィジェットより先に問い合わせる必要がある (ImGui の
            // コンテキストメニューは「直前のアイテム」= ラベルに紐づくため)。
            const auto beginRow = [&](const char* label,
                                      const char* popupId,
                                      bool& outResetRequested) {
                ImGui::PushID(label);
                const widgets::PropertyRowScope row = widgets::BeginPropertyRow();
                widgets::LabelEllipsis(
                    label, column - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
                outResetRequested = RowResetRequested(popupId);
                ImGui::SameLine();
                if (ImGui::GetCursorPosX() < column) ImGui::SetCursorPosX(column);
                ImGui::SetNextItemWidth(-FLT_MIN);
                return row;
            };
            const auto endRow = [](const widgets::PropertyRowScope& row) {
                widgets::EndPropertyRow(row);
                ImGui::PopID();
            };

            // ── Position ──
            {
                bool resetRequested = false;
                const widgets::PropertyRowScope row =
                    beginRow("Position", "##pos_ctx", resetRequested);
                widgets::DragAxes("##pos", t.position, 0.1f);
                TrackTransformEdit(*go, ctx, "Change Position");
                if (resetRequested) {
                    const scene::Transform before = t;
                    t.position = math::Vector3::ZERO;
                    PushTransformSnapshotUndo(*go, ctx, before, "Reset Position");
                }
                endRow(row);
            }

            // ── Rotation (内部は Quaternion、UI はオイラー角) ──
            {
                bool resetRequested = false;
                const widgets::PropertyRowScope row =
                    beginRow("Rotation", "##rot_ctx", resetRequested);
                widgets::DragQuatEuler3("##rot", t.rotation, 0.5f);
                TrackTransformEdit(*go, ctx, "Change Rotation");
                if (resetRequested) {
                    const scene::Transform before = t;
                    t.rotation = math::Quaternion::Identity();
                    PushTransformSnapshotUndo(*go, ctx, before, "Reset Rotation");
                }
                endRow(row);
            }

            // ── Scale (等比リンク付き) ──
            {
                bool resetRequested = false;
                const widgets::PropertyRowScope row =
                    beginRow("Scale", "##scale_ctx", resetRequested);
                widgets::DragScaleAxes("##scale", t.scale, UniformScaleLock(), 0.01f);
                TrackTransformEdit(*go, ctx, "Change Scale");
                if (resetRequested) {
                    const scene::Transform before = t;
                    t.scale = { 1.0f, 1.0f, 1.0f };
                    PushTransformSnapshotUndo(*go, ctx, before, "Reset Scale");
                }
                endRow(row);
            }

            // ── ワールド座標 (読み取り専用) ──
            // WHY: 上の 3 行はすべてローカル値。階層下のオブジェクトは
            //      「Position が 0,0,0 なのに原点にいない」が普通に起きるため、
            //      実際の位置を並べて出しておかないと毎回 Hierarchy を辿り直すことになる。
            //      編集はローカル側でしかできないので、こちらは表示専用にとどめる。
            if (const scene::GameObject* parent = go->GetParent()) {
                ImGui::PushID("world");
                const widgets::PropertyRowScope row = widgets::BeginPropertyRow();

                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextFaint));
                widgets::LabelEllipsis(
                    "World", column - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
                ImGui::PopStyleColor();

                ImGui::SameLine();
                if (ImGui::GetCursorPosX() < column) ImGui::SetCursorPosX(column);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%.2f   %.2f   %.2f",
                                    t.worldPosition.x, t.worldPosition.y, t.worldPosition.z);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("World position (read-only)\nLocal values above are relative to \"%s\"",
                                      parent->name.c_str());

                widgets::EndPropertyRow(row);
                ImGui::PopID();
            }

            // 非一様スケール + 半径ベースのコライダーの注意書き。
            // WHY: 球とカプセルは半径ひとつで形が決まるため、軸ごとに違う倍率を掛けられない。
            //      「見た目は潰れているのに当たり判定だけ真球」という状態は値を眺めても
            //      気付けず、原因の分からない当たり判定バグとして時間を溶かす。
            const bool nonUniform =
                std::fabs(t.scale.x - t.scale.y) > 0.001f ||
                std::fabs(t.scale.y - t.scale.z) > 0.001f;
            const bool radialCollider =
                go->GetComponent<scene::SphereColliderComponent>() ||
                go->GetComponent<scene::CapsuleColliderComponent>() ||
                go->GetComponent<scene::CylinderColliderComponent>();
            if (nonUniform && radialCollider) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Warning));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(
                    "Non-uniform scale: sphere / capsule colliders keep a single radius "
                    "and will not follow the squashed mesh.");
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
        }

        ImGui::Spacing();
        widgets::EndComponentBody(transformBody);
    }
    ImGui::Spacing();
}

void BeginScriptInspectorFrame(scene::GameObject* go, EditorContext& ctx)
{
    s_scriptFrame = {};
    auto* sc = go ? go->GetComponent<scene::ScriptComponent>() : nullptr;
    if (!sc) return;

    s_scriptFrame.active = true;
    s_scriptFrame.canTrackUndo =
        ctx.activeScene != nullptr &&
        ctx.undoStack != nullptr &&
        ctx.undoStack->IsRecordingEnabled();
    s_scriptFrame.activeBefore = ImGui::GetActiveID();

    // WHY: 眺めているだけのフレームでスナップショットを取らない。操作の開始候補
    //      (クリック / Enter / Space) が来たフレームだけ各スクリプトの現在値を控える。
    const bool mayStartEdit =
        !s_scriptUndo.active &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsKeyPressed(ImGuiKey_Enter) ||
         ImGui::IsKeyPressed(ImGuiKey_Space));

    // index → 描画前スナップショット。どのスクリプトが編集対象になるかは
    // ActiveID が確定するまで分からないため、候補フレームでは全件控えておく。
    if (s_scriptFrame.canTrackUndo && mayStartEdit) {
        s_scriptFrame.beforeSnapshots.reserve(sc->scripts.size());
        for (const auto& e : sc->scripts)
            s_scriptFrame.beforeSnapshots.push_back(
                e.script ? CaptureScriptSnapshot(*e.script) : std::string{});
    }
}

void DrawScriptCard(scene::GameObject* go, EditorContext& ctx, int index)
{
    FBZZ_PROFILE_SCOPE("Inspector::ScriptCard");

    auto* sc = go->GetComponent<scene::ScriptComponent>();
    if (!sc || index < 0 || index >= static_cast<int>(sc->scripts.size())) return;

    const int i = index;
    auto& entry = sc->scripts[static_cast<size_t>(i)];

    // 並び替えはエンジン Component とまったく同じ仕組み (COMPONENT スコープ) に乗せる。
    //
    // WHY 専用スコープをやめたか (不具合修正): 以前はスクリプトだけ別スコープ
    //   ("SCRIPT") で、Component 順リストには "Scripts" という 1 個のキーしか
    //   登録されていなかった。そのキーを持つドラッグ元 / ドロップ先を描く UI が
    //   どこにも無いため、スクリプトのカードは Component との相対位置を一切
    //   変えられず (常に最後尾に固定)、逆に Component をスクリプトより後ろへ
    //   落とすこともできなかった。カード 1 枚 = 並び順の 1 席に統一する。
    const std::string orderKey = ScriptOrderKey(*sc, i);
    const widgets::ComponentReorderTarget reorder =
        MakeComponentReorderTarget(go, ctx, orderKey.c_str());

    // ⋯ メニューの Move Up / Move Down。1 つ隣へずらすだけならメニューの方が確実で、
    // カードを畳んでいない縦長の Inspector ではドラッグの移動距離が大きくなる。
    //
    // WHY operator 経由か (Step 4): 端に居るかどうかの判定 (hasPrev / hasNext) を
    //   ここで書くと、同じ判定が operator の poll にもあり 2 箇所になる。
    //   poll は引数を見られるので、「このカードをこの方向へ動かせるか」まで
    //   operator 側 1 箇所で答えられる。淡色表示もその答えをそのまま使う。
    const auto drawMoveMenuItems = [&]() {
        OpArgs args;
        args.Set("node", go->instanceId);
        args.Set("component", orderKey);
        const auto item = [&ctx, &args](const char* operatorId, const char* label) {
            const bool enabled = CanInvokeOperator(ctx, operatorId, args);
            if (ImGui::MenuItem(label, nullptr, false, enabled))
                InvokeOperator(ctx, operatorId, args);
        };
        item("component.move_up", "Move Up");
        item("component.move_down", "Move Down");
    };

    ImGui::PushID(i);

    {
        if (entry.script) {
            const char* header = entry.script->GetTypeName();
            const widgets::ComponentHeaderResult hdr =
                widgets::ComponentHeader(header, kScriptAccent, &entry.script->enabled,
                                         true, reorder);
            if (hdr.menuClicked)
                ImGui::OpenPopup("##script_opts");

            if (ImGui::BeginPopup("##script_opts")) {
                drawMoveMenuItems();
                ImGui::Separator();
                if (ImGui::MenuItem("Remove Component"))
                    s_scriptFrame.removeIndex = i;
                ImGui::EndPopup();
            }

            if (hdr.open) {
                const widgets::ComponentBodyScope body =
                    widgets::BeginComponentBody(hdr, kScriptAccent);
                ImGui::Spacing();
                // FBZZ_EXECUTE_ALWAYS の宣言はスクリプト側にしか無く、Inspector からは
                // 「今シーンが勝手に書き換わっている」ことの説明が付かない。カード上で名指しする。
                if (entry.script->ExecuteInEditMode()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Info));
                    ImGui::TextUnformatted("Runs in Edit Mode");
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "FBZZ_EXECUTE_ALWAYS: this script updates while editing, not just in Play.\n"
                            "Values it writes become part of the scene and are saved with it.");
                    ImGui::Spacing();
                }
                // 不足している必須コンポーネントはフィールドより先に出す。
                // WHY 先頭か: 値をいじっても直らない類の問題なので、パラメーター調整に
                //      入る前に目に入る位置へ置く。
                if (DrawScriptRequirementBanner(*go, *entry.script, ctx)) {
                    if (ctx.markSceneDirty) ctx.markSceneDirty();
                }
                ImGuiReflector reflector;
                reflector.m_projectRoot = ctx.projectRoot; // アセットスロットの "..." パス検索用
                if (ctx.activeScene) {
                    reflector.m_goNameResolver = [scene = ctx.activeScene](scene::EntityID id) -> std::string {
                        auto* target = scene->GetGameObject(id);
                        return target ? target->name : "(Missing)";
                    };
                    // ◎ピッカー用の候補一覧。シーンの全 GameObject を (ID, 名前) で列挙する。
                    reflector.m_goListProvider =
                        [scene = ctx.activeScene]() -> std::vector<std::pair<scene::EntityID, std::string>> {
                            std::vector<std::pair<scene::EntityID, std::string>> out;
                            for (auto& go : scene->GameObjects())
                                out.emplace_back(go.GetID(), go.name);
                            return out;
                        };
                    reflector.m_tagListProvider =
                        [scene = ctx.activeScene]() -> std::vector<std::string> {
                            std::vector<std::string> tags;
                            for (auto& object : scene->GameObjects()) {
                                if (!object.tag.empty() &&
                                    std::find(tags.begin(), tags.end(), object.tag) == tags.end())
                                    tags.push_back(object.tag);
                            }
                            std::sort(tags.begin(), tags.end());
                            return tags;
                        };
                    // 型付き参照 (FBZZ_REF<T>) の型チェック: 対象 GO が typeName の
                    // Script かコンポーネントを持つか。typeName が空 (任意 GameObject) なら常に true。
                    // ドロップ検証とピッカーの絞り込みが同じ 1 本を使う。
                    reflector.m_refTypeValidator =
                        [scene = ctx.activeScene](scene::EntityID id, const char* typeName) -> bool {
                            if (!typeName || !typeName[0]) return true;
                            auto* target = scene->GetGameObject(id);
                            if (!target) return false;
                            if (auto* sc = target->GetComponent<scene::ScriptComponent>()) {
                                // WHY 名前一致ではなく IsA か: FBZZ_REF(EnemyAiBase, ...) の
                                //     スロットへ MiteComponent を持つ GO を落とせるようにするため。
                                //     基底型で受ける参照は継承鎖のどこで一致してもよい。
                                for (const auto& entry : sc->scripts)
                                    if (entry.script && entry.script->IsA(typeName))
                                        return true;
                            }
                            // コンポーネント参照 (FBZZ_REF(LightComponent, ...) 等)。
                            // 突き合わせるのは ComponentRegistry の serializedName で、
                            // これは登録マクロの #Type — FBZZ_REF に書いた型名と同じ文字列になる。
                            return HasRegisteredComponentByName(*target, typeName);
                        };
                }
                // WHY: どのスクリプトが編集対象になったかは、その Reflect() の描画中に
                //      ActiveID が確定したかどうかで判別する。index だけを後から推測すると
                //      複数スクリプトを付けた GameObject で取り違える。
                const ImGuiID activeBeforeScript = ImGui::GetActiveID();
                entry.script->Reflect(reflector);
                if (reflector.m_changed)
                    entry.script->OnValidate();
                const ImGuiID activeAfterScript = ImGui::GetActiveID();
                if (activeAfterScript != 0 && activeAfterScript != activeBeforeScript)
                    s_scriptFrame.editingIndex = i;

                ImGui::Spacing();
                widgets::EndComponentBody(body);
            }
            ImGui::Spacing();
        } else if (entry.serialized && !entry.serialized->type.empty()) {
            // WHY: DLL ビルド中は "Building..." と表示し、完了後に自動復元されることを示す。
            //      それ以外 (DLL 未ロード・ビルド失敗) は "Missing Script" のままにして問題を明示する。
            const bool isBuilding = ctx.scriptReloadBusy;
            const std::string header = (isBuilding ? "Building... " : "Missing Script: ")
                                       + entry.serialized->type;
            // 帯を警告色にして、正常なスクリプトカードと一目で区別できるようにする。
            const ImU32 accent = EditorTheme::ColorU32(
                isBuilding ? ThemeColor::Warning : ThemeColor::Danger);
            // DLL 未ロードのカードも並び替え対象にする。
            // WHY: 実行順は serialized のまま保存されるので、ビルドが通っていない間でも
            //   順番を整えられないと「直せるのはビルド成功後だけ」という余計な待ちが生まれる。
            const widgets::ComponentHeaderResult hdr =
                widgets::ComponentHeader(header.c_str(), accent, &entry.serialized->enabled,
                                         true, reorder);
            if (hdr.menuClicked)
                ImGui::OpenPopup("##missing_script_opts");

            if (ImGui::BeginPopup("##missing_script_opts")) {
                drawMoveMenuItems();
                ImGui::Separator();
                if (ImGui::MenuItem("Remove Component"))
                    s_scriptFrame.removeIndex = i;
                ImGui::EndPopup();
            }

            if (hdr.open) {
                const widgets::ComponentBodyScope body = widgets::BeginComponentBody(hdr, accent);
                ImGui::Spacing();
                if (isBuilding)
                    ImGui::TextDisabled("Script DLL is building. Fields will be restored on completion.");
                else
                    ImGui::TextDisabled("Script DLL is not loaded. Serialized fields are preserved.");
                ImGui::Spacing();
                widgets::EndComponentBody(body);
            }
            ImGui::Spacing();
        }
    }

    ImGui::PopID();
}

void EndScriptInspectorFrame(scene::GameObject* go, EditorContext& ctx)
{
    if (!s_scriptFrame.active) return;
    s_scriptFrame.active = false;

    auto* sc = go ? go->GetComponent<scene::ScriptComponent>() : nullptr;
    if (!sc) return;

    if (s_scriptFrame.removeIndex >= 0 &&
        s_scriptFrame.removeIndex < static_cast<int>(sc->scripts.size())) {
        sc->scripts.erase(sc->scripts.begin() + s_scriptFrame.removeIndex);
        if (sc->scripts.empty()) {
            go->RemoveComponent<scene::ScriptComponent>();
            return;
        }
    }

    // 表示順 (Component 順リスト) を正として実行順を追従させる。
    //
    // WHY 毎フレーム同期するか: 並び替えは Component 順リスト側で起きるため、
    //   移動を検知する専用の経路を作るとドラッグ / メニュー / Undo / Redo の 4 か所へ
    //   同じ同期を書き写すことになる。導出を 1 か所に置けば、どの経路で順序が
    //   変わっても必ず追従し、食い違いが原理的に起きない。
    //   並びが既に一致していれば何もしない (通常フレームのコストは比較だけ)。
    if (SyncScriptOrderToDisplay(*sc, ctx, go->instanceId)) {
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }

    if (!s_scriptFrame.canTrackUndo) {
        s_scriptUndo.active = false;
        return;
    }

    ScriptUndoTracker& undo = s_scriptUndo;

    // 対象スクリプトの現在値を撮る (追跡中のものを Undo コマンドの after にする)。
    const auto captureTracked = [&]() -> std::string {
        if (undo.scriptIndex < 0 ||
            undo.scriptIndex >= static_cast<int>(sc->scripts.size()))
            return {};
        auto& e = sc->scripts[static_cast<std::size_t>(undo.scriptIndex)];
        if (!e.script || e.script->GetTypeName() != undo.typeName) return {};
        return CaptureScriptSnapshot(*e.script);
    };

    auto pushCommand = [&](const std::string& before, const std::string& after) {
        if (before == after || before.empty()) return;

        scene::Scene* scene       = ctx.activeScene;
        const std::string guid    = undo.instanceId;
        const std::string type    = undo.typeName;
        const int         index   = undo.scriptIndex;
        const auto        markDirty = ctx.markSceneDirty;

        // WHY: GameObject* も Script* も Undo までの間に無効化され得るので、
        //      GUID → ScriptComponent → index の順で毎回引き直す。型名も照合して、
        //      間にスクリプトを付け外しされていた場合に別物へ書き込むのを防ぐ。
        auto apply = [scene, guid, type, index, markDirty](const std::string& snapshot) {
            auto* target = scene->FindByGuid(guid);
            if (!target) return;
            auto* comp = target->GetComponent<scene::ScriptComponent>();
            if (!comp || index < 0 || index >= static_cast<int>(comp->scripts.size())) return;
            auto& e = comp->scripts[static_cast<std::size_t>(index)];
            if (!e.script || e.script->GetTypeName() != type) return;
            ApplyScriptSnapshot(*e.script, snapshot);
            e.script->OnValidate();
            if (markDirty) markDirty();
        };

        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Edit Script (" + type + ")",
            [apply, after]()  { apply(after); },
            [apply, before]() { apply(before); }));
    };

    const ImGuiID activeAfter    = ImGui::GetActiveID();
    const int     editingIndex   = s_scriptFrame.editingIndex;
    const auto&   beforeSnapshots = s_scriptFrame.beforeSnapshots;

    if (!undo.active && activeAfter != 0 && activeAfter != s_scriptFrame.activeBefore &&
        editingIndex >= 0 &&
        editingIndex < static_cast<int>(beforeSnapshots.size()) &&
        editingIndex < static_cast<int>(sc->scripts.size())) {
        // 編集開始: 対象スクリプトの控えを Undo の before にする。
        auto& e = sc->scripts[static_cast<std::size_t>(editingIndex)];
        undo.activeId    = activeAfter;
        undo.before      = beforeSnapshots[static_cast<std::size_t>(editingIndex)];
        undo.instanceId  = go->instanceId;
        undo.typeName    = e.script ? e.script->GetTypeName() : "";
        undo.scriptIndex = editingIndex;
        undo.active      = true;
    } else if (undo.active && activeAfter != undo.activeId) {
        // 編集終了 (別のウィジェットへ移った / 入力欄から離れた)。
        pushCommand(undo.before, captureTracked());
        undo.active = false;
    }
}

// 旧 API 互換の一括描画。Inspector は 1 枚ずつ描く経路へ移ったが、
// 他所から「このオブジェクトのスクリプトを全部出す」用途で呼べるように残す。
void DrawScriptInspectors(scene::GameObject* go, EditorContext& ctx)
{
    FBZZ_PROFILE_SCOPE("Inspector::Scripts");
    auto* sc = go ? go->GetComponent<scene::ScriptComponent>() : nullptr;
    if (!sc) return;

    BeginScriptInspectorFrame(go, ctx);
    for (int i = 0; i < static_cast<int>(sc->scripts.size()); ++i)
        DrawScriptCard(go, ctx, i);
    EndScriptInspectorFrame(go, ctx);
}

} // namespace fbzz::editor
