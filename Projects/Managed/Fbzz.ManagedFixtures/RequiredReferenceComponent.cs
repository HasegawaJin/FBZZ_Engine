/// @file    RequiredReferenceComponent.cs
/// @brief   Required scene references nested in saved classes, structs and collections.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using Fbzz;

namespace ManagedFixtures;

[Serializable]
public sealed class RequiredReferenceSettings
{
    [RequiredReference] public GameObject target;
    [RequiredReference] public GameObject[] targets = [];
    [RequiredReference] public List<GameObject> list = [];
    [ValueRange(0f, 10f)] public float speed = 1f;
}

[Serializable]
public struct RequiredReferenceLeaf
{
    [RequiredReference] public GameObject target;
}

[Serializable]
public sealed class RequiredReferenceBranch
{
    public List<RequiredReferenceLeaf> leaves = [];
}

public sealed class RequiredReferenceComponent : Script
{
    public RequiredReferenceSettings settings = new();
    public RequiredReferenceBranch[] branches = [];
    [RequiredReference] public List<GameObject> requiredList = [];
    public GameObject optional;
    public List<GameObject> optionalList = [];
    public bool clearTargetInAwake;
    public int awakeCount;
    public int startCount;
    public int updateCount;
    protected override void Awake() { awakeCount++; if (clearTargetInAwake) settings.target = null; }
    protected override void Start() { startCount++; }
    protected override void Update() { updateCount++; }
}
