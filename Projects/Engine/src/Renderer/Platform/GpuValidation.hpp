/// @file    GpuValidation.hpp
/// @brief   DX11 / DX12 共通の検証レイヤー方針 (蓄積は上限付き・回収は終了時だけ)。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// @note 毎フレーム回収しない理由: InfoQueue の取り出しはメッセージ 1 件ごとに 2 回の COM 呼び出しに
///       なる。検証レイヤー自体より «読み出し» の方が重く、Editor のように 1 フレームで 2 面
///       描くところでは体感できるほど落ちる。溜めるだけにして、終了時に 1 度だけ吐き出す。
#pragma once

#include <Engine/Core/Logger.hpp>

/// @note windows.h をここで入れる理由: InfoQueue の GetMessage は winuser.h のマクロで
///       GetMessageW へ書き換わる。マクロが «居る TU と居ない TU» ができると同じテンプレートが
///       別物になるため、このヘッダーを入れた時点で必ず居る状態に揃える。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <vector>

namespace fbzz::renderer::gpuvalidation {

/// この構成に検証レイヤーのコードが入っているか (Debug / Development のみ)。
inline constexpr bool kBuiltIn =
#if defined(FBZZ_GPU_VALIDATION)
    true;
#else
    false;
#endif

/// 何も指定しないときに立てるか (Debug のみ true)。
inline constexpr bool kDefaultOn =
#if defined(FBZZ_GPU_VALIDATION_DEFAULT)
    true;
#else
    false;
#endif

/// 実際に立ち上げるか。環境変数 `FBZZ_GPU_VALIDATION` (0/1) が既定より優先される。
/// @note 環境変数で上書きできる理由: 検証レイヤーは常時 ON だと «遊べる速さ» と両立しないが、
///       じわじわ漏れる類は Debug 速度では再現しない。Development (Release 相当の最適化) に
///       1 を渡して追う日だけ立てられ、Debug で検証レイヤー自身を疑うときは 0 で外せる。
[[nodiscard]] bool IsEnabled();

/// 検証エラーでデバッガーへ落とすか。
/// @note デバッガー接続時だけに絞る理由: SetBreakOnSeverity はブレークポイント例外を投げるため、
///       デバッガーが居ない実行 (エディターを直接起動した場合) では «原因不明のクラッシュ» に
///       なる。止めて調べられる状況でだけ止める。
[[nodiscard]] bool ShouldBreakOnError();

/// InfoQueue に溜める件数の上限。
/// @note 無制限にしない理由: 毎フレーム出る警告が 1 つでもあると、リークを探しているこちらが
///       メモリを食い続けることになる。原因を掴むには先頭の数百件で足りる。
inline constexpr std::uint64_t kMaxStoredMessages = 1024;

/// D3D11 / D3D12 で値の並びが同じ severity を 1 か所で文字にする。
[[nodiscard]] inline const char* SeverityName(int severity)
{
    switch (severity) {
    case 0:  return "CORRUPTION";
    case 1:  return "ERROR";
    case 2:  return "WARNING";
    case 3:  return "INFO";
    default: return "MESSAGE";
    }
}

/// 溜まったメッセージを全部ログへ出し、キューを空にする。終了時にだけ呼ぶこと。
/// InfoQueue / Message は D3D11・D3D12 で別型だが、必要な形は同じなのでテンプレートで受ける。
template <class Message, class InfoQueue>
void DrainStoredMessages(InfoQueue& queue, const char* tag)
{
    const std::uint64_t count = queue.GetNumStoredMessages();
    if (count == 0) {
        queue.ClearStoredMessages();
        return;
    }

    FBZZ_LOG_WARN("%s: 検証レイヤーのメッセージ %llu 件", tag,
                  static_cast<unsigned long long>(count));

    std::vector<std::uint8_t> buffer;
    for (std::uint64_t i = 0; i < count; ++i) {
        SIZE_T length = 0;
        if (FAILED(queue.GetMessage(i, nullptr, &length)) || length == 0)
            continue;
        buffer.assign(static_cast<std::size_t>(length), std::uint8_t{ 0 });
        auto* message = reinterpret_cast<Message*>(buffer.data());
        if (FAILED(queue.GetMessage(i, message, &length)))
            continue;

        FBZZ_LOG_WARN("  [%s] (id=%d) %s",
                      SeverityName(static_cast<int>(message->Severity)),
                      static_cast<int>(message->ID),
                      message->pDescription != nullptr ? message->pDescription : "(no description)");
    }
    queue.ClearStoredMessages();
}

/// デバイスを手放した後に呼ぶ。まだ生きている COM オブジェクトを OutputDebugString へ出す。
/// @param apiId 集計対象の DXGI デバッグ ID。DXGI_DEBUG_D3D11 / DXGI_DEBUG_D3D12 を渡す。
/// @param label ログに出す名前 ("D3D11" など)。
/// @note GUID で受ける理由: DXGI_DEBUG_D3D11/D3D12 は dxgidebug.h でなく d3d11sdklayers.h/
///       d3d12sdklayers.h が持つため、両方 include すると片方しか使わない TU まで相手側の
///       SDK ヘッダーを抱える。
void ReportLiveObjects(const GUID& apiId, const char* label);

} // namespace fbzz::renderer::gpuvalidation
