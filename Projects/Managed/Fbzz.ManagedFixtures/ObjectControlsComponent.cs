/// @file    ObjectControlsComponent.cs
/// @brief   実 Host の一括生成・Component 制御と受け手所有のイベントを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

public sealed class ObjectControlsComponent : Script
{
    public int operation;
    public GameObject[] created = [];
    public int childrenCount;
    public int descendantsCount;
    public bool configured;
    public bool removed;
    public bool bodyAlive;
    public int callerJobs;
    public int receiverJobs;
    public int receiverEvents;
    public int destroyed;
    ObjectControlsChildComponent m_child;
    RigidBody m_body;

    protected override void Update()
    {
        if (operation == 1)
        {
            SpawnPose[] poses = [new(Vector3.ZERO, Quaternion.IDENTITY), new(new Vector3(1f, 0f, 0f), Quaternion.IDENTITY), new(new Vector3(2f, 0f, 0f), Quaternion.IDENTITY)];
            created = new GameObject[poses.Length];
            BatchResult result = Scene.CreateMany("Child", poses, created);
            if (!result.Succeeded) return;
            created[0].SetParent(GameObject);
            created[1].SetParent(created[0]);
            created[2].SetParent(GameObject);
            created[0].Rename("Controlled");
            created[0].Tag = "Enemy";
            created[0].Layer = 5;
            m_body = created[0].AddComponent<RigidBody>();
            created[0].AddComponent<SphereCollider>();
            m_child = created[0].AddComponent<ObjectControlsChildComponent>();
            m_child.Register();
            Events.Publish(3);
            m_child.Enabled = false;
            Events.Publish(7);
            Jobs.WaitFrames(1, () => ++callerJobs);
            childrenCount = GameObject.Children.Length;
            descendantsCount = GameObject.GetDescendants().Length;
            configured = created[0].CompareTag("Enemy") && created[0].Layer == 5 && created[1].Parent == created[0];
        }
        else if (operation == 2)
        {
            removed = created[0].RemoveComponent<ObjectControlsChildComponent>() && created[0].RemoveComponent<RigidBody>();
        }
        bodyAlive = m_body != null && m_body.IsAlive;
        receiverJobs = ObjectControlsChildComponent.jobCount;
        receiverEvents = ObjectControlsChildComponent.eventCount;
        destroyed = ObjectControlsChildComponent.destroyCount;
        operation = 0;
    }
}

public sealed class ObjectControlsChildComponent : Script
{
    public static int jobCount;
    public static int eventCount;
    public static int destroyCount;
    public void Register()
    {
        Jobs.WaitFrames(1, () => ++jobCount);
        Events.Subscribe<int>((int value) => eventCount += value);
    }
    protected override void OnDestroy() { ++destroyCount; }
}
