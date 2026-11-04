/// @file    RagdollControls.cs
/// @brief   部位目標・外力・支持観測・独立把持の公開API。
/// @author  Hasegawa Jin
/// @date    2026-11-03
using System;
using System.Globalization;
namespace Fbzz;

/// @note Positionは剛体重心。速度はSpaceと同じ空間のm/s・rad/s。両Weightは0〜1。
public struct RagdollBodyTarget
{
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public RagdollDriveSpace Space;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public Vector3 Position, Velocity, AngularVelocity;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public Quaternion Rotation;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public float PositionWeight, RotationWeight;
    [ApiContract(Call="any", Purity=ApiPurity.MANAGED)]
    public RagdollBodyTarget()
    { Space=RagdollDriveSpace.World; Position=Velocity=AngularVelocity=Vector3.ZERO; Rotation=Quaternion.IDENTITY; PositionWeight=RotationWeight=1; }
}
public struct RagdollBodyState
{
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public bool Valid, Supported;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public float Mass;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public int ContactCount;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public Vector3 Position, Velocity, AngularVelocity;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public Quaternion Rotation;
}
public struct RagdollBodyContact
{
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public bool Valid, Self, Dynamic;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public Vector3 Point, Normal, OtherVelocity;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public float Penetration;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public string OtherBone;
}
/// @note ワールド空間の観測。Marginは支持凸包の内側が正、外側が負。CapturePointは一定重心高さの近似。
public struct RagdollBalanceState
{
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public bool Valid, HasSupport, CaptureValid;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public float Mass, CenterMargin, CaptureMargin, Gravity, Height;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public int ContactCount;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public Vector3 Center, Velocity, SupportPoint, SupportNormal, SupportVelocity, CapturePoint;
}
public enum RagdollGrabStatus { None, Holding, Released, ForceLimit, DistanceLimit, TargetLost }
/// @note 周波数Hz、減衰比、上限N/N·m、切断N/m、到達距離m。BreakForce/Distanceの0は切断を無効にする。
public struct RagdollGrabSettings
{
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public float Frequency, DampingRatio, MaxForce, MaxTorque, BreakForce, BreakDistance, MaxReach;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public bool ConstrainRotation;
    [ApiContract(Call="any", Purity=ApiPurity.MANAGED)]
    public RagdollGrabSettings()
    { Frequency=8; DampingRatio=1; MaxForce=1000; MaxTorque=100; BreakForce=2000; BreakDistance=0.5f; MaxReach=0.35f; ConstrainRotation=false; }
}
public struct RagdollGrabState
{
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public RagdollGrabStatus Status;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public Vector3 Anchor, Target;
    [ApiContract(Read="any", Write="any", Purity=ApiPurity.MANAGED)]
    public float Force;
}
public sealed partial class Ragdoll
{
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", ParameterUnits=new[]{"substeps=count","linearDrag=1/s","angularDrag=1/s"},
        Summary="Sets 1..64 substeps, nonnegative linear/angular damping, and supported passive rest. Changes apply on the next physical solve.",
        Failure="invalid_or_nonfinite_input_throws_without_changes")]
    public void SetSimulation(int substeps,float linearDrag,float angularDrag,bool allowRest=true)
    {
        if (substeps<1 || substeps>64) throw new System.ArgumentOutOfRangeException(nameof(substeps));
        Invoke(Service,67,false,substeps|(allowRest?128:0),values:[linearDrag,angularDrag]);
    }
    static Vector3 ReadVector(NativeServiceResult r,int i)=>new(r.Value(i),r.Value(i+1),r.Value(i+2));
    [ApiContract(RunModes=new[]{"Edit","Play"}, Call="query", Summary="Returns the built body's bone name, or empty if the index is unavailable.")]
    public string GetBodyName(int index)=>Invoke(Service,66,true,index).String;
    [ApiContract(RunModes=new[]{"Edit","Play"}, Call="query", Units="kg;m;m/s;rad/s", Coordinate="world", Summary="Returns a snapshot of the physical body's center of mass; Valid is false before construction or for missing names.")]
    public RagdollBodyState GetBodyState(string bone)
    {
        var r=Invoke(Service,50,true,text:[bone]);
        return new(){Valid=(r.Integer&1)!=0,Supported=(r.Integer&2)!=0,Mass=r.Value(0),ContactCount=(int)r.Value(1),
            Position=ReadVector(r,2),Rotation=new(r.Value(5),r.Value(6),r.Value(7),r.Value(8)),
            Velocity=ReadVector(r,9),AngularVelocity=ReadVector(r,12)};
    }
    [ApiContract(RunModes=new[]{"Edit","Play"}, Call="query", Coordinate="world", Units="m;m/s", Summary="Enumerates current physical contacts; missing indices return Valid=false. Self contacts never count as support.")]
    public RagdollBodyContact GetBodyContact(string bone,int index)
    {
        var r=Invoke(Service,62,true,index,text:[bone]);
        return new(){Valid=(r.Integer&1)!=0,Self=(r.Integer&2)!=0,Dynamic=(r.Integer&4)!=0,Point=ReadVector(r,0),
            Normal=ReadVector(r,3),OtherVelocity=ReadVector(r,6),Penetration=r.Value(9),OtherBone=r.String};
    }
    [ApiContract(RunModes=new[]{"Edit","Play"}, Call="query", Summary="Reads authored or overridden drive settings; returns false for an unavailable body.")]
    public bool TryGetBodyDrive(string bone,out RagdollDriveSettings settings)
    {
        var r=Invoke(Service,65,true,text:[bone]);
        settings=new(){Enabled=(r.Integer&2)!=0,Space=(r.Integer&4)!=0?RagdollDriveSpace.World:RagdollDriveSpace.Parent,
            LinearFrequency=r.Value(0),AngularFrequency=r.Value(1),LinearDampingRatio=r.Value(2),AngularDampingRatio=r.Value(3),
            MaxForce=r.Value(4),MaxTorque=r.Value(5),TargetVelocityMultiplier=r.Value(6)};
        return (r.Integer&1)!=0;
    }
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", Summary="Blends a manual COM pose/velocity with animation in the selected space. Requires an enabled body drive and Active mode; StandingGuard still limits the final pose. Cleared on rebuild.", Failure="invalid_input_throws_without_changes; missing_body_returns_false")]
    public bool SetBodyTarget(string bone,RagdollBodyTarget target)=>Invoke(Service,51,false,RuntimeComponentValue.EnumValue(target.Space),
        [target.Position.X,target.Position.Y,target.Position.Z,target.Rotation.X,target.Rotation.Y,target.Rotation.Z,target.Rotation.W,
         target.Velocity.X,target.Velocity.Y,target.Velocity.Z,target.AngularVelocity.X,target.AngularVelocity.Y,target.AngularVelocity.Z,
         target.PositionWeight,target.RotationWeight],[bone]).Flag;
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", Summary="Restores animation targets and the configured drive space.")]
    public bool ClearBodyTarget(string bone)=>Invoke(Service,52,false,text:[bone]).Flag;
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed")]
    public void ClearBodyTargets()=>Invoke(Service,63,false);
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", Units="N*s", Coordinate="world")]
    public bool ApplyBodyImpulse(string bone,Vector3 impulse)=>Invoke(Service,53,false,values:[impulse.X,impulse.Y,impulse.Z],text:[bone]).Flag;
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", Units="N*s;m", Coordinate="world")]
    public bool ApplyBodyImpulseAt(string bone,Vector3 impulse,Vector3 point)=>Invoke(Service,53,false,1,
        [impulse.X,impulse.Y,impulse.Z,point.X,point.Y,point.Z],[bone]).Flag;
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", Units="N*m*s", Coordinate="world")]
    public bool ApplyBodyAngularImpulse(string bone,Vector3 impulse)=>Invoke(Service,64,false,values:[impulse.X,impulse.Y,impulse.Z],text:[bone]).Flag;
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", Units="N;s", Coordinate="world", Summary="Adds a force for positive duration seconds of simulated time. Repeated calls add forces; duration is independent of solver substeps.")]
    public bool ApplyBodyForce(string bone,Vector3 force,float duration)=>Invoke(Service,54,false,values:[force.X,force.Y,force.Z,duration],text:[bone]).Flag;
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", Units="N;m;s", Coordinate="world")]
    public bool ApplyBodyForceAt(string bone,Vector3 force,Vector3 point,float duration)=>Invoke(Service,54,false,1,
        [force.X,force.Y,force.Z,point.X,point.Y,point.Z,duration],[bone]).Flag;
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", Units="N*m;s", Coordinate="world")]
    public bool ApplyBodyTorque(string bone,Vector3 torque,float duration)=>Invoke(Service,55,false,values:[torque.X,torque.Y,torque.Z,duration],text:[bone]).Flag;
    [ApiContract(RunModes=new[]{"Edit","Play"}, Call="query", Units="m;kg;m/s", Coordinate="world", Summary="Reports COM, support convex hull margins and a constant-height capture point approximation; no automatic balance forces. Point/line support uses padding; airborne or zero gravity has CaptureValid=false.")]
    public RagdollBalanceState GetBalanceState(float padding=0.02f,float minSupportDot=0.5f)
    {
        var r=Invoke(Service,56,true,values:[padding,minSupportDot]);
        return new(){Valid=(r.Integer&1)!=0,HasSupport=(r.Integer&2)!=0,CaptureValid=(r.Integer&4)!=0,
            Mass=r.Value(0),ContactCount=(int)r.Value(1),CenterMargin=r.Value(2),CaptureMargin=r.Value(3),Gravity=r.Value(4),Height=r.Value(5),
            Center=ReadVector(r,6),Velocity=ReadVector(r,9),SupportPoint=ReadVector(r,12),SupportNormal=ReadVector(r,15),
            SupportVelocity=ReadVector(r,18),CapturePoint=ReadVector(r,21)};
    }
    static float[] GrabValues(Vector3 point,RagdollGrabSettings s)=>[point.X,point.Y,point.Z,s.Frequency,s.DampingRatio,s.MaxForce,s.MaxTorque,s.BreakForce,s.BreakDistance,s.MaxReach];
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", Coordinate="world", ParameterUnits=new[]{"point=m"}, Summary="Grips a fixed world point independently for each bone. Positive finite force caps; preserves previous grip on failed reach or missing body. Passive keeps grips.")]
    public bool GrabWorld(string bone,Vector3 point,RagdollGrabSettings settings)=>Invoke(Service,57,false,settings.ConstrainRotation?1:0,GrabValues(point,settings),[bone]).Flag;
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed", Coordinate="world", Lifetime="exact_current_scene_component", Summary="Grips a current scene RigidBody with equal opposite impulses. Owner contacts of dynamic held bodies resume after the last grip releases and all shapes separate by 2 cm; static supports and other world contacts remain. Removal releases with TargetLost; rebuilding or stopping the owning ragdoll releases all grips.")]
    public bool GrabBody(string bone,RigidBody target,Vector3 point,RagdollGrabSettings settings)
    {
        ArgumentNullException.ThrowIfNull(target);
        return Invoke(Service,58,false,settings.ConstrainRotation?1:0,GrabValues(point,settings),
            [bone,target.LifetimeToken.ToString(CultureInfo.InvariantCulture)],target.GameObject).Flag;
    }
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed")]
    public bool ReleaseGrab(string bone)=>Invoke(Service,59,false,text:[bone]).Flag;
    [ApiContract(RunModes=new[]{"Play"}, Call="fixed")]
    public void ReleaseAllGrabs()=>Invoke(Service,60,false);
    [ApiContract(RunModes=new[]{"Edit","Play"}, Call="query", Coordinate="world", Units="m;N", Summary="Returns holding/release reason, current anchors and the last substep force.")]
    public RagdollGrabState GetGrabState(string bone)
    {
        var r=Invoke(Service,61,true,text:[bone]);
        return new(){Status=(RagdollGrabStatus)r.Int32,Anchor=ReadVector(r,0),Target=ReadVector(r,3),Force=r.Value(6)};
    }
}
public static class RagdollBalanceUtility
{
    /// @note 支持面内の有限PD加速度だけを計算する。接地・重力補償・適用部位・出力配分は呼出側が決める。
    [ApiContract(Call="any", Purity=ApiPurity.MANAGED, Units="m/s^2", Coordinate="world", Summary="Computes bounded planar PD acceleration relative to the moving support. Returns zero without support; does not apply forces.")]
    public static Vector3 ComputeAcceleration(RagdollBalanceState state,Vector3 desiredPoint,float frequency=2,float dampingRatio=1,float maxAcceleration=20)
    {
        if(!desiredPoint.IsFinite||!float.IsFinite(frequency)||frequency<0||!float.IsFinite(dampingRatio)||dampingRatio<0
            ||!float.IsFinite(maxAcceleration)||maxAcceleration<0)throw new ArgumentOutOfRangeException(nameof(frequency));
        if(!state.Valid||!state.HasSupport)return Vector3.ZERO;
        if(!state.Center.IsFinite||!state.Velocity.IsFinite||!state.SupportVelocity.IsFinite||!state.SupportNormal.IsFinite)
            throw new ArgumentOutOfRangeException(nameof(state));
        /// @note floatの有限入力でも差分・周波数二乗はoverflowし得るため、上限制限までdoubleで評価する。
        double nx=state.SupportNormal.X,ny=state.SupportNormal.Y,nz=state.SupportNormal.Z;
        double norm=Math.Sqrt(nx*nx+ny*ny+nz*nz);
        if(norm<1e-8){nx=0;ny=1;nz=0;}else{nx/=norm;ny/=norm;nz/=norm;}
        double ex=(double)desiredPoint.X-state.Center.X,ey=(double)desiredPoint.Y-state.Center.Y,ez=(double)desiredPoint.Z-state.Center.Z;
        double vx=(double)state.Velocity.X-state.SupportVelocity.X,vy=(double)state.Velocity.Y-state.SupportVelocity.Y,vz=(double)state.Velocity.Z-state.SupportVelocity.Z;
        double normalError=ex*nx+ey*ny+ez*nz,normalSpeed=vx*nx+vy*ny+vz*nz;
        double omega=2*Math.PI*frequency,k=omega*omega,damping=2*dampingRatio*omega;
        double ax=(ex-nx*normalError)*k-(vx-nx*normalSpeed)*damping;
        double ay=(ey-ny*normalError)*k-(vy-ny*normalSpeed)*damping;
        double az=(ez-nz*normalError)*k-(vz-nz*normalSpeed)*damping;
        double length=Math.Sqrt(ax*ax+ay*ay+az*az),scale=length>maxAcceleration ? maxAcceleration/length : 1;
        return new((float)(ax*scale),(float)(ay*scale),(float)(az*scale));
    }
}
