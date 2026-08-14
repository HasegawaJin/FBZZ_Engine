// FBZZ Engine
// PostProcessProfile.hpp | fbzz::asset
// ポストプロセス / 高度グラフィクスの「ルック」を .fzdata (DataAsset) として共有するプロファイル。
//
// 中身は VolumeOverride のリスト。リストに載っている効果だけが適用される。
//
// WHY DataAsset に載せるか:
//   同じルックを複数のボリューム / シーンで使うと設定値が複製され、調整のたびに
//   全箇所を手で直すことになる。これは DataAsset.hpp が解決対象として挙げている
//   問題そのもの (「値が N 個に複製され、リバランスが各個編集になる」)。
//   DataAsset に載せることで以下がすべて既存機構のまま手に入る:
//     - .fzdata は AssetDatabase の拡張子リストにあるため .meta / GUID / リネーム追随
//     - DataAssetRef により Inspector のドラッグ&ドロップスロット
//     - DataAssetRegistry による「パス → 共有 1 実体」キャッシュ
//     - Asset<T> による型安全なスクリプトアクセス
//
// WHY 固定の設定構造体 + bool マスクをやめたか:
//   以前は 20 セクション分の値を常に保持し、「何を上書きするか」を別の
//   PostProcessOverrides マスクで表していた。差分プロファイルでも使わない
//   19 セクションがファイルに残り、Inspector も常に全セクションを並べていた。
//   オーバーライドのリストにすると、ファイルの中身がそのまま「このプロファイルの責務」
//   になり、マスクという二重管理も消える。
#pragma once
#include <Engine/Asset/DataAsset.hpp>
#include <Engine/Asset/VolumeOverride.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <memory>
#include <vector>

namespace fbzz::asset {

class PostProcessProfile final : public DataAsset {
public:
    // Asset<T> のピッカー絞り込み・ドロップ型チェックに使われる。
    // .fzdata の "type" キーにもこの名前が保存される。
    static constexpr const char* TYPE_NAME = "PostProcessProfile";
    const char* GetTypeName() const override { return TYPE_NAME; }

    void Reflect(scene::IReflector& r) override;

    // このプロファイルが持つ効果。リストの順に適用される。
    // WHY unique_ptr のベクタか: 効果ごとに別クラスで、保持するパラメーターの数も型も違う。
    //     共通の基底を値で持つことはできない。
    std::vector<std::unique_ptr<VolumeOverride>> overrides;

    // 解決中の設定へ、有効なオーバーライドを weight で順に混ぜる。
    void ApplyTo(renderer::VolumeSettings& target, float weight) const;

    // 指定型のオーバーライドを既に持っているか (Add Override メニューの重複防止)。
    [[nodiscard]] bool Contains(const char* typeName) const;

    // 深いコピー。unique_ptr を持つためコピーコンストラクタが使えない。
    [[nodiscard]] std::vector<std::unique_ptr<VolumeOverride>> CloneOverrides() const;
};

} // namespace fbzz::asset

// NOTE: DataAssetFactory への登録は PostProcessProfile.cpp で行う。
// WHY ヘッダーに置かないか: FBZZ_REGISTER_DATA_ASSET は無名名前空間の静的初期化を
//     展開するため、ヘッダーに置くと include した翻訳単位ごとに登録コードが複製される。
