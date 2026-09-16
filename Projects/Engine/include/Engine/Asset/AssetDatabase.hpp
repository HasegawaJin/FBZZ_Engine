/// @file    AssetDatabase.hpp
/// @brief   GUID ⇄ アセットパスの双方向インデックス。
/// @author  Hasegawa Jin
/// @date    2026-07-08
///
/// 各アセット (ファイルおよびフォルダ) は隣接する "<名前>.meta" サイドカーの
/// [meta] guid で恒久 ID を持つ。フォルダの .meta は親ディレクトリに置く
/// (例: Assets/Textures/ → Assets/Textures.meta)。

// 参照側 (.mat / .scene / コンポーネント) は "guid:<32hex>" 形式の文字列を保存でき、
// AssetManager::ResolvePath がロード時に実パスへ解決する。
// 読み手のために "|<プロジェクト相対パス>" を後ろへ併記できる (GuidRefCodec が付ける)。
// 権威は guid 側で、ヒントは解決できなかったときの復旧にしか使わない。
// WHY: パス文字列参照はリネーム・移動で全参照が壊れる。GUID は生成後不変なので、
//      ファイルを動かしてもインデックスの再構築だけで参照が生き続ける。
//
// スレッド規約: Init はメインスレッドで 1 回。以後の参照系 (PathFromGuid 等) は
// 内部 mutex で保護されるため任意スレッドから呼べる。
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset {

class AssetDatabase {
public:
    // "guid:" 参照プレフィックス。この後ろに 32 桁 hex が続く。
    static constexpr std::string_view kGuidPrefix = "guid:";

    // assetsRoot (例 "C:/proj/Assets/") 配下を走査してインデックスを構築する。
    // .meta が無い / guid の無い対象アセットには .meta を自動生成する (自己修復)。
    // ただし FBX は Import 実行時まで原本の隣に .meta を作らない。
    // AssetManager::Init から呼ばれる。
    static void Init(const std::string& assetsRoot);
    static void Shutdown();

    // guid (32hex) → アセット絶対パス。未登録なら空文字列。
    [[nodiscard]] static std::string PathFromGuid(const std::string& guid);

    // アセット絶対パス → guid。未登録なら .meta を生成して新 guid を返す。
    // 失敗 (.meta 書き込み不可等) なら空文字列。
    [[nodiscard]] static std::string GuidFromPath(const std::string& absPath);

    // アセット絶対パス → 既存 .meta の guid。未登録 / .meta 未作成なら空文字列を返す。
    // WHY: AssetBrowser の存在確認やパス解決が、Import 前の FBX に .meta を発行しないようにする。
    [[nodiscard]] static std::string TryGetGuidFromPath(const std::string& absPath);

    // リネーム / 移動フック。guid は不変のままパスの索引だけ付け替える。
    // 呼び出し側 (AssetBrowser) は本体と .meta を両方移動した後に呼ぶこと。
    static void OnAssetMoved(const std::string& oldAbsPath, const std::string& newAbsPath);

    // 削除されたアセットを索引から除去する。フォルダなら配下も一括で除去する。
    // WHY: 旧パスの GUID を残すと、同じ場所へ別アセットを作り直した際に
    //      新しい .meta より古い GUID を返し、参照先が静かに入れ替わる。
    static void OnAssetRemoved(const std::string& absPath);

    // "guid:xxxx" 形式か判定する (ResolvePath の分岐用)。
    [[nodiscard]] static bool IsGuidRef(std::string_view ref) {
        return ref.rfind(kGuidPrefix, 0) == 0;
    }

    // 参照へ人が読めるパスを併記するときの区切り。
    //   guid:<32hex>[<サブアセット接尾辞>]|<プロジェクト相対パス>
    //
    // WHY 併記するか: guid 単体は「どのファイルか」をファイルの中から一切辿れない。
    //     特に Library/Baked の導出 guid は DeriveGuid の計算結果でしかなく、
    //     どこにも文字列として存在しないため grep でも見つからない。
    //     権威は guid のまま、後ろに読める形を足して、人と外部ツールが
    //     ファイルを見ただけで対象を特定できるようにする (Godot の uid + path と同じ)。
    //     '|' は Windows のファイル名に使えないため、パスと衝突しない。
    static constexpr char kRefHintSeparator = '|';

    // 参照文字列から guid 本体 (32hex) だけを取り出す。
    // "guid:" プレフィックス・パスヒント・サブアセット接尾辞をすべて落とす。
    // guid 参照でなければ空文字列。
    [[nodiscard]] static std::string GuidFromRef(std::string_view ref);

    // 参照文字列に併記されたパスヒント (プロジェクト相対) を取り出す。無ければ空文字列。
    [[nodiscard]] static std::string HintFromRef(std::string_view ref);

    // Init に渡された Assets ルートの 1 つ上 (末尾 '/' 付き)。
    // パスヒントと索引ファイルをプロジェクト相対で書くために使う。
    [[nodiscard]] static std::string ProjectRoot();

    // guid → パスの索引を Library/AssetIndex.toml へ書き出す。
    //
    // WHY ファイルに落とすか: 索引はこれまでメモリ上にしか存在せず、エディターを
    //     起動していない状態では guid を 1 つも解決できなかった。導出 guid に至っては
    //     起動していても grep で辿れない。1 ファイル読めば全部引ける形を用意する。
    static void SaveIndexFile();

    // 索引が変わっていれば SaveIndexFile する。毎フレーム呼んでよい。
    static void FlushIndexFile();

    // 32 桁 hex の新規 GUID を生成する (Unity と同形式)。
    [[nodiscard]] static std::string GenerateGuid();

    // 原本の GUID とサブキーから、生成物の GUID を決定論的に導出する。
    //
    // WHY 乱数ではなく導出か:
    //   import 生成物 (.anim / .mat) は Library/Baked に隔離され、Library は .gitignore 済み。
    //   乱数 GUID を .meta に保存する方式では、クローン直後の再インポートで別の GUID が
    //   振られ、.animcontroller や .scene の参照が全部切れる (手元では動くのに他所で壊れる)。
    //   原本 FBX の GUID (これは Assets 側 .meta にあり git 管理下) とサブキーから導けば、
    //   どの環境でも・Library を消しても同じ値に戻る。
    //
    //   subKey は baked コンテナ内の相対パス ("anims/MiniBot@Idle.anim" 等)。
    [[nodiscard]] static std::string DeriveGuid(std::string_view sourceGuid,
                                                std::string_view subKey);

    // Library/Baked 配下を走査して導出 GUID で索引へ登録する。
    // Init から呼ばれる。libraryRoot が無ければ何もしない。
    static void IndexBakedLibrary(const std::string& libraryBakedRoot);

    // このファイルは .meta を持つべきか。引数はファイル名のみ (パスではない、小文字化済み)。
    // 除外リスト方式: 再生成できる派生物 (.fzasset/.mesh/.skel/.cso/.generated.hpp/ビルド出力)
    // と無拡張ファイル以外は、.md / .txt / .pdf のような資料も含めて GUID を持つ。
    [[nodiscard]] static bool ShouldHaveMeta(std::string_view lowerFileName);

    // このフォルダは .meta ("<フォルダ名>.meta" を隣に置く) を持つべきか。
    // WHY: フォルダも参照対象になりうる (デフォルト保存先、検索スコープ、パック単位)。
    //      パスで覚えるとリネームで壊れるのはファイルと同じなので、guid を持たせる。
    //      引数はフォルダ名のみ (パスではない)。ドット始まり・生成物置き場は除外する。
    [[nodiscard]] static bool ShouldHaveFolderMeta(std::string_view folderName);

    // 同じ guid を名乗る実体が 2 つ以上見つかった記録。
    //
    // WHY 記録を残すか: 索引は先勝ちで、後から来た側は «自分の guid» を名乗り続けるのに
    //     PathFromGuid は先勝ちした別の実体を返す。参照が静かに他人へ吸われるだけで、
    //     壊れた形では現れない。修復には「どれとどれが衝突しているか」が要る。
    struct GuidConflict {
        std::string guid;
        std::string keptPath;       // 索引が採用しているアセット絶対パス
        std::string duplicatePath;  // 索引から弾かれた側
    };

    // 現在の衝突一覧。呼び出しのたびに .meta を読み直し、解消済みの記録は捨てる。
    // ディスクを叩くので毎フレームではなく、要求されたときだけ呼ぶこと。
    [[nodiscard]] static std::vector<GuidConflict> GuidConflicts();

    // 記録されている衝突の件数。ディスクを叩かないので毎フレーム呼んでよい。
    // 解消済みの記録が残りうるため、正確な数は GuidConflicts() で取り直す。
    [[nodiscard]] static size_t GuidConflictCount();

    // absPath の .meta へ新しい guid を振り直し、索引を更新する。
    //
    // WHY 参照を書き換えないか: 既存の "guid:" 参照は衝突中も先勝ちした側へ解決されている。
    //     後から来た側に新しい guid を振れば、参照の解決先は今日と同じまま重複だけが消える。
    //     参照側を触らないので、どのファイルを壊す可能性も無い。
    static bool ReassignGuid(const std::string& absPath, std::string& outNewGuid);

    // Library/Baked の掃除結果。
    struct BakedSweepResult {
        size_t   scanned    = 0;  // 走査した guid ディレクトリ数
        size_t   removed    = 0;  // 消した孤児コンテナ数
        uint64_t bytesFreed = 0;
        bool     aborted    = false;  // 安全弁が働いて 1 件も消していない
        std::string abortReason;      // aborted のときだけ埋まる
        std::vector<std::string> removedGuids;  // 後片付け (ImportCache 等) 用
    };

    // Library/Baked/<guid>/ のうち、どの .meta もその guid を名乗っていないものを消す。
    //
    // guid は生成物の置き場所そのものなので、原本が消えたり guid が振り直されたりすると
    // 古いコンテナがそのまま残る。中身は原本から焼き直せるため、参照を失った時点で
    // 価値が無い。放っておくと索引の走査対象と容量だけが増え続ける。
    //
    // WHY 割合で止めないか: «孤児が多いこと» 自体は異常ではない (今日 8 割が孤児という
    //     状態も普通に起きる)。危ないのは «索引が不完全なまま消すこと» なので、
    //     安全弁は索引側の健全性で張る。索引が空、または Assets ルートを読めなければ
    //     1 件も消さずに aborted で返す。
    //
    // dryRun なら数えるだけで何も消さない。Init 済みであること。
    static BakedSweepResult SweepOrphanedBaked(bool dryRun);

    // 登録済みアセット数 (デバッグ / EditorUI 表示用)。
    [[nodiscard]] static size_t Count();

    // Init に渡された Assets ルート (末尾 '/' 付き)。"Assets/..." 相対化に使う。
    [[nodiscard]] static std::string AssetsRoot();
};

} // namespace fbzz::asset
