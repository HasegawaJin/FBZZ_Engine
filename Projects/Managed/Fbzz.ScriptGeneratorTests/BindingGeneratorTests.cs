/// @file    BindingGeneratorTests.cs
/// @brief   共通 ABI の全配置・数値 ID・単純 marshaller と生成対象の診断を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Fbzz.ScriptGenerator;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;

namespace Fbzz.ScriptGeneratorTests;

public static class BindingGeneratorTests
{
    private static readonly CSharpParseOptions PARSE_OPTIONS = new(LanguageVersion.Latest);
    private static readonly ImmutableArray<MetadataReference> REFERENCES = PlatformReferences();

    public static void RunAll()
    {
        (string Name, Action Test)[] tests =
        [
            ("CanonicalHostLayoutAndIds", CanonicalHostLayoutAndIds),
            ("CanonicalSdkEnums", CanonicalSdkEnums),
            ("GeneratedCallsPackAndDispatchOnce", GeneratedCallsPackAndDispatchOnce),
            ("GameCompilationSkipsBindings", GameCompilationSkipsBindings),
            ("MissingAndInvalidTargetsAreDiagnosed", MissingAndInvalidTargetsAreDiagnosed),
            ("MalformedContractsAreDiagnosed", MalformedContractsAreDiagnosed),
            ("QuotedCallFragmentsRemainIntact", QuotedCallFragmentsRemainIntact),
            ("CrossTargetEnumNamesRemainIndependent", CrossTargetEnumNamesRemainIndependent)
        ];
        foreach ((string name, Action test) in tests)
        {
            test();
            Console.WriteLine("[PASS] BindingGenerator." + name);
        }
        Console.WriteLine($"BINDING GENERATOR TESTS passed={tests.Length} failed=0");
    }

    private static void CanonicalHostLayoutAndIds()
    {
        Assembly assembly = Compile("Host", Contract());
        Check(IntPtr.Size == 8, "The managed bridge contract requires x64.");
        Type abi = RequiredType(assembly, "Fbzz.ManagedHost.BridgeAbi");
        Check(Convert.ToUInt32(Constant(abi, "VERSION")) == 1, "ABI version must remain 1.");
        Check(Convert.ToUInt64(Constant(abi, "SCHEMA_HASH")) == 0x46425A5A4D414E15UL, "The exact 64-bit schema hash must be preserved.");
        Dictionary<string, uint> flags = new(StringComparer.Ordinal)
        {
            ["FIELD_SETTING"] = 1, ["FIELD_OBSERVATION"] = 2, ["NULL_VALUE"] = 4, ["FIELD_HIDDEN"] = 8,
            ["FIELD_RANGE"] = 16, ["FIELD_REQUIRED"] = 32, ["FIELD_RELOAD_STATE"] = 64, ["INTERNAL_MODE"] = 1
        };
        foreach ((string name, uint value) in flags) Check(Convert.ToUInt32(Constant(abi, name)) == value, "Incorrect flag: " + name);
        Dictionary<string, uint> batch = new(StringComparer.Ordinal)
        {
            ["MAX_BATCH_PACKET_BYTES"] = 65536, ["BATCH_REFERENCE_BYTES"] = 16, ["BATCH_SET_ITEM_BYTES"] = 28,
            ["BATCH_READ_ITEM_BYTES"] = 16, ["BATCH_STATUS_BYTES"] = 4,
            ["COLLISION_HEADER_BYTES"] = 68, ["PHYSICS_HIT_BYTES"] = 40, ["INPUT_CATALOG_PACKET_BYTES"] = 65536
        };
        foreach ((string name, uint value) in batch) Check(Convert.ToUInt32(Constant(abi, name)) == value, "Incorrect batch wire constant: " + name);
        Check(abi.GetField("MAX_SCENE_OBJECTS", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static) is null,
            "The bridge must not impose a fixed Scene object capacity.");
        CheckEnum(assembly, "Fbzz.ManagedHost.BridgeOp",
            "INITIALIZE=1 LOAD_CANDIDATE=2 GET_TYPE=3 CREATE=4 INVOKE=5 GET_FIELD=6 READ_FIELD=7 WRITE_FIELD=8 WRITE_FIELDS=14 RELEASE_INSTANCE=9 RETIRE_GENERATION=10 SHUTDOWN=11 GET_DIAGNOSTICS=12 VALIDATE_FIELDS=13 BIND_OWNER=15 VALIDATE_REFERENCES=16 DISCOVER_SOURCES=17 GET_API_CATALOG=18 TRANSFORM_READ=100 TRANSFORM_WRITE_LOCAL=101 TRANSFORM_WRITE_WORLD=102 TRANSFORM_TRANSLATE_LOCAL=103 TIME=104 INPUT_KEY=105 DIAGNOSTIC=106 INPUT_ACTION=107 RESOLVE_GUID=108 OBJECT_IS_ALIVE=109 MANAGED_COMPONENTS=110 OBJECT_SELF=111 OBJECT_NAME=112 SCENE_FIND_NAME=113 FIND_CHILD_PATH=114 OBJECTS_READ_WORLD_POSITIONS=115 OBJECTS_SET_WORLD_POSITIONS=116 OBJECTS_TRANSLATE_WORLD=117 TRANSFORM_READ_ROTATION=118 TRANSFORM_WRITE_LOCAL_ROTATION=119 TRANSFORM_READ_SCALE=120 TRANSFORM_WRITE_LOCAL_SCALE=121 TRANSFORM_READ_DIRECTION=122 TRANSFORM_ROTATE=123 TRANSFORM_LOOK_AT=124 SCENE_CREATE=125 SCENE_INSTANTIATE=126 OBJECT_DESTROY=127 SCENE_CAN_CREATE=128 PHYSICS_BODY_READ=129 PHYSICS_BODY_WRITE=130 PHYSICS_BODY_COMMAND=131 PHYSICS_WORLD_READ=132 PHYSICS_RAYCAST=133 PHYSICS_SPHERE_CAST=134 OBJECTS_DESTROY=135 PHYSICS_RAYCAST_ALL=136 PHYSICS_OVERLAP_SPHERE=137 OBJECT_READ=138 OBJECT_WRITE=139 COMPONENT_READ=140 COMPONENT_ADD=141 COMPONENT_REMOVE=142 SCRIPT_ENABLED_READ=143 SCRIPT_ENABLED_WRITE=144 SCENE_QUERY=145 SCENE_SPAWN_MANY=146 OBJECT_SET_PARENT=147 OBJECTS_OWN=148 COLLIDER_READ=150 COLLIDER_WRITE=151 COLLIDER_COMMAND=152 PHYSICS_BODY_READ_ROTATION=153 PHYSICS_BODY_WRITE_ROTATION=154 PHYSICS_RAYCAST_FILTERED=155 PHYSICS_SPHERE_CAST_FILTERED=156 PHYSICS_RAYCAST_ALL_FILTERED=157 PHYSICS_OVERLAP_SPHERE_FILTERED=158 INPUT_QUERY=159 SERVICE_ANIMATOR=180 SERVICE_AUDIO=181 SERVICE_UI=182 SERVICE_CAMERA=183 SERVICE_NAVIGATION=184 SERVICE_CHARACTER=185 CAPTURE_RELOAD=220 RESTORE_RELOAD=221 COMMIT_RELOAD=222 INVOKE_JOB=223 INVOKE_EVENT=224 REGISTER_DELAY=225 REGISTER_TWEEN=226 CANCEL_JOB=227 CANCEL_OWNER=228 SUBSCRIBE_EVENT=229 UNSUBSCRIBE_EVENT=230 PUBLISH_EVENT=231 READ_ASSET=240 RESUME_INSTANCE=19 GET_SETTINGS_SNAPSHOT=20 GET_SETTINGS_REVISION=21 PREPARE_SETTINGS=22 COMMIT_SETTINGS=23 DISCARD_SETTINGS=24 RESTORE_SETTINGS=25 GET_PREPARED_SETTINGS=26 OPEN_OBSERVATION=27 READ_OBSERVATION=28 CLOSE_OBSERVATION=29 DRAW_GIZMOS_QUERY=30 RETAIN_GENERATION=31 RELEASE_GENERATION=32 RESOLVE_COMPONENT_REFERENCE=33 RESOLVE_SAVED_COMPONENT_REFERENCES=34 READ_SETTINGS_PAGE=40 OPEN_SETTINGS_EDIT=41 STAGE_SETTINGS_EDIT=42 PREPARE_SETTINGS_EDIT=43 CLOSE_SETTINGS_EDIT=44 GET_PREPARED_REVIEW=45 CAPTURE_SETTINGS_ADOPTION=46 PREPARE_SETTINGS_ADOPTION=47 MANAGE_SETTINGS_ADOPTION=48 GET_DATA_ASSET_SCHEMA=49 OPEN_DATA_ASSET_DOCUMENT=50 READ_DATA_ASSET_PAGE=51 PREPARE_DATA_ASSET_DOCUMENT=52 READ_PREPARED_DATA_ASSET=53 CLOSE_DATA_ASSET_DOCUMENT=54 SERVICE_CURSOR=186 SERVICE_APPLICATION=187 SERVICE_LIGHT=188 SERVICE_PARTICLE=189 SERVICE_SCENE=190 SERVICE_MATERIAL=191 SERVICE_SAVE=192 SERVICE_CONFIG=193 SERVICE_TERRAIN=194 SERVICE_WATER=195 SERVICE_VFX=196 SERVICE_SEQUENCE=197 SERVICE_RENDER=198 SERVICE_DISPLAY=199 SERVICE_JOINT=200 SERVICE_ASSETS=201 SERVICE_LINE=300 SERVICE_SPRITE=301 SERVICE_SORTING_GROUP=302 SERVICE_TRAIL=303 SERVICE_MESH_TRAIL=304 SERVICE_MESH=305 SERVICE_SKINNED_MESH=306 SERVICE_MOTION_WARP=307 SERVICE_IK=308 SERVICE_RAGDOLL=309 SERVICE_SPRING_BONE=310 SERVICE_UI_ANIMATOR=311 SERVICE_PATROL=312 SERVICE_SPLINE_FOLLOWER=313 SERVICE_CAMERA_SHAKE=314 SERVICE_VIRTUAL_CAMERA=315 SERVICE_AUDIO_MIXER_SEND=316 SERVICE_FLOW_FIELD=317 SERVICE_INSTANCES=318 AI_CROWD_STEP=340");
        CheckEnum(assembly, "Fbzz.ManagedHost.BridgeStatus",
            "OK=0 INVALID_ARGUMENT=1 ABI_MISMATCH=2 BUFFER_TOO_SMALL=3 NOT_FOUND=4 INVALID_SCOPE=5 STALE_GENERATION=6 CALLBACK_FAILED=7 LOAD_FAILED=8 UNSUPPORTED=9 PENDING_LIMIT=10 COLLECTION_PENDING=11 SCENE_MISMATCH=12 PHYSICS_BODY=13 CAPACITY_LIMIT=14");
        CheckEnum(assembly, "Fbzz.ManagedHost.ScriptFieldKind",
            "NONE=0 FLOAT=1 INT=2 BOOL=3 STRING=4 VECTOR3=5 GAME_OBJECT=6 FLOAT_ARRAY=7 INT_ARRAY=8 BOOL_ARRAY=9 STRING_ARRAY=10 VECTOR3_ARRAY=11 GAME_OBJECT_ARRAY=12 QUATERNION=13 STRUCTURED=14 ASSET_REFERENCE=15");
        CheckEnum(assembly, "Fbzz.ManagedHost.BridgeTransformValue", "LOCAL_POSITION=0 WORLD_POSITION=1");
        CheckEnum(assembly, "Fbzz.ManagedHost.BridgeTransformDirection", "FORWARD=0 RIGHT=1 UP=2");
        CheckLayout(assembly, abi, "BridgeRequest", "REQUEST", 104,
            "AbiVersion=0 Op=4 Generation=8 InstanceId=16 OwnerId=24 CallId=32 Index=40 Flags=44 Integer=48 X=56 Y=60 Z=64 W=68 Input=72 InputBytes=80 Reserved0=84 Output=88 OutputCapacity=96 Reserved1=100");
        CheckLayout(assembly, abi, "BridgeReply", "REPLY", 72,
            "Status=0 ValueKind=4 Generation=8 InstanceId=16 CallId=24 Index=32 Flags=36 Integer=40 X=48 Y=52 Z=56 W=60 OutputBytes=64 Reserved=68");
        Check(assembly.GetType("Fbzz.ManagedHost.ScriptCallback") is null, "SDK types must not be emitted into Host.");
    }

    private static void CanonicalSdkEnums()
    {
        Assembly assembly = Compile("Sdk", Contract());
        CheckEnum(assembly, "Fbzz.ScriptCallback",
            "AWAKE=0 START=1 ON_ENABLE=2 ON_DISABLE=3 UPDATE=4 FIXED_UPDATE=5 LATE_UPDATE=6 ON_DESTROY=7 RESET=8 ON_VALIDATE=9 ON_BEFORE_SERIALIZE=10 ON_AFTER_DESERIALIZE=11 CONNECT=12 AFTER_RELOAD=13 RELOAD_RETIRED=14 ON_COLLISION_ENTER=15 ON_COLLISION_STAY=16 ON_COLLISION_EXIT=17 ON_TRIGGER_ENTER=18 ON_TRIGGER_STAY=19 ON_TRIGGER_EXIT=20 EVENT=21 JOB=22 DRAW_GIZMOS=23 DRAW_GIZMOS_SELECTED=24 ON_ANIMATION_EVENT=26 ON_ANIMATOR_MOVE=27 ON_SEQUENCE_EVENT=28 ON_SEQUENCE_FINISHED=29 ON_NAV_MESH_DESTINATION_REACHED=30 ON_NAV_MESH_PATH_FAILED=31 ON_NAV_MESH_TARGET_SPOTTED=32 ON_NAV_MESH_TARGET_LOST=33");
        CheckEnum(assembly, "Fbzz.PhysicsBodyValue", "VELOCITY=0 ANGULAR_VELOCITY=1 POSITION=2 MASS=3 GRAVITY_SCALE=4 IS_STATIC=5 IS_SLEEPING=6 EXISTS=7 ENABLED=8 LINEAR_DRAG=9 ANGULAR_DRAG=10 USE_CCD=11 CCD_RADIUS=12 LOCAL_TIME_SCALE=13 FLOW_COUPLING=14 USE_GRAVITY=15 ALLOW_SLEEPING=16");
        CheckEnum(assembly, "Fbzz.PhysicsBodyCommand", "FORCE=0 IMPULSE=1 TORQUE=2 FORCE_AT_POINT=3 WAKE=4 SLEEP=5 FREEZE_POSITION=6 FREEZE_ROTATION=7 TELEPORT=8 POINT_IMPULSE=9 ANGULAR_IMPULSE=10");
        CheckEnum(assembly, "Fbzz.PhysicsWorldValue", "GRAVITY=0 LAYERS_COLLIDE=1");
        CheckEnum(assembly, "Fbzz.ScriptInputValue", "HELD=0 DOWN=1 UP=2 AXIS=3");
        CheckEnum(assembly, "Fbzz.ScriptInputQuery", "BUTTON_EXISTS=0 AXIS_EXISTS=1 BUTTON_NAMES=2 AXIS_NAMES=3");
        CheckEnum(assembly, "Fbzz.KeyCode", "SPACE=32 LEFT=37 UP=38 RIGHT=39 DOWN=40 A=65 D=68 E=69 Q=81 S=83 W=87");
        CheckEnum(assembly, "Fbzz.ObjectStatus", "VALID=0 UNSET=1 EXPIRED=2");
        CheckEnum(assembly, "Fbzz.BatchStatus", "OK=0 INVALID_ARGUMENT=1 BUFFER_TOO_SMALL=3 NOT_FOUND=4 INVALID_SCOPE=5 STALE_GENERATION=6 LOAD_FAILED=8 SCENE_MISMATCH=12 PHYSICS_BODY=13 CAPACITY_LIMIT=14");
        CheckEnum(assembly, "Fbzz.NativeComponentKind", "TRANSFORM=0 RIGID_BODY=1 BOX_COLLIDER=2 SPHERE_COLLIDER=3 CAPSULE_COLLIDER=4 ANIMATOR=5 AUDIO_SOURCE=6 UI=7 CAMERA=8 NAV_MESH_AGENT=9 CHARACTER_CONTROLLER=10 UI_TEXT=11 UI_IMAGE=12 UI_BUTTON=13 UI_CANVAS=14 CYLINDER_COLLIDER=15 AABB_COLLIDER=16 MESH_COLLIDER=17 CONVEX_HULL_COLLIDER=18 TERRAIN_COLLIDER=19 UI_SLIDER=20 UI_TOGGLE=21 UI_SCROLL_VIEW=22 UI_TEXT_FIELD=23 UI_CANVAS_GROUP=24 UI_DRAGGABLE=25 UI_DROP_TARGET=26 LIGHT=27 PARTICLE_SYSTEM=28 MATERIAL=29 VFX=30 SEQUENCE=31 TERRAIN=32 WATER=33 JOINT=34 LINE_RENDERER=35 SPRITE_RENDERER=36 SORTING_GROUP=37 TRAIL=38 MESH_TRAIL=39 MESH_RENDERER=40 SKINNED_MESH_RENDERER=41 MOTION_WARP=42 IK_SOLVER=43 RAGDOLL=44 SPRING_BONE=45 UI_ANIMATOR=46 NAV_MESH_PATROL=47 SPLINE_FOLLOWER=48 CAMERA_SHAKE=49 VIRTUAL_CAMERA=50 AUDIO_MIXER_SEND=51 FLOW_FIELD=52 INSTANCED_MESH=53 COUNT=54 MANAGED_SCRIPT=4294967295");
        Check(assembly.GetType("Fbzz.ManagedHost.BridgeRequest") is null, "Host wire structures must not leak into the public SDK.");
        CheckEnum(assembly, "Fbzz.ScriptServiceKind", "SERVICE_ANIMATOR=180 SERVICE_AUDIO=181 SERVICE_UI=182 SERVICE_CAMERA=183 SERVICE_NAVIGATION=184 SERVICE_CHARACTER=185 SERVICE_CURSOR=186 SERVICE_APPLICATION=187 SERVICE_LIGHT=188 SERVICE_PARTICLE=189 SERVICE_SCENE=190 SERVICE_MATERIAL=191 SERVICE_SAVE=192 SERVICE_CONFIG=193 SERVICE_TERRAIN=194 SERVICE_WATER=195 SERVICE_VFX=196 SERVICE_SEQUENCE=197 SERVICE_RENDER=198 SERVICE_DISPLAY=199 SERVICE_JOINT=200 SERVICE_ASSETS=201 SERVICE_LINE=300 SERVICE_SPRITE=301 SERVICE_SORTING_GROUP=302 SERVICE_TRAIL=303 SERVICE_MESH_TRAIL=304 SERVICE_MESH=305 SERVICE_SKINNED_MESH=306 SERVICE_MOTION_WARP=307 SERVICE_IK=308 SERVICE_RAGDOLL=309 SERVICE_SPRING_BONE=310 SERVICE_UI_ANIMATOR=311 SERVICE_PATROL=312 SERVICE_SPLINE_FOLLOWER=313 SERVICE_CAMERA_SHAKE=314 SERVICE_VIRTUAL_CAMERA=315 SERVICE_AUDIO_MIXER_SEND=316 SERVICE_FLOW_FIELD=317 SERVICE_INSTANCES=318");
        Check(assembly.GetType("Fbzz.NativeSceneCapacity") is null, "The SDK must not project a fixed Scene object capacity.");
        CheckEnum(assembly, "Fbzz.AssetLoadState", "NONE=0 QUEUED=1 DECODING=2 WAITING_DEPENDENCIES=3 UPLOAD_QUEUED=4 UPLOADING=5 READY=6 FAILED=7 CANCELED=8");
        CheckEnum(assembly, "Fbzz.AssetLoadError", "NONE=0 NO_CHANNEL=1 NOT_FOUND=2 DECODE_FAILED=3 UPLOAD_FAILED=4 DEPENDENCY_FAILED=5 DEPENDENCY_CYCLE=6 BUDGET_EXCEEDED=7 SHUTTING_DOWN=8");
    }

    private static void GeneratedCallsPackAndDispatchOnce()
    {
        Assembly assembly = Compile("Host", Contract(), probe: true);
        MethodInfo run = RequiredType(assembly, "Fbzz.ManagedHost.EntryPoint").GetMethod("RunProbe")
            ?? throw new InvalidOperationException("The probe entry point is missing.");
        Check((bool?)run.Invoke(null, null) == true, "Generated methods must preserve operation, payload, caller provenance and exactly one dispatch.");
    }

    private static void GameCompilationSkipsBindings()
    {
        Run(null, [new ContractText("FBZZ_MANAGED_UNKNOWN()")], out Compilation output, out ImmutableArray<Diagnostic> diagnostics, out GeneratorDriverRunResult result);
        Check(diagnostics.IsEmpty && result.Results.Single().GeneratedSources.IsEmpty, "Unspecified game builds must not parse or emit bridge bindings.");
        Check(!output.GetDiagnostics().Any(static value => value.Severity == DiagnosticSeverity.Error), "Skipping bindings must keep ordinary C# valid.");
    }

    private static void MissingAndInvalidTargetsAreDiagnosed()
    {
        Run("Host", [], out _, out ImmutableArray<Diagnostic> missing, out _);
        Check(missing.Any(static value => value.Id == "FBZZ1011"), "Host must explicitly provide one contract.");
        Run("Sdk", [new ContractText(Contract()), new ContractText(Contract(), "Other/ManagedBridgeContract.inl")], out _, out ImmutableArray<Diagnostic> duplicate, out _);
        Check(duplicate.Any(static value => value.Id == "FBZZ1011"), "Multiple contract files must be rejected.");
        Run("Unknown", [new ContractText(Contract())], out _, out ImmutableArray<Diagnostic> invalid, out _);
        Check(invalid.Any(static value => value.Id == "FBZZ1012"), "Unknown target spellings must be rejected.");
    }

    private static void MalformedContractsAreDiagnosed()
    {
        string valid = "FBZZ_MANAGED_ENUM_BEGIN(NativeOp, BridgeOp, uint32_t, uint, HOST, public)\nFBZZ_MANAGED_ENUM_VALUE(TIME, TIME, 104)\nFBZZ_MANAGED_ENUM_END()\n";
        string[] malformed =
        [
            valid + "FBZZ_MANAGED_UNKNOWN()",
            valid.Replace("FBZZ_MANAGED_ENUM_END()", "FBZZ_MANAGED_ENUM_VALUE(OTHER, OTHER, 0x68)\nFBZZ_MANAGED_ENUM_END()", StringComparison.Ordinal),
            valid.Replace("FBZZ_MANAGED_ENUM_END()", string.Empty, StringComparison.Ordinal),
            valid + "FBZZ_MANAGED_STRUCT_BEGIN(NativeRequest, BridgeRequest)\nFBZZ_MANAGED_FIELD(uint32_t, uint, index, Index, 0)\nFBZZ_MANAGED_FIELD(uint32_t, uint, index, Index, 0)\nFBZZ_MANAGED_STRUCT_END()",
            valid + "FBZZ_MANAGED_STRUCT_BEGIN(NativeRequest, BridgeRequest)\nFBZZ_MANAGED_FIELD(uint32_t, uint, index, Index, 0)\nFBZZ_MANAGED_STRUCT_END()\nFBZZ_MANAGED_SIZE(NativeRequest, BridgeRequest, 4)",
            valid + "FBZZ_MANAGED_CALL(\"public\", \"float\", Read, \"\", TIME, \"scope.OwnerId\", \"\", \"return reply.X;)",
            valid + "FBZZ_MANAGED_CALL(\"public\", \"float\", Read, \"\", NOT_DECLARED, \"scope.OwnerId\", \"\", \"return reply.X;\")",
            valid + "FBZZ_MANAGED_CONST(uint32_t, uint, NativeValue, VALUE, 4294967296)",
            valid + "FBZZ_MANAGED_CONST(uint32_t, uint, NativeValue, VALUE, 1)\nFBZZ_MANAGED_CONST(uint32_t, uint, OtherNative, VALUE, 2)",
            valid.Replace("TIME, TIME, 104", "TIME, TIME, 0104", StringComparison.Ordinal),
            valid + "FBZZ_MANAGED_CONST(int32_t, int, NativeValue, VALUE, -0104)",
            valid + "FBZZ_MANAGED_CONST(uint64_t, uint, NativeValue, VALUE, 1)",
            valid.Replace("NativeOp, BridgeOp, uint32_t, uint", "NativeOp, BridgeOp, int32_t, uint", StringComparison.Ordinal),
            Contract().Replace("FIELD(int64_t, long, integer", "FIELD(int64_t, double, integer", StringComparison.Ordinal),
            Contract().Replace("FIELD(const char*, byte*, input", "FIELD(const char*, ulong, input", StringComparison.Ordinal),
            Contract().Replace("FIELD(ManagedBridgeOp, BridgeOp, op", "FIELD(ManagedBridgeOp, BridgeStatus, op", StringComparison.Ordinal),
            Contract().Replace("FBZZ_MANAGED_CALLBACK(UPDATE, \"Update\", \"\", \"live\")", "", StringComparison.Ordinal),
            Contract().Replace("FBZZ_MANAGED_CALLBACK(UPDATE, \"Update\", \"\", \"live\")", "FBZZ_MANAGED_CALLBACK(UNDECLARED, \"Update\", \"\", \"live\")", StringComparison.Ordinal),
            Contract().Replace("FBZZ_MANAGED_CALLBACK(UPDATE, \"Update\", \"\", \"live\")", "FBZZ_MANAGED_CALLBACK(UPDATE, \"Awake\", \"\", \"live\")", StringComparison.Ordinal),
            Contract().Replace("FBZZ_MANAGED_CALLBACK(UPDATE, \"Update\", \"\", \"live\")", "FBZZ_MANAGED_CALLBACK(UPDATE, \"Update\", \"Task\", \"live\")", StringComparison.Ordinal)
        ];
        foreach (string schema in malformed)
        {
            Run("Host", [new ContractText(schema)], out _, out ImmutableArray<Diagnostic> diagnostics, out GeneratorDriverRunResult result);
            Check(diagnostics.Any(static value => value.Id == "FBZZ1010" && value.Location.GetLineSpan().Path.EndsWith("ManagedBridgeContract.inl", StringComparison.Ordinal)),
                "A malformed contract must report its source line.");
            Check(result.Results.Single().GeneratedSources.IsEmpty, "An invalid contract must not emit a partial ABI.");
            Check(!diagnostics.Any(static value => value.Id is "AD0001" or "CS8785"), "Contract errors must be diagnosed without crashing the generator.");
        }
    }

    private static void QuotedCallFragmentsRemainIntact()
    {
        string schema = string.Join('\n', Contract().Split('\n').Where(static line => !line.StartsWith("FBZZ_MANAGED_CALL(", StringComparison.Ordinal))) + "\n" +
            """
            FBZZ_MANAGED_CALL("public", "string", ReadLabel, "", TIME, "scope.OwnerId", "", "return \"a,\\\"b\\\"\";");
            """;
        Run("Host", [new ContractText(schema)], out Compilation output, out ImmutableArray<Diagnostic> diagnostics, out GeneratorDriverRunResult result);
        Check(!diagnostics.Any(static value => value.Severity == DiagnosticSeverity.Error), "Commas and escaped quotes inside snippets must remain one argument.");
        Check(!output.GetDiagnostics().Any(static value => value.Severity == DiagnosticSeverity.Error), "Escaped snippets must produce legal C#.");
        string calls = result.Results.Single().GeneratedSources.Single(static value => value.HintName == "ManagedBridgeCalls.g.cs").SourceText.ToString();
        Check(calls.Contains("return \"a,\\\"b\\\"\";", StringComparison.Ordinal), "Generated C# must decode the contract string exactly once.");
    }

    private static void CrossTargetEnumNamesRemainIndependent()
    {
        string schema = Contract() + "\nFBZZ_MANAGED_ENUM_BEGIN(OtherNativeOp, BridgeOp, uint32_t, uint, SDK, public)\nFBZZ_MANAGED_ENUM_VALUE(OTHER, OTHER, 1)\nFBZZ_MANAGED_ENUM_END()\n";
        Assembly host = Compile("Host", schema, probe: true);
        Check((bool?)RequiredType(host, "Fbzz.ManagedHost.EntryPoint").GetMethod("RunProbe")?.Invoke(null, null) == true,
            "A same-named SDK enum must not change Host operation resolution.");
        Assembly sdk = Compile("Sdk", schema);
        CheckEnum(sdk, "Fbzz.BridgeOp", "OTHER=1");
    }

    private static void CheckEnum(Assembly assembly, string typeName, string specification)
    {
        Type type = RequiredType(assembly, typeName);
        Dictionary<string, long> expected = new(StringComparer.Ordinal);
        foreach (string item in specification.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            string[] pair = item.Split('=');
            expected.Add(pair[0], long.Parse(pair[1], System.Globalization.CultureInfo.InvariantCulture));
        }
        Check(Enum.GetNames(type).Length == expected.Count, "Unexpected enum member count: " + typeName);
        foreach ((string name, long value) in expected)
            Check(Convert.ToInt64(Enum.Parse(type, name)) == value, "Incorrect enum value: " + typeName + "." + name);
    }

    private static void CheckLayout(Assembly assembly, Type abi, string name, string prefix, int size, string specification)
    {
        Type type = RequiredType(assembly, "Fbzz.ManagedHost." + name);
        Check(type.StructLayoutAttribute is { Value: LayoutKind.Sequential, Pack: 8 }, "Wire structs must use sequential Pack=8.");
        Check(Marshal.SizeOf(type) == size && Convert.ToInt32(Constant(abi, prefix + "_SIZE")) == size, "Incorrect wire size: " + name);
        Dictionary<string, int> expected = Pairs(specification);
        Check(type.GetFields().Length == expected.Count, "Every wire field must have a pinned offset.");
        foreach ((string field, int offset) in expected)
        {
            Check(Marshal.OffsetOf(type, field).ToInt32() == offset, "Incorrect runtime offset: " + name + "." + field);
            string constant = prefix + "_" + string.Concat(field.Select((character, index) => index > 0 && char.IsUpper(character) ? "_" + character : character.ToString())).ToUpperInvariant() + "_OFFSET";
            Check(Convert.ToInt32(Constant(abi, constant)) == offset, "Incorrect generated offset constant: " + constant);
        }
        object instance = Activator.CreateInstance(type) ?? throw new InvalidOperationException("A wire struct must have a zero default.");
        Check(Convert.ToUInt64(type.GetField("Generation")?.GetValue(instance)) == 0, "The C# wire default must remain zero initialized.");
    }

    private static Dictionary<string, int> Pairs(string specification) => specification.Split(' ').Select(static value => value.Split('='))
        .ToDictionary(static value => value[0], static value => int.Parse(value[1], System.Globalization.CultureInfo.InvariantCulture), StringComparer.Ordinal);
    private static object Constant(Type type, string name) => type.GetField(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)?.GetRawConstantValue()
        ?? throw new InvalidOperationException("Missing constant: " + name);
    private static Type RequiredType(Assembly assembly, string name) => assembly.GetType(name)
        ?? throw new InvalidOperationException("Missing generated type: " + name);

    private static Assembly Compile(string target, string contract, bool probe = false)
    {
        Run(target, [new ContractText(contract)], out Compilation output, out ImmutableArray<Diagnostic> diagnostics, out _, probe);
        Check(!diagnostics.Any(static value => value.Severity == DiagnosticSeverity.Error), "Canonical contract diagnostics: " + string.Join("; ", diagnostics));
        using MemoryStream stream = new();
        EmitResult result = output.Emit(stream);
        Check(result.Success, "Generated bindings must compile: " + string.Join("; ", result.Diagnostics));
        return Assembly.Load(stream.ToArray());
    }

    private static void Run(string? target, AdditionalText[] contracts, out Compilation output, out ImmutableArray<Diagnostic> diagnostics,
        out GeneratorDriverRunResult result, bool probe = false)
    {
        SyntaxTree stub = CSharpSyntaxTree.ParseText(SourceText.From(target == "Host" ? HOST_STUB + (probe ? PROBE : string.Empty) : SDK_STUB, Encoding.UTF8), PARSE_OPTIONS);
        CSharpCompilation input = CSharpCompilation.Create("Binding.Generated." + (target ?? "Game"), [stub], REFERENCES,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true, nullableContextOptions: NullableContextOptions.Enable));
        GeneratorDriver driver = CSharpGeneratorDriver.Create([new ManagedBridgeSourceGenerator().AsSourceGenerator()], contracts, PARSE_OPTIONS, new BindingOptions(target));
        driver = driver.RunGeneratorsAndUpdateCompilation(input, out output, out diagnostics);
        result = driver.GetRunResult();
    }

    private static string Contract()
    {
        using Stream stream = typeof(BindingGeneratorTests).Assembly.GetManifestResourceStream("Fbzz.ManagedBridgeContract")
            ?? throw new InvalidOperationException("Embed the canonical ManagedBridgeContract.inl in the generator tests.");
        using StreamReader reader = new(stream, Encoding.UTF8);
        return reader.ReadToEnd();
    }

    private static ImmutableArray<MetadataReference> PlatformReferences()
    {
        string platform = AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string
            ?? throw new InvalidOperationException("The runtime must provide platform references.");
        return platform.Split(Path.PathSeparator).Where(static path => !Path.GetFileName(path).StartsWith("Fbzz.", StringComparison.Ordinal))
            .Select(static path => (MetadataReference)MetadataReference.CreateFromFile(path)).ToImmutableArray();
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private sealed class ContractText(string source, string path = "ManagedBridgeContract.inl") : AdditionalText
    {
        public override string Path => path;
        public override SourceText GetText(CancellationToken cancellationToken = default) => SourceText.From(source, Encoding.UTF8);
    }

    private sealed class BindingOptions(string? target) : AnalyzerConfigOptionsProvider
    {
        private readonly Options m_global = new(target);
        private readonly Options m_empty = new(null);
        public override AnalyzerConfigOptions GlobalOptions => m_global;
        public override AnalyzerConfigOptions GetOptions(SyntaxTree tree) => m_empty;
        public override AnalyzerConfigOptions GetOptions(AdditionalText textFile) => m_empty;
    }

    private sealed class Options(string? target) : AnalyzerConfigOptions
    {
        public override bool TryGetValue(string key, out string value)
        {
            if (key == "build_property.FbzzBindingTarget" && target is not null) { value = target; return true; }
            value = string.Empty;
            return false;
        }
    }

    private const string SDK_STUB = "namespace Fbzz { public readonly struct Collision { } public sealed class GameObject { } public readonly struct AnimationEvent { } public readonly struct RootMotionInfo { } public struct SequenceEvent { } }";

    private const string HOST_STUB = """
        using System;
        namespace Fbzz
        {
            public readonly record struct Vector3(float X, float Y, float Z);
            public readonly record struct Quaternion(float X, float Y, float Z, float W);
            public enum KeyCode : uint { W = 87 }
            internal enum PhysicsWorldValue : uint { GRAVITY = 0, LAYERS_COLLIDE = 1 }
            internal sealed class ScriptExecutionScope
            {
                internal ulong Generation = 7, InstanceId = 9, OwnerId = 11, CallId = 13;
            }
        }
        namespace Fbzz.ManagedHost
        {
            public static unsafe partial class EntryPoint
            {
                static BridgeRequest s_last;
                static int s_dispatches;
                static bool s_fail;
                sealed partial class NativeBridge
                {
                    BridgeRequest Request(Fbzz.ScriptExecutionScope scope, ulong target, BridgeOp operation) => new()
                    {
                        AbiVersion = BridgeAbi.VERSION, Op = operation, Generation = scope.Generation,
                        InstanceId = scope.InstanceId, OwnerId = scope.OwnerId, CallId = scope.CallId, Integer = (long)target
                    };
                    BridgeReply Call(ref BridgeRequest request)
                    {
                        s_last = request; s_dispatches++;
                        if (s_fail) throw new InvalidOperationException("BUFFER_TOO_SMALL");
                        return new() { X = 1, Y = 2, Z = 3, W = 4, Integer = 1 };
                    }
                }
            }
        }
        """;

    private const string PROBE = """
        namespace Fbzz.ManagedHost
        {
            public static unsafe partial class EntryPoint
            {
                public static bool RunProbe()
                {
                    NativeBridge bridge = new(); Fbzz.ScriptExecutionScope scope = new();
                    Fbzz.Vector3 position = bridge.ReadPosition(scope, 99, true);
                    if (position != new Fbzz.Vector3(1, 2, 3) || s_last.Op != BridgeOp.TRANSFORM_READ || s_last.Index != 1 || s_last.Integer != 99
                        || s_last.Generation != 7 || s_last.InstanceId != 9 || s_last.OwnerId != 11 || s_last.CallId != 13 || s_last.AbiVersion != 1) return false;
                    bridge.WritePosition(scope, 101, new Fbzz.Vector3(4, 5, 6), true);
                    if (s_last.Op != BridgeOp.TRANSFORM_WRITE_WORLD || s_last.Integer != 101 || s_last.X != 4 || s_last.Y != 5 || s_last.Z != 6) return false;
                    bridge.TranslateLocal(scope, 103, new Fbzz.Vector3(7, 8, 9));
                    if (s_last.Op != BridgeOp.TRANSFORM_TRANSLATE_LOCAL || s_last.Integer != 103 || s_last.X != 7 || s_last.Y != 8 || s_last.Z != 9) return false;
                    if (!bridge.ReadKey(scope, Fbzz.KeyCode.W, 2) || s_last.Op != BridgeOp.INPUT_KEY || s_last.Integer != 87 || s_last.Index != 2) return false;
                    if (bridge.ProbeDelta(scope) != 1 || s_last.Op != BridgeOp.TIME || s_last.Integer != 11 || s_dispatches != 5) return false;
                    if (bridge.ReadRotation(scope, 202, true) != new Fbzz.Quaternion(1, 2, 3, 4)
                        || s_last.Op != BridgeOp.TRANSFORM_READ_ROTATION || s_last.Index != 1 || s_last.Integer != 202) return false;
                    bridge.WriteLocalRotation(scope, 202, new Fbzz.Quaternion(4, 5, 6, 7));
                    if (s_last.Op != BridgeOp.TRANSFORM_WRITE_LOCAL_ROTATION || s_last.X != 4 || s_last.Y != 5 || s_last.Z != 6 || s_last.W != 7) return false;
                    if (bridge.ReadLocalScale(scope, 202) != new Fbzz.Vector3(1, 2, 3) || s_last.Op != BridgeOp.TRANSFORM_READ_SCALE) return false;
                    bridge.WriteLocalScale(scope, 202, new Fbzz.Vector3(0, -2, 3));
                    if (s_last.Op != BridgeOp.TRANSFORM_WRITE_LOCAL_SCALE || s_last.X != 0 || s_last.Y != -2 || s_last.Z != 3) return false;
                    if (bridge.ReadDirection(scope, 202, 2) != new Fbzz.Vector3(1, 2, 3) || s_last.Op != BridgeOp.TRANSFORM_READ_DIRECTION || s_last.Index != 2) return false;
                    bridge.Rotate(scope, 202, new Fbzz.Vector3(1, 0, 0), 90);
                    if (s_last.Op != BridgeOp.TRANSFORM_ROTATE || s_last.X != 1 || s_last.Y != 0 || s_last.Z != 0 || s_last.W != 90) return false;
                    bridge.LookAt(scope, 202, new Fbzz.Vector3(7, 8, 9));
                    if (s_last.Op != BridgeOp.TRANSFORM_LOOK_AT || s_last.X != 7 || s_last.Y != 8 || s_last.Z != 9
                        || s_last.Integer != 202 || s_last.OwnerId != 11 || s_last.Generation != 7 || s_dispatches != 12) return false;
                    if (bridge.ReadPhysicsVector(scope, 303, 1) != new Fbzz.Vector3(1, 2, 3)
                        || s_last.Op != BridgeOp.PHYSICS_BODY_READ || s_last.Index != 1 || s_last.Integer != 303) return false;
                    if (bridge.ReadPhysicsScalar(scope, 303, 3) != 1 || s_last.Index != 3) return false;
                    if (!bridge.ReadPhysicsFlag(scope, 303, 7) || s_last.Index != 7) return false;
                    bridge.WritePhysicsVector(scope, 303, 0, new Fbzz.Vector3(4, 5, 6));
                    if (s_last.Op != BridgeOp.PHYSICS_BODY_WRITE || s_last.X != 4 || s_last.Y != 5 || s_last.Z != 6) return false;
                    bridge.WritePhysicsScalar(scope, 303, 4, -2);
                    if (s_last.Index != 4 || s_last.X != -2) return false;
                    bridge.WritePhysicsFlag(scope, 303, 5, true);
                    if (s_last.Index != 5 || s_last.X != 1) return false;
                    if (bridge.ReadWorldGravity(scope) != new Fbzz.Vector3(1, 2, 3) || s_last.Op != BridgeOp.PHYSICS_WORLD_READ || s_last.Integer != 0) return false;
                    if (!bridge.LayersCollide(scope, 2, 31) || s_last.Index != 1 || s_last.X != 2 || s_last.Y != 31 || s_dispatches != 20) return false;
                    s_fail = true;
                    try { bridge.WritePosition(scope, 101, default, false); return false; }
                    catch (InvalidOperationException) { return s_dispatches == 21 && s_last.Op == BridgeOp.TRANSFORM_WRITE_LOCAL; }
                }
                sealed partial class NativeBridge
                {
                    public float ProbeDelta(Fbzz.ScriptExecutionScope scope) => ReadDeltaSecondsNative(scope);
                }
            }
        }
        """;
}
