/// @file    FaultRecoveryComponent.cs
/// @brief   明示復帰の段階保持・必須設定再検証・購読解除・長診断を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using Fbzz;

namespace ManagedFixtures;

public sealed class FaultRecoveryComponent : Script
{
    [RequiredReference] public GameObject target;
    public int failPhase;
    public bool longMessage;
    public bool invalidMessage;
    public int awakeCount;
    public int startCount;
    public int enableCount;
    public int connectCount;
    public int updateCount;
    public int jobCount;
    public int eventCount;
    public int command;
    public bool clearRequiredOnRoutineDispose;
    public int routineStartCount;
    public int routineDisposeCount;

    void Fail(int phase)
    {
        if (failPhase != phase) return;
        string detail = invalidMessage ? "invalid \uD800 message" : longMessage ? new string('障', 9000) : "repair before resuming";
        throw new InvalidOperationException("FaultRecovery phase " + phase + ": " + detail);
    }
    protected override void Awake()
    {
        awakeCount++;
        if (clearRequiredOnRoutineDispose) { Jobs.Run(new RequiredClearingRoutine(this)); target = null; }
        Fail(1);
    }
    /// @note 未開始で破棄しても設定を変える後始末を検証する。compiler iterator の finally 開始条件に依存しない。
    sealed class RequiredClearingRoutine : IEnumerator<Wait>
    {
        readonly FaultRecoveryComponent m_owner;
        bool m_disposed;
        internal RequiredClearingRoutine(FaultRecoveryComponent owner) { m_owner = owner; }
        public Wait Current => Wait.ForSeconds(100f);
        object System.Collections.IEnumerator.Current => Current;
        public bool MoveNext()
        {
            if (m_disposed) return false;
            m_owner.routineStartCount++;
            return true;
        }
        public void Reset() => throw new NotSupportedException();
        public void Dispose()
        {
            if (m_disposed) return;
            m_disposed = true;
            m_owner.routineDisposeCount++;
            m_owner.target = null;
        }
    }
    protected override void Start() { startCount++; Fail(2); }
    protected override void OnEnable() { enableCount++; Fail(5); }
    protected override void Connect()
    {
        connectCount++;
        Events.Subscribe<int>((int value) => { eventCount += value; Fail(7); });
        Fail(3);
    }
    protected override void Update()
    {
        updateCount++;
        if (command == 1) { command = 0; Events.Publish(1); }
        else if (command == 2) { command = 0; Jobs.WaitFrames(2, () => { jobCount++; Fail(6); }); }
        Fail(4);
    }
}
