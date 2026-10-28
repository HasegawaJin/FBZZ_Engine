/// @file    ParticleServices.cs
/// @brief   粒子の発生形状・動作設定・typed assets と実 GPU 縮退診断。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
namespace Fbzz;

public enum ParticleEmitterShape { POINT = 0, SPHERE = 1, CONE = 2, BOX = 3, MESH_SURFACE = 4 }
public enum ParticleSortMode { NONE = 0, BACK_TO_FRONT = 1 }
public enum ParticleSimulationMode { CPU = 0, GPU = 1 }
public enum ParticleSimulationSpace { WORLD = 0, LOCAL = 1 }
public enum ParticleRenderMode { BILLBOARD = 0, STRETCHED_BILLBOARD = 1, HORIZONTAL_BILLBOARD = 2, VERTICAL_BILLBOARD = 3 }
public enum ParticleCollisionMode { NONE = 0, PHYSICS = 1, PLANE = 2, DEPTH = 3 }
public enum ParticleCollisionResponse { BOUNCE = 0, KILL = 1, STOP = 2 }
public enum ParticleGpuFallbackReason
{
    NONE = 0, NOT_REQUESTED = 1, LOCAL_SPACE = 2, COLLISION = 3, PREWARM = 4,
    FLIPBOOK_FRAME_BLENDING = 5, MOTION_VECTOR_FLIPBOOK = 6, TRAIL = 7, SUB_EMITTER = 8,
    SELF_SHADOW = 9, SPEED_MODULE = 10, RESERVED_VECTOR_FIELD = 11, LIGHT = 12
}
internal static class ParticleResult
{
    internal static void Count(NativeServiceResult result, int count)
    { if (result.ValueCount != count) throw new InvalidOperationException("Invalid native Particle settings response."); }
    internal static T Enum<T>(int value) where T : struct, System.Enum
    {
        T result = (T)System.Enum.ToObject(typeof(T), value);
        return System.Enum.IsDefined(result) ? result : throw new InvalidOperationException("Invalid native Particle enum response.");
    }
}
public readonly struct ParticlePlaybackSettings
{
    internal ParticlePlaybackSettings(NativeServiceResult result)
    {
        ParticleResult.Count(result, 1);
        if (result.Integer < 0 || result.Integer > 3) throw new InvalidOperationException("Invalid native Particle playback flags.");
        Loop = (result.Integer & 1) != 0; ClearOnStop = (result.Integer & 2) != 0; Duration = result.Scalar;
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public bool Loop { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public bool ClearOnStop { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "s")]
    public float Duration { get; }
}
public readonly struct ParticleMeshShapeSettings
{
    internal ParticleMeshShapeSettings(NativeServiceResult result)
    {
        ParticleResult.Count(result, 3);
        if (result.Value(2) != 0 && result.Value(2) != 1) throw new InvalidOperationException("Invalid native Particle mesh flags.");
        Model = result.String.Length == 0 ? default : new ModelAssetRef(result.String); MeshIndex = result.Int32;
        Scale = result.Value(0); NormalVelocity = result.Value(1); FollowSkinnedAnimation = result.Value(2) != 0;
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public ModelAssetRef Model { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public int MeshIndex { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "dimensionless")]
    public float Scale { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "m/s")]
    public float NormalVelocity { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public bool FollowSkinnedAnimation { get; }
}
public readonly struct ParticleRenderSettings
{
    internal ParticleRenderSettings(NativeServiceResult result)
    { Mode = ParticleResult.Enum<ParticleRenderMode>(result.Int32); StretchScale = result.Scalar; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public ParticleRenderMode Mode { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "dimensionless")]
    public float StretchScale { get; }
}
public readonly struct ParticleCollisionSettings
{
    internal ParticleCollisionSettings(NativeServiceResult result)
    {
        ParticleResult.Count(result, 2);
        if (result.Integer < 0 || result.Integer > 0x203 || (result.Integer & 0xFC) != 0) throw new InvalidOperationException("Invalid native Particle collision flags.");
        Mode = ParticleResult.Enum<ParticleCollisionMode>((int)(result.Integer & 3));
        Response = ParticleResult.Enum<ParticleCollisionResponse>((int)(result.Integer >> 8)); Radius = result.Value(0); Bounciness = result.Value(1);
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public ParticleCollisionMode Mode { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public ParticleCollisionResponse Response { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "m")]
    public float Radius { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "0_to_1")]
    public float Bounciness { get; }
}
public readonly struct ParticleSubEmitterSettings
{
    internal ParticleSubEmitterSettings(NativeServiceResult result)
    {
        if (result.Text.Length != 3) throw new InvalidOperationException("Invalid native Particle sub-emitter settings response.");
        Birth = result.Text[0]; Death = result.Text[1]; Collision = result.Text[2]; BurstCount = result.Int32;
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public string Birth { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public string Death { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public string Collision { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "particles")]
    public int BurstCount { get; }
}
public readonly struct ParticleNoiseSettings
{
    internal ParticleNoiseSettings(NativeServiceResult result)
    { ParticleResult.Count(result, 3); Strength = result.Value(0); Frequency = result.Value(1); Speed = result.Value(2); }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "m/s")]
    public float Strength { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "1/m")]
    public float Frequency { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "noise_scroll/s")]
    public float Speed { get; }
}

public sealed partial class ParticleSystem
{
    const uint ParticleService = (uint)ScriptServiceKind.SERVICE_PARTICLE;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "m", Coordinate = "emitter_local")]
    public Vector3 EmitPosition { get => Invoke(ParticleService, 13, true).Vector3; set => Invoke(ParticleService, 14, false, values: [value.X, value.Y, value.Z]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "m/s", Coordinate = "emitter_local")]
    public Vector3 EmitVelocity { get => Invoke(ParticleService, 15, true).Vector3; set => Invoke(ParticleService, 16, false, values: [value.X, value.Y, value.Z]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "m/s", Summary = "Isotropic per-axis initial velocity spread; finite negative input clamps to zero.")]
    public float VelocitySpread { get => Invoke(ParticleService, 17, true).Scalar; set => Invoke(ParticleService, 18, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "m/s^2", Coordinate = "world")]
    public Vector3 Gravity { get => Invoke(ParticleService, 19, true).Vector3; set => Invoke(ParticleService, 20, false, values: [value.X, value.Y, value.Z]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", ParameterUnits = new[] { "start=linear_rgba", "end=linear_rgba" })]
    public void GetColors(out Vector4 start, out Vector4 end)
    {
        NativeServiceResult result = Invoke(ParticleService, 21, true); ParticleResult.Count(result, 8);
        start = new(result.Value(0), result.Value(1), result.Value(2), result.Value(3)); end = new(result.Value(4), result.Value(5), result.Value(6), result.Value(7));
    }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "start=linear_rgba", "end=linear_rgba" })]
    public void SetColor(Vector4 start, Vector4 end) => Invoke(ParticleService, 22, false, values: [start.X, start.Y, start.Z, start.W, end.X, end.Y, end.Z, end.W]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "m", Summary = "Returns start and end native particle size.")]
    public Vector2 GetSize() => Invoke(ParticleService, 23, true).Vector2;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "start=m", "end=m" }, Summary = "Changes start/end size together, clamping finite negative values to zero.")]
    public void SetSize(float start, float end) => Invoke(ParticleService, 24, false, values: [start, end]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "particles", Summary = "Native capacity; assignment clamps to at least one and invalidates GPU storage when changed.")]
    public int MaxParticles { get => Invoke(ParticleService, 25, true).Int32; set => Invoke(ParticleService, 26, false, value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query")]
    public ParticlePlaybackSettings GetPlayback() => new(Invoke(ParticleService, 27, true));
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "duration=s" }, Summary = "Changes loop/duration/clear-on-stop together; finite negative duration clamps to zero.")]
    public void SetPlayback(bool loop, float duration, bool clearOnStop = false) => Invoke(ParticleService, 28, false, (loop ? 1 : 0) | (clearOnStop ? 2 : 0), [duration]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public ParticleEmitterShape Shape { get => RuntimeComponentValue.ReadEnum<ParticleEmitterShape>(Invoke(ParticleService, 29, true)); set => Invoke(ParticleService, 30, false, RuntimeComponentValue.EnumValue(value)); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "m")]
    public float SphereRadius => Invoke(ParticleService, 31, true).Scalar;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "radius=m" })]
    public void SetSphereShape(float radius) => Invoke(ParticleService, 32, false, values: [radius]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Summary = "Returns cone radius in meters and opening angle in degrees.")]
    public Vector2 GetConeShape() => Invoke(ParticleService, 33, true).Vector2;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "radius=m", "angleDegrees=degree" })]
    public void SetConeShape(float radius, float angleDegrees) => Invoke(ParticleService, 34, false, values: [radius, angleDegrees]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "m", Coordinate = "emitter_local")]
    public Vector3 BoxExtents => Invoke(ParticleService, 35, true).Vector3;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "extents=m" }, ParameterCoordinates = new[] { "extents=emitter_local" })]
    public void SetBoxShape(Vector3 extents) => Invoke(ParticleService, 36, false, values: [extents.X, extents.Y, extents.Z]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Failure = "unregistered_assigned_model_throws; empty_path_returns_empty_model_reference")]
    public ParticleMeshShapeSettings GetMeshShape() => new(Invoke(ParticleService, 37, true));
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "scale=dimensionless", "normalVelocity=m/s" }, Summary = "Validates a typed Model GUID and selected mesh before changing shape/cache. Negative mesh index selects all; empty Model clears the mesh source. Skinned following uses the same owner Animator.", Failure = "missing_wrong_type_or_unloadable_model_or_missing_mesh_returns_false_without_emitter_changes; invalid_numeric_scope_throws")]
    public bool SetMeshShape(ModelAssetRef model, int meshIndex = -1, float scale = 1, bool followSkinnedAnimation = false, float normalVelocity = 0)
        => Invoke(ParticleService, 38, false, meshIndex, [scale, normalVelocity, followSkinnedAnimation ? 1 : 0], [model.Reference]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "m/s", Summary = "Signed normal velocity; changing it does not invalidate the mesh shape cache.")]
    public float MeshShapeNormalVelocity { get => Invoke(ParticleService, 39, true).Scalar; set => Invoke(ParticleService, 40, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public ParticleSortMode SortMode { get => RuntimeComponentValue.ReadEnum<ParticleSortMode>(Invoke(ParticleService, 41, true)); set => Invoke(ParticleService, 42, false, RuntimeComponentValue.EnumValue(value)); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Summary = "Requested simulation path. Changing it requests GPU clear; actual eligibility uses IsGpuSimulated and fallback diagnostics.")]
    public ParticleSimulationMode SimulationMode { get => RuntimeComponentValue.ReadEnum<ParticleSimulationMode>(Invoke(ParticleService, 43, true)); set => Invoke(ParticleService, 44, false, RuntimeComponentValue.EnumValue(value)); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Summary = "Local space forces the requested simulation path to CPU through the existing native proxy.")]
    public ParticleSimulationSpace SimulationSpace { get => RuntimeComponentValue.ReadEnum<ParticleSimulationSpace>(Invoke(ParticleService, 45, true)); set => Invoke(ParticleService, 46, false, RuntimeComponentValue.EnumValue(value)); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query")]
    public ParticleRenderSettings GetRenderSettings() => new(Invoke(ParticleService, 47, true));
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Changes billboard orientation and stretch scale together; finite negative stretch clamps to zero.")]
    public void SetRenderMode(ParticleRenderMode mode, float stretchScale = 1) => Invoke(ParticleService, 48, false, RuntimeComponentValue.EnumValue(mode), [stretchScale]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query")]
    public ParticleCollisionSettings GetCollisionSettings() => new(Invoke(ParticleService, 49, true));
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "radius=m", "bounciness=0_to_1" }, Summary = "Validates collision mode/response together, clamps radius/bounce, and forces CPU for Physics collisions.")]
    public void SetCollision(ParticleCollisionMode mode, ParticleCollisionResponse response, float radius = 0.05f, float bounciness = 0.5f)
        => Invoke(ParticleService, 50, false, RuntimeComponentValue.EnumValue(mode) | (RuntimeComponentValue.EnumValue(response) << 8), [radius, bounciness]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "collisions_this_frame")]
    public int CollisionCount => Invoke(ParticleService, 51, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "particles/m")]
    public float RateOverDistance { get => Invoke(ParticleService, 52, true).Scalar; set => Invoke(ParticleService, 53, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Summary = "Changes prewarm and resets native pending prewarm state.")]
    public bool Prewarm { get => Invoke(ParticleService, 54, true).Flag; set => Invoke(ParticleService, 55, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query")]
    public ParticleSubEmitterSettings GetSubEmitters() => new(Invoke(ParticleService, 56, true));
    /// @note VFX が設定した subtree scope を保持する。手置き Emitter だけは既存 Scene 名前解決を使う。
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Sets scoped owner names for birth/death/collision sub-emitters; empty names clear individual links. VFX subtree isolation is preserved; burst count clamps to at least one.")]
    public void SetSubEmitters(string birthEmitter, string deathEmitter, string collisionEmitter, int burstCount = 1)
        => Invoke(ParticleService, 57, false, burstCount, text: [birthEmitter, deathEmitter, collisionEmitter]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "1/s", Summary = "Native flow coupling; this is the retained velocity-damping proxy behavior.")]
    public float FlowCoupling { get => Invoke(ParticleService, 58, true).Scalar; set => Invoke(ParticleService, 59, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "damping=1/s" })]
    public void SetVelocityDamping(float damping) => Invoke(ParticleService, 59, false, values: [damping]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "rad/s")]
    public Vector2 GetAngularVelocity() => Invoke(ParticleService, 60, true).Vector2;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "minValue=rad/s", "maxValue=rad/s" })]
    public void SetAngularVelocity(float minValue, float maxValue) => Invoke(ParticleService, 61, false, values: [minValue, maxValue]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Summary = "Reads the first local Curl force without adding one; an absent module has strength zero.")]
    public ParticleNoiseSettings GetNoise() => new(Invoke(ParticleService, 62, true));
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "strength=m/s", "frequency=1/m" }, Summary = "Reuses or creates the native local Curl module; strength clamps nonnegative and frequency to at least 0.0001.")]
    public void SetNoise(float strength, float frequency = 0.5f, float speed = 1) => Invoke(ParticleService, 63, false, values: [strength, frequency, speed]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool ReceiveFlowFields { get => Invoke(ParticleService, 64, true).Flag; set => Invoke(ParticleService, 65, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "uint32_bitmask")]
    public uint FlowFieldChannels { get => checked((uint)Invoke(ParticleService, 66, true).Integer); set => Invoke(ParticleService, 67, false, value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Summary = "Uses the native ParticlePass eligibility predicate including the last resolved material settings; before initial material resolution these diagnostics may be provisional.")]
    public bool IsGpuSimulated => Invoke(ParticleService, 68, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public ParticleGpuFallbackReason GpuFallbackReason => RuntimeComponentValue.ReadEnum<ParticleGpuFallbackReason>(Invoke(ParticleService, 69, true));
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public string GpuFallbackField => Invoke(ParticleService, 70, true).String;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public string GpuFallbackDescription => Invoke(ParticleService, 71, true).String;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Summary = "Returns the assigned registered Material GUID without creating metadata; empty assignment uses native fallback material.")]
    public MaterialAssetRef Material { get { string reference = Invoke(ParticleService, 72, true).String; return reference.Length == 0 ? default : new MaterialAssetRef(reference); } }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Assigns a validated real Material GUID; empty restores fallback. Existing native material resolution applies on the next draw.", Failure = "missing_wrong_type_or_unloadable_asset_returns_false_without_assignment_changes")]
    public bool TrySetMaterial(MaterialAssetRef material) => Invoke(ParticleService, 73, false, text: [material.Reference]).Flag;
}
