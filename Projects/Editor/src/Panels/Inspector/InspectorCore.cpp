/// @file    InspectorCore.cpp
/// @brief   Transform / Script の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
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

/// @note  スクリプトカードの帯色。Engine コンポーネントのどのカテゴリ色とも被らない色を当て、
/// @note  「ここから下はユーザーコード」であることを一目で分かるようにする。
constexpr ImU32 kScriptAccent = IM_COL32(120, 190, 255, 255);

/// @note  スクリプトカードを Component の表示順リストへ載せるためのキー。
/// @note  キーは .meta へ保存されるので index は使えない (1 つ足しただけで全部ずれる)。
/// @note  "Script:" を前置するのは、エンジン Component のキーが表示名そのもので衝突するため。
/// @note  同型が複数あるときの "#n" は、同じスクリプトを 2 つ付けたとき席を奪い合わないため。
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

/// @note  1 フレームのあいだ全スクリプトカードで共有する状態。
/// @note  カードは 1 枚ずつ別々のコールバックから描かれるので関数ローカルには持てない。
/// @note  Inspector は 1 フレームに 1 GameObject しか描かないので単一で足りる。
struct ScriptInspectorFrame {
    /// @note  編集開始時の値 (Undo の before)。index → スナップショット。
    std::vector<std::string> beforeSnapshots;
    ImGuiID activeBefore     = 0;
    int     editingIndex     = -1;   ///< @note この frame に編集が始まったカード
    int     removeIndex      = -1;   ///< @note ⋯ → Remove Component
    bool    canTrackUndo     = false;
    bool    active           = false; ///< @note Begin〜End の内側か
};
ScriptInspectorFrame s_scriptFrame;

/// @note  スクリプト 1 個ぶんの編集を 1 コマンドとして記録する追跡状態。
/// @note  Script は Reflect() を実装しているので、IReflector を 1 つ用意すれば型を知らないまま
/// @note  「そのスクリプトだけ」を読み書きできる。全シーン再構築にせずに済み、選択も維持される。
struct ScriptUndoTracker {
    ImGuiID     activeId = 0;
    std::string before;        ///< @note 編集開始時のスナップショット
    std::string instanceId;    ///< @note 対象 GameObject
    std::string typeName;      ///< @note 対象スクリプトの型 (index だけだと取り違える)
    int         scriptIndex = -1;
    bool        active = false;
};
ScriptUndoTracker s_scriptUndo;

/// @note  表示順 (Component 順リスト) に合わせて sc.scripts を並べ替え、リスト側のキーも
/// @note  並べ替え後の実体に合わせて書き直す。実行順は表示順から導出し、2 つの並びが
/// @note  食い違わないようにする (「上にあるカードほど先に動く」)。
/// @note  キーの "#n" は「配列の中で同じ型が何番目か」なので、同型を 2 つ付けて入れ替えると
/// @note  キーも一緒に入れ替わる。書き直さないと毎フレーム反転し続ける。
/// @return 実際に並びが変わったら true
bool SyncScriptOrderToDisplay(scene::ScriptComponent& sc,
                              EditorContext& ctx,
                              const std::string& instanceId)
{
    const int count = static_cast<int>(sc.scripts.size());
    if (count < 2) return false;

    const std::vector<std::string> displayOrder =
        ctx.editorSceneState.GetComponentOrder(instanceId);

    /// @note 表示順に現れるスクリプトキーを拾い、その順に並べたい index 列を作る。
    /// @note        併せて「リストのどの席がスクリプトだったか」も覚えておく (後でキーを埋め直す)。
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
    /// @note 表示順に載っていないカード (追加直後など) は現状の順で末尾へ回す。
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

    /// @note 並べ替え後の配列順で振り直したキーを、元のスクリプト席へ順に埋め戻す。
    if (!scriptSlots.empty()) {
        std::vector<std::string> rewritten = displayOrder;
        for (std::size_t n = 0; n < scriptSlots.size(); ++n)
            rewritten[scriptSlots[n]] = ScriptOrderKey(sc, static_cast<int>(n));
        ctx.editorSceneState.SetComponentOrder(instanceId, std::move(rewritten));
    }
    return true;
}

/// @note  FBZZ_REQUIRE_COMPONENT の不足をスクリプトカードの先頭へ出す。
/// @note  戻り値: Fix が押されて実際に追加が起きたら true (呼び出し側がシーンを dirty にする)。
/// @note  自動追加にしないのは、黙ってシーンが書き換わり Undo の粒度もずれるため。
/// @note  Animator のように足しても Controller 未設定なら動かないものもある。直すかは人が決める。
bool DrawScriptRequirementBanner(scene::GameObject& go,
                                 const scene::Script& script,
                                 EditorContext& ctx)
{
    std::vector<scene::ScriptRequirementIssue> issues;
    /// @note 任意コンポーネント (FBZZ_OPTIONAL_COMPONENT) も拾って情報として並べる。
    scene::CollectScriptRequirementIssues(go, script, issues, /*includeOptional=*/true);
    if (issues.empty()) return false;

    /// @note 「足せば直る」必須のものだけを Fix の対象にする。
    /// @note        unknown (宣言側の綴り違い) と addable=false (内部型) はボタンを出しても直らない。
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
        if (issue.kind != scene::ScriptRequirementKind::Component) {
            ImGui::TextUnformatted(scene::FormatScriptRequirementIssue(issue).c_str());
        } else if (issue.unknown) {
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
        /// @note fixable は「登録済み・追加可能・未アタッチ」だけを残した集合なので、
        /// @note        押された時点で必ず 1 個以上増える。コマンドが null になるのは
        /// @note        Undo を記録できない文脈 (Play 中など) のときだけ。
        auto command = AddMissingComponentsWithUndo(go, ctx, fixable);
        if (command && ctx.undoStack) ctx.undoStack->Push(std::move(command));
        added = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add the missing components with their default setup");

    ImGui::Spacing();
    return added;
}

/// @note  UIImage の原寸 = 元画像の 1 ピクセルが画面の 1 ピクセルになる大きさ。
/// @note  pixelsPerUnit と multiplier を両方掛ける。Sliced / Tiled は Border とタイルを
/// @note  「元画像のピクセル数 ÷ multiplier」で描くので、揃えないと 9-slice の角だけ合わない。
bool ResolveUIImageNativeSize(const scene::UIImage& image, EditorContext& ctx,
                              math::Vector2& outSize)
{
    /// @note 今 Viewport に出ている絵に合わせる。ボタンの状態差し替え中はそちらが正。
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

/// @note  ImGui の連続ドラッグを1つの Transform コマンドへまとめる。
/// @note 値が変化する各フレームを履歴へ積むと、1回のドラッグを戻すために Ctrl+Z が何十回も
/// @note        必要になるため、Activated～Deactivated を1操作とする。
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

/// @note  Transform の値クリップボードは EditorContext::transformClipboard にある
/// @note  (AI の transform.copy / transform.paste と同じ器を共有するため)。
/// @note  コピー/貼り付け/リセットの実体は Editor/Op/InspectorOperators.cpp。

/// @note  Transform をメニュー操作で書き換えた際の Undo コマンドを積む
/// @note  (連続ドラッグ用の TrackTransformEdit とは別経路)。
/// @note  シーン全体のスナップショットにすると EntityID が振り直され、選択・ロックが毎回消える。
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

/// @note  スケールの等比リンク状態。
/// @note エディターの操作モードであってシーンのデータではないため、GameObject 側には持たせず
/// @note        セッション内の 1 つのトグルとして扱う (シーンを汚さない)。
bool& UniformScaleLock() { static bool locked = false; return locked; }

/// @note  ラベルの右クリックで「この行だけ既定値へ戻す」メニューを出す。
/// @note  Ctrl+Z は直前の他の編集まで巻き戻すので、行単位で戻せる口を別に用意する。
/// @note  直前に描いたアイテム (= ラベル) に紐づくので、値ウィジェットを描く前に呼び、
/// @note  要求だけ受け取って値の適用は後で行う。
[[nodiscard]] bool RowResetRequested(const char* popupId)
{
    bool requested = false;
    if (ImGui::BeginPopupContextItem(popupId)) {
        if (ImGui::MenuItem("Reset")) requested = true;
        ImGui::EndPopup();
    }
    return requested;
}

/// @note  Transform ヘッダーの Copy / Paste / Reset メニュー (⋯ ボタン / ヘッダー右クリック)。
/// @note  値の書き換えは自前で持たず operator の消費者に徹する。実装が 2 つあると
/// @note  「AI から貼ったときだけ Undo ラベルが違う」といった食い違いが起きる。
/// @note  Docs/design/editor-operator-model.md
void DrawTransformHeaderMenu(scene::GameObject& go, EditorContext& ctx)
{
    if (!ImGui::BeginPopup("##transform_hdr_ctx")) return;

    /// @note 対象は「今 Inspector が映しているノード」。選択と一致しない場合があるため明示する。
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

} /// @note namespace

std::string GetScriptOrderKey(const scene::ScriptComponent& component, int index)
{
    return ScriptOrderKey(component, index);
}

void DrawTransformInspectors(scene::GameObject* go, EditorContext& ctx)
{
    /// @note Transform は常に有効なのでチェックボックスを持たない。帯はテーマのアクセント色にして、
    /// @note        「必ず一番上にある基準のカード」であることを他のコンポーネントと区別する。
    const widgets::ComponentHeaderResult transformHeader =
        widgets::ComponentHeader("Transform", EditorTheme::ColorU32(ThemeColor::Accent), nullptr);
    /// @note ⋯ / ヘッダー右クリック: Copy / Paste / Reset (開閉状態に関わらず有効)
    if (transformHeader.menuClicked)
        ImGui::OpenPopup("##transform_hdr_ctx");
    DrawTransformHeaderMenu(*go, ctx);

    widgets::ComponentBodyScope transformBody{};
    if (transformHeader.open) {
        transformBody = widgets::BeginComponentBody(transformHeader,
                                                    EditorTheme::ColorU32(ThemeColor::Accent));
        auto& t = go->transform;
        ImGui::Spacing();

        const bool isUI = scene::IsUIElement(*go);

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

            /// @note 実測サイズの文字以外は scale.xy が矩形そのものなので寸法として出す
            /// @note        (絵を持たない Mask や Scroll View もここでしか大きさを決められない)。
            /// @note        ストレッチしている軸は親から決まるので触らせない (「効かない欄」になる)。
            const scene::UIAnchor* uiAnchor = nullptr;
            if (const auto* image = go->GetComponent<scene::UIImage>())
                uiAnchor = &image->anchoring;
            else if (const auto* text = go->GetComponent<scene::UIText>())
                uiAnchor = &text->anchoring;
            const bool stretchX = uiAnchor && uiAnchor->stretchX;
            const bool stretchY = uiAnchor && uiAnchor->stretchY;

            if (!scene::HasMeasuredUISize(*go)) {
                ImGui::Text("Size");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(itemW);
                ImGui::BeginDisabled(stretchX);
                ImGui::DragFloat("##sw", &t.scale.x, 1.0f, 1.0f, 0.0f,
                                 stretchX ? "W (stretch)" : "W %.0f");
                ImGui::EndDisabled();
                TrackTransformEdit(*go, ctx, "Change Width");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(itemW);
                ImGui::BeginDisabled(stretchY);
                ImGui::DragFloat("##sh", &t.scale.y, 1.0f, 1.0f, 0.0f,
                                 stretchY ? "H (stretch)" : "H %.0f");
                ImGui::EndDisabled();
                TrackTransformEdit(*go, ctx, "Change Height");
                if ((stretchX || stretchY)
                    && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("ストレッチしている軸の大きさは親から決まります。"
                                      "余白は UI Image / UI Text の offsetMax と position で調整します");
                }
            }

            if (auto* uiImage = go->GetComponent<scene::UIImage>()) {
                /// @note 絵を貼った直後にまずやりたいのは「元の絵の比率に戻す」で、
                /// @note        それを手計算させないための 1 手。解決できないうちは押させない。
                math::Vector2 nativeSize{};
                const bool hasNativeSize =
                    ResolveUIImageNativeSize(*uiImage, ctx, nativeSize);
                /// @note 両軸ストレッチなら原寸に戻す先が無い。
                const bool canSetNative = hasNativeSize && !(stretchX && stretchY);
                ImGui::BeginDisabled(!canSetNative);
                if (ImGui::SmallButton("Set Native Size")) {
                    const scene::Transform before = t;
                    /// @note ストレッチしている軸には書かない。書いても捨てられる値で
                    /// @note        Undo の履歴だけが増える。
                    if (!stretchX) t.scale.x = nativeSize.x;
                    if (!stretchY) t.scale.y = nativeSize.y;
                    PushTransformSnapshotUndo(*go, ctx, before, "Set Native Size");
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(!hasNativeSize
                        ? "テクスチャが未設定か、まだ読み込まれていません"
                        : canSetNative
                            ? "元画像の 1 ピクセルが画面の 1 ピクセルになる大きさへ合わせます"
                            : "両軸ともストレッチしているため、大きさは親から決まります");
                }
            }
        } else {
            /// @note ラベルを左・値を右にそろえ、成分は軸色付き (X 赤 / Y 緑 / Z 青) にする。Transform だけ
            /// @note        ImGui 既定の «値 → ラベル» 順で直下のコンポーネント行と左端が食い違っていたため。
            /// @note        軸色は «どの成分を掴んでいるか» を数え直さずに判別するためのもの。
            const float column = widgets::PropertyLabelColumnWidth();

            /// @note 「地色 + ラベル + 右クリックのリセット要求」までを 1 か所にまとめる。
            /// @note        リセットは値ウィジェットより先に問い合わせる必要がある (ImGui の
            /// @note        コンテキストメニューは「直前のアイテム」= ラベルに紐づくため)。
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

            /// @name Position
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

            /// @name Rotation (内部は Quaternion、UI はオイラー角)
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

            /// @name Scale (等比リンク付き)
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

            /// @name ワールド座標 (読み取り専用)
            /// @note 上の 3 行はローカル値で、階層下では「Position が 0,0,0 なのに原点にいない」が
            /// @note        普通に起きる。編集はローカル側でしかできないので、こちらは表示専用。
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

            /// @note 非一様スケール + 半径ベースのコライダーの注意書き。球とカプセルは半径ひとつで
            /// @note        形が決まり、軸ごとの倍率を掛けられない。«見た目は潰れているのに当たり判定は
            /// @note        真球» は値を眺めても気付けず、原因不明の当たり判定バグとして時間を溶かす。
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

    /// @note 眺めているだけのフレームでスナップショットを取らない。操作の開始候補
    /// @note        (クリック / Enter / Space) が来たフレームだけ各スクリプトの現在値を控える。
    const bool mayStartEdit =
        !s_scriptUndo.active &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsKeyPressed(ImGuiKey_Enter) ||
         ImGui::IsKeyPressed(ImGuiKey_Space));

    /// @note index → 描画前スナップショット。どのスクリプトが編集対象になるかは
    /// @note        ActiveID が確定するまで分からないため、候補フレームでは全件控えておく。
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

    /// @note 並び替えはエンジン Component とまったく同じ仕組み (COMPONENT スコープ) に乗せる。
    /// @note        カード 1 枚 = 並び順の 1 席。専用スコープにすると Component との相対位置を
    /// @note        変えられなくなる。
    const std::string orderKey = ScriptOrderKey(*sc, i);
    const widgets::ComponentReorderTarget reorder =
        MakeComponentReorderTarget(go, ctx, orderKey.c_str());

    /// @note ⋯ メニューの Move Up / Move Down。1 つ隣へずらすだけならメニューの方が確実。
    /// @note        端の判定 (hasPrev / hasNext) は operator の poll が引数を見て答える。
    /// @note        ここで書くと同じ判定が 2 箇所になる。淡色表示もその答えを使う。
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
            auto& requirementFocus = ctx.scriptRequirementReview.focus;
            const bool requirementTarget = requirementFocus.nodeId == go->instanceId &&
                requirementFocus.scriptId == entry.script->InspectionId();
            const bool navigateRequirement = requirementTarget && requirementFocus.scrollPending;
            const float requirementCardY = ImGui::GetCursorPosY();
            if (navigateRequirement) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
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
                /// @note FBZZ_EXECUTE_ALWAYS の宣言はスクリプト側にしか無く、Inspector からは
                /// @note        「今シーンが勝手に書き換わっている」ことの説明が付かない。カード上で名指しする。
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
                /// @note 不足している必須コンポーネントはフィールドより先に出す。値をいじっても
                /// @note        直らない類の問題なので、パラメーター調整に入る前に目に入る位置へ置く。
                if (DrawScriptRequirementBanner(*go, *entry.script, ctx)) {
                    if (ctx.markSceneDirty) ctx.markSceneDirty();
                }
                if (requirementTarget) {
                    ImGui::TextColored({1.0f, 0.72f, 0.25f, 1}, "必須設定の確認箇所");
                    ImGui::TextWrapped("%s", requirementFocus.message.c_str());
                    if (ImGui::SmallButton("修正後に再検証")) {
                        OpArgs args;
                        args.Set("showPanel", true);
                        InvokeOperator(ctx, "script.requirements.validate", args);
                    }
                }
                ImGuiReflector reflector;
                if (requirementTarget) {
                    reflector.m_focusField = requirementFocus.field;
                    reflector.m_focusScrollRequested = navigateRequirement;
                }
                /// @note アセットスロットの "..." パス検索用
                reflector.m_projectRoot = ctx.projectRoot;
                if (ctx.activeScene) {
                    reflector.m_goNameResolver = [scene = ctx.activeScene](scene::EntityID id) -> std::string {
                        auto* target = scene->GetGameObject(id);
                        return target ? target->name : "(Missing)";
                    };
                    /// @note ◎ピッカー用の候補一覧。シーンの全 GameObject を (ID, 名前) で列挙する。
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
                    /// @note 型付き参照 (FBZZ_REF<T>) の型チェック: 対象 GO が typeName の
                    /// @note        Script かコンポーネントを持つか。typeName が空 (任意 GameObject) なら常に true。
                    /// @note        ドロップ検証とピッカーの絞り込みが同じ 1 本を使う。
                    reflector.m_refTypeValidator =
                        [scene = ctx.activeScene](scene::EntityID id, const char* typeName) -> bool {
                            if (!typeName || !typeName[0]) return true;
                            auto* target = scene->GetGameObject(id);
                            if (!target) return false;
                            if (auto* sc = target->GetComponent<scene::ScriptComponent>()) {
                                /// @note 名前一致ではなく IsA で判定するのは、FBZZ_REF(EnemyAiBase, ...) の
                                /// @note        スロットへ MiteComponent を持つ GO を落とせるようにするため。
                                /// @note        基底型で受ける参照は継承鎖のどこで一致してもよい。
                                for (const auto& entry : sc->scripts)
                                    if (entry.script && entry.script->IsA(typeName))
                                        return true;
                            }
                            /// @note コンポーネント参照 (FBZZ_REF(LightComponent, ...) 等)。
                            /// @note        突き合わせるのは ComponentRegistry の serializedName で、
                            /// @note        これは登録マクロの #Type — FBZZ_REF に書いた型名と同じ文字列になる。
                            return HasRegisteredComponentByName(*target, typeName);
                        };
                }
                /// @note どのスクリプトが編集対象になったかは、その Reflect() の描画中に
                /// @note        ActiveID が確定したかどうかで判別する。index だけを後から推測すると
                /// @note        複数スクリプトを付けた GameObject で取り違える。
                const ImGuiID activeBeforeScript = ImGui::GetActiveID();
                entry.script->Reflect(reflector);
                if (navigateRequirement) {
                    if (!reflector.m_focusFound)
                        ImGui::SetScrollFromPosY(requirementCardY, 0.2f);
                    requirementFocus.scrollPending = false;
                }
                if (reflector.m_changed)
                    entry.script->ExecuteProfiledCallback(&scene::Script::OnValidate, scene::ScriptCallbackKind::VALIDATE, "OnValidate");
                const ImGuiID activeAfterScript = ImGui::GetActiveID();
                if (activeAfterScript != 0 && activeAfterScript != activeBeforeScript)
                    s_scriptFrame.editingIndex = i;

                ImGui::Spacing();
                widgets::EndComponentBody(body);
            }
            ImGui::Spacing();
        } else if (entry.serialized && !entry.serialized->type.empty()) {
            /// @note DLL ビルド中は "Building..." と表示し、完了後に自動復元されることを示す。
            /// @note        それ以外 (DLL 未ロード・ビルド失敗) は "Missing Script" のままにして問題を明示する。
            const bool isBuilding = ctx.scriptReloadBusy;
            const std::string header = (isBuilding ? "Building... " : "Missing Script: ")
                                       + entry.serialized->type;
            /// @note 帯を警告色にして、正常なスクリプトカードと一目で区別できるようにする。
            const ImU32 accent = EditorTheme::ColorU32(
                isBuilding ? ThemeColor::Warning : ThemeColor::Danger);
            /// @note DLL 未ロードのカードも並び替え対象にする。実行順は serialized のまま保存されるため、
            /// @note        ビルドが通っていない間に整えられないと «直せるのはビルド成功後だけ» という
            /// @note        余計な待ちが生まれる。
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

    /// @note 表示順 (Component 順リスト) を正として実行順を追従させる。
    /// @note        移動を検知する専用経路を作るとドラッグ / メニュー / Undo / Redo の 4 か所へ
    /// @note        同じ同期を書き写すことになる。毎フレーム導出すればどの経路でも必ず追従する
    /// @note        (一致していれば比較だけで終わる)。
    if (SyncScriptOrderToDisplay(*sc, ctx, go->instanceId)) {
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }

    if (!s_scriptFrame.canTrackUndo) {
        s_scriptUndo.active = false;
        return;
    }

    ScriptUndoTracker& undo = s_scriptUndo;

    /// @note 対象スクリプトの現在値を撮る (追跡中のものを Undo コマンドの after にする)。
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

        /// @note GameObject* も Script* も Undo までの間に無効化され得るので、
        /// @note        GUID → ScriptComponent → index の順で毎回引き直す。型名も照合して、
        /// @note        間にスクリプトを付け外しされていた場合に別物へ書き込むのを防ぐ。
        auto apply = [scene, guid, type, index, markDirty](const std::string& snapshot) {
            auto* target = scene->FindByGuid(guid);
            if (!target) return;
            auto* comp = target->GetComponent<scene::ScriptComponent>();
            if (!comp || index < 0 || index >= static_cast<int>(comp->scripts.size())) return;
            auto& e = comp->scripts[static_cast<std::size_t>(index)];
            if (!e.script || e.script->GetTypeName() != type) return;
            ApplyScriptSnapshot(*e.script, snapshot);
                                e.script->ExecuteProfiledCallback(&scene::Script::OnValidate, scene::ScriptCallbackKind::VALIDATE, "OnValidate");
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
        /// @note 編集開始: 対象スクリプトの控えを Undo の before にする。
        auto& e = sc->scripts[static_cast<std::size_t>(editingIndex)];
        undo.activeId    = activeAfter;
        undo.before      = beforeSnapshots[static_cast<std::size_t>(editingIndex)];
        undo.instanceId  = go->instanceId;
        undo.typeName    = e.script ? e.script->GetTypeName() : "";
        undo.scriptIndex = editingIndex;
        undo.active      = true;
    } else if (undo.active && activeAfter != undo.activeId) {
        /// @note 編集終了 (別のウィジェットへ移った / 入力欄から離れた)。
        pushCommand(undo.before, captureTracked());
        undo.active = false;
    }
}

/// @note  旧 API 互換の一括描画。Inspector は 1 枚ずつ描く経路へ移ったが、
/// @note  他所から「このオブジェクトのスクリプトを全部出す」用途で呼べるように残す。
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

} /// @note namespace fbzz::editor
