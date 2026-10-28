/// @file    RenderComponents.cs
/// @brief   線・Sprite・Trail・Meshの型付き操作と実rowの寿命を扱う。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Buffers;
using System.Globalization;
namespace Fbzz;

public enum LineSpace { LOCAL = 0, WORLD = 1 }

public sealed class TrailRenderer : RenderComponent
{
    internal TrailRenderer(GameObject owner, ulong token = 0) : base(owner, 38, (uint)ScriptServiceKind.SERVICE_TRAIL, token) { }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live")]
    public bool Enabled { get => Read(0).Flag; set => SetEnabled(value); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetEnabled(bool enabled, bool clearWhenDisabled = true) => Write(1, enabled ? 1 : 0, [clearWhenDisabled ? 1f : 0f]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live", Units = "s")]
    public float Duration { get => Read(2).Scalar; set => Write(3, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public int PointCount => Read(4).Int32;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Clear() => Write(5);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live")]
    public int MaxPoints { get => Read(6).Int32; set => Write(7, value); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "interval=s", "minimumDistance=m" })]
    public void SetSampling(float interval, float minimumDistance) => Write(9, values: [interval, minimumDistance]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Units = "m")]
    public void SetWidths(float start, float end) => Write(11, values: [start, end]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Units = "linear_rgba")]
    public void SetColors(Vector4 start, Vector4 end) => Write(13, values: [start.X, start.Y, start.Z, start.W, end.X, end.Y, end.Z, end.W]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live")]
    public TrailWidthEasing WidthEasing { get => (TrailWidthEasing)Read(14).Int32; set => Write(15, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live")]
    public TrailAlignment Alignment { get => (TrailAlignment)Read(16).Int32; set => Write(17, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live")]
    public TrailUVMode UVMode { get => (TrailUVMode)Read(18).Int32; set => Write(19, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live")]
    public int SmoothSubdivisions { get => Read(20).Int32; set => Write(21, value); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Units = "m", Coordinate = "bone_local")]
    public void SetAttachBone(string name, Vector3 offset = default) { ArgumentNullException.ThrowIfNull(name); Write(23, values: [offset.X, offset.Y, offset.Z], text: [name]); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void ClearAttachBone() => Write(26);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live", Units = "m")]
    public float BreakDistance { get => Read(27).Scalar; set => Write(28, values: [value]); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "scrollSpeed=uv/s", "tiling=ratio" })]
    public void SetUV(float scrollSpeed, float tiling) => Write(30, values: [scrollSpeed, tiling]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public MaterialAssetRef SharedMaterial { get { string value = Read(24).String; return value.Length == 0 ? default : new(value); } }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "false_for_missing_or_wrong_asset_without_changes")]
    public bool TrySetSharedMaterial(MaterialAssetRef value) => Write(25, text: [value.Reference]).Flag;
}
public sealed class MeshTrail : RenderComponent
{
    internal MeshTrail(GameObject owner, ulong token = 0) : base(owner, 39, (uint)ScriptServiceKind.SERVICE_MESH_TRAIL, token) { }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live")]
    public bool Enabled { get => Read(0).Flag; set => SetEnabled(value); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetEnabled(bool enabled, bool clearWhenDisabled = true) => Write(1, enabled ? 1 : 0, [clearWhenDisabled ? 1f : 0f]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live", Units = "s")]
    public float Duration { get => Read(2).Scalar; set => Write(3, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public int SampleCount => Read(4).Int32;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Clear() => Write(5);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live")]
    public int MaxSamples { get => Read(6).Int32; set => Write(7, value); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "interval=s", "minimumDistance=m" })]
    public void SetSampling(float interval, float minimumDistance) => Write(9, values: [interval, minimumDistance]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Units = "linear_rgba")]
    public void SetColors(Vector4 start, Vector4 end) => Write(11, values: [start.X, start.Y, start.Z, start.W, end.X, end.Y, end.Z, end.W]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", WriteRunModes = new[] { "Play" }, Write = "live")]
    public bool DoubleSided { get => Read(12).Flag; set => Write(13, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void AddExcludedMeshIndex(int index) => Write(14, index);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void ClearExcludedMeshIndices() => Write(15);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public MaterialAssetRef SharedMaterial { get { string value = Read(16).String; return value.Length == 0 ? default : new(value); } }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "false_for_missing_or_wrong_asset_without_changes")]
    public bool TrySetSharedMaterial(MaterialAssetRef value) => Write(17, text: [value.Reference]).Flag;
}

public readonly struct MeshVertex
{
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", ParameterCoordinates = new[] { "position=local", "normal=local_direction", "tangent=local_direction" })]
    public MeshVertex(Vector3 position, Vector3 normal, Vector3 tangent, Vector2 uv, Vector4 color)
    { Position = position; Normal = normal; Tangent = tangent; UV = uv; Color = color; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "m", Coordinate = "local")]
    public Vector3 Position { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Coordinate = "local_direction")]
    public Vector3 Normal { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Coordinate = "local_direction")]
    public Vector3 Tangent { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "uv")]
    public Vector2 UV { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "linear_rgba")]
    public Vector4 Color { get; }
    internal bool IsFinite => Position.IsFinite && Normal.IsFinite && Tangent.IsFinite && float.IsFinite(UV.X) && float.IsFinite(UV.Y)
        && float.IsFinite(Color.X) && float.IsFinite(Color.Y) && float.IsFinite(Color.Z) && float.IsFinite(Color.W);
    internal void Encode(Span<float> target)
    {
        target[0] = Position.X; target[1] = Position.Y; target[2] = Position.Z;
        target[3] = Normal.X; target[4] = Normal.Y; target[5] = Normal.Z;
        target[6] = Tangent.X; target[7] = Tangent.Y; target[8] = Tangent.Z;
        target[9] = UV.X; target[10] = UV.Y;
        target[11] = Color.X; target[12] = Color.Y; target[13] = Color.Z; target[14] = Color.W;
    }
    internal static MeshVertex Decode(NativeServiceResult packet, int offset) => new(
        new(packet.Value(offset), packet.Value(offset + 1), packet.Value(offset + 2)),
        new(packet.Value(offset + 3), packet.Value(offset + 4), packet.Value(offset + 5)),
        new(packet.Value(offset + 6), packet.Value(offset + 7), packet.Value(offset + 8)),
        new(packet.Value(offset + 9), packet.Value(offset + 10)),
        new(packet.Value(offset + 11), packet.Value(offset + 12), packet.Value(offset + 13), packet.Value(offset + 14)));
}
public sealed class MeshRenderer : RenderComponent
{
    internal MeshRenderer(GameObject owner, ulong token = 0) : base(owner, 40, (uint)ScriptServiceKind.SERVICE_MESH, token) { }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Read(0).Flag; set => Write(1, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool CastShadows { get => Read(2).Flag; set => Write(3, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_missing_model_or_invalid_submesh_without_changes")]
    public bool TrySetModel(ModelAssetRef model, int submesh = 0) => Write(5, submesh, text: [model.Reference]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void SetPrimitive(PrimitiveMesh primitive) => Write(6, (long)primitive);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public int VertexCount => Read(7).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public int IndexCount => Read(8).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void Clear() => Write(9);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_missing_or_wrong_asset_without_changes")]
    public bool TrySetProceduralMaterial(MaterialAssetRef material) => Write(11, text: [material.Reference]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Coordinate = "local", Failure = "invalid_vertex_index_or_transfer_throws_without_mesh_changes", Summary = "Atomically replaces a clockwise triangle mesh; transfer packet capacity does not limit total mesh size.")]
    public void Apply(ReadOnlySpan<MeshVertex> vertices, ReadOnlySpan<uint> indices) => Transfer(vertices, indices, false);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Coordinate = "local", Failure = "vertex_count_mismatch_or_invalid_transfer_throws_without_mesh_changes")]
    public void UpdateVertices(ReadOnlySpan<MeshVertex> vertices) => Transfer(vertices, default, true);
    void Transfer(ReadOnlySpan<MeshVertex> vertices, ReadOnlySpan<uint> indices, bool update)
    {
        foreach (MeshVertex vertex in vertices) if (!vertex.IsFinite) throw new ArgumentOutOfRangeException(nameof(vertices));
        if (indices.Length % 3 != 0) throw new ArgumentException("Triangle indices must be triples.", nameof(indices));
        foreach (uint index in indices) if (index >= vertices.Length) throw new ArgumentOutOfRangeException(nameof(indices));
        long counts = unchecked((long)((ulong)(uint)vertices.Length | ((ulong)(uint)indices.Length << 32)));
        long token = Write(50, counts, [update ? 1f : 0f]).Integer;
        if (token <= 0) throw new InvalidOperationException("Invalid mesh reception.");
        try {
            Span<float> packet = stackalloc float[120];
            for (int offset = 0; offset < vertices.Length;) {
                int count = Math.Min(8, vertices.Length - offset);
                for (int index = 0; index < count; index++) vertices[offset + index].Encode(packet.Slice(index * 15, 15));
                Write(51, token, packet[..(count * 15)], [Number(offset)]); offset += count;
            }
            string[] indexPacket = ArrayPool<string>.Shared.Rent(128);
            try {
                for (int offset = 0; offset < indices.Length;) {
                    int count = Math.Min(127, indices.Length - offset);
                    indexPacket[0] = Number(offset);
                    for (int index = 0; index < count; index++) indexPacket[index + 1] = indices[offset + index].ToString(CultureInfo.InvariantCulture);
                    Write(52, token, text: indexPacket.AsSpan(0, count + 1)); offset += count;
                }
            } finally { ArrayPool<string>.Shared.Return(indexPacket, clearArray: true); }
            Write(53, token); token = 0;
        } finally { if (token != 0) { try { Write(54, token); } catch { } } }
    }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Coordinate = "local", Failure = "insufficient_capacity_returns_required_written_zero; failure_preserves_output")]
    public CollectionReadResult GetVertices(Span<MeshVertex> output)
    {
        int count = VertexCount; if (output.Length < count) return new(count, 0);
        MeshVertex[] staged = ArrayPool<MeshVertex>.Shared.Rent(count);
        try {
            for (int offset = 0; offset < count;) {
                NativeServiceResult packet = Read(55, offset, [Number(count)]);
                if (packet.ValueCount == 0 || packet.ValueCount % 15 != 0 || packet.ValueCount / 15 > count - offset) throw new InvalidOperationException("Invalid vertex page.");
                for (int index = 0; index < packet.ValueCount; index += 15) staged[offset++] = MeshVertex.Decode(packet, index);
            }
            staged.AsSpan(0, count).CopyTo(output); return new(count, count);
        } finally { ArrayPool<MeshVertex>.Shared.Return(staged); }
    }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Failure = "insufficient_capacity_returns_required_written_zero; failure_preserves_output")]
    public CollectionReadResult GetIndices(Span<uint> output)
    {
        int count = IndexCount; if (output.Length < count) return new(count, 0);
        uint[] staged = ArrayPool<uint>.Shared.Rent(count);
        try {
            for (int offset = 0; offset < count;) {
                NativeServiceResult packet = Read(56, offset, [Number(count)]);
                if (packet.Text.Length == 0 || packet.Text.Length > count - offset) throw new InvalidOperationException("Invalid index page.");
                foreach (string text in packet.Text) staged[offset++] = uint.Parse(text, NumberStyles.None, CultureInfo.InvariantCulture);
            }
            staged.AsSpan(0, count).CopyTo(output); return new(count, count);
        } finally { ArrayPool<uint>.Shared.Return(staged); }
    }
}
public sealed class SkinnedMeshRenderer : RenderComponent
{
    internal SkinnedMeshRenderer(GameObject owner, ulong token = 0) : base(owner, 41, (uint)ScriptServiceKind.SERVICE_SKINNED_MESH, token) { }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Read(0).Flag; set => Write(1, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool CastShadows { get => Read(2).Flag; set => Write(3, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_missing_or_non_skinned_model_without_changes")]
    public bool TrySetModel(ModelAssetRef model) => Write(5, text: [model.Reference]).Flag;
}

public enum LineShape { RIBBON = 0, TUBE = 1 }
public enum SpriteDrawMode { SIMPLE = 0, SLICED = 1, TILED = 2 }
public enum TrailAlignment { CAMERA_FACING = 0, WORLD_UP = 1 }
public enum TrailUVMode { STRETCH = 0, TILE = 1 }
public enum TrailWidthEasing { LINEAR = 0, EASE_IN = 1, EASE_OUT = 2, EASE_IN_OUT = 3 }
public enum PrimitiveMesh { CUBE = 0, SPHERE = 1, PLANE = 2, QUAD = 3, CYLINDER = 4, CONE = 5, TORUS = 6, CAPSULE = 7 }

public abstract class RenderComponent : NativeComponent
{
    private protected RenderComponent(GameObject owner, uint kind, uint service, ulong token) : base(owner, kind, token) { m_service = service; }
    readonly uint m_service;
    private protected NativeServiceResult Read(uint method, long integer = 0, ReadOnlySpan<string> text = default)
        => Invoke(m_service, method, true, integer, text: text);
    private protected NativeServiceResult Write(uint method, long integer = 0, ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default)
        => Invoke(m_service, method, false, integer, values, text);
    private protected static string Number(int value) => value.ToString(CultureInfo.InvariantCulture);
}

public sealed class LineRenderer : RenderComponent
{
    internal LineRenderer(GameObject owner, ulong token = 0) : base(owner, 35, (uint)ScriptServiceKind.SERVICE_LINE, token) { }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Read(0).Flag; set => Write(1, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "linear_rgba")]
    public Vector4 StartColor { get => Read(2).Vector4; set => Write(3, values: [value.X, value.Y, value.Z, value.W]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "linear_rgba")]
    public Vector4 EndColor { get => Read(4).Vector4; set => Write(5, values: [value.X, value.Y, value.Z, value.W]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "m", Coordinate = "selected_line_space")]
    public float StartWidth { get => Read(6).Scalar; set => Write(7, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "m", Coordinate = "selected_line_space")]
    public float EndWidth { get => Read(8).Scalar; set => Write(9, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public LineSpace Space { get => (LineSpace)Read(10).Int32; set => Write(11, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Loop { get => Read(12).Flag; set => Write(13, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Billboard { get => Read(16).Flag; set => Write(17, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public LineShape Shape { get => (LineShape)Read(18).Int32; set => Write(19, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public int RadialSegments { get => Read(20).Int32; set => Write(21, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public int SortingLayer { get => Read(22).Int32; set => Write(23, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public int OrderInLayer { get => Read(24).Int32; set => Write(25, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public int PositionCount => Read(14).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "m", Coordinate = "selected_line_space")]
    public Vector3 GetPosition(int index) => Read(15, index).Vector3;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "start=m", "end=m" }, Summary = "Replaces the path with two points; worldSpace selects world or local coordinates.")]
    public void SetLine(Vector3 start, Vector3 end, bool worldSpace = true) => Write(28, worldSpace ? 1 : 0, [start.X, start.Y, start.Z, end.X, end.Y, end.Z]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Units = "linear_rgba")]
    public void SetColors(Vector4 start, Vector4 end) => Write(29, values: [start.X, start.Y, start.Z, start.W, end.X, end.Y, end.Z, end.W]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Units = "m")]
    public void SetWidths(float start, float end) => Write(30, values: [start, end]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public MaterialAssetRef SharedMaterial { get { string value = Read(26).String; return value.Length == 0 ? default : new(value); } }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_missing_or_wrong_asset_without_changes")]
    public bool TrySetSharedMaterial(MaterialAssetRef value) => Write(27, text: [value.Reference]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Units = "m", Coordinate = "selected_line_space", Failure = "invalid_input_or_transfer_throws_without_path_changes", Summary = "Atomically replaces every point; internal packet size does not limit the path length.")]
    public void SetPositions(ReadOnlySpan<Vector3> positions, bool worldSpace = true)
    {
        foreach (Vector3 value in positions) if (!value.IsFinite) throw new ArgumentOutOfRangeException(nameof(positions));
        long token = Write(50, positions.Length, [worldSpace ? 1f : 0f]).Integer;
        if (token <= 0) throw new InvalidOperationException("Invalid line reception.");
        try {
            Span<float> packet = stackalloc float[126];
            for (int offset = 0; offset < positions.Length;) {
                int count = Math.Min(42, positions.Length - offset);
                for (int index = 0; index < count; index++) {
                    Vector3 point = positions[offset + index]; packet[index * 3] = point.X; packet[index * 3 + 1] = point.Y; packet[index * 3 + 2] = point.Z;
                }
                Write(51, token, packet[..(count * 3)], [Number(offset)]); offset += count;
            }
            Write(52, token); token = 0;
        } finally { if (token != 0) { try { Write(53, token); } catch { } } }
    }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "m", Coordinate = "selected_line_space", Failure = "insufficient_capacity_returns_required_written_zero; failure_preserves_output")]
    public CollectionReadResult GetPositions(Span<Vector3> output)
    {
        int count = PositionCount;
        if (output.Length < count) return new(count, 0);
        Vector3[] staged = ArrayPool<Vector3>.Shared.Rent(count);
        try {
            for (int offset = 0; offset < count;) {
                NativeServiceResult result = Read(54, offset, [Number(count)]);
                if (result.ValueCount == 0 || result.ValueCount % 3 != 0 || result.ValueCount / 3 > count - offset) throw new InvalidOperationException("Invalid line page.");
                for (int index = 0; index < result.ValueCount; index += 3) staged[offset++] = new(result.Value(index), result.Value(index + 1), result.Value(index + 2));
            }
            staged.AsSpan(0, count).CopyTo(output); return new(count, count);
        } finally { ArrayPool<Vector3>.Shared.Return(staged); }
    }
}

public sealed class SpriteRenderer : RenderComponent
{
    internal SpriteRenderer(GameObject owner, ulong token = 0) : base(owner, 36, (uint)ScriptServiceKind.SERVICE_SPRITE, token) { }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Read(0).Flag; set => Write(1, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "linear_rgba")]
    public Vector4 Color { get => Read(2).Vector4; set => Write(3, values: [value.X, value.Y, value.Z, value.W]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "m", Coordinate = "local")]
    public Vector2 Size { get => Read(4).Vector2; set => Write(5, values: [value.X, value.Y]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "ratio", Coordinate = "normalized_sprite")]
    public Vector2 Pivot { get => Read(6).Vector2; set => Write(7, values: [value.X, value.Y]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public SpriteDrawMode DrawMode { get => (SpriteDrawMode)Read(8).Int32; set => Write(9, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool FlipX { get => Read(10).Flag; set => Write(11, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool FlipY { get => Read(12).Flag; set => Write(13, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool ReceiveLighting { get => Read(14).Flag; set => Write(15, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool UseNativeSize { get => Read(16).Flag; set => Write(17, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public int SortingLayer { get => Read(18).Int32; set => Write(19, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public int OrderInLayer { get => Read(20).Int32; set => Write(21, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public string SpriteReference => Read(22).String;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_missing_texture_or_sprite_without_changes", Summary = "Assigns a texture GUID and optional existing sprite name or subasset ID.")]
    public bool TrySetSprite(TextureAssetRef texture, string sprite = "") { ArgumentNullException.ThrowIfNull(sprite); return Write(23, text: [texture.Reference, sprite]).Flag; }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public MaterialAssetRef SharedMaterial { get { string value = Read(24).String; return value.Length == 0 ? default : new(value); } }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", Failure = "false_for_missing_or_wrong_asset_without_changes")]
    public bool TrySetSharedMaterial(MaterialAssetRef value) => Write(25, text: [value.Reference]).Flag;
}

public sealed class SortingGroup : RenderComponent
{
    internal SortingGroup(GameObject owner, ulong token = 0) : base(owner, 37, (uint)ScriptServiceKind.SERVICE_SORTING_GROUP, token) { }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Read(0).Flag; set => Write(1, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public int SortingLayer { get => Read(2).Int32; set => Write(3, (long)value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public int OrderInLayer { get => Read(4).Int32; set => Write(5, (long)value); }
}

