/// @file    ServicesComponent.cs
/// @brief   通常 C# Component facade の実 CoreCLR サービス接続を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

namespace ManagedFixtures;

public sealed class ServicesComponent : Script
{
    [System.Serializable] sealed class SavedProfile
    {
        public int coins;
        public int[] scores = [];
        public string note = "";
        public MaterialAssetRef material;
    }
    public GameObject target;
    public GameObject ignored;
    public int operation;
    public int mask = -1;
    public int triggers;
    public bool runInFixed;
    public float scalar = 2f;
    public Vector3 value;
    public Vector3 point;
    public string text = "";
    public float observed;
    public bool succeeded;
    public Vector3 observedVector;
    public string observedText = "";
    public GameObject detected;
    public int hitCount;
    AudioLoop m_loop = new();
    RigidBody m_retainedBody;
    Script[] m_componentBuffer = [];
    BlendWeight[] m_blendBuffer = [];

    protected override void Update() { if (!runInFixed) Run(); }
    protected override void FixedUpdate() { if (runInFixed) Run(); }
    protected override void DrawGizmos()
    {
        if (operation != 33 && operation != 34) return;
        Run();
        Gizmos.Sphere(Vector3.ZERO, observed == 1f ? 1f : 2f);
    }

    void Run()
    {
        GameObject owner = target != null ? target : GameObject;
        switch (operation)
        {
            case 1:
                if (owner.GetComponent<Animator>() is Animator animator)
                { animator.SetSpeed(scalar); observed = animator.GetSpeed(); }
                break;
            case 2:
                if (owner.GetComponent<AudioSource>() is AudioSource audio)
                { audio.SetVolume(scalar); audio.Play(text); observed = audio.GetVolume(); }
                break;
            case 3:
                if (owner.GetComponent<UIText>() is UIText label)
                { label.SetText(text); observedText = label.GetText(); }
                break;
            case 4:
                if (owner.GetComponent<Camera>() is Camera camera)
                { Vector3 screen = camera.WorldToScreenPoint(value); observedVector = camera.ScreenToWorldPoint(screen); }
                break;
            case 5:
                if (owner.GetComponent<NavMeshAgent>() is NavMeshAgent agent)
                { agent.Warp(value); observedVector = owner.Transform.WorldPosition; }
                break;
            case 6:
                if (owner.GetComponent<CharacterController>() is CharacterController controller)
                { controller.JumpAtVelocity(scalar); succeeded = controller.IsGrounded(); }
                break;
            case 7:
                if (owner.GetComponent<SphereCollider>() is SphereCollider sphere)
                { sphere.Radius = scalar; observed = sphere.Radius; }
                break;
            case 8:
                if (owner.GetComponent<RigidBody>() is RigidBody body)
                {
                    body.LinearDrag = scalar; body.UseCCD = true;
                    body.AddImpulseAtPoint(value, point); observedVector = body.Velocity; succeeded = body.UseCCD;
                }
                break;
            case 9:
                PhysicsQueryOptions options = new(unchecked((uint)mask), (QueryTriggerMode)triggers, ignored);
                RaycastHit hit;
                succeeded = Physics.Raycast(value, point, scalar, options, out hit);
                detected = succeeded ? hit.GameObject : null;
                break;
            case 10:
                PhysicsQueryOptions allOptions = new(unchecked((uint)mask), (QueryTriggerMode)triggers, ignored);
                RaycastHit[] hits = Physics.RaycastAll(value, point, scalar, allOptions);
                hitCount = hits.Length;
                break;
            case 11:
                owner.RemoveComponent<SphereCollider>();
                succeeded = owner.GetComponent<SphereCollider>() is SphereCollider;
                hitCount = Physics.RaycastAll(value, point, scalar).Length;
                break;
            case 12:
                succeeded = Audio.UpdateLoop(m_loop, text, scalar); observed = m_loop.IsValid ? 1f : 0f;
                break;
            case 13:
                Audio.ReleaseLoop(m_loop); observed = m_loop.IsValid ? 1f : 0f;
                break;
            case 14:
                m_retainedBody = owner.GetComponent<RigidBody>(); succeeded = m_retainedBody != null && m_retainedBody.IsAlive;
                break;
            case 15:
                succeeded = m_retainedBody != null && m_retainedBody.IsAlive;
                break;
            case 16:
                succeeded = m_retainedBody == owner.GetComponent<RigidBody>(); observed = m_retainedBody.IsAlive ? 1f : 0f;
                break;
            case 17:
                if (m_componentBuffer.Length != mask) m_componentBuffer = new Script[mask];
                System.Array.Fill(m_componentBuffer, this);
                CollectionReadResult components = owner.GetComponents<Script>(m_componentBuffer);
                hitCount = components.Required; observed = components.Written;
                succeeded = components.Complete ? owner.GetComponents<Script>().Length == components.Required
                    && owner.GetComponent<Script>() == m_componentBuffer[0] : System.Array.TrueForAll(m_componentBuffer, item => item == this);
                break;
            case 18:
                hitCount = owner.UI.GetText().Length;
                break;
            case 19:
                Animator blendAnimator = owner.GetComponent<Animator>();
                if (blendAnimator is null) { hitCount = 0; observed = 0; succeeded = false; break; }
                if (m_blendBuffer.Length != mask) m_blendBuffer = new BlendWeight[mask];
                System.Array.Fill(m_blendBuffer, new BlendWeight("sentinel", -1));
                CollectionReadResult blends = blendAnimator.GetCurrentBlendWeights(m_blendBuffer);
                hitCount = blends.Required; observed = blends.Written;
                succeeded = blends.Complete ? blendAnimator.GetCurrentBlendWeights().Length == blends.Required
                    && m_blendBuffer[^1].Weight == blends.Required - 1 : System.Array.TrueForAll(m_blendBuffer, item => item.Name == "sentinel");
                break;
            case 20:
                Light sourceLight = owner.GetComponent<Light>();
                if (sourceLight is null) break;
                sourceLight.Type = LightType.SPOT; sourceLight.Color = value; sourceLight.Intensity = scalar;
                sourceLight.SetSpotAngles(15, 40); observedVector = sourceLight.Color; observed = sourceLight.Intensity;
                succeeded = sourceLight.IsAlive && sourceLight.Type == LightType.SPOT;
                break;
            case 21:
                ParticleSystem particles = owner.GetComponent<ParticleSystem>();
                if (particles is null) break;
                particles.Play(); particles.Burst(mask); observed = particles.EmissionRate; succeeded = particles.IsPlaying;
                break;
            case 22:
                ParticleSystem stoppedParticles = owner.GetComponent<ParticleSystem>();
                if (stoppedParticles is null) break;
                stoppedParticles.Stop(true); hitCount = stoppedParticles.ParticleCount; succeeded = stoppedParticles.IsPlaying;
                break;
            case 23:
                succeeded = Scene.TryLoadRegistered(text); observedText = Scene.RegisteredName;
                break;
            case 24:
                App.Quit(); succeeded = true;
                break;
            case 25:
                string stored = Save.GetString("large", "fallback");
                hitCount = stored.Length;
                observedText = stored.Length > 65523 ? stored.Substring(65523, 1) : "";
                succeeded = stored.EndsWith("tail", System.StringComparison.Ordinal);
                break;
            case 26:
                Material material = owner.GetComponent<Material>();
                if (material is null) break;
                material.SetSlotVisible(mask, true); succeeded = material.IsAlive; hitCount = material.SlotCount;
                break;
            case 27:
                Save.SetString("uploaded", text);
                observedText = Save.GetString("uploaded", "fallback"); hitCount = observedText.Length; succeeded = observedText == text;
                break;
            case 28:
                SavedProfile profile = new() { coins = 17, scores = new int[40001], note = text,
                    material = new MaterialAssetRef("11111111111111111111111111111111") };
                profile.scores[^1] = int.MaxValue;
                Save.Write("profile", profile);
                SavedProfile defaults = new() { coins = 99, scores = [8], note = "default" };
                succeeded = Save.TryRead("profile", defaults, out SavedProfile restored) && restored.coins == 17
                    && restored.scores.Length == 40001 && restored.scores[^1] == int.MaxValue && restored.note == text
                    && restored.material.Reference == profile.material.Reference && defaults.coins == 99 && defaults.scores.Length == 1;
                hitCount = succeeded ? restored.scores.Length : 0;
                break;
            case 29:
                Render.Brightness = scalar;
                Render.ShadowCascades = mask;
                observed = Render.ShadowCascades;
                succeeded = Render.IsAvailable && Render.Brightness == 4f;
                break;
            case 30:
                succeeded = Display.TrySetResolution(mask, (int)scalar);
                observed = Display.CanChangeWindow ? 1f : 0f;
                break;
            case 31:
                Joint joint = GameObject.GetComponent<Joint>();
                if (joint is null || ignored is null) break;
                succeeded = joint.TryConnectFixed(ignored);
                observed = joint.Distance;
                break;
            case 32:
                Joint chain = GameObject.GetComponent<Joint>();
                if (chain is null || target is null || ignored is null) break;
                GameObject[] links = [target, ignored];
                succeeded = chain.TryConnectChain(links, scalar, mask);
                hitCount = links.Length;
                break;
            case 33:
                if (owner.GetComponent<InstancedMeshes>() is InstancedMeshes instances)
                {
                    succeeded = !instances.TrySetSharedInstances(default);
                    instances.Enabled = false;
                    instances.CastShadows = true;
                    hitCount = instances.InstanceCount;
                    observed = instances.Enabled ? 1f : 0f;
                    observedText = instances.SharedInstances.Reference;
                }
                break;
            case 34:
                if (owner.GetComponent<InstancedMeshes>() is InstancedMeshes readInstances)
                {
                    observed = readInstances.Enabled ? 1f : 0f;
                    hitCount = readInstances.InstanceCount;
                    observedText = readInstances.SharedInstances.Reference;
                }
                break;
        }
    }
}
