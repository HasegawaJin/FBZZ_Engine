/// @file    Gizmos.cs
/// @brief   描画専用 callback の世界座標コマンドを bounded buffer へ記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;

namespace Fbzz;

internal readonly record struct GizmoDrawCommand(int Kind, Vector3 A, Vector3 B, float Radius, Vector4 Color, bool DepthTest);
internal sealed class GizmoDrawContext(int capacity)
{
    internal List<GizmoDrawCommand> Commands { get; } = new(Math.Min(capacity, 64));
    internal int Capacity { get; } = capacity;
    internal bool DepthTest { get; set; }
}

/// @note DrawGizmos / DrawGizmosSelected でだけ使用する。座標は world、長さは m。世界状態を変更しない。
public sealed class ScriptGizmos
{
    internal ScriptGizmos() { }
    static GizmoDrawContext Context() => ScriptApiRuntime.RequireScope().Gizmos
        ?? throw new InvalidOperationException("Gizmos requires a DrawGizmos or DrawGizmosSelected callback.");
    [ApiContract(Read = "draw", Write = "draw", Purity = ApiPurity.ENGINE)]
    public bool DepthTest { get => Context().DepthTest; set => Context().DepthTest = value; }
    [ApiContract(Call = "draw", ParameterCoordinates = new[] { "from=world", "to=world" }, ParameterUnits = new[] { "from=m", "to=m" })]
    public void Line(Vector3 from, Vector3 to, Vector4? color = null) => Add(0, from, to, 0, color);
    [ApiContract(Call = "draw", ParameterCoordinates = new[] { "center=world" }, ParameterUnits = new[] { "center=m", "radius=m" })]
    public void Sphere(Vector3 center, float radius, Vector4? color = null) => Add(1, center, Vector3.ZERO, radius, color);
    [ApiContract(Call = "draw", ParameterCoordinates = new[] { "center=world" }, ParameterUnits = new[] { "center=m", "halfExtents=m" })]
    public void Box(Vector3 center, Vector3 halfExtents, Vector4? color = null)
    {
        if (halfExtents.X < 0 || halfExtents.Y < 0 || halfExtents.Z < 0) throw new ArgumentOutOfRangeException(nameof(halfExtents));
        Add(2, center, halfExtents, 0, color);
    }
    static void Add(int kind, Vector3 a, Vector3 b, float radius, Vector4? color)
    {
        GizmoDrawContext context = Context();
        Vector4 tint = color ?? new Vector4(1, 1, 1, 1);
        if (!a.IsFinite || !b.IsFinite || !float.IsFinite(radius) || radius < 0 || !float.IsFinite(tint.X)
            || !float.IsFinite(tint.Y) || !float.IsFinite(tint.Z) || !float.IsFinite(tint.W))
            throw new ArgumentOutOfRangeException(nameof(a), "Gizmo coordinates, size and color must be finite and nonnegative in size.");
        if (context.Commands.Count >= context.Capacity) throw new InvalidOperationException("Gizmo command capacity is exhausted; configure a larger draw buffer or draw fewer primitives.");
        context.Commands.Add(new(kind, a, b, radius, tint, context.DepthTest));
    }
}
