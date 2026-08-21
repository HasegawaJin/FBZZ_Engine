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
#include <utility>

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

struct AssetReferenceParts {
    std::string base;
    std::string suffix;
};

// スプライト名やサブメッシュ番号を、GUID が指す本体パスから分離する。
// WHY: "foo.fbx:2" / "foo.png::sprite::Run" の拡張子はそのままだと .fbx:2 /
//      .png::sprite::Run になり、AssetDatabase の対象判定を通らない。後半は
//      サブアセット固有情報なので、GUID の外側へそのまま保持する。
AssetReferenceParts SplitAssetReference(const std::string& value)
{
    constexpr std::string_view kSpriteMarker = "::sprite::";
    if (const std::size_t spritePos = value.find(kSpriteMarker);
        spritePos != std::string::npos && spritePos > 0) {
        return { value.substr(0, spritePos), value.substr(spritePos) };
    }

    const std::size_t slashPos = value.find_last_of('/');
    const std::size_t colonPos = value.rfind(':');
    if (colonPos != std::string::npos
        && (slashPos == std::string::npos || colonPos > slashPos)
        && colonPos + 1 < value.size()) {
        const std::string_view suffix(value.data() + colonPos + 1,
                                      value.size() - colonPos - 1);
        bool allDigits = true;
        for (const char c : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }
        }
        if (allDigits)
            return { value.substr(0, colonPos), value.substr(colonPos) };
    }
    return { value, {} };
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

// 絶対パスをプロジェクト相対 ("Assets/..." / "Library/Baked/...") へ落とす。
// プロジェクト外なら空文字列。ヒントは git に載るので、機械固有の絶対パスは書かない。
std::string ToProjectRelative(const std::string& absPath)
{
    const std::string root = AssetDatabase::ProjectRoot();
    if (root.empty() || !StartsWithCI(absPath, root)) return {};
    return absPath.substr(root.size());
}

// ランタイム標準の参照形式へ戻す ("Assets/..." 相対、それ以外は絶対パスのまま)。
// WHY 相対に寄せるか: Inspector の表示も既存の比較ロジックも "Assets/..." 前提で、
//     ここだけ絶対パスを返すと同じアセットが 2 通りの文字列で流通する。
std::string ToRuntimeRef(const std::string& absPath, const std::string& suffix)
{
    const std::string root = AssetDatabase::AssetsRoot();
    if (!root.empty() && StartsWithCI(absPath, root))
        return "Assets/" + absPath.substr(root.size()) + suffix;
    return absPath + suffix;
}

} // namespace

std::string EncodeGuidRef(const std::string& pathOrRef)
{
    if (pathOrRef.empty()) return pathOrRef;
    if (AssetDatabase::IsGuidRef(pathOrRef)) return pathOrRef; // 既にエンコード済み

    const AssetReferenceParts parts = SplitAssetReference(pathOrRef);
    // 拡張子で高速に足切りする。baked (.fzasset/.mesh/.skel) や通常文字列はここで素通り。
    const std::string ext = LowerCopy(util::FileSystem::GetExtension(parts.base));
    if (!AssetDatabase::ShouldHaveMeta(ext)) return pathOrRef;

    // 相対 ("Assets/...") も絶対パスも AssetManager の解決規則で実パス化してから索引を引く。
    // 未登録 (実在しない / Assets 外) なら元の文字列を返し、データを壊さない。
    const std::string absPath = AssetManager::ResolveAssetPath(parts.base);
    const std::string guid = AssetDatabase::TryGetGuidFromPath(absPath);
    if (guid.empty()) return pathOrRef;

    std::string encoded = std::string(AssetDatabase::kGuidPrefix) + guid + parts.suffix;

    // 読める形を後ろへ併記する。権威はあくまで guid で、ヒントは読み手のためと
    // guid が引けなくなったときの復旧経路にしか使わない。
    if (const std::string hint = ToProjectRelative(absPath); !hint.empty())
        encoded += AssetDatabase::kRefHintSeparator + hint;
    return encoded;
}

std::string DecodeGuidRef(const std::string& ref)
{
    if (!AssetDatabase::IsGuidRef(ref)) return ref;

    // ヒントを先に切り離す。パスにはドットもコロンも入りうるので、
    // SplitAssetReference へ渡す前に落とさないとサブアセット接尾辞と混ざる。
    const std::string hint = AssetDatabase::HintFromRef(ref);
    const size_t sep = ref.find(AssetDatabase::kRefHintSeparator);
    const std::string guidRef = sep == std::string::npos ? ref : ref.substr(0, sep);

    const AssetReferenceParts parts = SplitAssetReference(guidRef);
    const std::string abs = AssetDatabase::PathFromGuid(AssetDatabase::GuidFromRef(parts.base));
    // WHY 実体の有無まで見るか: 索引に載っていてもファイルを消していれば参照は切れている。
    //     索引だけで成功扱いにすると、その先の読み込み失敗が「空のアセット」として
    //     静かに流れ、Console にも何も出ないまま見た目だけが壊れる。
    if (!abs.empty() && util::FileSystem::Exists(abs))
        return ToRuntimeRef(abs, parts.suffix);

    // guid が引けない / 実体が消えている。.meta を作り直した後などに起きる。
    // ヒントの実体が残っているなら、そちらで拾い直して参照を生かす。
    if (!hint.empty()) {
        const std::string recovered = AssetDatabase::ProjectRoot() + hint;
        if (util::FileSystem::Exists(recovered)) {
            FBZZ_LOG_WARN("GuidRefCodec: guid unresolved, recovered by path hint [%s]",
                          hint.c_str());
            return ToRuntimeRef(recovered, parts.suffix);
        }
    }

    // 復旧もできない。索引の実パスが分かっているならそちらを返し、
    // 後段のロードエラーが guid ではなくファイル名で読めるようにする。
    if (!abs.empty()) {
        FBZZ_LOG_ERROR("GuidRefCodec: asset file is missing [%s]\n  guid ref: %s",
                       abs.c_str(), ref.c_str());
        return ToRuntimeRef(abs, parts.suffix);
    }

    FBZZ_LOG_ERROR("GuidRefCodec: unresolved guid reference [%s]%s%s",
                   ref.c_str(),
                   hint.empty() ? "" : "\n  last known path: ",
                   hint.c_str());
    return ref;
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
