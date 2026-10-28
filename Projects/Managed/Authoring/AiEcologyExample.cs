/// @file    AiEcologyExample.cs
/// @brief   Caller-owned ecology policy joined to batched crowd movement, finite food and explicit birth acceptance.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;
using Fbzz.AI;

namespace Fbzz.Samples;

/// @note Authoring example, not a Script component. The owner supplies terrain, obstacles, object pooling, camera visibility and media.
/// @note Ordinary fish have no managed BT. At most eight special predators and eight filter feeders use the optional handlers below.
public sealed class AiEcologyExample : IDisposable
{
    public const uint OPEN_WATER = 1, REEF = 2, BOTTOM = 4;
    const int MAX_SPECIAL = 8;
    readonly Vector3[] m_positions;
    readonly Quaternion[] m_rotations;
    readonly Predator[] m_predators = new Predator[MAX_SPECIAL];
    readonly FilterFeeder[] m_filters = new FilterFeeder[MAX_SPECIAL];
    float m_densityElapsed;
    bool m_disposed;

    public AiEcology Ecology { get; }
    public AiCrowd Crowd { get; }
    public AiDensityField Density { get; }
    public bool EcologyEvaluatedThisUpdate { get; private set; }
    /// @note Read once after a successful Update. Not-due ecology steps retain their old span; this view hides those old events.
    public ReadOnlySpan<AiEcologyEvent> NewEvents => EcologyEvaluatedThisUpdate ? Ecology.Events : ReadOnlySpan<AiEcologyEvent>.Empty;

    public AiEcologyExample(int capacity, ReadOnlySpan<AiEcologySpecies> species, AiDensityField density, uint seed = 1)
    {
        ArgumentNullException.ThrowIfNull(density);
        Density = density;
        Ecology = new(capacity, species, seed);
        AiEcologySettings policy = AiEcologySettings.CreateDefault(capacity);
        policy.PopulationCap = capacity;
        policy.MaxPendingSpawns = Math.Min(16, capacity);
        if (!Ecology.TryConfigure(policy)) throw new ArgumentException("Species do not fit the bounded policy.", nameof(species));
        Crowd = new(capacity, species.Length);
        m_positions = new Vector3[capacity];
        m_rotations = new Quaternion[capacity];
        Array.Fill(m_rotations, Quaternion.IDENTITY);
        for (int i = 0; i < species.Length; ++i) Crowd.SetProfile(i, CreateMovementProfile(species[i]));
        // AiCrowd starts with active slots. Unbound pool slots must be disabled explicitly.
        for (int i = 0; i < capacity; ++i)
        {
            AiCrowdAgent agent = Crowd.GetAgent(i);
            agent.Flags = AiCrowdFlags.NONE;
            Crowd.SetAgent(i, agent);
        }
    }

    /// @note Fictional diets, in meters and game-defined energy. Roles never imply who wins or what a real fish eats.
    public static AiEcologySpecies[] CreateSpeciesExamples()
    {
        AiEcologySpecies[] species =
        [
            Recipe(100, AiEcologyTier.PRODUCER, AiEcologyFoodTags.PLANKTON | AiEcologyFoodTags.PLANT, AiEcologyFoodTags.NONE, .02f, 0, 0, OPEN_WATER | REEF),
            Recipe(110, AiEcologyTier.PRIMARY_CONSUMER, AiEcologyFoodTags.GRAZER, AiEcologyFoodTags.PLANKTON, .30f, .005f, .08f, OPEN_WATER | REEF),
            Recipe(120, AiEcologyTier.SMALL_PREDATOR, AiEcologyFoodTags.PREDATOR, AiEcologyFoodTags.GRAZER, .90f, .10f, .65f, OPEN_WATER | REEF),
            Recipe(130, AiEcologyTier.APEX_PREDATOR, AiEcologyFoodTags.PREDATOR, AiEcologyFoodTags.GRAZER | AiEcologyFoodTags.PREDATOR, 4, .15f, 1.5f, OPEN_WATER),
            Recipe(140, AiEcologyTier.SCAVENGER, AiEcologyFoodTags.GRAZER, AiEcologyFoodTags.CARRION, .45f, .02f, 2, REEF | BOTTOM)
        ];
        species[0].EnergyDrainPerSecond = 0;
        species[0].ReproductionInterval = 0;
        species[0].ResourceUnits = 16;
        species[0].PopulationCap = 64;
        return species;
    }

    /// @note A large filter feeder or a chain colony remains a plankton consumer. Give each variant its own StableId.
    public static AiEcologySpecies CreatePlanktonVariant(ulong stableId, float bodySize)
    {
        AiEcologySpecies species = Recipe(stableId, AiEcologyTier.PRIMARY_CONSUMER, AiEcologyFoodTags.GRAZER,
            AiEcologyFoodTags.PLANKTON, bodySize, .005f, .08f, OPEN_WATER);
        species.PopulationCap = 8;
        return species;
    }

    /// @note Optional authored anchors. A visible skeleton and visual eggs never create resource or spawn capacity themselves.
    public static AiEcologySite[] CreateSiteExamples(Vector3 skeletonAnchor, Vector3 eggBedAnchor) =>
    [
        new() { StableId = 1000, Kind = AiEcologySiteKind.NUTRIENT, Position = skeletonAnchor, Radius = 4,
            Resources = 120, Capacity = 120, RecoveryRate = 0, HabitatMask = REEF | BOTTOM,
            FoodTags = AiEcologyFoodTags.CARRION, SpeciesTagsMask = AiEcologyFoodTags.GRAZER | AiEcologyFoodTags.PREDATOR },
        new() { StableId = 1001, Kind = AiEcologySiteKind.SPAWNING, Position = eggBedAnchor, Radius = 3,
            HabitatMask = OPEN_WATER | REEF | BOTTOM, SpeciesTagsMask = AiEcologyFoodTags.GRAZER | AiEcologyFoodTags.PREDATOR,
            SpawnCapacity = 16 }
    ];

    /// @note BodySize is a conservative diameter here; art scale stays caller-owned. Tune this provisional motion profile per asset.
    public static AiCrowdProfile CreateMovementProfile(AiEcologySpecies species)
    {
        bool benthic = (species.HabitatMask & BOTTOM) != 0 && (species.HabitatMask & OPEN_WATER) == 0;
        AiCrowdProfile profile = benthic
            ? AiCrowdProfile.CreateFloorFollowing() : AiCrowdProfile.CreateSwimming();
        profile.BodyRadius = species.BodySize * .5f;
        profile.SeparationRadius = MathF.Max(.10f, species.BodySize * 1.25f);
        profile.NeighborRadius = MathF.Max(2, species.BodySize * 3);
        profile.FloorOffset = profile.BodyRadius + .05f;
        profile.ArrivalRadius = MathF.Max(.10f, profile.BodyRadius);
        profile.SlowingRadius = MathF.Max(2, profile.BodyRadius * 3);
        return profile;
    }

    /// @note Bind an already-authored body. No camera-relative placement or GameObject creation occurs here.
    public bool TryAddAuthoredAgent(int speciesIndex, Vector3 position, float floor, float ceiling, out AiEcologyHandle handle)
    {
        RequireLive();
        handle = AiEcologyHandle.INVALID;
        if (!FitsEnvelope(speciesIndex, position, floor, ceiling) || !Ecology.TryAdd(speciesIndex, position, out handle)) return false;
        BindCrowdSlot(handle, position, floor, ceiling);
        return true;
    }

    /// @note Supply the result of bounded habitat/terrain/obstacle sampling and a reserved scene object. Rejected reservations roll back.
    public bool AcceptSampledSpawn(AiEcologyHandle handle, Vector3 position, float floor, float ceiling,
        bool habitatSafe, bool cameraVisible, bool poolObjectReserved)
    {
        RequireLive();
        if (!Ecology.IsValid(handle) || Ecology.Slots[handle.Slot].State != AiEcologySlotState.PENDING_SPAWN) return false;
        int species = Ecology.Slots[handle.Slot].SpeciesIndex;
        if (!habitatSafe || cameraVisible || !poolObjectReserved || !FitsEnvelope(species, position, floor, ceiling)
            || !Ecology.CommitSpawn(handle, position))
        {
            Ecology.CancelSpawn(handle);
            return false;
        }
        BindCrowdSlot(handle, position, floor, ceiling);
        return true;
    }

    /// @note Exactly one native crowd call per successful frame. All following position, decision and nutrition work is managed.
    /// @note Use a regular owner callback. Clamped long frames are not replayed; all three clocks use the same bounded delta.
    public bool Update(float deltaSeconds)
    {
        RequireLive();
        EcologyEvaluatedThisUpdate = false;
        if (!float.IsFinite(deltaSeconds) || deltaSeconds < 0) return false;
        float maximumDelta = Crowd.Settings.MaximumDelta;
        if (!float.IsFinite(maximumDelta) || maximumDelta <= 0 || maximumDelta > .5f) return false;
        float dt = MathF.Min(deltaSeconds, maximumDelta);
        if (!Crowd.Step(dt)) return false;
        Crowd.CopyPositions(m_positions);
        Ecology.UpdatePositions(m_positions);
        CacheRotations();
        m_densityElapsed += dt;
        EcologyEvaluatedThisUpdate = Ecology.Step(dt);
        if (EcologyEvaluatedThisUpdate)
        {
            ApplyEcologyMovements();
            float elapsed = MathF.Min(.5f, m_densityElapsed);
            Density.Step(elapsed, 0, 0, Vector3.ZERO);
            UpdateFilterFeeders(elapsed);
            m_densityElapsed = 0;
        }
        RefreshMovingTargets();
        UpdateSpecialPredators(dt);
        return true;
    }

    /// @note The owner batches these world inputs through AiPoseBatch.TryCommitWorldPoses with its reused GameObject/status arrays.
    /// @note Only bind active/corpse render objects; FREE and PENDING_SPAWN slots have no visible body yet.
    public bool CopyPoseInputs(Span<Vector3> positions, Span<Quaternion> rotations)
    {
        if (positions.Length < Crowd.Count || rotations.Length < Crowd.Count) return false;
        Crowd.CopyPositions(positions);
        m_rotations.AsSpan().CopyTo(rotations);
        return true;
    }

    /// @note Optional low-frequency density feeding, limited to eight authored handlers. Local offset uses asset +Z-forward coordinates.
    public bool TryAttachFilterFeeder(AiEcologyHandle handle, Vector3 localMouthOffset, float mouthRadius,
        float energyPerSecond = 2, float searchRadius = 8, float minimumDensity = .05f)
    {
        RequireLive();
        if (!IsAlive(handle) || !Finite(localMouthOffset) || !Positive(mouthRadius) || !Positive(energyPerSecond)
            || !Positive(searchRadius) || !float.IsFinite(minimumDensity) || minimumDensity < 0) return false;
        AiEcologySpecies species = Ecology.Species[Ecology.Slots[handle.Slot].SpeciesIndex];
        if (species.Tier != AiEcologyTier.PRIMARY_CONSUMER || species.Diet != AiEcologyFoodTags.PLANKTON) return false;
        int index = FindFilterSlot(handle);
        if (index < 0) return false;
        m_filters[index] = new() { Handle = handle, MouthOffset = localMouthOffset, MouthRadius = mouthRadius,
            EnergyPerSecond = energyPerSecond, SearchRadius = searchRadius, MinimumDensity = minimumDensity, Enabled = true };
        return true;
    }

    /// @note A few authored predator exceptions share a definition but have private blackboards. Default fish use only ecology/crowd.
    public bool TryAttachSpecialPredator(AiEcologyHandle handle)
    {
        RequireLive();
        if (!IsAlive(handle)) return false;
        AiEcologyTier tier = Ecology.Species[Ecology.Slots[handle.Slot].SpeciesIndex].Tier;
        if (tier is not AiEcologyTier.SMALL_PREDATOR and not AiEcologyTier.APEX_PREDATOR) return false;
        for (int i = 0; i < m_predators.Length; ++i)
        {
            if (m_predators[i].Tree is not null && m_predators[i].Handle == handle) return true;
            if (m_predators[i].Tree is null)
            {
                m_predators[i] = new() { Handle = handle, Tree = PredatorPlan.Tree.CreateInstance(.5) };
                return true;
            }
        }
        return false;
    }

    /// @note The caller computes its custom danger predicate. Changing a decision bool wakes the next scheduled Tick; positions update quietly.
    public bool SetPredatorPerception(AiEcologyHandle handle, bool danger, Vector3 threat, bool hasWaypoint, Vector3 waypoint)
    {
        RequireLive();
        if (!Finite(threat) || !Finite(waypoint)) return false;
        for (int i = 0; i < m_predators.Length; ++i)
        {
            Predator predator = m_predators[i];
            if (predator.Tree is null || predator.Handle != handle || !IsAlive(handle)) continue;
            Blackboard board = predator.Tree.Blackboard;
            board.Set(PredatorPlan.Threat, threat, false);
            board.Set(PredatorPlan.Waypoint, waypoint, false);
            board.Set(PredatorPlan.Danger, danger);
            board.Set(PredatorPlan.HasWaypoint, hasWaypoint);
            return true;
        }
        return false;
    }

    public void Dispose()
    {
        if (m_disposed) return;
        foreach (Predator predator in m_predators) predator.Tree?.Dispose();
        m_disposed = true;
    }

    static AiEcologySpecies Recipe(ulong id, AiEcologyTier tier, AiEcologyFoodTags tags, AiEcologyFoodTags diet,
        float body, float minimumPrey, float maximumPrey, uint habitat)
    {
        AiEcologySpecies species = AiEcologySpecies.CreateDefault(tier, tags, diet);
        species.StableId = id; species.BodySize = body; species.MinPreySize = minimumPrey;
        species.MaxPreySize = maximumPrey; species.HabitatMask = habitat;
        return species;
    }

    void BindCrowdSlot(AiEcologyHandle handle, Vector3 position, float floor, float ceiling)
    {
        AiEcologySlot slot = Ecology.Slots[handle.Slot];
        AiCrowdAgent agent = AiCrowdAgent.Create(position, (uint)slot.SpeciesIndex, (uint)slot.SpeciesIndex, slot.Seed);
        agent.Floor = floor; agent.Ceiling = ceiling;
        if (Ecology.Species[slot.SpeciesIndex].Tier == AiEcologyTier.PRODUCER) agent.Flags = AiCrowdFlags.NONE;
        Crowd.SetAgent(handle.Slot, agent);
        m_rotations[handle.Slot] = Quaternion.IDENTITY;
    }

    void ApplyEcologyMovements()
    {
        ReadOnlySpan<AiEcologySlot> slots = Ecology.Slots;
        ReadOnlySpan<AiEcologyMovement> movements = Ecology.Movements;
        for (int i = 0; i < slots.Length; ++i)
        {
            AiCrowdAgent agent = Crowd.GetAgent(i); // Managed snapshot lookup, no interop.
            bool moves = slots[i].State == AiEcologySlotState.ALIVE && Ecology.Species[slots[i].SpeciesIndex].Tier != AiEcologyTier.PRODUCER;
            if (!moves) { agent.Flags = AiCrowdFlags.NONE; Crowd.SetAgent(i, agent); continue; }
            agent.Flags |= AiCrowdFlags.ACTIVE;
            SetEcologyDirective(ref agent, movements[i]);
            Crowd.SetAgent(i, agent);
        }
    }

    static void SetEcologyDirective(ref AiCrowdAgent agent, AiEcologyMovement movement)
    {
        AiCrowdIntent intent = movement.Intent switch
        {
            AiEcologyIntent.CHASE => movement.Target.Slot >= 0 ? AiCrowdIntent.PURSUE : AiCrowdIntent.ARRIVE,
            AiEcologyIntent.FLEE => AiCrowdIntent.FLEE,
            AiEcologyIntent.HOLD => AiCrowdIntent.HOLD,
            _ => AiCrowdIntent.WANDER
        };
        if ((intent is AiCrowdIntent.PURSUE or AiCrowdIntent.FLEE or AiCrowdIntent.ARRIVE)
            && (agent.Target - movement.TargetPosition).LengthSquared > .000001f)
            agent.Flags |= AiCrowdFlags.REQUEST_DECISION;
        SetDirective(ref agent, intent, movement.TargetPosition);
    }

    void RefreshMovingTargets()
    {
        ReadOnlySpan<AiEcologySlot> slots = Ecology.Slots;
        for (int i = 0; i < slots.Length; ++i)
        {
            if (slots[i].State != AiEcologySlotState.ALIVE) continue;
            AiEcologyHandle target = slots[i].FleeRemaining > 0 ? slots[i].Threat : slots[i].Target;
            if (!Ecology.IsValid(target) || (uint)target.Slot >= (uint)Crowd.Count) continue;
            AiCrowdAgent agent = Crowd.GetAgent(i);
            if (agent.Intent is not AiCrowdIntent.PURSUE and not AiCrowdIntent.FLEE) continue;
            AiCrowdAgent prey = Crowd.GetAgent(target.Slot);
            agent.Target = prey.Position; agent.TargetVelocity = prey.Velocity;
            // Continuous target coordinates do not force decision LOD every frame.
            Crowd.SetAgent(i, agent);
        }
    }

    void UpdateFilterFeeders(float elapsed)
    {
        for (int i = 0; i < m_filters.Length; ++i)
        {
            FilterFeeder feeder = m_filters[i];
            if (!feeder.Enabled) continue;
            if (!IsAlive(feeder.Handle)) { m_filters[i] = default; continue; }
            AiEcologySlot slot = Ecology.Slots[feeder.Handle.Slot];
            if (slot.FleeRemaining > 0) continue;
            AiCrowdAgent agent = Crowd.GetAgent(feeder.Handle.Slot);
            Vector3 mouth = agent.Position + m_rotations[feeder.Handle.Slot] * feeder.MouthOffset;
            float missing = Ecology.Species[slot.SpeciesIndex].MaxEnergy - slot.Energy;
            double requested = Math.Min(missing, (double)feeder.EnergyPerSecond * elapsed);
            if (requested > 0 && Density.TryConsumeSphere(mouth, feeder.MouthRadius, requested, out double actual, 64) && actual > 0)
            {
                float nutrition = (float)actual;
                if ((double)nutrition > actual) nutrition = MathF.BitDecrement(nutrition);
                Ecology.ApplyNutrition(feeder.Handle, nutrition);
            }
            bool found = Density.FindFeedingGoal(agent.Position, feeder.SearchRadius, 64, out Vector3 goal, feeder.MinimumDensity);
            if (found && FitsEnvelope(slot.SpeciesIndex, goal, agent.Floor, agent.Ceiling))
                SetDirective(ref agent, AiCrowdIntent.ARRIVE, goal);
            else SetDirective(ref agent, AiCrowdIntent.WANDER, agent.Position);
            Crowd.SetAgent(feeder.Handle.Slot, agent);
        }
    }

    void UpdateSpecialPredators(float dt)
    {
        for (int i = 0; i < m_predators.Length; ++i)
        {
            Predator predator = m_predators[i];
            if (predator.Tree is null) continue;
            if (!IsAlive(predator.Handle)) { predator.Tree.Dispose(); m_predators[i] = default; continue; }
            predator.Tree.Tick(dt);
            Blackboard board = predator.Tree.Blackboard;
            bool useOverride = board.Get(PredatorPlan.Override);
            if (useOverride || predator.WasOverride)
            {
                AiCrowdAgent agent = Crowd.GetAgent(predator.Handle.Slot);
                if (useOverride)
                {
                    AiCrowdIntent intent = board.Get(PredatorPlan.Intent);
                    Vector3 goal = board.Get(intent == AiCrowdIntent.FLEE ? PredatorPlan.Threat : PredatorPlan.Waypoint);
                    SetDirective(ref agent, intent, goal);
                }
                else SetEcologyDirective(ref agent, Ecology.Movements[predator.Handle.Slot]);
                Crowd.SetAgent(predator.Handle.Slot, agent);
            }
            predator.WasOverride = useOverride;
            m_predators[i] = predator;
        }
    }

    static void SetDirective(ref AiCrowdAgent agent, AiCrowdIntent intent, Vector3 target)
    {
        if (agent.Intent != intent) agent.Flags |= AiCrowdFlags.REQUEST_DECISION;
        agent.Intent = intent; agent.Target = target; agent.Goal = target;
        if (intent == AiCrowdIntent.FLEE) agent.DangerPosition = target;
    }

    void CacheRotations()
    {
        ReadOnlySpan<AiCrowdAgent> snapshot = Crowd.Snapshot;
        for (int i = 0; i < snapshot.Length; ++i)
            if (snapshot[i].Velocity.LengthSquared > Mathf.EPSILON) m_rotations[i] = Quaternion.LookRotation(snapshot[i].Velocity);
    }

    bool FitsEnvelope(int species, Vector3 position, float floor, float ceiling)
    {
        if ((uint)species >= (uint)Ecology.SpeciesCount || !Finite(position) || !float.IsFinite(floor) || !float.IsFinite(ceiling)) return false;
        float radius = Crowd.GetProfile(species).BodyRadius;
        AiCrowdSettings settings = Crowd.Settings;
        return position.X >= settings.Minimum.X + radius && position.X <= settings.Maximum.X - radius
            && position.Z >= settings.Minimum.Z + radius && position.Z <= settings.Maximum.Z - radius
            && position.Y >= MathF.Max(settings.Minimum.Y, floor) + radius
            && position.Y <= MathF.Min(settings.Maximum.Y, ceiling) - radius;
    }

    bool IsAlive(AiEcologyHandle handle) => Ecology.IsValid(handle) && Ecology.Slots[handle.Slot].State == AiEcologySlotState.ALIVE;
    int FindFilterSlot(AiEcologyHandle handle)
    {
        int free = -1;
        for (int i = 0; i < m_filters.Length; ++i)
        {
            if (m_filters[i].Enabled && m_filters[i].Handle == handle) return i;
            if (!m_filters[i].Enabled && free < 0) free = i;
        }
        return free;
    }
    static bool Finite(Vector3 p) => float.IsFinite(p.X) && float.IsFinite(p.Y) && float.IsFinite(p.Z);
    static bool Positive(float value) => float.IsFinite(value) && value > 0;
    void RequireLive() => ObjectDisposedException.ThrowIf(m_disposed, this);
    struct Predator { public AiEcologyHandle Handle; public BehaviorTreeInstance? Tree; public bool WasOverride; }
    struct FilterFeeder
    {
        public AiEcologyHandle Handle; public Vector3 MouthOffset;
        public float MouthRadius, EnergyPerSecond, SearchRadius, MinimumDensity; public bool Enabled;
    }

    static class PredatorPlan
    {
        internal static readonly BlackboardKey<bool> Danger, HasWaypoint, Override;
        internal static readonly BlackboardKey<Vector3> Threat, Waypoint;
        internal static readonly BlackboardKey<AiCrowdIntent> Intent;
        internal static readonly BehaviorTree Tree;
        static PredatorPlan()
        {
            BlackboardSchema.Builder schema = new();
            Danger = schema.Add<bool>("CustomDanger"); HasWaypoint = schema.Add<bool>("HasWaypoint");
            Override = schema.Add<bool>("OverrideEcology"); Threat = schema.Add<Vector3>("Threat");
            Waypoint = schema.Add<Vector3>("Waypoint");
            Intent = schema.Add<AiCrowdIntent>("Intent");
            Tree = new(schema.Build(), Bt.ReactiveSelector(
                Bt.Guard(static context => context.Blackboard.Get(Danger), Command(AiCrowdIntent.FLEE)),
                Bt.Guard(static context => context.Blackboard.Get(HasWaypoint), Command(AiCrowdIntent.SEEK)),
                Bt.Action(static context => { context.Blackboard.Set(Override, false, false); return BtStatus.RUNNING; })));
        }
        static BtNode Command(AiCrowdIntent intent) => Bt.Action(
            context =>
            {
                context.Blackboard.Set(Override, true, false); context.Blackboard.Set(Intent, intent, false);
                return BtStatus.RUNNING;
            }, static (context, _) => context.Blackboard.Set(Override, false, false));
    }
}
