/// @file    RenderComponentsComponent.cs
/// @brief   描画 facade の実 CLR 接続、保存参照、除去と再追加時の birth を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#nullable enable
using Fbzz;
using System;
namespace ManagedFixtures;
public sealed class RenderComponentsComponent : Script
{
    public GameObject? target;
    public int operation;
    public int connectionMask;
    public int observedCount;
    public float observedWidth;
    public Vector3 observedPosition;
    public bool capacityPreserved;
    public bool cachedIsAlive;
    public bool savedReferenceMissing;
    public bool staleWriteRejected;
    public int invalidRejected;
    public bool materialCleared;
    public bool modelAccepted;
    public ComponentRef<LineRenderer> lineReference;
    LineRenderer? m_cachedLine;
    protected override void Update()
    {
        GameObject owner = target ?? GameObject;
        switch (operation) {
        case 1: {
            LineRenderer line = owner.GetComponent<LineRenderer>()!;
            SpriteRenderer sprite = owner.GetComponent<SpriteRenderer>()!;
            SortingGroup sorting = owner.GetComponent<SortingGroup>()!;
            TrailRenderer trail = owner.GetComponent<TrailRenderer>()!;
            MeshTrail meshTrail = owner.GetComponent<MeshTrail>()!;
            MeshRenderer mesh = owner.GetComponent<MeshRenderer>()!;
            SkinnedMeshRenderer skinned = owner.GetComponent<SkinnedMeshRenderer>()!;
            connectionMask = (line is null ? 0 : 1) | (sprite is null ? 0 : 2) | (sorting is null ? 0 : 4)
                | (trail is null ? 0 : 8) | (meshTrail is null ? 0 : 16) | (mesh is null ? 0 : 32) | (skinned is null ? 0 : 64);
            line!.SetLine(new(2, 3, 4), new(8, 9, 10)); line.SetColors(new(1, 0, 0, 0.75f), new(0, 0, 1, 0.125f));
            line.SetWidths(0.3f, 0.8f); line.Loop = true; line.Billboard = false; line.Shape = LineShape.TUBE;
            line.RadialSegments = 7; line.SortingLayer = int.MinValue; line.OrderInLayer = int.MaxValue;
            observedWidth = line.EndWidth; observedPosition = line.GetPosition(1); observedCount = line.PositionCount;
            Span<Vector3> shortOutput = stackalloc Vector3[1]; shortOutput[0] = new(99, 98, 97);
            CollectionReadResult read = line.GetPositions(shortOutput);
            capacityPreserved = read.Required == 2 && read.Written == 0 && shortOutput[0].X == 99;
            sprite!.Color = new(0, 1, 0, 0.5f); sprite.Size = new(4, 2); sprite.Pivot = new(0.2f, 0.8f);
            sprite.DrawMode = SpriteDrawMode.SLICED; sprite.FlipX = true; sprite.FlipY = false; sprite.ReceiveLighting = true;
            sprite.UseNativeSize = false; sprite.SortingLayer = 2; sprite.OrderInLayer = -3;
            sorting!.SortingLayer = -4; sorting.OrderInLayer = 11; sorting.Enabled = false;
            trail!.SetEnabled(false, false); trail.Duration = 0.6f; trail.SetWidths(0.7f, 0.1f);
            trail.SetColors(new(1, 1, 0, 1), new(0, 1, 1, 0)); trail.SetSampling(0.02f, 0.03f);
            trail.Alignment = TrailAlignment.WORLD_UP; trail.UVMode = TrailUVMode.TILE; trail.WidthEasing = TrailWidthEasing.EASE_IN_OUT;
            trail.SetAttachBone("Hand", new(1, 2, 3)); trail.BreakDistance = 5; trail.SetUV(-2, 3);
            meshTrail!.SetEnabled(false, false); meshTrail.Duration = 0.4f; meshTrail.SetSampling(0.04f, 0.05f);
            meshTrail.DoubleSided = false; meshTrail.AddExcludedMeshIndex(2);
            mesh!.Enabled = false; mesh.CastShadows = false; skinned!.Enabled = false; skinned.CastShadows = false;
            m_cachedLine = line; lineReference = new(line); cachedIsAlive = line.IsAlive;
            break;
        }
        case 2:
            cachedIsAlive = m_cachedLine is not null && m_cachedLine.IsAlive;
            savedReferenceMissing = lineReference.IsMissing;
            break;
        case 3:
            owner.RemoveComponent<LineRenderer>();
            cachedIsAlive = m_cachedLine is not null && m_cachedLine.IsAlive;
            savedReferenceMissing = lineReference.IsMissing;
            break;
        case 4:
            owner.AddComponent<LineRenderer>();
            cachedIsAlive = m_cachedLine is not null && m_cachedLine.IsAlive;
            savedReferenceMissing = lineReference.IsMissing;
            try { m_cachedLine!.SetWidths(99, 99); }
            catch (InvalidOperationException) { staleWriteRejected = true; }
            break;
        case 5: {
            LineRenderer line = owner.GetComponent<LineRenderer>()!;
            try { line.StartWidth = float.NaN; } catch (Exception) { invalidRejected |= 1; }
            try { line.Shape = (LineShape)2; } catch (Exception) { invalidRejected |= 2; }
            try { line.RadialSegments = 2; } catch (Exception) { invalidRejected |= 4; }
            observedWidth = line.StartWidth;
            break;
        }
        case 6:
            owner.GetComponent<MeshRenderer>()!.Clear();
            break;
        case 7:
            materialCleared = owner.GetComponent<LineRenderer>()!.TrySetSharedMaterial(default)
                && owner.GetComponent<SpriteRenderer>()!.TrySetSharedMaterial(default)
                && owner.GetComponent<TrailRenderer>()!.TrySetSharedMaterial(default)
                && owner.GetComponent<MeshTrail>()!.TrySetSharedMaterial(default);
            break;
        case 8: {
            GameObject created = Scene.Create("FreshClrPrimitive")!;
            MeshRenderer mesh = created.AddComponent<MeshRenderer>();
            mesh.SetPrimitive(PrimitiveMesh.CUBE);
            mesh.CastShadows = false;
            break;
        }
        case 9: {
            GameObject created = Scene.Create("FreshClrEmptyModel")!;
            modelAccepted = created.AddComponent<MeshRenderer>().TrySetModel(default);
            break;
        }
        }
    }
}
