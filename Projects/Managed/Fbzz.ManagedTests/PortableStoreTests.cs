/// @file    PortableStoreTests.cs
/// @brief   Portable 保存値の large graph・資産 GUID・passive 復元・失敗 defaults 保護。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;
internal static class PortableStoreTests
{
    [Serializable] sealed class Profile
    {
        public int coins;
        public List<int> scores = [];
        public string note = "";
        public MaterialAssetRef skin;
        public int ExplosiveGetter => throw new InvalidOperationException("Saved getter must never run.");
        public Profile() { ConstructorCalls++; }
    }
    [Serializable] sealed class SceneBound { public GameObject? owner = default; }
    static int ConstructorCalls;
    internal static void Run()
    {
        Profile source = new() { coins = 17, note = new string('x', 2 * 1024 * 1024), skin = new MaterialAssetRef("11111111111111111111111111111111") };
        for (int index = 0; index < 40001; index++) source.scores.Add(index);
        Profile defaults = new() { coins = 99, scores = [8], note = "default" };
        int constructors = ConstructorCalls;
        string json;
        using (var stream = PortableStoreDocument.Capture(typeof(Profile), source)) {
            using StreamReader reader = new(stream, new UTF8Encoding(false, true)); json = reader.ReadToEnd();
        }
        string type = PortableStoreDocument.TypeIdentity(typeof(Profile)), schema = PortableStoreDocument.SchemaIdentity(typeof(Profile));
        Program.Check(PortableStoreDocument.TryRead(typeof(Profile), defaults, type, schema, json, out object? restored)
            && restored is Profile profile && profile.coins == 17 && profile.scores.Count == 40001 && profile.scores[40000] == 40000
            && profile.note == source.note && profile.skin.Reference == source.skin.Reference,
            "Large portable saved values must preserve all fields and typed asset GUIDs.");
        Program.Check(defaults.coins == 99 && defaults.scores.Count == 1 && defaults.note == "default" && ConstructorCalls == constructors,
            "Capture and passive read must not invoke user constructors/getters or mutate supplied defaults.");
        Program.Check(!PortableStoreDocument.TryRead(typeof(Profile), defaults, type, "wrong-schema", json, out object? failed) && failed is null,
            "Changed schemas require explicit migration and default failure output.");
        Program.Check(!PortableStoreDocument.TryRead(typeof(Profile), defaults, type, schema, "{bad", out failed) && defaults.coins == 99,
            "Malformed persisted data must not mutate defaults.");
        Program.Throws<InvalidDataException>(() => PortableStoreDocument.ValidateSchema(typeof(SceneBound)));
        source.note = "bad\uD800";
        Program.Throws<InvalidDataException>(() => PortableStoreDocument.Capture(typeof(Profile), source));
        source.note = new string('x', 2 * 1024 * 1024) + "\uD800";
        Program.Throws<InvalidDataException>(() => PortableStoreDocument.Capture(typeof(Profile), source));
        Program.Check(defaults.coins == 99 && defaults.note == "default", "Malformed text must reject without modifying supplied defaults.");
    }
}
