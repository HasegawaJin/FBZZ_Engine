/// @file    NativeDisplayBridge.cs
/// @brief   実モニター解像度 snapshot の caller buffer 保護と固定 packet 復号。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Buffers;
using System.IO;
using Fbzz;
namespace Fbzz.ManagedHost;
public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge
    {
        public CollectionReadResult ReadDisplayResolutions(ScriptExecutionScope scope, ulong objectId, Span<DisplayResolution> output)
        {
            Span<byte> input = stackalloc byte[24]; input.Clear();
            fixed (byte* source = input) {
                BridgeRequest request = Request(scope, objectId, BridgeOp.SERVICE_DISPLAY);
                request.Index = 9; request.Flags = 1; request.Input = source; request.InputBytes = 24;
                Span<byte> local = stackalloc byte[256]; NativeReadSnapshot snapshot = new(this, scope, ref request, local); DisplayResolution[]? rented = null;
                try {
                    uint count = snapshot.UInt32();
                    if (count != snapshot.Count || count > int.MaxValue || (ulong)count * 8 != snapshot.Remaining) throw new InvalidDataException("Invalid monitor mode snapshot.");
                    bool completeOutput = output.Length >= count;
                    int decoded = completeOutput ? (int)count : (int)Math.Min(count, snapshot.FetchedRemaining / 8);
                    if (completeOutput) rented = ArrayPool<DisplayResolution>.Shared.Rent(Math.Max(1, (int)count));
                    for (int index = 0; index < decoded; index++) {
                        uint width = snapshot.UInt32(), height = snapshot.UInt32();
                        if (width == 0 || height == 0 || width > int.MaxValue || height > int.MaxValue) throw new InvalidDataException("Invalid monitor resolution.");
                        if (completeOutput) rented![index] = new((int)width, (int)height);
                    }
                    if (!completeOutput) return new((int)count, 0);
                    rented!.AsSpan(0, (int)count).CopyTo(output); return new((int)count, (int)count);
                } finally { snapshot.Dispose(); if (rented is not null) ArrayPool<DisplayResolution>.Shared.Return(rented, true); }
            }
        }
    }
}
