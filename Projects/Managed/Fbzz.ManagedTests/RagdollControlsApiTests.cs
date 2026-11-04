/// @file    RagdollControlsApiTests.cs
/// @brief   部位操作packet・観測復号・バランス補助と公開契約の回帰。
/// @author  Hasegawa Jin
/// @date    2026-11-03
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;
namespace Fbzz.ManagedTests;
internal static class RagdollControlsApiTests
{
    internal static IReadOnlyList<(string Name,Action Run)> Cases=>[
        (nameof(BodyTargetsAndGripsTransferExactFramesAndLifetime),BodyTargetsAndGripsTransferExactFramesAndLifetime),
        (nameof(PhysicalSnapshotsDecodeFullValuePackets),PhysicalSnapshotsDecodeFullValuePackets),
        (nameof(BalanceHelperIsPlanarBoundedAndRelativeToSupport),BalanceHelperIsPlanarBoundedAndRelativeToSupport),
        (nameof(ControlCatalogPreservesFixedWritesAndPureMath),ControlCatalogPreservesFixedWritesAndPureMath)];
    static void BodyTargetsAndGripsTransferExactFramesAndLifetime()
    {
        using Fixture f=new();Ragdoll r=new(f.Target,91);f.Bridge.Reply=new([],[],1,null);
        Program.Check(r.SetBodyTarget("Hand",new(){Position=new(1,2,3),PositionWeight=0.25f,RotationWeight=0.75f})
            &&f.Bridge.Selector==51&&f.Bridge.Integer==1&&f.Bridge.Values.Length==15
            &&f.Bridge.Values[6]==1&&f.Bridge.Values[13]==0.25f,"Body target weights and World quaternion must retain packet order.");
        RigidBody target=new(f.Other,72);
        Program.Check(r.GrabBody("Hand",target,Vector3.UP,new(){ConstrainRotation=true})&&f.Bridge.Selector==58
            &&f.Bridge.Text.SequenceEqual(new[]{"Hand","72"})&&f.Bridge.Related==f.Other
            &&f.Bridge.Values.Length==10,"Grip must retain the target's exact birth token and scene identity.");
        int calls=f.Bridge.Calls;
        Program.Throws<ArgumentOutOfRangeException>(()=>r.SetBodyTarget("Hand",new(){Space=(RagdollDriveSpace)9}));
        Program.Throws<ArgumentOutOfRangeException>(()=>r.ApplyBodyForce("Hand",new(float.NaN,0,0),1));
        Program.Check(f.Bridge.Calls==calls,"Malformed packets must not dispatch.");
        r.SetSimulation(24,0.5f,3,false);
        Program.Check(f.Bridge.Selector==67&&f.Bridge.Integer==24&&f.Bridge.Values.Length==2,"Simulation settings must retain exact substeps and damping.");
        r.SetSimulation(12,0.5f,1.5f);
        Program.Check(f.Bridge.Integer==140,"Supported rest must remain independent of the substep count.");
        Program.Throws<ArgumentOutOfRangeException>(()=>r.SetSimulation(65,0,0));
        f.Bridge.Birth=92;Program.Throws<InvalidOperationException>(()=>r.ReleaseAllGrabs());
    }
    static void PhysicalSnapshotsDecodeFullValuePackets()
    {
        using Fixture f=new();Ragdoll r=new(f.Target,91);
        f.Bridge.Reply=new([2,3,4,5,6,0,0,0,1,7,8,9,10,11,12],[],3,null);
        var body=r.GetBodyState("Hand");
        Program.Check(body.Valid&&body.Supported&&body.Mass==2&&body.ContactCount==3&&body.Position.X==4
            &&body.Velocity.X==7&&body.AngularVelocity.Z==12,"COM and angular velocity must not be truncated to the inline eight-value packet.");
        f.Bridge.Reply=new(Enumerable.Range(0,24).Select(i=>(float)i).ToArray(),[],7,null);
        var balance=r.GetBalanceState();
        Program.Check(balance.Valid&&balance.HasSupport&&balance.CaptureValid&&balance.SupportVelocity.Z==20&&balance.CapturePoint.Z==23,
            "Balance must decode all 24 values and three independent validity flags.");
        f.Bridge.Reply=new([1,2,3,0,1,0,4,5,6,0.01f],["Other"],7,null);
        var contact=r.GetBodyContact("Hand",2);
        Program.Check(contact.Valid&&contact.Self&&contact.Dynamic&&contact.OtherBone=="Other"&&contact.OtherVelocity.Y==5,
            "Contact normal, other velocity and self/dynamic flags must remain independent.");
    }
    static void BalanceHelperIsPlanarBoundedAndRelativeToSupport()
    {
        ScriptApiRuntime.Current=null;
        RagdollBalanceState s=new(){Valid=true,HasSupport=true,SupportNormal=Vector3.UP,Center=new(0,1,0),Velocity=new(3,0,0),SupportVelocity=new(3,0,0)};
        var a=RagdollBalanceUtility.ComputeAcceleration(s,new(2,100,0),2,1,5);
        Program.Check(MathF.Abs(a.Length-5)<1e-5f&&a.Y==0&&a.X>0,"Helper must remove vertical error and clamp acceleration.");
        Program.Check(RagdollBalanceUtility.ComputeAcceleration(s,new(0,0,0)).Length<1e-5f,"Moving support velocity must be subtracted.");
        s.Center=new(float.MaxValue,1,0);
        var extreme=RagdollBalanceUtility.ComputeAcceleration(s,new(-float.MaxValue,0,0),float.MaxValue,1,5);
        Program.Check(extreme.IsFinite&&MathF.Abs(extreme.Length-5)<1e-5f,"Finite inputs must remain bounded before float overflow.");
        s.Center=new(float.NaN,0,0);
        Program.Throws<ArgumentOutOfRangeException>(()=>RagdollBalanceUtility.ComputeAcceleration(s,Vector3.ZERO));
        s.HasSupport=false;Program.Check(RagdollBalanceUtility.ComputeAcceleration(s,Vector3.ONE).Length==0,"Airborne observations must not invent support.");
        Program.Throws<ArgumentOutOfRangeException>(()=>RagdollBalanceUtility.ComputeAcceleration(s,Vector3.ZERO,-1));
    }
    static void ControlCatalogPreservesFixedWritesAndPureMath()
    {
        using JsonDocument doc=JsonDocument.Parse(ApiCatalog.Json);
        var types=doc.RootElement.GetProperty("types").EnumerateArray();
        var rag=types.Single(t=>t.GetProperty("name").GetString()=="Fbzz.Ragdoll");
        foreach(var method in rag.GetProperty("members").EnumerateArray().Where(m=>new[]{"SetBodyTarget","ApplyBodyForce","GrabWorld","GrabBody","ReleaseGrab","SetSimulation"}.Contains(m.GetProperty("name").GetString()))) {
            var allowed=method.GetProperty("contract").GetProperty("allowedCallbacks").GetProperty("call");
            Program.Check(allowed.GetArrayLength()==1&&allowed[0].GetString()=="FixedUpdate","Physical mutations must declare exact fixed authority.");
        }
        foreach(string name in new[]{"Fbzz.RagdollBodyTarget","Fbzz.RagdollBodyState","Fbzz.RagdollBodyContact","Fbzz.RagdollBalanceState","Fbzz.RagdollGrabSettings","Fbzz.RagdollGrabState"})
            foreach(var member in types.Single(t=>t.GetProperty("name").GetString()==name).GetProperty("members").EnumerateArray().Where(m=>m.GetProperty("kind").GetString()=="field"))
                Program.Check(member.GetProperty("contract").GetProperty("pure").GetBoolean(),"Owned snapshots and settings must be usable without native authority.");
        var helper=types.Single(t=>t.GetProperty("name").GetString()=="Fbzz.RagdollBalanceUtility").GetProperty("members").EnumerateArray().Single(m=>m.GetProperty("name").GetString()=="ComputeAcceleration");
        Program.Check(helper.GetProperty("contract").GetProperty("pure").GetBoolean(),"Balance math must work without an Engine callback.");
    }
    sealed class Fixture:IDisposable,IScriptComponentLookup
    {
        readonly ScriptExecutionScope? m_previous=ScriptApiRuntime.Current;
        internal Fixture(){ScriptApiRuntime.Current=new(Bridge,this,7,9,101,13,42,ScriptCallback.FIXED_UPDATE);}
        internal Bridge Bridge{get;}=new();
        internal GameObject Target{get;}=new(202,42,"target");
        internal GameObject Other{get;}=new(303,42,"other");
        public Component[] GetComponents(ScriptExecutionScope scope,GameObject target,Type type)=>[];
        public void Dispose(){ScriptApiRuntime.Current=m_previous;}
    }
    sealed class Bridge:Program.RecordingBridge,IScriptNativeBridge
    {
        internal ulong Birth=91;
        internal int Calls;
        internal uint Selector;
        internal long Integer;
        internal float[] Values=[];
        internal string[] Text=[];
        internal GameObject? Related;
        internal NativeServiceResult Reply=new([],[],0,null);
        ulong IScriptNativeBridge.ReadComponentLifetime(ScriptExecutionScope scope,ulong objectId,uint kind)=>objectId==202 ? Birth : objectId==303 ? 72UL : 0UL;
        NativeServiceResult IScriptNativeBridge.InvokeService(ScriptExecutionScope scope,ulong objectId,uint service,uint method,long integer,
            ReadOnlySpan<float> values,ReadOnlySpan<string> text,GameObject? related,bool readOnly)
        {Calls++;Selector=method;Integer=integer;Values=values.ToArray();Text=text.ToArray();Related=related;return Reply;}
    }
}
