/// @file    ObjectLifecycleComponent.cs
/// @brief   実 CoreCLR から生成・Prefab 復元・破棄 queue の寿命を観測する fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

public sealed class ObjectLifecycleComponent : Script
{
    public int operation;
    public string objectName = "ManagedCreated";
    public string prefab = string.Empty;
    public Vector3 position = new(7f, 8f, 9f);
    public Vector3 rotationXYZ = Vector3.ZERO;
    public float rotationW = 1f;
    public bool usePose;
    public float delay;
    public GameObject result;
    public GameObject[] targets = [];
    public bool succeeded;
    public bool aliveAfterRequest;
    public int batchApplied;
    public int batchSkipped;
    public int batchStatus;
    public int updateCount;
    public int observedDestroyed;
    public int requestedCapacity = 1;
    public bool canCreate;
    public GameObject[] spawned = [];
    public int queriedCount;
    public int uniqueCount;

    protected override void Update()
    {
        ++updateCount;
        Quaternion rotation = new(rotationXYZ.X, rotationXYZ.Y, rotationXYZ.Z, rotationW);
        switch (operation)
        {
            case 1:
                result = usePose ? Scene.Create(objectName, position, rotation) : Scene.Create(objectName);
                succeeded = result != null;
                break;
            case 2:
                result = usePose ? Scene.Instantiate(prefab, position, rotation) : Scene.Instantiate(prefab);
                succeeded = result != null;
                break;
            case 3:
                succeeded = result != null && result.Destroy(delay);
                break;
            case 4:
                BatchResult batch = Scene.DestroyMany(targets, delay);
                succeeded = batch.Succeeded;
                batchApplied = batch.AppliedCount;
                batchSkipped = batch.SkippedCount;
                batchStatus = (int)batch.Status;
                break;
            case 5:
                SpawnPose[] poses = new SpawnPose[requestedCapacity];
                spawned = new GameObject[requestedCapacity];
                for (int index = 0; index < poses.Length; ++index)
                    poses[index] = new SpawnPose(new Vector3(index, 0, 0), Quaternion.IDENTITY);
                BatchResult creation = Scene.CreateMany(objectName, poses, spawned);
                succeeded = creation.Succeeded;
                batchApplied = creation.AppliedCount;
                batchStatus = (int)creation.Status;
                break;
            case 6:
                GameObject[] queried = Scene.FindAll(objectName, includeInactive: true);
                queriedCount = queried.Length;
                uniqueCount = new System.Collections.Generic.HashSet<GameObject>(queried).Count;
                succeeded = queriedCount == requestedCapacity && uniqueCount == requestedCapacity;
                break;
        }
        aliveAfterRequest = result != null && result.IsAlive;
        canCreate = Scene.CanCreate(requestedCapacity);
        observedDestroyed = ObjectLifecycleChildComponent.destroyedCount;
    }
}

public sealed class ObjectLifecycleChildComponent : Script
{
    public GameObject target;
    public int awakeCount;
    public int startCount;
    public int updateCount;
    public int deserializeCount;
    public bool internalReferenceAlive;
    public static int destroyedCount;

    protected override void OnAfterDeserialize() { ++deserializeCount; }
    protected override void Awake() { ++awakeCount; internalReferenceAlive = target != null && target.IsAlive; }
    protected override void Start() { ++startCount; }
    protected override void Update() { ++updateCount; }
    protected override void OnDestroy() { ++destroyedCount; }
}
