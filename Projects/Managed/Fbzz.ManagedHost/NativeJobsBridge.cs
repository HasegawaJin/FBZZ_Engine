/// @file    NativeJobsBridge.cs
/// @brief   所有者付き jobs と同期イベントを一回の native dispatch へ転送する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Buffers.Binary;
using System.IO;
using Fbzz;

namespace Fbzz.ManagedHost;

public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge : IScriptJobNativeBridge
    {
        public void RegisterJob(ScriptExecutionScope scope, ulong receiver, ulong token, float delay, float interval, uint kind, bool unscaled, JobEase ease, float from, float to)
        {
            byte* input = stackalloc byte[8];
            BinaryPrimitives.WriteUInt64LittleEndian(new Span<byte>(input, 8), receiver);
            BridgeRequest request = Request(scope, token, kind == 3 ? BridgeOp.REGISTER_TWEEN : BridgeOp.REGISTER_DELAY);
            request.Input = input; request.InputBytes = 8;
            request.Flags = unscaled ? 1U : 0U;
            request.Index = kind == 3 ? (uint)ease : kind;
            request.X = kind == 3 ? from : delay; request.Y = kind == 3 ? to : interval; request.Z = kind == 3 ? delay : 0f;
            Call(ref request);
        }
        public void CancelJob(ScriptExecutionScope scope, ulong receiver, ulong token)
            => ReceiverCall(scope, receiver, token, BridgeOp.CANCEL_JOB);
        public void CancelOwner(ScriptExecutionScope scope, ulong receiver)
            => ReceiverCall(scope, receiver, 0, BridgeOp.CANCEL_OWNER);
        void ReceiverCall(ScriptExecutionScope scope, ulong receiver, ulong token, BridgeOp operation)
        {
            byte* input = stackalloc byte[8];
            BinaryPrimitives.WriteUInt64LittleEndian(new Span<byte>(input, 8), receiver);
            BridgeRequest request = Request(scope, token, operation); request.Input = input; request.InputBytes = 8;
            Call(ref request);
        }
        public void SubscribeEvent(ScriptExecutionScope scope, ulong receiver, ulong token, string channel)
        {
            using NativeTransferBuffer storage = new(channel, stackalloc byte[256], prefixBytes: 8);
            Span<byte> bytes = storage.Bytes;
            BinaryPrimitives.WriteUInt64LittleEndian(bytes, receiver);
            fixed (byte* input = bytes)
            { BridgeRequest request = Request(scope, token, BridgeOp.SUBSCRIBE_EVENT); request.Input = input; request.InputBytes = (uint)bytes.Length; Call(ref request); }
        }
        public void PublishEvent(ScriptExecutionScope scope, string channel, string json)
            => EventCall(scope, 0, BridgeOp.PUBLISH_EVENT, channel + "\0" + json);
        void EventCall(ScriptExecutionScope scope, ulong token, BridgeOp operation, string text)
        {
            using NativeTransferBuffer storage = new(text, stackalloc byte[256]);
            Span<byte> bytes = storage.Bytes;
            fixed (byte* input = bytes)
            { BridgeRequest request = Request(scope, token, operation); request.Input = input; request.InputBytes = (uint)bytes.Length; Call(ref request); }
        }
        public GameObject ResolveEventReference(ScriptExecutionScope scope, string guid)
        {
            using NativeTransferBuffer storage = new(guid, stackalloc byte[256]);
            Span<byte> input = storage.Bytes;
            fixed (byte* source = input)
            {
                BridgeRequest request = Request(scope, 0, BridgeOp.RESOLVE_GUID);
                request.Input = source; request.InputBytes = (uint)input.Length;
                QueryIdenticalBytes(ref request, input, out BridgeReply reply);
                return new GameObject(unchecked((ulong)reply.Integer), scope.SceneLifetime, guid);
            }
        }
    }
}
