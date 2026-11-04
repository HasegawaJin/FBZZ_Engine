/// @file    FootIKApiTests.cs
/// @brief   足 IK 調整値の転送・診断復号と SDK 公開契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;
namespace Fbzz.ManagedTests;
internal static class FootIKApiTests
{
    internal static IReadOnlyList<(string Name,Action Run)> Cases => [
        (nameof(FootSettingsAndStateRetainEveryValueAndFlag),FootSettingsAndStateRetainEveryValueAndFlag),
        (nameof(FootCatalogDeclaresRuntimeTuningAndWorldDiagnostics),FootCatalogDeclaresRuntimeTuningAndWorldDiagnostics)];
    static void FootSettingsAndStateRetainEveryValueAndFlag()
    {
        using Fixture f=new(); IKSolver ik=new(f.Target,91);
        FootIKSettings settings=new(){SurfaceOffset=0.1f,HipWeight=0.5f,MaxHipLowering=0.03f,MaxHipRaising=0.02f,BendReserve=0.01f};
        f.Bridge.Reply=new([],[],1,null);
        Program.Check(ik.SetFootSettings("Feet",settings)&&f.Bridge.Selector==11&&f.Bridge.Integer==7
            &&f.Bridge.Values.Length==11&&f.Bridge.Values[8]==0.03f&&f.Bridge.Values[10]==0.01f,"All foot settings and independent hip bounds must transfer atomically.");
        f.Bridge.Reply=new([0.5f,1.2f,0.1f,0.02f,0.15f,0.06f,0.12f,0.5f,0.03f,0.02f,0.01f],[],15,null);
        Program.Check(ik.TryGetFootSettings("Feet",out var read)&&read.AdjustHip&&read.UseAnimatorIKWeight&&read.UseRagdollFootShape
            &&read.MaxHipRaising==0.02f&&read.BendReserve==0.01f,"Readback must retain values beyond the inline packet size.");
        f.Bridge.Reply=new([-0.1f,0.02f,-0.03f,-0.025f,1,0.5f],[],27,null);
        var state=ik.GetFootState("Feet");
        Program.Check(state.Valid&&state.UsesPhysicsShape&&state.LeftGrounded&&!state.RightGrounded&&state.HipLimited
            &&state.HipTarget==-0.03f&&state.HipOffset==-0.025f,"Requested and applied hip displacements must remain distinct.");
        int calls=f.Bridge.Calls;
        settings.SurfaceOffset=float.NaN;
        Program.Throws<ArgumentOutOfRangeException>(()=>ik.SetFootSettings("Feet",settings));
        Program.Check(f.Bridge.Calls==calls,"Nonfinite settings must never reach the engine.");
        f.Bridge.Birth=92; Program.Throws<InvalidOperationException>(()=>ik.GetFootState("Feet"));
    }
    static void FootCatalogDeclaresRuntimeTuningAndWorldDiagnostics()
    {
        using JsonDocument doc=JsonDocument.Parse(ApiCatalog.Json);
        var types=doc.RootElement.GetProperty("types").EnumerateArray();
        var ik=types.Single(t=>t.GetProperty("name").GetString()=="Fbzz.IKSolver");
        var members=ik.GetProperty("members").EnumerateArray();
        var write=members.Single(m=>m.GetProperty("name").GetString()=="SetFootSettings").GetProperty("contract");
        Program.Check(write.GetProperty("runModes").EnumerateArray().All(v=>v.GetString()=="Play"),"Foot tuning must declare Play-only mutation.");
        foreach(string name in new[]{"Fbzz.FootIKSettings","Fbzz.FootIKState"})
            foreach(var member in types.Single(t=>t.GetProperty("name").GetString()==name).GetProperty("members").EnumerateArray().Where(m=>m.GetProperty("kind").GetString()=="field"))
                Program.Check(member.GetProperty("contract").GetProperty("pure").GetBoolean(),"Owned tuning and diagnostic fields must be pure.");
    }
    sealed class Fixture:IDisposable,IScriptComponentLookup
    {
        readonly ScriptExecutionScope? m_previous=ScriptApiRuntime.Current;
        internal Fixture(){ScriptApiRuntime.Current=new(Bridge,this,7,9,101,13,42,ScriptCallback.FIXED_UPDATE);}
        internal Bridge Bridge{get;}=new();
        internal GameObject Target{get;}=new(202,42,"target");
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
        internal NativeServiceResult Reply=new([],[],0,null);
        ulong IScriptNativeBridge.ReadComponentLifetime(ScriptExecutionScope scope,ulong objectId,uint kind)=>Birth;
        NativeServiceResult IScriptNativeBridge.InvokeService(ScriptExecutionScope scope,ulong objectId,uint service,uint method,long integer,
            ReadOnlySpan<float> values,ReadOnlySpan<string> text,GameObject? related,bool readOnly)
        {Calls++;Selector=method;Integer=integer;Values=values.ToArray();return Reply;}
    }
}
