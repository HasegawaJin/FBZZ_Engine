/// @file    BridgeAbi.cs
/// @brief   native ManagedBridgeAbi.hpp と一致する Pack=8 の値・ID 契約。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Runtime.InteropServices;

namespace Fbzz.ManagedHost;

/// @see Engine/Scene/ManagedBridgeContract.inl 数値・宣言・layout の共通定義。
public static partial class BridgeAbi
{
    /// @note native function pointer を使う前に実際の CLR layout を確認する。
    /// @see https://learn.microsoft.com/dotnet/api/system.runtime.interopservices.marshal.offsetof Marshal.OffsetOf
    internal static unsafe bool HasExpectedLayout() => IntPtr.Size == 8
        && sizeof(BridgeRequest) == REQUEST_SIZE && sizeof(BridgeReply) == REPLY_SIZE
        && Marshal.OffsetOf<BridgeRequest>(nameof(BridgeRequest.Input)).ToInt32() == REQUEST_INPUT_OFFSET
        && Marshal.OffsetOf<BridgeRequest>(nameof(BridgeRequest.Output)).ToInt32() == REQUEST_OUTPUT_OFFSET;
}
