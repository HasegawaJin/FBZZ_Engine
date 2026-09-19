/// @file    PostProcessVolumeComponent.hpp
/// @brief   ポストプロセス / 高度グラフィクス設定を、シーン内で領域・優先度つきに適用するボリューム。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note ルック設定は「その場所でどう見えるか」なので、屋外・洞窟・カットシーンのように場所ごとに
///       切り替えられる必要がある。Unity の Volume と同じく GameObject にプロファイルを
///       アサインする形にし、シーンを見れば効き方が分かるようにする。
///
/// 適用の仕組み (RenderSystem が毎フレーム合成する):
/// ベース = VolumeSettings の既定値 (+ スクリプトのランタイム上書き)
/// 有効なボリュームを priority 昇順に、重み付きで順次ブレンドする
/// 重み = blendWeight × (isGlobal ? 1 : 距離ウェイト)
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>

namespace fbzz::scene {

struct PostProcessVolumeComponent {
    bool enabled = true;

    /// 適用するプロファイル (.fzdata)。未アサインのボリュームは何もしない。
    /// @note インライン設定を持たせないのは、複数のボリューム / シーンで同じルックを使うと
    ///       フィールドが複製され調整が全箇所に及ぶため。プロファイル参照に一本化し、
    ///       「今どちらが効いているか」を確かめるコストを無くす。
    asset::Asset<asset::PostProcessProfile> profile;

    /// true: 距離に関係なく常時適用 / false: influenceRadius の球内でのみ適用
    bool isGlobal = true;

    /// 合成順序。小さいものから順にブレンドするため、大きいほど後勝ちで強く出る。
    /// @note 「屋外(全体) → 洞窟(局所) → ボス演出(最優先)」のような重ね方を決定的にするため。
    ///       順序が View の走査順に依存すると、GameObject を作り直しただけで見た目が変わる。
    int priority = 0;

    /// このボリューム自体の効き具合 (0=無効, 1=完全適用)
    float blendWeight = 1.0f;

    /// isGlobal=false 時の球影響半径 [m]
    float influenceRadius = 10.0f;

    /// 境界の内側でこの距離だけかけて 0 へフェードする [m]
    /// @note 半径の内外で 0/1 に切り替わると、境界をまたいだ瞬間にルックが飛んで見える。
    float blendDistance = 2.0f;

    const char* GetTypeName() const { return "PostProcessVolume"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        /// @note DataAssetRef の Field オーバーロードにより、Inspector には
        ///       .fzdata のドラッグ&ドロップスロットが出る。
        ///       シリアライズ時は path 文字列として保存され、GuidRefCodec が guid: へ変換する。
        r.BeginField("profile", "Profile");
        r.Field("profile", profile.ref);
        r.EndField();
        r.Field("isGlobal",        isGlobal);
        r.Field("priority",        priority);
        r.FloatRange("blendWeight", blendWeight, 0.0f, 1.0f);
        r.Field("influenceRadius", influenceRadius);
        r.Field("blendDistance",   blendDistance);
    }

    /// 適用するプロファイル。未アサイン / パス切れなら nullptr。
    /// @note 既定値へフォールバックしないのは、参照が切れたボリュームが「既定のルック」を
    ///       主張すると priority 次第で他のボリュームを打ち消すため。壊れている箇所は
    ///       画面ではなく Inspector の警告に留める。
    [[nodiscard]] const asset::PostProcessProfile* Resolve() const
    {
        return profile.Get();
    }
};

} // namespace fbzz::scene
