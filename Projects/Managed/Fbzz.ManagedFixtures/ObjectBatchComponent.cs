/// @file    ObjectBatchComponent.cs
/// @brief   通常の配列から一括 Transform API を実 CoreCLR で呼ぶ fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using Fbzz;

namespace ManagedFixtures;

class ObjectBatchComponent : Script
{
    public GameObject[] targets = Array.Empty<GameObject>();
    public Vector3[] positions = Array.Empty<Vector3>();
    public Vector3 displacement = new Vector3(1f, 0f, 0f);
    public int operation;
    public int repeatCount;
    public int poisonIndex = -1;
    public bool overflowDisplacement;
    public bool activeOnly;
    public bool succeeded;
    public int resultStatus;
    public int inputCount;
    public int appliedCount;
    public int skippedCount;
    public int[] observedStatuses = Array.Empty<int>();

    static GameObject m_foreign;
    ObjectStatus[] m_statuses = Array.Empty<ObjectStatus>();

    protected override void Update()
    {
        GameObject[] objects = targets;
        if (repeatCount > 0)
        {
            objects = new GameObject[repeatCount];
            Array.Fill(objects, targets[0]);
        }
        m_statuses = new ObjectStatus[objects.Length + 2];
        Array.Fill(m_statuses, (ObjectStatus)99);
        inputCount = objects.Length;
        appliedCount = 0;
        skippedCount = 0;
        resultStatus = 0;
        switch (operation)
        {
            case 0:
                succeeded = Objects.TryReadWorldPositions(objects, positions, m_statuses);
                break;
            case 1:
                Vector3[] values = (Vector3[])positions.Clone();
                if (poisonIndex >= 0) values[poisonIndex] = new Vector3(float.NaN, 0f, 0f);
                succeeded = Objects.TrySetWorldPositions(objects, values, m_statuses);
                break;
            case 2:
                Vector3 delta = overflowDisplacement
                    ? new Vector3(float.MaxValue * 0.75f, 0f, 0f) : displacement;
                Capture(Objects.TranslateWorld(objects, delta, activeOnly));
                break;
            case 3:
                m_foreign = targets[0];
                succeeded = true;
                break;
            case 4:
                Capture(Objects.TranslateWorld(new GameObject[] { targets[0], m_foreign }, displacement));
                break;
        }
        /// @note 観測する status は先頭 8 件だけ。この表示用要約を元の集合件数の上限にしない。
        int observedCount = Math.Min(m_statuses.Length, 8);
        observedStatuses = new int[observedCount];
        for (int i = 0; i < observedCount; i++) observedStatuses[i] = (int)m_statuses[i];
    }

    void Capture(BatchResult result)
    {
        succeeded = result.Succeeded;
        resultStatus = (int)result.Status;
        inputCount = result.InputCount;
        appliedCount = result.AppliedCount;
        skippedCount = result.SkippedCount;
    }
}
