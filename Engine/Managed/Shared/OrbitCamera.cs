namespace HEngine.Platform;

/// <summary>Host-independent orbit input, suitable for mouse drags or touch gestures.</summary>
public sealed class OrbitCamera
{
    public double Yaw { get; private set; }
    public double Elevation { get; private set; } = System.Math.Atan2(14, 17);
    private double mHorizontalSensitivity = .006, mVerticalSensitivity = .004;
    private double mMinElevation = .25, mMaxElevation = 1.35;
    public void Configure(double elevation, double sensitivity, double minElevation, double maxElevation, double yaw = 0)
    {
        if (!double.IsFinite(yaw) || !double.IsFinite(elevation) || !double.IsFinite(sensitivity) || sensitivity <= 0 ||
            !double.IsFinite(minElevation) || !double.IsFinite(maxElevation) || minElevation > maxElevation)
            throw new System.ArgumentException("Invalid orbit camera configuration.");
        mHorizontalSensitivity = mVerticalSensitivity = sensitivity;
        mMinElevation = minElevation; mMaxElevation = maxElevation;
        Elevation = System.Math.Clamp(elevation, minElevation, maxElevation);
        Yaw = System.Math.IEEERemainder(yaw, System.Math.PI * 2);
    }
    public void Drag(double horizontal, double vertical)
    {
        if (!double.IsFinite(horizontal) || !double.IsFinite(vertical)) throw new System.ArgumentException("Camera input must be finite.");
        Yaw = System.Math.IEEERemainder(Yaw - horizontal * mHorizontalSensitivity, System.Math.PI * 2);
        Elevation = System.Math.Clamp(Elevation + vertical * mVerticalSensitivity, mMinElevation, mMaxElevation);
    }
    public (double X, double Z) Movement(double x, double z) => (x * System.Math.Cos(Yaw) + z * System.Math.Sin(Yaw), -x * System.Math.Sin(Yaw) + z * System.Math.Cos(Yaw));
}
