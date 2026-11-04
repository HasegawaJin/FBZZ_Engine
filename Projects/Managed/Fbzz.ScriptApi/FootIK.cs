/// @file    FootIK.cs
/// @brief   足接地と腰補正を調整し、実際の補正量を照会する公開 SDK。
/// @author  Hasegawa Jin
/// @date    2026-11-03
namespace Fbzz;

/// @brief 距離は world m、時間は s。new() は腰を人工的に下げない標準設定。
public struct FootIKSettings
{
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)] public bool AdjustHip;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)] public bool UseAnimatorIKWeight;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="leg_length_ratio")] public float RayUpRatio;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="leg_length_ratio")] public float RayDownRatio;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m", Summary="Ankle pivot height above the surface; calibrate for each rig. Range 0..0.5.")] public float SurfaceOffset;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m")] public float DeadZone;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m")] public float MaxCorrection;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m")] public float PlantDistance;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="s", Summary="Exponential response time constant; zero applies immediately. Range 0..1.")] public float SmoothTime;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="0_to_1")] public float HipWeight;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m", Summary="Maximum downward hip correction, 0..1. Zero prevents lowering.")] public float MaxHipLowering;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m", Summary="Maximum upward hip correction, 0..1. Zero prevents raising.")] public float MaxHipRaising;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m", Summary="Explicit extra knee-bend reserve, 0..0.25. Zero preserves FK knee bend.")] public float BendReserve;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Summary="Uses the same actor's ragdoll shape and sole orientation; otherwise falls back to SurfaceOffset.")] public bool UseRagdollFootShape;
    public FootIKSettings()
    {
        AdjustHip = UseAnimatorIKWeight = UseRagdollFootShape = true;
        RayUpRatio = 0.5f; RayDownRatio = 1.2f; SurfaceOffset = 0.05f;
        DeadZone = 0.025f; MaxCorrection = 0.12f; PlantDistance = 0.06f; SmoothTime = 0.10f;
        HipWeight = 1; MaxHipLowering = 0.10f; MaxHipRaising = BendReserve = 0;
    }
}

/// @brief 前回の IK 更新結果。HipOffset は実際に腰へ適用した world Y 変位。
public struct FootIKState
{
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)] public bool Valid;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)] public bool LeftGrounded;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)] public bool RightGrounded;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)] public bool HipLimited;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)] public bool UsesPhysicsShape;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m")] public float LeftCorrection;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m")] public float RightCorrection;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m")] public float HipTarget;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="m")] public float HipOffset;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="0_to_1")] public float LeftPlantWeight;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED, Units="0_to_1")] public float RightPlantWeight;
}

public sealed partial class IKSolver
{
    [ApiContract(RunModes=new[]{"Edit","Play"}, Call="query", Failure="missing_or_nonfoot_chain_returns_false")]
    public bool TryGetFootSettings(string targetName, out FootIKSettings settings)
    {
        var r = Invoke(Service, 10, true, text: [targetName]);
        settings = new() { AdjustHip=(r.Integer&2)!=0, UseAnimatorIKWeight=(r.Integer&4)!=0, UseRagdollFootShape=(r.Integer&8)!=0,
            RayUpRatio=r.Value(0), RayDownRatio=r.Value(1), SurfaceOffset=r.Value(2), DeadZone=r.Value(3),
            MaxCorrection=r.Value(4), PlantDistance=r.Value(5), SmoothTime=r.Value(6), HipWeight=r.Value(7),
            MaxHipLowering=r.Value(8), MaxHipRaising=r.Value(9), BendReserve=r.Value(10) };
        return (r.Integer&1)!=0;
    }
    [ApiContract(RunModes=new[]{"Play"}, Call="live", Coordinate="world", Units="m;s;ratio",
        Summary="Atomically adjusts a named FootPlace chain without resetting smoothing. Distances 0..1 m except SurfaceOffset 0..0.5, DeadZone/BendReserve 0..0.25; ratios up 0..2/down 0..4; SmoothTime/HipWeight 0..1.",
        Failure="invalid_or_nonfinite_input_throws_without_changes; missing_or_nonfoot_chain_returns_false")]
    public bool SetFootSettings(string targetName, FootIKSettings settings) => Invoke(Service, 11, false,
        (settings.AdjustHip?1:0)|(settings.UseAnimatorIKWeight?2:0)|(settings.UseRagdollFootShape?4:0),
        [settings.RayUpRatio,settings.RayDownRatio,settings.SurfaceOffset,settings.DeadZone,settings.MaxCorrection,
         settings.PlantDistance,settings.SmoothTime,settings.HipWeight,settings.MaxHipLowering,settings.MaxHipRaising,settings.BendReserve],
        [targetName]).Flag;
    [ApiContract(RunModes=new[]{"Edit","Play"}, Call="query", Coordinate="world", Units="m",
        Summary="Reads the last evaluated corrections, support and hip limit state; disabled solvers report zero after the next IK update.",
        Failure="missing_or_nonfoot_chain_returns_Valid_false")]
    public FootIKState GetFootState(string targetName)
    {
        var r = Invoke(Service, 12, true, text:[targetName]);
        return new() { Valid=(r.Integer&1)!=0, LeftGrounded=(r.Integer&2)!=0, RightGrounded=(r.Integer&4)!=0,
            HipLimited=(r.Integer&8)!=0, UsesPhysicsShape=(r.Integer&16)!=0, LeftCorrection=r.Value(0), RightCorrection=r.Value(1),
            HipTarget=r.Value(2), HipOffset=r.Value(3), LeftPlantWeight=r.Value(4), RightPlantWeight=r.Value(5) };
    }
}
