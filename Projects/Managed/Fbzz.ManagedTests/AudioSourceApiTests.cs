/// @file    AudioSourceApiTests.cs
/// @brief   AudioSource 追加 facade の selector・lifetime・catalog 契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;
namespace Fbzz.ManagedTests;
internal static class AudioSourceApiTests
{
    internal static IReadOnlyList<(string Name, Action Run)> Cases => [
        (nameof(AudioSourceSettingsUseReservedSelectorsAndExactLifetime), AudioSourceSettingsUseReservedSelectorsAndExactLifetime),
        (nameof(AudioSourceSettingsRejectExpiredAndNonfiniteCalls), AudioSourceSettingsRejectExpiredAndNonfiniteCalls),
        (nameof(AudioSourceSettingsCatalogSeparatesQueriesAndPlayWrites), AudioSourceSettingsCatalogSeparatesQueriesAndPlayWrites)
    ];
    static void AudioSourceSettingsUseReservedSelectorsAndExactLifetime()
    {
        using Fixture f = new(); AudioSource source = new(f.Target, 91);
        source.Priority = -12; f.Assert(47, false, -12);
        f.Bridge.Reply = new([], [], 30, null); Program.Check(source.Priority == 30, "Priority query must preserve signed integer."); f.Assert(46, true);
        source.Streaming = false; f.Assert(49, false);
        f.Bridge.Reply = new([], [], 1, null); Program.Check(source.Streaming, "Streaming query must use strict native boolean."); f.Assert(48, true);
        source.AirAbsorption = 0.3f; f.Assert(51, false, values: [0.3f]);
        f.Bridge.Reply = new([0.4f], [], 0, null); Program.Check(source.AirAbsorption == 0.4f, "Air absorption must query source settings."); f.Assert(50, true);
        source.DopplerLevel = 0.2f; f.Assert(53, false, values: [0.2f]); _ = source.DopplerLevel; f.Assert(52, true);
        source.LowPassCutoff = 0.6f; f.Assert(55, false, values: [0.6f]); _ = source.LowPassCutoff; f.Assert(54, true);
        source.FadeLowPassTo(0.2f, 0.8f); f.Assert(56, false, values: [0.2f, 0.8f]);
        source.SetVolume(0.7f); f.Assert(14, false, values: [0.7f]);
    }
    static void AudioSourceSettingsRejectExpiredAndNonfiniteCalls()
    {
        using Fixture f = new(); AudioSource source = new(f.Target, 91); int calls = f.Bridge.Calls;
        Program.Throws<ArgumentOutOfRangeException>(() => source.LowPassCutoff = float.NaN);
        Program.Throws<ArgumentOutOfRangeException>(() => source.FadeLowPassTo(0.2f, float.PositiveInfinity));
        Program.Check(f.Bridge.Calls == calls, "Nonfinite source settings must not dispatch.");
        f.Bridge.Birth = 92; Program.Throws<InvalidOperationException>(() => source.Priority = 10);
        Program.Check(f.Bridge.Calls == calls, "A retained AudioSource must not mutate a replacement row.");
        f.Bridge.Birth = 91; ScriptApiRuntime.Current = null;
        Program.Throws<InvalidOperationException>(() => { _ = source.LowPassCutoff; });
        Program.Check(f.Bridge.Calls == calls, "AudioSource must not retain callback authority.");
    }
    static void AudioSourceSettingsCatalogSeparatesQueriesAndPlayWrites()
    {
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement type = document.RootElement.GetProperty("types").EnumerateArray().Single(t => t.GetProperty("name").GetString() == "Fbzz.AudioSource");
        foreach (string name in new[] { "Priority", "Streaming", "AirAbsorption", "DopplerLevel", "LowPassCutoff" }) {
            JsonElement contract = type.GetProperty("members").EnumerateArray().Single(m => m.GetProperty("name").GetString() == name).GetProperty("contract");
            Program.Check(contract.GetProperty("runModeAccess").GetProperty("read").GetArrayLength() == 2
                && contract.GetProperty("runModeAccess").GetProperty("write").GetArrayLength() == 1,
                "Source settings must publish Edit/Play reads and Play-only writes.");
        }
    }
    sealed class Fixture : IDisposable, IScriptComponentLookup
    {
        readonly ScriptExecutionScope? m_previous = ScriptApiRuntime.Current;
        internal Fixture() { ScriptApiRuntime.Current = new(Bridge, this, 7, 9, 101, 13, 42, ScriptCallback.UPDATE); }
        internal Bridge Bridge { get; } = new();
        internal GameObject Target { get; } = new(202, 42, "target");
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => [];
        public void Dispose() { ScriptApiRuntime.Current = m_previous; }
        internal void Assert(uint selector, bool read, long integer = 0, float[]? values = null) => Program.Check(
            Bridge.Service == 181 && Bridge.Selector == selector && Bridge.ReadOnly == read && Bridge.Integer == integer
                && Bridge.Values.SequenceEqual(values ?? []) && Bridge.Text.Length == 0,
            "AudioSource must transfer the exact typed packet without changing old selector assignments.");
    }
    sealed class Bridge : Program.RecordingBridge, IScriptNativeBridge
    {
        internal ulong Birth = 91;
        internal int Calls;
        internal uint Service, Selector;
        internal long Integer;
        internal float[] Values = [];
        internal string[] Text = [];
        internal bool ReadOnly;
        internal NativeServiceResult Reply = new([], [], 0, null);
        ulong IScriptNativeBridge.ReadComponentLifetime(ScriptExecutionScope scope, ulong objectId, uint kind) => objectId == 202 && kind == 6 ? Birth : 0;
        NativeServiceResult IScriptNativeBridge.InvokeService(ScriptExecutionScope scope, ulong objectId, uint service, uint method, long integer,
            ReadOnlySpan<float> values, ReadOnlySpan<string> text, GameObject? related, bool readOnly)
        {
            Calls++; Service = service; Selector = method; Integer = integer; Values = values.ToArray(); Text = text.ToArray(); ReadOnly = readOnly;
            return Reply;
        }
    }
}
