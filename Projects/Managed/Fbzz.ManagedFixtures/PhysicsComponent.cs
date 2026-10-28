/// @file    PhysicsComponent.cs
/// @brief   実 CoreCLR の剛体操作・物理検索・保持 facade を検証する fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

namespace ManagedFixtures;

public sealed class PhysicsComponent : Script
{
    public GameObject target;
    public int operation;
    public Vector3 value;
    public Vector3 point;
    public float scalar = 1f;
    public bool flag;
    public bool freezeX;
    public bool freezeY;
    public bool freezeZ;
    public bool runInUpdate;
    public bool useCached;
    public Vector3 origin;
    public Vector3 direction = Vector3.FORWARD;
    public float maxDistance = 100f;
    public float radius = 0.5f;
    public int layerA;
    public int layerB;
    public bool bodyAlive;
    public bool bodyEnabled;
    public bool isStatic;
    public bool sleeping;
    public Vector3 velocity;
    public Vector3 angularVelocity;
    public Vector3 bodyPosition;
    public float mass;
    public float gravityScale;
    public Vector3 worldGravity;
    public bool layersCollide;
    public bool hitFound;
    public bool hitObjectAlive;
    public GameObject hitObject;
    public Vector3 hitPoint;
    public Vector3 hitNormal;
    public float hitDistance;
    public GameObject[] hits = [];
    public float[] distances = [];
    RigidBody m_cached;

    protected override void Awake() { m_cached = (target != null ? target : GameObject).GetComponent<RigidBody>(); }
    protected override void Update() { if (runInUpdate) Run(); }
    protected override void FixedUpdate() { if (!runInUpdate) Run(); }

    void Run()
    {
        GameObject owner = target != null ? target : GameObject;
        RigidBody body = useCached ? m_cached : owner.GetComponent<RigidBody>();
        if (operation <= 14 && body != null)
        {
            switch (operation)
            {
                case 1: body.Velocity = value; break;
                case 2: body.AngularVelocity = value; break;
                case 3: body.Mass = scalar; break;
                case 4: body.GravityScale = scalar; break;
                case 5: body.IsStatic = flag; break;
                case 6: body.AddForce(value); break;
                case 7: body.AddImpulse(value); break;
                case 8: body.AddTorque(value); break;
                case 9: body.AddForceAtPoint(value, point); break;
                case 10: body.WakeUp(); break;
                case 11: body.Sleep(); break;
                case 12: body.SetFreezePosition(freezeX, freezeY, freezeZ); break;
                case 13: body.SetFreezeRotation(freezeX, freezeY, freezeZ); break;
                case 14: body.Teleport(value); break;
            }
        }
        bodyAlive = body != null && body.IsAlive;
        bodyEnabled = body != null && bodyAlive && body.Enabled;
        if (bodyEnabled && body != null)
        {
            velocity = body.Velocity;
            angularVelocity = body.AngularVelocity;
            bodyPosition = body.Position;
            mass = body.Mass;
            gravityScale = body.GravityScale;
            isStatic = body.IsStatic;
            sleeping = body.IsSleeping;
        }
        worldGravity = Physics.WorldGravity;
        layersCollide = Physics.LayersCollide(layerA, layerB);
        if (operation == 15 || operation == 16)
        {
            RaycastHit hit;
            hitFound = operation == 15 ? Physics.Raycast(origin, direction, maxDistance, out hit)
                : Physics.SphereCast(origin, radius, direction, maxDistance, out hit);
            hitObject = hitFound ? hit.GameObject : null;
            if (hitFound) { hitPoint = hit.Point; hitNormal = hit.Normal; hitDistance = hit.Distance; }
        }
        else if (operation == 17)
        {
            RaycastHit[] result = Physics.RaycastAll(origin, direction, maxDistance);
            hits = new GameObject[result.Length]; distances = new float[result.Length];
            for (int index = 0; index < result.Length; index++)
            { hits[index] = result[index].GameObject; distances[index] = result[index].Distance; }
        }
        else if (operation == 18) hits = Physics.OverlapSphere(origin, radius);
        hitObjectAlive = hitObject != null && hitObject.IsAlive;
    }
}
