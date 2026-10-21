using System.Text.Json.Nodes;
using GameScripts.Rules;
using HEngine.Portable;

static void Check(bool value,string message) { if(!value)throw new Exception(message); }
static JsonNode Read(string json)=>JsonNode.Parse(json)!;
static string Mode(GameSession game)=>Read(game.Snapshot())["mode"]!.GetValue<string>();
static GameSession Create(double speed=.65,double duration=60,string save="") => new($$"""{"title":"Test","playerSpeed":6,"enemySpeed":{{speed.ToString(System.Globalization.CultureInfo.InvariantCulture)}},"duration":{{duration}}} """,save);
var game=Create();Check(Mode(game)=="menu","Initial menu");game.Command("start","");
var before=Read(game.Snapshot())["objects"]![1]!["x"]!.GetValue<double>();
game.Step(.25,1,0);Check(Read(game.Snapshot())["objects"]![1]!["x"]!.GetValue<double>()>before,"Movement");
game.Command("pause","");string paused=game.Snapshot();game.Step(.25,1,1);Check(game.Snapshot()==paused,"Paused state changed");game.Command("resume","");
bool rejected=false;try{game.Step(double.NaN,0,0);}catch(ArgumentException){rejected=true;}Check(rejected,"Invalid time accepted");
game.Command("restart","");game.Step(.25,1,0);game.Step(1.0/12,1,0);
bool upgraded=false;
for(int frame=0;frame<5000 && Mode(game) is not ("won" or "lost");frame++) {
 if(Mode(game)=="upgrade"){upgraded=true;game.Command("upgrade","");}
 game.Step(1.0/60,0,0);
}
Check(upgraded,"Upgrade was not reachable");Check(Mode(game)=="won","Survival win was not reachable");
string saved=game.Save();Check(Read(saved)["best"]!.GetValue<int>()>0,"Best score was not saved");
Check(Create(save:saved).Save()==saved,"Save reload changed score");
var losing=Create(10,120);losing.Command("start","");
for(int frame=0;frame<5000&&Mode(losing)!="lost";frame++){if(Mode(losing)=="upgrade")losing.Command("upgrade","");losing.Step(1.0/60,-1,1);}
Check(Mode(losing)=="lost","Core destruction was not reachable");losing.Command("restart","");Check(Mode(losing)=="playing","Restart failed");
var a=Create();var b=Create();a.Command("start","");b.Command("start","");
for(int i=0;i<100;i++){a.Step(1.0/60,.2,-.1);b.Step(1.0/60,.2,-.1);}Check(a.Snapshot()==b.Snapshot(),"Deterministic replay mismatch");
foreach (var invalid in new PortableView[] {
    new() { worldWidth = 0 }, new() { objects = null! },
    new() { objects = new() { new() { id = "x", x = double.NaN } } },
    new() { objects = new() { new() { id = "x", color = "red" } } },
    new() { objects = new() { new() { id = "x" }, new() { id = "x" } } },
    new() { actions = new() { new() { id = "start" }, new() { id = "start" } } }
}) {
    bool failed = false;
    try { invalid.Validate(); } catch (InvalidOperationException) { failed = true; }
    Check(failed, "Invalid portable view accepted");
}
Console.WriteLine("SharedArena rules PASS: movement, pause, invalid input/view, upgrade, win/loss, restart, save reload, deterministic replay.");
