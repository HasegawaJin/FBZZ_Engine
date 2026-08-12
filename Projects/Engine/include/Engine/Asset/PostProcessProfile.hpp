// FBZZ Engine
// PostProcessProfile.hpp | fbzz::asset
// ポストプロセス設定を .fzdata (DataAsset) として共有するプロファイルアセット
//
// WHY DataAsset に載せるか:
//   従来 PostProcessVolumeComponent は PostProcessSettings を実体で抱えていた。
//   同じルックを複数のボリューム / シーンで使うと 30 以上のフィールドが複製され、
//   調整のたびに全箇所を手で直すことになる。これは DataAsset.hpp が解決対象として
//   挙げている問題そのもの (「値が N 個に複製され、リバランスが各個編集になる」)。
//   DataAsset に載せることで以下がすべて既存機構のまま手に入る:
//     - .fzdata は AssetDatabase の拡張子リストにあるため .meta / GUID / リネーム追随
//     - DataAssetRef により Inspector のドラッグ&ドロップスロット
//     - DataAssetRegistry による「パス → 共有 1 実体」キャッシュ
//     - Asset<T> による型安全なスクリプトアクセス
//
// WHY 専用形式 (.fzpp) を廃止したか:
//   ポストプロセスアセットの形式が 2 つ並ぶと、読み手に毎回「どちらを使うのか」を
//   判断させることになる。移行対象の実ファイルが存在しなかったため一本化した。
#pragma once
#include <Engine/Asset/DataAsset.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/PostProcessBlend.hpp>

namespace fbzz::asset {

class PostProcessProfile final : public DataAsset {
public:
    // Asset<T> のピッカー絞り込み・ドロップ型チェックに使われる。
    // .fzdata の "type" キーにもこの名前が保存される。
    static constexpr const char* TYPE_NAME = "PostProcessProfile";

    const char* GetTypeName() const override { return TYPE_NAME; }

    // WHY FBZZ_FIELD マクロ群を使わず手書きするか:
    //   PostProcessSettings はレンダラー層の既存構造体であり、フィールド宣言を
    //   FBZZ_FIELD へ置き換えると RenderSettings.hpp が Script.hpp に依存してしまう
    //   (依存方向の逆流)。手書きにすることで、各フィールドへ適正なレンジ・
    //   ツールチップ・カラーピッカー指定を個別に与えられる利点もある。
    void Reflect(scene::IReflector& r) override;

    renderer::PostProcessSettings settings;

    // このプロファイルが上書きするセクション。false のセクションは
    // ブレンド時にベース側の値をそのまま通す。
    //
    // WHY 必要か: マスクが無いと、プロファイルは常に 50 以上の全フィールドを
    //      持ち回ることになり、「洞窟では fog だけ変えたい」場合でも
    //      画面全体のルックを書き切る必要がある。結果としてベースを変更するたびに
    //      それを複製した全ボリュームを手直しすることになり、
    //      アセット化で解消したはずの複製問題がボリューム単位で再発する。
    renderer::PostProcessOverrides overrides;
};

} // namespace fbzz::asset

// NOTE: DataAssetFactory への登録は PostProcessProfile.cpp で行う。
// WHY ヘッダーに置かないか: FBZZ_REGISTER_DATA_ASSET は無名名前空間の静的初期化を
//     展開するため、ヘッダーに置くと include した翻訳単位ごとに登録コードが複製される。
