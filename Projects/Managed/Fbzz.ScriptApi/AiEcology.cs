/// @file    AiEcology.cs
/// @brief   Bounded, deterministic ecology decisions over caller-owned positions and explicit diets.
/// @author  Hasegawa Jin
/// @date    2026-10-12
using System;
using System.Text.Json.Serialization;

namespace Fbzz.AI;

public enum AiEcologyTier { PRODUCER, PRIMARY_CONSUMER, SMALL_PREDATOR, APEX_PREDATOR, SCAVENGER }
[Flags] public enum AiEcologyFoodTags : ulong { NONE = 0, PLANKTON = 1, PLANT = 2, GRAZER = 4, PREDATOR = 8, CARRION = 16, CUSTOM_0 = 32, CUSTOM_1 = 64 }
public enum AiEcologySlotState { FREE, ALIVE, DORMANT, CORPSE, PENDING_SPAWN }
public enum AiEcologyIntent { WANDER, CHASE, FLEE, HOLD }
public enum AiEcologyEventKind { HUNT_STARTED, HUNT_FAILED, ESCAPED, CONSUMED, DIED, RESOURCE, SPAWN_REQUESTED, BORN, SPAWN_CANCELLED, DESPAWNED }
public enum AiEcologySiteKind { NUTRIENT, SPAWNING }

public readonly record struct AiEcologyHandle(int Slot, uint Generation)
{
    public static AiEcologyHandle INVALID => new(-1, 0);
}

/// @note Tier describes a role only. Diet, tags, habitat overlap and explicit size limits decide edibility.
public struct AiEcologySpecies
{
    public ulong StableId;
    public AiEcologyTier Tier;
    public AiEcologyFoodTags Tags, Diet;
    public uint HabitatMask;
    public float BodySize, MinPreySize, MaxPreySize, MaxEnergy, InitialEnergy, EnergyDrainPerSecond, FoodEnergy;
    public float HuntRadius, CaptureRadius, SuccessChance, HuntCooldown, EscapeCooldown, TargetTimeout, CorpseTTL;
    public float ReproductionInterval, ResourceUnits;
    public int PopulationFloor, PopulationCap;

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Summary = "Create an editable species; assign unique StableId when roles are repeated.")]
    public static AiEcologySpecies CreateDefault(AiEcologyTier tier, AiEcologyFoodTags tags, AiEcologyFoodTags diet) => new()
    {
        StableId = (ulong)tier + 1, Tier = tier, Tags = tags, Diet = diet, HabitatMask = 1,
        BodySize = 1, MinPreySize = 0, MaxPreySize = 2, MaxEnergy = 24, InitialEnergy = 12,
        EnergyDrainPerSecond = 0.1f, FoodEnergy = 6, HuntRadius = 8, CaptureRadius = 0.4f,
        SuccessChance = 0.65f, HuntCooldown = 2, EscapeCooldown = 3, TargetTimeout = 8,
        CorpseTTL = 12, ReproductionInterval = 30, ResourceUnits = 4, PopulationCap = 128
    };
}

/// @note One capture attempt, including failure or site feeding, spends one MaxCaptures unit.
public struct AiEcologySettings
{
    public float StepInterval, CellSize, MaxResourceRecoveryPerStep;
    public int MaxCaptures, MaxSpawnRequests, CandidateLimit, MaxPendingSpawns, PopulationCap, MaxDecisions;
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public static AiEcologySettings CreateDefault(int capacity = 1024) => new()
    {
        StepInterval = 0.5f, CellSize = 8, MaxCaptures = 32, MaxSpawnRequests = 8,
        CandidateLimit = 64, MaxPendingSpawns = Math.Min(16, capacity), PopulationCap = Math.Min(1024, capacity),
        MaxDecisions = 1000, MaxResourceRecoveryPerStep = 8
    };
}

/// @note All timers are seconds, energy is caller-defined nutrition, and Position is a caller-supplied world position.
public struct AiEcologySlot
{
    public ulong StableId;
    public uint Generation, Seed;
    public int SpeciesIndex, SiteIndex, TargetSiteIndex;
    public AiEcologySlotState State;
    public Vector3 Position;
    public float Energy, Resources, Cooldown, EscapeRemaining, FleeRemaining, TargetAge, CorpseRemaining;
    public AiEcologyHandle Target, Threat;
}

public struct AiEcologyMovement
{
    public AiEcologyIntent Intent;
    public AiEcologyHandle Target;
    public Vector3 TargetPosition;
    public int SiteIndex;
}

/// @note CONSUMED Amount is actual energy added; RESOURCE Amount is remaining source units.
public struct AiEcologyEvent
{
    public AiEcologyEventKind Kind;
    public AiEcologyHandle Actor, Target;
    public int SpeciesIndex, SiteIndex;
    public float Amount;
}

/// @note Nutrient sources are finite; RecoveryRate defaults to zero and positive recovery shares a global step budget.
public struct AiEcologySite
{
    public ulong StableId;
    public AiEcologySiteKind Kind;
    public Vector3 Position;
    public float Radius, Resources, Capacity, RecoveryRate;
    public uint HabitatMask;
    public AiEcologyFoodTags SpeciesTagsMask, FoodTags;
    public int SpawnCapacity, OccupiedLive, OccupiedPending;
}

public struct AiEcologyStats
{
    public int Decisions, CandidateTests, CaptureAttempts, Captures, SpawnRequests, PendingSpawns, Population, Deaths, EventOverflows;
    public float ResourcesConsumed, ResourcesRecovered;
}

/// @note Allocate once and reuse; System.Text.Json saves require IncludeFields=true. Restore validates the complete published state.
public sealed class AiEcologySnapshot
{
    public int Version = 1;
    public int Capacity { get; }
    public int SpeciesCount { get; }
    public AiEcologySpecies[] Species;
    public AiEcologySlot[] Slots;
    public AiEcologyMovement[] Movements;
    public AiEcologySite[] Sites = new AiEcologySite[64];
    public float[] SpawnTimers;
    public AiEcologyEvent[] Events, QueuedEvents;
    public AiEcologySettings Settings;
    public AiEcologyStats Stats;
    public double TimeSeconds;
    public float Accumulator;
    public ulong NextStableId;
    public uint Seed;
    public int DecisionCursor, GridCursor, SpawnCursor, FreeCursor, SiteCursor, RecoveryCursor, SiteCount, EventCount, QueuedEventCount;

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    [JsonConstructor]
    public AiEcologySnapshot(int capacity, int speciesCount)
    {
        if (capacity < 1 || capacity > 4096 || speciesCount < 1 || speciesCount > 64)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        Capacity = capacity; SpeciesCount = speciesCount;
        Species = new AiEcologySpecies[speciesCount]; Slots = new AiEcologySlot[capacity];
        Movements = new AiEcologyMovement[capacity]; SpawnTimers = new float[speciesCount];
        Events = new AiEcologyEvent[capacity * 6 + 80]; QueuedEvents = new AiEcologyEvent[capacity + 64];
    }
}

/// @note Single owner, no callbacks or native calls. Slots and movement outputs are indexed by fixed slot number.
/// @note Step never moves or teleports entities. Pending births reserve capacity until caller CommitSpawn or CancelSpawn.
/// @note Searches inspect at most CandidateLimit hash-list entries; they are bounded local candidates, not global nearest neighbors.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine Data-oriented state and independent representation.
public sealed class AiEcology
{
    readonly AiEcologySpecies[] m_species;
    readonly AiEcologySlot[] m_slots;
    readonly AiEcologyMovement[] m_movements;
    readonly AiEcologySite[] m_sites = new AiEcologySite[64];
    readonly AiEcologyEvent[] m_events, m_queued;
    readonly int[] m_population, m_pending, m_checkPopulation, m_checkPending;
    readonly float[] m_spawnTimers;
    readonly int[] m_heads, m_next, m_cellX, m_cellY, m_cellZ;
    readonly int[] m_checkSiteLive = new int[64], m_checkSitePending = new int[64];
    readonly ulong[] m_checkStableIds;
    AiEcologySettings m_settings;
    AiEcologyStats m_stats;
    int m_siteCount, m_eventCount, m_queuedCount, m_totalPopulation, m_totalPending;
    int m_decisionCursor, m_gridCursor, m_spawnCursor, m_freeCursor, m_siteCursor, m_recoveryCursor;
    float m_accumulator;
    double m_time;
    uint m_seed;
    ulong m_nextStableId = 1;

    public int Capacity => m_slots.Length;
    public int SpeciesCount => m_species.Length;
    public AiEcologySettings Settings => m_settings;
    public AiEcologyStats Stats => m_stats;
    public double TimeSeconds => m_time;
    public ReadOnlySpan<AiEcologySlot> Slots => m_slots;
    public ReadOnlySpan<AiEcologyMovement> Movements => m_movements;
    public ReadOnlySpan<AiEcologyEvent> Events => m_events.AsSpan(0, m_eventCount);
    public ReadOnlySpan<AiEcologySite> Sites => m_sites.AsSpan(0, m_siteCount);
    public ReadOnlySpan<AiEcologySpecies> Species => m_species;

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Lifetime = "caller_owned_managed_object")]
    public AiEcology(int capacity, ReadOnlySpan<AiEcologySpecies> species, uint seed = 1)
    {
        if (capacity < 1 || capacity > 4096 || species.Length < 1 || species.Length > 64)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        m_settings = AiEcologySettings.CreateDefault(capacity);
        for (int i = 0; i < species.Length; ++i) {
            if (!ValidSpecies(species[i], m_settings)) throw new ArgumentException("Invalid ecology species", nameof(species));
            for (int j = 0; j < i; ++j)
                if (species[i].StableId == species[j].StableId) throw new ArgumentException("Duplicate species StableId", nameof(species));
        }
        m_species = species.ToArray(); m_slots = new AiEcologySlot[capacity]; m_movements = new AiEcologyMovement[capacity];
        m_events = new AiEcologyEvent[capacity * 6 + 80]; m_queued = new AiEcologyEvent[capacity + 64];
        m_population = new int[species.Length]; m_pending = new int[species.Length];
        m_checkPopulation = new int[species.Length]; m_checkPending = new int[species.Length];
        m_checkStableIds = new ulong[capacity];
        m_spawnTimers = new float[species.Length];
        int buckets = 1;
        while (buckets < (capacity + 64) * 2) buckets <<= 1;
        m_heads = new int[buckets]; m_next = new int[capacity + 64];
        m_cellX = new int[capacity + 64]; m_cellY = new int[capacity + 64]; m_cellZ = new int[capacity + 64];
        m_seed = seed == 0 ? 1u : seed;
        for (int i = 0; i < capacity; ++i) ClearFree(i);
        for (int i = 0; i < species.Length; ++i) m_spawnTimers[i] = species[i].ReproductionInterval;
    }

    static bool Nonnegative(float x) => float.IsFinite(x) && x >= 0;
    static bool ValidSpecies(in AiEcologySpecies x, in AiEcologySettings settings) =>
        x.StableId != 0 && x.Tier >= 0 && x.Tier <= AiEcologyTier.SCAVENGER && x.HabitatMask != 0 &&
        float.IsFinite(x.BodySize) && x.BodySize > 0 && Nonnegative(x.MinPreySize) && Nonnegative(x.MaxPreySize) && x.MaxPreySize >= x.MinPreySize &&
        float.IsFinite(x.MaxEnergy) && x.MaxEnergy > 0 && Nonnegative(x.InitialEnergy) && x.InitialEnergy <= x.MaxEnergy &&
        Nonnegative(x.EnergyDrainPerSecond) && Nonnegative(x.FoodEnergy) && Nonnegative(x.HuntRadius) && x.HuntRadius <= settings.CellSize &&
        Nonnegative(x.CaptureRadius) && x.CaptureRadius <= x.HuntRadius && Nonnegative(x.SuccessChance) && x.SuccessChance <= 1 &&
        Nonnegative(x.HuntCooldown) && Nonnegative(x.EscapeCooldown) && Nonnegative(x.TargetTimeout) && Nonnegative(x.CorpseTTL) &&
        Nonnegative(x.ReproductionInterval) && Nonnegative(x.ResourceUnits) && x.PopulationFloor >= 0 && x.PopulationCap >= x.PopulationFloor;

    bool ValidSettings(in AiEcologySettings x) => float.IsFinite(x.StepInterval) && x.StepInterval >= 0.5f && x.StepInterval <= 60 &&
        float.IsFinite(x.CellSize) && x.CellSize > 0 && Nonnegative(x.MaxResourceRecoveryPerStep) &&
        x.MaxCaptures >= 0 && x.MaxCaptures <= 4096 && x.MaxSpawnRequests >= 0 && x.MaxSpawnRequests <= 64 &&
        x.CandidateLimit >= 1 && x.CandidateLimit <= 64 && x.MaxPendingSpawns >= 0 && x.MaxPendingSpawns <= Capacity &&
        x.PopulationCap >= 0 && x.PopulationCap <= Capacity && x.MaxDecisions >= 0 && x.MaxDecisions <= 4096;

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "false_without_change_for_invalid_settings_or_reserved_population")]
    public bool TryConfigure(AiEcologySettings settings)
    {
        if (!ValidSettings(settings) || m_totalPopulation + m_totalPending > settings.PopulationCap || m_totalPending > settings.MaxPendingSpawns) return false;
        for (int i = 0; i < SpeciesCount; ++i) if (!ValidSpecies(m_species[i], settings)) return false;
        m_settings = settings; return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public bool TrySetSpecies(int index, AiEcologySpecies species)
    {
        if ((uint)index >= (uint)SpeciesCount || species.StableId != m_species[index].StableId || !ValidSpecies(species, m_settings)
            || m_population[index] + m_pending[index] > species.PopulationCap) return false;
        for (int i = 0; i < Capacity; ++i)
            if (m_slots[i].State != AiEcologySlotState.FREE && m_slots[i].SpeciesIndex == index &&
                (m_slots[i].Energy > species.MaxEnergy || (m_slots[i].SiteIndex >= 0 && !SpawnEligible(m_sites[m_slots[i].SiteIndex], species)))) return false;
        m_species[index] = species; return true;
    }

    static bool ValidSite(in AiEcologySite site) => site.StableId != 0 && site.Kind >= 0 && site.Kind <= AiEcologySiteKind.SPAWNING &&
        site.Position.IsFinite && Nonnegative(site.Radius) && site.HabitatMask != 0 && Nonnegative(site.Resources) &&
        Nonnegative(site.Capacity) && site.Resources <= site.Capacity && Nonnegative(site.RecoveryRate) && site.SpawnCapacity >= 0 && site.SpawnCapacity <= 4096;

    static bool SpawnEligible(in AiEcologySite site, in AiEcologySpecies species) => site.Kind == AiEcologySiteKind.SPAWNING &&
        (site.HabitatMask & species.HabitatMask) != 0 && (site.SpeciesTagsMask & species.Tags) != 0;

    static double DistanceSquared(Vector3 a, Vector3 b)
    {
        double x = (double)a.X - b.X, y = (double)a.Y - b.Y, z = (double)a.Z - b.Z;
        return x * x + y * y + z * z;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "false_without_change_when_invalid_or_site_identity_has_live_reservations_or_unread_events")]
    public bool ConfigureSites(ReadOnlySpan<AiEcologySite> sites)
    {
        if (sites.Length > 64) return false;
        for (int i = 0; i < Capacity; ++i) if (m_slots[i].State != AiEcologySlotState.FREE && m_slots[i].SiteIndex >= 0) return false;
        for (int i = 0; i < sites.Length; ++i) {
            if (!ValidSite(sites[i])) return false;
            for (int j = 0; j < i; ++j) if (sites[i].StableId == sites[j].StableId) return false;
        }
        for (int i = 0; i < m_eventCount; ++i) {
            int site = m_events[i].SiteIndex;
            if (site >= 0 && (site >= sites.Length || sites[site].StableId != m_sites[site].StableId)) return false;
        }
        for (int i = 0; i < m_queuedCount; ++i) {
            int site = m_queued[i].SiteIndex;
            if (site >= 0 && (site >= sites.Length || sites[site].StableId != m_sites[site].StableId)) return false;
        }
        Array.Clear(m_sites); sites.CopyTo(m_sites); m_siteCount = sites.Length;
        for (int i = 0; i < m_siteCount; ++i) { m_sites[i].OccupiedLive = 0; m_sites[i].OccupiedPending = 0; }
        for (int i = 0; i < Capacity; ++i) {
            m_slots[i].TargetSiteIndex = -1;
            if (m_movements[i].SiteIndex >= 0) m_movements[i] = new() {
                Intent = m_slots[i].State == AiEcologySlotState.ALIVE ? AiEcologyIntent.WANDER : AiEcologyIntent.HOLD,
                Target = AiEcologyHandle.INVALID, TargetPosition = m_slots[i].Position, SiteIndex = -1 };
        }
        m_siteCursor = 0; m_recoveryCursor = 0; m_gridCursor %= Capacity + m_siteCount; return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public bool IsValid(AiEcologyHandle handle) => (uint)handle.Slot < (uint)Capacity && handle.Generation != 0 &&
        m_slots[handle.Slot].Generation == handle.Generation && m_slots[handle.Slot].State != AiEcologySlotState.FREE;

    AiEcologyHandle Handle(int slot) => new(slot, m_slots[slot].Generation);
    static uint Next(ref uint seed) { uint x = seed == 0 ? 1u : seed; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return seed = x; }
    static uint GenerationAfter(uint generation) => generation == uint.MaxValue ? 1 : generation + 1;

    void ClearFree(int index)
    {
        uint generation = m_slots[index].Generation;
        m_slots[index] = new() { Generation = generation, SpeciesIndex = -1, SiteIndex = -1, TargetSiteIndex = -1,
            Target = AiEcologyHandle.INVALID, Threat = AiEcologyHandle.INVALID };
        m_movements[index] = new() { Intent = AiEcologyIntent.HOLD, Target = AiEcologyHandle.INVALID, SiteIndex = -1 };
    }

    int FindFree()
    {
        for (int n = 0; n < Capacity; ++n) {
            int i = m_freeCursor; m_freeCursor = (m_freeCursor + 1) % Capacity;
            if (m_slots[i].State == AiEcologySlotState.FREE) return i;
        }
        return -1;
    }

    bool HasPopulationRoom(int species) => (uint)species < (uint)SpeciesCount &&
        m_population[species] + m_pending[species] < m_species[species].PopulationCap && m_totalPopulation + m_totalPending < m_settings.PopulationCap;

    int Reserve(int species, int site, AiEcologySlotState state, Vector3 position)
    {
        if (m_nextStableId == ulong.MaxValue) return -1;
        int i = FindFree(); if (i < 0) return -1;
        uint generation = GenerationAfter(m_slots[i].Generation);
        ulong stable = m_nextStableId++;
        m_slots[i] = new() { StableId = stable, Generation = generation, Seed = Next(ref m_seed), SpeciesIndex = species,
            SiteIndex = site, TargetSiteIndex = -1, State = state, Position = position,
            Energy = m_species[species].InitialEnergy, Resources = m_species[species].Tier == AiEcologyTier.PRODUCER ? m_species[species].ResourceUnits : 0,
            Target = AiEcologyHandle.INVALID, Threat = AiEcologyHandle.INVALID };
        if (state == AiEcologySlotState.PENDING_SPAWN) {
            ++m_pending[species]; ++m_totalPending; if (site >= 0) ++m_sites[site].OccupiedPending;
        } else { ++m_population[species]; ++m_totalPopulation; if (site >= 0) ++m_sites[site].OccupiedLive; }
        RefreshCounts(); return i;
    }

    void RefreshCounts() { m_stats.Population = m_totalPopulation; m_stats.PendingSpawns = m_totalPending; }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "world", Summary = "Register an existing caller-spawned body; never moves a scene entity.")]
    public bool TryAdd(int speciesIndex, Vector3 position, out AiEcologyHandle handle)
    {
        handle = AiEcologyHandle.INVALID;
        if (!position.IsFinite || !HasPopulationRoom(speciesIndex)) return false;
        int i = Reserve(speciesIndex, -1, AiEcologySlotState.ALIVE, position);
        if (i < 0) return false; handle = Handle(i); return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "world", Summary = "Update every slot position once; unused slot positions must also be finite.")]
    public bool UpdatePositions(ReadOnlySpan<Vector3> positions)
    {
        if (positions.Length != Capacity) return false;
        for (int i = 0; i < Capacity; ++i) if (!positions[i].IsFinite) return false;
        for (int i = 0; i < Capacity; ++i) if (m_slots[i].State == AiEcologySlotState.ALIVE || m_slots[i].State == AiEcologySlotState.DORMANT || m_slots[i].State == AiEcologySlotState.CORPSE)
            m_slots[i].Position = positions[i];
        return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "world")]
    public bool CommitSpawn(AiEcologyHandle handle, Vector3 position)
    {
        if (!IsValid(handle) || m_slots[handle.Slot].State != AiEcologySlotState.PENDING_SPAWN || !position.IsFinite) return false;
        ref var slot = ref m_slots[handle.Slot];
        if (slot.SiteIndex >= 0 && (!SpawnEligible(m_sites[slot.SiteIndex], m_species[slot.SpeciesIndex]) ||
            DistanceSquared(position, m_sites[slot.SiteIndex].Position) > (double)m_sites[slot.SiteIndex].Radius * m_sites[slot.SiteIndex].Radius)) return false;
        --m_pending[slot.SpeciesIndex]; --m_totalPending; ++m_population[slot.SpeciesIndex]; ++m_totalPopulation;
        if (slot.SiteIndex >= 0) { --m_sites[slot.SiteIndex].OccupiedPending; ++m_sites[slot.SiteIndex].OccupiedLive; }
        slot.Position = position; slot.State = AiEcologySlotState.ALIVE;
        Queue(AiEcologyEventKind.BORN, handle, AiEcologyHandle.INVALID, slot.SpeciesIndex, slot.SiteIndex); RefreshCounts(); return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public bool CancelSpawn(AiEcologyHandle handle)
    {
        if (!IsValid(handle) || m_slots[handle.Slot].State != AiEcologySlotState.PENDING_SPAWN) return false;
        ref var slot = ref m_slots[handle.Slot];
        Queue(AiEcologyEventKind.SPAWN_CANCELLED, handle, AiEcologyHandle.INVALID, slot.SpeciesIndex, slot.SiteIndex);
        --m_pending[slot.SpeciesIndex]; --m_totalPending;
        if (slot.SiteIndex >= 0) --m_sites[slot.SiteIndex].OccupiedPending;
        ClearFree(handle.Slot); RefreshCounts(); return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public bool SetDormant(AiEcologyHandle handle, bool dormant)
    {
        if (!IsValid(handle)) return false;
        ref var slot = ref m_slots[handle.Slot];
        if (slot.State != AiEcologySlotState.ALIVE && slot.State != AiEcologySlotState.DORMANT) return false;
        slot.State = dormant ? AiEcologySlotState.DORMANT : AiEcologySlotState.ALIVE;
        slot.Target = AiEcologyHandle.INVALID; slot.TargetSiteIndex = -1; return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Summary = "Apply only actually consumed density or external food energy; finite nonnegative inputs, MaxEnergy clamp.")]
    public bool ApplyNutrition(AiEcologyHandle handle, float actualEnergy)
    {
        if (!Nonnegative(actualEnergy) || !IsValid(handle) || m_slots[handle.Slot].State != AiEcologySlotState.ALIVE) return false;
        ref var slot = ref m_slots[handle.Slot]; AddResource(ref slot.Energy, m_species[slot.SpeciesIndex].MaxEnergy, actualEnergy); return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public bool CanConsume(int hunterSpecies, int preySpecies)
    {
        if ((uint)hunterSpecies >= (uint)SpeciesCount || (uint)preySpecies >= (uint)SpeciesCount) return false;
        ref var hunter = ref m_species[hunterSpecies]; ref var prey = ref m_species[preySpecies];
        return (hunter.Diet & prey.Tags) != 0 && (hunter.HabitatMask & prey.HabitatMask) != 0 && prey.BodySize >= hunter.MinPreySize && prey.BodySize <= hunter.MaxPreySize;
    }

    void Emit(AiEcologyEventKind kind, AiEcologyHandle actor, AiEcologyHandle target, int species, int site = -1, float amount = 0)
    {
        if (m_eventCount >= m_events.Length) { ++m_stats.EventOverflows; return; }
        m_events[m_eventCount++] = new() { Kind = kind, Actor = actor, Target = target, SpeciesIndex = species, SiteIndex = site, Amount = amount };
    }
    void Queue(AiEcologyEventKind kind, AiEcologyHandle actor, AiEcologyHandle target, int species, int site)
    {
        if (m_queuedCount >= m_queued.Length) { ++m_stats.EventOverflows; return; }
        m_queued[m_queuedCount++] = new() { Kind = kind, Actor = actor, Target = target, SpeciesIndex = species, SiteIndex = site };
    }

    void Release(int i)
    {
        Emit(AiEcologyEventKind.DESPAWNED, Handle(i), AiEcologyHandle.INVALID, m_slots[i].SpeciesIndex, m_slots[i].SiteIndex);
        ClearFree(i);
    }
    void Die(int i, AiEcologyHandle killer)
    {
        ref var slot = ref m_slots[i]; var old = Handle(i); int species = slot.SpeciesIndex;
        --m_population[species]; --m_totalPopulation;
        if (slot.SiteIndex >= 0) --m_sites[slot.SiteIndex].OccupiedLive;
        Emit(AiEcologyEventKind.DIED, old, killer, species, slot.SiteIndex); ++m_stats.Deaths;
        slot.Generation = GenerationAfter(slot.Generation); slot.State = AiEcologySlotState.CORPSE;
        slot.Target = AiEcologyHandle.INVALID; slot.Threat = AiEcologyHandle.INVALID; slot.TargetSiteIndex = -1; slot.SiteIndex = -1;
        slot.Resources = 1; slot.CorpseRemaining = m_species[species].CorpseTTL;
    }

    int Bucket(int x, int y, int z) => (int)(unchecked((uint)x * 73856093u ^ (uint)y * 19349663u ^ (uint)z * 83492791u) & (uint)(m_heads.Length - 1));
    int Cell(float p) => (int)Math.Clamp(Math.Floor(p / m_settings.CellSize), int.MinValue + 1.0, int.MaxValue - 1.0);
    void InsertGrid(int i, Vector3 p)
    {
        int x = Cell(p.X), y = Cell(p.Y), z = Cell(p.Z); m_cellX[i] = x; m_cellY[i] = y; m_cellZ[i] = z;
        int bucket = Bucket(x, y, z); m_next[i] = m_heads[bucket]; m_heads[bucket] = i;
    }
    void BuildGrid()
    {
        Array.Fill(m_heads, -1);
        /// @note Rotate stable insertion order to avoid permanently hiding later bodies or sites in dense buckets.
        int nodes = Capacity + m_siteCount;
        for (int n = nodes - 1; n >= 0; --n) {
            int i = (m_gridCursor + n) % nodes;
            if (i >= Capacity) {
                int site = i - Capacity;
                if (m_sites[site].Kind == AiEcologySiteKind.NUTRIENT && m_sites[site].Resources > 0) InsertGrid(i, m_sites[site].Position);
            } else if (m_slots[i].State == AiEcologySlotState.ALIVE || m_slots[i].State == AiEcologySlotState.CORPSE) InsertGrid(i, m_slots[i].Position);
        }
        m_gridCursor = (m_gridCursor + m_settings.CandidateLimit) % nodes;
    }

    bool Edible(int hunter, int prey)
    {
        if (hunter == prey) return false;
        ref var actor = ref m_slots[hunter]; ref var target = ref m_slots[prey]; ref var species = ref m_species[actor.SpeciesIndex];
        if (target.State != AiEcologySlotState.ALIVE && target.State != AiEcologySlotState.CORPSE) return false;
        ref var food = ref m_species[target.SpeciesIndex];
        if ((species.HabitatMask & food.HabitatMask) == 0 || food.BodySize < species.MinPreySize || food.BodySize > species.MaxPreySize) return false;
        if (target.State == AiEcologySlotState.CORPSE) return (species.Diet & AiEcologyFoodTags.CARRION) != 0 && target.Resources > 0;
        if ((species.Diet & food.Tags) == 0 || target.EscapeRemaining > 0) return false;
        if (food.Tier == AiEcologyTier.PRODUCER) return target.Resources > 0 && (target.Resources > 1 || m_population[target.SpeciesIndex] > food.PopulationFloor);
        return m_population[target.SpeciesIndex] > food.PopulationFloor;
    }
    bool EdibleSite(int hunter, int site)
    {
        ref var actor = ref m_species[m_slots[hunter].SpeciesIndex]; ref var source = ref m_sites[site];
        return source.Kind == AiEcologySiteKind.NUTRIENT && source.Resources > 0 && (actor.HabitatMask & source.HabitatMask) != 0 &&
            (actor.Diet & source.FoodTags) != 0 && (actor.Tags & source.SpeciesTagsMask) != 0;
    }

    int FindFood(int hunter)
    {
        Vector3 p = m_slots[hunter].Position; int x = Cell(p.X), y = Cell(p.Y), z = Cell(p.Z), examined = 0, best = -1;
        double bestDistance = m_species[m_slots[hunter].SpeciesIndex].HuntRadius; bestDistance *= bestDistance;
        for (int dz = -1; dz <= 1 && examined < m_settings.CandidateLimit; ++dz)
            for (int dy = -1; dy <= 1 && examined < m_settings.CandidateLimit; ++dy)
                for (int dx = -1; dx <= 1 && examined < m_settings.CandidateLimit; ++dx) {
                    int cx = x + dx, cy = y + dy, cz = z + dz;
                    for (int candidate = m_heads[Bucket(cx, cy, cz)]; candidate >= 0 && examined < m_settings.CandidateLimit; candidate = m_next[candidate]) {
                        ++examined; ++m_stats.CandidateTests;
                        if (m_cellX[candidate] != cx || m_cellY[candidate] != cy || m_cellZ[candidate] != cz) continue;
                        bool site = candidate >= Capacity;
                        if (site ? !EdibleSite(hunter, candidate - Capacity) : !Edible(hunter, candidate)) continue;
                        double distance = DistanceSquared(site ? m_sites[candidate - Capacity].Position : m_slots[candidate].Position, p);
                        if (distance < bestDistance || (distance == bestDistance && (best < 0 || candidate < best))) { best = candidate; bestDistance = distance; }
                    }
                }
        return best;
    }

    void FailHunt(int i)
    {
        ref var slot = ref m_slots[i];
        Emit(AiEcologyEventKind.HUNT_FAILED, Handle(i), slot.Target, slot.SpeciesIndex, slot.TargetSiteIndex);
        slot.Target = AiEcologyHandle.INVALID; slot.TargetSiteIndex = -1; slot.TargetAge = 0; slot.Cooldown = m_species[slot.SpeciesIndex].HuntCooldown;
    }

    /// @note Retain any fraction that float storage cannot represent; nutrition uses only the actual source decrease.
    static float TakeResource(ref float resources, float requested)
    {
        float before = resources;
        double goal = Math.Max(0, (double)before - Math.Min(requested, before));
        float after = (float)goal;
        if ((double)after < goal) after = MathF.BitIncrement(after);
        resources = after;
        return (float)((double)before - after);
    }

    /// @note Round toward the existing reservoir so actual growth never exceeds its recovery budget.
    static double AddResource(ref float resources, float capacity, double requested)
    {
        float before = resources;
        double goal = Math.Min(capacity, (double)before + requested);
        float after = (float)goal;
        if ((double)after > goal) after = MathF.BitDecrement(after);
        resources = after;
        return (double)after - before;
    }

    void Decide(int i)
    {
        ref var slot = ref m_slots[i]; if (slot.State != AiEcologySlotState.ALIVE) return;
        ref var species = ref m_species[slot.SpeciesIndex];
        if (slot.Energy >= species.MaxEnergy) {
            if (slot.Target.Slot >= 0 || slot.TargetSiteIndex >= 0) FailHunt(i);
            return;
        }
        if (species.Diet == AiEcologyFoodTags.NONE || slot.Cooldown > 0 || slot.FleeRemaining > 0) return;
        ++m_stats.Decisions;
        bool siteTarget = slot.TargetSiteIndex >= 0;
        bool bodyTarget = slot.Target.Slot >= 0;
        if ((bodyTarget || siteTarget) && ((species.TargetTimeout > 0 && slot.TargetAge > species.TargetTimeout) ||
            (siteTarget ? !EdibleSite(i, slot.TargetSiteIndex) : !IsValid(slot.Target) || !Edible(i, slot.Target.Slot)))) {
            FailHunt(i); return;
        }
        if (!bodyTarget && !siteTarget) {
            int food = FindFood(i); if (food < 0) return;
            if (food >= Capacity) slot.TargetSiteIndex = food - Capacity;
            else { slot.Target = Handle(food); m_slots[food].Threat = Handle(i); m_slots[food].FleeRemaining = species.EscapeCooldown; }
            slot.TargetAge = 0;
            Emit(AiEcologyEventKind.HUNT_STARTED, Handle(i), slot.Target, slot.SpeciesIndex, slot.TargetSiteIndex);
        }
        Vector3 targetPosition = slot.TargetSiteIndex >= 0 ? m_sites[slot.TargetSiteIndex].Position : m_slots[slot.Target.Slot].Position;
        float capture = species.CaptureRadius;
        if (slot.TargetSiteIndex >= 0) capture = MathF.Max(capture, m_sites[slot.TargetSiteIndex].Radius);
        if (DistanceSquared(targetPosition, slot.Position) > (double)capture * capture || m_stats.CaptureAttempts >= m_settings.MaxCaptures) return;
        ++m_stats.CaptureAttempts;
        if (slot.TargetSiteIndex < 0 && Next(ref slot.Seed) / 4294967296.0 >= species.SuccessChance) {
            int prey = slot.Target.Slot;
            if (m_slots[prey].State == AiEcologySlotState.ALIVE) {
                m_slots[prey].EscapeRemaining = species.EscapeCooldown; m_slots[prey].FleeRemaining = species.EscapeCooldown;
                Emit(AiEcologyEventKind.ESCAPED, Handle(prey), Handle(i), m_slots[prey].SpeciesIndex);
            }
            FailHunt(i); return;
        }
        AiEcologyHandle foodHandle = slot.Target; int foodSite = slot.TargetSiteIndex; float units;
        if (foodSite >= 0) {
            units = TakeResource(ref m_sites[foodSite].Resources, 1);
            Emit(AiEcologyEventKind.RESOURCE, Handle(i), AiEcologyHandle.INVALID, slot.SpeciesIndex, foodSite, m_sites[foodSite].Resources);
        } else {
            int prey = foodHandle.Slot; bool producer = m_species[m_slots[prey].SpeciesIndex].Tier == AiEcologyTier.PRODUCER;
            if (m_slots[prey].State == AiEcologySlotState.ALIVE && !producer) Die(prey, Handle(i));
            units = TakeResource(ref m_slots[prey].Resources, 1);
            if (producer) Emit(AiEcologyEventKind.RESOURCE, Handle(prey), Handle(i), m_slots[prey].SpeciesIndex, -1, m_slots[prey].Resources);
            if (m_slots[prey].Resources <= 0) {
                if (m_slots[prey].State == AiEcologySlotState.ALIVE) Die(prey, Handle(i));
                Release(prey);
            }
        }
        if (units <= 0) { FailHunt(i); return; }
        double gained = AddResource(ref slot.Energy, species.MaxEnergy, (double)units * species.FoodEnergy);
        float reported = (float)gained; if ((double)reported > gained) reported = MathF.BitDecrement(reported);
        Emit(AiEcologyEventKind.CONSUMED, Handle(i), foodHandle, slot.SpeciesIndex, foodSite, reported);
        m_stats.ResourcesConsumed += units; ++m_stats.Captures;
        slot.Target = AiEcologyHandle.INVALID; slot.TargetSiteIndex = -1; slot.TargetAge = 0; slot.Cooldown = species.HuntCooldown;
    }

    void Lifecycle(float elapsed)
    {
        for (int i = 0; i < Capacity; ++i) {
            ref var slot = ref m_slots[i];
            slot.Cooldown = MathF.Max(0, slot.Cooldown - elapsed); slot.EscapeRemaining = MathF.Max(0, slot.EscapeRemaining - elapsed);
            slot.FleeRemaining = MathF.Max(0, slot.FleeRemaining - elapsed);
            if (slot.Target.Slot >= 0 || slot.TargetSiteIndex >= 0) slot.TargetAge += elapsed;
            if (slot.State == AiEcologySlotState.CORPSE) { slot.CorpseRemaining -= elapsed; if (slot.CorpseRemaining <= 0) Release(i); continue; }
            if (slot.State != AiEcologySlotState.ALIVE) continue;
            ref var species = ref m_species[slot.SpeciesIndex];
            if (species.Tier == AiEcologyTier.PRODUCER) continue;
            slot.Energy = MathF.Max(0, slot.Energy - species.EnergyDrainPerSecond * elapsed);
            if (slot.Energy <= 0 && m_population[slot.SpeciesIndex] > species.PopulationFloor) {
                Die(i, AiEcologyHandle.INVALID); if (slot.CorpseRemaining <= 0) Release(i);
            }
        }
    }

    void RecoverResources(float elapsed)
    {
        double budget = m_settings.MaxResourceRecoveryPerStep, total = 0;
        for (int n = 0; n < m_siteCount; ++n) {
            int i = (m_recoveryCursor + n) % m_siteCount; ref var site = ref m_sites[i];
            if (site.Kind != AiEcologySiteKind.NUTRIENT) continue;
            double requested = Math.Min(budget, Math.Min((double)site.Capacity - site.Resources, (double)site.RecoveryRate * elapsed));
            if (requested <= 0) continue;
            double actual = AddResource(ref site.Resources, site.Capacity, requested);
            budget -= actual; total += actual;
        }
        m_stats.ResourcesRecovered = (float)total;
        if (m_siteCount > 0) m_recoveryCursor = (m_recoveryCursor + 1) % m_siteCount;
    }

    int SpawnSite(int species)
    {
        bool hasSpawnSite = false;
        for (int n = 0; n < m_siteCount; ++n) {
            int i = (m_siteCursor + n) % m_siteCount; ref var site = ref m_sites[i];
            if (site.Kind != AiEcologySiteKind.SPAWNING) continue; hasSpawnSite = true;
            if (!SpawnEligible(site, m_species[species]) || site.OccupiedLive + site.OccupiedPending >= site.SpawnCapacity) continue;
            m_siteCursor = (i + 1) % m_siteCount; return i;
        }
        return hasSpawnSite ? -2 : -1;
    }

    void RequestBirths(float elapsed)
    {
        for (int i = 0; i < SpeciesCount; ++i) m_spawnTimers[i] = MathF.Max(0, m_spawnTimers[i] - elapsed);
        for (int n = 0; n < SpeciesCount && m_stats.SpawnRequests < m_settings.MaxSpawnRequests && m_totalPending < m_settings.MaxPendingSpawns; ++n) {
            int species = (m_spawnCursor + n) % SpeciesCount;
            bool shortage = m_population[species] + m_pending[species] < m_species[species].PopulationFloor;
            bool ordinary = m_spawnTimers[species] <= 0 && m_species[species].ReproductionInterval > 0 &&
                (m_population[species] > 0 || m_species[species].Tier == AiEcologyTier.PRODUCER);
            if ((!shortage && !ordinary) || !HasPopulationRoom(species)) continue;
            int site = SpawnSite(species); if (site == -2) continue;
            int slot = Reserve(species, site, AiEcologySlotState.PENDING_SPAWN, site < 0 ? Vector3.ZERO : m_sites[site].Position);
            if (slot < 0) break;
            Emit(AiEcologyEventKind.SPAWN_REQUESTED, Handle(slot), AiEcologyHandle.INVALID, species, site); ++m_stats.SpawnRequests;
            m_spawnTimers[species] = m_species[species].ReproductionInterval;
        }
        m_spawnCursor = (m_spawnCursor + 1) % SpeciesCount;
    }

    void WriteMovements()
    {
        for (int i = 0; i < Capacity; ++i) {
            ref var slot = ref m_slots[i];
            AiEcologyMovement movement = new() { Intent = AiEcologyIntent.HOLD, Target = AiEcologyHandle.INVALID, TargetPosition = slot.Position, SiteIndex = -1 };
            if (slot.State == AiEcologySlotState.ALIVE) {
                movement.Intent = AiEcologyIntent.WANDER;
                if (slot.FleeRemaining > 0 && IsValid(slot.Threat) && m_slots[slot.Threat.Slot].State == AiEcologySlotState.ALIVE) {
                    movement.Intent = AiEcologyIntent.FLEE; movement.Target = slot.Threat; movement.TargetPosition = m_slots[slot.Threat.Slot].Position;
                } else if (slot.TargetSiteIndex >= 0 && EdibleSite(i, slot.TargetSiteIndex)) {
                    movement.Intent = AiEcologyIntent.CHASE; movement.SiteIndex = slot.TargetSiteIndex; movement.TargetPosition = m_sites[slot.TargetSiteIndex].Position;
                } else if (IsValid(slot.Target) && Edible(i, slot.Target.Slot)) {
                    movement.Intent = AiEcologyIntent.CHASE; movement.Target = slot.Target; movement.TargetPosition = m_slots[slot.Target.Slot].Position;
                } else if (slot.Target.Slot >= 0 || slot.TargetSiteIndex >= 0) FailHunt(i);
            }
            m_movements[i] = movement;
        }
    }

    /// @note One bounded update uses accumulated real seconds; no catch-up loop. Read Events only when this returns true.
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", ParameterUnits = new[] { "dt=s" }, Failure = "false_for_invalid_dt_or_before_next_decision")]
    public bool Step(float dt)
    {
        if (!float.IsFinite(dt) || dt < 0 || dt > 60) return false;
        m_time += dt; m_accumulator += dt; if (m_accumulator < m_settings.StepInterval) return false;
        float elapsed = m_accumulator; m_accumulator = 0; m_eventCount = 0; m_stats = default;
        m_queued.AsSpan(0, m_queuedCount).CopyTo(m_events); m_eventCount = m_queuedCount; m_queuedCount = 0;
        Lifecycle(elapsed); RecoverResources(elapsed); BuildGrid();
        int decisions = Math.Min(Capacity, m_settings.MaxDecisions);
        for (int n = 0; n < decisions; ++n) { int i = m_decisionCursor; m_decisionCursor = (m_decisionCursor + 1) % Capacity; Decide(i); }
        RequestBirths(elapsed); WriteMovements(); RefreshCounts(); return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Summary = "Copy all persistent state into reusable caller storage without allocating.")]
    public bool CaptureSnapshot(AiEcologySnapshot snapshot)
    {
        if (!ValidSnapshotStorage(snapshot)) return false;
        m_species.CopyTo(snapshot.Species, 0); m_slots.CopyTo(snapshot.Slots, 0); m_movements.CopyTo(snapshot.Movements, 0);
        m_sites.CopyTo(snapshot.Sites, 0); m_spawnTimers.CopyTo(snapshot.SpawnTimers, 0);
        m_events.CopyTo(snapshot.Events, 0); m_queued.CopyTo(snapshot.QueuedEvents, 0);
        snapshot.Version = 1; snapshot.Settings = m_settings; snapshot.Stats = m_stats; snapshot.TimeSeconds = m_time; snapshot.Accumulator = m_accumulator;
        snapshot.NextStableId = m_nextStableId; snapshot.Seed = m_seed; snapshot.DecisionCursor = m_decisionCursor; snapshot.GridCursor = m_gridCursor; snapshot.SpawnCursor = m_spawnCursor;
        snapshot.FreeCursor = m_freeCursor; snapshot.SiteCursor = m_siteCursor; snapshot.RecoveryCursor = m_recoveryCursor;
        snapshot.SiteCount = m_siteCount; snapshot.EventCount = m_eventCount; snapshot.QueuedEventCount = m_queuedCount; return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "false_without_mutation_for_invalid_or_wrong_sized_snapshot")]
    public bool RestoreSnapshot(AiEcologySnapshot snapshot)
    {
        if (!ValidSnapshotStorage(snapshot) || snapshot.Version != 1 || !ValidSettings(snapshot.Settings) ||
            !double.IsFinite(snapshot.TimeSeconds) || snapshot.TimeSeconds < 0 || !Nonnegative(snapshot.Accumulator) || snapshot.Accumulator > 120 ||
            snapshot.NextStableId == 0 || snapshot.Seed == 0 || (uint)snapshot.DecisionCursor >= (uint)Capacity || (uint)snapshot.FreeCursor >= (uint)Capacity ||
            (uint)snapshot.SpawnCursor >= (uint)SpeciesCount || snapshot.SiteCount < 0 || snapshot.SiteCount > 64 ||
            snapshot.EventCount < 0 || snapshot.EventCount > m_events.Length || snapshot.QueuedEventCount < 0 || snapshot.QueuedEventCount > m_queued.Length ||
            (uint)snapshot.GridCursor >= (uint)(Capacity + snapshot.SiteCount) ||
            snapshot.SiteCursor < 0 || snapshot.RecoveryCursor < 0 || (snapshot.SiteCount > 0 && (snapshot.SiteCursor >= snapshot.SiteCount || snapshot.RecoveryCursor >= snapshot.SiteCount))) return false;
        for (int i = 0; i < SpeciesCount; ++i) {
            if (!ValidSpecies(snapshot.Species[i], snapshot.Settings) || !Nonnegative(snapshot.SpawnTimers[i])) return false;
            for (int j = 0; j < i; ++j) if (snapshot.Species[i].StableId == snapshot.Species[j].StableId) return false;
        }
        Array.Clear(m_checkPopulation); Array.Clear(m_checkPending); Array.Clear(m_checkSiteLive); Array.Clear(m_checkSitePending);
        int population = 0, pending = 0, stableCount = 0;
        for (int i = 0; i < snapshot.SiteCount; ++i) {
            if (!ValidSite(snapshot.Sites[i])) return false;
            for (int j = 0; j < i; ++j) if (snapshot.Sites[i].StableId == snapshot.Sites[j].StableId) return false;
        }
        for (int i = 0; i < Capacity; ++i) {
            ref var slot = ref snapshot.Slots[i];
            if (slot.State < 0 || slot.State > AiEcologySlotState.PENDING_SPAWN || !slot.Position.IsFinite || !Nonnegative(slot.Energy) ||
                !Nonnegative(slot.Resources) || !Nonnegative(slot.Cooldown) || !Nonnegative(slot.EscapeRemaining) || !Nonnegative(slot.FleeRemaining) ||
                !Nonnegative(slot.TargetAge) || !Nonnegative(slot.CorpseRemaining) || slot.TargetSiteIndex < -1 || slot.TargetSiteIndex >= snapshot.SiteCount ||
                slot.SiteIndex < -1 || slot.SiteIndex >= snapshot.SiteCount || !ValidStoredHandle(slot.Target) || !ValidStoredHandle(slot.Threat)) return false;
            if (slot.State == AiEcologySlotState.FREE) continue;
            if ((uint)slot.SpeciesIndex >= (uint)SpeciesCount || slot.Generation == 0 || slot.StableId == 0 || slot.StableId >= snapshot.NextStableId ||
                slot.Seed == 0 || slot.Energy > snapshot.Species[slot.SpeciesIndex].MaxEnergy) return false;
            m_checkStableIds[stableCount++] = slot.StableId;
            if (slot.State == AiEcologySlotState.PENDING_SPAWN) { ++m_checkPending[slot.SpeciesIndex]; ++pending; if (slot.SiteIndex >= 0) ++m_checkSitePending[slot.SiteIndex]; }
            if (slot.State == AiEcologySlotState.ALIVE || slot.State == AiEcologySlotState.DORMANT) { ++m_checkPopulation[slot.SpeciesIndex]; ++population; if (slot.SiteIndex >= 0) ++m_checkSiteLive[slot.SiteIndex]; }
            if (slot.SiteIndex >= 0 && !SpawnEligible(snapshot.Sites[slot.SiteIndex], snapshot.Species[slot.SpeciesIndex])) return false;
        }
        Array.Sort(m_checkStableIds, 0, stableCount);
        for (int i = 1; i < stableCount; ++i) if (m_checkStableIds[i] == m_checkStableIds[i - 1]) return false;
        if (population + pending > snapshot.Settings.PopulationCap || pending > snapshot.Settings.MaxPendingSpawns) return false;
        for (int i = 0; i < SpeciesCount; ++i) if (m_checkPopulation[i] + m_checkPending[i] > snapshot.Species[i].PopulationCap) return false;
        for (int i = 0; i < snapshot.SiteCount; ++i) if (m_checkSiteLive[i] + m_checkSitePending[i] > snapshot.Sites[i].SpawnCapacity ||
            m_checkSiteLive[i] != snapshot.Sites[i].OccupiedLive || m_checkSitePending[i] != snapshot.Sites[i].OccupiedPending) return false;
        for (int i = 0; i < Capacity; ++i) {
            ref var movement = ref snapshot.Movements[i];
            if (movement.Intent < 0 || movement.Intent > AiEcologyIntent.HOLD || !movement.TargetPosition.IsFinite ||
                !ValidStoredHandle(movement.Target) || movement.SiteIndex < -1 || movement.SiteIndex >= snapshot.SiteCount) return false;
        }
        for (int i = 0; i < snapshot.EventCount; ++i) if (!ValidEvent(snapshot.Events[i], snapshot.SiteCount, false)) return false;
        for (int i = 0; i < snapshot.QueuedEventCount; ++i) if (!ValidEvent(snapshot.QueuedEvents[i], snapshot.SiteCount, true)) return false;
        ref var stats = ref snapshot.Stats;
        /// @note Counters describe the previous completed step; settings may have changed since that step.
        if (stats.Decisions < 0 || stats.Decisions > Capacity || stats.CandidateTests < 0 ||
            stats.CandidateTests > stats.Decisions * 64 || stats.CaptureAttempts < 0 || stats.CaptureAttempts > 4096 ||
            stats.Captures < 0 || stats.Captures > stats.CaptureAttempts || stats.SpawnRequests < 0 || stats.SpawnRequests > 64 ||
            stats.PendingSpawns != pending || stats.Population != population || stats.Deaths < 0 || stats.Deaths > Capacity || stats.EventOverflows < 0 ||
            !Nonnegative(stats.ResourcesConsumed) || stats.ResourcesConsumed > stats.CaptureAttempts || !Nonnegative(stats.ResourcesRecovered)) return false;
        snapshot.Species.CopyTo(m_species, 0); snapshot.Slots.CopyTo(m_slots, 0); snapshot.Movements.CopyTo(m_movements, 0);
        snapshot.Sites.CopyTo(m_sites, 0); snapshot.SpawnTimers.CopyTo(m_spawnTimers, 0); snapshot.Events.CopyTo(m_events, 0); snapshot.QueuedEvents.CopyTo(m_queued, 0);
        m_checkPopulation.CopyTo(m_population, 0); m_checkPending.CopyTo(m_pending, 0); m_totalPopulation = population; m_totalPending = pending;
        m_settings = snapshot.Settings; m_stats = snapshot.Stats; m_time = snapshot.TimeSeconds; m_accumulator = snapshot.Accumulator;
        m_nextStableId = snapshot.NextStableId; m_seed = snapshot.Seed; m_decisionCursor = snapshot.DecisionCursor; m_gridCursor = snapshot.GridCursor; m_spawnCursor = snapshot.SpawnCursor;
        m_freeCursor = snapshot.FreeCursor; m_siteCursor = snapshot.SiteCursor; m_recoveryCursor = snapshot.RecoveryCursor; m_siteCount = snapshot.SiteCount;
        m_eventCount = snapshot.EventCount; m_queuedCount = snapshot.QueuedEventCount; RefreshCounts(); return true;
    }

    bool ValidStoredHandle(AiEcologyHandle handle) => handle.Slot == -1 ? handle.Generation == 0 : (uint)handle.Slot < (uint)Capacity && handle.Generation != 0;

    bool ValidSnapshotStorage(AiEcologySnapshot? snapshot) => snapshot != null && snapshot.Capacity == Capacity && snapshot.SpeciesCount == SpeciesCount &&
        snapshot.Slots != null && snapshot.Slots.Length == Capacity && snapshot.Species != null && snapshot.Species.Length == SpeciesCount &&
        snapshot.Movements != null && snapshot.Movements.Length == Capacity && snapshot.Sites != null && snapshot.Sites.Length == 64 &&
        snapshot.SpawnTimers != null && snapshot.SpawnTimers.Length == SpeciesCount && snapshot.Events != null && snapshot.Events.Length == m_events.Length &&
        snapshot.QueuedEvents != null && snapshot.QueuedEvents.Length == m_queued.Length;

    bool ValidEvent(in AiEcologyEvent item, int siteCount, bool queued) => item.Kind >= 0 && item.Kind <= AiEcologyEventKind.DESPAWNED &&
        (!queued || ((item.Kind == AiEcologyEventKind.BORN || item.Kind == AiEcologyEventKind.SPAWN_CANCELLED) && item.Amount == 0)) &&
        ValidStoredHandle(item.Actor) && item.Actor.Slot >= 0 && ValidStoredHandle(item.Target) && (uint)item.SpeciesIndex < (uint)SpeciesCount &&
        item.SiteIndex >= -1 && item.SiteIndex < siteCount && Nonnegative(item.Amount);
}
