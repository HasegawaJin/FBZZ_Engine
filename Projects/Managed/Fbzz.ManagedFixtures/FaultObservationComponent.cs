/// @file    FaultObservationComponent.cs
/// @brief   fault 後の個別フィールド観測と getter 非実行を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using Fbzz;

namespace ManagedFixtures;

[Serializable]
public sealed class FaultInspectionSettings
{
    public static int constructorCalls;
    public static int getterCalls;
    public int constructorSnapshot;
    [FieldKey("$version")] public int version = 23;
    public string label = "before";
    public List<float> weights = [1f, 2f];
    public Quaternion[] rotations = [Quaternion.IDENTITY];
    public GameObject target;
    public FaultInspectionSettings() { constructorSnapshot = ++constructorCalls; }
    public int GetterTrap
    {
        get { getterCalls++; throw new InvalidOperationException("Observation must not execute a nested property getter."); }
    }
}

class FaultObservationComponent : Script
{
    public int updateCount;
    public int getterReads;
    public float scalar = 1f;
    public float[] numbers = [1f, 2f];
    public Vector3[] vectors = [Vector3.ZERO];
    public string text = "before";
    public string badText = "before";
    public string[] labels = ["first", "second"];
    public GameObject target;
    public GameObject[] targets = [];
    public GameObject[] nullTargets = [];
    public Quaternion rotation = Quaternion.IDENTITY;
    public FaultInspectionSettings settings = new();
    public FaultInspectionSettings stableSettings = new();
    public FaultInspectionSettings optionalSettings;
    public List<int> samples = [3, 5];
    public DataAssetRef<WeaponData> weapon;
    public int nestedConstructorCalls;
    public int nestedGetterReads;
    object m_unsupported = new();
    public bool HasUnsupported => m_unsupported is not null;

    public int GetterTrap
    {
        get { getterReads++; throw new InvalidOperationException("Observation must not execute a property getter."); }
    }

    protected override void Update()
    {
        updateCount++;
        nestedConstructorCalls = FaultInspectionSettings.constructorCalls;
        nestedGetterReads = FaultInspectionSettings.getterCalls;
        scalar = float.NaN;
        rotation = new Quaternion(0f, 0f, 0f, float.NaN);
        settings.weights[1] = float.PositiveInfinity;
        numbers = [1f, float.PositiveInfinity];
        vectors = [Vector3.ZERO, new Vector3(float.NaN, 0f, 0f)];
        text = null!;
        badText = "\uD800";
        labels = ["valid", null!];
        nullTargets = null!;
        throw new InvalidOperationException("Intentional observation failure.");
    }
}
