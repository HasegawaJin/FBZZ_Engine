/// @file    NativeInputBridge.cs
/// @brief   入力名の誤記を名前付き診断にし、実行中の設定から存在と名前一覧を読む。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Buffers;
using System.Buffers.Binary;
using System.IO;
using Fbzz;

namespace Fbzz.ManagedHost;

public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge
    {
        public float ReadAction(ScriptExecutionScope scope, string action, uint state)
        {
            using NativeTransferBuffer storage = new(action, stackalloc byte[256]);
            Span<byte> bytes = storage.Bytes;
            fixed (byte* input = bytes)
            {
                BridgeRequest request = Request(scope, scope.OwnerId, BridgeOp.INPUT_ACTION);
                request.Input = input; request.InputBytes = (uint)bytes.Length; request.Index = state;
                BridgeReply reply = default;
                int status = m_dispatch(&request, &reply);
                if (status == (int)BridgeStatus.NOT_FOUND && reply.Status == BridgeStatus.NOT_FOUND)
                {
                    bool axis = state == (uint)ScriptInputValue.AXIS;
                    throw InputActionAccess.Unknown(action, axis);
                }
                RequireSuccess(status, reply.Status);
                bool isAxis = state == (uint)ScriptInputValue.AXIS;
                if (reply.ValueKind != (isAxis ? ScriptFieldKind.FLOAT : ScriptFieldKind.BOOL)
                    || (isAxis ? !float.IsFinite(reply.X) : reply.Integer is not (0 or 1)))
                    throw new InvalidDataException("Native input value has an invalid kind or value.");
                return isAxis ? reply.X : reply.Integer != 0 ? 1f : 0f;
            }
        }

        public bool HasInputAction(ScriptExecutionScope scope, string action, uint query)
        {
            using NativeTransferBuffer storage = new(action, stackalloc byte[256]);
            Span<byte> bytes = storage.Bytes;
            fixed (byte* input = bytes)
            {
                BridgeRequest request = Request(scope, scope.OwnerId, BridgeOp.INPUT_QUERY);
                request.Input = input; request.InputBytes = (uint)bytes.Length; request.Index = query;
                BridgeReply reply = Call(ref request);
                if (reply.ValueKind != ScriptFieldKind.BOOL || reply.Integer is not (0 or 1))
                    throw new InvalidDataException("Native input existence query has an invalid result.");
                return reply.Integer != 0;
            }
        }

        public string[] ReadInputNames(ScriptExecutionScope scope, uint query)
        {
            BridgeRequest request = Request(scope, scope.OwnerId, BridgeOp.INPUT_QUERY); request.Index = query; request.Flags = 1;
            Span<byte> local = stackalloc byte[256]; NativeReadSnapshot reader = new(this, scope, ref request, local);
            try {
                int count = InputNameCount(ref reader); string[] names = new string[count]; ReadNameValues(ref reader, names); return names;
            } finally { reader.Dispose(); }
        }
        public CollectionReadResult ReadInputNames(ScriptExecutionScope scope, uint query, Span<string?> output)
        {
            BridgeRequest request = Request(scope, scope.OwnerId, BridgeOp.INPUT_QUERY); request.Index = query; request.Flags = 1;
            Span<byte> local = stackalloc byte[256]; NativeReadSnapshot reader = new(this, scope, ref request, local);
            try {
                int count = InputNameCount(ref reader); if (output.Length < count) return new(count, 0);
                string[] names = ArrayPool<string>.Shared.Rent(Math.Max(1, count));
                try {
                    /// @note Validate all packets before touching caller storage, including malformed final UTF-8 names.
                    ReadNameValues(ref reader, names.AsSpan(0, count));
                    for (int index = 0; index < count; index++) output[index] = names[index];
                    return new(count, count);
                } finally { ArrayPool<string>.Shared.Return(names, clearArray: true); }
            } finally { reader.Dispose(); }
        }
        static int InputNameCount(ref NativeReadSnapshot reader)
        {
            if (reader.ValueKind != ScriptFieldKind.STRING_ARRAY || reader.Count > int.MaxValue || reader.Remaining < 4 || (ulong)reader.Count * 4 + 4 > reader.Remaining)
                throw new InvalidDataException("Native input names have invalid snapshot metadata.");
            return (int)reader.Count;
        }
        static void ReadNameValues(ref NativeReadSnapshot reader, Span<string> output)
        {
            if (reader.UInt32() != (uint)output.Length) throw new InvalidDataException("Input snapshot count changed.");
            for (int index = 0; index < output.Length; index++) {
                string name = reader.Text(); if (name.Length == 0) throw new InvalidDataException("Input names cannot be empty."); output[index] = name;
            }
            if (reader.Remaining != 0) throw new InvalidDataException("Input snapshot has trailing bytes.");
        }
    }
}
