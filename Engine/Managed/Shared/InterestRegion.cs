namespace HEngine.Platform;

// Host presentation filter; does not mutate the authoritative world.
public sealed class InterestRegion
{
    public double Radius { get; private set; }
    public void Configure(double radius)
    {
        if (!double.IsFinite(radius) || radius < 0) throw new ArgumentOutOfRangeException(nameof(radius));
        Radius = radius;
    }
    public bool Contains(double x, double z, double centerX, double centerZ)
        => Radius == 0 || (x-centerX)*(x-centerX)+(z-centerZ)*(z-centerZ) <= Radius*Radius;
}
