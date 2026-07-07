// FBZZ Engine
// ImGuiWidgets.cpp | fbzz::editor
// プロジェクト固有の ImGui カスタムウィジェット実装
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Math/Vector3.hpp>
#include <imgui.h>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::editor::widgets {

// ── AssetPathField / DrawAssetPickerModal ─────────────────────────────────

namespace {

struct AssetPickerState {
    bool                     open             = false;
    std::string*             target           = nullptr;
    std::string*             justPickedTarget = nullptr;
    std::string              projectRoot;
    std::vector<std::string> filterExts;
    char                     search[256]      = {};
    std::vector<std::string> files;
    ImVec2                   anchorPos        = {};  // "..." ボタンの直下位置
};
AssetPickerState s_picker;

// ピッカー行のサムネイル。texId があれば画像を、無ければ color スウォッチを描く。
struct PickerThumb {
    void*  texId = nullptr;              // 画像 / マテリアルのアルベドテクスチャ
    ImVec4 color = { 0.0f, 0.0f, 0.0f, 0.0f }; // マテリアルのアルベド色 (テクスチャ無し時, a>0 で有効)
};
// パス → サムネイル。ピッカーを開くたびにクリアして最新の見た目を反映する。
std::unordered_map<std::string, PickerThumb> s_thumbCache;

// このプロジェクトの ImGui は ImTextureID を ImU64 として扱うため、void* を数値経由で変換する。
ImTextureID ToImTextureID(void* ptr)
{
    return static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(ptr));
}

bool IsImageExt(const std::string& ext)
{
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" ||
           ext == ".dds" || ext == ".bmp" || ext == ".hdr"  || ext == ".exr";
}

// 画像 / マテリアルのサムネイルを解決してキャッシュする (GPU ロードは ResourceManager がキャッシュ)。
const PickerThumb& ResolveThumb(const std::string& absPath, const std::string& rel,
                                const std::string& ext,
                                renderer::ResourceManager* res,
                                renderer::IImGuiRenderer* imgui)
{
    if (auto it = s_thumbCache.find(absPath); it != s_thumbCache.end())
        return it->second;

    PickerThumb t;
    if (res && imgui) {
        if (IsImageExt(ext)) {
            const auto h = res->LoadTexture(absPath);
            if (h.IsValid()) t.texId = imgui->GetImTextureID(h, *res);
        } else if (ext == ".mat") {
            const auto mh = asset::AssetManager::LoadMaterial(rel);
            if (const asset::MaterialAsset* m = asset::AssetManager::GetMaterial(mh)) {
                // アルベドテクスチャがあればそれを、無ければアルベド色をスウォッチにする。
                if (auto tx = m->textures.find("albedo");
                    tx != m->textures.end() && !tx->second.empty()) {
                    const auto h = res->LoadTexture(
                        asset::AssetManager::ResolveAssetPath(tx->second));
                    if (h.IsValid()) t.texId = imgui->GetImTextureID(h, *res);
                }
                if (!t.texId) {
                    if (auto pa = m->params.find("albedo");
                        pa != m->params.end() && pa->second.size() >= 3)
                        t.color = { pa->second[0], pa->second[1], pa->second[2], 1.0f };
                }
            }
        }
    }
    return s_thumbCache.emplace(absPath, t).first->second;
}

// 拡張子 → バッジ色
ImVec4 ExtBadgeColor(const std::string& ext)
{
    if (ext == ".mat")                                          return { 0.75f, 0.4f,  0.9f,  1.0f }; // purple
    if (ext == ".asset" || ext == ".fzasset")                   return { 0.3f,  0.85f, 0.9f,  1.0f }; // cyan
    if (ext == ".scene")                                           return { 0.45f, 0.65f, 1.0f,  1.0f }; // blue
    if (ext == ".hlsl")                                           return { 1.0f,  0.9f,  0.3f,  1.0f }; // yellow
    if (ext == ".animcontroller" || ext == ".anim")         return { 1.0f,  0.6f,  0.2f,  1.0f }; // orange
    if (ext == ".skel")                                         return { 1.0f,  0.7f,  0.7f,  1.0f }; // pink
    if (ext == ".png"  || ext == ".jpg" || ext == ".jpeg" ||
        ext == ".tga"  || ext == ".dds" || ext == ".bmp"  ||
        ext == ".hdr"  || ext == ".exr")                          return { 0.4f,  0.9f,  0.5f,  1.0f }; // green
    return { 0.6f, 0.6f, 0.6f, 1.0f }; // gray
}

std::vector<std::string> SplitFilterExts(const char* exts)
{
    std::vector<std::string> result;
    if (!exts || !exts[0]) return result;
    const std::string s = exts;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t comma = s.find(',', pos);
        const size_t end = (comma == std::string::npos) ? s.size() : comma;
        std::string ext = s.substr(pos, end - pos);
        while (!ext.empty() && ext.front() == ' ') ext.erase(ext.begin());
        while (!ext.empty() && ext.back()  == ' ') ext.pop_back();
        if (!ext.empty()) result.push_back(util::StringUtils::ToLower(ext));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return result;
}

void ScanProjectFiles(const std::string& projectRoot,
                      const std::vector<std::string>& exts,
                      std::vector<std::string>& out)
{
    out.clear();
    namespace fs = std::filesystem;
    const fs::path root = util::FileSystem::PathFromUtf8(projectRoot);
    if (!util::FileSystem::Exists(root)) return;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(
                root, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = util::StringUtils::ToLower(
                util::FileSystem::PathToUtf8(entry.path().extension()));
            if (!exts.empty()) {
                bool match = false;
                for (const auto& e : exts) if (e == ext) { match = true; break; }
                if (!match) continue;
            }
            out.push_back(util::FileSystem::PathToUtf8(entry.path()));
        }
    } catch (...) {}
    std::sort(out.begin(), out.end());
}

} // namespace

bool AcceptAssetPathDrop(std::string& outPath)
{
    bool dropped = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            outPath = NormalizeAssetPath(
                std::string(static_cast<const char*>(p->Data),
                            static_cast<size_t>(p->DataSize) - 1));
            dropped = true;
        }
        ImGui::EndDragDropTarget();
    }
    return dropped;
}

bool AssetPathField(const char* label, std::string& path,
                    const char* filterExts,
                    const std::string& projectRoot)
{
    ImGui::PushID(label);
    bool changed = false;

    // ピッカーがこのターゲットを選択した直後 → 同フレームで changed を通知
    if (s_picker.justPickedTarget == &path) {
        changed = true;
        s_picker.justPickedTarget = nullptr;
    }

    constexpr float kBtnW = 26.0f;
    const ImGuiStyle& style = ImGui::GetStyle();

    // InputText 幅を CalcItemWidth() ベースにすることで、
    // ラベルが ImGui 標準のラベル列（右側 ~35% 幅）に収まるようにする。
    // GetContentRegionAvail() を使うとラベルがウィンドウ外に押し出される。
    const float inputW = std::max(40.0f,
        ImGui::CalcItemWidth() - kBtnW - style.ItemSpacing.x);
    const float fieldLeft = ImGui::GetCursorScreenPos().x;  // ピッカー位置決め用
    ImGui::SetNextItemWidth(inputW);

    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", path.c_str());
    if (ImGui::InputText("##path", buf, sizeof(buf))) {
        path    = NormalizeAssetPath(buf);
        changed = true;
    }
    if (AcceptAssetPathDrop(path))
        changed = true;

    ImGui::SameLine(0.0f, style.ItemSpacing.x);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        { 3.0f, style.FramePadding.y });
    const bool browse = ImGui::Button("...", { kBtnW, 0.0f });
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Browse Assets...");
    if (browse) {
        s_picker.open        = true;
        s_picker.target      = &path;
        s_picker.projectRoot = projectRoot;
        s_picker.filterExts  = SplitFilterExts(filterExts);
        s_picker.search[0]   = '\0';
        s_picker.anchorPos   = { fieldLeft, ImGui::GetItemRectMax().y + 2.0f };
        ScanProjectFiles(projectRoot, s_picker.filterExts, s_picker.files);
    }

    // ラベルを ImGui 標準ラベル列（右側）に配置。ウィンドウ幅でクリップされる。
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);

    ImGui::PopID();
    return changed;
}

void OpenAssetPicker(std::string& target, const char* filterExts,
                     const std::string& projectRoot, ImVec2 anchorPos)
{
    s_picker.open        = true;
    s_picker.target      = &target;
    s_picker.projectRoot = projectRoot;
    s_picker.filterExts  = SplitFilterExts(filterExts);
    s_picker.search[0]   = '\0';
    s_picker.anchorPos   = anchorPos;
    ScanProjectFiles(projectRoot, s_picker.filterExts, s_picker.files);
}

void DrawAssetPickerModal(renderer::ResourceManager* resources,
                          renderer::IImGuiRenderer* imgui)
{
    if (s_picker.open) {
        ImGui::OpenPopup("##asset_picker");
        s_picker.open = false;
        // 開くたびにサムネイルを作り直し、直近のアセット編集を反映する。
        s_thumbCache.clear();
    }

    // ── パネル位置: フィールド左端の直下。画面端でクランプ ──────────────────
    constexpr float kW = 370.0f;
    constexpr float kH = 400.0f;
    const ImVec2 vpPos  = ImGui::GetMainViewport()->Pos;
    const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    ImVec2 pos = s_picker.anchorPos;
    pos.x = std::max(pos.x, vpPos.x + 4.0f);
    if (pos.x + kW > vpPos.x + vpSize.x - 4.0f)
        pos.x = vpPos.x + vpSize.x - kW - 4.0f;
    if (pos.y + kH > vpPos.y + vpSize.y - 4.0f)
        pos.y = s_picker.anchorPos.y - kH - ImGui::GetFrameHeightWithSpacing();
    pos.y = std::max(pos.y, vpPos.y + 4.0f);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ kW, kH }, ImGuiCond_Always);
    if (!ImGui::BeginPopup("##asset_picker",
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar))
        return;

    // ── 検索バー ──────────────────────────────────────────────────────────
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##search", "Search by name...",
                             s_picker.search, sizeof(s_picker.search));
    ImGui::Separator();

    // ── アセットリスト ─────────────────────────────────────────────────────
    ImGui::BeginChild("##list", { 0.0f, ImGui::GetContentRegionAvail().y }, false);

    // "(none)" — フィールドをクリアするオプション
    {
        const bool selNone = (s_picker.target && s_picker.target->empty());
        if (selNone) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImGui::GetCursorScreenPos(),
                { ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
                  ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeightWithSpacing() + 4.0f },
                IM_COL32(55, 95, 170, 120), 3.0f);
        }
        if (ImGui::Selectable("(none)", selNone)) {
            if (s_picker.target) {
                *s_picker.target          = {};
                s_picker.justPickedTarget = s_picker.target;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }

    const std::string searchStr = s_picker.search;
    const float rowH = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 6.0f;

    for (const auto& absPath : s_picker.files) {
        namespace fs = std::filesystem;
        const std::string fname = util::FileSystem::GetFilename(absPath);
        const std::string ext   = util::StringUtils::ToLower(
            util::FileSystem::GetExtension(absPath));
        const std::string stem  = util::FileSystem::PathToUtf8(
            util::FileSystem::PathFromUtf8(absPath).stem());

        // 名前検索フィルタ（stem でも fname でも通す）
        if (!searchStr.empty() &&
            !util::StringUtils::ContainsCI(stem,  searchStr) &&
            !util::StringUtils::ContainsCI(fname, searchStr))
            continue;

        const std::string rel      = NormalizeAssetPath(absPath);
        const bool        selected = (s_picker.target && *s_picker.target == rel);
        ImGui::PushID(rel.c_str());

        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float  rowW   = ImGui::GetContentRegionAvail().x;

        // 選択ハイライト背景
        if (selected) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                rowMin, { rowMin.x + rowW, rowMin.y + rowH },
                IM_COL32(55, 95, 170, 130), 3.0f);
        }

        // 左端のサムネイル枠 (Unity のオブジェクトピッカー風の可視化)。
        // 画像 → 実プレビュー、.mat → アルベドのテクスチャ/色、その他 → 拡張子バッジ色の四角。
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float thumbSz = rowH - 6.0f;
        const ImVec2 thumbMin = { rowMin.x + 4.0f, rowMin.y + 3.0f };
        const ImVec2 thumbMax = { thumbMin.x + thumbSz, thumbMin.y + thumbSz };
        const ImVec4 badgeCol = ExtBadgeColor(ext);
        const PickerThumb& thumb = ResolveThumb(absPath, rel, ext, resources, imgui);
        if (thumb.texId) {
            // 市松模様の下地 (アルファ付きテクスチャの視認性)
            dl->AddRectFilled(thumbMin, thumbMax, IM_COL32(40, 40, 40, 255), 3.0f);
            dl->AddImageRounded(ToImTextureID(thumb.texId),
                                thumbMin, thumbMax, { 0, 0 }, { 1, 1 },
                                IM_COL32_WHITE, 3.0f);
        } else if (thumb.color.w > 0.0f) {
            dl->AddRectFilled(thumbMin, thumbMax, ImGui::GetColorU32(thumb.color), 3.0f);
        } else {
            // 型アイコン: バッジ色の四角 + 拡張子頭2文字
            const ImU32 fill = ImGui::GetColorU32({ badgeCol.x, badgeCol.y, badgeCol.z, 0.28f });
            dl->AddRectFilled(thumbMin, thumbMax, fill, 3.0f);
            dl->AddRect(thumbMin, thumbMax, ImGui::GetColorU32(badgeCol), 3.0f);
        }
        dl->AddRect(thumbMin, thumbMax, IM_COL32(0, 0, 0, 90), 3.0f);

        const float textLeft = thumbMax.x + 8.0f;

        // バッジ（[EXT]）+ 名前
        std::string badge = ext.size() > 1 ? ext.substr(1) : ext;
        for (char& c : badge) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        ImGui::SetCursorScreenPos({ textLeft, rowMin.y + 2.0f });
        ImGui::TextColored(badgeCol, "[%s]", badge.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(stem.c_str());

        // 相対パス（薄い色）
        ImGui::SetCursorScreenPos({ textLeft + 2.0f,
            rowMin.y + ImGui::GetTextLineHeightWithSpacing() + 2.0f });
        ImGui::TextDisabled("%s", rel.c_str());

        // クリック判定（透明な Selectable を行全体に重ねる）
        ImGui::SetCursorScreenPos(rowMin);
        if (ImGui::Selectable("##row", selected,
                ImGuiSelectableFlags_AllowOverlap, { rowW, rowH })) {
            if (s_picker.target) {
                *s_picker.target          = rel;
                s_picker.justPickedTarget = s_picker.target;
            }
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", absPath.c_str());

        ImGui::PopID();
        ImGui::Dummy({ 0.0f, 2.0f });
    }

    ImGui::EndChild();
    ImGui::EndPopup();
}

// ── 既存ウィジェット ─────────────────────────────────────────────────────

bool DragVec3(const char* label, math::Vector3& v, float speed, float min, float max)
{
    ImGui::PushID(label);
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 100.0f);
    ImGui::Text("%s", label);
    ImGui::NextColumn();
    float arr[3] = { v.x, v.y, v.z };
    bool changed = ImGui::DragFloat3("##v", arr, speed, min, max);
    if (changed) { v.x = arr[0]; v.y = arr[1]; v.z = arr[2]; }
    ImGui::Columns(1);
    ImGui::PopID();
    return changed;
}

bool ColorEdit3(const char* label, math::Vector3& color)
{
    float arr[3] = { color.x, color.y, color.z };
    bool changed = ImGui::ColorEdit3(label, arr);
    if (changed) { color.x = arr[0]; color.y = arr[1]; color.z = arr[2]; }
    return changed;
}

bool RangeField(const char* label, float& value, float min, float max, const char* fmt)
{
    ImGui::PushID(label);
    bool changed = false;

    const ImGuiStyle& style = ImGui::GetStyle();
    constexpr float kInputW = 58.0f; // 右側の数値入力ボックス幅
    const float total   = ImGui::CalcItemWidth();
    const float sliderW = std::max(40.0f, total - kInputW - style.ItemSpacing.x);

    // ゲージ (バー)。数値は右の入力ボックスで表示するため、バー上の数値は消す ("")。
    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::SliderFloat("##slider", &value, min, max, "")) changed = true;

    // 数値入力ボックス: DragFloat を流用し、ダブルクリックで直接タイプ・ドラッグで微調整。
    // min/max クランプはスライダーと共通なので、範囲外の値が入らない。
    ImGui::SameLine(0.0f, style.ItemSpacing.x);
    ImGui::SetNextItemWidth(kInputW);
    const float dragSpeed = (max > min) ? (max - min) * 0.005f : 0.01f;
    if (ImGui::DragFloat("##input", &value, dragSpeed, min, max, fmt)) changed = true;

    // ラベルを右に配置する。ただし "##" 始まりは「ラベル非表示」指定として描画を省く
    // (呼び出し側が左カラムへ別途ラベルを描くレイアウトで使う)。
    if (!(label[0] == '#' && label[1] == '#')) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::TextUnformatted(label);
    }

    ImGui::PopID();
    return changed;
}

void SectionHeader(const char* label)
{
    ImGui::SeparatorText(label);
}

void ColoredText(const char* text, ImVec4 color)
{
    ImGui::TextColored(color, "%s", text);
}

void ReadOnlyText(const char* label, const char* text)
{
    ImGui::LabelText(label, "%s", text);
}

} // namespace fbzz::editor::widgets
