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

/// 大文字小文字を無視した前方一致 (Windows パス比較用)。
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
/// @note `foo.fbx:2` / `foo.png::sprite::Run` の拡張子をそのまま残すと `.fbx:2` /
///       `.png::sprite::Run` になり AssetDatabase の対象判定を通らない。後半はサブアセット
///       固有情報なので GUID の外側へそのまま保持する。
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

/// 文字列値に変換関数を適用しながら TOML ツリーを再帰走査する。
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

/// 絶対パスをプロジェクト相対 ("Assets/..." / "Library/Baked/...") へ落とす。
/// プロジェクト外なら空文字列。ヒントは git に載るので、機械固有の絶対パスは書かない。
std::string ToProjectRelative(const std::string& absPath)
{
    const std::string root = AssetDatabase::ProjectRoot();
    if (root.empty() || !StartsWithCI(absPath, root)) return {};
    return absPath.substr(root.size());
}

/// @brief ランタイム標準の参照形式へ戻す (`Assets/...` 相対、それ以外は絶対パスのまま)。
/// @note Inspector の表示も既存の比較ロジックも `Assets/...` 前提で、ここだけ絶対パスを
///       返すと同じアセットが 2 通りの文字列で流通するため相対に寄せる。
std::string ToRuntimeRef(const std::string& absPath, const std::string& suffix)
{
    const std::string root = AssetDatabase::AssetsRoot();
    if (!root.empty() && StartsWithCI(absPath, root))
        return "Assets/" + absPath.substr(root.size()) + suffix;
    return absPath + suffix;
}

/// Sprite の接尾辞を、保存形 (ID) と読める形 (名前) の 2 つに分けて返す。
/// 権威は ID、ヒントは名前、という規約はパスヒントとまったく同じ。
/// ID が引けなかったときに «名前で書かれた参照» をそのまま通すため、変換できない
/// トークンは触らずに返す (データを勝手に捨てない)。
struct SpriteSuffixParts {
    std::string authority;  ///< `::sprite::<id>` (引けなければ入力のまま)
    std::string hintName;   ///< `<name>` (引けなければ空)
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
/// @note パスヒントによるファイル復旧と同じ考え方。権威 (ID) が引けなくなったときだけ
///       読める側の控えで生かし、復旧したことは必ずログに出す (黙って拾うとアトラスを
///       切り直すたびに参照が入れ替わっても気付けない)。
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

} // namespace

std::string EncodeGuidRef(const std::string& pathOrRef)
{
    if (pathOrRef.empty()) return pathOrRef;
    /// @note 既にエンコード済み
    if (AssetDatabase::IsGuidRef(pathOrRef)) return pathOrRef;

    const AssetReferenceParts parts = SplitAssetReference(pathOrRef);
    /// @note ファイル名で高速に足切りする。baked (.fzasset/.mesh/.skel) や通常文字列はここで素通り。
    const std::string name = LowerCopy(util::FileSystem::GetFilename(parts.base));
    if (!AssetDatabase::ShouldHaveMeta(name)) return pathOrRef;

    /// @note 相対 ("Assets/...") も絶対パスも AssetManager の解決規則で実パス化してから索引を引く。
    ///       未登録 (実在しない / Assets 外) なら元の文字列を返し、データを壊さない。
    const std::string absPath = AssetManager::ResolveAssetPath(parts.base);
    const std::string guid = AssetDatabase::TryGetGuidFromPath(absPath);
    if (guid.empty()) return pathOrRef;

    /// @note Sprite は「保存は ID、併記は名前」へ正規化する。名前で書かれた参照
    ///       ("Atlas.png::sprite::Key_W") はここで ID へ寄せられ、ディスク上には
    ///       リネームに強い形だけが残る。
    const SpriteSuffixParts sprite = SplitSpriteSuffix(absPath, parts.suffix);
    std::string encoded = std::string(AssetDatabase::kGuidPrefix) + guid + sprite.authority;

    /// @note 読める形を後ろへ併記する。権威はあくまで guid で、ヒントは読み手のためと
    ///       guid が引けなくなったときの復旧経路にしか使わない。
    if (std::string hint = ToProjectRelative(absPath); !hint.empty()) {
        if (!sprite.hintName.empty()) hint += "::sprite::" + sprite.hintName;
        encoded += AssetDatabase::kRefHintSeparator + hint;
    }
    return encoded;
}

std::string DecodeGuidRef(const std::string& ref)
{
    if (!AssetDatabase::IsGuidRef(ref)) return ref;

    /// @note ヒントを先に切り離す。パスにはドットもコロンも入りうるので、
    ///       SplitAssetReference へ渡す前に落とさないとサブアセット接尾辞と混ざる。
    const std::string hintRaw = AssetDatabase::HintFromRef(ref);
    const size_t sep = ref.find(AssetDatabase::kRefHintSeparator);
    const std::string guidRef = sep == std::string::npos ? ref : ref.substr(0, sep);

    /// @note ヒント側も「パス + Sprite 名」の形を取りうる。パスとして使う前に分ける。
    std::string hint;
    std::string hintSpriteName;
    (void)ParseSpriteReference(hintRaw, hint, hintSpriteName);

    /// @note guid 参照の本体は `guid:` + 32hex で長さが決まっている。サブアセット接尾辞は
    ///       その後ろだけを取る (SplitAssetReference は使わない)。あちらは `foo.fbx:2` のように
    ///       «最後のコロンの後ろが全部数字なら submesh 番号» と見なすため、guid が偶然 10 進
    ///       数字だけで構成されていると `guid:` 自身のコロンを区切りと誤読してしまう。
    const std::string guid = AssetDatabase::GuidFromRef(guidRef);
    const std::size_t bodyLength = AssetDatabase::kGuidPrefix.size() + guid.size();
    const std::string suffix =
        bodyLength < guidRef.size() ? guidRef.substr(bodyLength) : std::string{};

    const std::string abs = AssetDatabase::PathFromGuid(guid);
    /// @note 索引に載っていてもファイルを消していれば参照は切れているため実体の有無まで見る。
    ///       索引だけで成功扱いにすると、その先の読み込み失敗が「空のアセット」として
    ///       静かに流れ、Console にも何も出ないまま見た目だけが壊れる。
    if (!abs.empty() && util::FileSystem::Exists(abs))
        return ToRuntimeRef(abs, ReconcileSpriteSuffix(abs, suffix, hintSpriteName));

    /// @note guid が引けない / 実体が消えている。.meta を作り直した後などに起きる。
    ///       ヒントの実体が残っているなら、そちらで拾い直して参照を生かす。
    if (!hint.empty()) {
        const std::string recovered = AssetDatabase::ProjectRoot() + hint;
        if (util::FileSystem::Exists(recovered)) {
            FBZZ_LOG_WARN("GuidRefCodec: guid unresolved, recovered by path hint [%s]",
                          hint.c_str());
            return ToRuntimeRef(recovered,
                                ReconcileSpriteSuffix(recovered, suffix, hintSpriteName));
        }
    }

    /// @note 復旧もできない。索引の実パスが分かっているならそちらを返し、
    ///       後段のロードエラーが guid ではなくファイル名で読めるようにする。
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
    TransformStrings(root, [](const std::string& v) { return EncodeGuidRef(v); });
}

void DecodeGuidRefs(toml::table& root)
{
    TransformStrings(root, [](const std::string& v) { return DecodeGuidRef(v); });
}

} // namespace fbzz::asset
