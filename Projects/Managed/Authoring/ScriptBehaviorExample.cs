/// @file    ScriptBehaviorExample.cs
/// @brief   Custom danger rules, arbitrary targets and replaceable continuous movement without per-frame BT callbacks.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;
using Fbzz;
using Fbzz.AI;

namespace Fbzz.Samples;

public enum SwimIntent { CRUISE, SEEK, FLEE }

/// @note A game can replace this strategy without changing the BT or C++. Continuous movement runs outside decision callbacks.
public interface ISwimMovement
{
    void SetIntent(SwimIntent intent, Vector3 target);
    void ClearIntent();
    void Step(double deltaSeconds);
}

/// @note One definition can serve many agents. This helper belongs to the game assembly; release references when the owner is destroyed or reloaded.
public sealed class ScriptBehaviorExample
{
    public readonly BlackboardKey<float> Fear;
    public readonly BlackboardKey<float> FearThreshold;
    public readonly BlackboardKey<bool> Alarm;
    public readonly BlackboardKey<bool> HasTarget;
    public readonly BlackboardKey<Vector3> Target;
    public readonly BlackboardKey<Vector3> Threat;
    public readonly BlackboardKey<ISwimMovement?> Movement;
    public readonly BehaviorTree Tree;

    public ScriptBehaviorExample()
    {
        BlackboardSchema.Builder schema = new();
        Fear = schema.Add<float>("Fear");
        FearThreshold = schema.Add("FearThreshold", 0.65f);
        Alarm = schema.Add<bool>("Alarm");
        HasTarget = schema.Add<bool>("HasTarget");
        Target = schema.Add<Vector3>("Target");
        Threat = schema.Add<Vector3>("Threat");
        Movement = schema.Add<ISwimMovement?>("Movement");
        Tree = new(schema.Build(), Bt.ReactiveSelector(
            Bt.Guard(IsDangerous, Intent(SwimIntent.FLEE, Threat), "Custom danger"),
            Bt.Guard((BtContext context) => context.Blackboard.Get(HasTarget), Intent(SwimIntent.SEEK, Target), "Target available"),
            Intent(SwimIntent.CRUISE, Target)));
    }

    public BehaviorTreeInstance CreateAgent(ISwimMovement movement, double intervalSeconds = 0.1)
    {
        ArgumentNullException.ThrowIfNull(movement);
        BehaviorTreeInstance agent = Tree.CreateInstance(intervalSeconds);
        agent.Blackboard.Set(Movement, movement);
        return agent;
    }

    /// @note Alarm/Fear changes wake the next Tick even with a far-distance decision interval. A supplied target can be a waypoint, player, prey or script-selected position.
    public void SetPerception(BehaviorTreeInstance agent, float fear, bool alarm, Vector3 threat)
    {
        agent.Blackboard.Set(Threat, threat, requestEvaluation: false);
        agent.Blackboard.Set(Fear, fear);
        agent.Blackboard.Set(Alarm, alarm);
    }

    /// @note Call in the owner's Update. The strategy retains the last intent between evaluations, while habitat/obstacle/acceleration constraints remain active every movement step.
    public void UpdateAgent(BehaviorTreeInstance agent, double deltaSeconds)
    {
        agent.Tick(deltaSeconds);
        agent.Blackboard.Get(Movement)?.Step(deltaSeconds);
    }

    bool IsDangerous(BtContext context) => context.Blackboard.Get(Alarm)
        || context.Blackboard.Get(Fear) >= context.Blackboard.Get(FearThreshold);

    BtNode Intent(SwimIntent intent, BlackboardKey<Vector3> target) => Bt.Action(
        (BtContext context) =>
        {
            ISwimMovement? movement = context.Blackboard.Get(Movement);
            if (movement is null) return BtStatus.FAILURE;
            movement.SetIntent(intent, context.Blackboard.Get(target));
            return BtStatus.RUNNING;
        },
        (BtContext context, BtExitReason reason) => context.Blackboard.Get(Movement)?.ClearIntent(),
        intent.ToString());
}
