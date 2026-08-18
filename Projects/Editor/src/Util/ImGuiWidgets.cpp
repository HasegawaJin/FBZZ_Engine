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
// SetNextItemColorMarker (軸色マーカー) が internal 側にあるため取り込む。
#include <imgui_internal.h>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
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

// カード / 区切り表現の寸法。すべて現在のフォントサイズから作る。
// WHY: 帯の太さやヘッダーの余白を px 直値で持つと、UI スケール (EditorTheme::SetUiScale)
//      が FontGlobalScale で文字だけを拡大するため、倍率を上げると区切りが相対的に細く、
//      ヘッダーの高さも詰まって見える (縮めると逆に帯だけが太く残る)。
//      基準を FontSize に一本化すれば、どの倍率でも文字と区切りの比率が変わらない。
// NOTE: ヘッダーと本文で別々に計算しないこと。継ぎ目で帯の太さが変わると段差になる。
struct CardMetrics {
    float accent; // 左帯の太さ
    float indent; // 本文の字下げ
};

CardMetrics Metrics()
{
    const float font = ImGui::GetFontSize();
    CardMetrics metrics;
    metrics.accent = std::max(2.0f, std::floor(font * 0.22f));
    metrics.indent = std::floor(font * 0.50f);
    return metrics;
}

// 見出しの帯に使う上下余白。文字の高さから作る薄い余白。
// WHY: 既定の FramePadding は「入力欄として押しやすい高さ」に合わせた値で、
//      読ませるだけの見出しには厚すぎる。コンポーネント数が多い Inspector では
//      1 枚あたり数 px の厚みがそのまま縦スクロール量になるため、帯は文字に
//      寄り添う高さまで詰めて、カードの枠が中身より目立たないようにする。
float HeaderPadY()
{
    return std::max(2.0f, std::floor(ImGui::GetFontSize() * 0.14f));
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

bool UpdateDragAutoScroll(const DragAutoScrollOptions& options)
{
    const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
    if (!window) return false;
    // InnerClipRect はスクロール量を含まない、現在画面に見えているコンテンツ領域。
    // GetWindowContentRegionMin/Max はスクロール量を含むため、画面端判定には使わない。
    return UpdateDragAutoScroll(window->InnerClipRect.Min, window->InnerClipRect.Max, options);
}

bool UpdateDragAutoScroll(const ImVec2& regionMin, const ImVec2& regionMax,
                          const DragAutoScrollOptions& options)
{
    // Payload が無いときは通常のマウス移動なので、スクロールを発生させない。
    if (!ImGui::IsDragDropActive()) return false;
    if (options.edgeSize <= 0.0f || options.maxSpeed <= 0.0f) return false;
    if (regionMax.x <= regionMin.x || regionMax.y <= regionMin.y) return false;
    if (!ImGui::IsMouseHoveringRect(regionMin, regionMax, false)) return false;

    const float edgeSize = options.edgeSize;
    const float mouseY = ImGui::GetIO().MousePos.y;
    float intensity = 0.0f;
    float direction = 0.0f;
    if (mouseY < regionMin.y + edgeSize) {
        intensity = 1.0f - std::clamp((mouseY - regionMin.y) / edgeSize, 0.0f, 1.0f);
        direction = -1.0f;
    } else if (mouseY > regionMax.y - edgeSize) {
        intensity = 1.0f - std::clamp((regionMax.y - mouseY) / edgeSize, 0.0f, 1.0f);
        direction = 1.0f;
    }
    if (intensity <= 0.0f) return false;

    // 端に近いほど加速させる。線形速度だと帯の入口で急に速く感じるため、
    // 二乗カーブで微調整しやすく、端では十分な速度になるようにする。
    const float deltaTime = ImGui::GetIO().DeltaTime > 0.0f
        ? ImGui::GetIO().DeltaTime : (1.0f / 60.0f);
    const float speed = options.maxSpeed * intensity * intensity;
    const float current = ImGui::GetScrollY();
    const float target = std::clamp(current + direction * speed * deltaTime,
                                    0.0f, ImGui::GetScrollMaxY());
    if (std::abs(target - current) <= 0.001f) return false;

    ImGui::SetScrollY(target);
    return true;
}

namespace {

// AssetBrowser へ渡す Ping / 選択要求の唯一の置き場。
// WHY: 1 フレームに何度クリックが起きても最後の 1 件だけが意味を持つため、キューではなく
//      上書きの単一スロットにする。EditorApp が取り出した時点で空へ戻す。
AssetRevealRequest s_revealRequest;
bool               s_revealRequestPending = false;

} // namespace

void RequestAssetReveal(std::string assetPath, bool selectInInspector)
{
    if (assetPath.empty()) return;
    // ダブルクリックは「1 回目のクリック (Ping) → 2 回目 (選択)」の順で届く。
    // 同じアセットへの要求なら選択指定を落とさないよう OR で畳む。
    if (s_revealRequestPending && s_revealRequest.path == assetPath)
        selectInInspector = selectInInspector || s_revealRequest.selectInInspector;
    s_revealRequest.path              = std::move(assetPath);
    s_revealRequest.selectInInspector = selectInInspector;
    s_revealRequestPending            = true;
}

bool ConsumeAssetRevealRequest(AssetRevealRequest& out)
{
    if (!s_revealRequestPending) return false;
    out                    = std::move(s_revealRequest);
    s_revealRequest        = {};
    s_revealRequestPending = false;
    return true;
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
        // ホバー判定と押下判定はツールチップを描く前に確定させる。
        // WHY: BeginTooltip 内のテキストで ImGui の「直前のアイテム」が上書きされるため、
        //      IsItemClicked 系をツールチップの後ろで呼ぶと本体ではなく別物を見てしまう。
        const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const bool rightClicked  = hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right);

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
            ImGui::Separator();
            ImGui::TextDisabled("Click: Asset Browser で表示  /  Double-Click: 選択して Inspector へ");
            ImGui::TextDisabled("Right-Click: パス編集・コピー・クリア");
            ImGui::EndTooltip();
        }

        // Unity の Object Field と同じ動線にする。
        // シングルクリックは Ping (AssetBrowser 側で場所を示すだけ)、ダブルクリックは選択して
        // Inspector の表示対象そのものを参照先アセットへ移す。
        // NOTE: ダブルクリック時は 2 回目の押下で clicked / doubleClicked が同時に立つ。
        //       RequestAssetReveal が同一パスの要求を畳むため、選択指定は落ちない。
        if (clicked)       RequestAssetReveal(path, false);
        if (doubleClicked) RequestAssetReveal(path, true);

        // ドロップ受理はメニュー描画より前に置く。
        // WHY: BeginDragDropTarget は「直前のアイテム」を受け皿にするため、間に別ウィンドウの
        //      アイテム (ポップアップの MenuItem 等) を挟むと本体の矩形を見失う。
        if (AcceptAssetPathDrop(path, filterExts))
            changed = true;

        // パスを文字列として直接いじりたいケース (手打ち・貼り付け・参照外し) は
        // クリックから右クリックメニューへ移す。頻度の低い操作に主クリックを使わせない。
        if (rightClicked) ImGui::OpenPopup("##assetFieldMenu");
        if (ImGui::BeginPopup("##assetFieldMenu")) {
            if (ImGui::MenuItem("Show in Asset Browser"))
                RequestAssetReveal(path, true);
            ImGui::Separator();
            if (ImGui::MenuItem("Edit Path...")) {
                storage->SetBool(editingId, true);
                storage->SetBool(focusReqId, true);
            }
            if (ImGui::MenuItem("Copy Path"))
                ImGui::SetClipboardText(path.c_str());
            if (ImGui::MenuItem("Clear")) {
                path.clear();
                changed = true;
            }
            ImGui::EndPopup();
        }
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
    const ImGuiStyle& style = ImGui::GetStyle();
    const float refreshW = ImGui::CalcTextSize("Reindex").x + style.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - refreshW - style.ItemSpacing.x);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    // EnterReturnsTrue: 入力自体は毎フレーム反映されたまま、戻り値だけが
    // 「Enter が押された」を表すようになる。1 件に絞れたらそのまま確定できる。
    const bool enterPressed = ImGui::InputTextWithHint(
        "##search", "Search by name...", s_picker.search, sizeof(s_picker.search),
        ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SameLine();
    if (ImGui::Button("Reindex", { refreshW, 0.0f }))
        AssetSearch::Rebuild();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rebuild the asset index\n(use this if a new file does not show up)");

    const std::string searchStr = s_picker.search;

    // 索引をスコア順で引く。
    // WHY 変更したか: 以前は空クエリで索引を全件取り出し、行を描くときに Match() で
    //     捨てるだけだった。つまり並びは常にパス順で、スコア (完全一致 > 前方一致 >
    //     部分一致 > 部分列一致) が一切効いていない。Match は部分列一致まで拾うため、
    //     "rock" のような語でも R…o…c…k を含む無関係なファイルが大量に、しかも
    //     パス順で混ざって出る = 「検索しても目的のものが出てこない」状態だった。
    //     さらに maxResults に索引の全件数を渡していたので、索引が空の場合は
    //     resize(0) で無条件に 0 件になり、その手掛かりも画面に出なかった。
    constexpr std::size_t kMaxPickerRows = 300;
    const auto candidates =
        AssetSearch::Query(searchStr, s_picker.filterExts, kMaxPickerRows);

    // 絞り込み条件と件数を 1 行で見せる。索引が空なのか語が悪いのかを区別できるようにする。
    {
        std::string status;
        for (const std::string& ext : s_picker.filterExts) {
            if (!status.empty()) status += " ";
            status += ext;
        }
        if (status.empty()) status = "all files";
        char countBuf[64];
        std::snprintf(countBuf, sizeof(countBuf), "  -  %zu / %zu indexed",
                      candidates.size(), AssetSearch::Count());
        status += countBuf;
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextFaint));
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    // ── アセットリスト ─────────────────────────────────────────────────────
    ImGui::BeginChild("##list", { 0.0f, ImGui::GetContentRegionAvail().y }, false);

    // 最初の候補。検索欄で Enter を押したときの決定先にする。
    const std::string* firstReference = nullptr;
    std::string firstReferenceStorage;

    // "(none)" — フィールドをクリアするオプション
    {
        const bool selNone = (s_picker.target && s_picker.target->empty());
        if (ImGui::Selectable("(none)", selNone)) {
            if (s_picker.target) {
                *s_picker.target          = {};
                s_picker.justPickedTarget = s_picker.target;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }

    // 行の寸法もフォントサイズから作る。
    // WHY: 62px 固定だったため、UI スケールを上げると 2 行ぶんのテキストが行に収まらず
    //      下段のパスが次の行のサムネイルへ重なっていた。
    const float rowPad = std::floor(std::max(4.0f, ImGui::GetFontSize() * 0.42f));
    const float lineH  = ImGui::GetTextLineHeight();
    const float gapY   = std::floor(rowPad * 0.8f);
    const float rowH   = std::floor(std::max(ImGui::GetFontSize() * 4.0f,
                                             lineH * 2.0f + gapY + rowPad * 2.0f));
    // 選択行の左バーは、コンポーネントカードの帯と同じ太さにそろえる。
    const float selectionBar = Metrics().accent;

    auto drawEntry = [&](const std::string& absPath, const std::string& ext,
                         const std::string& displayName, const std::string& reference,
                         const asset::SpriteRect* sprite) {
        if (firstReferenceStorage.empty()) {
            firstReferenceStorage = reference;
            firstReference = &firstReferenceStorage;
        }

        const bool selected = s_picker.target && *s_picker.target == reference;
        ImGui::PushID(reference.c_str());
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;

        // 行の地色。選択はアクセント、ホバーは Surface。
        // 当たり判定用の Selectable は最後に置くため、ホバーは前フレームの矩形で自前に見る。
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool rowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
                             && mouse.x >= rowMin.x && mouse.x < rowMin.x + rowW
                             && mouse.y >= rowMin.y && mouse.y < rowMin.y + rowH;
        if (selected || rowHovered) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                rowMin, { rowMin.x + rowW, rowMin.y + rowH },
                EditorTheme::ColorU32(selected ? ThemeColor::AccentSoft : ThemeColor::SurfaceHover,
                                      selected ? 0.85f : 0.6f),
                4.0f);
        }
        if (selected) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                rowMin, { rowMin.x + selectionBar, rowMin.y + rowH },
                EditorTheme::ColorU32(ThemeColor::Accent), ImGui::GetStyle().FrameRounding,
                ImDrawFlags_RoundCornersLeft);
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float thumbSize = rowH - rowPad * 2.0f;
        const ImVec2 thumbMin = { rowMin.x + rowPad * 2.0f, rowMin.y + rowPad };
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

        const float textLeft = thumbMax.x + rowPad * 1.6f;
        std::string badge = sprite != nullptr
            ? "SPRITE" : (ext.size() > 1 ? ext.substr(1) : ext);
        for (char& c : badge)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        // 名前 + パスの 2 行を行の中央へ置く。
        const float blockH = lineH * 2.0f + gapY;
        const float nameY  = rowMin.y + std::floor((rowH - blockH) * 0.5f);

        // 拡張子は角丸のピルにする。"[MAT]" の文字より小さく収まり、色の面積が増えて
        // 種別をスクロール中の視野で拾いやすい。
        float nameRight = rowMin.x + rowW - rowPad * 2.0f;
        if (!badge.empty()) {
            const ImVec2 badgeSize = ImGui::CalcTextSize(badge.c_str());
            const float  pillPadY  = std::floor(gapY * 0.5f);
            const ImVec2 pillMax = { nameRight, nameY + badgeSize.y + pillPadY };
            const ImVec2 pillMin = { pillMax.x - badgeSize.x - gapY * 2.0f, nameY - pillPadY };
            drawList->AddRectFilled(pillMin, pillMax,
                ImGui::GetColorU32({ badgeColor.x, badgeColor.y, badgeColor.z, 0.22f }),
                (pillMax.y - pillMin.y) * 0.5f);
            drawList->AddText({ pillMin.x + gapY, nameY },
                              ImGui::GetColorU32(badgeColor), badge.c_str());
            nameRight = pillMin.x - rowPad * 1.6f;
        }

        // 名前は右のピルへ食い込まないよう末尾を省略する。
        {
            const float availW = std::max(ImGui::GetFontSize(), nameRight - textLeft);
            const char* nameEnd = displayName.c_str() + displayName.size();
            drawList->PushClipRect({ textLeft, rowMin.y },
                                   { textLeft + availW, rowMin.y + rowH }, true);
            drawList->AddText({ textLeft, nameY },
                              EditorTheme::ColorU32(ThemeColor::Text),
                              displayName.c_str(), nameEnd);
            drawList->PopClipRect();
        }

        // 参照パスは末尾 (ファイル名側) が読めるよう、あふれたら先頭を省略する。
        {
            const float availW = std::max(ImGui::GetFontSize(),
                                          rowMin.x + rowW - rowPad * 2.0f - textLeft);
            std::string shown = reference;
            bool truncated = false;
            while (ImGui::CalcTextSize(shown.c_str()).x > availW && shown.size() > 1) {
                shown.erase(0, 1);
                truncated = true;
            }
            if (truncated) shown = "\xE2\x80\xA6" + shown;
            drawList->AddText({ textLeft, nameY + lineH + gapY },
                              EditorTheme::ColorU32(ThemeColor::TextFaint), shown.c_str());
        }

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

    for (const AssetSearchHit& candidate : candidates) {
        const std::string& absPath  = candidate.entry->absolutePath;
        const std::string& ext      = candidate.entry->extension;
        const std::string& filename = candidate.entry->filename;
        const std::string textureReference = NormalizeAssetPath(absPath);
        drawEntry(absPath, ext, filename, textureReference, nullptr);

        if (!IsImageExt(ext)) continue;

        // スプライトのサブ項目。親テクスチャ名が語に当たっているなら全部出し、
        // パス経由でしか当たっていないなら、さらにスプライト名でも絞る。
        // WHY: アトラス 1 枚に 40 個入っていることがあり、無条件に展開すると
        //      検索したのに一覧が親テクスチャ 1 枚で埋まってしまう。
        const bool parentMatched =
            searchStr.empty() || AssetSearch::Match(filename, searchStr) != 0;
        for (const asset::SpriteRect& sprite : ResolveSprites(absPath)) {
            if (!parentMatched && AssetSearch::Match(sprite.name, searchStr) == 0)
                continue;
            drawEntry(absPath, ext, sprite.name,
                      asset::MakeSpriteReference(
                          textureReference, sprite.id.empty() ? sprite.name : sprite.id),
                      &sprite);
        }
    }

    // 空の結果は「何も出ない」ではなく理由を出す。
    // WHY: 索引が空 (プロジェクトルート未設定) なのか、語が当たっていないのかで
    //      次にやることが違う。区別が付かないと「検索が壊れている」ようにしか見えない。
    if (firstReference == nullptr) {
        ImGui::Spacing();
        if (AssetSearch::Count() == 0) {
            ImGui::TextDisabled("The asset index is empty.");
            ImGui::TextDisabled("Press R above to rebuild it.");
        } else if (!searchStr.empty()) {
            ImGui::TextDisabled("No asset matches \"%s\".", searchStr.c_str());
        } else {
            ImGui::TextDisabled("No asset of this type was found in the project.");
        }
    }

    // 検索欄で Enter → 先頭の候補を決定する。1 件に絞れたときに
    // マウスへ手を戻さず確定できるようにする。
    if (firstReference != nullptr && enterPressed) {
        if (s_picker.target) {
            *s_picker.target = *firstReference;
            s_picker.justPickedTarget = s_picker.target;
        }
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndChild();
    ImGui::EndPopup();
}

// ── 既存ウィジェット ─────────────────────────────────────────────────────

bool DragVec3(const char* label, math::Vector3& v, float speed, float min, float max)
{
    // ラベル列は Inspector 共通の幅を使い、成分は軸色付きにする。
    // WHY: 以前は Columns(2) + 固定 100px のラベル列だったため、(1) UI スケールを上げると
    //      ラベルが切れる、(2) Reflector 生成の行と値の左端が揃わない、という 2 つの
    //      不揃いを抱えていた。共通のプロパティ行に乗せ替えて両方まとめて解消する。
    const PropertyRowScope row = BeginPropertyField(label);
    float arr[3] = { v.x, v.y, v.z };
    const bool changed = DragAxes("##v", arr, 3, speed, min, max);
    if (changed) { v.x = arr[0]; v.y = arr[1]; v.z = arr[2]; }
    EndPropertyField(row);
    return changed;
}

bool ColorEdit3(const char* label, math::Vector3& color)
{
    float arr[3] = { color.x, color.y, color.z };
    bool changed = ImGui::ColorEdit3(label, arr);
    if (changed) { color.x = arr[0]; color.y = arr[1]; color.z = arr[2]; }
    return changed;
}

bool RangeField(const char* label, float& value, float min, float max, const char* fmt,
                const char* tooltip)
{
    ImGui::PushID(label);
    bool changed = false;
    bool hovered = false;

    const ImGuiStyle& style = ImGui::GetStyle();
    // 入力ボックス幅は「符号付き 4 桁 + 小数」がちょうど収まる実測値から取る。
    // WHY: 固定 58px だと UI スケールを上げた途端に数字が切れて読めなくなる。
    //      フォント実寸から求めればスケール設定に自動追従する。
    const float inputW = ImGui::CalcTextSize("-8888.888").x + style.FramePadding.x * 2.0f;
    const float total   = ImGui::CalcItemWidth();
    const float sliderW = std::max(40.0f, total - inputW - style.ItemSpacing.x);

    // ゲージ (バー)。数値は右の入力ボックスで表示するため、バー上の数値は消す ("")。
    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::SliderFloat("##slider", &value, min, max, "")) changed = true;
    hovered |= ImGui::IsItemHovered();

    // 現在値までを淡いアクセントで塗り、掴み位置だけでなく「どれくらいか」を面で見せる。
    // WHY: ImGui 標準のスライダーは掴み手だけなので、値の大小が一瞬で読み取れない。
    //      塗りは掴み手と同色系なので重なっても情報が潰れない。
    // NOTE: Ctrl+Click で数値入力に切り替わっている間は塗らない (入力中の文字が隠れる)。
    if (max > min && !ImGui::TempInputIsActive(ImGui::GetItemID())) {
        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        const float t = std::clamp((value - min) / (max - min), 0.0f, 1.0f);
        if (t > 0.0f) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                lo, { lo.x + (hi.x - lo.x) * t, hi.y },
                EditorTheme::ColorU32(ThemeColor::Accent, 0.30f),
                style.FrameRounding,
                t >= 1.0f ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
        }
    }

    // 数値入力ボックス: DragFloat を流用し、ダブルクリックで直接タイプ・ドラッグで微調整。
    // AlwaysClamp を付けるのは、Ctrl+Click のタイプ入力だけは既定で範囲外を通してしまい、
    // スライダーが端に張り付いたまま実値がずれる状態になるため。
    ImGui::SameLine(0.0f, style.ItemSpacing.x);
    ImGui::SetNextItemWidth(inputW);
    const float dragSpeed = (max > min) ? (max - min) * 0.005f : 0.01f;
    if (ImGui::DragFloat("##input", &value, dragSpeed, min, max, fmt,
                         ImGuiSliderFlags_AlwaysClamp))
        changed = true;
    hovered |= ImGui::IsItemHovered();

    // ラベルを右に配置する。ただし "##" 始まりは「ラベル非表示」指定として描画を省く
    // (呼び出し側が左カラムへ別途ラベルを描くレイアウトで使う)。
    if (!(label[0] == '#' && label[1] == '#')) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::TextUnformatted(label);
        hovered |= ImGui::IsItemHovered();
    }

    // 説明はウィジェット側で出す。
    // WHY: この 1 行はスライダー / 入力ボックス / ラベルの 3 アイテムで構成されるため、
    //      呼び出し側の ImGui::IsItemHovered() では最後のアイテムしか拾えない。
    //      「ラベルの上でしかツールチップが出ない」という分かりにくい罠を封じる。
    if (tooltip && tooltip[0] && hovered)
        ImGui::SetTooltip("%s", tooltip);

    ImGui::PopID();
    return changed;
}

bool RangeField(const char* label, int& value, int min, int max, const char* fmt,
                const char* tooltip)
{
    // 構成は float 版と同じ「塗り付きゲージ + 数値ボックス」。
    // WHY float 版へ委譲しないか: ImGui の書式指定子は型と対でなければならず、
    //   整数値を float スライダーに通すと "%d" が使えない (あるいは丸め残りで
    //   1 ずれた値が表示される)。見た目の規則だけを共有し、型は分けて扱う。
    ImGui::PushID(label);
    bool changed = false;
    bool hovered = false;

    const ImGuiStyle& style = ImGui::GetStyle();
    const float inputW  = ImGui::CalcTextSize("-8888.888").x + style.FramePadding.x * 2.0f;
    const float total   = ImGui::CalcItemWidth();
    const float sliderW = std::max(40.0f, total - inputW - style.ItemSpacing.x);

    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::SliderInt("##slider", &value, min, max, "")) changed = true;
    hovered |= ImGui::IsItemHovered();

    if (max > min && !ImGui::TempInputIsActive(ImGui::GetItemID())) {
        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        const float t = std::clamp(static_cast<float>(value - min) /
                                   static_cast<float>(max - min), 0.0f, 1.0f);
        if (t > 0.0f) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                lo, { lo.x + (hi.x - lo.x) * t, hi.y },
                EditorTheme::ColorU32(ThemeColor::Accent, 0.30f),
                style.FrameRounding,
                t >= 1.0f ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
        }
    }

    ImGui::SameLine(0.0f, style.ItemSpacing.x);
    ImGui::SetNextItemWidth(inputW);
    // 刻みは範囲の 0.5% (最低 1)。広い範囲でもドラッグ 1 往復で端まで届く。
    const float dragSpeed = std::max(1.0f, static_cast<float>(max - min) * 0.005f);
    if (ImGui::DragInt("##input", &value, dragSpeed, min, max, fmt ? fmt : "%d",
                       ImGuiSliderFlags_AlwaysClamp))
        changed = true;
    hovered |= ImGui::IsItemHovered();

    if (!(label[0] == '#' && label[1] == '#')) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::TextUnformatted(label);
        hovered |= ImGui::IsItemHovered();
    }
    if (tooltip && tooltip[0] && hovered)
        ImGui::SetTooltip("%s", tooltip);

    ImGui::PopID();
    return changed;
}

namespace {

// 軸色 (X=赤 / Y=緑 / Z=青 / W=灰)。彩度は抑えめにして暗色テーマで浮かないようにする。
ImU32 AxisMarkerColor(int axis)
{
    switch (axis) {
    case 0:  return IM_COL32(214,  87,  96, 255);
    case 1:  return IM_COL32(126, 190,  92, 255);
    case 2:  return IM_COL32( 84, 148, 224, 255);
    default: return IM_COL32(150, 150, 158, 255);
    }
}

const char* AxisLetter(int axis)
{
    static constexpr const char* kLetters[] = { "X", "Y", "Z", "W" };
    return kLetters[axis < 0 ? 0 : (axis > 3 ? 3 : axis)];
}

// 施錠アイコン。フォントの絵文字に依存しないよう自前で描く。
// WHY: このエディターの ImGui フォントは ASCII 中心で、鍵やチェーンのグリフを持たない。
//      文字を諦めて図形で描けば、フォント差し替えの影響も受けない。
void DrawLockGlyph(ImDrawList* drawList, ImVec2 center, float size, bool locked, ImU32 color)
{
    const float bodyW  = size * 0.62f;
    const float bodyH  = size * 0.44f;
    const float radius = bodyW * 0.32f;
    // シャックル上端から本体下端までを center に対して縦中央へ収める。
    const float glyphH  = bodyH + radius;
    const float bodyTop = center.y + glyphH * 0.5f - bodyH;

    const ImVec2 bodyMin{ center.x - bodyW * 0.5f, bodyTop };
    const ImVec2 bodyMax{ bodyMin.x + bodyW, bodyTop + bodyH };
    drawList->AddRectFilled(bodyMin, bodyMax, color, size * 0.10f);

    // シャックル (上部の弧)。開錠時は右へずらして「外れている」ことを形で示す。
    // NOTE: IM_PI は imgui_internal.h 側の定義なので、ここでは自前の定数を使う。
    constexpr float kPi = 3.14159265f;
    const float cx = center.x + (locked ? 0.0f : radius);
    drawList->PathClear();
    drawList->PathArcTo({ cx, bodyTop }, radius, kPi, kPi * 2.0f, 12);
    drawList->PathStroke(color, ImDrawFlags_None, std::max(1.0f, size * 0.10f));
}

} // namespace

bool DragAxes(const char* id, float* values, int count,
              float speed, float min, float max, const char* fmt)
{
    if (!values || count <= 0) return false;

    const ImGuiStyle& style = ImGui::GetStyle();
    // ラベルを右に描くぶんの幅を先に確保しておく (ImGui::DragFloat3 と同じ見た目にするため)。
    const bool showLabel = !(id[0] == '#' && id[1] == '#');
    const float labelW = showLabel
        ? ImGui::CalcTextSize(id, nullptr, true).x + style.ItemInnerSpacing.x
        : 0.0f;

    // 呼び出し側の SetNextItemWidth (-FLT_MIN など) を解決してから成分数で等分する。
    // 成分間は ItemInnerSpacing (ImGui の DragFloat3 と同じ) にして、3 つで 1 つの値だと分かる詰め方にする。
    const float gap   = style.ItemInnerSpacing.x;
    const float total = std::max(ImGui::GetFontSize() * 6.0f, ImGui::CalcItemWidth() - labelW);

    // 頭文字は枠の「外」に置く。1 成分あたりこのスロット幅を先に取り、残りを数値欄に回す。
    // WHY 外に出すか: 書式へ埋め込むと "X 0.000" が枠内で中央寄せされ、成分ごとに
    //     数値の左端がばらつく (桁数で揺れる)。頭文字を外へ出すと数値だけが枠に残り、
    //     3 成分の数字が同じ位置から始まるので、値の大小を縦に並べて比較できる。
    // NOTE: X/Y/Z/W で字幅が違うと数値欄の幅までずれるため、最も広い字で固定幅を取る。
    float letterGlyphW = 0.0f;
    for (int i = 0; i < count; ++i)
        letterGlyphW = std::max(letterGlyphW, ImGui::CalcTextSize(AxisLetter(i)).x);
    const float letterSlotW = letterGlyphW + style.ItemInnerSpacing.x;

    // 頭文字のぶんだけ数値欄は狭くなる。狭い Inspector では桁が欠けてしまうので、
    // 数値が読める幅を確保できないときは頭文字を落として数字を優先する。
    // WHY 文字を捨ててよいか: そのときは枠の左端に軸色のマーカーを出すため、
    //     色だけでも X / Y / Z は判別できる。読めない数字より欠けない数字を採る。
    const float sampleW = ImGui::CalcTextSize("-8888.888").x + style.FramePadding.x * 2.0f;
    const float span    = total - gap * static_cast<float>(count - 1);
    const float withLetter =
        (span - letterSlotW * static_cast<float>(count)) / static_cast<float>(count);
    const bool  showAxisLetter = withLetter >= sampleW;
    const float each = std::max(ImGui::GetFontSize() * 2.0f,
                                showAxisLetter ? withLetter : span / static_cast<float>(count));

    // BeginGroup で囲むのが必須。成分を個別の DragFloat で描くと IsItemActivated() /
    // IsItemDeactivatedAfterEdit() が「最後の成分」だけを見るようになり、X や Y を
    // 動かした操作が Undo に積まれなくなる (DragFloat3 も内部で同じことをしている)。
    // グループ化すれば ImGui が Active / Deactivated / Edited を全成分ぶん集約してくれる。
    ImGui::BeginGroup();
    ImGui::PushID(id);
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, gap);

        // 頭文字を枠の左隣へ描く。字幅が違っても数値欄の左端がそろうよう、
        // 描いた後はスロット幅ぶん進めた位置へカーソルを置き直す。
        if (showAxisLetter) {
            const float slotX = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, AxisMarkerColor(i));
            ImGui::TextUnformatted(AxisLetter(i));
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::SetCursorPosX(slotX + letterSlotW);
        }

        char label[16];
        std::snprintf(label, sizeof(label), "##axis%d", i);

        const float axisSpeed = speed > 0.0f ? speed : AdaptiveDragSpeed(values[i]);
        ImGui::SetNextItemWidth(each);
        // 頭文字を出せない狭さのときだけ、枠内の軸色マーカーで成分を示す。
        // WHY 標準機能を使うか: 自前で後描きすると枠線の上に乗ってしまうし、
        //     UI スケール変更にも追従しない。標準なら枠の内側・境界線の下に描かれ、
        //     style.ColorMarkerSize がスケールされる。
        if (!showAxisLetter) ImGui::SetNextItemColorMarker(AxisMarkerColor(i));
        if (ImGui::DragFloat(label, &values[i], axisSpeed, min, max, fmt ? fmt : "%.3f"))
            changed = true;
    }
    ImGui::PopID();

    // ImGui::DragFloat3 と同じく、"##" 始まりでないラベルは右側へ描く。
    // WHY: これがあると既存の DragFloat3 呼び出しをそのまま差し替えられる。
    if (showLabel) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::TextEx(id, ImGui::FindRenderedTextEnd(id));
    }

    ImGui::EndGroup();
    return changed;
}

bool DragScaleAxes(const char* id, math::Vector3& scale, bool& uniform, float speed)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lockW = ImGui::GetFrameHeight();
    const float total = ImGui::CalcItemWidth();

    ImGui::PushID(id);

    // リンクボタン。押下は即トグルなので、Push/Pop の対称を保つため描画前の状態で判定する。
    const bool wasLocked = uniform;
    ImGui::PushStyleColor(ImGuiCol_Button,
        EditorTheme::Color(wasLocked ? ThemeColor::AccentSoft : ThemeColor::Field));
    const bool lockClicked = ImGui::Button("##uniform", { lockW, lockW });
    ImGui::PopStyleColor();

    DrawLockGlyph(ImGui::GetWindowDrawList(),
                  { (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                    (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f },
                  lockW * 0.8f, wasLocked,
                  EditorTheme::ColorU32(wasLocked ? ThemeColor::Accent : ThemeColor::TextMuted));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(wasLocked
            ? "Uniform scale: ON\nDragging one axis keeps the others in proportion"
            : "Uniform scale: OFF\nEach axis is edited independently");
    if (lockClicked) uniform = !wasLocked;

    ImGui::SameLine(0.0f, style.ItemSpacing.x);

    const math::Vector3 before = scale;
    float arr[3] = { scale.x, scale.y, scale.z };
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.0f,
                                    total - lockW - style.ItemSpacing.x));
    const bool edited = DragAxes("##xyz", arr, 3, speed);
    ImGui::PopID();

    if (!edited) return false;
    scale = { arr[0], arr[1], arr[2] };

    // リンク中は「動かした成分の変化率」を他成分へ掛けて比率を保つ。
    // WHY: 差分を足す方式だと 0.1 と 10 が混ざったスケールで形が崩れる。比率なら形を保てる。
    //      before が 0 の成分は比率を作れないため、その場合は等値コピーで代用する。
    if (wasLocked) {
        int movedAxis = -1;
        const float beforeArr[3] = { before.x, before.y, before.z };
        for (int i = 0; i < 3; ++i)
            if (arr[i] != beforeArr[i]) { movedAxis = i; break; }

        if (movedAxis >= 0) {
            const float from = beforeArr[movedAxis];
            const float to   = arr[movedAxis];
            float out[3] = { arr[0], arr[1], arr[2] };
            if (std::abs(from) > 1e-6f) {
                const float ratio = to / from;
                for (int i = 0; i < 3; ++i)
                    if (i != movedAxis) out[i] = beforeArr[i] * ratio;
            } else {
                for (int i = 0; i < 3; ++i) out[i] = to;
            }
            scale = { out[0], out[1], out[2] };
        }
    }
    return true;
}

void LabelEllipsis(const char* text, float maxWidth)
{
    if (!text) return;
    ImGui::AlignTextToFramePadding();

    if (maxWidth <= 0.0f || ImGui::CalcTextSize(text).x <= maxWidth) {
        ImGui::TextUnformatted(text);
        return;
    }

    // 収まる文字数まで縮めて "..." を足す。全文はツールチップで補う。
    const float ellipsisW = ImGui::CalcTextSize("...").x;
    const std::size_t length = std::strlen(text);
    std::size_t fit = 0;
    for (std::size_t i = 1; i <= length; ++i) {
        if (ImGui::CalcTextSize(text, text + i).x + ellipsisW > maxWidth) break;
        fit = i;
    }
    std::string shortened(text, fit);
    shortened += "...";
    ImGui::TextUnformatted(shortened.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text);
}

PropertyRowScope BeginPropertyRow()
{
    PropertyRowScope row;
    row.key = ImGui::GetID("##fbzz_property_row");
    row.top = ImGui::GetCursorScreenPos().y;

    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetStateStorage()->GetFloat(row.key, ImGui::GetFrameHeight());

    // 行の横幅は「現在のインデント位置から利用可能幅いっぱい」。ウィンドウ基準ではなく
    // カーソル基準にしておくと、入れ子グループの行がインデントに合わせて内側に寄る。
    // 左右に少しだけはみ出させて余白ごと塗る。
    const float x0 = ImGui::GetCursorScreenPos().x - style.ItemInnerSpacing.x;
    const float x1 = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x
                   + style.ItemInnerSpacing.x;
    const ImVec2 lo{ x0, row.top - style.ItemSpacing.y * 0.5f };
    const ImVec2 hi{ x1, row.top + height + style.ItemSpacing.y * 0.5f };

    // ホバー判定は矩形の内外だけで行う。中身のウィジェットが ImGui のホバーを
    // 奪ってしまうため、行全体を対象にするには自前で当たり判定する必要がある。
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                         mouse.x >= lo.x && mouse.x < hi.x &&
                         mouse.y >= lo.y && mouse.y < hi.y;

    if (hovered) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            lo, hi, EditorTheme::ColorU32(ThemeColor::SurfaceHover, 0.75f), style.FrameRounding);
    }

    return row;
}

void EndPropertyRow(const PropertyRowScope& row)
{
    if (row.key == 0) return;
    // 中身を描き終えた時点のカーソルから実測高さを求め、次フレームの背景に使う。
    // ImGui は行末で改行済みなので ItemSpacing.y を引いて中身ぶんだけを取り出す。
    const float bottom = ImGui::GetCursorScreenPos().y;
    const float height = std::max(ImGui::GetFrameHeight(),
                                 bottom - row.top - ImGui::GetStyle().ItemSpacing.y);
    ImGui::GetStateStorage()->SetFloat(row.key, height);
}

PropertyRowScope BeginPropertyField(const char* label)
{
    ImGui::PushID(label);
    const PropertyRowScope row = BeginPropertyRow();

    const float column = PropertyLabelColumnWidth();
    LabelEllipsis(label, column - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SameLine();
    if (ImGui::GetCursorPosX() < column)
        ImGui::SetCursorPosX(column);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return row;
}

void EndPropertyField(const PropertyRowScope& row)
{
    EndPropertyRow(row);
    ImGui::PopID();
}

namespace {

// ◎ (一覧から選ぶ) と × (クリア) のグリフ。
// WHY 自前で描くか: このエディタの ImGui フォントは ASCII 中心で、
//     従来は "O" と "x" の文字を代用していた。文字は行によって太さも中心もばらつき、
//     何のボタンなのかがアイコンとして読めていなかった。
void DrawPickGlyph(ImDrawList* drawList, ImVec2 center, float size, ImU32 color)
{
    drawList->AddCircle(center, size * 0.30f, color, 16,
                        (std::max)(1.0f, size * 0.085f));
    drawList->AddCircleFilled(center, size * 0.11f, color, 12);
}

void DrawClearGlyph(ImDrawList* drawList, ImVec2 center, float size, ImU32 color)
{
    const float reach     = size * 0.22f;
    const float thickness = (std::max)(1.0f, size * 0.10f);
    drawList->AddLine(ImVec2{ center.x - reach, center.y - reach },
                      ImVec2{ center.x + reach, center.y + reach }, color, thickness);
    drawList->AddLine(ImVec2{ center.x - reach, center.y + reach },
                      ImVec2{ center.x + reach, center.y - reach }, color, thickness);
}

// ▲ / ▼ (1 つ上へ / 1 つ下へ)。三角の塗りではなく山形の 2 本線にする。
// WHY: 塗り三角は小さい寸法だとアンチエイリアスで潰れて「点」に見える。
//      線なら太さを font size に比例させられ、UI スケールを変えても形が残る。
void DrawChevronGlyph(ImDrawList* drawList, ImVec2 center, float size, ImU32 color, bool up)
{
    const float halfW     = size * 0.20f;
    const float halfH     = size * 0.10f;
    const float thickness = (std::max)(1.0f, size * 0.10f);
    const float dir       = up ? -1.0f : 1.0f;
    const ImVec2 apex{ center.x, center.y + halfH * dir };
    drawList->AddLine(ImVec2{ center.x - halfW, center.y - halfH * dir }, apex, color, thickness);
    drawList->AddLine(ImVec2{ center.x + halfW, center.y - halfH * dir }, apex, color, thickness);
}

// + (要素を足す)。
void DrawPlusGlyph(ImDrawList* drawList, ImVec2 center, float size, ImU32 color)
{
    const float reach     = size * 0.24f;
    const float thickness = (std::max)(1.0f, size * 0.10f);
    drawList->AddLine(ImVec2{ center.x - reach, center.y },
                      ImVec2{ center.x + reach, center.y }, color, thickness);
    drawList->AddLine(ImVec2{ center.x, center.y - reach },
                      ImVec2{ center.x, center.y + reach }, color, thickness);
}

// ⠿ (ドラッグつまみ)。2 列 3 段の点で「掴んで動かせる」ことを示す慣用表現。
void DrawGripGlyph(ImDrawList* drawList, ImVec2 center, float size, ImU32 color)
{
    const float stepY  = (std::max)(2.0f, size * 0.17f);
    const float stepX  = (std::max)(2.0f, size * 0.13f);
    const float radius = (std::max)(1.0f, size * 0.055f);
    for (int row = -1; row <= 1; ++row)
        for (int col = -1; col <= 1; col += 2)
            drawList->AddCircleFilled(
                { center.x + stepX * static_cast<float>(col),
                  center.y + stepY * static_cast<float>(row) },
                radius, color, 6);
}

// リスト行の小さなグリフボタン共通処理。
// 参照スロットの ◎ / × と同じく「透明ボタン + 自前グリフ + ホバーで明色化」で組む。
enum class ListGlyph { Up, Down, Remove, Add, Grip };

bool ListGlyphButton(const char* id, ListGlyph glyph, bool enabled, const char* tooltip)
{
    const float size = ImGui::GetFrameHeight();
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    const bool pressed = ImGui::Button(id, { size, size });
    ImGui::PopStyleColor();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);

    const ImVec2 center{ (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                         (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f };
    // 無効なボタンは沈めた色にして、押せる / 押せないを色で先に伝える。
    const ImU32 color = EditorTheme::ColorU32(
        !enabled ? ThemeColor::TextFaint : (hovered ? ThemeColor::Text : ThemeColor::TextMuted));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    switch (glyph) {
    case ListGlyph::Up:     DrawChevronGlyph(drawList, center, size, color, true);  break;
    case ListGlyph::Down:   DrawChevronGlyph(drawList, center, size, color, false); break;
    case ListGlyph::Remove: DrawClearGlyph(drawList, center, size, color);          break;
    case ListGlyph::Add:    DrawPlusGlyph(drawList, center, size, color);           break;
    case ListGlyph::Grip:   DrawGripGlyph(drawList, center, size, color);           break;
    }
    ImGui::EndDisabled();

    if (hovered && tooltip && tooltip[0]) ImGui::SetTooltip("%s", tooltip);
    return pressed && enabled;
}

// 並び替えペイロード。listId を載せることで、同時に開いている別のリストへは落とせない。
// WHY 文字列 scope ではなく ID か: 配列フィールドは名前が同じでも別 Script / 別要素に
//     いくらでも生えるため、一意な名前を人が付けられない。ImGui の ID 木をそのまま使う。
struct ListReorderPayload {
    ImGuiID listId = 0;
    int     index  = -1;
};

} // namespace

float ListRowToolbarWidth(bool removable)
{
    const int count = removable ? 3 : 2;
    return (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x)
         * static_cast<float>(count);
}

ListRowButtons ListRowToolbar(int index, int count, bool removable)
{
    ListRowButtons result;
    const ImGuiStyle& style = ImGui::GetStyle();

    ImGui::SameLine(0.0f, style.ItemSpacing.x);
    result.moveUp = ListGlyphButton("##up", ListGlyph::Up, index > 0, "Move up");
    ImGui::SameLine(0.0f, style.ItemSpacing.x);
    result.moveDown = ListGlyphButton("##down", ListGlyph::Down, index + 1 < count, "Move down");
    if (removable) {
        ImGui::SameLine(0.0f, style.ItemSpacing.x);
        result.remove = ListGlyphButton("##remove", ListGlyph::Remove, true, "Remove this item");
    }
    return result;
}

ImGuiID ListScopeId()
{
    return ImGui::GetID("##fbzz_list_scope");
}

int ListRowDragHandle(ImGuiID listId, int index, bool& outInsertAfter)
{
    outInsertAfter = false;

    // つまみ自体はボタンとして描く (押しても何も起きないが、ホバー地色で掴めることが伝わる)。
    ListGlyphButton("##grip", ListGlyph::Grip, true, "Drag to reorder");
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();

    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        const ListReorderPayload payload{ listId, index };
        ImGui::SetDragDropPayload("FBZZ_LIST_ROW", &payload, sizeof(payload));
        ImGui::Text("Move item %d", index);
        ImGui::EndDragDropSource();
    }

    int draggedIndex = -1;
    if (ImGui::BeginDragDropTarget()) {
        // Component カードの並び替えと同じ規則:
        //   Preview 中はガイド線だけを描き、実際の並び替えは Delivery の 1 回だけ返す。
        //   前後どちらへ挿すかはマウス位置で決め、線もその辺へ描いて結果と一致させる。
        constexpr ImGuiDragDropFlags kAcceptFlags =
            ImGuiDragDropFlags_AcceptBeforeDelivery
            | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("FBZZ_LIST_ROW", kAcceptFlags)) {
            if (payload->Data && payload->DataSize == sizeof(ListReorderPayload)) {
                ListReorderPayload dragged{};
                std::memcpy(&dragged, payload->Data, sizeof(dragged));
                // 別のリストからのドラッグは受け付けない。
                if (dragged.listId == listId && dragged.index != index) {
                    const float mid = (rowMin.y + rowMax.y) * 0.5f;
                    const bool insertAfter = ImGui::GetMousePos().y >= mid;
                    const float lineY = insertAfter ? rowMax.y : rowMin.y;
                    // ガイド線は行の全幅へ引く (つまみの幅だけだと線が短すぎて見落とす)。
                    const float right = ImGui::GetWindowPos().x
                                      + ImGui::GetWindowContentRegionMax().x;
                    ImGui::GetWindowDrawList()->AddLine(
                        { rowMin.x, lineY }, { right, lineY },
                        EditorTheme::ColorU32(ThemeColor::Accent), 2.0f);
                    if (payload->IsDelivery()) {
                        draggedIndex   = dragged.index;
                        outInsertAfter = insertAfter;
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    return draggedIndex;
}

bool ListAddButton(std::size_t count, bool addable)
{
    ImGui::TextDisabled("%zu item%s", count, count == 1 ? "" : "s");
    if (!addable) return false;
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
    return ListGlyphButton("##add", ListGlyph::Add, true, "Add an item");
}

bool ListRemoveButton()
{
    return ListGlyphButton("##remove", ListGlyph::Remove, true, "Remove this item");
}

RightReserveScope BeginRightReserve(float reserve)
{
    RightReserveScope scope;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window || reserve <= 0.0f) return scope;

    scope.previousWorkRight = window->WorkRect.Max.x;
    scope.active = true;
    // 詰めすぎて幅が消えると値ウィジェットが潰れるので、最低限の幅は残す。
    const float minimum = window->WorkRect.Min.x + ImGui::GetFontSize() * 4.0f;
    window->WorkRect.Max.x = (std::max)(minimum, scope.previousWorkRight - reserve);
    return scope;
}

void EndRightReserve(const RightReserveScope& scope)
{
    if (!scope.active) return;
    if (ImGuiWindow* window = ImGui::GetCurrentWindow())
        window->WorkRect.Max.x = scope.previousWorkRight;
}

bool BeginReferenceSlot(const char* id, const char* text, ReferenceSlotState state,
                        bool dropActive, int trailing)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float buttonSize = height;

    float width = ImGui::GetContentRegionAvail().x
                - (buttonSize + style.ItemSpacing.x) * static_cast<float>((std::max)(0, trailing));
    width = (std::max)(width, ImGui::GetFontSize() * 3.0f);

    const ImVec2 boxMin = ImGui::GetCursorScreenPos();
    const ImVec2 boxMax = { boxMin.x + width, boxMin.y + height };

    ImGui::PushID(id);
    ImGui::InvisibleButton("##slot", { width, height });
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(boxMin, boxMax,
        ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
        style.FrameRounding);

    // 枠の色で状態を出す。ドラッグ中は「ここへ落とせる」を最優先で見せたいので
    // 型不一致より前に判定する (落とす前の話なので、今の中身の是非は二の次)。
    ImU32 border    = EditorTheme::ColorU32(ThemeColor::Border);
    float thickness = 1.0f;
    if (state == ReferenceSlotState::Invalid) {
        border    = EditorTheme::ColorU32(ThemeColor::Danger);
        thickness = 2.0f;
    }
    if (dropActive) {
        border    = EditorTheme::ColorU32(ThemeColor::Accent);
        thickness = 2.0f;
    }
    drawList->AddRect(boxMin, boxMax, border, style.FrameRounding, 0, thickness);

    // 左端の状態ドット。空 = 中抜き、割り当て済み = 塗り、型違い = 赤塗り。
    const float  dotRadius = height * 0.16f;
    const ImVec2 dotCenter = { boxMin.x + style.FramePadding.x + dotRadius,
                               (boxMin.y + boxMax.y) * 0.5f };
    const ImU32 dotColor =
        state == ReferenceSlotState::Assigned ? EditorTheme::ColorU32(ThemeColor::Accent)
      : state == ReferenceSlotState::Invalid  ? EditorTheme::ColorU32(ThemeColor::Danger)
                                              : EditorTheme::ColorU32(ThemeColor::TextFaint);
    if (state == ReferenceSlotState::Empty)
        drawList->AddCircle(dotCenter, dotRadius, dotColor, 12, 1.4f);
    else
        drawList->AddCircleFilled(dotCenter, dotRadius, dotColor, 12);

    // 本文。あふれたら先頭を省略して、名前の末尾 (識別に効く側) を残す。
    const float textLeft = dotCenter.x + dotRadius + style.ItemInnerSpacing.x;
    const float availW   = (std::max)(8.0f, boxMax.x - style.FramePadding.x - textLeft);
    std::string shown    = text ? text : "";
    bool truncated = false;
    while (ImGui::CalcTextSize(shown.c_str()).x > availW && shown.size() > 1) {
        shown.erase(0, 1);
        truncated = true;
    }
    if (truncated) shown = "\xE2\x80\xA6" + shown;

    drawList->AddText(ImVec2{ textLeft, boxMin.y + (height - ImGui::GetTextLineHeight()) * 0.5f },
                      EditorTheme::ColorU32(state == ReferenceSlotState::Empty
                                                ? ThemeColor::TextFaint : ThemeColor::Text),
                      shown.c_str());
    return clicked;
}

ReferenceSlotButtons EndReferenceSlot(bool showPick, bool showClear)
{
    ReferenceSlotButtons result;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonSize = ImGui::GetFrameHeight();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const auto glyphButton = [&](const char* id, bool isPick, const char* tip) {
        ImGui::SameLine(0.0f, style.ItemSpacing.x);
        const bool pressed = ImGui::Button(id, ImVec2{ buttonSize, buttonSize });
        const ImVec2 center = {
            (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
            (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f
        };
        const bool hovered = ImGui::IsItemHovered();
        const ImU32 color = EditorTheme::ColorU32(hovered ? ThemeColor::Text
                                                          : ThemeColor::TextMuted);
        if (isPick) DrawPickGlyph(drawList, center, buttonSize, color);
        else        DrawClearGlyph(drawList, center, buttonSize, color);
        if (hovered) ImGui::SetTooltip("%s", tip);
        return pressed;
    };

    if (showPick)  result.pick  = glyphButton("##pick",  true,  "Pick from a list");
    if (showClear) result.clear = glyphButton("##clear", false, "Clear the reference");
    return result;
}

namespace {

// ComponentHeader が折り畳み状態を書き込む ImGuiID の集合。
// 1 セッション内で描かれたカードのぶんだけ溜まる (ラベルごとに 1 つなので数百程度)。
std::unordered_set<ImGuiID>& HeaderStateIdRegistry()
{
    static std::unordered_set<ImGuiID> ids;
    return ids;
}

} // namespace

const std::unordered_set<ImGuiID>& ComponentHeaderStateIds()
{
    return HeaderStateIdRegistry();
}

ComponentHeaderResult ComponentHeader(const char* label, ImU32 accent,
                                       bool* enabled, bool defaultOpen,
                                       const ComponentReorderTarget& reorder)
{
    ComponentHeaderResult result;
    const ImGuiStyle& style = ImGui::GetStyle();
    const CardMetrics metrics = Metrics();

    ImGui::PushID(label);

    // ヘッダーを描く前の行頭と右端を控える。
    // WHY: 枠付き CollapsingHeader の矩形は WindowPadding.x * 0.5 だけ左右へはみ出す
    //      (ImGui の TreeNodeBehavior が framed のとき outer_extend を足す) 一方、
    //      ImGui 自身のラベルは「はみ出す前のカーソル位置」を基準に置かれる。
    //      GetItemRect から重ね描きの座標を作ると、その差分だけ左へずれる。
    const ImVec2 startPos     = ImGui::GetCursorScreenPos();
    const float  contentRight = startPos.x + ImGui::GetContentRegionAvail().x;

    // 見出しだけ本文より 1 段大きい文字で組む。
    // WHY: コンポーネント名と行ラベル (Position など) が同じ大きさだと、どこがカードの
    //      切れ目なのかを色と位置だけで探すことになり、スクロール中に見出しが本文へ
    //      埋もれる。1 段上げるだけで「名前 > 値の名前」の主従が文字の大きさでも伝わる。
    // NOTE: PushFont には「グローバル倍率を掛ける前」の値 (style.FontSizeBase) を渡すこと。
    //       GetFontSize() は倍率適用後の値なので、渡すと UI スケールが二重に掛かる
    //       (ImGui 1.92 の仕様)。ここで押した文字サイズは矢印・チェック・⋯ にも効くため、
    //       ヘッダーの部品がまとめて一回り大きくなり、帯の高さもそれに追従する。
    //       サイズを整数へ丸めるのは、半端な値だとグリフのラスタライズがにじんで
    //       「大きくしたのに細くぼやけて見える」ため。
    constexpr float kTitleScale = 1.15f;
    ImGui::PushFont(nullptr, std::floor(style.FontSizeBase * kTitleScale));

    // 帯の高さは見出しの文字にちょうど寄り添うぶんだけにする (HeaderPadY)。
    // NOTE: PushFont 後に呼ぶこと。余白を見出しの文字サイズから作るため。
    // 色は CollapsingHeader の 3 状態を Surface 系へ寄せ、選択色 (青) を出さない。
    // WHY: 既定の Header 色はアクティブなアイテムと同じ扱いに見えるため、
    //      「開いているだけ」のカードが全部強調されて主従が消える。
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { style.FramePadding.x, HeaderPadY() });
    ImGui::PushStyleColor(ImGuiCol_Header,        EditorTheme::ColorU32(ThemeColor::SurfaceRaised));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::ColorU32(ThemeColor::SurfaceHover));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  EditorTheme::ColorU32(ThemeColor::SurfaceHover));

    // ラベルは "##" で伏せ、矢印だけを ImGui に描かせる。
    // WHY: チェックボックスと名前をヘッダーの帯の中へ収めたい。標準の並び
    //      (Checkbox → SameLine → CollapsingHeader) だとヘッダーがチェック分だけ
    //      右から始まり、カードの左端が毎行ギザギザになっていた。
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_AllowOverlap;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    // 折り畳み状態の保存先 ID を名乗っておく (EditorApp が永続化対象を絞るのに使う)。
    // CollapsingHeader は GetID("##hdr") をキーにウィンドウの StateStorage へ開閉を書く。
    HeaderStateIdRegistry().insert(ImGui::GetID("##hdr"));
    result.open = ImGui::CollapsingHeader("##hdr", flags);

    // ヘッダー自体をドラッグ元 / ドロップ先にする。
    // WHY ヘッダーだけか: メニューや有効チェックをドラッグ対象にすると、既存のクリック操作と
    //     競合する。Component の境界を表すヘッダーだけに並び替え操作を限定する。
    //
    // WHY 並び替え可能なカードだけを掴めるようにするか (不具合修正):
    //     以前は BeginDragDropSource だけが reorder の有無を見ておらず、全カードが
    //     "FBZZ_COMPONENT_ORDER" を撒いていた。結果、Transform やスクリプトカード、
    //     Post Process のカードまで掴めるのに落としても何も起きない (掴める = 動かせる、
    //     という UI の約束が破れている) 状態になり、さらに Transform を Component カードへ
    //     落とすと「描画順のリストに存在しないキー」が保存データへ紛れ込んでいた。
    if (reorder) {
        // ペイロード名はリスト (scope) ごとに分ける。ImGui のペイロード名は
        // 32 バイト上限なので、prefix 13 文字 + scope は 18 文字以内に収めること。
        char payloadType[32];
        std::snprintf(payloadType, sizeof(payloadType), "FBZZ_REORDER_%s", reorder.scope);
        const char* dragKey = reorder.dragKey ? reorder.dragKey : label;

        if (ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload(payloadType, dragKey, std::strlen(dragKey) + 1);
            ImGui::Text("Move %s", label);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            // Preview 中に順序を書き換えると、同じフレームの描画順と保存順がずれる。
            // ドロップラインだけを Preview で描き、順序変更は Delivery の 1 回だけ実行する。
            constexpr ImGuiDragDropFlags kAcceptFlags =
                ImGuiDragDropFlags_AcceptBeforeDelivery
                | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(payloadType, kAcceptFlags)) {
                const char* dragged = static_cast<const char*>(payload->Data);
                const bool valid = dragged != nullptr
                                && payload->DataSize > 0
                                && std::memchr(dragged, '\0', payload->DataSize) != nullptr
                                && dragged[0] != '\0'
                                && std::strcmp(dragged, dragKey) != 0;
                if (valid) {
                    const ImVec2 dropMin = ImGui::GetItemRectMin();
                    const ImVec2 dropMax = ImGui::GetItemRectMax();

                    // カードの上半分なら手前へ、下半分なら後ろへ挿入する。
                    //
                    // WHY 修正したか: 以前は挿入位置が常に「対象の手前」で固定なのに、
                    //   ガイド線だけを下端に描いていた。(1) 線が指す位置と実際の着地が
                    //   1 枚ずれる、(2) どのカードへ落としても手前にしか入らないため
                    //   最後尾へは絶対に移動できず、末尾のカードが事実上固定される、
                    //   という 2 つが同時に起きていた。マウス位置で前後を決め、
                    //   ガイド線もその辺へ描くことで見た目と結果を一致させる。
                    const float mid = (dropMin.y + dropMax.y) * 0.5f;
                    const bool  insertAfter = ImGui::GetMousePos().y >= mid;
                    const float lineY = insertAfter ? dropMax.y : dropMin.y;
                    ImGui::GetWindowDrawList()->AddLine(
                        { dropMin.x, lineY }, { dropMax.x, lineY },
                        EditorTheme::ColorU32(ThemeColor::Accent), 2.0f);
                    if (payload->IsDelivery())
                        reorder.onDrop(dragged, insertAfter);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    result.rectMin = ImGui::GetItemRectMin();
    result.rectMax = ImGui::GetItemRectMax();
    // 右クリックでも ⋯ と同じメニューを開けるようにする (押しやすさのため)。
    const bool headerRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

    const float centerY = (result.rectMin.y + result.rectMax.y) * 0.5f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // 左端のカテゴリ帯。折り畳んでいても出して、系統を色で拾えるようにする。
    drawList->AddRectFilled(result.rectMin,
                            { result.rectMin.x + metrics.accent, result.rectMax.y },
                            accent, style.FrameRounding, ImDrawFlags_RoundCornersLeft);

    // 矢印のぶんだけ空けた位置から、チェックボックスと名前をヘッダーへ重ねる。
    // NOTE: 枠付きツリーノードのラベル開始 X は ImGui 内部で
    //       CursorPos.x + FontSize + FramePadding.x * 3 (矢印幅 + 余白)。
    //       ここを合わせないと矢印と文字が重なる / 不自然に離れる。
    float x = startPos.x + ImGui::GetFontSize() + style.FramePadding.x * 3.0f;

    // チェックの枠は「持たないコンポーネントでも」必ず確保する。
    // WHY: 以前はチェックの有無で名前の開始位置が変わり、Transform (チェック無し) だけ
    //      名前が左へ寄って、Inspector を縦に見たとき見出しの左端がぎざぎざになっていた。
    //      幅を固定すれば、どのカードでも名前が同じ 1 本の線から始まる。
    // NOTE: チェックの大きさは FramePadding 依存なので、帯と同じ薄い余白で描く。
    //       既定値のままだと帯より背が高くなり、上下がはみ出す。
    const float boxH = ImGui::GetFontSize() + HeaderPadY() * 2.0f;
    if (enabled) {
        ImGui::SameLine();
        ImGui::SetCursorScreenPos({ x, centerY - boxH * 0.5f });
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { style.FramePadding.x, HeaderPadY() });
        result.enabledChanged = ImGui::Checkbox("##en", enabled);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", *enabled ? "Enabled - uncheck to disable this component"
                                             : "Disabled - check to enable this component");
    }
    x += boxH + style.ItemInnerSpacing.x;

    // ⋯ ボタンのぶんを先に確保してから名前を描く。長い名前がボタンへ食い込むと
    // 「押せるはずのものが文字で潰れている」状態になるため、名前側を省略する。
    const float btn        = std::max(ImGui::GetFontSize(), boxH * 0.85f);
    const float labelAvail = std::max(ImGui::GetFontSize(),
                                      contentRight - btn - style.ItemInnerSpacing.x - x);

    const char* shownLabel = label;
    char clipped[96];
    if (ImGui::CalcTextSize(label).x > labelAvail) {
        const float dotsW  = ImGui::CalcTextSize("...").x;
        const std::size_t length = std::strlen(label);
        std::size_t fit = 0;
        for (std::size_t i = 1; i <= length && i < sizeof(clipped) - 4; ++i) {
            if (ImGui::CalcTextSize(label, label + i).x + dotsW > labelAvail) break;
            fit = i;
        }
        std::memcpy(clipped, label, fit);
        std::memcpy(clipped + fit, "...", 4);
        shownLabel = clipped;
    }

    // 無効なコンポーネントは名前を沈める。値まで読む前に「効いていない」と分かる。
    const bool dimmed = enabled && !*enabled;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos({ x, centerY - ImGui::GetTextLineHeight() * 0.5f });
    ImGui::PushStyleColor(ImGuiCol_Text,
        EditorTheme::Color(dimmed ? ThemeColor::TextFaint : ThemeColor::Text));
    ImGui::TextUnformatted(shownLabel);
    ImGui::PopStyleColor();
    if (shownLabel != label && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", label);

    // ⋯ メニュー。作業領域の右端へ寄せる (ヘッダー矩形の右端はパディング分はみ出すので使わない)。
    ImGui::SameLine();
    ImGui::SetCursorScreenPos({ contentRight - btn, centerY - btn * 0.5f });
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    const bool menuButtonClicked = ImGui::Button("##opt", { btn, btn });
    ImGui::PopStyleColor();
    const bool menuHovered = ImGui::IsItemHovered();

    // 3 点は自前で描く。ASCII 中心のフォントに縦三点のグリフが無いため。
    const ImVec2 dotCenter = { (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                               centerY };
    const ImU32 dotColor = EditorTheme::ColorU32(menuHovered ? ThemeColor::Text
                                                            : ThemeColor::TextMuted);
    const float dotStep   = std::max(3.0f, ImGui::GetFontSize() * 0.28f);
    const float dotRadius = std::max(1.0f, ImGui::GetFontSize() * 0.11f);
    for (int i = -1; i <= 1; ++i)
        drawList->AddCircleFilled({ dotCenter.x + dotStep * static_cast<float>(i), dotCenter.y },
                                  dotRadius, dotColor, 8);
    if (menuHovered)
        ImGui::SetTooltip("Component options (right-click the header works too)");

    result.menuClicked = menuButtonClicked || headerRightClicked;

    ImGui::PopFont(); // 見出し用に押した 1 段大きい文字サイズを戻す
    ImGui::PopID();
    return result;
}

namespace {

// カード地色の共通処理。前フレームの実測高さで矩形を先に敷き、字下げして返す。
// left / right はスクリーン座標。呼び出し側がヘッダーの実測矩形をそのまま渡すことで、
// ヘッダーと本文の左右端が必ず一致する。
ComponentBodyScope BeginCardCommon(const char* storageId, ImU32 accent,
                                   float left, float right, bool roundTopCorners)
{
    const CardMetrics metrics = Metrics();

    ComponentBodyScope scope;
    scope.key    = ImGui::GetID(storageId);
    scope.top    = ImGui::GetCursorScreenPos().y;
    scope.left   = left;
    scope.right  = right;
    scope.indent = metrics.indent;
    scope.active = true;

    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetStateStorage()->GetFloat(scope.key, ImGui::GetFrameHeight());

    // コンポーネント本文 (roundTopCorners == false) はヘッダーの直下へ密着させる。
    // ImGui はヘッダー描画後に ItemSpacing.y だけカーソルを送るので、その分だけ上へ戻す。
    const float topPad = roundTopCorners ? style.ItemSpacing.y * 0.5f : style.ItemSpacing.y;
    const ImVec2 lo{ left,  scope.top - topPad };
    const ImVec2 hi{ right, scope.top + height + style.ItemSpacing.y * 0.5f };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(lo, hi, EditorTheme::ColorU32(ThemeColor::Surface, 0.55f),
                            style.FrameRounding,
                            roundTopCorners ? ImDrawFlags_RoundCornersAll
                                            : ImDrawFlags_RoundCornersBottom);
    if (accent != 0) {
        // ヘッダーの帯をそのまま本文へ延長し、1 コンポーネントの縦の範囲を示す。
        // 太さは Metrics() 由来でヘッダーと同一。別々に決めると継ぎ目で段差になる。
        const ImU32 soft = (accent & 0x00FFFFFFu) | (70u << IM_COL32_A_SHIFT);
        drawList->AddRectFilled(lo, { lo.x + metrics.accent, hi.y }, soft,
                                style.FrameRounding, ImDrawFlags_RoundCornersBottomLeft);
    } else {
        drawList->AddRect(lo, hi, EditorTheme::ColorU32(ThemeColor::Border, 0.6f),
                          style.FrameRounding, ImDrawFlags_RoundCornersAll, 1.0f);
    }

    ImGui::Indent(scope.indent);
    return scope;
}

void EndCardCommon(const ComponentBodyScope& scope)
{
    if (!scope.active) return;
    ImGui::Unindent(scope.indent);
    // 中身を描き終えた時点のカーソルから実測高さを求め、次フレームの地色に使う。
    const float bottom = ImGui::GetCursorScreenPos().y;
    const float height = std::max(ImGui::GetFrameHeight(),
                                  bottom - scope.top - ImGui::GetStyle().ItemSpacing.y);
    ImGui::GetStateStorage()->SetFloat(scope.key, height);
}

} // namespace

ComponentBodyScope BeginComponentBody(const ComponentHeaderResult& header, ImU32 accent)
{
    return BeginCardCommon("##fbzz_comp_body", accent,
                           header.rectMin.x, header.rectMax.x, false);
}

void EndComponentBody(const ComponentBodyScope& body)
{
    EndCardCommon(body);
}

ComponentBodyScope BeginCard()
{
    // 単独カードはヘッダーを持たないので自前で矩形を作る。
    // 枠付き CollapsingHeader と同じだけ左右へはみ出させ、
    // 下に続くコンポーネントカードと左右端をそろえる (揃っていないと段差に見える)。
    const float extend = std::floor(ImGui::GetStyle().WindowPadding.x * 0.5f);
    const float left   = ImGui::GetCursorScreenPos().x - extend;
    const float right  = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x + extend;
    return BeginCardCommon("##fbzz_card", 0, left, right, true);
}

void EndCard(const ComponentBodyScope& card)
{
    EndCardCommon(card);
}

void SectionHeader(const char* label)
{
    // WHAT: 左のブランドライン、見出し、残り幅の細い罫線を一行で描く。
    // WHY: SeparatorText の標準表現だけでは情報階層が弱く、長い Inspector で区切りを見失うため。
    // NOTE: 直前の行と密着すると見出しが本文の続きに見えるので、上に一拍入れてから描く。
    ImGui::Spacing();

    // 帯の太さと余白はコンポーネントカードと同じ Metrics() から取る。
    // WHY: 見出しの区切りだけ px 直値だと、UI スケールを変えたときに
    //      カードの帯とここの帯で太さが食い違い、同じ役割の線に見えなくなる。
    const CardMetrics metrics = Metrics();
    const float gap = std::floor(ImGui::GetFontSize() * 0.55f);

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float lineHeight = ImGui::GetTextLineHeight();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        cursor,
        { cursor.x + metrics.accent, cursor.y + lineHeight },
        EditorTheme::ColorU32(ThemeColor::Accent),
        metrics.accent * 0.5f);

    ImGui::SetCursorScreenPos({ cursor.x + metrics.accent + gap, cursor.y });
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Text));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    const ImVec2 textMax = ImGui::GetItemRectMax();
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (textMax.x + gap * 2.0f < right) {
        const float y = cursor.y + lineHeight * 0.5f;
        drawList->AddLine(
            { textMax.x + gap * 2.0f, y },
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
