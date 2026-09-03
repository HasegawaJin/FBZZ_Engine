/// @file    InspectorMultiEdit.cpp
/// @brief   複数選択時の Inspector。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY: 従来は Transform を「全員をプライマリの値に揃える」だけで、共通コンポーネントは
/// BulletText で名前を並べるだけだった。レベル調整では「選んだ 20 個のライトの強度を
/// まとめて下げる」「全員を +Y に 2m ずらす」が毎日発生するため、
/// 1) Transform に Set / Offset の 2 モード
/// 2) 共通コンポーネントの実編集 (Reflect() 経由で自動生成)
/// 3) 列挙をコンポーネントレジストリ由来にして追加漏れを無くす
/// の 3 点を入れる。
#include "InspectorMultiEdit.hpp"
#include "InspectorCommon.hpp"

#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>

#include <imgui.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

// ── 一括編集用リフレクタ ──────────────────────────────────────────────────────
// 基底 (ImGuiReflector) の描画をそのまま使い、呼び出し前後で値が変わったフィールドを
// 「名前 + 新しい値」として記録する。
// WHY: ImGuiReflector は値を直接書き換えるだけで「どのフィールドが変わったか」を返さない。
//      これが分かれば、同じ Reflect() をもう一度回して他の選択オブジェクトの同名フィールド
//      だけへ値を配れる。コンポーネントごとに一括編集 UI を手書きしなくて済む。
struct MultiEditReflector final : ImGuiReflector {
    struct Change {
        enum class Kind { Float, Int, Bool, Vec2, Vec3, Vec4, Quat, String };
        std::string      name;
        Kind             kind = Kind::Float;
        float            f  = 0.0f;
        int              i  = 0;
        bool             b  = false;
        math::Vector2    v2{};
        math::Vector3    v3{};
        math::Vector4    v4{};
        math::Quaternion q{};
        std::string      s;
    };
    std::vector<Change> changes;

    void Field(const char* name, float& v) override
    {
        const float before = v;
        ImGuiReflector::Field(name, v);
        if (v != before) Record(name, Change::Kind::Float, [&](Change& c) { c.f = v; });
    }
    void Field(const char* name, int& v) override
    {
        const int before = v;
        ImGuiReflector::Field(name, v);
        if (v != before) Record(name, Change::Kind::Int, [&](Change& c) { c.i = v; });
    }
    void Field(const char* name, bool& v) override
    {
        const bool before = v;
        ImGuiReflector::Field(name, v);
        if (v != before) Record(name, Change::Kind::Bool, [&](Change& c) { c.b = v; });
    }
    void Field(const char* name, math::Vector2& v) override
    {
        const math::Vector2 before = v;
        ImGuiReflector::Field(name, v);
        if (v.x != before.x || v.y != before.y)
            Record(name, Change::Kind::Vec2, [&](Change& c) { c.v2 = v; });
    }
    void Field(const char* name, math::Vector3& v) override
    {
        const math::Vector3 before = v;
        ImGuiReflector::Field(name, v);
        if (v.x != before.x || v.y != before.y || v.z != before.z)
            Record(name, Change::Kind::Vec3, [&](Change& c) { c.v3 = v; });
    }
    void Field(const char* name, math::Vector4& v) override
    {
        const math::Vector4 before = v;
        ImGuiReflector::Field(name, v);
        if (v.x != before.x || v.y != before.y || v.z != before.z || v.w != before.w)
            Record(name, Change::Kind::Vec4, [&](Change& c) { c.v4 = v; });
    }
    void Field(const char* name, math::Quaternion& v) override
    {
        const math::Quaternion before = v;
        ImGuiReflector::Field(name, v);
        if (v.x != before.x || v.y != before.y || v.z != before.z || v.w != before.w)
            Record(name, Change::Kind::Quat, [&](Change& c) { c.q = v; });
    }
    void Field(const char* name, std::string& v) override
    {
        const std::string before = v;
        ImGuiReflector::Field(name, v);
        if (v != before) Record(name, Change::Kind::String, [&](Change& c) { c.s = v; });
    }
    void AssetField(const char* name,
                    scene::ScriptAssetReference& value,
                    scene::ScriptAssetType type) override
    {
        const std::string before = value.ResolvePath();
        ImGuiReflector::AssetField(name, value, type);
        const std::string after = value.ResolvePath();
        if (after != before)
            Record(name, Change::Kind::String, [&](Change& c) { c.s = after; });
    }
    void FloatRange(const char* name, float& v, float min, float max) override
    {
        const float before = v;
        ImGuiReflector::FloatRange(name, v, min, max);
        if (v != before) Record(name, Change::Kind::Float, [&](Change& c) { c.f = v; });
    }
    void IntRange(const char* name, int& v, int min, int max) override
    {
        const int before = v;
        ImGuiReflector::IntRange(name, v, min, max);
        if (v != before) Record(name, Change::Kind::Int, [&](Change& c) { c.i = v; });
    }
    void Enum(const char* name, int& v, std::span<const char* const> labels) override
    {
        const int before = v;
        ImGuiReflector::Enum(name, v, labels);
        if (v != before) Record(name, Change::Kind::Int, [&](Change& c) { c.i = v; });
    }
    void Flags(const char* name, int& v, std::span<const char* const> labels) override
    {
        const int before = v;
        ImGuiReflector::Flags(name, v, labels);
        if (v != before) Record(name, Change::Kind::Int, [&](Change& c) { c.i = v; });
    }

private:
    template<typename Fill>
    void Record(const char* name, Change::Kind kind, Fill fill)
    {
        Change c;
        c.name = PersistentKey(name);
        c.kind = kind;
        fill(c);
        changes.push_back(std::move(c));
    }
};

// 記録された変更を「同名フィールドだけ」へ書き込むリフレクタ。
// WHY: 変更されていないフィールドまでプライマリの値で塗ると、
//      Intensity を触っただけで他オブジェクトの色や範囲まで揃ってしまう (Unity は触った値だけ揃える)。
struct ApplyFieldReflector final : scene::IReflector {
    const MultiEditReflector::Change* change = nullptr;

    bool Matches(const char* name, MultiEditReflector::Change::Kind kind) const
    {
        return change && change->kind == kind && change->name == PersistentKey(name);
    }

    void Field(const char* name, float& v) override
    {
        if (Matches(name, MultiEditReflector::Change::Kind::Float)) v = change->f;
    }
    void Field(const char* name, int& v) override
    {
        if (Matches(name, MultiEditReflector::Change::Kind::Int)) v = change->i;
    }
    void Field(const char* name, bool& v) override
    {
        if (Matches(name, MultiEditReflector::Change::Kind::Bool)) v = change->b;
    }
    void Field(const char* name, math::Vector2& v) override
    {
        if (Matches(name, MultiEditReflector::Change::Kind::Vec2)) v = change->v2;
    }
    void Field(const char* name, math::Vector3& v) override
    {
        if (Matches(name, MultiEditReflector::Change::Kind::Vec3)) v = change->v3;
    }
    void Field(const char* name, math::Vector4& v) override
    {
        if (Matches(name, MultiEditReflector::Change::Kind::Vec4)) v = change->v4;
    }
    void Field(const char* name, math::Quaternion& v) override
    {
        if (Matches(name, MultiEditReflector::Change::Kind::Quat)) v = change->q;
    }
    void Field(const char* name, std::string& v) override
    {
        if (Matches(name, MultiEditReflector::Change::Kind::String)) v = change->s;
    }
    void AssetField(const char* name,
                    scene::ScriptAssetReference& value,
                    scene::ScriptAssetType) override
    {
        if (Matches(name, MultiEditReflector::Change::Kind::String))
            value.SetPath(change->s);
    }
};

// ── Transform 一括編集 ───────────────────────────────────────────────────────

enum class TransformEditMode { Set, Offset };

// 選択全体の Transform を before/after で 1 コマンドにまとめて Undo へ積む。
void PushBulkTransformUndo(EditorContext& ctx,
                           const std::vector<std::string>& guids,
                           const std::vector<scene::Transform>& before,
                           const std::vector<scene::Transform>& after,
                           const char* description)
{
    if (!ctx.undoStack || !ctx.undoStack->IsRecordingEnabled() || !ctx.activeScene) return;
    if (guids.empty()) return;

    scene::Scene* scene = ctx.activeScene;
    const auto markDirty = ctx.markSceneDirty;
    auto applyAll = [scene, guids, markDirty](const std::vector<scene::Transform>& values) {
        for (std::size_t i = 0; i < guids.size() && i < values.size(); ++i)
            if (auto* target = scene->FindByGuid(guids[i]))
                target->transform = values[i];
        if (markDirty) markDirty();
    };
    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        description,
        [applyAll, after]()  { applyAll(after); },
        [applyAll, before]() { applyAll(before); }));
}

std::vector<std::string> GuidsOf(const std::vector<scene::GameObject*>& gos)
{
    std::vector<std::string> guids;
    guids.reserve(gos.size());
    for (auto* g : gos) guids.push_back(g->instanceId);
    return guids;
}

std::vector<scene::Transform> TransformsOf(const std::vector<scene::GameObject*>& gos)
{
    std::vector<scene::Transform> values;
    values.reserve(gos.size());
    for (auto* g : gos) values.push_back(g->transform);
    return values;
}

// 「全員を同じ値にする」モード。従来の挙動をそのまま踏襲する。
void DrawTransformSetMode(EditorContext& ctx, const std::vector<scene::GameObject*>& gos)
{
    const auto& refT = gos.front()->transform;

    bool posAllSame = true, rotAllSame = true, scaleAllSame = true;
    for (std::size_t i = 1; i < gos.size(); ++i) {
        const auto& t = gos[i]->transform;
        if (t.position.x != refT.position.x || t.position.y != refT.position.y ||
            t.position.z != refT.position.z)
            posAllSame = false;
        if (t.rotation.x != refT.rotation.x || t.rotation.y != refT.rotation.y ||
            t.rotation.z != refT.rotation.z || t.rotation.w != refT.rotation.w)
            rotAllSame = false;
        if (t.scale.x != refT.scale.x || t.scale.y != refT.scale.y || t.scale.z != refT.scale.z)
            scaleAllSame = false;
    }

    // ドラッグ 1 回を 1 コマンドにまとめるための編集状態。
    struct MultiTransformEdit {
        std::vector<std::string>      guids;
        std::vector<scene::Transform> before;
        bool                          active = false;
    };
    static MultiTransformEdit edit;

    auto track = [&](bool changed, const char* description) {
        if (ImGui::IsItemActivated()) {
            edit.guids  = GuidsOf(gos);
            edit.before = TransformsOf(gos);
            edit.active = true;
        }
        if (!changed && !ImGui::IsItemDeactivatedAfterEdit()) return;
        // WHY: 反映は各フィールドの changed ハンドラ側で済ませている。ここで transform 全体を
        //      コピーすると、Position を触っただけで他の回転・スケールまで潰れる。
        if (!ImGui::IsItemDeactivatedAfterEdit()) return;
        if (edit.active)
            PushBulkTransformUndo(ctx, edit.guids, edit.before, TransformsOf(gos), description);
        if (ctx.markSceneDirty) ctx.markSceneDirty();
        edit.active = false;
    };

    const ImVec4 mutedColor = EditorTheme::Color(ThemeColor::TextMuted);

    float pos[3] = { refT.position.x, refT.position.y, refT.position.z };
    if (!posAllSame) ImGui::PushStyleColor(ImGuiCol_Text, mutedColor);
    const bool posChanged =
        widgets::DragAxes(posAllSame ? "Position" : "Position (---)", pos, 3, 0.1f);
    if (!posAllSame) ImGui::PopStyleColor();
    if (posChanged)
        for (auto* g : gos) g->transform.position = { pos[0], pos[1], pos[2] };
    track(posChanged, "Move (Multi)");

    {
        const math::Vector3 euler = widgets::QuatToEulerDeg(refT.rotation);
        float rot[3] = { euler.x, euler.y, euler.z };
        if (!rotAllSame) ImGui::PushStyleColor(ImGuiCol_Text, mutedColor);
        const bool rotChanged =
            widgets::DragAxes(rotAllSame ? "Rotation" : "Rotation (---)", rot, 3, 0.5f,
                              0.0f, 0.0f, "%.1f");
        if (!rotAllSame) ImGui::PopStyleColor();
        if (rotChanged) {
            const auto newRot = widgets::EulerDegToQuat({ rot[0], rot[1], rot[2] });
            for (auto* g : gos) g->transform.rotation = newRot;
        }
        track(rotChanged, "Rotate (Multi)");
    }

    float scale[3] = { refT.scale.x, refT.scale.y, refT.scale.z };
    if (!scaleAllSame) ImGui::PushStyleColor(ImGuiCol_Text, mutedColor);
    const bool scaleChanged =
        widgets::DragAxes(scaleAllSame ? "Scale" : "Scale (---)", scale, 3, 0.01f, 0.001f, 1000.0f);
    if (!scaleAllSame) ImGui::PopStyleColor();
    if (scaleChanged)
        for (auto* g : gos) g->transform.scale = { scale[0], scale[1], scale[2] };
    track(scaleChanged, "Scale (Multi)");
}

// 「全員に相対的な差分を加える」モード。
// WHY: Set モードでは「全員を +Y に 2m」ができない (全員が同じ Y に揃ってしまう)。
//      その場適用ではなく [Apply] ボタン式にしているのは、ドラッグ中に毎フレーム
//      加算されて発散するのを避けるためと、Undo を 1 操作 = 1 コマンドに保つため。
void DrawTransformOffsetMode(EditorContext& ctx, const std::vector<scene::GameObject*>& gos)
{
    static float moveDelta[3]  = { 0.0f, 0.0f, 0.0f };
    static float rotDelta[3]   = { 0.0f, 0.0f, 0.0f };
    static float scaleMul[3]   = { 1.0f, 1.0f, 1.0f };

    ImGui::TextDisabled("Applies a relative change to every selected object.");
    ImGui::Spacing();

    auto applyBulk = [&](const char* description, auto mutate) {
        const std::vector<std::string>      guids  = GuidsOf(gos);
        const std::vector<scene::Transform> before = TransformsOf(gos);
        for (auto* g : gos) mutate(g->transform);
        PushBulkTransformUndo(ctx, guids, before, TransformsOf(gos), description);
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    };

    widgets::DragAxes("Move by", moveDelta, 3, 0.1f);
    ImGui::SameLine();
    if (ImGui::Button("Apply##move")) {
        const math::Vector3 d{ moveDelta[0], moveDelta[1], moveDelta[2] };
        applyBulk("Move by Offset (Multi)", [d](scene::Transform& t) { t.position = t.position + d; });
        moveDelta[0] = moveDelta[1] = moveDelta[2] = 0.0f;
    }

    widgets::DragAxes("Rotate by", rotDelta, 3, 0.5f, 0.0f, 0.0f, "%.1f");
    ImGui::SameLine();
    if (ImGui::Button("Apply##rot")) {
        // WHY: ワールド軸まわりの回転を左から掛けることで、各オブジェクトの現在の向きに
        //      関係なく「同じ方向へ同じ角度だけ回す」になる。
        const math::Quaternion delta =
            widgets::EulerDegToQuat({ rotDelta[0], rotDelta[1], rotDelta[2] });
        applyBulk("Rotate by Offset (Multi)", [delta](scene::Transform& t) {
            t.rotation = (delta * t.rotation).Normalized();
        });
        rotDelta[0] = rotDelta[1] = rotDelta[2] = 0.0f;
    }

    widgets::DragAxes("Scale x", scaleMul, 3, 0.01f, 0.001f, 1000.0f);
    ImGui::SameLine();
    if (ImGui::Button("Apply##scale")) {
        const math::Vector3 m{ scaleMul[0], scaleMul[1], scaleMul[2] };
        applyBulk("Scale by Factor (Multi)", [m](scene::Transform& t) {
            t.scale = { t.scale.x * m.x, t.scale.y * m.y, t.scale.z * m.z };
        });
        scaleMul[0] = scaleMul[1] = scaleMul[2] = 1.0f;
    }
}

// ── 共通コンポーネントの一括編集 ──────────────────────────────────────────────

// T を全員が持っているか。
template<typename T>
bool AllHaveComponent(const std::vector<scene::GameObject*>& gos)
{
    for (auto* g : gos)
        if (!g->GetComponent<T>()) return false;
    return true;
}

// 共通コンポーネント 1 種類ぶんのセクションを描く。
// プライマリのコンポーネントを Reflect() で描画し、変更されたフィールドだけを
// 他の選択オブジェクトの同名フィールドへ配る。
template<typename T, typename Registration>
void DrawSharedComponentSection(EditorContext& ctx,
                                const std::vector<scene::GameObject*>& gos,
                                const char* label)
{
    // レジストリが既に「Reflect() を持つか」を判定しているので、それをそのまま使う。
    constexpr bool hasReflect = Registration::hasReflect;
    constexpr bool copyable   = std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>;

    ImGui::PushID(label);

    if constexpr (!hasReflect || !copyable) {
        // Reflect() を持たない / コピーできない型 (MeshCollider 等) は一括編集できない。
        // WHY: 前者はフィールドを機械的に辿れず、後者は Undo 用のスナップショットが取れない。
        //      黙って出さないと「なぜ編集できないのか」が分からないため、理由を明示する。
        ImGui::BulletText("%s", label);
        ImGui::SameLine();
        ImGui::TextDisabled("(select a single object to edit)");
        ImGui::PopID();
        return;
    } else {
        // 単体 Inspector と同じカード表現に揃える。複数選択でも「どの系統の
        // コンポーネントを触っているか」を帯の色で拾えるようにするため。
        const ImU32 accent = ComponentAccent<T>();
        const widgets::ComponentHeaderResult header =
            widgets::ComponentHeader(label, accent, nullptr, false);
        if (!header.open) {
            ImGui::Spacing();
            ImGui::PopID();
            return;
        }

        auto* primary = gos.front()->GetComponent<T>();
        if (!primary) {
            ImGui::Spacing();
            ImGui::PopID();
            return;
        }

        const widgets::ComponentBodyScope body = widgets::BeginComponentBody(header, accent);
        ImGui::Spacing();

        // ドラッグ 1 回を 1 コマンドにまとめるための編集状態 (型ごとに別インスタンス)。
        struct BulkEdit {
            std::vector<std::string> guids;
            std::vector<T>           before;
            ImGuiID                  activeId = 0;
            bool                     active   = false;
        };
        static BulkEdit edit;

        const bool    canUndo      = CanRecordEditorUndo(ctx) && ctx.activeScene;
        const ImGuiID activeBefore = ImGui::GetActiveID();

        MultiEditReflector reflector;
        reflector.m_projectRoot = ctx.projectRoot;
        primary->Reflect(reflector);

        const ImGuiID activeAfter = ImGui::GetActiveID();

        // 編集の開始を検出したら、選択全員のコンポーネントを before として保存する。
        if (canUndo && !edit.active && activeAfter != 0 && activeAfter != activeBefore) {
            edit.guids.clear();
            edit.before.clear();
            for (auto* g : gos) {
                if (auto* c = g->GetComponent<T>()) {
                    edit.guids.push_back(g->instanceId);
                    edit.before.push_back(*c);
                }
            }
            edit.activeId = activeAfter;
            edit.active   = true;
        }

        // 変更されたフィールドだけを他の選択オブジェクトへ配る。
        if (!reflector.changes.empty()) {
            ApplyFieldReflector applier;
            for (const auto& change : reflector.changes) {
                applier.change = &change;
                for (std::size_t i = 1; i < gos.size(); ++i)
                    if (auto* c = gos[i]->GetComponent<T>())
                        c->Reflect(applier);
            }
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }

        // 編集の終了 (ウィジェットが非アクティブになった) で 1 コマンドを積む。
        // WHY: 編集途中に Play へ入る等で記録が止まる場合があるため、積む直前にも確認する。
        const bool committed =
            edit.active && ctx.undoStack && ctx.undoStack->IsRecordingEnabled() &&
            (activeAfter == 0 || activeAfter != edit.activeId);
        if (committed) {
            std::vector<T> after;
            after.reserve(edit.guids.size());
            scene::Scene* scene = ctx.activeScene;
            for (const std::string& guid : edit.guids) {
                auto* target = scene->FindByGuid(guid);
                auto* c      = target ? target->GetComponent<T>() : nullptr;
                after.push_back(c ? *c : edit.before[after.size()]);
            }

            const std::vector<std::string> guids  = edit.guids;
            const std::vector<T>           before = edit.before;
            const auto                     markDirty = ctx.markSceneDirty;
            auto applyAll = [scene, guids, markDirty](const std::vector<T>& values) {
                for (std::size_t i = 0; i < guids.size() && i < values.size(); ++i)
                    if (auto* target = scene->FindByGuid(guids[i]))
                        if (auto* c = target->GetComponent<T>())
                            *c = values[i];
                if (markDirty) markDirty();
            };
            ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                std::string("Edit ") + label + " (Multi)",
                [applyAll, after]()  { applyAll(after); },
                [applyAll, before]() { applyAll(before); }));

            edit.active = false;
        } else if (edit.active && (activeAfter == 0 || activeAfter != edit.activeId)) {
            // Undo を積めない状況 (Play 中など) でも、編集状態は必ず閉じる。
            edit.active = false;
        }

        ImGui::Spacing();
        if (ImGui::SmallButton("Remove from all selected")) {
            // 全員から取り除き、1 回の Undo で全員ぶん戻す。
            std::vector<std::string> guids;
            std::vector<T>           removed;
            for (auto* g : gos) {
                if (auto* c = g->GetComponent<T>()) {
                    guids.push_back(g->instanceId);
                    // やり直し用のコピーからは GPU ハンドルを消す (実体は下で返される)。
                    removed.push_back(*c);
                    scene::ClearComponentGpuHandles(removed.back());
                    g->RemoveComponent<T>();
                }
            }
            if (canUndo && !guids.empty()) {
                scene::Scene* scene = ctx.activeScene;
                const auto markDirty = ctx.markSceneDirty;
                ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                    std::string("Remove ") + label + " (Multi)",
                    [scene, guids, markDirty]() {
                        for (const std::string& guid : guids)
                            if (auto* target = scene->FindByGuid(guid))
                                if (target->GetComponent<T>())
                                    target->RemoveComponent<T>();
                        if (markDirty) markDirty();
                    },
                    [scene, guids, removed, markDirty]() {
                        for (std::size_t i = 0; i < guids.size(); ++i)
                            if (auto* target = scene->FindByGuid(guids[i]))
                                if (!target->GetComponent<T>())
                                    target->AddComponent<T>(removed[i]);
                        if (markDirty) markDirty();
                    }));
            }
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
        ImGui::Spacing();
        widgets::EndComponentBody(body);
        ImGui::Spacing();
    }

    ImGui::PopID();
}

} // namespace

void DrawMultiSelectInspector(EditorContext& ctx, const std::vector<scene::EntityID>& ids)
{
    if (!ctx.activeScene) return;

    std::vector<scene::GameObject*> gos;
    gos.reserve(ids.size());
    for (auto id : ids)
        if (auto* g = ctx.activeScene->GetGameObject(id))
            gos.push_back(g);
    if (gos.empty()) return;

    ImGui::TextColored({ 0.7f, 0.85f, 1.0f, 1.0f }, "%zu objects selected", gos.size());
    ImGui::Separator();
    ImGui::Spacing();

    // ── Transform ────────────────────────────────────────────────────────────
    const widgets::ComponentHeaderResult transformHeader =
        widgets::ComponentHeader("Transform", EditorTheme::ColorU32(ThemeColor::Accent), nullptr);
    if (transformHeader.open) {
        const widgets::ComponentBodyScope transformBody =
            widgets::BeginComponentBody(transformHeader, EditorTheme::ColorU32(ThemeColor::Accent));
        ImGui::Spacing();

        static TransformEditMode mode = TransformEditMode::Set;
        const auto modeButton = [&](const char* label, TransformEditMode value, const char* tip) {
            const bool active = (mode == value);
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(label)) mode = value;
            if (active) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        modeButton("Set", TransformEditMode::Set,
                   "Assign the same value to every selected object");
        ImGui::SameLine();
        modeButton("Offset", TransformEditMode::Offset,
                   "Apply a relative change, keeping each object's own value");
        ImGui::Spacing();

        if (mode == TransformEditMode::Set) DrawTransformSetMode(ctx, gos);
        else                                DrawTransformOffsetMode(ctx, gos);

        ImGui::Spacing();
        widgets::EndComponentBody(transformBody);
    }
    ImGui::Spacing();

    // ── 共通コンポーネント ────────────────────────────────────────────────────
    // WHY: 以前は 8 種類を決め打ちで BulletText していたため、コンポーネントを追加するたび
    //      ここへ書き足す必要があり、実際に漏れていた。レジストリから列挙して自動追従させる。
    ImGui::Spacing();
    ImGui::TextDisabled("Shared Components");
    ImGui::Separator();

    bool anyShared = false;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        // Hidden (Internal) は単体 Inspector にも出ないので、こちらでも出さない。
        if constexpr (Registration::inspectorMode == scene::ComponentInspectorMode::Hidden)
            return;
        else {
            if (!AllHaveComponent<T>(gos)) return;
            anyShared = true;
            DrawSharedComponentSection<T, Registration>(ctx, gos, Registration::displayName);
        }
    });

    if (!anyShared)
        ImGui::TextDisabled("The selected objects have no component in common.");

    // ── 選択全体へコンポーネントを追加 ────────────────────────────────────────
    // WHY: 「選んだ 20 個全部に AudioSource を足す」はレベル調整で普通に出る操作。
    //      既に持っている対象は飛ばし、追加ぶんは 1 回の Undo でまとめて戻る。
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    {
        static char filterBuffer[64] = {};
        char label[64];
        std::snprintf(label, sizeof(label), "Add Component to %zu objects", gos.size());
        DrawAddComponentMenuMulti(gos, filterBuffer, ctx, label);
    }
}

} // namespace fbzz::editor
