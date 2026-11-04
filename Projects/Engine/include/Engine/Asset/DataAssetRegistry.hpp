/// @file    DataAssetRegistry.hpp
/// @brief   DataAsset (純共有 ScriptableObject) の「パス → 共有 1 実体」キャッシュ兼 TOML 入出力。
/// @author  Hasegawa Jin
/// @date    2026-07-01

/// @note Registry が .fzdata パス単位で DataAsset を 1 つだけ生成・キャッシュし、Asset<T>::Get() は常にこの共有
/// @note 実体を返す (ScriptableObject は複数参照が同じ実体を共有する)。Inspector の編集は参照側すべてに即反映される。
/// @note 静的メソッドは Engine 側 TU に実体を持つ (AssetManager と同方針)。スクリプト DLL からも DLL 境界越しに呼べ、
/// @note Engine 実装へ downcast せず値を引ける。
#pragma once
#include <Engine/Api.hpp>
#include <string>

namespace fbzz::asset {

class DataAsset;

class DataAssetRegistry {
public:
    /// @note パスから共有 1 実体を解決する。未ロードなら .fzdata を読み生成・キャッシュする。
    /// @note 失敗 (未存在・型未登録・パース失敗) 時は nullptr。
    FBZZ_ENGINE_API static DataAsset* Resolve(const std::string& path);

    /// @note 現在キャッシュしている実体を .fzdata へ書き戻す (Inspector 編集後の自動保存に使う)。
    /// @note 未キャッシュなら false。
    FBZZ_ENGINE_API static bool Save(const std::string& path);

    /// @note 新規 DataAsset を既定値で生成し .fzdata として書き出す。
    /// @note 既に存在する場合は失敗。typeName は DataAssetFactory に登録済みである必要がある。
    FBZZ_ENGINE_API static bool Create(const std::string& path, const std::string& typeName);

    /// @note キャッシュ済み実体の型名を返す (Inspector がどの型を描画するか等に使う)。未キャッシュなら空。
    FBZZ_ENGINE_API static std::string TypeOf(const std::string& path);

    /// @name Undo スナップショット
    /// @{
    /// @note DataAsset は多態基底で値コピーできないため `.mat` のような「編集前の値を
    /// @note 丸ごと控える」方式が使えない。TOML 直列化をスナップショット実体とし、
    /// @note Editor 側は不透明文字列として扱う。

    /// @note キャッシュ済み実体を .fzdata と同じ TOML テキストへ書き出す。未キャッシュなら空。
    FBZZ_ENGINE_API static std::string Snapshot(const std::string& path);

    /// @brief Snapshot() の文字列を、キャッシュ済みの同じ実体へ読み戻す。
    /// @note Asset<T>::Get() が返したポインタを保持する参照側 (スクリプト・Inspector) が
    /// @note いるため、実体は作り直さずアドレスを維持したまま中身だけ差し替える。
    FBZZ_ENGINE_API static bool RestoreSnapshot(const std::string& path, const std::string& snapshot);

    /// @brief 指定ファイルを参照しているキャッシュ済み実体を、ディスクから読み直す。
    /// @param absPath ファイル監視が返す絶対パス
    /// @return 読み直した件数。0 ならこのファイルはキャッシュに載っていない
    /// @note RestoreSnapshot と同じ理由で、型が変わらない限りアドレスを維持し中身だけ差し替える。
    FBZZ_ENGINE_API static int ReloadFile(const std::string& absPath);

    /// @brief DLL ホットリロード時にキャッシュを破棄する。
    /// @note DataAsset の仮想デストラクタは DLL コード内にあるため、FreeLibrary 前に破棄
    /// @note しないとリロード後にアクセス違反になる。破棄後は次回 Resolve で遅延再ロードされる。
    FBZZ_ENGINE_API static void ClearCache();
    /// @}
};

} /// @note namespace fbzz::asset
