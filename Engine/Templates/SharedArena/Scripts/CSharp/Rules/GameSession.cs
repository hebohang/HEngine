using System.Text.Json;
using System.Text.Json.Nodes;
using HEngine;
using HEngine.Portable;

namespace GameScripts.Rules;

// Pure rules: compiled from this same file for Windows, Android, Web and WeChat.
public sealed class GameSession : IGameSession
{
    private sealed class Enemy { public int id; public double x, y; }
    private readonly List<Enemy> mEnemies = new();
    private readonly double mPlayerSpeed, mEnemySpeed, mDuration;
    private readonly string mTitle;
    private string mMode = "menu";
    private double mX, mY, mElapsed, mSpawnTimer, mAttackTimer, mRange = 3.3;
    private int mHealth = 10, mKills, mNextId, mLevel = 1, mBest;
    private long mSaveRevision, mSoundRevision;
    private uint mRandom = 76431;

    public GameSession(string catalog, string save)
    {
        var settings = JsonNode.Parse(catalog)!;
        mTitle = settings["title"]!.GetValue<string>();
        mPlayerSpeed = settings["playerSpeed"]!.GetValue<double>();
        mEnemySpeed = settings["enemySpeed"]!.GetValue<double>();
        mDuration = settings["duration"]!.GetValue<double>();
        if (!double.IsFinite(mPlayerSpeed) || mPlayerSpeed <= 0 || !double.IsFinite(mEnemySpeed) || mEnemySpeed <= 0 ||
            !double.IsFinite(mDuration) || mDuration < 10) throw new ArgumentException("Invalid game rules.");
        if (!string.IsNullOrEmpty(save))
        {
            var data = JsonNode.Parse(save)!;
            if (data["schema"]?.GetValue<int>() != 1) throw new ArgumentException("Unsupported save schema.");
            mBest = Math.Max(0, data["best"]!.GetValue<int>());
        }
    }
    public string Command(string name, string value)
    {
        if (name is "start" or "restart")
        {
            mMode = "playing"; mX = -2; mY = 0; mElapsed = 0; mSpawnTimer = 0; mAttackTimer = 0;
            mHealth = 10; mKills = 0; mNextId = 0; mLevel = 1; mRange = 3.3; mRandom = 76431; mEnemies.Clear();
        }
        else if (name == "pause" && mMode == "playing") mMode = "paused";
        else if (name == "resume" && mMode == "paused") mMode = "playing";
        else if (name == "upgrade" && mMode == "upgrade") { mRange += .7; ++mLevel; mMode = "playing"; }
        else if (name == "home" && mMode != "playing") mMode = "menu";
        else throw new ArgumentException($"Command {name} is unavailable in {mMode}.");
        ++mSoundRevision;
        return Snapshot();
    }
    public string Step(double seconds, double x, double y)
    {
        if (!double.IsFinite(seconds) || !double.IsFinite(x) || !double.IsFinite(y) || seconds < 0)
            throw new ArgumentException("Input must be finite and time nonnegative.");
        if (mMode != "playing") return Snapshot();
        // Bound catch-up and subdivide collisions so a stalled frame cannot
        // tunnel through the objective or skip attack timing.
        double remaining = Math.Min(seconds, .25);
        double length = Math.Max(1, Math.Sqrt(x*x+y*y));
        while (remaining > 0 && mMode == "playing")
        {
            double dt = Math.Min(remaining, 1.0/60); remaining -= dt;
            mX = Math.Clamp(mX + x/length*mPlayerSpeed*dt, -10.5, 10.5);
            mY = Math.Clamp(mY + y/length*mPlayerSpeed*dt, -5.5, 5.5);
            mElapsed += dt; mSpawnTimer -= dt; mAttackTimer -= dt;
            if (mSpawnTimer <= 0 && mEnemies.Count < 100)
            {
                mRandom = unchecked(mRandom*1664525+1013904223);
                double angle = mRandom/(double)uint.MaxValue*Math.PI*2;
                mEnemies.Add(new Enemy { id=++mNextId, x=Math.Cos(angle)*10, y=Math.Sin(angle)*5.5 });
                mSpawnTimer += Math.Max(.45, 1.8-mElapsed/90);
            }
            foreach (var enemy in mEnemies.ToArray())
            {
                double distance = Math.Sqrt(enemy.x*enemy.x+enemy.y*enemy.y);
                if (distance < .75) { mEnemies.Remove(enemy); --mHealth; ++mSoundRevision; continue; }
                enemy.x -= enemy.x/distance*mEnemySpeed*dt;
                enemy.y -= enemy.y/distance*mEnemySpeed*dt;
            }
            if (mAttackTimer <= 0)
            {
                var nearest = mEnemies.OrderBy(e => Math.Pow(e.x-mX,2)+Math.Pow(e.y-mY,2)).FirstOrDefault();
                if (nearest != null && Math.Pow(nearest.x-mX,2)+Math.Pow(nearest.y-mY,2) <= mRange*mRange)
                { mEnemies.Remove(nearest); ++mKills; ++mSoundRevision; if (mKills >= mLevel*5) mMode="upgrade"; }
                mAttackTimer += .45;
            }
            if (mHealth <= 0 || mElapsed >= mDuration)
            {
                mMode = mHealth <= 0 ? "lost" : "won";
                mBest = Math.Max(mBest, mKills); ++mSaveRevision; ++mSoundRevision;
            }
        }
        return Snapshot();
    }
    public string Save() => JsonSerializer.Serialize(new { schema=1, best=mBest });
    public string Snapshot()
    {
        var view = new PortableView { mode=mMode, title=mTitle, saveRevision=mSaveRevision, soundRevision=mSoundRevision,
            sound=mSoundRevision>0 ? "Assets/Audio/Cue.wav" : "",
            status=$"核心 {Math.Max(0,mHealth)}/10  击败 {mKills}  等级 {mLevel}  时间 {(int)mElapsed}/{(int)mDuration}  纪录 {mBest}" };
        view.objects.Add(new() {id="core",width=1.3,height=1.3,color="#80ed99"});
        view.objects.Add(new() {id="player",x=mX,y=mY,width=.8,height=.8,color="#68c9ff"});
        foreach (var e in mEnemies) view.objects.Add(new() {id="enemy-"+e.id,x=e.x,y=e.y,width=.7,height=.7,color="#ff7388"});
        if(mMode=="menu") { view.status="保护绿色核心：WASD 或触摸移动，靠近敌人自动攻击。坚持到计时结束。"; view.actions.Add(new(){id="start",text="开始守护"}); }
        else if(mMode=="playing") view.actions.Add(new(){id="pause",text="暂停"});
        else if(mMode=="paused") { view.actions.Add(new(){id="resume",text="继续"}); view.actions.Add(new(){id="restart",text="重新开始"}); }
        else if(mMode=="upgrade") { view.title="升级：扩大攻击范围"; view.actions.Add(new(){id="upgrade",text="确认升级"}); }
        else { view.title=mMode=="won"?"守护成功":"核心被摧毁"; view.actions.Add(new(){id="restart",text="再试一次"}); view.actions.Add(new(){id="home",text="返回标题"}); }
        return view.ToJson();
    }
}
