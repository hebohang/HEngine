using System.Collections;
using System.Reflection;
using System.Text.Json.Nodes;
using GameScripts.Survivor;

// Controlled integration fixtures exercise the real session without exposing test cheats in the game.
var catalog=JsonNode.Parse(File.ReadAllText(args[0]))!.AsObject();
int checks=0;
void Require(bool value,string message){if(!value)throw new Exception(message);checks++;}
object Field(object target,string name)=>target.GetType().GetField(name,BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic)!.GetValue(target)!;
void Set(object target,string name,object value)=>target.GetType().GetField(name,BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic)!.SetValue(target,value);
object? Call(object target,string method,params object[] values)=>target.GetType().GetMethod(method,BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(target,values);
WebGameSession Session(){var s=new WebGameSession(catalog.ToJsonString(),"");s.Command("start", "");Set(s,"mSpawnTimer",9999d);return s;}
object Enemy(WebGameSession s,string kind,double x=1,double z=0)
{
    Call(s,"Spawn",kind,new JsonObject());var enemies=(IList)Field(s,"mAgents");var e=enemies[^1]!;Set(e,"X",x);Set(e,"Z",z);Set(e,"Timer",0d);return e;
}
JsonObject Snapshot(WebGameSession s)=>JsonNode.Parse(s.Snapshot())!.AsObject();
void TickEnemy(WebGameSession s,int frames=180){for(int i=0;i<frames;i++)Call(s,"UpdateEnemies",1d/60);}

foreach(string kind in new[]{"bee_ranger","mushroom_lobber","yeti_zoner","alien_commander","bomb_spore","crab_guard","alien_charger"})
{
    var s=Session();var e=Enemy(s,kind);TickEnemy(s);
    if(kind is "bee_ranger" or "mushroom_lobber")Require(((IList)Field(s,"mProjectiles")).Count>0,kind+" must fire");
    if(kind is "yeti_zoner" or "mushroom_lobber" or "bomb_spore")Require(((IList)Field(s,"mAreas")).Count>0,kind+" must create a hazard");
    if(kind=="alien_commander")Require(((IList)Field(s,"mAgents")).Count>1,"Summoner must spawn enemies");
    if(kind=="bomb_spore")Require(!((IList)Field(s,"mAgents")).Contains(e),"Suicide enemy must retire");
    if(kind=="alien_charger")Require((double)Field(s,"mHealth")<100,"Charge must hit nearby player");
    if(kind=="crab_guard")Require((double)Field(e,"EnemyShield")>0,"Shield enemy must acquire shield during telegraph");
}
for(int phase=1;phase<=3;phase++)
{
    var s=Session();var e=Enemy(s,"star_dragon",1);Set(e,"Health",(double)Field(e,"MaxHealth")*(phase==1?1:phase==2?.5:.2));TickEnemy(s,80);
    Require((int)Field(e,"BossPhase")==phase,"Boss health phase");
    if(phase<3)Require(((IList)Field(s,"mProjectiles")).Count==(phase==1?8:5),"Boss ring / breath projectile pattern");
    else Require(((IList)Field(s,"mAreas")).Count>0&&((IList)Field(s,"mAgents")).Count>1,"Boss final phase zone and summon");
}
{
    var s=Session();var levels=(Dictionary<string,int>)Field(s,"mWeaponLevels");var slots=(List<string>)Field(s,"mWeaponSlots");var tomes=(Dictionary<string,int>)Field(s,"mTomeLevels");
    var recipe=catalog["evolutions"]!.AsArray().First(r=>r!["weaponId"]!.GetValue<string>()=="bubble")!;
    slots.Clear();slots.Add("bubble");levels.Clear();levels["bubble"]=10;tomes[recipe["tomeId"]!.GetValue<string>()]=5;
    Call(s,"OpenChest",3);var snapshot=Snapshot(s);
    Require(snapshot["evolutions"]!["bubble"]!=null,"Chest must activate evolution");
    Require(levels["bubble"]==10&&slots[0]=="bubble","Evolution must preserve runtime base weapon level and slot");
    Call(s,"OpenChest",3);Require(Snapshot(s)["evolutions"]!.AsObject().Count==1,"Evolution may not duplicate");
}
foreach(var character in catalog["characters"]!.AsArray())
{
    string id=character!["id"]!.GetValue<string>();var copy=catalog.DeepClone().AsObject();copy["defaultProfile"]!["owned"]!["characters"]!.AsArray().Add(id);
    var s=new WebGameSession(copy.ToJsonString(),"");s.Command("selectCharacter",id);s.Command("start", "");
    for(int i=0;i<600;i++){var state=JsonNode.Parse(s.Step(1d/60,0,0))!;if(state["mode"]!.GetValue<string>()=="upgrade")s.Command("upgrade",state["choices"]![0]!["id"]!.GetValue<string>());}
    Require(Snapshot(s)["mode"]!.GetValue<string>()!="result",id+" early run must remain playable");
}
foreach(string region in new[]{"star","thunder","water","wind"})
{
    var s=Session();var e=Enemy(s,"green_blob",1);Set(e,"Health",1000d);Set(e,"MaxHealth",1000d);
    Call(s,"GrantDomain",region);for(int i=0;i<90;i++)Call(s,"UpdateAreas",1d/60);
    Require((double)Field(e,"Health")<1000,region+" domain must deal damage");
    Require(((IDictionary)Field(s,"mWeaponStats")).Count==0,"Domain damage must not enter weapon telemetry / proc paths");
    Require(((IList)Field(s,"mWeaponSlots")).Count==1,"Domains may not occupy weapon slots");
}
{
    var s=Session();var region=catalog["regions"]!["star"]!;Set(s,"mX",region["authoredPosition"]![0]!.GetValue<double>());Set(s,"mZ",region["authoredPosition"]![2]!.GetValue<double>());
    for(int i=0;i<240;i++){Call(s,"UpdateObjectives",1d/60);Call(s,"UpdateEncounters",1d/60);}
    Require(((IList)Field(s,"mAgents")).Count>10,"Region assault must generate encounter pressure");
    Require(((IList)Field(s,"mAgents")).Cast<object>().All(e=>(string)Field(e,"Region")=="star"),"Region enemies must retain reward ownership");
}
{
    var s=new WebGameSession(catalog.ToJsonString(),"");s.Command("configureAdRevive","true");s.Command("start","");
    Set(s,"mHealth",-1d);Set(s,"mRevivesRemaining",0);s.Step(0,0,0);
    Require(Snapshot(s)["mode"]!.GetValue<string>()=="revive","Ad revival must defer settlement");
    var frozen=Snapshot(s)["elapsed"]!.GetValue<double>();s.Step(.05,1,0);
    Require(Snapshot(s)["elapsed"]!.GetValue<double>()==frozen,"Revival choice freezes simulation");
    s.Command("rewardRevive","");Require(Snapshot(s)["mode"]!.GetValue<string>()=="playing","Completed reward resumes play");
    Require((double)Field(s,"mHealth")==((double)Field(s,"mMaxHealth"))*.5,"Revival restores half health");
    try{s.Command("rewardRevive","");throw new Exception("Duplicate reward accepted");}catch(InvalidOperationException){}Require((int)Field(s,"mRevivesUsed")==1,"Duplicate reward must not revive twice");
    Set(s,"mHealth",-1d);s.Step(0,0,0);Require(Snapshot(s)["mode"]!.GetValue<string>()=="result","Second death settles without another ad");
}
{
    var s=new WebGameSession(catalog.ToJsonString(),"");s.Command("configureAdRevive","true");s.Command("start","");
    Set(s,"mHealth",-1d);Set(s,"mRevivesRemaining",0);s.Step(0,0,0);s.Command("declineRevive","");
    Require(Snapshot(s)["mode"]!.GetValue<string>()=="result","Declining revival settles once");
    var save=s.Save();foreach(var command in new[]{"rewardRevive","declineRevive"})try{s.Command(command,"");throw new Exception("Late command accepted");}catch(InvalidOperationException){}Require(s.Save()==save,"Late callbacks cannot duplicate settlement");
}
{
    var s=Session();Set(s,"mHealth",-1d);Set(s,"mRevivesRemaining",0);s.Step(0,0,0);
    Require(Snapshot(s)["mode"]!.GetValue<string>()=="result","Windows/default session keeps original death behavior");
}
{
    var full=Session();Enemy(full,"green_blob",1000,1000);Enemy(full,"green_blob",1,0);
    Require(Snapshot(full)["enemies"]!.AsArray().Count==2,"Default host receives distant actors");
    var filtered=new WebGameSession(catalog.ToJsonString(),"");filtered.Command("configureInterestRadius","80");filtered.Command("start","");
    Enemy(filtered,"green_blob",1000,1000);Enemy(filtered,"green_blob",1,0);
    Require(Snapshot(filtered)["enemies"]!.AsArray().Count==1,"Portable snapshot omits distant actor work");
    Require(((IList)Field(filtered,"mAgents")).Count==2,"Presentation ROI must retain authoritative actors");
}
{
    var desktop = new WebGameSession(catalog.ToJsonString(), "");
    desktop.ConfigureCamera(20 * Math.PI / 180, .0024, -5 * Math.PI / 180, 75 * Math.PI / 180, Math.PI);
    desktop.Command("start", "");
    desktop.Command("look", "[100,0]");
    Require(Math.Abs(Snapshot(desktop)["cameraYaw"]!.GetValue<double>() - (Math.PI - .24)) < 1e-9, "Desktop mouse sensitivity");
    desktop.Command("look", "[0,10000]");
    Require(Math.Abs(Snapshot(desktop)["cameraElevation"]!.GetValue<double>() - 75 * Math.PI / 180) < 1e-9, "Desktop pitch limit");
    desktop.Command("pause", "");
    desktop.Command("look", "[100,0]");
    Require(Math.Abs(Snapshot(desktop)["cameraYaw"]!.GetValue<double>() - (Math.PI - .24)) < 1e-9, "Paused mouse cannot rotate the camera");
    var portable = Session();
    portable.Command("look", "[100,0]");
    Require(Math.Abs(Snapshot(portable)["cameraYaw"]!.GetValue<double>() + .6) < 1e-9, "Desktop configuration cannot change portable orbit sensitivity");
    Require(Math.Abs(Snapshot(portable)["cameraElevation"]!.GetValue<double>() - Math.Atan2(14,17)) < 1e-9, "Portable camera retains its initial elevation");
}
{
    var profile=catalog["defaultProfile"]!.DeepClone().AsObject();profile["walletGold"]=10000;profile["trackedQuests"]=new JsonArray();
    var menu=new WebGameSession(catalog.ToJsonString(),profile.ToJsonString());
    string power=catalog["metaPowerUps"]![0]!["id"]!.GetValue<string>();
    menu.Command("power",power);Require(Snapshot(menu)["profile"]!["walletGold"]!.GetValue<double>()<10000,"Shop purchase deducts currency");
    menu.Command("refundPower",power);Require(Snapshot(menu)["profile"]!["walletGold"]!.GetValue<double>()==10000,"Refund restores exact paid prices");
    menu.Command("refundPower",power);Require(Snapshot(menu)["profile"]!["walletGold"]!.GetValue<double>()==10000,"Repeated refund cannot mint currency");
    menu.Command("power",power);menu.Command("power",catalog["metaPowerUps"]![1]!["id"]!.GetValue<string>());
    menu.Command("refundAllPower", "");Require(Snapshot(menu)["profile"]!["walletGold"]!.GetValue<double>()==10000,"Refund all restores the combined prices atomically");
    var quests=catalog["quests"]!.AsArray().Take(4).Select(q=>q!["id"]!.GetValue<string>()).ToArray();
    foreach(string quest in quests)menu.Command("trackQuest",quest);
    Require(Snapshot(menu)["profile"]!["trackedQuests"]!.AsArray().Count==3,"Tracking retains three-slot limit");
    menu.Command("trackQuest",quests[0]);menu.Command("trackQuest",quests[3]);
    var reloaded=new WebGameSession(catalog.ToJsonString(),menu.Save());
    Require(Snapshot(reloaded)["profile"]!["trackedQuests"]!.AsArray().Any(q=>q!.GetValue<string>()==quests[3]),"Tracking survives save/reload");
}
{
    var session=Session();session.EnablePresentationEvents();Enemy(session,"green_blob");
    ((Dictionary<string,int>)Field(session,"mWeaponLevels"))["ring"]=1;
    Call(session,"CastWeapon","ring",1d,false);
    var first=Snapshot(session);var second=Snapshot(session);
    Require(first["areas"]!.AsArray().Count>0,"Area weapons expose their authored visual lifecycle");
    Require(first["areas"]![0]!["id"]!.ToString()==second["areas"]![0]!["id"]!.ToString(),"Presentation identity is stable across snapshots");
    Require(first["presentationEvents"]!.AsArray().Count>0,"Native presentation receives successful cast events");
    Require(first["presentationEvents"]!.ToJsonString()==second["presentationEvents"]!.ToJsonString(),"Reading presentation cannot replay gameplay or consume events");
}
Console.WriteLine($"unit-test PASS: {checks} session behavior assertions (combat, platform camera, desktop economy/tracking, presentation events)");
