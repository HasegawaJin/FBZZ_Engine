/// @file    ActiveRagdollSampleComponent.cs
/// @brief   公開 API だけで歩行、接触反応、把持、起き上がりを制御する。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#nullable enable
using System;
using Fbzz;

public sealed class ActiveRagdollSampleComponent : Script
{
    private const float HIP_HEIGHT = 0.975f;
    private const float TOE_HEIGHT = 0.045f;
    private const float STEP_SECONDS = 0.54f;
    private const float STRIDE = 0.40f;
    private const int HIPS = 0, TORSO = 1, HEAD = 2;
    private static readonly float[] FOOT_SEARCH = [0f, 0.075f, 0.15f, 0.225f, -0.075f, -0.15f];
    private static readonly string[] BONES =
    [
        "Hips", "Torso", "Head",
        "LeftThigh", "LeftShin", "LeftFoot", "LeftUpperArm", "LeftForearm", "LeftHand",
        "RightThigh", "RightShin", "RightFoot", "RightUpperArm", "RightForearm", "RightHand"
    ];

    /// @note 観測値はワールド空間の剛体と実際の接地から更新する。
    public bool ready;
    public string state = "starting";
    public bool finite = true;
    public float simulationWeight;
    public float forwardTravel;
    public float lateralTravel;
    public float facingDegrees;
    public bool grounded;
    public float hipHeight;
    public int landings;
    public int stepsUp;
    public int stepsDown;
    public float maxSupportSlip;
    public int trips;
    public int tripRecoveries;
    public int airborneTicks;
    public int jumpLandings;
    public float jumpHeight;
    public bool grabbing;
    public float crateForward;
    public float cratePullBack;
    public int proneRecoveries;
    public int supineRecoveries;
    public int riseSupportFrames;
    public string rigStatus = "";

    private enum Phase { GROUND, TRIP, CROUCH, AIRBORNE, LAND, FALLING, RISING }

    private sealed class Foot(int bone, float side)
    {
        internal readonly int Bone = bone;
        internal readonly float Side = side;
        internal Vector3 Goal;
        internal Quaternion Rotation = Quaternion.IDENTITY;
        internal bool Planted;
        internal Vector3 Anchor;
        internal Vector3 LocalAnchor;
    }

    private readonly Foot[] m_feet = [new(5, -1f), new(11, 1f)];
    private readonly RagdollBodyState[] m_rest = new RagdollBodyState[BONES.Length];
    private readonly RagdollBodyState[] m_fallPose = new RagdollBodyState[BONES.Length];
    private readonly Vector3[] m_previous = new Vector3[BONES.Length];
    private readonly Vector3[] m_positions = new Vector3[BONES.Length];
    private readonly Quaternion[] m_rotations = new Quaternion[BONES.Length];
    private Ragdoll? m_rig;
    private RigidBody? m_crate;
    private Transform? m_camera;
    private PhysicsQueryOptions m_floorQuery;
    private Phase m_phase;
    private Vector3 m_origin;
    private Vector3 m_crateOrigin;
    private Vector3 m_center;
    private Vector3 m_stepCenterStart;
    private Vector3 m_stepCenterEnd;
    private Vector3 m_footStart;
    private Vector3 m_footEnd;
    private Quaternion m_footRotationStart;
    private Foot? m_swing;
    private float m_yaw;
    private float m_yawStart;
    private float m_yawEnd;
    private float m_stepTime;
    private float m_stepLength;
    private float m_supportHeight;
    private float m_phaseTime;
    private float m_jumpFrom;
    private float m_travelSign;
    private bool m_nextRight;
    private bool m_tripObstacle;
    private float m_tripCooldown;
    private bool m_catchStep;
    private bool m_fallForward;
    private bool m_recoverProne;
    private bool m_reaching;
    private bool m_jumpDown;
    private bool m_grabDown;
    private bool m_proneDown;
    private bool m_supineDown;
    private float m_reach;
    private Vector3 m_lastCameraTarget;

    protected override void FixedUpdate()
    {
        if (m_rig is null)
        {
            m_rig = GameObject.GetComponent<Ragdoll>();
            if (m_rig is null) return;
            m_floorQuery = new PhysicsQueryOptions(triggers: QueryTriggerMode.IGNORE, ignore: GameObject);
            m_crate = Scene.TryFind("PracticeCrate", out var crate) ? crate.GetComponent<RigidBody>() : null;
            m_camera = Scene.TryFind("MainCamera", out var camera) ? camera.Transform : null;
            m_rig.SetSimulation(12, 0.4f, 1.1f, false);
            m_rig.SetMuscle(1f, 1f, 1f);
            m_rig.SetRootAnchor(0f, 0f, 0f);
            m_rig.SetBlend(0f, 0f);
            m_rig.BeginActive();
            return;
        }

        simulationWeight = m_rig.Weight;
        rigStatus = m_rig.Status;
        if (!ready)
        {
            InitializePose();
            return;
        }

        float dt = Time.DeltaSeconds;
        if (dt <= 0f) return;
        var hips = Body(HIPS);
        var torso = Body(TORSO);
        var left = Body(m_feet[0].Bone);
        var right = Body(m_feet[1].Bone);
        m_tripCooldown = MathF.Max(0f, m_tripCooldown - dt);
        finite &= hips.Valid && torso.Valid && left.Valid && right.Valid &&
            hips.Position.IsFinite && torso.Position.IsFinite && left.Position.IsFinite && right.Position.IsFinite;
        if (!finite) return;
        forwardTravel = hips.Position.Z - m_origin.Z;
        lateralTravel = hips.Position.X - m_origin.X;
        Vector3 facing = torso.Rotation * Vector3.FORWARD;
        facingDegrees = MathF.Atan2(facing.X, facing.Z) * (180f / MathF.PI);
        grounded = left.Supported || right.Supported;
        hipHeight = hips.Position.Y;
        TrackSupport(m_feet[0], left);
        TrackSupport(m_feet[1], right);
        TrackCrate();

        bool jump = Pressed(KeyCode.SPACE, ref m_jumpDown);
        bool grab = Pressed(KeyCode.UP, ref m_grabDown);
        bool prone = Pressed(KeyCode.LEFT, ref m_proneDown);
        bool supine = Pressed(KeyCode.RIGHT, ref m_supineDown);
        Vector3 input = new(Axis(KeyCode.D, KeyCode.A), 0f, Axis(KeyCode.W, KeyCode.S));
        float turn = Axis(KeyCode.E, KeyCode.Q);
        m_phaseTime += dt;
        if (m_phase == Phase.GROUND)
        {
            if (grab) ToggleGrip();
            if (m_swing is null && grounded && !grabbing && !m_reaching)
            {
                if (prone || supine) StartFall(prone);
                else if (jump && left.Supported && right.Supported) Enter(Phase.CROUCH);
            }
        }

        switch (m_phase)
        {
            case Phase.GROUND:
            case Phase.TRIP:
                Walk(input, turn, left, right, dt);
                break;
            case Phase.CROUCH:
            case Phase.AIRBORNE:
            case Phase.LAND:
                Jump(hips, dt);
                break;
            case Phase.FALLING:
            case Phase.RISING:
                Recover(hips, torso, dt);
                break;
        }
        UpdateCamera(hips.Position, dt);
    }

    private void InitializePose()
    {
        if (simulationWeight < 1f || m_rig!.ParticleCount != BONES.Length) return;
        for (int i = 0; i < BONES.Length; ++i)
        {
            m_rest[i] = Body(i);
            if (!m_rest[i].Valid) return;
            m_previous[i] = m_rest[i].Position;
        }
        m_origin = m_rest[HIPS].Position;
        m_center = new Vector3(m_origin.X, 0f, m_origin.Z);
        foreach (Foot foot in m_feet)
        {
            if (!GroundAt(m_rest[foot.Bone].Position, out float ground)) return;
            var rest = m_rest[foot.Bone];
            foot.Goal = new Vector3(rest.Position.X, ground + TOE_HEIGHT - 0.002f, rest.Position.Z);
            foot.Rotation = rest.Rotation;
            m_center.Y += ground * 0.5f;
        }
        m_crateOrigin = m_crate?.Position ?? Vector3.ZERO;
        m_supportHeight = m_center.Y;
        m_lastCameraTarget = m_origin;
        state = "idle";
        ready = true;
    }

    private void Walk(Vector3 input, float turn, RagdollBodyState left, RagdollBodyState right, float dt)
    {
        Vector3 direction = Yaw(m_yaw) * input.NormalizedOr(Vector3.ZERO);
        m_travelSign = input.Z;
        if (!grabbing && m_reaching) Reach(dt);
        if (grabbing && (m_crate?.IsAlive != true || m_rig!.GetGrabState(BONES[8]).Status != RagdollGrabStatus.Holding))
            ReleaseHands();

        if (m_swing is Foot moving)
        {
            m_stepTime += dt;
            float progress = Clamp01(m_stepTime / m_stepLength);
            float eased = Smooth(progress);
            m_yaw = Mix(m_yawStart, m_yawEnd, eased);
            float path = Smooth(Clamp01((progress - 0.21f) / 0.57f));
            Vector3 sole = Mix(m_footStart, m_footEnd, path);
            float arc = Smooth(Clamp01(progress / 0.3f)) * Smooth(Clamp01((1f - progress) / 0.32f));
            float lift = m_tripObstacle ? 0.033f : 0.11f;
            sole.Y += arc * (lift + MathF.Abs(m_footEnd.Y - m_footStart.Y) * 0.65f);
            moving.Goal = sole;
            moving.Rotation = Quaternion.Slerp(m_footRotationStart, Yaw(m_yawEnd), eased);
            float transfer = Smooth(Clamp01((progress - 0.18f) / 0.82f));
            Vector3 center = Mix(m_stepCenterStart, m_stepCenterEnd, transfer);
            m_center.X = center.X;
            m_center.Z = center.Z;
            var observed = moving.Side < 0f ? left : right;
            if (progress < 0.86f) AssistSwing(moving, observed, dt);
            if (m_tripObstacle && progress > 0.2f && DetectTrip(observed, direction))
                StartTrip(moving, observed);
            else if (progress >= 1f && Plant(moving, observed))
                FinishStep();
            else if (m_stepTime > m_stepLength + 0.60f)
                AbortStep(moving, observed);
        }

        if (m_swing is null && m_phase == Phase.TRIP)
        {
            if (m_catchStep)
            {
                m_catchStep = false;
                BeginStep(Yaw(m_yaw) * Vector3.FORWARD, 0f, 0.34f);
                if (m_swing is null) Enter(Phase.GROUND);
            }
        }
        if (m_phase == Phase.GROUND && m_swing is null)
        {
            Plant(m_feet[0], left);
            Plant(m_feet[1], right);
            if ((direction.LengthSquared > 0f || turn != 0f) && (m_feet[0].Planted || m_feet[1].Planted))
                BeginStep(direction, turn);
        }
        state = m_phase == Phase.TRIP ? "tripping" : m_swing is null ? "idle" : "walking";
        float shift = m_swing is null ? 0f : MathF.Sin(MathF.PI * Clamp01(m_stepTime / m_stepLength)) * m_swing.Side;
        float trip = m_phase == Phase.TRIP ? MathF.Sin(MathF.PI * Clamp01(m_phaseTime / 0.64f)) : 0f;
        float pitch = 0.04f + trip * 0.48f;
        if (grabbing) pitch += 0.20f * m_travelSign;
        float lower = m_swing is not null ? Math.Clamp(m_footStart.Y - m_footEnd.Y, 0f, 0.06f) *
            Smooth(Clamp01(m_stepTime / m_stepLength)) : 0f;
        Pose(m_center, 0.10f + 0.025f * MathF.Abs(shift) + lower, pitch, -0.09f * shift, dt);
    }

    private void BeginStep(Vector3 direction, float turn, float stride = STRIDE)
    {
        Foot swing = m_feet[m_nextRight ? 1 : 0];
        Foot stance = m_feet[m_nextRight ? 0 : 1];
        if (!stance.Planted && swing.Planted) (swing, stance) = (stance, swing);
        if (!stance.Planted) return;

        float yawEnd = m_yaw + turn * 0.53f;
        Quaternion rotation = Yaw(yawEnd);
        Vector3 forward = rotation * Vector3.FORWARD;
        Vector3 side = rotation * Vector3.RIGHT;
        Vector3 end = m_center + direction * (grabbing ? stride * 0.66f : stride) +
            side * (swing.Side * 0.125f) + forward * 0.059f;
        float gap = swing.Side * Vector3.Dot(end - stance.Goal, side);
        end += side * (swing.Side * MathF.Max(0f, 0.205f - gap));
        float stanceHeight = stance.Goal.Y - TOE_HEIGHT;
        if (!TryFoothold(end, direction, rotation, stanceHeight, out end)) return;
        m_tripObstacle = m_phase == Phase.GROUND && m_tripCooldown <= 0f && CrossesTripBar(swing.Goal, end);
        m_nextRight = swing.Side < 0f;
        m_swing = swing;
        m_footStart = swing.Goal;
        m_footEnd = end;
        m_footRotationStart = swing.Rotation;
        m_yawStart = m_yaw;
        m_yawEnd = yawEnd;
        m_stepTime = 0f;
        m_stepLength = (m_phase == Phase.TRIP ? 0.41f : STEP_SECONDS) + (grabbing ? 0.13f : 0f);
        m_stepCenterStart = m_center;
        m_stepCenterEnd = (end + stance.Goal) * 0.5f - forward * 0.059f;
        /// @note 高さは接地の確認後に上げる。腰を先に段差へ動かすと支持足を引っ張る。
        m_stepCenterEnd.Y = m_center.Y;
        Unplant(swing);
    }

    /// @note 箱型の足裏が床から離れるときにだけ、遊脚の目標へ有限の外力を足す。
    private void AssistSwing(Foot foot, RagdollBodyState observed, float dt)
    {
        Vector3 goalVelocity = (foot.Goal - m_previous[foot.Bone]) / dt;
        Vector3 force = (foot.Goal - observed.Position) * 4200f + (goalVelocity - observed.Velocity) * 100f;
        float envelope = Smooth(Clamp01(m_stepTime / 0.08f)) *
            Smooth(Clamp01((0.86f * m_stepLength - m_stepTime) / 0.12f));
        Vector3 bounded = force.NormalizedOr(Vector3.ZERO) * (MathF.Min(680f, force.Length) * envelope);
        m_rig!.ApplyBodyForce(BONES[foot.Bone], bounded, dt);
    }

    private void FinishStep()
    {
        ++landings;
        float rise = m_footEnd.Y - m_footStart.Y;
        if (rise > 0.09f) ++stepsUp;
        if (rise < -0.09f) ++stepsDown;
        m_supportHeight = (m_feet[0].Goal.Y + m_feet[1].Goal.Y) * 0.5f - TOE_HEIGHT + 0.002f;
        m_swing = null;
        if (m_phase == Phase.TRIP && !m_catchStep)
        {
            ++tripRecoveries;
            Enter(Phase.GROUND);
        }
    }

    private void AbortStep(Foot foot, RagdollBodyState observed)
    {
        foot.Goal = observed.Position;
        m_swing = null;
        if (m_phase == Phase.TRIP) Enter(Phase.GROUND);
    }

    private bool CrossesTripBar(Vector3 from, Vector3 to)
    {
        /// @note 探す距離は歩幅の定数ではなく、足場を探した後の実際の足先の移動距離。
        Vector3 motion = Horizontal(to - from);
        Vector3 direction = motion.NormalizedOr(Vector3.ZERO);
        return direction.LengthSquared > 0f && Physics.Raycast(
            from + direction * 0.10f - Vector3.UP * 0.015f, direction, motion.Length, m_floorQuery, out var hit) &&
            hit.GameObject.Name == "TripBar";
    }

    private bool DetectTrip(RagdollBodyState foot, Vector3 direction)
    {
        for (int i = 0; i < foot.ContactCount; ++i)
        {
            var contact = m_rig!.GetBodyContact(BONES[m_swing!.Bone], i);
            if (contact.Valid && !contact.Self && !contact.Dynamic && MathF.Abs(contact.Normal.Y) < 0.55f)
                return true;
        }
        Vector3 travel = direction.NormalizedOr(Yaw(m_yaw) * Vector3.FORWARD);
        return Physics.SphereCast(foot.Position - Vector3.UP * 0.018f + travel * 0.10f,
            0.028f, travel, 0.045f, m_floorQuery, out var hit) && hit.GameObject.Name == "TripBar";
    }

    private void StartTrip(Foot foot, RagdollBodyState observed)
    {
        ++trips;
        m_tripCooldown = 1.5f;
        m_tripObstacle = false;
        m_catchStep = true;
        Vector3 direction = Yaw(m_yaw) * Vector3.FORWARD;
        Vector3 across = observed.Position + direction * 0.31f;
        if (!GroundAt(across, out float floor))
        {
            AbortStep(foot, observed);
            return;
        }
        /// @note 引っ掛かった足を桟の向こうへ逃がし、接地を確認してから反対側の足を出す。
        m_footStart = observed.Position;
        m_footEnd = new Vector3(across.X, floor + TOE_HEIGHT - 0.002f, across.Z);
        m_footRotationStart = observed.Rotation;
        m_yawStart = m_yawEnd = m_yaw;
        m_stepCenterStart = m_center;
        m_stepCenterEnd = m_center + direction * 0.11f;
        m_stepTime = 0f;
        m_stepLength = 0.39f;
        m_rig!.ApplyBodyImpulse(BONES[TORSO], direction * 6f);
        m_nextRight = foot.Side < 0f;
        Enter(Phase.TRIP);
    }

    private void Jump(RagdollBodyState hips, float dt)
    {
        if (m_phase == Phase.CROUCH)
        {
            float t = Clamp01(m_phaseTime / 0.28f);
            float crouch = 0.10f + 0.16f * MathF.Sin(MathF.PI * Smooth(t));
            Pose(m_center, crouch, 0.10f, 0f, dt);
            if (t < 1f) return;
            foreach (Foot foot in m_feet) Unplant(foot);
            m_jumpFrom = hips.Position.Y;
            m_rig!.SetMuscle(0f, 1f, 1f);
            for (int i = 0; i < BONES.Length; ++i)
                m_rig.ApplyBodyImpulse(BONES[i], Vector3.UP * (Body(i).Mass * 2.8f));
            Enter(Phase.AIRBORNE);
            return;
        }
        if (m_phase == Phase.AIRBORNE)
        {
            jumpHeight = MathF.Max(jumpHeight, hips.Position.Y - m_jumpFrom);
            if (!grounded) ++airborneTicks;
            if ((grounded && m_phaseTime > 0.20f && hips.Velocity.Y <= 0f) || m_phaseTime > 1.0f)
            {
                PlaceFeetBelowBody(hips.Position, m_yaw);
                Enter(Phase.LAND);
            }
            return;
        }
        float tLand = Clamp01(m_phaseTime / 0.55f);
        foreach (Foot foot in m_feet) Plant(foot, Body(foot.Bone));
        m_rig!.SetMuscle(Mix(0.45f, 1f, Smooth(tLand)), 1f, 1f);
        float crouchLanding = 0.10f + 0.19f * (1f - Smooth(tLand));
        Pose(m_center, crouchLanding, 0.11f * (1f - tLand), 0f, dt);
        if (tLand >= 1f && grounded)
        {
            ++jumpLandings;
            Enter(Phase.GROUND);
        }
    }

    private void ToggleGrip()
    {
        if (grabbing || m_reaching) ReleaseHands();
        else if (m_crate?.IsAlive == true && Horizontal(m_crate.Position - m_center).Length < 1.12f)
            m_reaching = true;
    }

    private void Reach(float dt)
    {
        if (m_crate?.IsAlive != true)
        {
            ReleaseHands();
            return;
        }
        m_reach = MathF.Min(1f, m_reach + dt * 2.1f);
        if (m_reach < 1f) return;
        var settings = new RagdollGrabSettings { Frequency = 13f, DampingRatio = 1.1f,
            MaxForce = 420f, MaxReach = 0.23f, BreakForce = 0f, BreakDistance = 0.48f };
        Vector3 leftPoint = CrateHandle(-1f);
        Vector3 rightPoint = CrateHandle(1f);
        if ((Body(8).Position - leftPoint).Length > 0.13f || (Body(14).Position - rightPoint).Length > 0.13f) return;
        if (m_rig!.GrabBody(BONES[8], m_crate, leftPoint, settings) &&
            m_rig.GrabBody(BONES[14], m_crate, rightPoint, settings))
        {
            grabbing = true;
            m_reaching = false;
        }
        else ReleaseHands();
    }

    private Vector3 CrateHandle(float side) => m_crate!.Position + new Vector3(side * 0.23f, 0.38f, -0.295f);

    private void ReleaseHands()
    {
        m_rig!.ReleaseGrab(BONES[8]);
        m_rig.ReleaseGrab(BONES[14]);
        grabbing = false;
        m_reaching = false;
    }

    private void StartFall(bool forward)
    {
        m_fallForward = forward;
        foreach (Foot foot in m_feet) Unplant(foot);
        Quaternion yaw = Yaw(m_yaw);
        Vector3 angular = yaw * Vector3.RIGHT * (forward ? 2.05f : -2.05f);
        Vector3 pivot = (m_feet[0].Goal + m_feet[1].Goal) * 0.5f;
        m_rig!.SetMuscle(0f, 1f, 1f);
        /// @note 足元を支点に各部位へ同じ回転の速度を与えると、胴だけに力を加えるより姿勢を保って倒れる。
        for (int i = 0; i < BONES.Length; ++i)
        {
            var body = Body(i);
            m_rig.ApplyBodyImpulse(BONES[i], Vector3.Cross(angular, body.Position - pivot) * body.Mass);
        }
        m_rig.PushAngular(angular);
        Enter(Phase.FALLING);
    }

    private void Recover(RagdollBodyState hips, RagdollBodyState torso, float dt)
    {
        if (m_phase == Phase.FALLING)
        {
            if (m_phaseTime < 0.62f || !GroundAt(torso.Position, out float floor) ||
                torso.Position.Y > floor + 0.37f) return;
            float chestFacing = (torso.Rotation * Vector3.FORWARD).Y;
            if (MathF.Abs(chestFacing) < 0.30f && m_phaseTime < 1.15f) return;
            m_recoverProne = MathF.Abs(chestFacing) >= 0.30f ? chestFacing < 0f : m_fallForward;
            for (int i = 0; i < BONES.Length; ++i)
            {
                m_fallPose[i] = Body(i);
                m_previous[i] = m_fallPose[i].Position;
            }
            PlaceFeetBelowBody(hips.Position, m_yaw);
            Enter(Phase.RISING);
        }
        float progress = Clamp01(m_phaseTime / 2.05f);
        if (progress is >= 0.22f and < 0.65f &&
            (Body(8).Supported || Body(14).Supported || Body(4).Supported || Body(10).Supported))
            ++riseSupportFrames;
        float press = Smooth(Clamp01(progress / 0.27f));
        float stand = Smooth(Clamp01((progress - 0.42f) / 0.58f));
        float pitch = m_recoverProne
            ? 0.91f * (1f - Smooth(Clamp01(progress / 0.44f))) + 0.56f * MathF.Sin(MathF.PI * Smooth(progress))
            : -0.87f * (1f - Smooth(Clamp01((progress - 0.32f) / 0.36f))) +
                0.40f * MathF.Sin(MathF.PI * Smooth(Clamp01((progress - 0.36f) / 0.64f)));
        /// @note 仰向けでは腰を低く保ち、後ろについた手へ荷重を移してから上体を起こす。
        float crouch = Mix(m_recoverProne ? 0.55f : 0.735f, 0.10f, stand);
        Vector3 forward = Yaw(m_yaw) * Vector3.FORWARD;
        Vector3 leftHand = m_center + forward * (m_recoverProne ? 0.36f : -0.31f) + Yaw(m_yaw) * new Vector3(-0.27f, 0.048f, 0f);
        Vector3 rightHand = leftHand + Yaw(m_yaw) * Vector3.RIGHT * 0.54f;
        m_rig!.SetMuscle(Mix(0.30f, 1f, Smooth(Clamp01(progress / 0.50f))), 1f, 1f);
        Pose(m_center, crouch, pitch, 0f, dt,
            recoveryBlend: press, supportHands: press * (1f - Smooth(Clamp01((progress - 0.40f) / 0.29f))),
            leftHand: leftHand, rightHand: rightHand);
        if (progress > 0.69f)
            foreach (Foot foot in m_feet) Plant(foot, Body(foot.Bone));
        if (progress >= 1f && grounded && hips.Position.Y >= m_center.Y + 0.83f &&
            (torso.Rotation * Vector3.UP).Y > 0.85f)
        {
            if (m_recoverProne) ++proneRecoveries;
            else ++supineRecoveries;
            Enter(Phase.GROUND);
        }
    }

    private void PlaceFeetBelowBody(Vector3 hips, float yaw)
    {
        m_center = new Vector3(hips.X, m_center.Y, hips.Z);
        float height = 0f;
        foreach (Foot foot in m_feet)
        {
            Vector3 point = m_center + Yaw(yaw) * new Vector3(0.13f * foot.Side, 0f, 0.06f - 0.035f * foot.Side);
            if (!GroundAt(point, out float floor)) floor = m_center.Y;
            point.Y = floor + TOE_HEIGHT - 0.002f;
            foot.Goal = point;
            foot.Rotation = Yaw(yaw);
            height += floor * 0.5f;
        }
        m_center.Y = height;
        m_supportHeight = height;
    }

    private void Pose(Vector3 center, float crouch, float pitch, float roll, float dt,
        float recoveryBlend = 1f,
        float supportHands = 0f, Vector3 leftHand = default, Vector3 rightHand = default)
    {
        if (m_phase == Phase.GROUND || m_phase == Phase.TRIP)
        {
            m_center.Y = Mix(m_center.Y, m_supportHeight, MathF.Min(1f, dt * 7f));
            center.Y = m_center.Y;
        }
        if (!grabbing && !m_reaching) m_reach = MathF.Max(0f, m_reach - dt * 3f);
        Quaternion yaw = Yaw(m_yaw);
        Quaternion torsoRotation = yaw * Quaternion.FromAxisAngle(Vector3.RIGHT, pitch) *
            Quaternion.FromAxisAngle(Vector3.FORWARD, roll);
        var hips = center + Vector3.UP * (HIP_HEIGHT - crouch);
        var torso = hips + torsoRotation * new Vector3(0f, 0.275f, 0f);
        Aim(HIPS, hips, yaw);
        Aim(TORSO, torso, torsoRotation);
        Aim(HEAD, torso + torsoRotation * new Vector3(0f, 0.295f, 0f), Quaternion.Slerp(torsoRotation, yaw, 0.65f));
        for (int i = 0; i < m_feet.Length; ++i)
        {
            Foot foot = m_feet[i];
            float side = foot.Side;
            int thigh = foot.Bone - 2;
            Vector3 hipJoint = hips + yaw * new Vector3(side * 0.125f, -0.055f, 0f);
            Vector3 ankle = foot.Goal - foot.Rotation * new Vector3(0f, -0.04f, 0.055f);
            AimLimb(thigh, hipJoint, ankle, yaw * Vector3.FORWARD, yaw,
                new Vector3(0f, -0.42f, 0.05f), new Vector3(0f, -0.415f, -0.046f),
                new Vector3(0f, -0.20f, 0.025f), new Vector3(0f, -0.20f, -0.023f));
            Aim(foot.Bone, foot.Goal, foot.Rotation);

            int upperArm = foot.Bone + 1;
            Vector3 shoulder = torso + torsoRotation * new Vector3(side * 0.235f, 0.13f, 0f);
            Vector3 hand = shoulder + torsoRotation * new Vector3(side * 0.09f, -0.48f,
                m_swing is null ? 0.035f : m_swing.Side * side * 0.12f * MathF.Sin(MathF.PI * Clamp01(m_stepTime / m_stepLength)));
            if (m_crate?.IsAlive == true && m_reach > 0f)
            {
                Vector3 handle = grabbing ? center + yaw * new Vector3(side * 0.23f, 0.86f, 0.40f) : CrateHandle(side);
                hand = Mix(hand, handle, Smooth(m_reach));
            }
            if (supportHands > 0f) hand = Mix(hand, i == 0 ? leftHand : rightHand, supportHands);
            AimLimb(upperArm, shoulder, hand + yaw * new Vector3(0f, 0.04f, -0.02f),
                torsoRotation * new Vector3(side * 0.55f, -0.20f, -1f), torsoRotation,
                new Vector3(side * 0.067f, -0.258f, 0f), new Vector3(side * 0.024f, -0.217f, 0.023f),
                new Vector3(side * 0.035f, -0.123f, 0f), new Vector3(side * 0.012f, -0.102f, 0.012f));
            Aim(upperArm + 2, hand, yaw);
        }

        for (int i = 0; i < BONES.Length; ++i)
        {
            Vector3 pos = m_positions[i];
            Quaternion rot = m_rotations[i];
            if (recoveryBlend < 1f)
            {
                pos = Mix(m_fallPose[i].Position, pos, recoveryBlend);
                rot = Quaternion.Slerp(m_fallPose[i].Rotation, rot, recoveryBlend);
            }
            Vector3 speed = (pos - m_previous[i]) / dt;
            m_rig!.SetBodyTarget(BONES[i], new RagdollBodyTarget
            {
                Position = pos, Rotation = rot, Velocity = speed.NormalizedOr(Vector3.ZERO) * MathF.Min(4f, speed.Length),
            });
            m_previous[i] = pos;
        }
    }

    /// @note 二節の長さと曲げる側を保ち、関節位置から剛体重心への変換を行う。
    private void AimLimb(int upper, Vector3 start, Vector3 end, Vector3 bend, Quaternion frame,
        Vector3 upperRest, Vector3 lowerRest, Vector3 upperCenter, Vector3 lowerCenter)
    {
        float first = upperRest.Length;
        float second = lowerRest.Length;
        Vector3 delta = end - start;
        Vector3 axis = delta.NormalizedOr(frame * -Vector3.UP);
        float distance = Math.Clamp(delta.Length, MathF.Abs(first - second) + 0.008f, first + second - 0.006f);
        Vector3 pole = (bend - axis * Vector3.Dot(bend, axis)).NormalizedOr(frame * Vector3.RIGHT);
        float along = (first * first - second * second + distance * distance) / (2f * distance);
        Vector3 middle = start + axis * along + pole * MathF.Sqrt(MathF.Max(0f, first * first - along * along));
        Quaternion upperRotation = Align(frame, upperRest, middle - start);
        Quaternion lowerRotation = Align(frame, lowerRest, start + axis * distance - middle);
        Aim(upper, start + upperRotation * upperCenter, upperRotation);
        Aim(upper + 1, middle + lowerRotation * lowerCenter, lowerRotation);
    }

    private static Quaternion Align(Quaternion rest, Vector3 original, Vector3 target)
    {
        Vector3 a = (rest * original).NormalizedOr(-Vector3.UP);
        Vector3 b = target.NormalizedOr(a);
        float dot = Math.Clamp(Vector3.Dot(a, b), -1f, 1f);
        Vector3 axis = Vector3.Cross(a, b);
        if (axis.LengthSquared < 0.000001f)
            return dot > 0f ? rest : Quaternion.FromAxisAngle(rest * Vector3.RIGHT, MathF.PI) * rest;
        return Quaternion.FromAxisAngle(axis, MathF.Acos(dot)) * rest;
    }

    private void Aim(int bone, Vector3 position, Quaternion rotation)
    {
        m_positions[bone] = position;
        m_rotations[bone] = rotation;
    }

    private bool Plant(Foot foot, RagdollBodyState body)
    {
        if (foot.Planted) return true;
        if (!body.Supported || Horizontal(body.Position - foot.Goal).Length > 0.105f ||
            MathF.Abs(body.Position.Y - foot.Goal.Y) > 0.06f) return false;
        float expectedGround = foot.Goal.Y - TOE_HEIGHT + 0.002f;
        Vector3 point = Vector3.ZERO;
        int contacts = 0;
        for (int i = 0; i < body.ContactCount; ++i)
        {
            var hit = m_rig!.GetBodyContact(BONES[foot.Bone], i);
            if (!hit.Valid || hit.Self || hit.Dynamic || hit.Normal.Y < 0.72f ||
                MathF.Abs(hit.Point.Y - expectedGround) > 0.025f) continue;
            point += hit.Point;
            ++contacts;
        }
        if (contacts == 0) return false;
        point /= contacts;
        var settings = new RagdollGrabSettings { Frequency = 110f, DampingRatio = 1.15f,
            MaxForce = 4500f, MaxTorque = 260f, ConstrainRotation = true, BreakForce = 0f, MaxReach = 0.29f };
        if (!m_rig!.GrabWorld(BONES[foot.Bone], point, settings)) return false;
        foot.Planted = true;
        foot.Anchor = point;
        foot.LocalAnchor = body.Rotation.Inverse() * (point - body.Position);
        foot.Goal = new Vector3(body.Position.X, foot.Goal.Y, body.Position.Z);
        foot.Rotation = body.Rotation;
        return true;
    }

    private void Unplant(Foot foot)
    {
        if (!foot.Planted) return;
        m_rig!.ReleaseGrab(BONES[foot.Bone]);
        foot.Planted = false;
    }

    private void TrackSupport(Foot foot, RagdollBodyState body)
    {
        if (!foot.Planted) return;
        Vector3 point = body.Position + body.Rotation * foot.LocalAnchor;
        maxSupportSlip = MathF.Max(maxSupportSlip, Horizontal(point - foot.Anchor).Length);
    }

    private void TrackCrate()
    {
        if (!grabbing || m_crate?.IsAlive != true) return;
        float progress = m_crate.Position.Z - m_crateOrigin.Z;
        crateForward = MathF.Max(crateForward, progress);
        cratePullBack = MathF.Max(cratePullBack, crateForward - progress);
    }

    private bool GroundAt(Vector3 target, out float ground)
    {
        Vector3 top = new(target.X, MathF.Max(m_center.Y, target.Y - 0.15f) + 0.36f, target.Z);
        bool found = Physics.Raycast(top, -Vector3.UP, 0.76f, m_floorQuery, out var hit) &&
            hit.Normal.Y >= 0.72f && hit.GameObject.Name != "PracticeCrate";
        ground = found ? hit.Point.Y : 0f;
        return found;
    }

    /// @note 中心だけが段に乗ると足裏が縁を支点に回る。踵とつま先にも同じ高さの足場を要求する。
    private bool TryFoothold(Vector3 nominal, Vector3 direction, Quaternion rotation, float stanceHeight, out Vector3 target)
    {
        Vector3 halfLength = rotation * Vector3.FORWARD * 0.115f;
        Vector3 halfWidth = rotation * Vector3.RIGHT * 0.055f;
        foreach (float distance in FOOT_SEARCH)
        {
            Vector3 candidate = nominal + direction * distance;
            if (!GroundAt(candidate, out float floor) || MathF.Abs(floor - stanceHeight) > 0.22f ||
                !SameFloor(candidate + halfLength + halfWidth, floor) ||
                !SameFloor(candidate + halfLength - halfWidth, floor) ||
                !SameFloor(candidate - halfLength + halfWidth, floor) ||
                !SameFloor(candidate - halfLength - halfWidth, floor)) continue;
            target = new Vector3(candidate.X, floor + TOE_HEIGHT - 0.002f, candidate.Z);
            return true;
        }
        target = default;
        return false;
    }

    private bool SameFloor(Vector3 point, float floor) =>
        GroundAt(point, out float height) && MathF.Abs(height - floor) <= 0.024f;

    private void UpdateCamera(Vector3 hips, float dt)
    {
        if (m_camera is null || !m_camera.IsAlive) return;
        Vector3 target = new(hips.X, 0.79f + m_center.Y, hips.Z);
        m_lastCameraTarget = Mix(m_lastCameraTarget, target, 1f - MathF.Exp(-dt * 4f));
        Vector3 position = m_lastCameraTarget + new Vector3(2.9f, 1.22f, -3.55f);
        m_camera.WorldPosition = position;
        m_camera.LocalRotation = Quaternion.LookRotation(m_lastCameraTarget - position);
    }

    private void Enter(Phase phase)
    {
        m_phase = phase;
        m_phaseTime = 0f;
        state = phase switch
        {
            Phase.CROUCH => "crouching", Phase.AIRBORNE => "airborne", Phase.LAND => "landing",
            Phase.FALLING => "falling", Phase.RISING => "rising", Phase.TRIP => "tripping", _ => "idle"
        };
    }

    private RagdollBodyState Body(int bone) => m_rig!.GetBodyState(BONES[bone]);
    private float Axis(KeyCode positive, KeyCode negative) => (Input.GetKey(positive) ? 1f : 0f) - (Input.GetKey(negative) ? 1f : 0f);

    private bool Pressed(KeyCode key, ref bool wasDown)
    {
        bool down = Input.GetKey(key);
        bool pressed = down && !wasDown;
        wasDown = down;
        return pressed;
    }

    private static Vector3 Horizontal(Vector3 v) => new(v.X, 0f, v.Z);
    private static Quaternion Yaw(float radians) => Quaternion.FromAxisAngle(Vector3.UP, radians);
    private static float Clamp01(float x) => Math.Clamp(x, 0f, 1f);
    private static float Smooth(float t) => t * t * t * (10f + t * (6f * t - 15f));
    private static float Mix(float a, float b, float t) => a + (b - a) * t;
    private static Vector3 Mix(Vector3 a, Vector3 b, float t) => a + (b - a) * t;
}
