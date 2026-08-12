// FBZZ Engine
// ImGuiWidgets.cpp | fbzz::editor
// プロジェクト固有の ImGui カスタムウィジェット実装
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ITexture.hpp>
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
#include <system_error>
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
    uint32_t width = 0;
    uint32_t height = 0;
};
// パス → サムネイル。ピッカーを開くたびにクリアして最新の見た目を反映する。
std::unordered_map<std::string, PickerThumb> s_thumbCache;
std::unordered_map<std::string, std::vector<asset::SpriteRect>> s_spriteCache;
renderer::ResourceManager* s_thumbnailResources = nullptr;
renderer::IImGuiRenderer* s_thumbnailImGui = nullptr;

struct AssignedSpriteThumb {
    void* texId = nullptr;
    ImVec2 uvMin = { 0.0f, 0.0f };
    ImVec2 uvMax = { 1.0f, 1.0f };
    std::string displayName;
    std::filesystem::file_time_type metaWriteTime{};
    std::uint64_t resetVersion = 0;
    bool resolved = false;
};
std::unordered_map<std::string, AssignedSpriteThumb> s_assignedSpriteCache;

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
            if (h.IsValid()) {
                t.texId = imgui->GetImTextureID(h, *res);
                if (const renderer::ITexture* texture = res->Get(h)) {
                    t.width = texture->GetWidth();
                    t.height = texture->GetHeight();
                }
            }
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

// Sprite型Textureのサブアセット矩形を.metaから取得する。
// ピッカーを開いている間はキャッシュし、候補行ごとのTOML再解析を避ける。
const std::vector<asset::SpriteRect>& ResolveSprites(const std::string& absPath)
{
    if (auto found = s_spriteCache.find(absPath); found != s_spriteCache.end())
        return found->second;

    std::vector<asset::SpriteRect> sprites;
    asset::TextureAsset textureAsset;
    asset::TexDescSerializer serializer;
    const std::string metaPath = absPath + ".meta";
    if (util::FileSystem::Exists(util::FileSystem::PathFromUtf8(metaPath))
        && serializer.Load(metaPath, textureAsset)
        && textureAsset.settings.type == asset::TextureType::Sprite) {
        sprites = textureAsset.settings.sprites;
        if (sprites.empty()) {
            asset::SpriteRect sprite;
            sprite.name = util::FileSystem::PathToUtf8(
                util::FileSystem::PathFromUtf8(absPath).stem());
            sprites.push_back(std::move(sprite));
        }
    }
    return s_spriteCache.emplace(absPath, std::move(sprites)).first->second;
}

// Inspector に割り当て済みの Sprite 参照を、元 Texture と UV 矩形へ解決する。
// WHY: 名前だけでは atlas 内のどの絵か確認できないため、閉じたフィールドにも切り抜き画像を表示する。
const AssignedSpriteThumb& ResolveAssignedSpriteThumb(const std::string& reference)
{
    static const AssignedSpriteThumb EMPTY;
    if (s_thumbnailResources == nullptr || s_thumbnailImGui == nullptr) return EMPTY;

    std::string texturePath;
    std::string spriteName;
    if (!asset::ParseSpriteReference(reference, texturePath, spriteName)) return EMPTY;

    const std::string absPath = asset::AssetManager::ResolveAssetPath(texturePath);
    const std::string metaPath = absPath + ".meta";
    std::error_code ec;
    const auto metaWriteTime = std::filesystem::last_write_time(
        util::FileSystem::PathFromUtf8(metaPath), ec);
    const std::uint64_t resetVersion = s_thumbnailResources->GetResetVersion();

    AssignedSpriteThumb& cached = s_assignedSpriteCache[reference];
    if (cached.resolved && cached.metaWriteTime == metaWriteTime
        && cached.resetVersion == resetVersion)
        return cached;

    cached = {};
    cached.metaWriteTime = metaWriteTime;
    cached.resetVersion = resetVersion;
    cached.resolved = true;

    asset::TextureAsset textureAsset;
    asset::TexDescSerializer serializer;
    if (ec || !serializer.Load(metaPath, textureAsset)) return cached;
    asset::SpriteRect implicitSingleSprite;
    const asset::SpriteRect* sprite = asset::FindSprite(textureAsset.settings, spriteName);
    if (sprite == nullptr
        && textureAsset.settings.type == asset::TextureType::Sprite
        && textureAsset.settings.spriteMode == asset::SpriteMode::Single) {
        implicitSingleSprite.name = util::FileSystem::PathToUtf8(
            util::FileSystem::PathFromUtf8(absPath).stem());
        if (implicitSingleSprite.name == spriteName)
            sprite = &implicitSingleSprite;
    }
    if (sprite == nullptr) return cached;
    cached.displayName = sprite->name;

    const auto handle = s_thumbnailResources->LoadTexture(absPath);
    const renderer::ITexture* texture = handle.IsValid()
        ? s_thumbnailResources->Get(handle) : nullptr;
    if (texture == nullptr) return cached;

    cached.texId = s_thumbnailImGui->GetImTextureID(handle, *s_thumbnailResources);
    const float width = static_cast<float>(std::max<uint32_t>(1, texture->GetWidth()));
    const float height = static_cast<float>(std::max<uint32_t>(1, texture->GetHeight()));
    const float spriteWidth = sprite->width > 0 ? static_cast<float>(sprite->width) : width;
    const float spriteHeight = sprite->height > 0 ? static_cast<float>(sprite->height) : height;
    cached.uvMin = {
        std::clamp(static_cast<float>(sprite->x) / width, 0.0f, 1.0f),
        std::clamp(static_cast<float>(sprite->y) / height, 0.0f, 1.0f)
    };
    cached.uvMax = {
        std::clamp((static_cast<float>(sprite->x) + spriteWidth) / width, 0.0f, 1.0f),
        std::clamp((static_cast<float>(sprite->y) + spriteHeight) / height, 0.0f, 1.0f)
    };
    return cached;
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

// 拡張子フィルタの分解は AssetSearch と共通にする。
// WHY 薄いラッパーを残すか: 呼び出し側が 3 箇所あり、シグネチャ (const char*) も
//     既存のまま維持したい。実体を共通化しつつ呼び出しは変えない。
std::vector<std::string> SplitFilterExts(const char* exts)
{
    if (!exts || !exts[0]) return {};
    return AssetSearch::ParseExtensionFilter(exts);
}

} // namespace

bool InputString(const char* label, std::string& value, std::size_t capacity)
{
    // ImGui::InputText は生バッファしか受け取らないため、毎回作り直して書き戻す。
    // capacity より現在値が長い場合は現在値に合わせる (切り詰めて黙って壊さない)。
    std::vector<char> buffer((std::max)(capacity, value.size() + 2), '\0');
    std::memcpy(buffer.data(), value.data(), value.size());
    if (!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
    value = buffer.data();
    return true;
}

ImTextureID ToImTextureID(void* ptr)
{
    return static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(ptr));
}

void* ResolveAssetThumbnail(const std::string& relativePath,
                            renderer::ResourceManager* resources,
                            renderer::IImGuiRenderer* imguiRenderer)
{
    if (relativePath.empty() || resources == nullptr || imguiRenderer == nullptr) return nullptr;

    // デバイスリセット後は過去のテクスチャ ID が全て無効になる。世代が変わったら丸ごと捨てる。
    static std::unordered_map<std::string, void*> cache;
    static std::uint64_t cachedResetVersion = 0;
    if (const std::uint64_t version = resources->GetResetVersion(); version != cachedResetVersion) {
        cachedResetVersion = version;
        cache.clear();
    }
    if (auto found = cache.find(relativePath); found != cache.end()) return found->second;

    void* textureId = nullptr;
    std::string texturePath;
    std::string spriteName;
    asset::ParseSpriteReference(relativePath, texturePath, spriteName);
    const std::string extension =
        util::StringUtils::ToLower(util::FileSystem::GetExtension(texturePath));
    if (IsImageExt(extension)) {
        const auto handle = resources->LoadTexture(asset::AssetManager::ResolveAssetPath(texturePath));
        if (handle.IsValid()) textureId = imguiRenderer->GetImTextureID(handle, *resources);
    } else if (extension == ".mat") {
        // .mat は albedo を代表画にする。マテリアルを割り当てた Particle でも
        // 「どんな絵が出るのか」がノードから読めるようにするため。
        const auto materialHandle = asset::AssetManager::LoadMaterial(relativePath);
        if (const asset::MaterialAsset* material = asset::AssetManager::GetMaterial(materialHandle)) {
            if (auto albedo = material->textures.find("albedo");
                albedo != material->textures.end() && !albedo->second.empty()) {
                const auto handle = resources->LoadTexture(
                    asset::AssetManager::ResolveAssetPath(albedo->second));
                if (handle.IsValid()) textureId = imguiRenderer->GetImTextureID(handle, *resources);
            }
        }
    }
    // 解決できなかった場合も nullptr を覚える。毎フレーム同じ探索を繰り返さないため。
    cache.emplace(relativePath, textureId);
    return textureId;
}

bool AcceptAssetPathDrop(std::string& outPath, const char* filterExts)
{
    bool dropped = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            const std::string candidate = NormalizeAssetPath(
                std::string(static_cast<const char*>(p->Data),
                            static_cast<size_t>(p->DataSize) - 1));
            std::string texturePath;
            std::string spriteName;
            asset::ParseSpriteReference(candidate, texturePath, spriteName);
            const std::string extension = util::StringUtils::ToLower(
                util::FileSystem::GetExtension(texturePath));
            const std::vector<std::string> allowed = SplitFilterExts(filterExts);
            if (allowed.empty()
                || std::find(allowed.begin(), allowed.end(), extension) != allowed.end()) {
                outPath = candidate;
                dropped = true;
            }
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

    // Unity 風: 非フォーカス時はフルパスではなく [拡張子バッジ] + ファイル名だけを表示する。
    // WHY: "Assets/Nature/Rock/Rock/materials/namaqualand_boulder_03.mat" のような長い相対パスを
    //      そのまま InputText に出すと欄の幅で切れて視認性が悪い。クリックした瞬間だけフルパス
    //      編集用の InputText に切り替え、そこでは従来通りタイプ入力・ドラッグ&ドロップができる。
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID editingId    = ImGui::GetID("##editing");
    const ImGuiID focusReqId   = ImGui::GetID("##focusReq");
    const bool editing = path.empty() || storage->GetBool(editingId, false);

    if (editing) {
        ImGui::SetNextItemWidth(inputW);
        if (storage->GetBool(focusReqId, false)) {
            ImGui::SetKeyboardFocusHere();
            storage->SetBool(focusReqId, false);
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", path.c_str());
        if (ImGui::InputText("##path", buf, sizeof(buf))) {
            path    = NormalizeAssetPath(buf);
            changed = true;
        }
        if (ImGui::IsItemDeactivated())
            storage->SetBool(editingId, false);
        if (AcceptAssetPathDrop(path, filterExts))
            changed = true;
    } else {
        std::string texturePath;
        std::string spriteName;
        const bool isSprite = asset::ParseSpriteReference(path, texturePath, spriteName);
        const std::string ext =
            util::StringUtils::ToLower(util::FileSystem::GetExtension(texturePath));
        const AssignedSpriteThumb& spriteThumb = ResolveAssignedSpriteThumb(path);
        const std::string stem = isSprite
            ? (spriteThumb.displayName.empty() ? spriteName : spriteThumb.displayName)
            : util::FileSystem::PathToUtf8(
                util::FileSystem::PathFromUtf8(texturePath).stem());
        std::string badge = isSprite ? "SPRITE" : (ext.size() > 1 ? ext.substr(1) : ext);
        for (char& c : badge) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const ImVec4 badgeCol = ExtBadgeColor(ext);

        const ImVec2 boxMin  = ImGui::GetCursorScreenPos();
        const ImVec2 boxSize = {
            inputW,
            isSprite ? std::max(34.0f, ImGui::GetFrameHeight()) : ImGui::GetFrameHeight()
        };
        ImGui::InvisibleButton("##display", boxSize);
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 boxMax = { boxMin.x + boxSize.x, boxMin.y + boxSize.y };
        dl->AddRectFilled(boxMin, boxMax,
            ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
            style.FrameRounding);
        dl->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);

        float tx = boxMin.x + style.FramePadding.x;
        const float ty = boxMin.y + (boxSize.y - ImGui::GetTextLineHeight()) * 0.5f;
        if (isSprite && spriteThumb.texId) {
            const float previewSize = boxSize.y - 4.0f;
            const ImVec2 previewMin = { boxMin.x + 2.0f, boxMin.y + 2.0f };
            const ImVec2 previewMax = { previewMin.x + previewSize, previewMin.y + previewSize };
            constexpr int CHECKER_COUNT = 4;
            const float checkerSize = previewSize / static_cast<float>(CHECKER_COUNT);
            for (int y = 0; y < CHECKER_COUNT; ++y) {
                for (int x = 0; x < CHECKER_COUNT; ++x) {
                    const ImVec2 cellMin = {
                        previewMin.x + checkerSize * static_cast<float>(x),
                        previewMin.y + checkerSize * static_cast<float>(y)
                    };
                    const ImVec2 cellMax = {
                        std::min(cellMin.x + checkerSize, previewMax.x),
                        std::min(cellMin.y + checkerSize, previewMax.y)
                    };
                    dl->AddRectFilled(cellMin, cellMax, ((x + y) & 1) == 0
                        ? IM_COL32(70, 70, 70, 255) : IM_COL32(42, 42, 42, 255));
                }
            }
            dl->AddImageRounded(ToImTextureID(spriteThumb.texId), previewMin, previewMax,
                                spriteThumb.uvMin, spriteThumb.uvMax,
                                IM_COL32_WHITE, 2.0f);
            dl->AddRect(previewMin, previewMax, IM_COL32(0, 0, 0, 100), 2.0f);
            tx = previewMax.x + style.ItemInnerSpacing.x;
        }
        if (!badge.empty()) {
            char badgeLabel[16];
            std::snprintf(badgeLabel, sizeof(badgeLabel), "[%s]", badge.c_str());
            dl->AddText({ tx, ty }, ImGui::GetColorU32(badgeCol), badgeLabel);
            tx += ImGui::CalcTextSize(badgeLabel).x + style.ItemInnerSpacing.x;
        }
        // 右端に収まらない場合は先頭を "…" で省略し、常にファイル名の末尾が見えるようにする。
        const float availTextW = boxMax.x - style.FramePadding.x - tx;
        std::string shown = stem.empty() ? path : stem;
        bool truncated = false;
        while (ImGui::CalcTextSize(shown.c_str()).x > availTextW && shown.size() > 1) {
            shown.erase(0, 1);
            truncated = true;
        }
        if (truncated) shown = "\xE2\x80\xA6" + shown; // "…"
        dl->AddText({ tx, ty }, ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());

        if (hovered) {
            ImGui::BeginTooltip();
            if (isSprite && spriteThumb.texId) {
                ImGui::Image(ToImTextureID(spriteThumb.texId), { 160.0f, 160.0f },
                             spriteThumb.uvMin, spriteThumb.uvMax);
                ImGui::Separator();
            }
            ImGui::TextUnformatted(path.c_str());
            ImGui::EndTooltip();
        }
        if (clicked) {
            storage->SetBool(editingId, true);
            storage->SetBool(focusReqId, true);
        }
        if (AcceptAssetPathDrop(path, filterExts))
            changed = true;
    }

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
        // 索引は AssetSearch が保持する。ルートが同じなら再走査は起きない。
        // WHY 変更したか: 以前はピッカーを開くたびにプロジェクトルート全体を
        //     recursive_directory_iterator で舐めており、build/ や ThirdParty/ まで
        //     含めて数万ファイルを走査していた。開くたびに待ちが発生していた。
        AssetSearch::SetProjectRoot(projectRoot);
    }

    // ラベルを ImGui 標準ラベル列（右側）に配置。ウィンドウ幅でクリップされる。
    // "##" 始まりは共通 Reflector が左カラムへラベルを描画済みであることを示す。
    // WHY: 同じ AssetPathField を手書き Inspector と自動生成 Inspector の両方で使い、
    //      自動生成側で内部 ID が画面へ重複表示されるのを防ぐ。
    if (!(label[0] == '#' && label[1] == '#')) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::TextUnformatted(label);
    }

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
    AssetSearch::SetProjectRoot(projectRoot);
}

void DrawAssetPickerModal(renderer::ResourceManager* resources,
                          renderer::IImGuiRenderer* imgui)
{
    s_thumbnailResources = resources;
    s_thumbnailImGui = imgui;
    if (s_picker.open) {
        ImGui::OpenPopup("##asset_picker");
        s_picker.open = false;
        // 開くたびにサムネイルを作り直し、直近のアセット編集を反映する。
        s_thumbCache.clear();
        s_spriteCache.clear();
    }

    // ── パネル位置: フィールド左端の直下。画面端でクランプ ──────────────────
    // Texture の絵と名前を同時に判別できるよう、Inspector 幅より少し広い
    // Unity 風オブジェクトピッカーとして表示する。
    constexpr float kW = 460.0f;
    constexpr float kH = 480.0f;
    const ImVec2 vpPos  = ImGui::GetMainViewport()->Pos;
    const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    const float windowW = std::max(280.0f, std::min(kW, vpSize.x - 8.0f));
    const float windowH = std::max(240.0f, std::min(kH, vpSize.y - 8.0f));
    ImVec2 pos = s_picker.anchorPos;
    pos.x = std::max(pos.x, vpPos.x + 4.0f);
    if (pos.x + windowW > vpPos.x + vpSize.x - 4.0f)
        pos.x = vpPos.x + vpSize.x - windowW - 4.0f;
    if (pos.y + windowH > vpPos.y + vpSize.y - 4.0f)
        pos.y = s_picker.anchorPos.y - windowH - ImGui::GetFrameHeightWithSpacing();
    pos.y = std::max(pos.y, vpPos.y + 4.0f);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ windowW, windowH }, ImGuiCond_Always);
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
    const float rowH = 62.0f;

    auto drawEntry = [&](const std::string& absPath, const std::string& ext,
                         const std::string& displayName, const std::string& reference,
                         const asset::SpriteRect* sprite) {
        // 一致判定は AssetSearch と共通にする。
        // WHY: 以前は単純な部分一致だったため、"ppvol" のような略記では
        //      "PostProcessVolume" に辿り着けなかった。Search パネルとも
        //      当たり方が違い、同じ語で結果が食い違っていた。
        if (!searchStr.empty()
            && AssetSearch::Match(displayName, searchStr) == 0
            && AssetSearch::Match(reference, searchStr) == 0) {
            return;
        }

        const bool selected = s_picker.target && *s_picker.target == reference;
        ImGui::PushID(reference.c_str());
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        if (selected) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                rowMin, { rowMin.x + rowW, rowMin.y + rowH },
                IM_COL32(55, 95, 170, 130), 3.0f);
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float thumbSize = rowH - 8.0f;
        const ImVec2 thumbMin = { rowMin.x + 4.0f, rowMin.y + 3.0f };
        const ImVec2 thumbMax = { thumbMin.x + thumbSize, thumbMin.y + thumbSize };
        const ImVec4 badgeColor = ExtBadgeColor(ext);
        const std::string textureReference = NormalizeAssetPath(absPath);
        const PickerThumb& thumb =
            ResolveThumb(absPath, textureReference, ext, resources, imgui);

        ImVec2 uvMin = { 0.0f, 0.0f };
        ImVec2 uvMax = { 1.0f, 1.0f };
        if (sprite != nullptr && thumb.width > 0 && thumb.height > 0) {
            const float width = static_cast<float>(thumb.width);
            const float height = static_cast<float>(thumb.height);
            const float spriteWidth = sprite->width > 0
                ? static_cast<float>(sprite->width) : width;
            const float spriteHeight = sprite->height > 0
                ? static_cast<float>(sprite->height) : height;
            uvMin = {
                static_cast<float>(sprite->x) / width,
                static_cast<float>(sprite->y) / height
            };
            uvMax = {
                (static_cast<float>(sprite->x) + spriteWidth) / width,
                (static_cast<float>(sprite->y) + spriteHeight) / height
            };
        }

        if (thumb.texId) {
            constexpr int CHECKER_COUNT = 6;
            const float checkerSize = thumbSize / static_cast<float>(CHECKER_COUNT);
            for (int y = 0; y < CHECKER_COUNT; ++y) {
                for (int x = 0; x < CHECKER_COUNT; ++x) {
                    const ImU32 color = ((x + y) & 1) == 0
                        ? IM_COL32(70, 70, 70, 255)
                        : IM_COL32(42, 42, 42, 255);
                    const ImVec2 checkerMin = {
                        thumbMin.x + checkerSize * static_cast<float>(x),
                        thumbMin.y + checkerSize * static_cast<float>(y)
                    };
                    const ImVec2 checkerMax = {
                        std::min(checkerMin.x + checkerSize, thumbMax.x),
                        std::min(checkerMin.y + checkerSize, thumbMax.y)
                    };
                    drawList->AddRectFilled(checkerMin, checkerMax, color);
                }
            }
            drawList->AddImageRounded(ToImTextureID(thumb.texId),
                                      thumbMin, thumbMax, uvMin, uvMax,
                                      IM_COL32_WHITE, 3.0f);
        } else if (thumb.color.w > 0.0f) {
            drawList->AddRectFilled(
                thumbMin, thumbMax, ImGui::GetColorU32(thumb.color), 3.0f);
        } else {
            const ImU32 fill = ImGui::GetColorU32(
                { badgeColor.x, badgeColor.y, badgeColor.z, 0.28f });
            drawList->AddRectFilled(thumbMin, thumbMax, fill, 3.0f);
            drawList->AddRect(
                thumbMin, thumbMax, ImGui::GetColorU32(badgeColor), 3.0f);
        }
        drawList->AddRect(thumbMin, thumbMax, IM_COL32(0, 0, 0, 90), 3.0f);

        const float textLeft = thumbMax.x + 8.0f;
        std::string badge = sprite != nullptr
            ? "SPRITE" : (ext.size() > 1 ? ext.substr(1) : ext);
        for (char& c : badge)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        ImGui::SetCursorScreenPos({ textLeft, rowMin.y + 8.0f });
        ImGui::TextColored(badgeColor, "[%s]", badge.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(displayName.c_str());
        ImGui::SetCursorScreenPos({
            textLeft + 2.0f,
            rowMin.y + ImGui::GetTextLineHeightWithSpacing() + 10.0f
        });
        ImGui::TextDisabled("%s", reference.c_str());

        ImGui::SetCursorScreenPos(rowMin);
        if (ImGui::Selectable("##row", selected,
                ImGuiSelectableFlags_AllowOverlap, { rowW, rowH })) {
            if (s_picker.target) {
                *s_picker.target = reference;
                s_picker.justPickedTarget = s_picker.target;
            }
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (thumb.texId) {
                ImGui::Image(ToImTextureID(thumb.texId), { 160.0f, 160.0f }, uvMin, uvMax);
                ImGui::Separator();
            }
            ImGui::TextUnformatted(displayName.c_str());
            ImGui::TextDisabled("%s", reference.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
        ImGui::Dummy({ 0.0f, 2.0f });
    };

    // 索引から拡張子で絞った候補を取り出す。
    // 検索語による絞り込みは drawEntry 側で行う (スプライトのサブ項目も
    // 同じ規則で弾く必要があるため、ここでは拡張子だけを見る)。
    const auto candidates = AssetSearch::Query(
        {}, s_picker.filterExts, /*maxResults=*/AssetSearch::Count());

    for (const AssetSearchHit& candidate : candidates) {
        const std::string& absPath  = candidate.entry->absolutePath;
        const std::string& ext      = candidate.entry->extension;
        const std::string& filename = candidate.entry->filename;
        const std::string textureReference = NormalizeAssetPath(absPath);
        drawEntry(absPath, ext, filename, textureReference, nullptr);

        if (!IsImageExt(ext)) continue;
        for (const asset::SpriteRect& sprite : ResolveSprites(absPath)) {
            drawEntry(absPath, ext, sprite.name,
                      asset::MakeSpriteReference(
                          textureReference, sprite.id.empty() ? sprite.name : sprite.id),
                      &sprite);
        }
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
    // WHAT: 左のブランドライン、見出し、残り幅の細い罫線を一行で描く。
    // WHY: SeparatorText の標準表現だけでは情報階層が弱く、長い Inspector で区切りを見失うため。
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float lineHeight = ImGui::GetTextLineHeight();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        cursor,
        { cursor.x + 3.0f, cursor.y + lineHeight },
        EditorTheme::ColorU32(ThemeColor::Accent),
        1.5f);

    ImGui::SetCursorScreenPos({ cursor.x + 9.0f, cursor.y });
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Text));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    const ImVec2 textMax = ImGui::GetItemRectMax();
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (textMax.x + 10.0f < right) {
        const float y = cursor.y + lineHeight * 0.5f;
        drawList->AddLine(
            { textMax.x + 10.0f, y },
            { right, y },
            EditorTheme::ColorU32(ThemeColor::Border));
    }
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
