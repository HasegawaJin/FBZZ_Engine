/// @file    InspectorPresentationComponent.cs
/// @brief   Inspector settings, private observations and hidden nested values without property evaluation.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;
using System.Collections.Generic;
using Fbzz;

namespace ManagedFixtures;

[Serializable]
public sealed class InspectorPresentationSettings
{
    public Vector2 point = new(1, 2);
    [Setting(Visible = false)] public int generatedValue = 23;
}

public sealed class InspectorPresentationComponent : Script
{
    public double precision = 1e50;
    public uint[] counts = [0, uint.MaxValue];
    public ComponentRef<Component> component;
    public GameObject[] targets = [];
    public InspectorPresentationSettings nested = new();
    [Setting(Visible = false)] public int hiddenValue = 17;
    [NonSerialized, KeepOnReload] public int[] observedCounts = [1, 2];
    [NonSerialized] public List<GameObject> observedTargets = [];
    [NonSerialized] public InspectorPresentationSettings observedNested = new();
    [NonSerialized] public InspectorPresentationSettings observedNull;
    public readonly string status = "idle";
    [Setting(Label = "Authored Gain")] private float m_authoredGain = 1.5f;
    private Vector3 m_cachedDirection = Vector3.FORWARD;
    private object m_unsupportedCache = new();
    public static int getterReads;
    public float AuthoredGain => m_authoredGain;
    public Vector3 CachedDirection => m_cachedDirection;
    public bool HasCache => m_unsupportedCache is not null;
    public int AutomaticProperty { get; set; } = 11;
    public int GetterTrap
    {
        get { getterReads++; throw new InvalidOperationException("Inspector must not evaluate properties."); }
    }
}
