/// @file    FluidDocument.cpp
/// @brief   Fluid Editor が編集している .fluid の持ち主 (Undo のまとめ方・保存・外からの書き換えの検知)
/// @author  Hasegawa Jin
/// @date    2026-09-12

#include <Editor/Util/FluidDocument.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FluidRecipe.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {

// ディスクの stat は 1 秒に 2 回で足りる (AI・Baker の書き込みは人の操作の速さでしか来ない)。
constexpr std::chrono::milliseconds kPollInterval{ 500 };

std::int64_t DiskStamp(const std::string& path)
{
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(util::FileSystem::PathFromUtf8(path), error);
    return error ? 0 : static_cast<std::int64_t>(stamp.time_since_epoch().count());
}

// 同じファイルを «C:\a\b.fluid» と «c:/a/b.fluid» で持ち合っても 1 つに寄せる (Windows は大文字小文字を区別しない)。
std::string RegistryKey(const std::string& path)
{
    std::string key = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(path).lexically_normal());
    for (char& c : key) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return key;
}

// Undo / Redo が «今それを開いている文書» を探すための登録簿。
// WHY ポインタを Undo コマンドに持たせないか: 文書は閉じられ・開き直されるので、履歴に残った
//     ポインタはすぐ宙に浮く。パスで引き直せば、閉じていればファイルへ書く経路に自然に落ちる。
std::unordered_map<std::string, FluidDocument*>& Registry()
{
    static std::unordered_map<std::string, FluidDocument*> registry;
    return registry;
}

std::unordered_map<const FluidDocument*, std::chrono::steady_clock::time_point>& LastPolls()
{
    static std::unordered_map<const FluidDocument*, std::chrono::steady_clock::time_point> polls;
    return polls;
}

void Unregister(const FluidDocument* doc)
{
    auto& registry = Registry();
    for (auto it = registry.begin(); it != registry.end();) {
        if (it->second == doc) it = registry.erase(it);
        else ++it;
    }
    LastPolls().erase(doc);
}

FluidDocument* FindOpenDocument(const std::string& path)
{
    const auto& registry = Registry();
    const auto it = registry.find(RegistryKey(path));
    return it != registry.end() ? it->second : nullptr;
}

// Reflect() を通した値をすべて 1 本のバイト列へ落とす。
// WHY 手書きの項目比較にしないか: 項目が増えたときに比較へ足し忘れると «変えたのに Undo が積まれない»
//     になる。ReflectFluidRecipe は TOML のキーと一致することを FluidRecipeBakeTests が縛っているので、
//     これに乗れば «保存される値» と «比べる値» がずれない。kind で隠れる項目も訪問はされる (表示だけ切る)。
// WHY float をビットで持つか: to_string の 6 桁だと、細かいドラッグの差が «変わっていない» に丸まる。
class RecipeDigest final : public scene::IReflector {
public:
    [[nodiscard]] const std::string& Result() const { return m_out; }

    void Field(const char*, float& v) override { Bytes(&v, sizeof(v)); }
    void Field(const char*, int& v) override { Bytes(&v, sizeof(v)); }
    void Field(const char*, bool& v) override { m_out += v ? '1' : '0'; }
    void Field(const char*, math::Vector2& v) override { Floats({ v.x, v.y }); }
    void Field(const char*, math::Vector3& v) override { Floats({ v.x, v.y, v.z }); }
    void Field(const char*, math::Vector4& v) override { Floats({ v.x, v.y, v.z, v.w }); }
    void Field(const char*, math::Quaternion& v) override { Floats({ v.x, v.y, v.z, v.w }); }
    void Field(const char*, std::string& v) override
    {
        // 長さを先に書く。書かないと "ab"+"c" と "a"+"bc" が同じ列になる。
        const std::size_t length = v.size();
        Bytes(&length, sizeof(length));
        m_out += v;
    }
    void BeginObject(const char*) override { m_out += '{'; }
    void EndObject() override { m_out += '}'; }
    std::size_t BeginObjectList(const char*, std::size_t count) override
    {
        m_out += '[';
        Bytes(&count, sizeof(count));
        return count;
    }
    void BeginObjectElement(std::size_t) override { m_out += '('; }
    void EndObjectElement() override { m_out += ')'; }
    std::size_t EndObjectList() override
    {
        m_out += ']';
        return NO_REMOVE;
    }

private:
    void Bytes(const void* data, std::size_t size) { m_out.append(static_cast<const char*>(data), size); }
    void Floats(std::initializer_list<float> values)
    {
        for (const float x : values) Bytes(&x, sizeof(x));
    }
    std::string m_out;
};

std::string Digest(const asset::FluidRecipe& recipe)
{
    // ReflectFluidRecipe は非 const を取る (上限を超えた部品を切る等)。比べるだけで元を変えないよう写しに通す。
    asset::FluidRecipe copy = recipe;
    RecipeDigest digest;
    asset::ReflectFluidRecipe(copy, digest);
    return digest.Result();
}

bool RecipesEqual(const asset::FluidRecipe& a, const asset::FluidRecipe& b)
{
    if (a.sources.size() != b.sources.size() || a.forces.size() != b.forces.size()
        || a.colliders.size() != b.colliders.size())
        return false;
    return Digest(a) == Digest(b);
}

int ListCount(const asset::FluidRecipe& recipe, FluidSelectionKind list)
{
    switch (list) {
    case FluidSelectionKind::Source: return static_cast<int>(recipe.sources.size());
    case FluidSelectionKind::Force: return static_cast<int>(recipe.forces.size());
    case FluidSelectionKind::Collider: return static_cast<int>(recipe.colliders.size());
    default: return 0;
    }
}

// Undo で部品が減ると選択の添字が範囲外になる。Outliner / Properties が範囲外を引かないよう寄せる。
void ClampSelection(FluidSelection& selection, const asset::FluidRecipe& recipe)
{
    if (!selection.IsPart()) return;
    const int count = ListCount(recipe, selection.kind);
    if (count <= 0 || selection.index < 0) {
        selection = FluidSelection{};
        return;
    }
    selection.index = (std::min)(selection.index, count - 1);
}

std::pair<int, int> PartKey(FluidSelectionKind list, int index)
{
    return { static_cast<int>(list), index };
}

const char* LabelOr(const char* label)
{
    return (label != nullptr && label[0] != '\0') ? label : "Edit Fluid";
}

} // namespace

FluidDocument::~FluidDocument()
{
    Unregister(this);
}

bool FluidDocument::Open(const std::string& absPath, std::string& outError)
{
    Close();
    asset::FluidRecipe recipe;
    if (!asset::LoadFluidRecipe(absPath, recipe, &outError)) {
        if (outError.empty()) outError = "読み込めませんでした: " + absPath;
        return false;
    }
    m_path = absPath;
    m_recipe = std::move(recipe);
    m_diskStamp = DiskStamp(absPath);
    // 通番は 0 に戻さない。別の文書へ開き直したときにプレビューのキャッシュが同じ鍵で当たらないように。
    ++m_revision;
    m_open = true;
    m_dirty = false;
    m_conflict = false;
    m_interactive = false;
    m_commitActive = false;
    m_hidden.clear();
    m_solo = { -1, -1 };
    selection = FluidSelection{};
    Registry()[RegistryKey(m_path)] = this;
    LastPolls().erase(this);
    return true;
}

void FluidDocument::Close()
{
    Unregister(this);
    if (m_open) ++m_revision;
    m_open = false;
    m_path.clear();
    m_recipe = asset::FluidRecipe{};
    m_interactiveBefore = asset::FluidRecipe{};
    m_commitBefore = asset::FluidRecipe{};
    m_commitLabel.clear();
    m_diskStamp = 0;
    m_dirty = false;
    m_conflict = false;
    m_interactive = false;
    m_commitActive = false;
    m_hidden.clear();
    m_solo = { -1, -1 };
    selection = FluidSelection{};
}

bool FluidDocument::Edit(EditorContext& ctx, const char* label, const std::function<void(asset::FluidRecipe&)>& fn)
{
    if (!m_open || !fn) return false;

    // 操作中のまとまりが残っていたら先に閉じる。閉じないと、まとまりの «前» がこの編集より前になり、
    // まとまりを Undo するとこの編集まで一緒に戻る。
    if (m_commitActive) {
        m_commitActive = false;
        if (!RecipesEqual(m_commitBefore, m_recipe))
            PushUndo(ctx, m_commitLabel.c_str(), m_commitBefore, m_recipe);
    }

    asset::FluidRecipe after = m_recipe;
    fn(after);
    if (RecipesEqual(m_recipe, after)) {
        ClampSelection(selection, m_recipe);
        return false;
    }
    asset::FluidRecipe before = std::move(m_recipe);
    m_recipe = std::move(after);
    ++m_revision;
    m_dirty = true;
    ClampSelection(selection, m_recipe);
    PushUndo(ctx, LabelOr(label), before, m_recipe);
    return true;
}

void FluidDocument::Commit(EditorContext& ctx, const char* label, const asset::FluidRecipe& working, bool changed)
{
    if (!m_open) {
        m_commitActive = false;
        return;
    }
    // ImGui の文脈が無い (テスト・起動直後) ときは «操作中ではない» として扱う。
    const bool active = ImGui::GetCurrentContext() != nullptr && ImGui::IsAnyItemActive();

    if (changed && active) {
        if (!m_commitActive) {
            m_commitActive = true;
            m_commitBefore = m_recipe;
            m_commitLabel = LabelOr(label);
        }
        m_recipe = working;
        ++m_revision;
        m_dirty = true;
        ClampSelection(selection, m_recipe);
        return;
    }

    if (changed) {
        // 手を離したフレームにも値が来る (InputText の確定など)。まとまりがあれば同じ 1 つに入れる。
        const asset::FluidRecipe before = m_commitActive ? m_commitBefore : m_recipe;
        const std::string groupLabel = m_commitActive ? m_commitLabel : std::string(LabelOr(label));
        m_commitActive = false;
        const bool differsFromCurrent = !RecipesEqual(m_recipe, working);
        if (differsFromCurrent) {
            m_recipe = working;
            ++m_revision;
            m_dirty = true;
            ClampSelection(selection, m_recipe);
        }
        if (!RecipesEqual(before, m_recipe)) PushUndo(ctx, groupLabel.c_str(), before, m_recipe);
        return;
    }

    if (m_commitActive && !active) {
        m_commitActive = false;
        if (!RecipesEqual(m_commitBefore, m_recipe))
            PushUndo(ctx, m_commitLabel.c_str(), m_commitBefore, m_recipe);
    }
}

void FluidDocument::BeginInteractiveEdit()
{
    if (!m_open || m_interactive) return;
    m_interactiveBefore = m_recipe;
    m_interactive = true;
}

void FluidDocument::ApplyInteractive(const asset::FluidRecipe& working)
{
    if (!m_open) return;
    // Begin を呼び忘れても操作前を失わないよう、最初の Apply で覚える。
    if (!m_interactive) BeginInteractiveEdit();
    m_recipe = working;
    ++m_revision;
    m_dirty = true;
    ClampSelection(selection, m_recipe);
}

void FluidDocument::EndInteractiveEdit(EditorContext& ctx, const char* label)
{
    if (!m_interactive) return;
    m_interactive = false;
    if (!m_open) return;
    if (!RecipesEqual(m_interactiveBefore, m_recipe))
        PushUndo(ctx, LabelOr(label), m_interactiveBefore, m_recipe);
}

bool FluidDocument::Save(EditorContext& ctx, std::string& outError)
{
    if (!m_open) {
        outError = "開いている .fluid がありません";
        return false;
    }
    // 書く直前にもう一度ディスクを見る。PollExternalChange は間を置いてしか見ないので、その隙に
    // 外 (AI の fluid.set・Baker パネル) が書いていると、気づかないまま上書きしてしまう。
    // 自動保存もここを通るので、«3 秒の無操作» が他人の書き込みを飲み込むことはない。
    if (const std::int64_t stamp = DiskStamp(m_path); stamp != 0 && m_diskStamp != 0 && stamp != m_diskStamp) {
        m_conflict = true;
        outError = "外で書き換えられています。読み直すか手元を残すかを選んでください: " + m_path;
        return false;
    }
    if (!asset::SaveFluidRecipe(m_path, m_recipe)) {
        outError = "保存できませんでした: " + m_path;
        return false;
    }
    m_dirty = false;
    m_conflict = false;
    // 自分の書き込みを «外からの変更» と取り違えないよう、書いた直後の時刻を覚え直す。
    m_diskStamp = DiskStamp(m_path);
    LastPolls().erase(this);
    (void)asset::AssetManager::ReloadPath(m_path);
    ctx.requestAssetBrowserRefresh = true;
    return true;
}

void FluidDocument::PollExternalChange()
{
    if (!m_open) return;
    const auto now = std::chrono::steady_clock::now();
    auto& polls = LastPolls();
    if (const auto it = polls.find(this); it != polls.end() && now - it->second < kPollInterval) return;
    polls[this] = now;

    const std::int64_t stamp = DiskStamp(m_path);
    // 0 は消えた・掴めない。書き込み途中のことがあるので、手元はそのまま次の問い合わせを待つ。
    if (stamp == 0 || stamp == m_diskStamp) return;
    if (m_dirty) {
        // m_diskStamp は進めない。KeepLocal / ReloadFromDisk で決着するまで衝突を出し続ける。
        m_conflict = true;
        return;
    }
    asset::FluidRecipe recipe;
    // 読めなければ書き込み途中とみなし、時刻を進めずに次で読み直す。
    if (!asset::LoadFluidRecipe(m_path, recipe)) return;
    ReplaceRecipe(recipe, false);
    m_diskStamp = stamp;
    m_conflict = false;
}

bool FluidDocument::HasUnsavedChanges(const std::string& absPath)
{
    const auto& registry = Registry();
    const auto it = registry.find(RegistryKey(absPath));
    return it != registry.end() && it->second != nullptr && it->second->IsDirty();
}

void FluidDocument::ReloadFromDisk()
{
    if (!m_open) return;
    asset::FluidRecipe recipe;
    if (!asset::LoadFluidRecipe(m_path, recipe)) return;
    ReplaceRecipe(recipe, false);
    m_diskStamp = DiskStamp(m_path);
    m_conflict = false;
    LastPolls().erase(this);
}

void FluidDocument::KeepLocal()
{
    if (!m_open) return;
    // 今のディスクの時刻を «見た» ことにする。次に外から書かれるまで衝突は出ない。
    m_diskStamp = DiskStamp(m_path);
    m_conflict = false;
    m_dirty = true;
    LastPolls().erase(this);
}

bool FluidDocument::IsHidden(FluidSelectionKind list, int index) const
{
    return m_hidden.count(PartKey(list, index)) != 0;
}

void FluidDocument::SetHidden(FluidSelectionKind list, int index, bool hidden)
{
    if (hidden) m_hidden.insert(PartKey(list, index));
    else m_hidden.erase(PartKey(list, index));
}

void FluidDocument::ToggleSolo(FluidSelectionKind list, int index)
{
    const std::pair<int, int> key = PartKey(list, index);
    m_solo = (m_solo == key) ? std::pair<int, int>{ -1, -1 } : key;
}

bool FluidDocument::IsSolo(FluidSelectionKind list, int index) const
{
    return m_solo.first >= 0 && m_solo == PartKey(list, index);
}

void FluidDocument::RemapVisibility(FluidSelectionKind list, const std::function<int(int)>& map)
{
    if (!map) return;
    const int id = static_cast<int>(list);
    std::set<std::pair<int, int>> next;
    for (const std::pair<int, int>& key : m_hidden) {
        if (key.first != id) {
            next.insert(key);
            continue;
        }
        const int moved = map(key.second);
        if (moved >= 0) next.insert(PartKey(list, moved));
    }
    m_hidden = std::move(next);

    if (m_solo.first != id || m_solo.second < 0) return;
    const int solo = map(m_solo.second);
    m_solo = solo >= 0 ? PartKey(list, solo) : std::pair<int, int>{ -1, -1 };
}

void FluidDocument::RemapVisibilityAfterInsert(FluidSelectionKind list, int insertedIndex)
{
    // 挿し込んだ部品そのものは «印なし» で始まる (挿した場所以降の印だけを 1 つ後ろへ送る)。
    RemapVisibility(list, [insertedIndex](int index) { return index >= insertedIndex ? index + 1 : index; });
}

void FluidDocument::RemapVisibilityAfterRemove(FluidSelectionKind list, int removedIndex)
{
    RemapVisibility(list, [removedIndex](int index) {
        if (index == removedIndex) return -1;
        return index > removedIndex ? index - 1 : index;
    });
}

void FluidDocument::RemapVisibilityAfterMove(FluidSelectionKind list, int from, int to)
{
    if (from == to) return;
    // FixSelectionAfterMove (FluidEditorCommon) と同じ写し方。間に挟まれた部品が 1 つずつずれる。
    RemapVisibility(list, [from, to](int index) {
        if (index == from) return to;
        if (from < index && index <= to) return index - 1;
        if (to <= index && index < from) return index + 1;
        return index;
    });
}

asset::FluidRecipe FluidDocument::PreviewRecipe() const
{
    asset::FluidRecipe preview = m_recipe;
    const auto apply = [this](auto& parts, FluidSelectionKind list) {
        const int count = static_cast<int>(parts.size());
        // 消えた部品を指したままのソロは無視する (リストが丸ごと消えて見えるより分かりやすい)。
        const bool soloHere = m_solo.first == static_cast<int>(list) && m_solo.second >= 0 && m_solo.second < count;
        for (int i = 0; i < count; ++i) {
            if (IsHidden(list, i) || (soloHere && i != m_solo.second))
                parts[static_cast<std::size_t>(i)].enabled = false;
        }
    };
    apply(preview.sources, FluidSelectionKind::Source);
    apply(preview.forces, FluidSelectionKind::Force);
    apply(preview.colliders, FluidSelectionKind::Collider);
    return preview;
}

void FluidDocument::ReplaceRecipe(const asset::FluidRecipe& recipe, bool markDirty)
{
    m_recipe = recipe;
    ++m_revision;
    m_dirty = markDirty;
    // 操作の途中で中身が差し替わったら、そのまとまりは捨てる (差し替え前を «前» にして積むと別物に戻る)。
    m_commitActive = false;
    m_interactive = false;
    ClampSelection(selection, m_recipe);
}

void FluidDocument::PushUndo(EditorContext& ctx, const char* label, const asset::FluidRecipe& before,
                             const asset::FluidRecipe& after)
{
    if (ctx.undoStack == nullptr) return;

    EditorContext* context = &ctx;
    const std::string path = m_path;
    // メンバー関数の中のラムダなので private の ReplaceRecipe を呼べる。
    const auto applySnapshot = [context, path](const asset::FluidRecipe& snapshot) {
        if (FluidDocument* doc = FindOpenDocument(path)) {
            doc->ReplaceRecipe(snapshot, true);
            return;
        }
        if (!asset::SaveFluidRecipe(path, snapshot)) return;
        (void)asset::AssetManager::ReloadPath(path);
        context->requestAssetBrowserRefresh = true;
    };

    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        std::string(LabelOr(label)),
        [applySnapshot, after]() { applySnapshot(after); },
        [applySnapshot, before]() { applySnapshot(before); }));
}

} // namespace fbzz::editor
