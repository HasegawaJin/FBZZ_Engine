/// @file    Fixtures.cs
/// @brief   保存規則・private lifecycle・世代交換を検証するゲーム側 fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using Fbzz;

namespace ManagedFixtures;

class LifecycleComponent : Script
{
#if FIXTURE_VERSION_TWO
    public float speed = 8f;
#else
    public float speed = 4f;
#endif
    [Setting]
    float privateSpeed = 2f;
    public int awakeCount;
    public int startCount;
    public int updateCount;
    public int destroyCount;
    float runtimeElapsed;
    public float PrivateSpeed => privateSpeed;
    public float RuntimeElapsed => runtimeElapsed;

    protected override void Awake() { awakeCount++; }
    protected override void Start() { startCount++; }
    protected override void Update()
    {
        updateCount++;
        runtimeElapsed += Time.DeltaSeconds;
        Transform.TranslateLocal(new Vector3(speed * Time.DeltaSeconds, 0f, 0f));
    }
    protected override void OnDestroy() { destroyCount++; }
}

static class PlainMovementHelper
{
    /// @param delta ローカル空間の移動量 [m]。
    public static void Move(GameObject actor, Vector3 delta)
    {
        actor.Transform.TranslateLocal(delta);
    }
}

class PlainHelperComponent : Script
{
    public GameObject target = null;
    public float speed = 4f;

    protected override void Update()
    {
        if (target is null || !target.IsAlive)
            return;

        float distance = speed * Time.DeltaSeconds;
        PlainMovementHelper.Move(target, new Vector3(distance, 0f, 0f));
    }
}

class ThrowingCallbackComponent : Script
{
    public int updateCount;
    public int deserializeCount;
    protected override void OnAfterDeserialize() { deserializeCount++; }
    protected override void Update()
    {
        updateCount++;
        throw new InvalidOperationException("Intentional callback failure.");
    }
}

class SettingsComponent : Script
{
    public float speed = 4f;
    public int count = 3;
    public bool active = true;
    public string label = "fixture";
    public Vector3 offset = new Vector3(1f, 2f, 3f);
    [Setting(Label = "Hidden speed", Unit = "m/s", Visible = false)]
    [ValueRange(0f, 10f)]
    [FieldKey("stableSpeed")]
    [PreviousFieldKey("oldSpeed")]
    float privateSpeed = 2f;
    [NonSerialized]
    public float transient = 9f;
    float runtimeElapsed = 1f;
    public readonly int readOnly = 10;
    public static int sharedState = 0;
    public float PrivateSpeed => privateSpeed;
    public float RuntimeElapsed => runtimeElapsed;
    public float GetterTrap => throw new InvalidOperationException("Catalog must not evaluate property getters.");
}

abstract class BaseCallbackComponent : Script
{
    [NonSerialized]
    public int baseCalls;
    [Setting]
    float baseSpeed = 3f;
    public float BaseSpeed => baseSpeed;
    protected override void Update() { baseCalls++; }
}

class InheritedCallbackComponent : BaseCallbackComponent
{
    [NonSerialized]
    public int derivedCalls;
    protected override void Update() { derivedCalls++; }
}

class InheritedOnlyCallbackComponent : BaseCallbackComponent { }

class PrivateConstructorComponent : Script
{
    public static int constructorCalls;
    public int value = 7;
    PrivateConstructorComponent() { constructorCalls++; }
}

class HealthComponent : Script
{
    public int health = 10;
    public void TakeDamage(float damage)
    {
        if (float.IsFinite(damage) && damage > 0f) health = Math.Max(0, health - (int)damage);
    }
    public void Move(Vector3 delta) { Transform.TranslateLocal(delta); }
}

enum PatrolMode { IDLE, ACTIVE }

class ReferenceComponent : Script
{
    public GameObject target = null!;
    public GameObject[] targets = [];
    [ValueRange(0f, 10f)]
    public float[] weights = [1f, 2f];
    public int[] counts = [1, 2];
    public bool[] toggles = [true, false];
    public string[] labels = ["first", "second"];
    public Vector3[] offsets = [Vector3.ZERO];
    public PatrolMode[] modes = [PatrolMode.IDLE];
    public bool cachedTargetAlive;
    public bool cachedTransformAlive;
    public bool cachedHealthAlive;
    public bool moveCached = false;
    GameObject m_cachedTarget;
    Transform m_cachedTransform;
    HealthComponent m_cachedHealth;

    protected override void Awake()
    {
        if (target is null) return;
        m_cachedTarget = target;
        m_cachedTransform = target.Transform;
        m_cachedHealth = target.GetComponent<HealthComponent>();
    }
    protected override void Update()
    {
        cachedTargetAlive = m_cachedTarget is not null && m_cachedTarget.IsAlive;
        cachedTransformAlive = m_cachedTransform is not null && m_cachedTransform.IsAlive;
        cachedHealthAlive = m_cachedHealth is not null && m_cachedHealth.IsAlive;
        if (moveCached && m_cachedTransform is not null) m_cachedTransform.TranslateLocal(new Vector3(1f, 0f, 0f));
        if (target is null || !target.IsAlive) return;
        target.Transform.TranslateLocal(new Vector3(1f, 0f, 0f));
        HealthComponent health = target.GetComponent<HealthComponent>();
        if (health is not null) health.TakeDamage(3f);
    }
}

class RequiredComponent : Script
{
    [RequiredReference]
    public GameObject target = null!;
    public int updateCount;
    protected override void Update() { updateCount++; }
}

class RequiredArrayComponent : Script
{
    [RequiredReference]
    public GameObject[] targets = [];
}

class HierarchyQueryComponent : Script
{
    public GameObject root = null!;
    public string[] segments = [];
    public GameObject found = null!;
    public bool succeeded = false;
    protected override void Update()
    {
        found = null!;
        succeeded = root is not null && root.TryFindChild(segments, out found);
    }
}

public abstract class InheritedSelectedGizmoBaseComponent : Script
{
    protected override void DrawGizmosSelected() { Gizmos.Sphere(Vector3.ZERO, 3); }
}

[ExecuteAlways]
public sealed class InheritedSelectedGizmoComponent : InheritedSelectedGizmoBaseComponent { }

public sealed class GizmoReloadMaskComponent : Script
{
#if FIXTURE_VERSION_TWO
    protected override void DrawGizmosSelected() { Gizmos.Sphere(Vector3.ZERO, 2); }
#else
    protected override void DrawGizmos() { Gizmos.Sphere(Vector3.ZERO, 1); }
#endif
}

/// @note 世代交換で更新の有無だけが変わる。故障と後始末は不在 callback と独立に検証する。
public sealed class LifecyclePresenceComponent : Script
{
    public int awakeCount;
    public int updateCount;
    public int fixedCount;
    public int lateCount;
    public int disableCount;
    public int destroyCount;
    public bool failAwake;
    protected override void Awake()
    {
        awakeCount++;
        if (failAwake) throw new InvalidOperationException("Lifecycle presence fixture failure.");
    }
#if FIXTURE_VERSION_TWO
    protected override void FixedUpdate() { fixedCount++; }
    protected override void LateUpdate() { lateCount++; }
#else
    protected override void Update() { updateCount++; }
#endif
    protected override void OnDisable() { disableCount++; }
    protected override void OnDestroy() { destroyCount++; }
}
