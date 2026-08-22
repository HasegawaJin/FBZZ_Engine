// FBZZ Engine
// PostProcessVolumeComponent.hpp | fbzz::scene
// ポストプロセス / 高度グラフィクス設定を、シーン内で領域・優先度つきに適用するボリューム。
//
// WHY: ルック設定はプロジェクト全体の固定値ではなく「その場所でどう見えるか」なので、
//      屋外・洞窟・カットシーンのように場所ごとに切り替えられる必要がある。
//      Unity の Volume と同じく、GameObject に付けたボリュームへプロファイルを
//      アサインしていく形にすることで、シーンを見れば効き方が分かるようにする。
//
// 適用の仕組み (RenderSystem が毎フレーム合成する):
//      ベース = VolumeSettings の既定値 (+ スクリプトのランタイム上書き)
//      有効なボリュームを priority 昇順に、重み付きで順次ブレンドする
//      重み = blendWeight × (isGlobal ? 1 : 距離ウェイト)
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>

namespace fbzz::scene {

struct PostProcessVolumeComponent {
    bool enabled = true;

    // 適用するプロファイル (.fzdata)。未アサインのボリュームは何もしない。
    // WHY インライン設定を持たせないか:
    //      同じルックを複数のボリューム / シーンで使うと 50 以上のフィールドが複製され、
    //      調整のたびに全箇所を手で直すことになる。プロファイル参照に一本化すれば
    //      1 か所の編集が全参照へ届く。「インラインもプロファイルも書ける」状態は、
    //      読み手に毎回「今どちらが効いているのか」を確かめさせるコストを生む。
    asset::Asset<asset::PostProcessProfile> profile;

    // true: 距離に関係なく常時適用 / false: influenceRadius の球内でのみ適用
    bool isGlobal = true;

    // 合成順序。小さいものから順にブレンドするため、大きいほど後勝ちで強く出る。
    // WHY 必要か: 「屋外(全体) → 洞窟(局所) → ボス演出(最優先)」のような
    //      重ね方を決定的にするため。順序が View の走査順に依存すると、
    //      GameObject を作り直しただけで見た目が変わる。
    int priority = 0;

    // このボリューム自体の効き具合 (0=無効, 1=完全適用)
    float blendWeight = 1.0f;

    // isGlobal=false 時の球影響半径 [m]
    float influenceRadius = 10.0f;

    // 境界の内側でこの距離だけかけて 0 へフェードする [m]
    // WHY 必要か: 半径の内外で 0/1 に切り替わると、境界をまたいだ瞬間に
    //      ルックが飛んで見える。
    float blendDistance = 2.0f;

    const char* GetTypeName() const { return "PostProcessVolume"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        // DataAssetRef の Field オーバーロードにより、Inspector には
        // .fzdata のドラッグ&ドロップスロットが出る。
        // シリアライズ時は path 文字列として保存され、GuidRefCodec が guid: へ変換する。
        r.BeginField("profile", "Profile");
        r.Field("profile", profile.ref);
        r.EndField();
        r.Field("isGlobal",        isGlobal);
        r.Field("priority",        priority);
        r.FloatRange("blendWeight", blendWeight, 0.0f, 1.0f);
        r.Field("influenceRadius", influenceRadius);
        r.Field("blendDistance",   blendDistance);
    }

    // 適用するプロファイル。未アサイン / パス切れなら nullptr。
    // WHY 既定値へフォールバックしないか:
    //      参照が切れたボリュームが「既定のルック」を主張し始めると、
    //      priority 次第で他のボリュームを打ち消してしまう。何もしない方が、
    //      壊れている箇所が画面ではなく Inspector の警告に留まる。
    [[nodiscard]] const asset::PostProcessProfile* Resolve() const
    {
        return profile.Get();
    }
};

} // namespace fbzz::scene
