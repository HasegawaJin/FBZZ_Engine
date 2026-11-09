/// @file    GuidRefCodec.cpp
/// @brief   TOML シリアライズ境界でのアセット参照 ⇄ GUID 変換実装。
/// @author  Hasegawa Jin
/// @date    2026-07-08
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <cctype>
#include <unordered_map>

namespace fbzz::asset {

namespace {

/// @note Windows のパス比較として大文字小文字を無視する。
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

/// @brief スプライト名やサブメッシュ番号を、GUID が指す本体パスから分離する。
/// @note サブアセット接尾辞を拡張子判定から除き、本体の GUID の外側へ保持する。
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

template<typename Fn>
void TransformStrings(toml::table& tbl, const Fn& fn);

template<typename Fn>
void TransformStrings(toml::array& arr, const Fn& fn)
{
    for (auto& node : arr) {
        if (auto* s = node.as_string()) {
            const std::string& converted = fn(s->get());
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
            const std::string& converted = fn(s->get());
            if (converted != s->get()) *s = converted;
        } else if (auto* t = node.as_table()) {
            TransformStrings(*t, fn);
        } else if (auto* a = node.as_array()) {
            TransformStrings(*a, fn);
        }
    }
}

/// @return プロジェクト相対パス。プロジェクト外なら空文字列。
/// @note 保存されるヒントに機械固有の絶対パスを載せない。
std::string ToProjectRelative(const std::string& absPath)
{
    const std::string root = AssetDatabase::ProjectRoot();
    if (root.empty() || !StartsWithCI(absPath, root)) return {};
    return absPath.substr(root.size());
}

/// @brief ランタイム標準の参照形式へ戻す (`Assets/...` 相対、それ以外は絶対パスのまま)。
/// @note Inspector と比較処理の `Assets/...` 規約に合わせ、同じアセットを 2 通りの文字列で流通させない。
std::string ToRuntimeRef(const std::string& absPath, const std::string& suffix)
{
    const std::string root = AssetDatabase::AssetsRoot();
    if (!root.empty() && StartsWithCI(absPath, root))
        return "Assets/" + absPath.substr(root.size()) + suffix;
    return absPath + suffix;
}

/// @note Sprite は ID を正本、名前を復旧用ヒントとする。
/// @note ID を引けない場合は名前の参照を保持し、変換できないトークンを捨てない。
struct SpriteSuffixParts {
    std::string authority;  ///< @note `::sprite::<id>` (引けなければ入力のまま)
    std::string hintName;   ///< @note `<name>` (引けなければ空)
};

SpriteSuffixParts SplitSpriteSuffix(const std::string& absTexturePath,
                                    const std::string& suffix)
{
    constexpr std::string_view kSpriteMarker = "::sprite::";
    if (!suffix.starts_with(kSpriteMarker)) return { suffix, {} };

    const std::string token = suffix.substr(kSpriteMarker.size());
    const std::string id   = LookupSpriteId(absTexturePath, token);
    const std::string name = LookupSpriteName(absTexturePath, token);
    if (id.empty()) return { suffix, name };
    return { std::string(kSpriteMarker) + id, name };
}

/// @brief 保存された ID が .meta から消えていたら、併記した名前で拾い直す。
/// @note ID が引けないときだけ名前の控えで復旧し、アトラスの切り直しによる参照変更をログへ残す。
std::string ReconcileSpriteSuffix(const std::string& absTexturePath,
                                  const std::string& suffix,
                                  const std::string& hintName)
{
    constexpr std::string_view kSpriteMarker = "::sprite::";
    if (!suffix.starts_with(kSpriteMarker) || hintName.empty()) return suffix;

    const std::string token = suffix.substr(kSpriteMarker.size());
    if (!LookupSpriteId(absTexturePath, token).empty()) return suffix;
    if (LookupSpriteId(absTexturePath, hintName).empty()) return suffix;

    FBZZ_LOG_WARN("GuidRefCodec: sprite id unresolved, recovered by name hint [%s]",
                  hintName.c_str());
    return std::string(kSpriteMarker) + hintName;
}

bool IsAssetReferenceCandidate(const AssetReferenceParts& parts)
{
    const std::string name = util::StringUtils::ToLowerCopy(util::FileSystem::GetFilename(parts.base));
    return AssetDatabase::ShouldHaveMeta(name);
}

std::string EncodeAssetReference(const std::string& pathOrRef, const AssetReferenceParts& parts)
{
    /// @note 相対 ("Assets/...") も絶対パスも AssetManager の解決規則で実パス化してから索引を引く。
    /// @note 未登録 (実在しない / Assets 外) なら元の文字列を返し、データを壊さない。
    const std::string absPath = AssetManager::ResolveAssetPath(parts.base);
    const std::string guid = AssetDatabase::TryGetGuidFromPath(absPath);
    if (guid.empty()) return pathOrRef;

    /// @note 名前の Sprite 参照を ID へ正規化し、ディスク上の参照をリネームに強い形へ揃える。
    const SpriteSuffixParts sprite = SplitSpriteSuffix(absPath, parts.suffix);
    std::string encoded = std::string(AssetDatabase::kGuidPrefix) + guid + sprite.authority;

    /// @note guid を正本とし、読めるパスを表示と参照切れの復旧に使うヒントとして併記する。
    if (std::string hint = ToProjectRelative(absPath); !hint.empty()) {
        if (!sprite.hintName.empty()) hint += "::sprite::" + sprite.hintName;
        encoded += AssetDatabase::kRefHintSeparator + hint;
    }
    return encoded;
}

} /// @note namespace

std::string EncodeGuidRef(const std::string& pathOrRef)
{
    if (pathOrRef.find('.') == std::string::npos || AssetDatabase::IsGuidRef(pathOrRef)) return pathOrRef;
    const AssetReferenceParts parts = SplitAssetReference(pathOrRef);
    return IsAssetReferenceCandidate(parts) ? EncodeAssetReference(pathOrRef, parts) : pathOrRef;
}

std::string DecodeGuidRef(const std::string& ref)
{
    if (!AssetDatabase::IsGuidRef(ref)) return ref;

    /// @note ドットやコロンを含むヒントを先に切り離し、サブアセット接尾辞との混同を防ぐ。
    const std::string hintRaw = AssetDatabase::HintFromRef(ref);
    const size_t sep = ref.find(AssetDatabase::kRefHintSeparator);
    const std::string guidRef = sep == std::string::npos ? ref : ref.substr(0, sep);

    /// @note ヒント側も「パス + Sprite 名」の形を取りうる。パスとして使う前に分ける。
    std::string hint;
    std::string hintSpriteName;
    (void)ParseSpriteReference(hintRaw, hint, hintSpriteName);

    /// @note `guid:` + 32hex の長さで接尾辞を分離し、数字だけの GUID を submesh 番号と誤読することを防ぐ。
    const std::string guid = AssetDatabase::GuidFromRef(guidRef);
    const std::size_t bodyLength = AssetDatabase::kGuidPrefix.size() + guid.size();
    const std::string suffix =
        bodyLength < guidRef.size() ? guidRef.substr(bodyLength) : std::string{};

    const std::string abs = AssetDatabase::PathFromGuid(guid);
    /// @note 索引だけで成功扱いにすると空のアセットで描画が壊れるため、実体の有無も確認する。
    if (!abs.empty() && util::FileSystem::Exists(abs))
        return ToRuntimeRef(abs, ReconcileSpriteSuffix(abs, suffix, hintSpriteName));

    /// @note guid が引けない / 実体が消えている。.meta を作り直した後などに起きる。
    /// @note ヒントの実体が残っているなら、そちらで拾い直して参照を生かす。
    if (!hint.empty()) {
        const std::string recovered = AssetDatabase::ProjectRoot() + hint;
        if (util::FileSystem::Exists(recovered)) {
            FBZZ_LOG_WARN("GuidRefCodec: guid unresolved, recovered by path hint [%s]",
                          hint.c_str());
            return ToRuntimeRef(recovered,
                                ReconcileSpriteSuffix(recovered, suffix, hintSpriteName));
        }
    }

    /// @note 復旧できない場合は索引の実パスを返し、後段のロードエラーをファイル名で読めるようにする。
    if (!abs.empty()) {
        FBZZ_LOG_ERROR("GuidRefCodec: asset file is missing [%s]\n  guid ref: %s",
                       abs.c_str(), ref.c_str());
        return ToRuntimeRef(abs, suffix);
    }

    FBZZ_LOG_ERROR("GuidRefCodec: unresolved guid reference [%s]%s%s",
                   ref.c_str(),
                   hint.empty() ? "" : "\n  last known path: ",
                   hint.c_str());
    return ref;
}

void EncodeGuidRefs(toml::table& root)
{
    /// @note 同じ参照が数千個あってもパス解決とファイル照会は文書内で一度だけ行う。
    std::unordered_map<std::string, std::string> encoded;
    TransformStrings(root, [&encoded](const std::string& value) -> const std::string& {
        if (value.find('.') == std::string::npos || AssetDatabase::IsGuidRef(value)) return value;
        if (const auto found = encoded.find(value); found != encoded.end()) return found->second;
        const AssetReferenceParts parts = SplitAssetReference(value);
        if (!IsAssetReferenceCandidate(parts)) return value;
        return encoded.emplace(value, EncodeAssetReference(value, parts)).first->second;
    });
}

void DecodeGuidRefs(toml::table& root)
{
    TransformStrings(root, [](const std::string& v) { return DecodeGuidRef(v); });
}

} /// @note namespace fbzz::asset
