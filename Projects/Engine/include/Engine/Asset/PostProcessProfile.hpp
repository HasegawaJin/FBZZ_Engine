/// @file    PostProcessProfile.hpp
/// @brief   ポストプロセス / 高度グラフィクスの「ルック」を .fzdata (DataAsset) として共有するプロファイル。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// 中身は VolumeOverride のリスト。リストに載っている効果だけが適用される。
///
/// @note DataAsset に載せる理由: 同じルックを複数のボリューム / シーンで使うと設定値が複製され、
///       調整のたびに全箇所を手で直すことになる (DataAsset.hpp が解決対象として挙げている
///       「値が N 個に複製され、リバランスが各個編集になる」問題そのもの)。DataAsset に載せる
///       ことで .fzdata の .meta/GUID/リネーム追随・DataAssetRef のドラッグ&ドロップ・
///       DataAssetRegistry の共有キャッシュ・Asset<T> の型安全アクセスが既存機構のまま手に入る。
///
/// @note 固定の設定構造体 + bool マスクをやめた理由: 以前は 20 セクション分の値を常に保持し
///       「何を上書きするか」を別の PostProcessOverrides マスクで表していたため、差分プロファイル
///       でも使わない 19 セクションがファイルに残り Inspector も常に全セクションを並べていた。
///       オーバーライドのリストにすると、ファイルの中身がそのまま「このプロファイルの責務」に
///       なり、マスクという二重管理も消える。
#pragma once
#include <Engine/Asset/DataAsset.hpp>
#include <Engine/Asset/VolumeOverride.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <memory>
#include <vector>

namespace fbzz::asset {

class PostProcessProfile final : public DataAsset {
public:
    /// Asset<T> のピッカー絞り込み・ドロップ型チェックに使われる。
    /// .fzdata の "type" キーにもこの名前が保存される。
    static constexpr const char* TYPE_NAME = "PostProcessProfile";
    const char* GetTypeName() const override { return TYPE_NAME; }

    void Reflect(scene::IReflector& r) override;

    /// このプロファイルが持つ効果。リストの順に適用される。
    /// @note 効果ごとに別クラスで保持するパラメーターの数も型も違うため、共通の基底を値で
    ///       持つことはできず unique_ptr のベクタにした。
    std::vector<std::unique_ptr<VolumeOverride>> overrides;

    /// 解決中の設定へ、有効なオーバーライドを weight で順に混ぜる。
    void ApplyTo(renderer::VolumeSettings& target, float weight) const;

    /// 指定型のオーバーライドを既に持っているか (Add Override メニューの重複防止)。
    [[nodiscard]] bool Contains(const char* typeName) const;

    /// 深いコピー。unique_ptr を持つためコピーコンストラクタが使えない。
    [[nodiscard]] std::vector<std::unique_ptr<VolumeOverride>> CloneOverrides() const;
};

} // namespace fbzz::asset

/// @note DataAssetFactory への登録は PostProcessProfile.cpp で行う。ヘッダーに置かないのは、
///       FBZZ_REGISTER_DATA_ASSET が無名名前空間の静的初期化を展開するため、置くと include
///       した翻訳単位ごとに登録コードが複製されるから。
