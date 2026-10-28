/// @file    InputBridgeTests.cs
/// @brief   実 NativeBridge で型付き入力・現在 map の発見・名前診断と借用バッファ検証を確かめる。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static unsafe class InputBridgeTests
{
    public static (string Name, Action Test)[] Cases =>
    [
        ("TypedInputAndCurrentNames", TypedInputAndCurrentNames),
        ("InputNamesDiagnoseTypoKindAndRemoval", NameDiagnostics),
        ("InputHandlesDoNotCaptureAuthority", CurrentAuthority),
        ("InputNameQueriesRejectMalformedReplies", InvalidNames),
        ("InputNameLaterPageFailurePreservesCallerAndClosesSnapshot", LaterPageFailure),
        ("InputNamePacketsRetainUnboundedOrderedCallerSizedResults", CallerSizedNames),
        ("InputUtf8PacketsAvoidWarmTemporaryArrays", InputPacketAllocation),
        ("InputUtf8PoolLeasesSurviveNestedScopesAndFailures", InputPacketReentry)
    ];

    static readonly UTF8Encoding s_utf8 = new(false, true);
    static readonly Dictionary<string, bool[]> s_buttons = new(StringComparer.Ordinal);
    static readonly Dictionary<string, float> s_axes = new(StringComparer.Ordinal);
    static ScriptExecutionScope? s_expectedScope;
    static byte[]? s_overrideNames;
    static uint? s_overrideCount;
    static int s_dispatches;
    static bool s_wireValid;
    static byte[]? s_nameSnapshot;
    static uint s_snapshotCount;
    static int s_closes;
    static bool s_failLaterPage;
    static BridgeRequest s_lastRequest;
    static bool s_fastInput, s_failFastInput;
    static byte[] s_fastName = [];
    static Action? s_reenterInput;

    static void Reset()
    {
        s_buttons.Clear(); s_axes.Clear(); s_overrideNames = null; s_overrideCount = null;
        s_dispatches = 0; s_wireValid = true;
        s_nameSnapshot = null; s_snapshotCount = 0; s_closes = 0;
        s_failLaterPage = false;
        s_fastInput = false; s_failFastInput = false; s_reenterInput = null;
        s_buttons.Add("Fire", [true, false, true]); s_axes.Add("MoveX", 0f);
    }

    static void TypedInputAndCurrentNames()
    {
        using ApiFixture fixture = new();
        Reset();
        ButtonAction fire = fixture.Script.Input.Button("Fire");
        AxisAction move = fixture.Script.Input.Axis("MoveX");
        Program.Check(fire.Name == "Fire" && move.Name == "MoveX" && fire.Held && !fire.Pressed && fire.Released && move.Value == 0f,
            "Typed handles must distinguish held, pressed, released and a registered zero axis.");
        Program.Check(!fixture.Script.Input.HasButton("Missing") && !fixture.Script.Input.HasAxis("Fire")
            && fixture.Script.Input.HasAxis("MoveX"), "Existence queries must distinguish names and action kinds from input state.");
        string longName = new('x', 300);
        s_buttons.Add("ジャンプ", [false, false, false]); s_buttons.Add(longName, [false, false, false]);
        int before = s_dispatches;
        string[] snapshot = fixture.Script.Input.GetButtonNames();
        Program.Check(snapshot.SequenceEqual(new[] { "Fire", "ジャンプ", longName }) && s_dispatches == before + 3 && s_closes == 1,
            "Current names must preserve UTF-8 and registration order using one immutable open/page/close acquisition.");
        s_buttons.Remove("Fire"); s_buttons.Add("Alternate", [false, false, false]);
        Program.Check(snapshot[0] == "Fire" && fixture.Script.Input.GetButtonNames().Contains("Alternate")
            && fixture.Script.Input.GetAxisNames().SequenceEqual(new[] { "MoveX" }) && s_wireValid,
            "Snapshots stay immutable while later queries observe the current map.");
    }

    static void NameDiagnostics()
    {
        using ApiFixture fixture = new();
        Reset();
        ExpectUnknown(() => fixture.Script.Input.Button("fire"), "fire", "button", "GetButtonNames");
        ExpectUnknown(() => fixture.Script.Input.Axis("Horizontal"), "Horizontal", "axis", "GetAxisNames");
        ExpectUnknown(() => fixture.Script.Input.Button("MoveX"), "MoveX", "button", "GetButtonNames");
        ButtonAction fire = fixture.Script.Input.Button("Fire");
        AxisAction move = fixture.Script.Input.Axis("MoveX");
        s_buttons.Remove("Fire"); s_axes.Remove("MoveX");
        ExpectUnknown(() => _ = fire.Held, "Fire", "button", "GetButtonNames");
        ExpectUnknown(() => _ = move.Value, "MoveX", "axis", "GetAxisNames");
        Program.Check(s_wireValid, "Removed handles must be revalidated by the native value request.");
    }

    static void CurrentAuthority()
    {
        using ApiFixture fixture = new();
        Reset();
        ButtonAction fire = fixture.Script.Input.Button("Fire");
        int before = s_dispatches;
        ScriptApiRuntime.Current = null;
        ExpectInvalid(() => _ = fire.Held, "callback");
        Program.Check(fire.Name == "Fire" && s_dispatches == before, "A handle must not retain permission after its callback ends.");
        ScriptApiRuntime.Current = fixture.Scope;
        ExpectInvalid(() => _ = default(ButtonAction).Held, "Input.Button");
        ExpectInvalid(() => _ = default(AxisAction).Value, "Input.Axis");
        ScriptExecutionScope replacement = new(fixture.Bridge, fixture, 11, 12, 303, 14, 84, ScriptCallback.UPDATE);
        s_expectedScope = replacement; ScriptApiRuntime.Current = replacement;
        Program.Check(fire.Held && s_lastRequest.Generation == 11 && s_lastRequest.OwnerId == 303 && s_wireValid,
            "Named input is global map identity and must use the current caller instead of captured generation or owner authority.");
    }

    static void InvalidNames()
    {
        using ApiFixture fixture = new();
        Reset();
        s_overrideNames = [1, 0, 0, 0, 5, 0, 0, 0, 65]; s_overrideCount = 1;
        ExpectMalformed(() => fixture.Script.Input.GetButtonNames());
        s_overrideNames = [1, 0, 0, 0, 1, 0, 0, 0, 255];
        ExpectMalformed(() => fixture.Script.Input.GetButtonNames());
        s_overrideNames = [0, 0, 0, 0, 0]; s_overrideCount = 0;
        ExpectMalformed(() => fixture.Script.Input.GetButtonNames());
        s_overrideNames = [255, 255, 255, 255]; s_overrideCount = uint.MaxValue;
        ExpectMalformed(() => fixture.Script.Input.GetButtonNames());
        string?[] output = ["unchanged", "tail"];
        ExpectMalformed(() => fixture.Script.Input.GetButtonNames(output));
        Program.Check(output.SequenceEqual(new[] { "unchanged", "tail" }) && s_nameSnapshot is null, "Malformed replies must not partially change caller output or retain the owned query.");
    }

    static void LaterPageFailure()
    {
        using ApiFixture fixture = new();
        string?[] output = ["sentinel", "sentinel", "tail"];
        void CheckUntouched() => Program.Check(output.SequenceEqual(new[] { "sentinel", "sentinel", "tail" })
            && s_nameSnapshot is null && s_closes == 1, "Malformed later packets must preserve the full caller output and close the owned snapshot.");
        Reset();
        s_overrideNames = EncodeNames(new[] { "first", new string('界', 30000) }); s_overrideCount = 2;
        s_failLaterPage = true;
        ExpectMalformed(() => fixture.Script.Input.GetButtonNames(output));
        Program.Check(s_dispatches >= 4, "The failure must occur after open and a valid first bounded page."); CheckUntouched();
        Reset();
        s_overrideNames = EncodeNames(new[] { "first", new string('界', 30000) }); s_overrideCount = 2;
        s_overrideNames[^1] = 0xff;
        ExpectMalformed(() => fixture.Script.Input.GetButtonNames(output)); CheckUntouched();
        Reset();
        s_overrideNames = EncodeNames(new[] { "first", new string('界', 30000) }); s_overrideCount = 2;
        Array.Resize(ref s_overrideNames, s_overrideNames.Length + 1);
        ExpectMalformed(() => fixture.Script.Input.GetButtonNames(output)); CheckUntouched();
    }

    static void CallerSizedNames()
    {
        using ApiFixture fixture = new(); Reset(); s_buttons.Clear();
        for (int index = 0; index < 4097; index++) s_buttons.Add("Button" + index + new string('x', 256), [false, false, false]);
        string giant = new('界', 350001); s_buttons.Add(giant, [false, false, false]);
        string[] expected = s_buttons.Keys.ToArray(); Program.Check(EncodeNames(expected).Length > 1048576, "The test must cross the former total payload ceiling and a single packet boundary.");
        string?[] small = ["sentinel", "tail"]; int before = s_dispatches;
        CollectionReadResult insufficient = fixture.Script.Input.GetButtonNames(small);
        Program.Check(insufficient.Required == expected.Length && insufficient.Written == 0 && small[0] == "sentinel" && small[1] == "tail" && s_dispatches == before + 2,
            "Insufficient caller capacity reports the complete count and closes without paging or touching output.");
        string?[] output = new string?[expected.Length + 1]; output[^1] = "tail";
        CollectionReadResult result = fixture.Script.Input.GetButtonNames(output);
        Program.Check(result.Required == expected.Length && result.Written == expected.Length && output.Take(expected.Length).SequenceEqual(expected) && output[^1] == "tail" && s_nameSnapshot is null,
            "All ordered names, including a giant UTF-8 name, must transfer through bounded packets and preserve unused caller storage.");
        Program.Check(fixture.Script.Input.GetButtonNames().SequenceEqual(expected) && s_wireValid, "The allocating convenience overload retains the same complete ordered values.");
    }

    static void ExpectUnknown(Action action, string name, string kind, string discovery)
    {
        try { action(); }
        catch (ArgumentException exception)
        {
            Program.Check(exception.Message.Contains(name, StringComparison.Ordinal) && exception.Message.Contains(kind, StringComparison.Ordinal)
                && exception.Message.Contains(discovery, StringComparison.Ordinal), "Unknown input diagnostics must name the action, kind and discovery API.");
            return;
        }
        throw new InvalidOperationException("An unknown input name was accepted.");
    }
    static void ExpectInvalid(Action action, string message)
    {
        try { action(); }
        catch (InvalidOperationException exception)
        { Program.Check(exception.Message.Contains(message, StringComparison.OrdinalIgnoreCase), "The input scope/default diagnostic is unclear."); return; }
        throw new InvalidOperationException("An invalid input handle was accepted.");
    }
    static void ExpectMalformed(Action action)
    {
        try { action(); }
        catch (InvalidDataException) { return; }
        catch (DecoderFallbackException) { return; }
        throw new InvalidOperationException("A malformed native input catalog was accepted.");
    }

    /// @note The fake native endpoint compares borrowed UTF8 directly so its own string decoding cannot hide Host allocation regressions.
    static void InputPacketAllocation()
    {
        using ApiFixture fixture = new(); Reset(); s_fastInput = true;
        foreach (string name in new[] { "MoveX", new string('海', 700) }) {
            s_fastName = s_utf8.GetBytes(name); AxisAction action = fixture.Script.Input.Axis(name);
            for (int index = 0; index < 32; index++) { _ = action.Value; _ = fixture.Script.Input.HasAxis(name); }
            float sum = 0f; bool present = true;
            long before = GC.GetAllocatedBytesForCurrentThread();
            for (int index = 0; index < 256; index++) { sum += action.Value; present &= fixture.Script.Input.HasAxis(name); }
            long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
            Program.Check(allocated == 0 && sum == 640f && present && s_wireValid,
                "Short stack and warmed long multibyte pool packets must preserve exact input without transient arrays.");
        }
    }
    static void InputPacketReentry()
    {
        using ApiFixture fixture = new(); Reset(); s_fastInput = true;
        string name = new('海', 700); s_fastName = s_utf8.GetBytes(name);
        AxisAction action = fixture.Script.Input.Axis(name); bool nestedRan = false;
        s_reenterInput = () => {
            ScriptExecutionScope outer = ScriptApiRuntime.RequireScope();
            ScriptExecutionScope nested = new(fixture.Bridge, fixture, 17, 19, 303, 29, 84, ScriptCallback.UPDATE);
            try {
                ScriptApiRuntime.Current = nested; s_expectedScope = nested;
                nestedRan = action.Value == 2.5f;
            } finally { ScriptApiRuntime.Current = outer; s_expectedScope = outer; }
        };
        Program.Check(action.Value == 2.5f && nestedRan && s_wireValid && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope),
            "A nested scope and long packet must not overwrite or capture the outer caller's borrowed storage.");
        s_failFastInput = true;
        Program.Throws<InvalidDataException>(() => _ = action.Value);
        s_failFastInput = false;
        Program.Check(action.Value == 2.5f && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope),
            "A rejected reply must dispose its pool lease and preserve current callback authority.");
        int before = s_dispatches;
        Program.Throws<EncoderFallbackException>(() => fixture.Script.Input.HasAxis("\ud800"));
        Program.Check(s_dispatches == before, "Malformed UTF16 must fail strict UTF8 encoding before native dispatch.");
    }

    static byte[] EncodeNames(IEnumerable<string> names)
    {
        string[] values = names.ToArray();
        using MemoryStream stream = new();
        using BinaryWriter writer = new(stream, s_utf8, true);
        writer.Write((uint)values.Length);
        foreach (string value in values) { byte[] bytes = s_utf8.GetBytes(value); writer.Write((uint)bytes.Length); writer.Write(bytes); }
        return stream.ToArray();
    }

    /// @see https://learn.microsoft.com/dotnet/api/system.runtime.interopservices.unmanagedcallersonlyattribute Unmanaged callback ABI.
    [UnmanagedCallersOnly(CallConvs = new Type[] { typeof(CallConvCdecl) })]
    static int Dispatch(BridgeRequest* request, BridgeReply* reply)
    {
        s_dispatches++; s_lastRequest = *request; *reply = default;
        ScriptExecutionScope scope = s_expectedScope!.Value;
        s_wireValid &= request->AbiVersion == BridgeAbi.VERSION && request->Generation == scope.Generation
            && request->InstanceId == scope.InstanceId && request->OwnerId == scope.OwnerId && request->CallId == scope.CallId
            && (request->Op == BridgeOp.INPUT_QUERY ? request->Flags is 0 or 1 or 513 or 769 : request->Flags == 0);
        if (s_fastInput) {
            ReadOnlySpan<byte> input = new(request->Input, (int)request->InputBytes);
            s_wireValid &= input.SequenceEqual(s_fastName);
            Action? nested = s_reenterInput; s_reenterInput = null;
            try { nested?.Invoke(); }
            catch (Exception) { reply->Status = BridgeStatus.CALLBACK_FAILED; return (int)reply->Status; }
            s_wireValid &= input.SequenceEqual(s_fastName) && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, scope);
            reply->ValueKind = request->Op == BridgeOp.INPUT_ACTION && !s_failFastInput ? ScriptFieldKind.FLOAT : ScriptFieldKind.BOOL;
            reply->X = 2.5f; reply->Integer = 1; return 0;
        }
        string name = request->InputBytes == 0 ? "" : s_utf8.GetString(new ReadOnlySpan<byte>(request->Input, (int)request->InputBytes));
        if (request->Op == BridgeOp.INPUT_ACTION)
        {
            bool axis = request->Index == (uint)ScriptInputValue.AXIS;
            if (axis ? !s_axes.ContainsKey(name) : !s_buttons.ContainsKey(name))
            { reply->Status = BridgeStatus.NOT_FOUND; return (int)reply->Status; }
            reply->ValueKind = axis ? ScriptFieldKind.FLOAT : ScriptFieldKind.BOOL;
            if (axis) reply->X = s_axes[name]; else reply->Integer = s_buttons[name][(int)request->Index] ? 1 : 0;
            return 0;
        }
        s_wireValid &= request->Op == BridgeOp.INPUT_QUERY;
        if (request->Flags == 769) { s_nameSnapshot = null; s_closes++; return 0; }
        if (request->Flags == 513) {
            if (s_nameSnapshot is null) { reply->Status = BridgeStatus.NOT_FOUND; return (int)reply->Status; }
            ulong offset = ((ulong)request->Reserved1 << 32) | request->Reserved0;
            if (offset > (ulong)s_nameSnapshot.Length || request->OutputCapacity > BridgeAbi.INPUT_CATALOG_PACKET_BYTES) { reply->Status = BridgeStatus.INVALID_ARGUMENT; return (int)reply->Status; }
            int count = (int)Math.Min(request->OutputCapacity, (ulong)s_nameSnapshot.Length - offset);
            s_nameSnapshot.AsSpan((int)offset, count).CopyTo(new Span<byte>(request->Output, count));
            reply->OutputBytes = (uint)count; reply->Generation = (ulong)s_nameSnapshot.Length; reply->Index = s_snapshotCount; reply->CallId = scope.SceneLifetime;
            if (s_failLaterPage && offset != 0) reply->CallId = scope.SceneLifetime + 1;
            reply->Integer = (long)offset + count; reply->Flags = offset + (uint)count == (ulong)s_nameSnapshot.Length ? 1U : 0U; return 0;
        }
        bool axisQuery = request->Index == (uint)ScriptInputQuery.AXIS_EXISTS || request->Index == (uint)ScriptInputQuery.AXIS_NAMES;
        if (request->Index <= (uint)ScriptInputQuery.AXIS_EXISTS)
        {
            reply->ValueKind = ScriptFieldKind.BOOL; reply->Integer = (axisQuery ? s_axes.ContainsKey(name) : s_buttons.ContainsKey(name)) ? 1 : 0;
            return 0;
        }
        s_wireValid &= name.Length == 0;
        byte[] output = s_overrideNames ?? EncodeNames(axisQuery ? s_axes.Keys : s_buttons.Keys);
        if (request->Flags == 1) {
            s_snapshotCount = s_overrideCount ?? (uint)(axisQuery ? s_axes.Count : s_buttons.Count);
            reply->ValueKind = ScriptFieldKind.STRING_ARRAY; reply->Index = s_snapshotCount; reply->CallId = scope.SceneLifetime;
            if (output.Length <= request->OutputCapacity) { reply->OutputBytes = (uint)output.Length; output.CopyTo(new Span<byte>(request->Output, output.Length)); return 0; }
            s_nameSnapshot = output; reply->Integer = 1; reply->Generation = (ulong)output.Length; reply->OutputBytes = (uint)output.Length;
            reply->Status = BridgeStatus.BUFFER_TOO_SMALL; return (int)reply->Status;
        }
        reply->ValueKind = ScriptFieldKind.STRING_ARRAY;
        reply->Index = s_overrideCount ?? (uint)(axisQuery ? s_axes.Count : s_buttons.Count);
        reply->OutputBytes = (uint)output.Length;
        if (output.Length > request->OutputCapacity)
        { reply->Status = BridgeStatus.BUFFER_TOO_SMALL; return (int)reply->Status; }
        output.CopyTo(new Span<byte>(request->Output, output.Length));
        return 0;
    }

    sealed class OwnerScript : Script { }
    sealed class ApiFixture : IDisposable, IScriptComponentLookup
    {
        readonly ScriptExecutionScope? m_previous = ScriptApiRuntime.Current;
        internal ApiFixture()
        {
            Type type = typeof(EntryPoint).GetNestedType("NativeBridge", BindingFlags.NonPublic)!;
            ConstructorInfo constructor = type.GetConstructor(BindingFlags.Instance | BindingFlags.NonPublic, null, [typeof(nint)], null)!;
            nint address = (nint)(delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)&Dispatch;
            Bridge = (IScriptNativeBridge)constructor.Invoke([address]);
            ScriptBinding binding = new(7, 9, 101) { GameObject = new GameObject(101, 42, "owner") };
            Script = new OwnerScript { Binding = binding };
            Scope = new(Bridge, this, 7, 9, 101, 13, 42, ScriptCallback.UPDATE);
            s_expectedScope = Scope; ScriptApiRuntime.Current = Scope;
        }
        internal IScriptNativeBridge Bridge { get; }
        internal Script Script { get; }
        internal ScriptExecutionScope Scope { get; }
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => [];
        public void Dispose() { ScriptApiRuntime.Current = m_previous; s_expectedScope = null; }
    }
}
