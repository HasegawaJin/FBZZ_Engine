/// @file    WorldFamiliesComponent.cs
/// @brief   Actual CLR world-family facade queries, fixed geometry edits, playback and readonly draw boundaries.
/// @author  Hasegawa Jin
/// @date    2026-10-06
#nullable enable
using Fbzz;
namespace ManagedFixtures;
[RequireComponent(typeof(Terrain), typeof(Water), typeof(VFX), typeof(SequencePlayer))]
public sealed class WorldFamiliesComponent : Script
{
    public int connectionMask;
    public float terrainHeight;
    public float waterHeight;
    public bool sampled;
    public float sampleY;
    public bool playbackRequested;
    public double sequenceTime;
    public bool preciseSequenceTime;
    public bool rejectDrawMutation;
    Terrain? m_terrain;
    Water? m_water;
    VFX? m_vfx;
    SequencePlayer? m_sequence;
    protected override void Connect()
    {
        m_terrain = GetComponent<Terrain>(); m_water = GetComponent<Water>();
        m_vfx = GetComponent<VFX>(); m_sequence = GetComponent<SequencePlayer>();
        connectionMask = (m_terrain is null ? 0 : 1) | (m_water is null ? 0 : 2)
            | (m_vfx is null ? 0 : 4) | (m_sequence is null ? 0 : 8);
    }
    protected override void Update()
    {
        terrainHeight = m_terrain!.GetHeightLocal(1, 1);
        waterHeight = m_water!.GetSurfaceHeightWorld(1, 1, 0);
        sampled = m_terrain.TrySampleWorld(1, 1, out TerrainSurfaceSample sample);
        sampleY = sample.Position.Y;
        sequenceTime = m_sequence!.Time;
        preciseSequenceTime = sequenceTime == 1000000000000.125;
    }
    protected override void FixedUpdate()
    {
        m_terrain!.SetHeightAtGrid(1, 1, 4);
        m_vfx!.Play(); m_vfx.Trigger("Hit");
        m_sequence!.Seek(1000000000000.125); m_sequence.Play();
        playbackRequested = true;
    }
    protected override void DrawGizmos()
    {
        Terrain? terrain = GetComponent<Terrain>();
        if (terrain is not null && terrain.TrySampleWorld(1, 1, out TerrainSurfaceSample sample))
            Gizmos.Line(sample.Position, sample.Position + sample.Normal);
        if (rejectDrawMutation) {
            /// @note Deliberate runtime-negative fixture; separate analyzer tests keep production mutation diagnostics strict.
#pragma warning disable FBZZ2010
            GetComponent<VFX>()!.Stop();
#pragma warning restore FBZZ2010
        }
    }
}
