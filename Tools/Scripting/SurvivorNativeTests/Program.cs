using System.Numerics;
using System.Reflection;
using System.Text.Json.Nodes;
using GameScripts;
using GameScripts.Survivor;
using HEngine;

// Exercise the actual native adapter and upgrade commands with an in-memory host.
// Controlled enemy/upgrade fixtures remain here, never in the shipped game.
object Field(object target,string name)=>target.GetType().GetField(name,BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(target)!;
void Set(object target,string name,object value)=>target.GetType().GetField(name,BindingFlags.Instance|BindingFlags.NonPublic)!.SetValue(target,value);
void Require(bool condition,string message){if(!condition)throw new Exception(message);}
var host=new RecordingHost();Engine.Attach(host);var game=new SurvivorNativeGame();
try
{
    game.Initialize();var session=(WebGameSession)Field(game,"mSession");
    session.Command("start", "");
    foreach(string weapon in new[]{"moonblade","fireball"})
    {
        var pool=new UpgradePool(new(){Weapons=[new(){Id=weapon,MaxLevel=10}],Owned=new(){["weapons"]=[weapon]},Enabled=new(){["weapons"]=[weapon]},MaxWeaponSlots=3});
        pool.Roll();Set(session,"mUpgradePool",pool);Set(session,"mMode","upgrade");session.Command("upgrade",weapon);
    }
    typeof(WebGameSession).GetMethod("Spawn",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(session,["green_blob",new JsonObject()]);
    game.Update(1d/60);
    foreach(var (slot,weapon) in new[]{(2,"moonblade"),(3,"fireball")})
    {
        Require(host.Classes[("weapon_slot_"+slot,"empty")]==false,"Equipped weapon must clear empty slot style");
        Require(host.Classes[("weapon_slot_"+slot,"locked")]==false,"Equipped weapon must clear locked slot style");
        Require(host.Images["weapon_icon_"+slot].EndsWith("/"+weapon+".png"),"New weapon icon path");
        Require(host.Effects.Any(p=>p.EndsWith("Casts/"+weapon+".efkpkg")),"New weapon cast effect: "+weapon);
    }
    Require(host.Effects.Any(p=>p.EndsWith("MoonBladeToon.efkpkg")),"Moonblade body must reach the renderer");
    Require(host.Effects.Any(p=>p.EndsWith("FireballChibiToon.efkpkg")),"Fireball body must reach the renderer");
    var catalog=(JsonObject)Field(game,"mCatalog");
    // Sample the real shared catalog through the native spawn/movement adapter.
    // All imported character animation palettes are Y-up; heading must rotate
    // around world up, without tipping the character at any compass direction.
    foreach(var character in catalog["characters"]!.AsArray())
    {
        var entity=(Entity)typeof(SurvivorNativeGame).GetMethod("Spawn",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(game,[character!["id"]!.GetValue<string>()])!;
        foreach(float yaw in new[]{0f,MathF.PI/2,MathF.PI,-MathF.PI/2})
        {
            typeof(SurvivorNativeGame).GetMethod("Move",BindingFlags.Static|BindingFlags.NonPublic)!.Invoke(null,[entity,new JsonObject{["x"]=0d,["y"]=0d,["z"]=0d,["yaw"]=(double)yaw}]);
            var state=host.States[entity.Handle];
            var rotation=Quaternion.CreateFromYawPitchRoll(state.Rotation.Y,state.Rotation.X,state.Rotation.Z);
            Require(Vector3.Dot(Vector3.Transform(Vector3.UnitY,rotation),Vector3.UnitY)>.999f,"Character tipped during native movement: "+character["id"]);
            Require(Math.Abs(state.Rotation.Y-yaw)<.0001f,"Native heading changed");
        }
    }
    foreach(string weapon in new[]{"moonblade","fireball"})
    {
        string prefab=catalog["nativeWeaponVfx"]![weapon]!["projectilePrefab"]!.GetValue<string>();
        Require(host.Prefabs.ContainsValue(prefab),"Original projectile mesh must be instantiated: "+weapon);
        Require(host.States.Any(p=>host.Prefabs.GetValueOrDefault(p.Key)==prefab&&Math.Abs(p.Value.Scale.X-(weapon=="fireball"?.62f:.20f))<.001),"Original mesh scale: "+weapon);
    }
    session.Command("pause", "");game.Update(1d/60);
    Require(host.Classes[("weapon_slot_2","empty")]==false,"Pause must preserve acquired icon");
    Console.WriteLine("unit-test PASS: acquired moonblade/fireball clear empty/locked styles, preserve pause icons, and submit original mesh cores plus cast/body effects.");
    Console.WriteLine("unit-test PASS: all 10 shared character models remain Y-up at four native movement headings.");
}
finally{game.Destroy();Engine.Detach();}

sealed class RecordingHost:IEngineApi
{
    private ulong mNextEntity=2;private int mNextEffect;
    public readonly Dictionary<(string,string),bool> Classes=new();
    public readonly Dictionary<string,string> Images=new();
    public readonly List<string> Effects=new();
    public readonly Dictionary<ulong,string> Prefabs=new();
    public readonly Dictionary<ulong,EntityState> States=new();
    public bool IsValid(ulong entity)=>entity!=0;
    public EntityState ReadEntity(ulong entity)=>States.GetValueOrDefault(entity,new(){Scale=Vector3.One});
    public void WriteEntity(ulong entity,in EntityState state,bool transform,bool sprite)=>States[entity]=state;
    public InputState ReadInput()=>default;
    public void DestroyEntity(ulong entity){}
    public Entity FindEntity(string stableId)=>default;
    public void SetEnabled(ulong entity,bool enabled){}
    public string ReadTexture(ulong entity)=>"";
    public void WriteTexture(ulong entity,string path){}
    public void Log(string message){}
    public string ReadSave(string key)=>"";
    public void WriteSave(string key,string value){}
    public void EmitEvent(string name,string payload){}
    public string ReadEvent(string name,ulong afterSequence,out ulong sequence){sequence=afterSequence;return "";}
    public string PlatformService(string name,string request)
    {
        var a=JsonNode.Parse(request)!;
        switch(name)
        {
            case "scene.camera":return "1";
            case "scene.findTag":return "0";
            case "scene.instantiate":Prefabs[++mNextEntity]=a["path"]!.GetValue<string>();return mNextEntity.ToString();
            case "ui.load":return "\"hud\"";
            case "settings.read":return "{}";
            case "settings.display":return "{\"MasterVolume\":0.5,\"UIScale\":1.0,\"VSync\":false,\"Fullscreen\":false}";
            case "input.buttons":return "{\"jump\":false,\"pause\":false,\"captured\":false,\"right\":false,\"pointerX\":0,\"pointerY\":0}";
            case "audio.play":return "0";
            case "vfx.play":Effects.Add(a["path"]!.GetValue<string>());return (++mNextEffect).ToString();
            case "ui.class":Classes[(a["element"]!.GetValue<string>(),a["name"]!.GetValue<string>())]=a["enabled"]!.GetValue<bool>();break;
            case "ui.attribute":if(a["name"]!.GetValue<string>()=="src")Images[a["element"]!.GetValue<string>()]=a["value"]!.GetValue<string>();break;
        }
        return "true";
    }
}
