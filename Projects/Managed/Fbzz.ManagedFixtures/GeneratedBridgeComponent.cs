/// @file    GeneratedBridgeComponent.cs
/// @brief   生成された位置・入力・時間接続を実 native callback から検証する fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

namespace ManagedFixtures;

class GeneratedBridgeComponent : Script
{
    public Vector3 localDestination = new Vector3(2f, 3f, 4f);
    public Vector3 worldDestination = new Vector3(15f, 7f, 9f);
    public Vector3 localVelocity = new Vector3(0f, 0f, 4f);
    public Vector3 initialLocal;
    public Vector3 initialWorld;
    public int operation;
    public bool held;
    public bool down;
    public bool up;
    public bool otherHeld;
    public float deltaSeconds;
    public int updateCount;
    public string buttonName = "Jump";
    public string axisName = "MoveX";
    public bool buttonExists;
    public bool axisExists;
    public bool buttonNameListed;
    public bool axisNameListed;
    public float axisValue;
    ButtonAction m_button;
    AxisAction m_axis;

    protected override void Update()
    {
        updateCount++;
        initialLocal = Transform.LocalPosition;
        initialWorld = Transform.WorldPosition;
        deltaSeconds = Time.DeltaSeconds;
        held = Input.GetKey(KeyCode.W);
        down = Input.GetKeyDown(KeyCode.W);
        up = Input.GetKeyUp(KeyCode.W);
        otherHeld = Input.GetKey(KeyCode.A);
        /// @note 書き込み後に getter で同期を補わず、native 側で子孫の即時同期を観測する。
        switch (operation)
        {
            case 0: Transform.LocalPosition = localDestination; break;
            case 1: Transform.WorldPosition = worldDestination; break;
            case 2: Transform.TranslateLocal(localVelocity * deltaSeconds); break;
            case 3:
                buttonExists = Input.HasButton(buttonName);
                axisExists = Input.HasAxis(axisName);
                buttonNameListed = System.Array.IndexOf(Input.GetButtonNames(), buttonName) >= 0;
                axisNameListed = System.Array.IndexOf(Input.GetAxisNames(), axisName) >= 0;
                m_button = Input.Button(buttonName);
                m_axis = Input.Axis(axisName);
                held = m_button.Held;
                down = m_button.Pressed;
                up = m_button.Released;
                axisValue = m_axis.Value;
                break;
            case 4: held = m_button.Held; break;
            case 5: axisValue = Input.Axis(axisName).Value; break;
        }
    }
}
