/// @file    FluidHandlers.cpp
/// @brief   .fluid レシピの編集と焼きジョブ (fluid.*)。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <Editor/Ai/JsonReflector.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/FluidBakeService.hpp>
#include <Editor/Util/FluidDocument.hpp>
#include <Editor/Util/SpriteSlicer.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace fbzz::editor::ai::bus {

using scene::GameObject;
using scene::EntityID;

namespace {

/// @name 流体 (.fluid)
/// @note レシピの読み書きは ReflectFluidRecipe 1 本に通す。キー名は .fluid (TOML) と同じなので、
/// @note fluid.get で読んだ形をそのまま fluid.set へ返せる。焼き・プレビューは FluidBakeService のジョブで受け付ける。
/// @note id を返すだけにし、バスの drain をベイクで止めない。

/// @brief プリセットの識別子テーブルの 1 行。
/// @note FluidPresetName は «Fire (loop)» のような表示名で、空白や括弧を含むため
/// @note AI が引数として書き写す識別子には向かない。
struct FluidPresetEntry {
    asset::FluidPreset preset;
    const char*        id;
};

constexpr FluidPresetEntry kFluidPresets[] = {
    { asset::FluidPreset::Smoke,       "Smoke" },
    { asset::FluidPreset::Fire,        "Fire" },
    { asset::FluidPreset::Explosion,   "Explosion" },
    { asset::FluidPreset::Steam,       "Steam" },
    { asset::FluidPreset::DustBurst,   "DustBurst" },
    { asset::FluidPreset::Ink,         "Ink" },
    { asset::FluidPreset::MagicWisp,   "MagicWisp" },
    { asset::FluidPreset::HeatHaze,    "HeatHaze" },
    { asset::FluidPreset::WaterSplash, "WaterSplash" },
    { asset::FluidPreset::WaterJet,    "WaterJet" },
    { asset::FluidPreset::BloodBurst,  "BloodBurst" },
    { asset::FluidPreset::LavaBlob,    "LavaBlob" },
    { asset::FluidPreset::PlasmaBurst, "PlasmaBurst" },
    { asset::FluidPreset::ArcHaze,     "ArcHaze" },
    { asset::FluidPreset::GroundRing,  "GroundRing" },
    { asset::FluidPreset::ColdMist,    "ColdMist" },
    { asset::FluidPreset::ChargeVortex,"ChargeVortex" },
    { asset::FluidPreset::EmberBurst,  "EmberBurst" },
    { asset::FluidPreset::SigilFlare,  "SigilFlare" },
};

bool FindFluidPreset(const std::string& requested, asset::FluidPreset& outPreset)
{
    const std::string wanted = LowerAscii(requested);
    for (const FluidPresetEntry& entry : kFluidPresets) {
        if (wanted == LowerAscii(entry.id) || wanted == LowerAscii(asset::FluidPresetName(entry.preset))) {
            outPreset = entry.preset;
            return true;
        }
    }
    return false;
}

std::string FluidPresetIdList()
{
    std::string list;
    for (const FluidPresetEntry& entry : kFluidPresets) {
        if (!list.empty()) list += ", ";
        list += entry.id;
    }
    return list;
}

/// @brief LoadFluidRecipe / SaveFluidRecipe / FluidBakeService が受け取る UTF-8 のパス文字列に変換する。
std::string FluidAbsolutePath(const std::filesystem::path& path)
{
    std::string text = util::FileSystem::PathToUtf8(path);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

/// @brief FluidBakeService が返す実パスを、要求と同じ projectRoot 相対へ戻す。
/// @return projectRoot の外なら実パスのまま。
std::string FluidProjectRelative(const editor::EditorContext& ctx, const std::string& absolute)
{
    namespace fs = std::filesystem;
    if (absolute.empty() || ctx.projectRoot.empty()) return absolute;
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
    const fs::path path = fs::weakly_canonical(util::FileSystem::PathFromUtf8(absolute), ec);
    const fs::path relative = fs::relative(path, root, ec);
    if (ec || relative.empty() || relative.begin()->generic_string() == "..") return absolute;
    return relative.generic_string();
}

bool ResolveFluidPath(const editor::EditorContext& ctx, const std::string& requested,
                      std::filesystem::path& outFile, std::string& outRelative, Outcome& err)
{
    if (ctx.projectRoot.empty()) {
        err = Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
        return false;
    }
    std::error_code ec;
    if (!ResolveProjectFile(ctx, requested, outFile, outRelative)
        || std::filesystem::is_directory(outFile, ec)) {
        err = Outcome::Err("BAD_PATH", "projectRoot 配下の .fluid を指定してください: " + requested);
        return false;
    }
    if (LowerAscii(outFile.extension().string()) != ".fluid") {
        err = Outcome::Err("BAD_PATH", ".fluid のパスを指定してください: " + requested);
        return false;
    }
    return true;
}

bool LoadFluidAt(const std::filesystem::path& file, const std::string& relative,
                 fluid::FluidRecipe& outRecipe, Outcome& err)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        err = Outcome::Err("FLUID_NOT_FOUND", ".fluid が見つかりません: " + relative);
        return false;
    }
    std::string reason;
    if (!asset::LoadFluidRecipe(FluidAbsolutePath(file), outRecipe, &reason)) {
        err = Outcome::Err("FLUID_READ_FAILED", ".fluid を読み取れません: " + relative
                           + (reason.empty() ? std::string{} : " (" + reason + ")"));
        return false;
    }
    return true;
}

JsonValue FluidRecipeToJson(fluid::FluidRecipe recipe)
{
    JsonReadReflector reader;
    asset::ReflectFluidRecipe(recipe, reader);
    return reader.Result();
}

/// @brief JsonWriteReflector が扱わない 2 つ (オブジェクト配列の要素数と enum のラベル指定) を
/// @note ドット区切りのパスで 1 箇所だけ書く。
/// @note パスの数え方は JsonWriteReflector と同じ (入れ子は名前、配列要素は添字) にして、
/// @note 同じキーが両方で同じ場所を指すようにする。
class FluidPathReflector final : public scene::IReflector {
public:
    enum class Mode { ResizeList, EnumLabel };

    FluidPathReflector(Mode mode, std::string target) : m_mode(mode), m_target(std::move(target)) {}

    void SetCount(std::size_t count) { m_count = count; }
    void SetLabel(std::string label) { m_label = std::move(label); }

    /// @brief 目標のパスがレシピに実在したか。
    /// @note Applied と違い、ラベル違いでも true になる。
    [[nodiscard]] bool Found() const { return m_found; }
    [[nodiscard]] bool Applied() const { return m_applied; }
    [[nodiscard]] const std::string& Error() const { return m_error; }

    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Quaternion&) override {}

    void BeginObject(const char* name) override { m_path.emplace_back(PersistentKey(name)); }
    void EndObject() override { Leave(); }

    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        m_path.emplace_back(PersistentKey(name));
        if (m_mode != Mode::ResizeList || Joined() != m_target) return count;
        m_found = true;
        m_applied = true;
        return m_count;
    }
    void BeginObjectElement(std::size_t index) override { m_path.push_back(std::to_string(index)); }
    void EndObjectElement() override { Leave(); }
    std::size_t EndObjectList() override
    {
        Leave();
        return NO_REMOVE;
    }

    void Enum(const char* name, int& v, std::span<const char* const> labels) override
    {
        if (m_mode != Mode::EnumLabel || m_found) return;
        std::string path = Joined();
        if (!path.empty()) path += ".";
        path += PersistentKey(name);
        if (path != m_target) return;
        m_found = true;
        const std::string wanted = LowerAscii(m_label);
        std::string accepted;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const std::string label = labels[i] != nullptr ? labels[i] : "";
            if (LowerAscii(label) == wanted) {
                v = static_cast<int>(i);
                m_applied = true;
                return;
            }
            if (!accepted.empty()) accepted += " | ";
            accepted += label;
        }
        m_error = "'" + m_target + "' は " + accepted + " のいずれか (または添字の数値) を指定してください";
    }

private:
    std::string Joined() const
    {
        std::string joined;
        for (const std::string& segment : m_path) {
            if (!joined.empty()) joined += ".";
            joined += segment;
        }
        return joined;
    }

    void Leave()
    {
        if (!m_path.empty()) m_path.pop_back();
    }

    Mode                     m_mode;
    std::string              m_target;
    std::size_t              m_count = 0;
    std::string              m_label;
    std::vector<std::string> m_path;
    bool                     m_found = false;
    bool                     m_applied = false;
    std::string              m_error;
};

struct FluidFieldReport {
    std::vector<std::string> applied;
    std::vector<std::string> unknown;
    std::vector<std::string> errors;
    std::vector<std::string> clamped;  ///< 上限で切り詰めた配列 ("source: 20 -> 16")。
};

/// @brief 部品リストの上限。
/// @note GPU の定数バッファに載る数と揃えてあり、超えて置くと CPU と GPU で絵が変わる。
std::size_t FluidListLimit(const std::string& path)
{
    if (path == "source") return static_cast<std::size_t>(fluid::kMaxFluidSources);
    if (path == "force") return static_cast<std::size_t>(fluid::kMaxFluidForces);
    if (path == "collider") return static_cast<std::size_t>(fluid::kMaxFluidColliders);
    if (path.ends_with(".motion.key")) return static_cast<std::size_t>(fluid::kMaxFluidMotionKeys);
    if (path.ends_with(".amount.key")) return static_cast<std::size_t>(fluid::kMaxFluidAmountKeys);
    return (std::numeric_limits<std::size_t>::max)();
}

JsonValue FluidStringArray(const std::vector<std::string>& items)
{
    JsonValue array = JsonValue::MakeArray();
    for (const std::string& item : items) array.Push(JsonValue(item));
    return array;
}

/// @brief fields の 1 項目をレシピへ書く。オブジェクトは入れ子として潜る。
/// @note オブジェクトの配列は «要素数をその長さにしてから各要素へ部分適用» する
/// @note 配列内で省いたキーは既存値のまま。
void ApplyFluidValue(fluid::FluidRecipe& recipe, const std::string& path,
                     const JsonValue& value, FluidFieldReport& report)
{
    if (value.IsObject()) {
        for (const auto& member : value.AsObject())
            ApplyFluidValue(recipe, path.empty() ? member.first : path + "." + member.first,
                            member.second, report);
        return;
    }
    if (path.empty()) return;

    if (value.IsArray()) {
        const auto& items = value.AsArray();
        const bool objectList = std::all_of(items.begin(), items.end(),
                                            [](const JsonValue& item) { return item.IsObject(); });
        if (objectList) {
            const std::size_t count = (std::min)(items.size(), FluidListLimit(path));
            FluidPathReflector resize(FluidPathReflector::Mode::ResizeList, path);
            resize.SetCount(count);
            asset::ReflectFluidRecipe(recipe, resize);
            if (resize.Found()) {
                report.applied.push_back(path);
                /// @note 上限は焼き側の都合で、AI が知らずに多めに並べることがある。全部拒否すると
/// @note 残りの変更まで捨てることになるため切り詰め、落とした分は clamped で返す。
                if (count < items.size())
                    report.clamped.push_back(path + ": " + std::to_string(items.size()) + " -> " + std::to_string(count));
                for (std::size_t i = 0; i < count; ++i)
                    ApplyFluidValue(recipe, path + "." + std::to_string(i), items[i], report);
                return;
            }
            /// @note 空配列はベクトル型の誤りとして JsonWriteReflector に型エラーを言わせる。
            if (!items.empty()) {
                report.unknown.push_back(path);
                return;
            }
        }
    }

    if (value.IsString()) {
        /// @note enum のラベルは TOML に書く名前と同じ ("smoke" / "3d")。fluid.get が添字で返した値を
/// @note TOML と同じ綴りでも書き戻せるようにする。
        FluidPathReflector label(FluidPathReflector::Mode::EnumLabel, path);
        label.SetLabel(value.AsString());
        asset::ReflectFluidRecipe(recipe, label);
        if (label.Found()) {
            if (label.Applied()) report.applied.push_back(path);
            else report.errors.push_back(label.Error());
            return;
        }
    }

    JsonWriteReflector writer(path, value);
    asset::ReflectFluidRecipe(recipe, writer);
    if (writer.Applied()) report.applied.push_back(path);
    else if (!writer.Error().empty()) report.errors.push_back(writer.Error());
    else report.unknown.push_back(path);
}

/// @brief fields をレシピへ適用する。1 つでも書けないキーがあれば何も書かない。
/// @param basePath 指定すると fields をその下 ("source.3" など) への部分指定として読む。
/// @note 一部だけ書いて成功にすると、書けなかったキーを AI が見落としたまま焼き直しに進み、
/// @note 何度焼いても狙いの絵にならない。
bool ApplyFluidFields(fluid::FluidRecipe& recipe, const JsonValue& fields,
                      std::vector<std::string>& outChanged, Outcome& err,
                      std::vector<std::string>* outClamped = nullptr,
                      const std::string& basePath = std::string{})

{
    FluidFieldReport report;
    fluid::FluidRecipe working = recipe;
    ApplyFluidValue(working, basePath, fields, report);

    const auto join = [](const std::vector<std::string>& items) {
        std::string text;
        for (const std::string& item : items) {
            if (!text.empty()) text += ", ";
            text += item;
        }
        return text;
    };
    if (!report.unknown.empty()) {
        err = Outcome::Err("UNKNOWN_FIELD", "未知のキー: " + join(report.unknown)
                           + " (fluid_schema で項目名を確認してください。何も書き込んでいません)");
        return false;
    }
    if (!report.errors.empty()) {
        err = Outcome::Err("TYPE_MISMATCH", join(report.errors) + " (何も書き込んでいません)");
        return false;
    }
    recipe = std::move(working);
    outChanged = std::move(report.applied);
    if (outClamped != nullptr) *outClamped = std::move(report.clamped);
    return true;
}

constexpr std::size_t kNoFluidSource = (std::numeric_limits<std::size_t>::max)();

struct FluidTextureCheck {
    std::size_t index = 0;
    bool        textureWritten = false;  ///< texture そのものを今回書いたか (shape だけ変えたなら false)。
};

/// @brief changed ("source.3.texture" / "source.3.shape") から、画像を確かめ直す発生源を拾う。
void CollectFluidTextureChecks(const std::vector<std::string>& changed, std::vector<FluidTextureCheck>& out)
{
    constexpr std::string_view kPrefix = "source.";
    for (const std::string& path : changed) {
        const std::string_view view(path);
        if (!view.starts_with(kPrefix)) continue;
        const std::size_t dot = view.find('.', kPrefix.size());
        if (dot == std::string_view::npos || dot == kPrefix.size()) continue;
        const std::string_view digits = view.substr(kPrefix.size(), dot - kPrefix.size());
        if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        const std::string_view key = view.substr(dot + 1);
        if (key != "texture" && key != "shape") continue;
        /// @note changed に載るのは実在した要素の添字だけなので、桁あふれは起きない (上限 kMaxFluidSources)。
        std::size_t index = 0;
        for (const char digit : digits) index = index * 10 + static_cast<std::size_t>(digit - '0');
        auto found = std::find_if(out.begin(), out.end(),
                                  [index](const FluidTextureCheck& check) { return check.index == index; });
        if (found == out.end()) found = out.insert(out.end(), FluidTextureCheck{ index, false });
        if (key == "texture") found->textureWritten = true;
    }
}

/// @brief texture 形の発生源の画像を確かめる。
/// @note projectRoot の外を新しく書く画像が見つからなければ BAD_PATH、既存画像が見つからなければ warnings に載せる。
/// @note 画像が無くても書けるのは、レシピを先に組み後から画像を描き起こす運用があるため。
/// @note 外を拒むのは今回書いたときだけ。既存の .fluid が実パスを持っていても shape の変更までは止めない。
bool CheckFluidTextureSources(const editor::EditorContext& ctx, const fluid::FluidRecipe& recipe,
                              const std::vector<std::string>& changed, std::vector<std::string>& outWarnings,
                              Outcome& err, std::size_t addedSource = kNoFluidSource)
{
    namespace fs = std::filesystem;
    std::vector<FluidTextureCheck> checks;
    CollectFluidTextureChecks(changed, checks);
    if (addedSource != kNoFluidSource
        && std::none_of(checks.begin(), checks.end(),
                        [addedSource](const FluidTextureCheck& check) { return check.index == addedSource; }))
        checks.push_back({ addedSource, false });

    for (const FluidTextureCheck& check : checks) {
        if (check.index >= recipe.sources.size()) continue;
        const fluid::FluidSource& source = recipe.sources[check.index];
        if (source.shape != fluid::FluidSourceShape::Texture) continue;
        const std::string key = "source." + std::to_string(check.index) + ".texture";
        if (source.texture.empty()) {
            outWarnings.push_back(key + " is empty (shape=texture は画像が無いと何も湧きません)");
            continue;
        }
        /// @note Sprite 参照 (`<画像>::sprite::<ID>`) は元画像を確かめ、コマの在処は別に見る。
/// @note 参照のまま存在チェックへ回すと、実在する画像まで «見つからない» になる。
        std::string imagePath;
        std::string spriteToken;
        const bool isSprite = asset::ParseSpriteReference(source.texture, imagePath, spriteToken);
        if (imagePath.starts_with("guid:")) {
            if (asset::AssetManager::ResolveAssetPath(imagePath).empty())
                outWarnings.push_back("texture not found: " + source.texture + " (" + key + ")");
            continue;
        }
        fs::path file;
        std::string relative;
        if (!ResolveProjectFile(ctx, imagePath, file, relative)) {
            if (check.textureWritten) {
                err = Outcome::Err("BAD_PATH", key + " は projectRoot 相対の画像パスで指定してください (例 \"Assets/Textures/Logo.png\"。何も書き込んでいません): "
                                   + source.texture);
                return false;
            }
            outWarnings.push_back("texture not found: " + source.texture + " (" + key + ")");
            continue;
        }
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            outWarnings.push_back("texture not found: " + relative + " (" + key + ")");
            continue;
        }
        /// @note 切れた Sprite 参照は «アトラス全面» ではなく «読めない» になる (LoadFluidSourceMask)。
/// @note 黙って板の形で湧くので、書いた側に見える形で言う。
        if (!isSprite) continue;
        const std::string image = file.generic_string();
        asset::TextureImportSettings settings;
        if (!asset::GetCachedTextureImportSettings(image, settings)) {
            outWarnings.push_back("sprite meta not found: " + relative
                                  + " (" + key + " — Texture Type を Sprite にしてください)");
        } else if (asset::FindSprite(settings, spriteToken) == nullptr
                   && !IsImplicitSingleSprite(image, spriteToken)) {
            outWarnings.push_back("sprite not found: " + spriteToken + " in " + relative
                                  + " (" + key + " — sprite_list で ID を確かめてください)");
        }
    }
    return true;
}

/// @brief 書き込み前の姿。Undo はこれへ戻す (無かったなら消す)。
struct FluidFileSnapshot {
    bool        existed = false;
    std::string text;
    bool        metaExisted = false;
};

FluidFileSnapshot CaptureFluidFile(const std::filesystem::path& file)
{
    FluidFileSnapshot snapshot;
    std::error_code ec;
    snapshot.existed = std::filesystem::is_regular_file(file, ec);
    if (snapshot.existed) (void)util::FileSystem::ReadText(file, snapshot.text);
    std::filesystem::path metaFile = file;
    metaFile += ".meta";
    snapshot.metaExisted = std::filesystem::exists(metaFile, ec);
    return snapshot;
}

void RestoreFluidFile(const std::filesystem::path& file, const FluidFileSnapshot& snapshot)
{
    if (snapshot.existed) {
        (void)util::FileSystem::WriteText(file, snapshot.text);
        return;
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
    /// @note 書いた直後に Asset Browser が .meta を発行する。本体だけ消すと持ち主の無い GUID が残る。
    if (!snapshot.metaExisted) {
        std::filesystem::path metaFile = file;
        metaFile += ".meta";
        std::filesystem::remove(metaFile, ec);
    }
}

/// @param written Execute が実際に書けたかを返す (nullptr 可)。
std::unique_ptr<ICommand> MakeFluidWriteCommand(editor::EditorContext& ctx, std::string label,
                                                const std::filesystem::path& file,
                                                const fluid::FluidRecipe& recipe,
                                                std::shared_ptr<bool> written)
{
    const FluidFileSnapshot before = CaptureFluidFile(file);
    editor::EditorContext* context = &ctx;
    return std::make_unique<LambdaCommand>(std::move(label),
        [context, file, recipe, written]() {
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            const bool ok = asset::SaveFluidRecipe(FluidAbsolutePath(file), recipe);
            if (written) *written = ok;
            if (ok) context->requestAssetBrowserRefresh = true;
        },
        [context, file, before]() {
            RestoreFluidFile(file, before);
            context->requestAssetBrowserRefresh = true;
        });
}

/// @brief Undo で戻せる .fluid の書き込みかどうか。
/// @note DoFluidCommand と editor.transaction (BuildCommand) の両方がこの判定を通す。
bool IsFluidAssetCommand(const std::string& type)
{
    return type == "fluid.create" || type == "fluid.set" || type == "fluid.addOperator"
        || type == "fluid.removeOperator" || type == "fluid.moveOperator";
}

/// @brief 部品リスト。name は ReflectFluidRecipe の配列キー、typeField は種類を決める enum のキー。
struct FluidOperatorList {
    std::string name;
    std::string typeField;
    std::size_t limit = 0;
    const char* noun = "";  ///< Undo の表示名 ("AI: Add Fluid Collider")。
};

bool ReadFluidOperatorList(const JsonValue& payload, FluidOperatorList& out, Outcome& err)
{
    const std::string list = LowerAscii(StringField(payload, "list"));
    if (list == "source") {
        out = { list, "shape", static_cast<std::size_t>(fluid::kMaxFluidSources), "Source" };
        return true;
    }
    if (list == "force") {
        out = { list, "type", static_cast<std::size_t>(fluid::kMaxFluidForces), "Force" };
        return true;
    }
    if (list == "collider") {
        out = { list, "shape", static_cast<std::size_t>(fluid::kMaxFluidColliders), "Collider" };
        return true;
    }
    err = Outcome::Err("BAD_ARG", "list は \"source\" (発生源) / \"force\" (力) / \"collider\" (障害物) のいずれかを指定してください: "
                       + StringField(payload, "list"));
    return false;
}

/// @brief 部品の配列は種類ごとに要素の型が違う。list の名前で 1 本選んで fn へ渡す。
/// @pre list は ReadFluidOperatorList を通った名前であること。
template <typename Fn>
auto WithFluidOperatorList(fluid::FluidRecipe& recipe, const std::string& list, Fn&& fn)
{
    if (list == "source") return fn(recipe.sources);
    if (list == "force") return fn(recipe.forces);
    return fn(recipe.colliders);
}

/// @brief 省略なら hasValue=false で成功。数値でない・整数でない・[0, maxInclusive] の外は BAD_ARG。
bool ReadFluidOperatorIndex(const JsonValue& payload, const char* key, std::size_t maxInclusive,
                            std::size_t& out, bool& hasValue, Outcome& err)
{
    const JsonValue* value = payload.Find(key);
    hasValue = value != nullptr && !value->IsNull();
    if (!hasValue) return true;
    const bool integral = value->IsNumber() && value->AsNumber() >= 0.0
        && std::floor(value->AsNumber()) == value->AsNumber()
        && value->AsNumber() <= static_cast<double>(maxInclusive);
    if (!integral) {
        err = Outcome::Err("BAD_ARG", std::string(key) + " は 0〜" + std::to_string(maxInclusive)
                           + " の整数で指定してください");
        return false;
    }
    out = static_cast<std::size_t>(value->AsNumber());
    return true;
}

/// @brief ReflectFluidRecipe の ReflectList (Inspector の並べ替え) と同じ «取り出して差し込む» 意味にそろえる。
template <typename T>
void MoveFluidOperator(std::vector<T>& items, std::size_t from, std::size_t to)
{
    if (from == to) return;
    T moved = std::move(items[from]);
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
}

/// @brief fluid.addOperator / removeOperator / moveOperator。レシピを直してから fluid.set と同じ書き込みにする。
std::unique_ptr<ICommand> BuildFluidOperatorCommand(editor::EditorContext& ctx, const std::string& type,
                                                    const JsonValue& payload, const std::filesystem::path& file,
                                                    const std::string& relative, Outcome& err,
                                                    JsonValue* detailSink, std::shared_ptr<bool> written)
{
    fluid::FluidRecipe recipe;
    if (!LoadFluidAt(file, relative, recipe, err)) return nullptr;
    FluidOperatorList list;
    if (!ReadFluidOperatorList(payload, list, err)) return nullptr;
    const std::size_t count = WithFluidOperatorList(recipe, list.name,
                                                    [](const auto& items) { return items.size(); });

    JsonValue detail = JsonValue::MakeObject();
    detail.Set("path", JsonValue(relative));
    detail.Set("list", JsonValue(list.name));
    std::string label;

    if (type == "fluid.addOperator") {
        if (count >= list.limit) {
            err = Outcome::Err("OPERATOR_LIMIT", list.name + " は " + std::to_string(list.limit)
                               + " 個までです (今 " + std::to_string(count) + " 個)。不要な部品を fluid_remove_operator で消してください");
            return nullptr;
        }
        std::size_t index = count;
        bool hasIndex = false;
        if (!ReadFluidOperatorIndex(payload, "index", count, index, hasIndex, err)) return nullptr;
        if (!hasIndex) index = count;
        WithFluidOperatorList(recipe, list.name, [index](auto& items) {
            using Item = typename std::decay_t<decltype(items)>::value_type;
            items.insert(items.begin() + static_cast<std::ptrdiff_t>(index), Item{});
        });
        const std::string elementPath = list.name + "." + std::to_string(index);

        if (const JsonValue* typeValue = payload.Find("type"); typeValue != nullptr && !typeValue->IsNull()) {
            if (!typeValue->IsString() && !typeValue->IsNumber()) {
                err = Outcome::Err("BAD_ARG", "type はラベル文字列 (fluid_schema の operators." + list.name + ".types) で指定してください");
                return nullptr;
            }
            FluidFieldReport report;
            ApplyFluidValue(recipe, elementPath + "." + list.typeField, *typeValue, report);
            if (!report.errors.empty() || report.applied.empty()) {
                err = Outcome::Err("BAD_ARG", "未知の type です"
                                   + (report.errors.empty() ? std::string{} : ": " + report.errors.front()));
                return nullptr;
            }
        }

        std::vector<std::string> changed;
        std::vector<std::string> clamped;
        if (const JsonValue* fields = payload.Find("fields"); fields != nullptr && !fields->IsNull()) {
            if (!fields->IsObject()) {
                err = Outcome::Err("BAD_ARG", "fields はオブジェクト (新しい部品 1 つぶんの部分指定) で指定してください");
                return nullptr;
            }
            if (!ApplyFluidFields(recipe, *fields, changed, err, &clamped, elementPath)) return nullptr;
        }
        std::vector<std::string> warnings;
        if (list.name == "source"
            && !CheckFluidTextureSources(ctx, recipe, changed, warnings, err, index)) return nullptr;
        detail.Set("index", JsonValue(static_cast<int>(index)));
        detail.Set("count", JsonValue(static_cast<int>(count + 1)));
        detail.Set("changed", FluidStringArray(changed));
        if (!clamped.empty()) detail.Set("clamped", FluidStringArray(clamped));
        if (!warnings.empty()) detail.Set("warnings", FluidStringArray(warnings));
        label = std::string("AI: Add Fluid ") + list.noun;
    } else if (type == "fluid.removeOperator") {
        if (count == 0) {
            err = Outcome::Err("BAD_ARG", list.name + " に部品がありません");
            return nullptr;
        }
        std::size_t index = 0;
        bool hasIndex = false;
        if (!ReadFluidOperatorIndex(payload, "index", count - 1, index, hasIndex, err)) return nullptr;
        if (!hasIndex) {
            err = Outcome::Err("BAD_ARG", "index (消す部品の添字) が必要です");
            return nullptr;
        }
        WithFluidOperatorList(recipe, list.name, [index](auto& items) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
        });
        detail.Set("removed", JsonValue(static_cast<int>(index)));
        detail.Set("count", JsonValue(static_cast<int>(count - 1)));
        label = std::string("AI: Remove Fluid ") + list.noun;
    } else {
        if (count == 0) {
            err = Outcome::Err("BAD_ARG", list.name + " に部品がありません");
            return nullptr;
        }
        std::size_t from = 0;
        std::size_t to = 0;
        bool hasFrom = false;
        bool hasTo = false;
        if (!ReadFluidOperatorIndex(payload, "from", count - 1, from, hasFrom, err)) return nullptr;
        if (!ReadFluidOperatorIndex(payload, "to", count - 1, to, hasTo, err)) return nullptr;
        if (!hasFrom || !hasTo) {
            err = Outcome::Err("BAD_ARG", "from と to (部品の添字) が必要です");
            return nullptr;
        }
        WithFluidOperatorList(recipe, list.name, [from, to](auto& items) { MoveFluidOperator(items, from, to); });
        detail.Set("from", JsonValue(static_cast<int>(from)));
        detail.Set("to", JsonValue(static_cast<int>(to)));
        label = std::string("AI: Move Fluid ") + list.noun;
    }

    if (detailSink != nullptr)
        for (const auto& member : detail.AsObject()) detailSink->Set(member.first, member.second);
    return MakeFluidWriteCommand(ctx, std::move(label), file, recipe, std::move(written));
}

/// @brief Fluid Editor が未保存で開いていると、ここで書いた内容は保存時に消えることを警告へ足す。
/// @note 黙って書けたことにするより消えうると伝える方が、AI が次の書き込みを諦めるか人に知らせられる。
void AppendFluidEditorWarning(const std::filesystem::path& file, std::vector<std::string>& warnings)
{
    if (!editor::FluidDocument::HasUnsavedChanges(FluidAbsolutePath(file))) return;
    warnings.emplace_back("この .fluid は Fluid Editor が未保存の変更を抱えたまま開いています。"
                          "人が保存すると今の書き込みは消えます (エディター側で Reload を選んでください)");
}

/// @brief fluid.create / fluid.set / 部品の増減。Undo で戻せるファイル書き込みだけを作る。
/// @note editor.transaction からも組めるよう BuildCommand からも呼ぶ。
std::unique_ptr<ICommand> BuildFluidAssetCommand(editor::EditorContext& ctx, const std::string& type,
                                                 const JsonValue& payload, Outcome& err,
                                                 JsonValue* detailSink, std::shared_ptr<bool> written)
{
    namespace fs = std::filesystem;
    fs::path file;
    std::string relative;
    if (!ResolveFluidPath(ctx, StringField(payload, "path"), file, relative, err)) return nullptr;

    if (type == "fluid.create") {
        std::string presetName = StringField(payload, "preset");
        if (presetName.empty()) presetName = "Smoke";
        asset::FluidPreset preset = asset::FluidPreset::Smoke;
        if (!FindFluidPreset(presetName, preset)) {
            err = Outcome::Err("UNKNOWN_PRESET", "未知のプリセットです: " + presetName
                               + " (" + FluidPresetIdList() + ")");
            return nullptr;
        }
        const JsonValue* overwriteValue = payload.Find("overwrite");
        const bool overwrite = overwriteValue != nullptr && overwriteValue->AsBool();
        std::error_code ec;
        const bool exists = fs::exists(file, ec);
        if (exists && !overwrite) {
            err = Outcome::Err("FLUID_EXISTS", "既にあります: " + relative + " (置き換えるなら overwrite=true)");
            return nullptr;
        }
        if (detailSink != nullptr) {
            detailSink->Set("path", JsonValue(relative));
            detailSink->Set("preset", JsonValue(presetName));
            detailSink->Set("overwrote", JsonValue(exists));
            std::vector<std::string> warnings;
            AppendFluidEditorWarning(file, warnings);
            if (!warnings.empty()) detailSink->Set("warnings", FluidStringArray(warnings));
        }
        return MakeFluidWriteCommand(ctx, "AI: Create Fluid", file, asset::MakeFluidPreset(preset),
                                     std::move(written));
    }

    if (type == "fluid.set") {
        fluid::FluidRecipe recipe;
        if (!LoadFluidAt(file, relative, recipe, err)) return nullptr;
        const JsonValue* fields = payload.Find("fields");
        if (fields == nullptr || !fields->IsObject() || fields->AsObject().empty()) {
            err = Outcome::Err("BAD_ARG", "fields に 1 つ以上の項目を持つオブジェクトが必要です");
            return nullptr;
        }
        std::vector<std::string> changed;
        std::vector<std::string> clamped;
        if (!ApplyFluidFields(recipe, *fields, changed, err, &clamped)) return nullptr;
        std::vector<std::string> warnings;
        if (!CheckFluidTextureSources(ctx, recipe, changed, warnings, err)) return nullptr;
        AppendFluidEditorWarning(file, warnings);
        if (detailSink != nullptr) {
            detailSink->Set("path", JsonValue(relative));
            detailSink->Set("changed", FluidStringArray(changed));
            if (!clamped.empty()) detailSink->Set("clamped", FluidStringArray(clamped));
            if (!warnings.empty()) detailSink->Set("warnings", FluidStringArray(warnings));
        }
        return MakeFluidWriteCommand(ctx, "AI: Set Fluid Fields", file, recipe, std::move(written));
    }

    if (type == "fluid.addOperator" || type == "fluid.removeOperator" || type == "fluid.moveOperator")
        return BuildFluidOperatorCommand(ctx, type, payload, file, relative, err, detailSink, std::move(written));

    err = Outcome::Err("UNKNOWN_COMMAND", "未対応の fluid コマンドです: " + type);
    return nullptr;
}

/// @brief UndoStack が無い文脈 (単体の VFX Editor 等) でもファイル操作そのものは成立させる。
void ExecuteFluidCommand(editor::EditorContext& ctx, std::unique_ptr<ICommand> command)
{
    if (ctx.undoStack != nullptr) ctx.undoStack->Execute(std::move(command));
    else command->Execute();
}

bool ReadFluidJobId(const JsonValue& payload, std::uint32_t& outId, Outcome& err)
{
    const JsonValue* value = payload.Find("job");
    if (value == nullptr || !value->IsNumber() || value->AsNumber() < 1.0
        || value->AsNumber() > 4294967295.0 || std::floor(value->AsNumber()) != value->AsNumber()) {
        err = Outcome::Err("BAD_ARG", "job (1 以上の整数) が必要です");
        return false;
    }
    outId = static_cast<std::uint32_t>(value->AsNumber());
    return true;
}

/// @brief createEffect の name はそのままファイル名になる。区切り文字を許すと dir の外へ書けてしまう。
bool IsFluidEffectName(const std::string& name)
{
    if (name.empty() || name.size() > 64 || name.front() == '.') return false;
    /// @note Windows は末尾の空白とドットを黙って落とすため、書いた名前と実ファイル名がずれる。
    if (name.back() == '.' || name.back() == ' ') return false;
    for (const char character : name) {
        if (static_cast<unsigned char>(character) < 0x20) return false;
        if (std::string_view("\\/:*?\"<>|").find(character) != std::string_view::npos) return false;
    }
    return true;
}

const char* FluidJobStateName(editor::FluidJobState state)
{
    switch (state) {
    case editor::FluidJobState::Queued:    return "queued";
    case editor::FluidJobState::Running:   return "running";
    case editor::FluidJobState::Encoding:  return "encoding";
    case editor::FluidJobState::Done:      return "done";
    case editor::FluidJobState::Failed:    return "failed";
    case editor::FluidJobState::Cancelled: return "cancelled";
    }
    return "failed";
}

Outcome FluidServiceUnavailable()
{
    return Outcome::Err("SERVICE_UNAVAILABLE", "流体の焼きサービスがありません (エディター本体でのみ使えます)");
}

JsonValue FluidCatalog(fluid::FluidRecipe recipe)
{
    JsonCatalogReflector catalog;
    asset::ReflectFluidRecipe(recipe, catalog);
    return catalog.Result();
}

const JsonValue* FindFluidCatalogField(const JsonValue* fields, std::string_view name)
{
    if (fields == nullptr || !fields->IsArray()) return nullptr;
    for (const JsonValue& field : fields->AsArray()) {
        const JsonValue* fieldName = field.Find("name");
        if (fieldName != nullptr && fieldName->IsString() && fieldName->AsString() == name) return &field;
    }
    return nullptr;
}

const JsonValue* FluidOperatorElementFields(const JsonValue& catalog, std::string_view list)
{
    const JsonValue* listField = FindFluidCatalogField(&catalog, list);
    return listField != nullptr ? listField->Find("elementFields") : nullptr;
}

/// @brief 部品 1 つぶんの «効いている» 項目名。
/// @note JsonCatalogReflector は FieldIf で隠れた項目も落とさず visible=false で載せる
/// @note 保存は種類によらず全項目で、どれが効くかは種類ごとの要素を反射しないと分からない。
/// @note 見え方は kind (気体 / 液体) でも変わりうるので、両方で見えた項目の和を返す。
JsonValue VisibleFluidOperatorFields(fluid::FluidRecipe recipe, std::string_view list)
{
    std::vector<std::string> names;
    for (const fluid::FluidKind kind : { fluid::FluidKind::Gas, fluid::FluidKind::Liquid }) {
        recipe.kind = kind;
        const JsonValue catalog = FluidCatalog(recipe);
        const JsonValue* elementFields = FluidOperatorElementFields(catalog, list);
        if (elementFields == nullptr || !elementFields->IsArray()) continue;
        for (const JsonValue& field : elementFields->AsArray()) {
            const JsonValue* name = field.Find("name");
            const JsonValue* visible = field.Find("visible");
            if (name == nullptr || !name->IsString()) continue;
            if (visible != nullptr && visible->IsBool() && !visible->AsBool()) continue;
            if (std::find(names.begin(), names.end(), name->AsString()) == names.end())
                names.push_back(name->AsString());
        }
    }
    return FluidStringArray(names);
}

/// @brief 種類の一覧は enum のラベル (= TOML の綴り) から採る。
/// @param makeElement makeElement(recipe, typeIndex) はその種類の要素を 1 つだけ持つレシピへ整える。
template <typename MakeElement>
JsonValue FluidOperatorSchema(const JsonValue& baseCatalog, std::string_view list, std::string_view typeField,
                              int limit, MakeElement makeElement)
{
    JsonValue types = JsonValue::MakeArray();
    JsonValue fields = JsonValue::MakeObject();
    const JsonValue* typeDescriptor = FindFluidCatalogField(FluidOperatorElementFields(baseCatalog, list), typeField);
    const JsonValue* labels = typeDescriptor != nullptr ? typeDescriptor->Find("enumLabels") : nullptr;
    if (labels != nullptr && labels->IsArray()) {
        int typeIndex = 0;
        for (const JsonValue& label : labels->AsArray()) {
            fluid::FluidRecipe recipe;
            makeElement(recipe, typeIndex++);
            if (!label.IsString()) continue;
            types.Push(label);
            fields.Set(label.AsString(), VisibleFluidOperatorFields(std::move(recipe), list));
        }
    }
    JsonValue section = JsonValue::MakeObject();
    section.Set("typeField", JsonValue(std::string(typeField)));
    section.Set("limit", JsonValue(limit));
    section.Set("types", std::move(types));
    section.Set("fields", std::move(fields));
    return section;
}

Outcome DoFluidSchema()
{
    fluid::FluidRecipe recipe;
    /// @note 配列のスキーマは先頭要素から採る (JsonCatalogReflector)。空のままだと要素の項目が 1 つも出ない。
/// @note motion.key / amount.key も配列なので、部品ごとに 1 キーを持たせる (障害物に amount は無い)。
    recipe.sources.emplace_back();
    recipe.sources.back().motion.keys.emplace_back();
    recipe.sources.back().amount.keys.emplace_back();
    recipe.forces.emplace_back();
    recipe.forces.back().motion.keys.emplace_back();
    recipe.forces.back().amount.keys.emplace_back();
    recipe.colliders.emplace_back();
    recipe.colliders.back().motion.keys.emplace_back();
    const JsonValue catalog = FluidCatalog(recipe);

    JsonValue operators = JsonValue::MakeObject();
    operators.Set("source", FluidOperatorSchema(catalog, "source", "shape", fluid::kMaxFluidSources,
        [](fluid::FluidRecipe& target, int typeIndex) {
            target.sources.emplace_back();
            target.sources.back().shape = static_cast<fluid::FluidSourceShape>(typeIndex);
        }));
    operators.Set("force", FluidOperatorSchema(catalog, "force", "type", fluid::kMaxFluidForces,
        [](fluid::FluidRecipe& target, int typeIndex) {
            target.forces.emplace_back();
            target.forces.back().type = static_cast<fluid::FluidForceType>(typeIndex);
        }));
    operators.Set("collider", FluidOperatorSchema(catalog, "collider", "shape", fluid::kMaxFluidColliders,
        [](fluid::FluidRecipe& target, int typeIndex) {
            target.colliders.emplace_back();
            target.colliders.back().shape = static_cast<fluid::FluidColliderShape>(typeIndex);
        }));
    JsonValue limits = JsonValue::MakeObject();
    limits.Set("source", JsonValue(fluid::kMaxFluidSources));
    limits.Set("force", JsonValue(fluid::kMaxFluidForces));
    limits.Set("collider", JsonValue(fluid::kMaxFluidColliders));
    limits.Set("motionKey", JsonValue(fluid::kMaxFluidMotionKeys));

    JsonValue presets = JsonValue::MakeArray();
    JsonValue presetLabels = JsonValue::MakeArray();
    for (const FluidPresetEntry& entry : kFluidPresets) {
        presets.Push(JsonValue(entry.id));
        presetLabels.Push(JsonValue(asset::FluidPresetName(entry.preset)));
    }
    JsonValue bakeModes = JsonValue::MakeArray();
    bakeModes.Push(JsonValue("2d"));
    bakeModes.Push(JsonValue("3d"));

    JsonValue result = JsonValue::MakeObject();
    result.Set("fields", catalog);
    result.Set("operators", std::move(operators));
    result.Set("limits", std::move(limits));
    result.Set("presets", std::move(presets));
    result.Set("presetLabels", std::move(presetLabels));
    result.Set("bakeModes", std::move(bakeModes));
    result.Set("hint", JsonValue(std::string(
        "流体は部品の組み合わせで作ります: source (発生源。気体は密度・温度・燃料を注ぎ、液体は粒子を撃ち出す。最大 16)、"
        "force (流れにかかる力。最大 8)、collider (障害物。流体が入り込めない形。最大 8) の 3 つのリスト。"
        "部品は fluid_add_operator で足し (type に形 / 力の種類のラベル)、fluid_remove_operator / fluid_move_operator で消す・並べ替えます。"
        "種類ごとに効く項目は operators.<source|force|collider>.fields.<種類> です (他の項目も保存はされますが、その種類では効きません)。"
        "fields の source / force / collider は既定の種類 (sphere / wind / sphere) で採った目録で、visible もその種類のものです。"
        "部品の motion.key は {time, offset} の配列 (最大 8、time の昇順) で、部品の中心を時間で動かします "
        "(motion.inherit_velocity で動きの速さを流れに足す)。"
        "collider の shape は sphere (size.x = 半径) / box (size = 各軸の半分。回転なし) / plane (center を通り direction を法線とする面。"
        "法線の反対側がすべて固体で、壁や斜めの床になる) / capsule (center を通り direction を軸とする線分に肉を付けた形。"
        "size.x = 半径、size.y = 芯の半分の長さで両端は半球。腕・脚・棒・パイプ) / cylinder (同じ size で両端が平らな柱)。"
        "動く collider (motion.key) は流体を押しのけます。"
        "friction は液体が表面を滑るときの減速、start_time / duration で居る時間を区切れます。"
        "床は collider ではなく今までどおり gas.floor / liquid.floor で別に持ちます。"
        "source の shape \"texture\" は画像の形に湧きます (文字・ロゴ・魔法陣): direction を法線とする板に texture "
        "(projectRoot 相対の画像パス。例 \"Assets/Textures/Logo.png\") を貼り、白く不透明なところほど強く注ぎます (輝度 × α)。"
        "size.x / size.y が板の半幅 / 半高さ、size.z が厚みの半分。画像が見つからなくても書き込みは通り、応答の warnings に載ります。"
        "source の shape \"capsule\" は direction を軸とする線分に肉を付けた形 (size.x = 半径、size.y = 芯の半分の長さ。両端は半球。"
        "腕・脚・棒から湧く煙)、\"cylinder\" は両端が平らな柱 (煙突・通気口から柱状に立ち上る煙。cone は広がり、ring は輪になる)。"
        "fluid_set の fields は fluid_get の recipe と同じ形です (部分指定可・省いたキーは既存値のまま)。"
        "入れ子は {\"gas\":{\"buoyancy\":2}} でも \"gas.buoyancy\" でも書けます。"
        "オブジェクト配列は配列ごと渡すと要素数がその長さになり (上限を超えた分は切り詰めて clamped で返す)、"
        "1 要素だけなら \"source.0.density\"。"
        "enum は添字の数値かラベル文字列 (大文字小文字は問わない。例 \"source.0.shape\": \"cone\" / \"force.0.type\": \"vortex\" / "
        "\"collider.0.shape\": \"plane\")、"
        "焼き方は \"bake.mode\": \"2d\" | \"3d\"。"
        "発生源ごとに色を変えるには render.use_albedo_ramp=true にし、render.albedo_ramp (4 点固定の "
        "{color: リニア RGB, position: 0〜1 の昇順} の配列) に色を並べて、各 source の color_key (0〜1) でどの色かを選びます "
        "(例 \"source.1.color_key\": 1)。気体は色が煙に乗って運ばれ、複数の発生源の煙が混ざると色も混ざります。"
        "液体は粒子ごとに撃ち出した発生源の色を持ちます (水と血を 1 枚に焼き分けられる)。"
        "use_albedo_ramp が false なら color_key は効かず、smoke_color / liquid_color の 1 色です。"
        "3d でもループ (output.loop) と歪み (render.shading=\"distortion\") を焼け、全プリセットが 3d で焼けます。"
        "bake.solver は \"auto\" (既定。GPU で解き、使えなければ CPU へ落ちる) / \"gpu\" (落とさず失敗させる。"
        "同じ .fluid から必ず同じ絵が欲しいとき) / \"cpu\" (常に CPU・96³ まで) の 3 つです。"
        "どれで解いたかは fluid_job_status の solverUsed に出ます。"
        "焼きとプレビューは同じ経路で解くので、fluid_preview の frame と fluid_bake のコマは同じ絵です。"
        "絵が変わったかどうかは fingerprint (16 桁) の一致で判定できます。")));
    return Outcome::Ok(std::move(result));
}

Outcome DoFluidGet(editor::EditorContext& ctx, const JsonValue& payload)
{
    std::filesystem::path file;
    std::string relative;
    Outcome err;
    if (!ResolveFluidPath(ctx, StringField(payload, "path"), file, relative, err)) return err;
    fluid::FluidRecipe recipe;
    if (!LoadFluidAt(file, relative, recipe, err)) return err;

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("bakeMode", JsonValue(recipe.bake.mode == fluid::FluidBakeMode::Volume3D ? "3d" : "2d"));
    result.Set("recipe", FluidRecipeToJson(recipe));
    return Outcome::Ok(std::move(result));
}

Outcome DoFluidJobStatus(editor::EditorContext& ctx, const JsonValue& payload)
{
    std::uint32_t id = 0;
    Outcome err;
    if (!ReadFluidJobId(payload, id, err)) return err;
    /// @note サービスが無ければジョブも存在し得ない。区別すると AI が «待てば出る» と誤読する。
    const editor::FluidJobStatus* status = ctx.fluidBake != nullptr ? ctx.fluidBake->Find(id) : nullptr;
    if (status == nullptr)
        return Outcome::Err("FLUID_JOB_NOT_FOUND", "ジョブが見つかりません: " + std::to_string(id)
                            + " (終わったジョブは直近 32 件だけ残ります)");

    JsonValue outputs = JsonValue::MakeArray();
    for (const std::string& output : status->outputs) outputs.Push(JsonValue(FluidProjectRelative(ctx, output)));

    JsonValue result = JsonValue::MakeObject();
    result.Set("job", JsonValue(static_cast<std::int64_t>(status->id)));
    result.Set("kind", JsonValue(status->kind == editor::FluidJobKind::Preview ? "preview" : "bake"));
    result.Set("state", JsonValue(FluidJobStateName(status->state)));
    result.Set("finished", JsonValue(status->Finished()));
    result.Set("progress", JsonValue(static_cast<double>(status->progress)));
    result.Set("fluidPath", JsonValue(FluidProjectRelative(ctx, status->fluidPath)));
    result.Set("message", JsonValue(status->message));
    result.Set("outputs", std::move(outputs));
    result.Set("materialPath", JsonValue(FluidProjectRelative(ctx, status->materialPath)));
    result.Set("vfxPath", JsonValue(FluidProjectRelative(ctx, status->vfxPath)));
    result.Set("previewPngPath", JsonValue(FluidProjectRelative(ctx, status->previewPngPath)));
    /// @note 指紋は «前と同じ絵か» を画像を見比べずに決めるための値。ソルバーとフォールバックの理由も返し、
/// @note «同じレシピなのに絵が違う» の原因 (GPU が使えず CPU で解かれた) を AI 側で切り分けられるようにする。
    if (!status->fingerprint.empty()) result.Set("fingerprint", JsonValue(status->fingerprint));
    if (!status->solverUsed.empty()) result.Set("solverUsed", JsonValue(status->solverUsed));
    if (!status->fallbackReason.empty()) result.Set("fallbackReason", JsonValue(status->fallbackReason));
    result.Set("seed", JsonValue(static_cast<std::int64_t>(status->seed)));
    if (status->kind == editor::FluidJobKind::Preview)
        result.Set("frame", JsonValue(static_cast<std::int64_t>(status->previewFrame)));

    const JsonValue* includeValue = payload.Find("includeImage");
    const bool includeImage = includeValue == nullptr || includeValue->AsBool(true);
    if (includeImage && status->kind == editor::FluidJobKind::Preview
        && status->state == editor::FluidJobState::Done && !status->previewPngPath.empty()) {
        std::vector<std::uint8_t> bytes;
        if (util::FileSystem::ReadBinary(util::FileSystem::PathFromUtf8(status->previewPngPath), bytes)
            && !bytes.empty() && bytes.size() <= 16u * 1024u * 1024u) {
            /// @note asset.thumbnail と同じ形。MCP 側はこれをそのまま image content へ移す。
            JsonValue image = JsonValue::MakeObject();
            image.Set("mimeType", JsonValue("image/png"));
            image.Set("base64", JsonValue(Base64Encode(bytes)));
            image.Set("path", JsonValue(FluidProjectRelative(ctx, status->previewPngPath)));
            result.Set("image", std::move(image));
        }
    }
    return Outcome::Ok(std::move(result));
}

/// @brief fluid.* コマンドの実体。create / set は Undo 可能。
/// @note 焼き系 (preview / bake / cancel と createEffect の焼き部分) は build.run と同じく
/// @note ジョブを受け付けるだけで Undo に載せない。
Outcome DoFluidCommand(editor::EditorContext& ctx, const std::string& type, const JsonValue& payload, bool dryRun)
{
    namespace fs = std::filesystem;
    const char* kPoll = "fluid_job_status";

    if (IsFluidAssetCommand(type)) {
        Outcome err;
        JsonValue detail = JsonValue::MakeObject();
        auto written = std::make_shared<bool>(false);
        std::unique_ptr<ICommand> command = BuildFluidAssetCommand(ctx, type, payload, err, &detail, written);
        if (command == nullptr) return err;
        if (dryRun) {
            Outcome preview = DryRunPreview(type);
            preview.result.Set("detail", std::move(detail));
            return preview;
        }
        ExecuteFluidCommand(ctx, std::move(command));
        if (!*written) return Outcome::Err("FLUID_WRITE_FAILED", ".fluid を書き込めません: " + StringField(payload, "path"));
        return Outcome::Ok(std::move(detail));
    }

    if (type == "fluid.preview" || type == "fluid.bake") {
        if (ctx.fluidBake == nullptr) return FluidServiceUnavailable();
        fs::path file;
        std::string relative;
        Outcome err;
        if (!ResolveFluidPath(ctx, StringField(payload, "path"), file, relative, err)) return err;
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) return Outcome::Err("FLUID_NOT_FOUND", ".fluid が見つかりません: " + relative);

        fs::path materialRelative(relative);
        materialRelative.replace_extension(".mat");
        if (dryRun) {
            Outcome preview = DryRunPreview(type);
            preview.result.Set("path", JsonValue(relative));
            return preview;
        }

        editor::FluidJobError jobError;
        std::uint32_t id = 0;
        if (type == "fluid.preview") {
            editor::FluidPreviewRequest request;
            request.fluidPath = FluidAbsolutePath(file);
            /// @note コマ番号が第一級。秒しか来なければ一番近いコマへ吸着させる (どのコマでもない絵を作らない)。
            if (const JsonValue* frame = payload.Find("frame"); frame != nullptr && frame->IsNumber()) {
                request.frame = std::clamp(frame->AsInt(), 0, 1023);
            } else if (const JsonValue* time = payload.Find("time"); time != nullptr && time->IsNumber()) {
                request.frame = -1;
                request.time = std::clamp(static_cast<float>(time->AsNumber()), 0.0f, 600.0f);
            }
            if (const JsonValue* size = payload.Find("size"); size != nullptr && size->IsNumber())
                request.size = std::clamp(size->AsInt(), 32, 2048);
            if (const JsonValue* sheet = payload.Find("contactSheet"); sheet != nullptr)
                request.contactSheet = sheet->AsBool(false);
            if (const JsonValue* variants = payload.Find("variants"); variants != nullptr && variants->IsNumber())
                request.variants = std::clamp(variants->AsInt(), 1, 16);
            if (const JsonValue* seed = payload.Find("seed"); seed != nullptr && seed->IsNumber())
                request.seed = static_cast<std::uint32_t>(std::clamp(seed->AsNumber(), 0.0, 4294967295.0));
            if (request.variants > 1) request.contactSheet = true;
            id = ctx.fluidBake->EnqueuePreview(ctx, request, jobError);
        } else {
            editor::FluidBakeRequest request;
            request.fluidPath = FluidAbsolutePath(file);
            if (const JsonValue* seed = payload.Find("seed"); seed != nullptr && seed->IsNumber())
                request.seed = static_cast<std::uint32_t>(std::clamp(seed->AsNumber(), 0.0, 4294967295.0));
            id = ctx.fluidBake->EnqueueBake(ctx, request, jobError);
        }
        if (id == 0)
            return Outcome::Err(jobError.code.empty() ? "FLUID_BAKE_FAILED" : jobError.code, jobError.message);

        JsonValue result = JsonValue::MakeObject();
        result.Set("job", JsonValue(static_cast<std::int64_t>(id)));
        result.Set("async", JsonValue(true));
        result.Set("poll", JsonValue(kPoll));
        result.Set("path", JsonValue(relative));
        if (type == "fluid.bake")
            result.Set("materialPath", JsonValue(materialRelative.generic_string()));
        return Outcome::Ok(std::move(result));
    }

    if (type == "fluid.cancel") {
        std::uint32_t id = 0;
        Outcome err;
        if (!ReadFluidJobId(payload, id, err)) return err;
        if (ctx.fluidBake == nullptr) return FluidServiceUnavailable();
        if (dryRun) return DryRunPreview(type);
        JsonValue result = JsonValue::MakeObject();
        result.Set("job", JsonValue(static_cast<std::int64_t>(id)));
        result.Set("cancelled", JsonValue(ctx.fluidBake->Cancel(id)));
        return Outcome::Ok(std::move(result));
    }

    if (type == "fluid.createEffect") {
        const std::string name = StringField(payload, "name");
        if (!IsFluidEffectName(name))
            return Outcome::Err("BAD_ARG", "name はファイル名 1 つ分 (64 文字以内・区切り文字と先頭の . は不可) で指定してください");
        std::string dir = StringField(payload, "dir");
        while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
        if (dir.empty()) dir = "Assets/VFX/Fluid";

        fs::path file;
        std::string relative;
        Outcome err;
        if (!ResolveFluidPath(ctx, dir + "/" + name + ".fluid", file, relative, err)) return err;
        std::error_code ec;
        if (fs::exists(file, ec)) return Outcome::Err("FLUID_EXISTS", "既にあります: " + relative);

        std::string presetName = StringField(payload, "preset");
        if (presetName.empty()) presetName = "Smoke";
        asset::FluidPreset preset = asset::FluidPreset::Smoke;
        if (!FindFluidPreset(presetName, preset))
            return Outcome::Err("UNKNOWN_PRESET", "未知のプリセットです: " + presetName + " (" + FluidPresetIdList() + ")");
        fluid::FluidRecipe recipe = asset::MakeFluidPreset(preset);
        std::vector<std::string> changed;
        std::vector<std::string> clamped;
        std::vector<std::string> warnings;
        if (const JsonValue* fields = payload.Find("fields"); fields != nullptr) {
            if (!fields->IsObject()) return Outcome::Err("BAD_ARG", "fields はオブジェクトで指定してください");
            if (!ApplyFluidFields(recipe, *fields, changed, err, &clamped)) return err;
            if (!CheckFluidTextureSources(ctx, recipe, changed, warnings, err)) return err;
        }

        const JsonValue* bakeValue = payload.Find("bake");
        const bool bake = bakeValue == nullptr || bakeValue->AsBool(true);
        /// @note 焼けないと分かっているのに .fluid だけ書くと、成功に見えて何も出ない。書く前に止める。
        if (bake && ctx.fluidBake == nullptr) return FluidServiceUnavailable();

        fs::path vfxFile = file;
        vfxFile.replace_extension(".vfx");
        fs::path vfxRelative(relative);
        vfxRelative.replace_extension(".vfx");
        fs::path materialRelative(relative);
        materialRelative.replace_extension(".mat");

        JsonValue result = JsonValue::MakeObject();
        result.Set("fluidPath", JsonValue(relative));
        result.Set("preset", JsonValue(presetName));
        result.Set("changed", FluidStringArray(changed));
        if (!clamped.empty()) result.Set("clamped", FluidStringArray(clamped));
        if (!warnings.empty()) result.Set("warnings", FluidStringArray(warnings));
        if (bake) {
            result.Set("materialPath", JsonValue(materialRelative.generic_string()));
            result.Set("vfxPath", JsonValue(vfxRelative.generic_string()));
        }
        if (dryRun) {
            Outcome preview = DryRunPreview(type);
            preview.result.Set("detail", std::move(result));
            return preview;
        }

        auto written = std::make_shared<bool>(false);
        ExecuteFluidCommand(ctx, MakeFluidWriteCommand(ctx, "AI: Create Fluid Effect", file, recipe, written));
        if (!*written) return Outcome::Err("FLUID_WRITE_FAILED", ".fluid を書き込めません: " + relative);
        if (!bake) return Outcome::Ok(std::move(result));

        editor::FluidBakeRequest request;
        request.fluidPath = FluidAbsolutePath(file);
        request.vfxPath = FluidAbsolutePath(vfxFile);
        request.vfxRootName = name;
        editor::FluidJobError jobError;
        const std::uint32_t id = ctx.fluidBake->EnqueueBake(ctx, request, jobError);
        if (id == 0) {
            /// @note .fluid は書けているので失敗にはしない。焼きだけを fluid_bake でやり直せる。
            JsonValue bakeError = JsonValue::MakeObject();
            bakeError.Set("code", JsonValue(jobError.code.empty() ? std::string("FLUID_BAKE_FAILED") : jobError.code));
            bakeError.Set("message", JsonValue(jobError.message));
            result.Set("bakeError", std::move(bakeError));
            return Outcome::Ok(std::move(result));
        }
        result.Set("job", JsonValue(static_cast<std::int64_t>(id)));
        result.Set("async", JsonValue(true));
        result.Set("poll", JsonValue(kPoll));
        return Outcome::Ok(std::move(result));
    }

    return Outcome::Err("UNKNOWN_COMMAND", "未対応の fluid コマンドです: " + type);
}
} // namespace

void RegisterFluidHandlers(BusHandlerTable& table)
{
    table.AddQuery("fluid.schema", [](BusCall&) { return DoFluidSchema(); });
    table.AddQuery("fluid.get", [](BusCall& call) { return DoFluidGet(call.ctx, call.payload); });
    table.AddQuery("fluid.jobStatus", [](BusCall& call) { return DoFluidJobStatus(call.ctx, call.payload); });

    const CommandFn command = [](BusCall& call) { return DoFluidCommand(call.ctx, call.type, call.payload, call.dryRun); };
    /// @note .fluid の作成・編集はファイル単体で完結し、transaction にも混ぜられる。
    const BuilderFn assetEdit = [](editor::EditorContext& ctx, const std::string& type, const JsonValue& payload,
                                   Outcome& err, std::shared_ptr<std::string>, JsonValue* detailSink) {
        return BuildFluidAssetCommand(ctx, type, payload, err, detailSink, nullptr);
    };
    for (const char* type : { "fluid.create", "fluid.set", "fluid.addOperator", "fluid.removeOperator", "fluid.moveOperator" })
        table.AddCommand(type, command, assetEdit);
    /// @note 焼き系はジョブを積むだけで Undo できないため transaction へ混ぜさせない。
    for (const char* type : { "fluid.preview", "fluid.bake", "fluid.cancel", "fluid.createEffect" })
        table.AddCommand(type, command);
}

} // namespace fbzz::editor::ai::bus
