// FBZZ Engine
// PostProcessVolumeComponent.hpp | fbzz::scene
// ポストプロセス設定をシーン内で領域・優先度つきに適用するボリューム。
//
// WHY: ProjectSettings のポストプロセス設定をシーン単位で上書きすることで、
//      屋外・屋内・カットシーンなど場所ごとに異なる演出を切り替えられるようにする。
//
// 適用の仕組み (RenderSystem が毎フレーム合成する):
//      ベース = ProjectSettings の設定
//      有効なボリュームを priority 昇順に、重み付きで順次ブレンドする
//      重み = blendWeight × (isGlobal ? 1 : 距離ウェイト)
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>

namespace fbzz::scene {

struct PostProcessVolumeComponent {
    bool enabled = true;

    // 参照するプロファイル (.fzdata)。未アサインならインラインの settings を使う。
    //
    // WHY 参照とインラインを両立させるか:
    //   既存シーンは settings を実体で持っている。参照を必須にすると全シーンが壊れる。
    //   プロファイルを D&D した時点で参照モードへ切り替わる形にすることで、
    //   無改修のまま段階的に移行できる。
    //
    // WHY 参照を推奨するか:
    //   インラインだと同じルックがボリュームごとに複製され、調整のたびに
    //   全箇所を手で直すことになる。プロファイル参照なら 1 か所の編集が全体へ届く。
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
    //      画面のルックが不連続に飛んで目に付く。
    float blendDistance = 2.0f;

    // profile 未アサイン時に使うインライン設定。
    // WHY Reflect で個別列挙しないか: PostProcessSettings は 30+ フィールドを持ち、
    //      SceneSerializer が専用パスで TOML 化している。Inspector 側も
    //      PostProcessInspectorWidgets の手書き UI で編集する。
    renderer::PostProcessSettings settings;

    const char* GetTypeName() const { return "PostProcessVolume"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);

        // DataAssetRef の Field オーバーロードにより、Inspector には
        // .fzdata のドラッグ&ドロップスロットが出る。
        // シリアライズ時は path 文字列として保存され、GuidRefCodec が guid: へ変換する。
        r.BeginField("profile", "Profile");
        r.Field("profile", profile.ref);

        r.Field("isGlobal",        isGlobal);
        r.Field("priority",        priority);
        r.FloatRange("blendWeight", blendWeight, 0.0f, 1.0f);
        r.Field("influenceRadius", influenceRadius);
        r.Field("blendDistance",   blendDistance);
    }

    // 実際に適用する設定を返す。プロファイルが解決できればそちらを優先する。
    // 解決できない (未アサイン / パス切れ) 場合はインライン設定へフォールバックする。
    // WHY フォールバックするか: プロファイルを消してしまったときに画面が
    //      真っ黒や無補正へ飛ぶより、直前のインライン設定で描き続ける方が復旧しやすい。
    [[nodiscard]] const renderer::PostProcessSettings& Resolve() const
    {
        if (const asset::PostProcessProfile* resolved = profile.Get())
            return resolved->settings;
        return settings;
    }

    // 上書き対象セクションを返す。
    // インライン設定 (プロファイル未参照) は従来どおり全セクションを上書きする。
    // WHY インラインにマスクを持たせないか: インライン設定はそのボリューム専用で、
    //      他と共有されない。差分オーサリングの動機 (共有元の変更に追従したい) が
    //      そもそも生じないため、フィールドを増やす価値がない。
    [[nodiscard]] renderer::PostProcessOverrides ResolveOverrides() const
    {
        if (const asset::PostProcessProfile* resolved = profile.Get())
            return resolved->overrides;
        return renderer::PostProcessOverrides::All();
    }
};

} // namespace fbzz::scene
