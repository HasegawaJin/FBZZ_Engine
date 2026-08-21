// FBZZ Engine
// DataAssetRegistry.hpp | fbzz::asset
// DataAsset (純共有 ScriptableObject) の「パス → 共有 1 実体」キャッシュ兼 TOML 入出力。
//
// 設計意図 (WHY):
//   ScriptableObject の本質は「データを 1 か所で持ち、複数の参照が同じ実体を共有する」こと。
//   Registry が .fzdata パス単位で DataAsset を 1 つだけ生成・キャッシュし、Asset<T>::Get() は
//   常にこの共有実体を返す。Inspector の編集はこの実体を直接書き換えるため、参照側すべてに即反映される。
//
//   静的メソッドは Engine 側 TU に実体を持ち (AssetManager と同方針)、スクリプト DLL からも
//   DLL 境界越しに呼べる。これによりスクリプトは Engine 実装へ downcast 依存せずに値を引ける。
#pragma once
#include <string>

namespace fbzz::asset {

class DataAsset;

class DataAssetRegistry {
public:
    // パスから共有 1 実体を解決する。未ロードなら .fzdata を読み生成・キャッシュする。
    // 失敗 (未存在・型未登録・パース失敗) 時は nullptr。
    static DataAsset* Resolve(const std::string& path);

    // 現在キャッシュしている実体を .fzdata へ書き戻す (Inspector 編集後の自動保存に使う)。
    // 未キャッシュなら false。
    static bool Save(const std::string& path);

    // 新規 DataAsset を既定値で生成し .fzdata として書き出す。
    // 既に存在する場合は失敗。typeName は DataAssetFactory に登録済みである必要がある。
    static bool Create(const std::string& path, const std::string& typeName);

    // キャッシュ済み実体の型名を返す (Inspector がどの型を描画するか等に使う)。未キャッシュなら空。
    static std::string TypeOf(const std::string& path);

    // ── Undo スナップショット ────────────────────────────────────────────────
    // WHY: DataAsset は多態基底で値コピーできないため、.mat のような
    //      「編集前の値をまるごと控える」方式が使えない。TOML 直列化を
    //      スナップショットの実体として使い、Editor 側は不透明な文字列として扱う。

    // キャッシュ済み実体を .fzdata と同じ TOML テキストへ書き出す。未キャッシュなら空。
    static std::string Snapshot(const std::string& path);

    // Snapshot() の文字列を、キャッシュ済みの「同じ実体」へ読み戻す。
    // WHY 実体を作り直さないか: Asset<T>::Get() が返したポインタを保持している
    //      参照側 (スクリプト・Inspector) がいる。アドレスは維持し中身だけ差し替える。
    static bool RestoreSnapshot(const std::string& path, const std::string& snapshot);

    // 指定ファイルを参照しているキャッシュ済み実体を、ディスクから読み直す。
    // @param absPath ファイル監視が返す絶対パス
    // @return 読み直した件数。0 ならこのファイルはキャッシュに載っていない
    //
    // WHY 実体を作り直さないか: RestoreSnapshot と同じ理由で、Asset<T>::Get() が返した
    //      ポインタを保持している参照側がいる。型が変わっていない限りアドレスは維持する。
    static int ReloadFile(const std::string& absPath);

    // DLL ホットリロード時にキャッシュを破棄する。
    // WHY: キャッシュした DataAsset の仮想デストラクタは DLL コード内にあるため、
    //      FreeLibrary 前にここで破棄しておかないとリロード後にアクセス違反になる。
    //      破棄後は次回 Resolve でディスクから遅延再ロードされる。
    static void ClearCache();
};

} // namespace fbzz::asset
