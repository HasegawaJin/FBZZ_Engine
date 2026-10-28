/// @file    WorldFamilies.cs
/// @brief   Terrain/Water/VFX/Sequence の実行時 facade と明示座標・callback 契約。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Globalization;
namespace Fbzz;

public readonly struct TerrainSurfaceSample
{
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public TerrainSurfaceSample(Vector3 position, Vector3 normal) { Position = position; Normal = normal; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "m", Coordinate = "world")]
    public Vector3 Position { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "unit_vector", Coordinate = "world")]
    public Vector3 Normal { get; }
}

public sealed class Terrain : NativeComponent
{
    internal Terrain(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.TERRAIN, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_TERRAIN;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "vertices")]
    public int Columns => Invoke(Service, 2, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "vertices")]
    public int Rows => Invoke(Service, 3, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "m", Coordinate = "terrain_local")]
    public float CellSize => Invoke(Service, 4, true).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "m", Coordinate = "terrain_local")]
    public float MaxHeight => Invoke(Service, 5, true).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "m", Coordinate = "terrain_local", ParameterUnits = new[] { "x=m", "z=m" }, ParameterCoordinates = new[] { "x=terrain_local", "z=terrain_local" }, Summary = "Clamps local coordinates to the authored grid and interpolates its two triangles; holes still return height.")]
    public float GetHeightLocal(float x, float z) => Invoke(Service, 6, true, values: [x, z]).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "unit_vector", Coordinate = "terrain_local")]
    public Vector3 GetNormalLocal(float x, float z) => Invoke(Service, 7, true, values: [x, z]).Vector3;
    /// @return 最上の worldXZ 鉛直交点。穴・範囲外・非可逆姿勢では false と default。caller の queryY を推測しない。
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", ParameterUnits = new[] { "worldX=m", "worldZ=m" }, ParameterCoordinates = new[] { "worldX=world", "worldZ=world" }, Failure = "false_and_default_for_hole_outside_or_singular_transform; malformed_lifetime_scope_throws", Summary = "Returns the highest vertical world-XZ triangle intersection and inverse-transpose world normal; Physics initialization is not required.")]
    public bool TrySampleWorld(float worldX, float worldZ, out TerrainSurfaceSample sample)
    {
        NativeServiceResult result = Invoke(Service, 8, true, values: [worldX, worldZ]);
        sample = default;
        if (!result.Flag) { if (result.ValueCount != 0) throw new InvalidOperationException("Invalid Terrain miss response."); return false; }
        if (result.ValueCount != 6) throw new InvalidOperationException("Invalid Terrain surface response.");
        sample = new(new(result.Value(0), result.Value(1), result.Value(2)), new(result.Value(3), result.Value(4), result.Value(5)));
        return true;
    }
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "height=m" }, ParameterCoordinates = new[] { "height=terrain_local_y" }, Summary = "Updates a grid vertex and requests renderer/collider rebuild; indices and authored height range clamp as native Terrain does.")]
    public bool SetHeightAtGrid(int x, int z, float height) => Invoke(Service, 9, false, Grid(x, z), [height]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "layers")]
    public int LayerCount => Invoke(Service, 10, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "weight_0_to_1")]
    public float GetLayerWeightAtGrid(int x, int z, int layer) => Invoke(Service, 11, true, Grid(x, z), text: [layer.ToString(CultureInfo.InvariantCulture)]).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "weight=weight_0_to_1" })]
    public bool PaintLayerAtGrid(int x, int z, int layer, float weight) => Invoke(Service, 12, false, Grid(x, z), [weight], [layer.ToString(CultureInfo.InvariantCulture)]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query")]
    public bool IsHoleCell(int x, int z) => Invoke(Service, 13, true, Grid(x, z)).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", Summary = "Changes triangle presence and requests renderer/collider rebuild only when the cell changes.")]
    public bool SetHoleCell(int x, int z, bool hole) => Invoke(Service, 14, false, Grid(x, z), [hole ? 1 : 0]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Summary = "Assigns a real Material GUID; empty clears the layer reference, index==LayerCount appends within native terrain layer capacity.")]
    public bool SetLayerMaterial(int layer, MaterialAssetRef material) => Invoke(Service, 15, false, layer, text: [material.Reference ?? string.Empty]).Flag;

    /// @return Nearest floor below reference; false returns default sample and does not require a Physics collider.
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", ParameterUnits = new[] { "reference=m", "maxDistance=m" }, ParameterCoordinates = new[] { "reference=world" }, Failure = "false_and_default_for_miss_or_invalid_numeric_arguments; malformed_lifetime_scope_throws")]
    public bool TrySampleFloorWorld(Vector3 reference, float maxDistance, out TerrainSurfaceSample sample)
        => TrySampleDirectionalWorld(16, reference, maxDistance, out sample);
    /// @return Nearest ceiling above reference; false returns default sample.
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", ParameterUnits = new[] { "reference=m", "maxDistance=m" }, ParameterCoordinates = new[] { "reference=world" }, Failure = "false_and_default_for_miss_or_invalid_numeric_arguments; malformed_lifetime_scope_throws")]
    public bool TrySampleCeilingWorld(Vector3 reference, float maxDistance, out TerrainSurfaceSample sample)
        => TrySampleDirectionalWorld(17, reference, maxDistance, out sample);
    bool TrySampleDirectionalWorld(uint method, Vector3 reference, float maxDistance, out TerrainSurfaceSample sample)
    {
        Scope();
        sample = default;
        if (!reference.IsFinite || !float.IsFinite(maxDistance) || maxDistance < 0) return false;
        NativeServiceResult result = Invoke(Service, method, true, values: [reference.X, reference.Y, reference.Z, maxDistance]);
        if (!result.Flag) { if (result.ValueCount != 0) throw new InvalidOperationException("Invalid Terrain miss response."); return false; }
        if (result.ValueCount != 6) throw new InvalidOperationException("Invalid Terrain surface response.");
        sample = new(new(result.Value(0), result.Value(1), result.Value(2)), new(result.Value(3), result.Value(4), result.Value(5)));
        return true;
    }
    static long Grid(int x, int z) => unchecked((long)(((ulong)(uint)z << 32) | (uint)x));
}

/// @brief Authorable water-contact visual impulse kind.
public enum WaterInteractionKind { SURFACE_FOAM = 0, BUBBLES = 1, SEDIMENT = 2 }

public sealed class Water : NativeComponent
{
    internal Water(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.WATER, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_WATER;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "dimensionless", Summary = "Nonnegative scale on assigned material waves; finite negative input clamps to zero.")]
    public float WaveAmplitudeScale { get => Invoke(Service, 2, true).Scalar; set => Invoke(Service, 3, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool GerstnerEnabled { get => Invoke(Service, 4, true).Flag; set => Invoke(Service, 5, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool BuoyancyEnabled { get => Invoke(Service, 6, true).Flag; set => Invoke(Service, 7, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "m/s", Coordinate = "world_xz")]
    public Vector2 Current => Invoke(Service, 8, true).Vector2;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "m", Coordinate = "world_y", ParameterUnits = new[] { "worldX=m", "worldZ=m", "time=s" }, ParameterCoordinates = new[] { "worldX=world", "worldZ=world" })]
    public float GetSurfaceHeightWorld(float worldX, float worldZ, float time) => Invoke(Service, 9, true, values: [worldX, worldZ, time]).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "m", Summary = "Returns wave displacement from baseline at local-XZ transformed into world phase; it is not absolute local height.")]
    public float GetSurfaceDisplacementLocal(float localX, float localZ, float time) => Invoke(Service, 10, true, values: [localX, localZ, time]).Scalar;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "worldPosition=m", "strength=0_to_1" }, ParameterCoordinates = new[] { "worldPosition=world" })]
    public void AddRipple(Vector3 worldPosition, float strength = 0.5f) => Invoke(Service, 11, false, values: [worldPosition.X, worldPosition.Y, worldPosition.Z, strength]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "worldPosition=m", "intensity=0_to_1" }, ParameterCoordinates = new[] { "worldPosition=world" })]
    public void Splash(Vector3 worldPosition, float intensity = 0.5f) => Invoke(Service, 12, false, values: [worldPosition.X, worldPosition.Y, worldPosition.Z, intensity]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "worldPosition=m", "worldVelocity=m/s", "radius=m", "strength=0_to_1", "lifetime=s" }, ParameterCoordinates = new[] { "worldPosition=world", "worldVelocity=world" }, Summary = "Queues foam, bubbles or a sediment plume for serial Cleanup. Returns false outside the allowed water, when disabled, or at the bounded queue capacity; runtime effects are not saved.", Failure = "invalid_kind_or_numeric_arguments_fail_without_changes")]
    public bool EmitInteraction(WaterInteractionKind kind, Vector3 worldPosition, Vector3 worldVelocity,
        float radius = 0.25f, float strength = 0.5f, float lifetime = 2.0f)
        => Invoke(Service, 13, false, (int)kind, values: [worldPosition.X, worldPosition.Y, worldPosition.Z,
            worldVelocity.X, worldVelocity.Y, worldVelocity.Z, radius, strength, lifetime]).Flag;
}

public sealed class VFX : NativeComponent
{
    internal VFX(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.VFX, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_VFX;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "dimensionless")]
    public float Speed { get => Invoke(Service, 2, true).Scalar; set => Invoke(Service, 3, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public bool IsPlaying => Invoke(Service, 4, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "s")]
    public float Time => Invoke(Service, 5, true).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "s", Summary = "Explicit duration when positive, otherwise descendant-derived resolved duration.")]
    public float Duration => Invoke(Service, 6, true).Scalar;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Play(bool restart = true) => Invoke(Service, 7, false, restart ? 1 : 0);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Pause() => Invoke(Service, 8, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Resume() => Invoke(Service, 9, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Stop() => Invoke(Service, 10, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "empty_name_throws; missing_or_expired_row_scope_throws")]
    public void Trigger(string name) { ArgumentException.ThrowIfNullOrEmpty(name); Invoke(Service, 11, false, text: [name]); }
}

public sealed class SequencePlayer : NativeComponent
{
    internal SequencePlayer(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.SEQUENCE, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_SEQUENCE;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "dimensionless", Summary = "Finite signed speed; native sequence wrap/time policy determines reverse playback.")]
    public float Speed { get => Invoke(Service, 2, true).Scalar; set => Invoke(Service, 3, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public bool IsPlaying => Invoke(Service, 4, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public bool IsPaused => Invoke(Service, 5, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "s")]
    public double Time => double.Parse(Invoke(Service, 6, true).String, CultureInfo.InvariantCulture);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "s", Failure = "assigned_but_unloaded_sequence_throws; no_assignment_returns_zero", Summary = "Reads the loaded sequence or current authoring sequence duration; readonly query never imports or pins an asset.")]
    public double Duration => double.Parse(Invoke(Service, 7, true).String, CultureInfo.InvariantCulture);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Queues Play for SequenceSystem to capture target snapshots.")]
    public void Play() => Invoke(Service, 8, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Queues Stop for SequenceSystem to restore target snapshots.")]
    public void Stop() => Invoke(Service, 9, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Pause() => Invoke(Service, 10, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Resume() => Invoke(Service, 11, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "seconds=s" }, Summary = "Queues a finite double-precision seek, clamping negative input to zero.")]
    public void Seek(double seconds) { if (!double.IsFinite(seconds)) throw new ArgumentOutOfRangeException(nameof(seconds)); Invoke(Service, 12, false, text: [seconds.ToString("R", CultureInfo.InvariantCulture)]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Summary = "Assigns an exact current Scene object to a sequence binding; null explicitly clears this key.")]
    public void Bind(string key, GameObject? target) { ArgumentException.ThrowIfNullOrEmpty(key); Invoke(Service, 13, false, text: [key], related: target); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void ClearBindings() => Invoke(Service, 14, false);
}
