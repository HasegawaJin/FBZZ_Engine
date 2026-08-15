// FBZZ Engine
// SaveData.cpp | fbzz::util
// TOML backed の KVS セーブデータ実装
#include <Engine/Util/SaveData.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Logger.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <sstream>

namespace fbzz::util {
namespace {

// メモリ上の正本。TOML テーブルをそのまま保持することで、
// 型ごとの variant を自前で持たずに済み、保存も oss << table で完結する。
toml::table& Table()
{
    static toml::table s_table;
    return s_table;
}

std::string& SlotPath()
{
    static std::string s_slotPath = "Saves/save0.toml";
    return s_slotPath;
}

bool g_dirty = false;

// ベクター型は double 配列で保存する。要素数で型を判別できるため、
// Vector2/3/4 を同じ読み出しヘルパーで扱える。
template<size_t N>
void WriteFloats(std::string_view key, const float (&values)[N])
{
    toml::array arr;
    for (size_t i = 0; i < N; ++i)
        arr.push_back(static_cast<double>(values[i]));
    Table().insert_or_assign(std::string(key), std::move(arr));
    g_dirty = true;
}

// 配列を最大 N 要素まで読み出す。欠損要素は out に触れない (既定値が残る)。
template<size_t N>
bool ReadFloats(std::string_view key, float (&out)[N])
{
    const auto it = Table().find(std::string(key));
    if (it == Table().end()) return false;
    const toml::array* arr = it->second.as_array();
    if (!arr) return false;

    const size_t count = std::min<size_t>(N, arr->size());
    for (size_t i = 0; i < count; ++i) {
        const auto value = (*arr)[i].value<double>();
        if (!value) return false;
        out[i] = static_cast<float>(*value);
    }
    return true;
}

} // namespace

void SaveData::SetSlotPath(const std::string& path)
{
    SlotPath() = path;
}

const std::string& SaveData::GetSlotPath()
{
    return SlotPath();
}

// ── 書き込み ────────────────────────────────────────────────────────────────

void SaveData::SetBool(std::string_view key, bool value)
{
    Table().insert_or_assign(std::string(key), value);
    g_dirty = true;
}

void SaveData::SetInt(std::string_view key, int value)
{
    Table().insert_or_assign(std::string(key), static_cast<int64_t>(value));
    g_dirty = true;
}

void SaveData::SetFloat(std::string_view key, float value)
{
    Table().insert_or_assign(std::string(key), static_cast<double>(value));
    g_dirty = true;
}

void SaveData::SetString(std::string_view key, std::string_view value)
{
    Table().insert_or_assign(std::string(key), std::string(value));
    g_dirty = true;
}

void SaveData::SetVector2(std::string_view key, const math::Vector2& value)
{
    const float values[2] = { value.x, value.y };
    WriteFloats(key, values);
}

void SaveData::SetVector3(std::string_view key, const math::Vector3& value)
{
    const float values[3] = { value.x, value.y, value.z };
    WriteFloats(key, values);
}

void SaveData::SetVector4(std::string_view key, const math::Vector4& value)
{
    const float values[4] = { value.x, value.y, value.z, value.w };
    WriteFloats(key, values);
}

// ── 読み出し ────────────────────────────────────────────────────────────────

bool SaveData::GetBool(std::string_view key, bool defaultValue)
{
    return Table()[std::string(key)].value_or(defaultValue);
}

int SaveData::GetInt(std::string_view key, int defaultValue)
{
    return static_cast<int>(
        Table()[std::string(key)].value_or(static_cast<int64_t>(defaultValue)));
}

float SaveData::GetFloat(std::string_view key, float defaultValue)
{
    // WHY int64 も見るか: TOML では 1.0 を書いても整数として読み戻る場合があり、
    //      SetFloat(1.0f) → GetFloat() が既定値に落ちる事故を防ぐ。
    const auto node = Table()[std::string(key)];
    if (const auto asDouble = node.value<double>()) return static_cast<float>(*asDouble);
    if (const auto asInt = node.value<int64_t>())   return static_cast<float>(*asInt);
    return defaultValue;
}

std::string SaveData::GetString(std::string_view key, std::string_view defaultValue)
{
    return Table()[std::string(key)].value_or(std::string(defaultValue));
}

math::Vector2 SaveData::GetVector2(std::string_view key, const math::Vector2& defaultValue)
{
    float values[2] = { defaultValue.x, defaultValue.y };
    if (!ReadFloats(key, values)) return defaultValue;
    return { values[0], values[1] };
}

math::Vector3 SaveData::GetVector3(std::string_view key, const math::Vector3& defaultValue)
{
    float values[3] = { defaultValue.x, defaultValue.y, defaultValue.z };
    if (!ReadFloats(key, values)) return defaultValue;
    return { values[0], values[1], values[2] };
}

math::Vector4 SaveData::GetVector4(std::string_view key, const math::Vector4& defaultValue)
{
    float values[4] = { defaultValue.x, defaultValue.y, defaultValue.z, defaultValue.w };
    if (!ReadFloats(key, values)) return defaultValue;
    return { values[0], values[1], values[2], values[3] };
}

// ── 管理 ────────────────────────────────────────────────────────────────────

bool SaveData::Has(std::string_view key)
{
    return Table().contains(std::string(key));
}

void SaveData::Remove(std::string_view key)
{
    if (Table().erase(std::string(key)) > 0)
        g_dirty = true;
}

void SaveData::Clear()
{
    if (Table().empty()) return;
    Table().clear();
    g_dirty = true;
}

std::vector<std::string> SaveData::Keys()
{
    std::vector<std::string> keys;
    keys.reserve(Table().size());
    for (const auto& [key, value] : Table())
        keys.emplace_back(key.str());
    std::sort(keys.begin(), keys.end());
    return keys;
}

// ── I/O ─────────────────────────────────────────────────────────────────────

std::string SaveData::ResolveAbsolutePath()
{
    const std::filesystem::path slot = FileSystem::PathFromUtf8(SlotPath());
    if (slot.is_absolute())
        return FileSystem::PathToUtf8(slot);
    // WHY 実行ファイル基準か: カレントディレクトリはエディタ / GameHub / スタンドアロンで
    //     それぞれ違う。セーブが起動元に依存して別ファイルになるのを避ける。
    return FileSystem::PathToUtf8(FileSystem::GetExecutableDirectory() / slot);
}

bool SaveData::Save()
{
    const std::string absPath = ResolveAbsolutePath();
    if (!FileSystem::EnsureParentDirectory(FileSystem::PathFromUtf8(absPath))) {
        FBZZ_LOG_ERROR("SaveData: could not create save directory -> %s", absPath.c_str());
        return false;
    }

    std::ostringstream oss;
    oss << Table();
    if (!FileSystem::WriteText(absPath, oss.str())) {
        FBZZ_LOG_ERROR("SaveData: write failed -> %s", absPath.c_str());
        return false;
    }

    g_dirty = false;
    return true;
}

bool SaveData::Load()
{
    const std::string absPath = ResolveAbsolutePath();

    std::string text;
    if (!FileSystem::ReadText(absPath, text)) {
        // 初回起動 = セーブ未作成。空のまま成功扱いにする。
        Table().clear();
        g_dirty = false;
        return true;
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_ERROR("SaveData: TOML parse failed (corrupted save?) -> %s", absPath.c_str());
        return false;
    }

    Table() = std::move(result.table());
    g_dirty = false;
    return true;
}

bool SaveData::IsDirty()
{
    return g_dirty;
}

} // namespace fbzz::util
