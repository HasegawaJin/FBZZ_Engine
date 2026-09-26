/// @file    AssetDatabase.hpp
/// @brief   GUID ⇄ アセットパスの双方向インデックス。
/// @author  Hasegawa Jin
/// @date    2026-07-08
///
/// 各アセット (ファイル・フォルダ) は隣接する `<名前>.meta` の [meta] guid で恒久 ID を持つ。
/// フォルダの .meta は親ディレクトリに置く (例: `Assets/Textures/` → `Assets/Textures.meta`)。
/// Init はメインスレッドで 1 回。以後の参照系は内部 mutex 保護で任意スレッドから呼べる。
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset {

/// @brief GUID ⇄ アセットパスの静的インデックス (全メンバー static)。
/// @note 参照は `"guid:<32hex>"` を保存し AssetManager::ResolvePath が解決する。GUID は生成後不変なため、リネーム・移動しても索引再構築だけで参照が生きる。
class AssetDatabase {
public:
    /// @brief `"guid:"` 参照プレフィックス。この後ろに 32 桁 hex が続く。
    static constexpr std::string_view kGuidPrefix = "guid:";

    /// @brief assetsRoot 配下を走査してインデックスを構築する。AssetManager::Init から呼ぶ。
    /// @param assetsRoot 例 `"C:/proj/Assets/"`。
    /// @note .meta が無い / guid が無い対象は自動生成する。ただし FBX は Import 実行時まで生成しない。
    static void Init(const std::string& assetsRoot);
    static void Shutdown();

    /// @brief guid (32hex) → アセット絶対パス。
    /// @return 未登録なら空文字列。
    [[nodiscard]] static std::string PathFromGuid(const std::string& guid);

    /// @brief アセット絶対パス → guid。未登録なら .meta を生成して新規発行する。
    /// @return 失敗 (.meta 書き込み不可等) なら空文字列。
    [[nodiscard]] static std::string GuidFromPath(const std::string& absPath);

    /// @brief .meta の GUID だけを確保し、索引には登録しない。
    /// @note 仮出力を本番パスへ移動してから GuidFromPath で登録する場合に使う。
    [[nodiscard]] static std::string EnsureGuidMetaUnindexed(const std::string& absPath);

    /// @brief アセット絶対パス → 既存 .meta の guid。GuidFromPath と違い新規発行しない。
    /// @return 未登録 / .meta 未作成なら空文字列。
    /// @note AssetBrowser の存在確認等で、Import 前の FBX に .meta を発行させないために使う。
    [[nodiscard]] static std::string TryGetGuidFromPath(const std::string& absPath);

    /// @brief リネーム / 移動フック。guid は不変のままパス索引だけ付け替える。
    /// @pre 呼び出し側 (AssetBrowser) が本体と .meta を両方移動した後に呼ぶ。
    static void OnAssetMoved(const std::string& oldAbsPath, const std::string& newAbsPath);

    /// @brief 削除されたアセットを索引から除去する。フォルダなら配下も一括で除去する。
    /// @note 残すと、同じ場所への作り直し時に旧 GUID を返し参照先が静かに入れ替わる。
    static void OnAssetRemoved(const std::string& absPath);

    /// @brief `"guid:xxxx"` 形式か判定する (ResolvePath の分岐用)。
    [[nodiscard]] static bool IsGuidRef(std::string_view ref) {
        return ref.rfind(kGuidPrefix, 0) == 0;
    }

    /// @brief 参照に人が読めるパスを併記するときの区切り。
    /// @note 形式: `guid:<32hex>[サブアセット接尾辞]|<プロジェクト相対パス>`。権威は guid、ヒントは復旧用。
    /// @note 導出 guid (Library/Baked) は文字列として存在せず grep で辿れないため併記する。'|' は Windows のファイル名に使えず衝突しない。
    static constexpr char kRefHintSeparator = '|';

    /// @brief 参照文字列から guid 本体 (32hex) だけを取り出す。`"guid:"` プレフィックス・パスヒント・サブアセット接尾辞を落とす。
    /// @return guid 参照でなければ空文字列。
    [[nodiscard]] static std::string GuidFromRef(std::string_view ref);

    /// @brief 参照文字列に併記されたパスヒント (プロジェクト相対) を取り出す。
    /// @return 無ければ空文字列。
    [[nodiscard]] static std::string HintFromRef(std::string_view ref);

    /// @brief Init に渡された Assets ルートの 1 つ上 (末尾 '/' 付き)。
    /// @note パスヒントと索引ファイルをプロジェクト相対で書くために使う。
    [[nodiscard]] static std::string ProjectRoot();

    /// @brief guid → パスの索引を `Library/AssetIndex.toml` へ書き出す。
    /// @note メモリ上だけだとエディター未起動時に guid を解決できない。1 ファイルで全部引ける形にする。
    static void SaveIndexFile();

    /// @brief 索引が変わっていれば SaveIndexFile する。毎フレーム呼んでよい。
    static void FlushIndexFile();

    /// @brief 32 桁 hex の新規 GUID を生成する (Unity と同形式)。
    [[nodiscard]] static std::string GenerateGuid();

    /// @brief 原本の GUID とサブキーから、生成物の GUID を決定論的に導出する。
    /// @param sourceGuid 原本 (git 管理下) の GUID。
    /// @param subKey baked コンテナ内の相対パス (例 `"anims/MiniBot@Idle.anim"`)。
    /// @note 乱数だと Library (.gitignore 済み) の再インポートで GUID が変わり参照が切れる。導出なら環境やキャッシュ削除に依らず同じ値に戻る。
    [[nodiscard]] static std::string DeriveGuid(std::string_view sourceGuid,
                                                std::string_view subKey);

    /// @brief `Library/Baked` 配下を走査して導出 GUID で索引へ登録する。Init から呼ばれる。
    /// @note libraryRoot が無ければ何もしない。
    static void IndexBakedLibrary(const std::string& libraryBakedRoot);

    /// @brief このファイルは .meta を持つべきか。
    /// @param lowerFileName ファイル名のみ (パスではない、小文字化済み)。
    /// @note 除外リスト方式。再生成できる派生物 (.fzasset/.mesh/.skel/.cso/.generated.hpp/ビルド出力) と無拡張ファイル以外は GUID を持つ。
    [[nodiscard]] static bool ShouldHaveMeta(std::string_view lowerFileName);

    /// @brief このフォルダは .meta (`<フォルダ名>.meta` を隣に置く) を持つべきか。
    /// @param folderName フォルダ名のみ (パスではない)。
    /// @note フォルダも参照対象になりうるため guid を持たせる。ドット始まり・生成物置き場は除外する。
    [[nodiscard]] static bool ShouldHaveFolderMeta(std::string_view folderName);

    /// @brief 同じ guid を名乗る実体が 2 つ以上見つかった記録。
    /// @note 索引は先勝ちで、後から来た側は自分の guid を名乗り続けるが PathFromGuid は先勝ち側を返す。参照が静かに吸われるだけで壊れた形で現れないため、修復には衝突組の記録が要る。
    struct GuidConflict {
        std::string guid;
        std::string keptPath;       ///< 索引が採用しているアセット絶対パス。
        std::string duplicatePath;  ///< 索引から弾かれた側。
    };

    /// @brief 現在の衝突一覧。呼び出しのたびに .meta を読み直し、解消済みの記録は捨てる。
    /// @note ディスクを叩くので毎フレームではなく要求されたときだけ呼ぶこと。
    [[nodiscard]] static std::vector<GuidConflict> GuidConflicts();

    /// @brief 記録されている衝突の件数。ディスクを叩かないので毎フレーム呼んでよい。
    /// @note 解消済みの記録が残りうる。正確な数は GuidConflicts() で取り直す。
    [[nodiscard]] static size_t GuidConflictCount();

    /// @brief absPath の .meta へ新しい guid を振り直し、索引を更新する。
    /// @note 参照側は書き換えない。既存の `"guid:"` 参照は先勝ち側へ解決済みのため、後勝ち側にだけ新 guid を振れば解決先を変えずに重複が消える。
    static bool ReassignGuid(const std::string& absPath, std::string& outNewGuid);

    /// @brief Library/Baked の掃除結果。
    struct BakedSweepResult {
        size_t   scanned    = 0;  ///< 走査した guid ディレクトリ数。
        size_t   removed    = 0;  ///< 消した孤児コンテナ数。
        uint64_t bytesFreed = 0;
        bool     aborted    = false;  ///< 安全弁が働いて 1 件も消していない。
        std::string abortReason;      ///< aborted のときだけ埋まる。
        std::vector<std::string> removedGuids;  ///< 後片付け (ImportCache 等) 用。
    };

    /// @brief `Library/Baked/<guid>/` のうち、どの .meta もその guid を名乗っていないものを消す。
    /// @param dryRun true なら数えるだけで何も消さない。
    /// @pre Init 済みであること。
    /// @note 中身は原本から焼き直せるため、参照を失った時点で価値が無い。
    /// @note 安全弁は孤児の割合でなく索引の健全性で張る。索引が空 / Assets ルートを読めなければ 1 件も消さず aborted で返す。
    static BakedSweepResult SweepOrphanedBaked(bool dryRun);

    /// @brief 登録済みアセット数 (デバッグ / EditorUI 表示用)。
    [[nodiscard]] static size_t Count();

    /// @brief Init に渡された Assets ルート (末尾 '/' 付き)。`"Assets/..."` 相対化に使う。
    [[nodiscard]] static std::string AssetsRoot();
};

} // namespace fbzz::asset
