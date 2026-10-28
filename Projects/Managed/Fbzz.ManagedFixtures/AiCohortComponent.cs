/// @file    AiCohortComponent.cs
/// @brief   Real CoreCLR crowd/pose and terrain directional-query bridge fixture.
/// @author  Hasegawa Jin
/// @date    2026-10-12
using Fbzz;
using Fbzz.AI;

namespace ManagedFixtures;

public sealed class AiCohortComponent : Script
{
    public int operation;
    public bool stepped, committed, floorHit, ceilingHit, sceneFloorHit, sceneCeilingHit, miss, invalidRejected;
    public int decisions, applied, objectStatus;
    public Vector3 position, velocity, floorPoint, floorNormal, ceilingPoint, ceilingNormal, sceneFloorPoint, sceneCeilingPoint;
    readonly AiCrowd m_crowd = new(1);
    readonly GameObject[] m_objects = new GameObject[1];
    readonly Vector3[] m_positions = new Vector3[1];
    readonly Quaternion[] m_rotations = new Quaternion[1];
    readonly ObjectStatus[] m_statuses = new ObjectStatus[1];

    protected override void Update()
    {
        if (operation == 1) { TerrainQueries(); return; }
        AiCrowdAgent agent = AiCrowdAgent.Create(new Vector3(0, -4, 0), seed: 42);
        agent.Intent = AiCrowdIntent.EXTERNAL;
        agent.Flags |= AiCrowdFlags.OVERRIDE_STEERING;
        agent.ExternalVelocity = new Vector3(1, 0, 0);
        m_crowd.SetAgent(0, agent);
        stepped = m_crowd.Step(0.1f);
        if (!stepped) return;
        agent = m_crowd.Snapshot[0]; position = agent.Position; velocity = agent.Velocity;
        decisions = (int)m_crowd.Stats.Decisions;
        m_objects[0] = GameObject; m_positions[0] = position; m_rotations[0] = new Quaternion(0, 0, 0, 2);
        BatchResult result = AiPoseBatch.TryCommitWorldPoses(m_objects, m_positions, m_rotations, m_statuses);
        committed = result.Succeeded; applied = result.AppliedCount; objectStatus = (int)m_statuses[0];
    }

    void TerrainQueries()
    {
        Terrain terrain = GetComponent<Terrain>() ?? throw new System.InvalidOperationException("Terrain fixture is missing its component.");
        Vector3 reference = new(0.5f, -4, 0.5f);
        floorHit = terrain.TrySampleFloorWorld(reference, 10, out TerrainSurfaceSample floor);
        ceilingHit = terrain.TrySampleCeilingWorld(reference, 10, out TerrainSurfaceSample ceiling);
        floorPoint = floor.Position; floorNormal = floor.Normal;
        ceilingPoint = ceiling.Position; ceilingNormal = ceiling.Normal;
        sceneFloorHit = Scene.TryGetTerrainFloorAt(reference, 10, out sceneFloorPoint, out _);
        sceneCeilingHit = Scene.TryGetTerrainCeilingAt(reference, 10, out sceneCeilingPoint, out _);
        miss = !terrain.TrySampleFloorWorld(reference, 1, out TerrainSurfaceSample absent) && absent.Position.Equals(Vector3.ZERO);
        invalidRejected = !Scene.TryGetTerrainCeilingAt(reference, -1, out Vector3 invalid, out _) && invalid.Equals(Vector3.ZERO);
    }
}
