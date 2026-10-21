namespace HEngine.AndroidHost;

// Pure input model: stable pointer ownership, radial clamping, explicit cancel.
// Rendering and hit testing use the same normalized control layout.
public sealed class TouchRouter
{
    public readonly record struct Snapshot(float X, float Y, float MoveX, float MoveY, int Buttons);
    private enum Role { Primary, Orbit, Stick, Jump, Pause, Ignored }
    private readonly Dictionary<int, Role> mPointers = new();
    private int mStick = -1, mPrimary = -1, mOrbit = -1;
    private float mOriginX, mOriginY, mWidth = 1, mHeight = 1;
    private Snapshot mState;
    private bool bControls;
    public Snapshot State => mState;
    public float StickRadius => Math.Min(mWidth, mHeight) * .12f;
    public float StickX => mWidth * .16f;
    public float StickY => mHeight * .78f;
    public float JumpX => mWidth * .87f;
    public float JumpY => mHeight * .78f;
    public float PauseX => mWidth * .94f;
    public float PauseY => mHeight * .09f;
    public void Resize(float width, float height)
    {
        if (!float.IsFinite(width) || !float.IsFinite(height) || width <= 0 || height <= 0)
            throw new ArgumentOutOfRangeException(nameof(width));
        mWidth = width; mHeight = height; Cancel();
    }
    public void EnableControls(bool enabled) { if (enabled != bControls) { bControls = enabled; Cancel(); } }
    private bool Hit(float x, float y, float cx, float cy, float radius)
        => (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
    public void Down(int id, float x, float y)
    {
        if (id < 0 || mPointers.ContainsKey(id) || mPointers.Count >= 16 || !float.IsFinite(x) || !float.IsFinite(y)) return;
        Role role = Role.Ignored;
        if (bControls && Hit(x, y, JumpX, JumpY, StickRadius * .8f)) role = Role.Jump;
        else if (bControls && Hit(x, y, PauseX, PauseY, StickRadius * .6f)) role = Role.Pause;
        else if (bControls && mStick < 0 && x < mWidth * .4f && y > mHeight * .45f)
        {
            role = Role.Stick; mStick = id; mOriginX = x; mOriginY = y;
        }
        else if (bControls && mOrbit < 0 && x >= mWidth * .4f)
        {
            role = Role.Orbit; mOrbit = id;
        }
        else if (mPrimary < 0) { role = Role.Primary; mPrimary = id; }
        mPointers.Add(id, role); Move(id, x, y); UpdateButtons();
    }
    public void Move(int id, float x, float y)
    {
        if (!float.IsFinite(x) || !float.IsFinite(y) || !mPointers.TryGetValue(id, out var role)) return;
        if (role is Role.Primary or Role.Orbit) mState = mState with { X = x, Y = y };
        if (role != Role.Stick) return;
        float dx = (x - mOriginX) / StickRadius, dy = (mOriginY - y) / StickRadius;
        float magnitude = MathF.Sqrt(dx * dx + dy * dy);
        if (magnitude < .12f) dx = dy = 0;
        else { float scale = Math.Min(1, (magnitude - .12f) / .88f) / magnitude; dx *= scale; dy *= scale; }
        mState = mState with { MoveX = dx, MoveY = dy };
    }
    public void Up(int id, float x, float y)
    {
        Move(id, x, y);
        if (!mPointers.Remove(id, out var role)) return;
        if (role == Role.Stick) { mStick = -1; mState = mState with { MoveX = 0, MoveY = 0 }; }
        if (role == Role.Primary) mPrimary = -1;
        if (role == Role.Orbit) mOrbit = -1;
        UpdateButtons();
    }
    private void UpdateButtons()
    {
        int buttons = (mPrimary >= 0 ? 1 : 0) | (mOrbit >= 0 ? 2 : 0);
        if (mPointers.ContainsValue(Role.Jump)) buttons |= 4;
        if (mPointers.ContainsValue(Role.Pause)) buttons |= 8;
        mState = mState with { Buttons = buttons };
    }
    public void Cancel() { mPointers.Clear(); mStick = mPrimary = mOrbit = -1; mState = default; }
}
