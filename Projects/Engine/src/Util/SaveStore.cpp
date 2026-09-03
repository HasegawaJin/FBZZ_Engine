/// @file    SaveStore.cpp
/// @brief   ランタイム永続化ストアの TOML 入出力。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Engine/Util/SaveStore.hpp>

#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Scene/TomlReflector.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

namespace fbzz::util {

// メモリ上の正本を toml::table で持つ。型ごとの variant を自前で持たずに済み、
// 保存も oss << table で完結する。
struct SaveStore::Impl {
    toml::table table;
    std::string path;
    bool        dirty = false;

    [[nodiscard]] std::string ResolveAbsolutePath() const
    {
        const std::filesystem::path slot = FileSystem::PathFromUtf8(path);
        if (slot.is_absolute()) return FileSystem::PathToUtf8(slot);
        // WHY 実行ファイル基準か: カレントディレクトリはエディタ / GameHub / スタンドアロンで
        //     それぞれ違う。保存先が起動元に依存して別ファイルになるのを避ける。
        return FileSystem::PathToUtf8(FileSystem::GetExecutableDirectory() / slot);
    }
};

namespace {

template<std::size_t N>
void WriteFloats(toml::table& table, std::string_view key, const float (&values)[N])
{
    toml::array arr;
    for (std::size_t i = 0; i < N; ++i) arr.push_back(static_cast<double>(values[i]));
    table.insert_or_assign(std::string(key), std::move(arr));
}

[[nodiscard]] const toml::array* FindArray(const toml::table& table, std::string_view key)
{
    const auto it = table.find(std::string(key));
    return it == table.end() ? nullptr : it->second.as_array();
}

} // namespace

SaveStore::SaveStore(std::string defaultPath)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->path = std::move(defaultPath);
}

SaveStore::~SaveStore() = default;
SaveStore::SaveStore(SaveStore&&) noexcept = default;
SaveStore& SaveStore::operator=(SaveStore&&) noexcept = default;

void SaveStore::SetPath(const std::string& path) { m_impl->path = path; }
const std::string& SaveStore::GetPath() const { return m_impl->path; }

// ── ユーザー定義型 ───────────────────────────────────────────────────────────

bool SaveStore::Write(std::string_view key, scene::IScriptSerializable& object)
{
    // 先にルートへ空テーブルを置き、その実体へ書き込む。
    // WHY: 組み立ててから move で挿入すると、構築中に子のアドレスを保持できない。
    auto [iterator, inserted] =
        m_impl->table.insert_or_assign(std::string(key), toml::table{});
    toml::table* target = iterator->second.as_table();
    if (!target) return false;

    TomlWriteReflector writer(*target);
    object.Reflect(writer);
    m_impl->dirty = true;
    return true;
}

bool SaveStore::Read(std::string_view key, scene::IScriptSerializable& object)
{
    const auto it = m_impl->table.find(std::string(key));
    if (it == m_impl->table.end()) return false;
    const toml::table* source = it->second.as_table();
    if (!source) return false;

    TomlReadReflector reader(*source);
    object.Reflect(reader);
    return true;
}

// ── スカラー書き込み ─────────────────────────────────────────────────────────

void SaveStore::SetBool(std::string_view key, bool value)
{
    m_impl->table.insert_or_assign(std::string(key), value);
    m_impl->dirty = true;
}

void SaveStore::SetInt(std::string_view key, int value)
{
    m_impl->table.insert_or_assign(std::string(key), static_cast<int64_t>(value));
    m_impl->dirty = true;
}

void SaveStore::SetFloat(std::string_view key, float value)
{
    m_impl->table.insert_or_assign(std::string(key), static_cast<double>(value));
    m_impl->dirty = true;
}

void SaveStore::SetString(std::string_view key, std::string_view value)
{
    m_impl->table.insert_or_assign(std::string(key), std::string(value));
    m_impl->dirty = true;
}

void SaveStore::SetVector2(std::string_view key, const math::Vector2& value)
{
    const float values[2] = { value.x, value.y };
    WriteFloats(m_impl->table, key, values);
    m_impl->dirty = true;
}

void SaveStore::SetVector3(std::string_view key, const math::Vector3& value)
{
    const float values[3] = { value.x, value.y, value.z };
    WriteFloats(m_impl->table, key, values);
    m_impl->dirty = true;
}

void SaveStore::SetVector4(std::string_view key, const math::Vector4& value)
{
    const float values[4] = { value.x, value.y, value.z, value.w };
    WriteFloats(m_impl->table, key, values);
    m_impl->dirty = true;
}

// ── スカラー読み出し ─────────────────────────────────────────────────────────

bool SaveStore::GetBool(std::string_view key, bool defaultValue) const
{
    return m_impl->table[std::string(key)].value_or(defaultValue);
}

int SaveStore::GetInt(std::string_view key, int defaultValue) const
{
    return static_cast<int>(
        m_impl->table[std::string(key)].value_or(static_cast<int64_t>(defaultValue)));
}

float SaveStore::GetFloat(std::string_view key, float defaultValue) const
{
    // WHY int64 も見るか: TOML では 1.0 を書いても整数として読み戻る場合があり、
    //      SetFloat(1.0f) → GetFloat() が既定値に落ちる事故を防ぐ。
    const auto node = m_impl->table[std::string(key)];
    if (const auto asDouble = node.value<double>()) return static_cast<float>(*asDouble);
    if (const auto asInt = node.value<int64_t>())   return static_cast<float>(*asInt);
    return defaultValue;
}

std::string SaveStore::GetString(std::string_view key, std::string_view defaultValue) const
{
    return m_impl->table[std::string(key)].value_or(std::string(defaultValue));
}

math::Vector2 SaveStore::GetVector2(std::string_view key, const math::Vector2& defaultValue) const
{
    return ArrToVec2(FindArray(m_impl->table, key), defaultValue);
}

math::Vector3 SaveStore::GetVector3(std::string_view key, const math::Vector3& defaultValue) const
{
    return ArrToVec3(FindArray(m_impl->table, key), defaultValue);
}

math::Vector4 SaveStore::GetVector4(std::string_view key, const math::Vector4& defaultValue) const
{
    return ArrToVec4(FindArray(m_impl->table, key), defaultValue);
}

// ── 管理 ────────────────────────────────────────────────────────────────────

bool SaveStore::Has(std::string_view key) const
{
    return m_impl->table.contains(std::string(key));
}

void SaveStore::Remove(std::string_view key)
{
    if (m_impl->table.erase(std::string(key)) > 0) m_impl->dirty = true;
}

void SaveStore::Clear()
{
    if (m_impl->table.empty()) return;
    m_impl->table.clear();
    m_impl->dirty = true;
}

std::vector<std::string> SaveStore::Keys() const
{
    std::vector<std::string> keys;
    keys.reserve(m_impl->table.size());
    for (const auto& [key, value] : m_impl->table) keys.emplace_back(key.str());
    std::sort(keys.begin(), keys.end());
    return keys;
}

bool SaveStore::Save()
{
    const std::string absPath = m_impl->ResolveAbsolutePath();
    if (!FileSystem::EnsureParentDirectory(FileSystem::PathFromUtf8(absPath))) {
        FBZZ_LOG_ERROR("SaveStore: could not create directory -> %s", absPath.c_str());
        return false;
    }

    std::ostringstream oss;
    oss << m_impl->table;
    if (!FileSystem::WriteText(absPath, oss.str())) {
        FBZZ_LOG_ERROR("SaveStore: write failed -> %s", absPath.c_str());
        return false;
    }

    m_impl->dirty = false;
    return true;
}

bool SaveStore::Load()
{
    const std::string absPath = m_impl->ResolveAbsolutePath();

    std::string text;
    if (!FileSystem::ReadText(absPath, text)) {
        // 初回起動 = ファイル未作成。空のまま成功扱いにする。
        m_impl->table.clear();
        m_impl->dirty = false;
        return true;
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_ERROR("SaveStore: TOML parse failed (corrupted?) -> %s", absPath.c_str());
        return false;
    }

    m_impl->table = std::move(result.table());
    m_impl->dirty = false;
    return true;
}

bool SaveStore::IsDirty() const { return m_impl->dirty; }

} // namespace fbzz::util
