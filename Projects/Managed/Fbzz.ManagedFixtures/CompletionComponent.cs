/// @file    CompletionComponent.cs
/// @brief   複合設定・共有アセット・Play 再読み込み・所有者付き jobs の受入 fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using Fbzz;

namespace ManagedFixtures;

public abstract class RequiredPeerBase : Script { }
public sealed class RequiredPeerComponent : RequiredPeerBase { }

[RequireComponent(typeof(Camera))]
[ExecutionOrder(-20)]
public abstract class RequiredAuthoringBase : Script { }

[RequireComponent(typeof(RequiredPeerBase))]
public sealed class RequiredAuthoringComponent : RequiredAuthoringBase
{
    public int awakes;
    public int starts;
    public int updates;
    protected override void Awake() { awakes++; }
    protected override void Start() { starts++; }
    protected override void Update() { updates++; }
}

[ExecutionOrder(-30)]
public sealed class RequirementRemovalComponent : Script
{
    public int command;
    protected override void Update()
    {
        if (command == 1) GameObject.RemoveComponent<RequiredPeerComponent>();
        else if (command == 2) GameObject.RemoveComponent<Camera>();
        command = 0;
    }
}

[ExecutionOrder(-10)]
public sealed class OrderedEarlyComponent : Script
{
    protected override void Update() { Transform.TranslateLocal(new Vector3(1, 0, 0)); }
}

[ExecutionOrder(10)]
public sealed class OrderedLateComponent : Script
{
    protected override void Update() { Transform.LocalPosition = Transform.LocalPosition * 10f; }
}

public sealed class StaticDomainProbeComponent : Script
{
    static int s_updates;
    public int observedUpdates;
    public int reloadCallbacks;
    protected override void Update() { observedUpdates = ++s_updates; }
    protected override void AfterReload() { reloadCallbacks++; }
}

[Serializable]
public sealed class CompletionSettings
{
    public string label = "default";
    public List<float> weights = [1f, 2f];
    public GameObject target;
}

public sealed class WeaponData : DataAsset
{
    public int damage = 5;
    public Quaternion rotation = Quaternion.IDENTITY;
    public List<float> weights = [2f, 3f];
}

public abstract class CompletionCacheBase : Script
{
    GameObject shadowedCache;
    protected GameObject CaptureBaseCache() { shadowedCache = GameObject; return shadowedCache; }
}

public sealed class CompletionComponent : CompletionCacheBase
{
    public int actionCalls;
    public int actionValue = 2;
    public int actionCoupled = 4;
    public bool failAction;
    [EditorAction("double-value", "値を倍にする")]
    void DoubleValue()
    {
        actionCalls++;
        actionValue *= 2;
        actionCoupled = actionValue + 1;
        if (failAction) throw new InvalidOperationException("Intentional editor action failure.");
    }
    [FieldKey("array/~values")] public int[] observationArray = [];
    public int shadowedCache = 7;
    public CompletionSettings settings = new();
    public Quaternion rotation = Quaternion.IDENTITY;
    public DataAssetRef<WeaponData> weapon;
    public int awakeCount;
    public int startCount;
    public int afterReloadCount;
    public int reloadAddCount;
    public GameObject reloadAddTarget;
    public int reloadAddedCount;
    public int connectCount;
    public int eventSum;
    public bool failConnect;
    public int command;
    public int jobCount;
    public int frameCount;
    public int tweenCompleted;
    public float tweenValue;
    public int timelineCount;
    public int assetDamage;
    public int cacheChecks;
    public int cacheFailures;
    public bool cacheScriptExpired;
    [KeepOnReload] float m_elapsed;
    float m_transient;
    GameObject m_target;
    Transform m_targetTransform;
    HealthComponent m_health;
    List<GameObject> m_objectCaches = [];
    List<Component> m_componentCaches = [];
    List<HealthComponent> m_healthCaches = [];
    protected override void Awake()
    {
        awakeCount++;
        CaptureBaseCache();
        Scene.TryFind("Target", out m_target);
        m_targetTransform = m_target?.Transform;
        m_health = m_target?.GetComponent<HealthComponent>();
    }
    protected override void Start() { startCount++; }
    protected override void Connect()
    {
        connectCount++;
        Events.Subscribe<int>(OnEvent);
        if (failConnect) throw new InvalidOperationException("Connect candidate must not publish its registrations.");
    }
    void OnEvent(int value) { eventSum += value; Transform.TranslateLocal(new Vector3(value, 0, 0)); }
    protected override void AfterReload()
    {
        afterReloadCount++;
        GameObject target = reloadAddTarget ?? GameObject;
        for (int index = 0; index < reloadAddCount; index++)
        {
            target.AddComponent<CompletionComponent>();
            reloadAddedCount++;
        }
        reloadAddCount = 0;
    }
    protected override void Update()
    {
        m_elapsed += Time.DeltaSeconds;
        m_transient += Time.DeltaSeconds;
        Transform.TranslateLocal(Vector3.ZERO);
        if (command == 1)
        {
            Jobs.Wait(0.5f, () => jobCount++);
            Jobs.WaitFrames(2, () => frameCount++);
            Jobs.Tween(0, 1, 0.5f, (float value) => tweenValue = value, completed: () => tweenCompleted++);
        }
        else if (command == 2)
            Jobs.Timeline().Then(() => timelineCount++).Wait(0.25f).Tween(0, 1, 0.25f, (float value) => tweenValue = value).Then(() => timelineCount++).Play();
        else if (command == 3) Events.Publish(3);
        else if (command == 4)
        {
            if (m_target is not null && m_target.IsAlive && m_targetTransform is not null && m_targetTransform.IsAlive
                && m_health is not null && m_health.IsAlive)
            {
                m_targetTransform.TranslateLocal(new Vector3(2, 0, 0));
                m_health.TakeDamage(1f);
                cacheChecks++;
            }
            else cacheFailures++;
            cacheScriptExpired = m_health is not null && !m_health.IsAlive;
        }
        else if (command == 5) assetDamage = DataAssets.Load(weapon)?.damage ?? -1;
        else if (command == 6)
        {
            if (settings.target is not null && settings.target.IsAlive)
            { settings.target.Transform.TranslateLocal(new Vector3(1, 0, 0)); cacheChecks++; }
            else cacheFailures++;
        }
        command = 0;
    }
    public bool HasReconnectedCache() => m_target is not null && m_targetTransform is not null && m_health is not null;
}

public sealed class GizmoProbeComponent : Script
{
    public int[] samples = [];
    public float radius = 1;
    public int command;
    int m_runtimeCache = 17;
    public int RuntimeCache => m_runtimeCache;
    /// @note 不正コードが analyzer を迂回しても draw scope が拒否し、private cache を候補へ引き継がないことを検証する fixture。
#pragma warning disable FBZZ2010
    protected override void DrawGizmos()
    {
        Vector3 position = Transform.WorldPosition;
        Gizmos.DepthTest = true;
        Gizmos.Line(position, position + new Vector3(m_runtimeCache + (samples.Length == 0 ? 0 : samples[0]), 1, 0));
        Gizmos.Sphere(position, radius);
        Gizmos.Box(position, new Vector3(radius, radius, radius));
        radius = 99;
        if (samples.Length != 0) samples[0] = 999;
        if (command == 1) Transform.TranslateLocal(new Vector3(100, 0, 0));
        else if (command == 2) Jobs.Wait(0, () => { });
        else if (command == 3) GameObject.GetComponent<HealthComponent>();
    }
#pragma warning restore FBZZ2010
    protected override void DrawGizmosSelected()
    {
        Gizmos.Line(Transform.WorldPosition, Transform.WorldPosition + new Vector3(1, 0, 0));
    }
}
