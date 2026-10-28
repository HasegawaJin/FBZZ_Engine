/// @file    NativeAiCrowdBridge.cs
/// @brief   Reusable numeric packets for callback-scoped crowd simulation.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;
using System.IO;
using System.Runtime.InteropServices;
using Fbzz;
using Fbzz.AI;

namespace Fbzz.ManagedHost;

public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge : IScriptAiCrowdNativeBridge
    {
        byte[] m_crowdInput = [], m_crowdOutput = [];
        bool m_crowdInFlight;

        /// @note Pack=4 numeric payloads are copied, never retained; buffers grow only when the largest batch grows.
        /// @see https://learn.microsoft.com/dotnet/api/system.runtime.interopservices.memorymarshal.asbytes Numeric struct byte spans.
        public bool StepCrowd(ScriptExecutionScope scope, AiCrowdSettings settings, ReadOnlySpan<AiCrowdProfile> profiles,
            ReadOnlySpan<AiCrowdAgent> agents, ReadOnlySpan<AiCrowdThreat> threats, ReadOnlySpan<AiCrowdObstacle> obstacles,
            Span<AiCrowdAgent> output, out AiCrowdStats stats)
        {
            stats = default;
            if (scope.Identity == 0 || scope.ThreadId != Environment.CurrentManagedThreadId || scope.CallId == 0 || m_crowdInFlight)
                throw new InvalidOperationException("AiCrowd requires the current callback thread and a nonreentrant bridge.");
            if (!BitConverter.IsLittleEndian || sizeof(AiCrowdSettings) != AiCrowdWire.SETTINGS_BYTES
                || sizeof(AiCrowdProfile) != AiCrowdWire.PROFILE_BYTES || sizeof(AiCrowdAgent) != AiCrowdWire.AGENT_BYTES
                || sizeof(AiCrowdThreat) != AiCrowdWire.THREAT_BYTES || sizeof(AiCrowdObstacle) != AiCrowdWire.OBSTACLE_BYTES
                || sizeof(AiCrowdStats) != AiCrowdWire.STATS_BYTES)
                throw new InvalidDataException("AiCrowd numeric layouts do not match the native packet.");
            if (agents.Length > AiCrowdWire.MAX_AGENTS || profiles.Length < 1 || profiles.Length > AiCrowdWire.MAX_PROFILES
                || threats.Length > AiCrowdWire.MAX_THREATS || obstacles.Length > AiCrowdWire.MAX_OBSTACLES || output.Length < agents.Length)
                return false;
            int inputBytes = AiCrowdWire.HEADER_BYTES + AiCrowdWire.SETTINGS_BYTES + profiles.Length * AiCrowdWire.PROFILE_BYTES
                + agents.Length * AiCrowdWire.AGENT_BYTES + threats.Length * AiCrowdWire.THREAT_BYTES + obstacles.Length * AiCrowdWire.OBSTACLE_BYTES;
            int outputBytes = AiCrowdWire.STATS_BYTES + agents.Length * AiCrowdWire.AGENT_BYTES;
            if (inputBytes > AiCrowdWire.MAX_PACKET_BYTES || outputBytes > AiCrowdWire.MAX_PACKET_BYTES) return false;
            if (m_crowdInput.Length < inputBytes) m_crowdInput = new byte[inputBytes];
            if (m_crowdOutput.Length < outputBytes) m_crowdOutput = new byte[outputBytes];
            Span<byte> input = m_crowdInput.AsSpan(0, inputBytes);
            Span<uint> header = MemoryMarshal.Cast<byte, uint>(input[..AiCrowdWire.HEADER_BYTES]);
            header[0] = AiCrowdWire.VERSION; header[1] = AiCrowdWire.SETTINGS_BYTES; header[2] = AiCrowdWire.PROFILE_BYTES;
            header[3] = AiCrowdWire.AGENT_BYTES; header[4] = AiCrowdWire.THREAT_BYTES; header[5] = AiCrowdWire.OBSTACLE_BYTES;
            header[6] = AiCrowdWire.STATS_BYTES; header[7] = (uint)agents.Length; header[8] = (uint)profiles.Length;
            header[9] = (uint)threats.Length; header[10] = (uint)obstacles.Length; header[11] = 0;
            int offset = AiCrowdWire.HEADER_BYTES;
            MemoryMarshal.Write(input[offset..], in settings); offset += AiCrowdWire.SETTINGS_BYTES;
            AppendCrowd(input, ref offset, profiles); AppendCrowd(input, ref offset, agents);
            AppendCrowd(input, ref offset, threats); AppendCrowd(input, ref offset, obstacles);
            m_crowdInFlight = true;
            try
            {
                fixed (byte* inputPointer = m_crowdInput)
                fixed (byte* outputPointer = m_crowdOutput)
                {
                    BridgeRequest request = Request(scope, 0, BridgeOp.AI_CROWD_STEP);
                    request.Input = inputPointer; request.InputBytes = (uint)inputBytes;
                    request.Output = outputPointer; request.OutputCapacity = (uint)outputBytes;
                    BridgeReply reply = default;
                    int returned = m_dispatch(&request, &reply);
                    if (returned != (int)reply.Status) throw new InvalidDataException("AiCrowd return code disagrees with the reply status.");
                    if (reply.Flags != 0 || reply.Reserved != 0) throw new InvalidDataException("AiCrowd returned unknown reply flags.");
                    if (reply.Status == BridgeStatus.INVALID_ARGUMENT)
                    {
                        if (reply.OutputBytes != 0 || reply.Index != 0) throw new InvalidDataException("AiCrowd failure returned partial output.");
                        return false;
                    }
                    RequireSuccess(returned, reply.Status, request.Op);
                    if (reply.OutputBytes != (uint)outputBytes || reply.Index != (uint)agents.Length)
                        throw new InvalidDataException("AiCrowd returned an invalid output length or agent count.");
                    ReadOnlySpan<byte> bytes = m_crowdOutput.AsSpan(0, outputBytes);
                    AiCrowdStats result = MemoryMarshal.Read<AiCrowdStats>(bytes);
                    ReadOnlySpan<AiCrowdAgent> snapshot = MemoryMarshal.Cast<byte, AiCrowdAgent>(bytes[AiCrowdWire.STATS_BYTES..]);
                    if (!ValidCrowdStats(result, settings, agents.Length)) throw new InvalidDataException("AiCrowd returned invalid statistics.");
                    foreach (ref readonly AiCrowdAgent agent in snapshot)
                        if (!ValidCrowdAgent(agent, agents.Length, profiles.Length)) throw new InvalidDataException("AiCrowd returned an invalid agent.");
                    /// @note Validate every slot before copying so a malformed later slot cannot partially change caller state.
                    snapshot.CopyTo(output); stats = result; return true;
                }
            }
            finally { m_crowdInFlight = false; }
        }

        static void AppendCrowd<T>(Span<byte> input, ref int offset, ReadOnlySpan<T> values) where T : unmanaged
        {
            ReadOnlySpan<byte> bytes = MemoryMarshal.AsBytes(values);
            bytes.CopyTo(input[offset..]); offset += bytes.Length;
        }

        static bool CrowdScalar(float value) => float.IsFinite(value) && MathF.Abs(value) <= 1000000;
        static bool CrowdPositive(float value) => CrowdScalar(value) && value >= 0;
        static bool CrowdVector(Vector3 value) => CrowdScalar(value.X) && CrowdScalar(value.Y) && CrowdScalar(value.Z);
        static bool ValidCrowdAgent(in AiCrowdAgent a, int count, int profiles)
            => CrowdVector(a.Position) && CrowdVector(a.Velocity) && CrowdVector(a.DesiredVelocity) && CrowdVector(a.Goal)
            && CrowdVector(a.Target) && CrowdVector(a.TargetVelocity) && CrowdVector(a.ExternalVelocity) && CrowdVector(a.DangerPosition)
            && CrowdScalar(a.Floor) && CrowdScalar(a.Ceiling) && a.Floor <= a.Ceiling && CrowdPositive(a.Fear) && a.Fear <= 1
            && CrowdScalar(a.DecisionIn) && CrowdPositive(a.DecisionAge) && CrowdPositive(a.ThreatIn) && CrowdPositive(a.StuckSeconds)
            && a.Profile < (uint)profiles && (uint)a.Intent <= 6 && (uint)a.Mode <= 4 && ((uint)a.Flags & ~7U) == 0
            && ((uint)a.Events & ~63U) == 0 && a.SensedIndex >= -1 && a.SensedIndex < count && CrowdPositive(a.SensedDistance);
        static bool ValidCrowdStats(AiCrowdStats s, AiCrowdSettings settings, int count)
            => s.Decisions <= (uint)count && s.Decisions <= settings.MaximumDecisions
            && s.CandidateTests <= (ulong)s.Decisions * settings.CandidateLimit && s.Neighbors <= (ulong)s.Decisions * settings.NeighborLimit
            && (ulong)s.Decisions + s.DeferredDecisions <= (uint)count && s.Constraints <= (uint)count && s.Recoveries <= (uint)count
            && s.ThreatReactions <= (uint)count && s.OccupiedCells <= (uint)count && (count == 0 ? s.DecisionCursor == 0 : s.DecisionCursor < (uint)count)
            && CrowdPositive(s.MaximumAcceleration) && CrowdPositive(s.MaximumTurn);
    }
}
