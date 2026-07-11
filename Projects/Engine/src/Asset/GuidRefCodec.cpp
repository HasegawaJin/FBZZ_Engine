// FBZZ Engine
// GuidRefCodec.cpp | fbzz::asset
// TOML シリアライズ境界でのアセット参照 ⇄ GUID 変換実装
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cctype>

namespace fbzz::asset {

namespace {

std::string LowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 大文字小文字を無視した前方一致 (Windows パス比較用)。
bool StartsWithCI(const std::string& s, const std::string& prefix)
{
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    }
    return true;
}

// 文字列値に変換関数を適用しながら TOML ツリーを再帰走査する。
template<typename Fn>
void TransformStrings(toml::table& tbl, const Fn& fn);

template<typename Fn>
void TransformStrings(toml::array& arr, const Fn& fn)
{
    for (auto& node : arr) {
        if (auto* s = node.as_string()) {
            const std::string converted = fn(s->get());
            if (converted != s->get()) *s = converted;
        } else if (auto* t = node.as_table()) {
            TransformStrings(*t, fn);
        } else if (auto* a = node.as_array()) {
            TransformStrings(*a, fn);
        }
    }
}

template<typename Fn>
void TransformStrings(toml::table& tbl, const Fn& fn)
{
    for (auto&& [key, node] : tbl) {
        if (auto* s = node.as_string()) {
            const std::string converted = fn(s->get());
            if (converted != s->get()) *s = converted;
        } else if (auto* t = node.as_table()) {
            TransformStrings(*t, fn);
        } else if (auto* a = node.as_array()) {
            TransformStrings(*a, fn);
        }
    }
}

} // namespace

std::string EncodeGuidRef(const std::string& pathOrRef)
{
    if (pathOrRef.empty()) return pathOrRef;
    if (AssetDatabase::IsGuidRef(pathOrRef)) return pathOrRef; // 既にエンコード済み

    // 拡張子で高速に足切りする。baked (.fzasset/.mesh/.skel) や通常文字列はここで素通り。
    const std::string ext = LowerCopy(util::FileSystem::GetExtension(pathOrRef));
    if (!AssetDatabase::ShouldHaveMeta(ext)) return pathOrRef;

    // 相対 ("Assets/...") も絶対パスも AssetManager の解決規則で実パス化してから索引を引く。
    // 未登録 (実在しない / Assets 外) なら元の文字列を返し、データを壊さない。
    const std::string absPath = AssetManager::ResolveAssetPath(pathOrRef);
    const std::string guid = AssetDatabase::GuidFromPath(absPath);
    if (guid.empty()) return pathOrRef;
    return std::string(AssetDatabase::kGuidPrefix) + guid;
}

std::string DecodeGuidRef(const std::string& ref)
{
    if (!AssetDatabase::IsGuidRef(ref)) return ref;

    const std::string abs = AssetDatabase::PathFromGuid(
        ref.substr(AssetDatabase::kGuidPrefix.size()));
    if (abs.empty()) {
        // アセット削除済み等。元の guid: を残せば ResolvePath が改めてエラーを報告する。
        FBZZ_LOG_WARN("GuidRefCodec: unresolved guid reference [%s]", ref.c_str());
        return ref;
    }

    // ランタイム標準の "Assets/..." 相対形式へ戻す (Inspector や既存比較ロジックとの互換)。
    const std::string root = AssetDatabase::AssetsRoot();
    if (!root.empty() && StartsWithCI(abs, root))
        return "Assets/" + abs.substr(root.size());
    return abs;
}

void EncodeGuidRefs(toml::table& root)
{
    TransformStrings(root, [](const std::string& v) { return EncodeGuidRef(v); });
}

void DecodeGuidRefs(toml::table& root)
{
    TransformStrings(root, [](const std::string& v) { return DecodeGuidRef(v); });
}

} // namespace fbzz::asset
