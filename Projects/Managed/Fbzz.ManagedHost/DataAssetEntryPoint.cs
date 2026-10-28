/// @file    DataAssetEntryPoint.cs
/// @brief   DataAsset の staged UTF8 と bounded pages を ABI の既存 packet layout へ投影する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Linq;

namespace Fbzz.ManagedHost;

public static unsafe partial class EntryPoint
{
    static void DataAssetOperation(ManagedScriptRuntime runtime, ref BridgeRequest request, ref BridgeReply reply)
    {
        ulong id = request.InstanceId;
        switch (request.Op) {
        case BridgeOp.GET_DATA_ASSET_SCHEMA:
            ScriptCatalog? catalog = runtime.GetCatalog(request.Generation);
            if (catalog is null) { reply.Status = BridgeStatus.STALE_GENERATION; return; }
            reply.Index = (uint)catalog.DataAssets.Count;
            if (request.Index >= catalog.DataAssets.Count) { reply.Status = BridgeStatus.NOT_FOUND; return; }
            DataAssetTypeDescriptor type = catalog.DataAssets[(int)request.Index];
            WriteOutput(ref request, ref reply, request.Flags switch { 0 => type.PersistentId, 1 => SettingsDocument.CatalogEntry(type), 2 => type.TypeName, _ => throw new ArgumentException("Unknown DataAsset schema selector.") }); return;
        case BridgeOp.OPEN_DATA_ASSET_DOCUMENT:
        case BridgeOp.PREPARE_DATA_ASSET_DOCUMENT:
            if (request.Flags == 0) {
                if (request.Op == BridgeOp.OPEN_DATA_ASSET_DOCUMENT) {
                    ManagedRuntimeResult opened = runtime.OpenDataAssetDocument(request.Generation, ReadInput(request), out ulong document);
                    reply.Integer = unchecked((long)document); Complete(opened, ref request, ref reply);
                } else Complete(runtime.BeginDataAssetPreparation(request.Generation, id), ref request, ref reply);
            } else if (request.Flags == 1) {
                long offset = unchecked((long)((ulong)request.Reserved0 | ((ulong)request.Reserved1 << 32)));
                Complete(runtime.StageDataAssetDocument(request.Generation, id, offset, new ReadOnlySpan<byte>(request.Input, (int)request.InputBytes)), ref request, ref reply);
            } else if (request.Flags == 2) {
                if (request.OutputCapacity < 2048) { reply.Status = BridgeStatus.INVALID_ARGUMENT; return; }
                ManagedRuntimeResult completed = runtime.FinishDataAssetDocument(request.Generation, id, out string summary);
                Complete(completed, ref request, ref reply); if (completed.Success) WriteOutput(ref request, ref reply, summary);
            } else reply.Status = BridgeStatus.INVALID_ARGUMENT;
            return;
        case BridgeOp.READ_DATA_ASSET_PAGE:
            ManagedRuntimeResult paged = runtime.ReadDataAssetPage(request.Generation, id, ReadInput(request), out string page);
            Complete(paged, ref request, ref reply); if (paged.Success) WriteOutput(ref request, ref reply, page); return;
        case BridgeOp.READ_PREPARED_DATA_ASSET:
            if (request.Flags == 0) {
                long offset = unchecked((long)((ulong)request.Reserved0 | ((ulong)request.Reserved1 << 32)));
                ManagedRuntimeResult read = runtime.ReadPreparedDataAssetBytes(request.Generation, id, offset, (int)request.Index, out byte[] bytes, out long total);
                Complete(read, ref request, ref reply);
                if (read.Success) { reply.Generation = (ulong)total; reply.Integer = offset + bytes.Length; reply.Flags = offset + bytes.Length == total ? 1U : 0U; WriteBytes(ref request, ref reply, bytes); }
            } else if (request.Flags == 1) {
                ManagedRuntimeResult read = runtime.ReadPreparedDataAssetReview(request.Generation, id, ReadInput(request), out string review);
                Complete(read, ref request, ref reply); if (read.Success) WriteOutput(ref request, ref reply, review);
            } else reply.Status = BridgeStatus.INVALID_ARGUMENT;
            return;
        case BridgeOp.CLOSE_DATA_ASSET_DOCUMENT:
            Complete(runtime.CloseDataAssetDocument(request.Generation, id), ref request, ref reply); return;
        }
    }
}
