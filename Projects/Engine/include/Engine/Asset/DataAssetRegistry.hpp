/// @file    DataAssetRegistry.hpp
/// @brief   DataAsset (純共有 ScriptableObject) の「パス → 共有 1 実体」キャッシュ兼 TOML 入出力。
/// @author  Hasegawa Jin
/// @date    2026-07-01
///
/// @note Registry が .fzdata パス単位で DataAsset を 1 つだけ生成・キャッシュし、Asset<T>::Get() は常にこの共有
///       実体を返す (ScriptableObject は複数参照が同じ実体を共有する)。Inspector の編集は参照側すべてに即反映される。
/// @note 静的メソッドは Engine 側 TU に実体を持つ (AssetManager と同方針)。スクリプト DLL からも DLL 境界越しに呼べ、
///       Engine 実装へ downcast せず値を引ける。
#pragma once
#include <string>

namespace fbzz::asset {

class DataAsset;

class DataAssetRegistry {
public:
    /// パスから共有 1 実体を解決する。未ロードなら .fzdata を読み生成・キャッシュする。
    /// 失敗 (未存在・型未登録・パース失敗) 時は nullptr。
    static DataAsset* Resolve(const std::string& path);

    /// 現在キャッシュしている実体を .fzdata へ書き戻す (Inspector 編集後の自動保存に使う)。
    /// 未キャッシュなら false。
    static bool Save(const std::string& path);

    /// 新規 DataAsset を既定値で生成し .fzdata として書き出す。
    /// 既に存在する場合は失敗。typeName は DataAssetFactory に登録済みである必要がある。
    static bool Create(const std::string& path, const std::string& typeName);

    /// キャッシュ済み実体の型名を返す (Inspector がどの型を描画するか等に使う)。未キャッシュなら空。
    static std::string TypeOf(const std::string& path);

    /// @name Undo スナップショット
    /// @{
    /// @note DataAsset は多態基底で値コピーできないため `.mat` のような「編集前の値を
    ///       丸ごと控える」方式が使えない。TOML 直列化をスナップショット実体とし、
    ///       Editor 側は不透明文字列として扱う。

    /// キャッシュ済み実体を .fzdata と同じ TOML テキストへ書き出す。未キャッシュなら空。
    static std::string Snapshot(const std::string& path);

    /// @brief Snapshot() の文字列を、キャッシュ済みの同じ実体へ読み戻す。
    /// @note Asset<T>::Get() が返したポインタを保持する参照側 (スクリプト・Inspector) が
    ///       いるため、実体は作り直さずアドレスを維持したまま中身だけ差し替える。
    static bool RestoreSnapshot(const std::string& path, const std::string& snapshot);

    /// @brief 指定ファイルを参照しているキャッシュ済み実体を、ディスクから読み直す。
    /// @param absPath ファイル監視が返す絶対パス
    /// @return 読み直した件数。0 ならこのファイルはキャッシュに載っていない
    /// @note RestoreSnapshot と同じ理由で、型が変わらない限りアドレスを維持し中身だけ差し替える。
    static int ReloadFile(const std::string& absPath);

    /// @brief DLL ホットリロード時にキャッシュを破棄する。
    /// @note DataAsset の仮想デストラクタは DLL コード内にあるため、FreeLibrary 前に破棄
    ///       しないとリロード後にアクセス違反になる。破棄後は次回 Resolve で遅延再ロードされる。
    static void ClearCache();
    /// @}
};

} // namespace fbzz::asset
