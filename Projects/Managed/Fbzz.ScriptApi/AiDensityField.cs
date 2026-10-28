/// @file    AiDensityField.cs
/// @brief   Bounded 3D energy density, conservative flow and single-owner feeding reservoirs.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;

namespace Fbzz.AI;

/// @note Caller stores this header and the matching double density array together; schema 1 uses world meters and energy/m³.
public struct AiDensitySnapshot
{
    public uint Version;
    public Vector3 Minimum, Maximum;
    public int CellsX, CellsY, CellsZ;
    public float Capacity;
    public double Time;
    public ulong Seed;
}

/// @note Pure managed, single-owner sequential service. Rendering particles are consumers of density, never its storage.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine Separating data-oriented simulation from representation.
public sealed class AiDensityField
{
    public const int MAX_CELLS_X = 32, MAX_CELLS_Y = 16, MAX_CELLS_Z = 32;
    public const int MAX_CELLS = 16384, MAX_QUERY_CELLS = 1024;
    public const uint SNAPSHOT_VERSION = 1;
    readonly Vector3 m_minimum, m_maximum;
    readonly int m_cellsX, m_cellsY, m_cellsZ, m_plane;
    readonly float m_capacity;
    readonly double m_dx, m_dy, m_dz, m_cellVolume;
    readonly double[] m_density, m_growth, m_flow;
    readonly int[] m_heap, m_seen, m_consumptionCells;
    readonly double[] m_heapDistance, m_consumptionAfter;
    int m_heapCount, m_queryEpoch;
    double m_time;
    ulong m_seed;

    /// @note Grid bounds and capacity are immutable; seed is deterministic snapshot provenance. Step and feeding consume no random stream.
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, Coordinate = "world",
        ParameterUnits = new[] { "minimum=m", "maximum=m", "capacity=energy_per_m3", "initialDensity=energy_per_m3" },
        Failure = "invalid_bounds_grid_capacity_or_initial_density_throws")]
    public AiDensityField(Vector3 minimum, Vector3 maximum, int cellsX, int cellsY, int cellsZ,
        float capacity, float initialDensity = 0, ulong seed = 1)
    {
        if (!minimum.IsFinite || !maximum.IsFinite || maximum.X <= minimum.X || maximum.Y <= minimum.Y || maximum.Z <= minimum.Z
            || cellsX < 1 || cellsX > MAX_CELLS_X || cellsY < 1 || cellsY > MAX_CELLS_Y || cellsZ < 1 || cellsZ > MAX_CELLS_Z
            || !float.IsFinite(capacity) || capacity <= 0 || !float.IsFinite(initialDensity) || initialDensity < 0 || initialDensity > capacity)
            throw new ArgumentOutOfRangeException(nameof(capacity), "Density requires finite ordered bounds, a bounded positive grid and 0 <= initialDensity <= capacity.");
        m_minimum = minimum; m_maximum = maximum; m_cellsX = cellsX; m_cellsY = cellsY; m_cellsZ = cellsZ;
        m_capacity = capacity; m_seed = seed; m_plane = cellsX * cellsY;
        m_dx = ((double)maximum.X - minimum.X) / cellsX;
        m_dy = ((double)maximum.Y - minimum.Y) / cellsY;
        m_dz = ((double)maximum.Z - minimum.Z) / cellsZ;
        m_cellVolume = m_dx * m_dy * m_dz;
        int count = m_plane * cellsZ;
        m_density = new double[count]; m_growth = new double[count]; m_flow = new double[count];
        m_heap = new int[count]; m_heapDistance = new double[count]; m_seen = new int[count];
        m_consumptionCells = new int[MAX_QUERY_CELLS]; m_consumptionAfter = new double[MAX_QUERY_CELLS];
        Array.Fill(m_density, initialDensity);
    }

    public int CellCount => m_density.Length;
    public float Capacity => m_capacity;
    public double Time => m_time;
    public ulong Seed => m_seed;
    public Vector3 Minimum => m_minimum;
    public Vector3 Maximum => m_maximum;
    public int CellsX => m_cellsX;
    public int CellsY => m_cellsY;
    public int CellsZ => m_cellsZ;
    public int LastQueryCellCount { get; private set; }

    /// @note Diagnostic O(CellCount) integral; use less often than individual density reads.
    [ApiContract(Read = "any", Purity = ApiPurity.MANAGED, Units = "energy")]
    public double TotalEnergy
    {
        get {
            double sum = 0, compensation = 0;
            foreach (double density in m_density) {
                double term = density * m_cellVolume - compensation;
                double next = sum + term;
                compensation = (next - sum) - term;
                sum = next;
            }
            return sum;
        }
    }

    /// @note Sample is piecewise constant per cell. Outside bounds or a nonfinite position returns zero.
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, Coordinate = "world", Units = "energy_per_m3")]
    public float ReadDensity(Vector3 position) => TryCell(position, out int cell) ? (float)m_density[cell] : 0;

    /// @return Invalid position/density leaves the field unchanged and returns false.
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, Coordinate = "world", ParameterUnits = new[] { "density=energy_per_m3" })]
    public bool TrySetDensity(Vector3 position, float density)
    {
        if (!float.IsFinite(density) || density < 0 || density > m_capacity || !TryCell(position, out int cell)) return false;
        m_density[cell] = density;
        return true;
    }

    /// @note One low-frequency step is limited to 0.5 seconds. Growth is logistic; recovery replenishes available capacity.
    /// @note Flow uses original per-step donors, no-flux boundaries, capacity-limited flux and sum(abs(velocity)*dt/cellWidth) &lt;= 1.
    /// @see https://www.clawpack.org/riemann_book/html/Advection.html Conservation of mass and advection flux; FBZZ additionally bounds explicit transport fractions.
    /// @return Invalid inputs leave density and time unchanged. Excessive flow speed is CFL-clamped, retaining direction.
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED,
        ParameterUnits = new[] { "deltaSeconds=s", "growthPerSecond=1_per_s", "recoveryPerSecond=1_per_s", "flowVelocity=m_per_s" })]
    public bool Step(float deltaSeconds, float growthPerSecond, float recoveryPerSecond, Vector3 flowVelocity)
    {
        double nextTime = m_time + deltaSeconds;
        if (!float.IsFinite(deltaSeconds) || deltaSeconds < 0 || deltaSeconds > 0.5f || !double.IsFinite(nextTime)
            || !float.IsFinite(growthPerSecond) || growthPerSecond < 0 || !float.IsFinite(recoveryPerSecond) || recoveryPerSecond < 0
            || !flowVelocity.IsFinite) return false;
        if (deltaSeconds == 0) return true;
        double growth = (double)growthPerSecond * deltaSeconds, recovery = (double)recoveryPerSecond * deltaSeconds;
        for (int cell = 0; cell < CellCount; cell++) {
            double density = m_density[cell];
            double next = Math.Min(m_capacity, density + growth * density * (1 - density / m_capacity) + recovery * (m_capacity - density));
            m_growth[cell] = next; m_flow[cell] = next;
        }
        double fx = Math.Abs((double)flowVelocity.X) * deltaSeconds / m_dx;
        double fy = Math.Abs((double)flowVelocity.Y) * deltaSeconds / m_dy;
        double fz = Math.Abs((double)flowVelocity.Z) * deltaSeconds / m_dz;
        double cfl = fx + fy + fz;
        if (cfl > 1) { fx /= cfl; fy /= cfl; fz = Math.Min(fz / cfl, Math.Max(0, 1 - fx - fy)); }
        int sx = flowVelocity.X >= 0 ? 1 : -1, sy = flowVelocity.Y >= 0 ? 1 : -1, sz = flowVelocity.Z >= 0 ? 1 : -1;
        for (int z = 0; z < m_cellsZ; z++)
            for (int y = 0; y < m_cellsY; y++)
                for (int x = 0; x < m_cellsX; x++) {
                    int cell = x + m_cellsX * y + m_plane * z;
                    if (fx > 0 && x + sx >= 0 && x + sx < m_cellsX) Transfer(cell, cell + sx, fx);
                    if (fy > 0 && y + sy >= 0 && y + sy < m_cellsY) Transfer(cell, cell + sy * m_cellsX, fy);
                    if (fz > 0 && z + sz >= 0 && z + sz < m_cellsZ) Transfer(cell, cell + sz * m_plane, fz);
                }
        m_flow.AsSpan().CopyTo(m_density);
        m_time = nextTime;
        return true;
    }

    void Transfer(int source, int target, double fraction)
    {
        double amount = Math.Min(m_growth[source] * fraction, Math.Min(m_flow[source], m_capacity - m_flow[target]));
        if (amount <= 0) return;
        m_flow[source] = Math.Max(0, m_flow[source] - amount);
        m_flow[target] = Math.Min(m_capacity, m_flow[target] + amount);
    }

    /// @note Tests at most maxCells nearest sphere-intersecting cells; a budget can hide a farther qualifying reservoir.
    /// @note Goal is the selected cell center, which can lie up to half a cell diagonal beyond the query radius.
    /// @return False for invalid arguments or no density above minimumDensity within the visited budget; field density is unchanged.
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, Coordinate = "world",
        ParameterUnits = new[] { "radius=m", "minimumDensity=energy_per_m3" })]
    public bool FindFeedingGoal(Vector3 position, float radius, int maxCells, out Vector3 goal, float minimumDensity = 0)
    {
        goal = default;
        if (!ValidQuery(position, radius, maxCells) || !float.IsFinite(minimumDensity) || minimumDensity < 0 || minimumDensity > m_capacity) return false;
        BeginQuery(position);
        double squared = (double)radius * radius;
        while (LastQueryCellCount < maxCells && PopQuery(position, squared, out int cell)) {
            if (m_density[cell] > minimumDensity) { goal = CellCenter(cell); return true; }
        }
        return false;
    }

    /// @note Energy is the actual reservoir reduction integrated over cell volume; insufficient density succeeds with less or zero energy.
    /// @note Sphere intersection selects coarse whole-cell reservoirs, rather than counting or destroying visual particles.
    /// @note Requested energy below a reservoir's floating-point resolution consumes zero; no unmeasured depletion is published.
    /// @return Invalid inputs atomically return false and zero. Success is sequentially visible to the next feeding call.
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, Coordinate = "world",
        ParameterUnits = new[] { "radius=m", "requestedEnergy=energy", "actualEnergy=energy" })]
    public bool TryConsumeSphere(Vector3 center, float radius, double requestedEnergy, out double actualEnergy, int maxCells = 256)
    {
        actualEnergy = 0;
        if (!ValidQuery(center, radius, maxCells) || !double.IsFinite(requestedEnergy) || requestedEnergy < 0) return false;
        if (radius == 0 || requestedEnergy == 0) { LastQueryCellCount = 0; return true; }
        BeginQuery(center);
        int staged = 0;
        double squared = (double)radius * radius;
        while (LastQueryCellCount < maxCells && actualEnergy < requestedEnergy && PopQuery(center, squared, out int cell)) {
            double density = m_density[cell], remaining = requestedEnergy - actualEnergy;
            double amount = Math.Min(density * m_cellVolume, remaining);
            double after = Math.Max(0, density - amount / m_cellVolume);
            double removed = (density - after) * m_cellVolume;
            /// @note Round toward retaining mass when one density ULP would exceed the remaining request.
            for (int adjustment = 0; removed > remaining && adjustment < 4; adjustment++) {
                after = Math.BitIncrement(after);
                removed = (density - after) * m_cellVolume;
            }
            if (removed <= 0 || removed > remaining || after < 0 || after > density) continue;
            m_consumptionCells[staged] = cell; m_consumptionAfter[staged] = after; staged++;
            actualEnergy += removed;
        }
        for (int index = 0; index < staged; index++) m_density[m_consumptionCells[index]] = m_consumptionAfter[index];
        return true;
    }

    /// @note Store the header and first CellCount density doubles together; caller tail is untouched.
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, Failure = "insufficient_storage_returns_false_without_writes")]
    public bool TrySave(Span<double> densities, out AiDensitySnapshot snapshot)
    {
        snapshot = default;
        if (densities.Length < CellCount) return false;
        m_density.AsSpan().CopyTo(densities);
        snapshot = new() { Version = SNAPSHOT_VERSION, Minimum = m_minimum, Maximum = m_maximum,
            CellsX = m_cellsX, CellsY = m_cellsY, CellsZ = m_cellsZ, Capacity = m_capacity, Time = m_time, Seed = m_seed };
        return true;
    }

    /// @note Construct matching geometry/capacity before restoring; this copies data and retains no caller array references.
    /// @return Version, geometry, time and all density values are validated before any cell or metadata changes.
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, Failure = "invalid_snapshot_returns_false_without_field_changes")]
    public bool TryRestore(in AiDensitySnapshot snapshot, ReadOnlySpan<double> densities)
    {
        if (snapshot.Version != SNAPSHOT_VERSION || snapshot.CellsX != m_cellsX || snapshot.CellsY != m_cellsY || snapshot.CellsZ != m_cellsZ
            || !snapshot.Minimum.Equals(m_minimum) || !snapshot.Maximum.Equals(m_maximum) || snapshot.Capacity != m_capacity
            || !double.IsFinite(snapshot.Time) || snapshot.Time < 0 || densities.Length < CellCount) return false;
        for (int cell = 0; cell < CellCount; cell++)
            if (!double.IsFinite(densities[cell]) || densities[cell] < 0 || densities[cell] > m_capacity) return false;
        densities[..CellCount].CopyTo(m_density);
        m_time = snapshot.Time; m_seed = snapshot.Seed;
        LastQueryCellCount = 0;
        return true;
    }

    bool TryCell(Vector3 position, out int cell)
    {
        cell = 0;
        if (!position.IsFinite || position.X < m_minimum.X || position.Y < m_minimum.Y || position.Z < m_minimum.Z
            || position.X > m_maximum.X || position.Y > m_maximum.Y || position.Z > m_maximum.Z) return false;
        cell = ClosestCell(position);
        return true;
    }
    static int AxisCell(float position, float minimum, double width, int count)
    {
        double value = ((double)position - minimum) / width;
        return value <= 0 ? 0 : value >= count ? count - 1 : (int)value;
    }
    int ClosestCell(Vector3 position) => AxisCell(position.X, m_minimum.X, m_dx, m_cellsX)
        + m_cellsX * AxisCell(position.Y, m_minimum.Y, m_dy, m_cellsY)
        + m_plane * AxisCell(position.Z, m_minimum.Z, m_dz, m_cellsZ);
    static bool ValidQuery(Vector3 center, float radius, int maxCells) => center.IsFinite && float.IsFinite(radius)
        && radius >= 0 && maxCells >= 1 && maxCells <= MAX_QUERY_CELLS;

    void BeginQuery(Vector3 position)
    {
        LastQueryCellCount = 0; m_heapCount = 0;
        if (m_queryEpoch == int.MaxValue) { Array.Clear(m_seen); m_queryEpoch = 0; }
        m_queryEpoch++;
        Enqueue(ClosestCell(position), position);
    }
    bool PopQuery(Vector3 position, double radiusSquared, out int cell)
    {
        cell = 0;
        if (m_heapCount == 0 || m_heapDistance[0] > radiusSquared) return false;
        cell = m_heap[0];
        int last = --m_heapCount;
        if (last > 0) {
            int nextCell = m_heap[last]; double nextDistance = m_heapDistance[last]; int hole = 0;
            while (hole * 2 + 1 < last) {
                int child = hole * 2 + 1;
                if (child + 1 < last && Before(m_heapDistance[child + 1], m_heap[child + 1], m_heapDistance[child], m_heap[child])) child++;
                if (!Before(m_heapDistance[child], m_heap[child], nextDistance, nextCell)) break;
                m_heap[hole] = m_heap[child]; m_heapDistance[hole] = m_heapDistance[child]; hole = child;
            }
            m_heap[hole] = nextCell; m_heapDistance[hole] = nextDistance;
        }
        LastQueryCellCount++;
        int x = cell % m_cellsX, y = cell / m_cellsX % m_cellsY, z = cell / m_plane;
        if (x > 0) Enqueue(cell - 1, position);
        if (x + 1 < m_cellsX) Enqueue(cell + 1, position);
        if (y > 0) Enqueue(cell - m_cellsX, position);
        if (y + 1 < m_cellsY) Enqueue(cell + m_cellsX, position);
        if (z > 0) Enqueue(cell - m_plane, position);
        if (z + 1 < m_cellsZ) Enqueue(cell + m_plane, position);
        return true;
    }
    static bool Before(double aDistance, int aCell, double bDistance, int bCell) => aDistance < bDistance || aDistance == bDistance && aCell < bCell;
    void Enqueue(int cell, Vector3 position)
    {
        if (m_seen[cell] == m_queryEpoch) return;
        m_seen[cell] = m_queryEpoch;
        double distance = CellDistanceSquared(cell, position);
        int hole = m_heapCount++;
        while (hole > 0) {
            int parent = (hole - 1) / 2;
            if (!Before(distance, cell, m_heapDistance[parent], m_heap[parent])) break;
            m_heap[hole] = m_heap[parent]; m_heapDistance[hole] = m_heapDistance[parent]; hole = parent;
        }
        m_heap[hole] = cell; m_heapDistance[hole] = distance;
    }
    double CellDistanceSquared(int cell, Vector3 position)
    {
        int x = cell % m_cellsX, y = cell / m_cellsX % m_cellsY, z = cell / m_plane;
        double dx = AxisDistance(position.X, m_minimum.X + x * m_dx, m_dx);
        double dy = AxisDistance(position.Y, m_minimum.Y + y * m_dy, m_dy);
        double dz = AxisDistance(position.Z, m_minimum.Z + z * m_dz, m_dz);
        return dx * dx + dy * dy + dz * dz;
    }
    static double AxisDistance(double position, double minimum, double width) => position < minimum ? minimum - position : position > minimum + width ? position - minimum - width : 0;
    Vector3 CellCenter(int cell) => new((float)(m_minimum.X + (cell % m_cellsX + 0.5) * m_dx),
        (float)(m_minimum.Y + (cell / m_cellsX % m_cellsY + 0.5) * m_dy), (float)(m_minimum.Z + (cell / m_plane + 0.5) * m_dz));
}
