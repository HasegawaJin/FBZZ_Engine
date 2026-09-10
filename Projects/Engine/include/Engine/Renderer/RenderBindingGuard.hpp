/// @file    RenderBindingGuard.hpp
/// @brief   パス境界でレンダーターゲット束縛を無効化し、暗黙の依存を炙り出す診断スイッチ。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// 各パスの Execute 直前に 1x1 のダミー RT を束縛する。自分で SetRenderTarget を
/// 呼ばないパスは «直前のパスが残した束縛» へ描いているので、その描画は 1 ピクセルへ
/// 潰れて画面から消える。消えたものが、申告と実際の束縛が食い違っているパス。
///
/// WHY これが要るか: RenderGraph は依存で並べ替えるので、束縛を «前のパス任せ» に
///     しているパスは、構成が変わって順序が入れ替わった瞬間に別の RT へ描き始める。
///     絵は出ているのに描き先だけが違う、という形で壊れるため、普通に遊んでいても
///     気付けない。順序に依存していることを能動的に露出させる。
///
/// WHY 既定で切るか: 束縛漏れが残っている間は «正しく壊れる» ので、そのままでは
///     絵が出ない。追う日に環境変数で立てる。1 パスにつき OMSetRenderTargets が
///     1 回増えるコストもあるので、直った後も常時 ON にはしない。
#pragma once

namespace fbzz::renderer::bindingguard {

/// この構成に診断コードが入っているか (Debug / Development のみ)。
/// WHY GPU 検証と同じ定義を使うか: どちらも «追う日にだけ立てる GPU 診断» で、
///     積む構成を分ける理由が無い。増やすと «どちらを付けた構成か» が分からなくなる。
inline constexpr bool kBuiltIn =
#if defined(FBZZ_GPU_VALIDATION)
    true;
#else
    false;
#endif

/// 実際に毒を撒くか。環境変数 `FBZZ_RENDER_BINDING_GUARD` (0 / 1) で切り替える。
/// 既定は「立てない」。Debug でも明示的に 1 を渡したときだけ動く。
[[nodiscard]] bool IsEnabled();

} // namespace fbzz::renderer::bindingguard
