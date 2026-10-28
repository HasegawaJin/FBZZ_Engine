/// @file    InstancedMeshes.cs
/// @brief   Typed shared tree/detail assets and stable-ID Play-local instance changes.
/// @author  Hasegawa Jin
/// @date    2026-10-10
namespace Fbzz;

public readonly struct InstanceSetAssetRef : IAssetReference
{
    readonly string? m_reference;
    [ApiContract(Purity=ApiPurity.MANAGED,Call="any",Failure="empty_or_path_like_GUID_throws_ArgumentException")]
    public InstanceSetAssetRef(string guid) { m_reference=AssetReferenceGuid.Canonical(guid); }
    [ApiContract(Purity=ApiPurity.MANAGED,Read="any")]
    public string Reference => m_reference ?? "";
    [ApiContract(Purity=ApiPurity.MANAGED,Read="any")]
    public bool IsEmpty => Reference.Length==0;
    string IAssetReference.AssetKind => "dataAsset";
}
public sealed class InstancedMeshes : RenderComponent
{
    internal InstancedMeshes(GameObject owner,ulong token=0) : base(owner,(uint)NativeComponentKind.INSTANCED_MESH,(uint)ScriptServiceKind.SERVICE_INSTANCES,token) { }
    [ApiContract(RunModes=new[]{"Edit","Play"},Read="query",Write="live")]
    public bool Enabled { get=>Read(0).Flag; set=>Write(1,value ? 1 : 0); }
    [ApiContract(RunModes=new[]{"Edit","Play"},Read="query",Write="live")]
    public bool CastShadows { get=>Read(2).Flag; set=>Write(3,value ? 1 : 0); }
    [ApiContract(RunModes=new[]{"Edit","Play"},Read="query")]
    public int InstanceCount => Read(4).Int32;
    [ApiContract(RunModes=new[]{"Edit","Play"},Read="query")]
    public InstanceSetAssetRef SharedInstances { get { string reference=Read(5).String; return reference.Length==0 ? default : new(reference); } }
    [ApiContract(RunModes=new[]{"Edit","Play"},Call="live",Failure="false_for_missing_wrong_type_or_invalid_asset_without_changes")]
    public bool TrySetSharedInstances(InstanceSetAssetRef asset) => Write(6,text:[asset.Reference]).Flag;
    [ApiContract(RunModes=new[]{"Play"},Call="live",Failure="false_for_missing_ID_or_zero_scale;_nonfinite_or_ID_outside_positive_Int32_throws_without_changes",Coordinate="owner_local",ParameterUnits=new[]{"position=m","eulerRadians=rad","scale=ratio"})]
    public bool SetTransform(uint instanceId,Vector3 position,Vector3 eulerRadians,Vector3 scale) => Write(7,instanceId,
        [position.X,position.Y,position.Z,eulerRadians.X,eulerRadians.Y,eulerRadians.Z,scale.X,scale.Y,scale.Z]).Flag;
    [ApiContract(RunModes=new[]{"Play"},Call="live",Failure="false_for_missing_ID;_ID_outside_positive_Int32_throws;_removed_ID_is_not_reused")]
    public bool Remove(uint instanceId) => Write(8,instanceId).Flag;
    [ApiContract(RunModes=new[]{"Play"},Call="live",Failure="zero_for_zero_scale_asset_or_capacity;_nonfinite_throws",Coordinate="owner_local",ParameterUnits=new[]{"position=m","eulerRadians=rad","scale=ratio"})]
    public uint Add(Vector3 position,Vector3 eulerRadians,Vector3 scale) => checked((uint)Write(10,values:
        [position.X,position.Y,position.Z,eulerRadians.X,eulerRadians.Y,eulerRadians.Z,scale.X,scale.Y,scale.Z]).Integer);
    [ApiContract(RunModes=new[]{"Play"},Call="live",Summary="Restore authored placements without modifying the shared asset or Edit scene.")]
    public void ResetOverrides() => Write(9);
}
