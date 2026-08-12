// FBZZ Engine
// PostProcessBlend.hpp | fbzz::renderer
// PostProcessSettings の重み付き合成
//
// WHY 必要か: PostProcessVolume を複数置いて「屋外 → 洞窟 → ボス演出」のように
//      重ねるには、設定同士を連続的に混ぜる操作が要る。単純な代入だと
//      境界をまたいだ瞬間に画面が飛ぶ。
#pragma once
#include <Engine/Renderer/RenderSettings.hpp>

namespace fbzz::renderer {

// どのセクションを上書きするかのマスク。
//
// WHY PostProcessSettings に持たせないか:
//   これは「設定値」ではなく「合成時のメタデータ」。PostProcessSettings は
//   シェーダーへの転送元でもあり、Project Settings や runtime override とも共有される。
//   そこへ override フラグを混ぜると、上書き対象でない経路にも意味のないフラグが付き回る。
//   マスクをブレンド側の型として独立させることで、RenderSettings.hpp は無改修で済む。
//
// WHY フィールド単位でなくセクション単位か:
//   Unity の Volume framework 相当のフィールド単位 override を入れると、
//   PostProcessSettings の 50 以上の全フィールドが Overridable<T> ラッパーになり、
//   シェーダー転送・シリアライズ・既存の Project Settings UI がすべて変わる。
//   セクション単位なら 14 個の bool で済み、実用上の要求
//   (「洞窟では fog と colorGrading だけ変えたい」) はほぼ満たせる。
struct PostProcessOverrides {
    bool fxaa             = true;
    bool exposure         = true;
    bool bloom            = true;
    bool ambientOcclusion = true;
    bool fog              = true;
    bool colorGrading     = true;
    bool vignette         = true;
    bool filmGrain        = true;
    bool sharpen          = true;
    bool depthOfField     = true;
    bool lens             = true;
    bool stylized         = true;
    bool imageQuality     = true;
    bool customEffects    = true;

    // WHY 既定を全 true にするか:
    //   全 false を既定にすると、新規プロファイルを作って値をいじっても
    //   画面が一切変わらず「壊れている」ように見える。
    //   全 true なら従来の「まるごと差し替え」と同じ挙動で始まり、
    //   不要なセクションのチェックを外していく引き算の操作になる。
    //   インライン設定 (プロファイル未参照) のボリュームもこの既定と一致する。
    [[nodiscard]] static PostProcessOverrides All() { return PostProcessOverrides{}; }

    // 全セクションを上書きしない。ベースを一切変えないプロファイルになる。
    [[nodiscard]] static PostProcessOverrides None()
    {
        PostProcessOverrides mask;
        mask.fxaa = mask.exposure = mask.bloom = mask.ambientOcclusion = false;
        mask.fog  = mask.colorGrading = mask.vignette = mask.filmGrain = false;
        mask.sharpen = mask.depthOfField = mask.lens = mask.stylized  = false;
        mask.imageQuality = mask.customEffects = false;
        return mask;
    }
};

// t=0 で a、t=1 で b を返す。t は呼び出し側で 0..1 にクランプ済みであること。
//
// 補間規則:
//   - 数値フィールド: 線形補間
//   - bool フィールド: t >= 0.5 で b を採用する
//   - customEffects (配列): 補間せず t >= 0.5 で b を採用する
//
// WHY bool を閾値で切り替えるか:
//   Unity の Volume framework は Overridable<T> 相当のラッパーで
//   「この項目を上書きするか」をフィールドごとに持つが、それを導入すると
//   PostProcessSettings の 30+ フィールドすべてがラッパー型になり、
//   シェーダーへの転送・.fzdata の形・既存の Project Settings UI が全部変わる。
//   閾値方式は「ブルームの ON/OFF がブレンドの途中で切り替わる」という
//   限界を持つが、実用上は許容できる。将来の拡張余地として記録しておく。
//
// WHY customEffects を補間しないか:
//   要素数も名前も異なりうる配列を補間する自然な定義が存在しない。
//   「重みが大きい側を丸ごと採用する」が唯一破綻しない規則。
// overrides で false のセクションは a の値をそのまま保つ (b を無視する)。
//
// WHY マスクがあると bool の閾値切替の実害が減るか:
//   「ブレンド途中でブルームの ON/OFF が飛ぶ」問題は、そもそも b が
//   そのセクションを上書きする意図があるときにしか起きない。
//   上書きしないセクションを素通しできれば、意図しない切り替わりは発生しなくなる。
[[nodiscard]] PostProcessSettings LerpPostProcessSettings(
    const PostProcessSettings& a, const PostProcessSettings& b, float t,
    const PostProcessOverrides& overrides);

// 全セクションを上書きする版 (従来の挙動)。
[[nodiscard]] PostProcessSettings LerpPostProcessSettings(
    const PostProcessSettings& a, const PostProcessSettings& b, float t);

// ボリュームの距離ウェイトを返す。
//   distance <= radius - blendDistance : 1
//   distance >= radius                 : 0
//   その間                              : 線形に降下
[[nodiscard]] float PostProcessVolumeDistanceWeight(
    float distance, float radius, float blendDistance);

} // namespace fbzz::renderer
