using HEngine;

static class Program
{
    sealed class Behaviour : ScriptBehaviour { }
    sealed class AudioApi : EngineApi
    {
        public string Name = "", Payload = "", Response = "true";
        protected override string Call(ref HEngine.Scripting.Interop.Request request, string name = "", string input = "", int outputCapacity = 4096)
        {
            Require(request.Operation == (uint)HEngine.Scripting.Interop.EngineOperation.PlatformService, "Audio escaped the shared service ABI");
            Name = name; Payload = input; return Response;
        }
    }
    sealed class ActionApi : EngineApi
    {
        public string Context = "", Saved = "", Map = "test-input-map";
        public bool Rebound;
        protected override unsafe string Call(ref HEngine.Scripting.Interop.Request request, string name = "", string input = "", int outputCapacity = 4096)
        {
            var operation = (HEngine.Scripting.Interop.EngineOperation)request.Operation;
            if (operation == HEngine.Scripting.Interop.EngineOperation.ReadSave) return Saved;
            if (operation == HEngine.Scripting.Interop.EngineOperation.WriteSave) { Saved = input; return ""; }
            Require(operation == HEngine.Scripting.Interop.EngineOperation.InputAction, "Wrong action operation");
            switch (request.Flags)
            {
                case 0:
                    Require(name == "Move", "Action name lost in transport");
                    request.Data[0] = 0.3f; request.Data[1] = -0.4f; request.Result = 1 | 4 | 16;
                    break;
                case 1: Context = name; break;
                case 2:
                    Require(name == "Jump" && input == "Key" && request.Instance == 0 && request.Data[0] == 70 && request.Data[1] == 0 && request.Data[2] == 1,
                        "Binding ABI values were altered");
                    Rebound = true; break;
                case 3: return Map;
                case 4: Map = input; break;
                default: throw new Exception("Unknown input action request");
            }
            return "";
        }
    }
    sealed class EntityApi : EngineApi
    {
        public bool Alive = true, Attached = true;
        public int Calls;
        protected override string Call(ref HEngine.Scripting.Interop.Request request, string name = "", string input = "", int outputCapacity = 4096)
        {
            Calls++;
            if (!Attached) throw new ObjectDisposedException("test host");
            if (request.Operation == (uint)HEngine.Scripting.Interop.EngineOperation.EntityValid) request.Result = Alive ? 1u : 0u;
            else if (!Alive) throw new InvalidOperationException("Native entity has retired");
            return "";
        }
    }
    sealed class SceneApi : EngineApi
    {
        public int Released;
        public bool FailUpdate;
        protected override string Call(ref HEngine.Scripting.Interop.Request request, string name = "", string input = "", int outputCapacity = 4096)
        {
            if (request.Operation == (uint)HEngine.Scripting.Interop.EngineOperation.EntityValid) { request.Result = 1; return ""; }
            Require(request.Operation == (uint)HEngine.Scripting.Interop.EngineOperation.PlatformService && Engine.CurrentEntity == 41,
                "Owned service lost its creator's context");
            if (name == "audio.ownedPlay" || name == "vfx.ownedPlay") return "17";
            if (name == "ui.ownedLoad") return "{\"lease\":18,\"document\":\"owned-ui\"}";
            if (name == "service.release") { Released++; return "true"; }
            if (FailUpdate) throw new InvalidOperationException("test service failure");
            return "true";
        }
    }
    sealed class PhysicsBehaviour : ScriptBehaviour, IScriptPhysicsEvents
    {
        public int Collisions, Triggers;
        public void Collision(in ScriptContact contact) => Collisions++;
        public void Trigger(in ScriptContact contact) => Triggers++;
    }
    sealed class ResourceApi : EngineApi
    {
        public bool Ready;
        public int Released;
        protected override string Call(ref HEngine.Scripting.Interop.Request request, string name = "", string input = "", int outputCapacity = 4096)
        {
            if (request.Operation != (uint)HEngine.Scripting.Interop.EngineOperation.PrefabRequest) throw new Exception("Unexpected binding");
            if (request.Flags == 0) request.Result = 7;
            else if (request.Flags == 1) request.Result = Ready ? 1u : 0u;
            else if (request.Flags == 2) Released++;
            else if (request.Flags == 3) { Require(Ready, "Instantiated before ready"); request.Result = 123; }
            return "";
        }
    }
    readonly record struct Counter(long Value) : IScriptData<Counter>
    {
        public static string Schema => "contract.counter.v1";
        public static string Encode(Counter value) => value.Value.ToString(System.Globalization.CultureInfo.InvariantCulture);
        public static Counter Decode(System.Text.Json.JsonElement value) => new(value.GetInt64());
    }
    static void Require(bool condition, string message) { if (!condition) throw new Exception(message); }
    static void Main()
    {
        var audio = new AudioApi();
        Engine.Attach(audio); Engine.ExchangeEntity(41);
        try
        {
            ulong workerPrevious = ulong.MaxValue;
            var worker = new Thread(() => workerPrevious = Engine.ExchangeEntity(99));
            worker.Start(); worker.Join();
            Require(workerPrevious == 0 && Engine.CurrentEntity == 41,
                "An off-thread service context corrupted the engine thread before transport validation");
            audio.Response = "18446744073709551614";
            var handle = Audio.Play(new("Assets/sound.wav"), new AudioOptions {
                Position = new(1, 2, 3), Bus = AudioBus.UI, Loop = true, StartPaused = true, Pitch = 1.5f });
            Require(handle == ulong.MaxValue - 1 && audio.Name == "audio.play", "Audio handle lost precision");
            using var request = System.Text.Json.JsonDocument.Parse(audio.Payload);
            Require(request.RootElement.GetProperty("bus").GetInt32() == 2 && request.RootElement.GetProperty("position")[1].GetSingle() == 2 &&
                request.RootElement.GetProperty("paused").GetBoolean() && request.RootElement.GetProperty("stream").GetBoolean(), "Audio options lost routing, spatial data or flags");
            audio.Response = "true";
            Require(Audio.Seek(handle, 1.25) && audio.Name == "audio.seek" && audio.Payload.Contains("18446744073709551614"), "Audio seek changed its voice identity");
            audio.Response = "false";
            Require(!Audio.IsPlaying(0), "Absent voice became playing");
            audio.Response = "{}";
            GameSettings.ApplyDisplay(fullscreen: true);
            Require(audio.Name == "settings.display" && audio.Payload == "{\"Fullscreen\":true}", "Display patch wrote unspecified preferences");
        }
        finally { Engine.Detach(); }
        var services = new SceneApi();
        Engine.Attach(services);
        try
        {
            var owner = new Behaviour(); owner.Bind(new Entity(41)); Engine.ExchangeEntity(41);
            var sound = AudioPlayback.Play(owner, new("Assets/test.wav"));
            var effect = VfxPlayback.Play(owner, new("Assets/test.efkpkg"), System.Numerics.Vector3.Zero, System.Numerics.Vector3.UnitY);
            var document = UiDocument.Load(owner, new("Assets/test.hui"));
            Engine.ExchangeEntity(99);
            Require(sound.Pause() && sound.Resume() && sound.SetVolume(.5f) && sound.SetPosition(new(1, 2, 3)) && Engine.CurrentEntity == 99,
                "Owned audio controls lost their creator context or did not restore the caller");
            Require(document.SetText("label", "hello") && Engine.CurrentEntity == 99, "UI update did not restore the caller context");
            services.FailUpdate = true;
            try { effect.Update(System.Numerics.Vector3.Zero, System.Numerics.Vector3.UnitY); throw new Exception("Service failure swallowed"); }
            catch (InvalidOperationException) { Require(Engine.CurrentEntity == 99, "Failed update leaked owner context"); }
            Require(owner.ReleaseScheduled().Length == 0 && services.Released == 3 && Engine.CurrentEntity == 99, "Scene service cleanup leaked or changed context");
            sound.Dispose(); effect.Dispose(); document.Dispose(); Require(services.Released == 3, "Scene services released twice");
            bool staleAudio = false;
            try { sound.Pause(); } catch (ObjectDisposedException) { staleAudio = true; }
            Require(staleAudio, "Disposed audio reached the host");
            bool stale = false;
            try { document.SetText("label", "stale"); } catch (ObjectDisposedException) { stale = true; }
            Require(stale, "Retired UI service reached host");
            var another = new Behaviour(); another.Bind(new Entity(77));
            bool wrongOwner = false;
            try { AudioPlayback.Play(another, new("Assets/test.wav")); } catch (InvalidOperationException) { wrongOwner = true; }
            Require(wrongOwner, "A script created playback owned by another script");
        }
        finally { Engine.Detach(); }
        var actions = new ActionApi();
        Engine.Attach(actions);
        try
        {
            var state = InputActions.Read("Move");
            Require(state.Value == new System.Numerics.Vector2(0.3f, -0.4f) && state.Pressed && state.Held && state.DoubleTapped && !state.Released && !state.HoldPerformed,
                "Input snapshot flags or axes were decoded incorrectly");
            InputActions.SetContext("UI"); Require(actions.Context == "UI", "Input context was lost");
            InputActions.Rebind("Jump", 0, new(ActionDevice.Key, 70)); Require(actions.Rebound, "Input rebind did not reach host");
            Require(!InputActions.Load(), "Absent input preferences reported as loaded");
            InputActions.Save(); actions.Map = "changed";
            Require(InputActions.Load() && actions.Map == "test-input-map", "Input preferences failed to roundtrip through save service");
            bool rejected = false;
            try { InputActions.Rebind("Jump", 0, new((ActionDevice)999, 70)); } catch (ArgumentOutOfRangeException) { rejected = true; }
            Require(rejected, "Unknown input device crossed the ABI");
            using (Engine.EnterEditValidation())
            {
                rejected = false;
                try { InputActions.SetContext("Gameplay"); } catch (InvalidOperationException) { rejected = true; }
                Require(rejected, "Input mutation was allowed in OnValidate");
            }
        }
        finally { Engine.Detach(); }
        var entityApi = new EntityApi();
        _ = entityApi.ReadEntity(17);
        Require(entityApi.Calls == 1, "Successful entity access added a redundant boundary call");
        entityApi.Alive = false;
        try { entityApi.ReadEntity(17); throw new Exception("Retired entity access succeeded"); }
        catch (InvalidEngineObjectException error)
        {
            Require(error.ObjectType == "Entity" && error.EntityHandle == 17 && error.Origin == "ReadEntity" && error.InnerException != null, "Structured object failure lost its origin");
        }
        entityApi.Attached = false;
        try { entityApi.ReadEntity(0); throw new Exception("Detached transport access succeeded"); }
        catch (ObjectDisposedException) { }
        Require(ScriptDataCodec<Counter>.Decode(ScriptDataCodec<Counter>.Encode(new(42))).Value == 42, "Typed payload roundtrip failed");
        bool mismatched = false;
        try { ScriptDataCodec<Counter>.Decode("{\"schema\":\"different.v1\",\"value\":42}"); } catch (InvalidDataException) { mismatched = true; }
        Require(mismatched, "Incompatible typed save/event schema accepted");
        int stage = 0, disposed = 0;
        bool condition = false;
        IEnumerator<ScriptWait> Routine()
        {
            try
            {
                yield return ScriptWait.NextFrame; stage++;
                yield return ScriptWait.FixedUpdate; stage++;
                yield return ScriptWait.GameSeconds(0.5); stage++;
                yield return ScriptWait.RealSeconds(0); stage++;
                yield return ScriptWait.Until(() => condition); stage++;
                yield return ScriptWait.NextFrame;
            }
            finally { disposed++; }
        }
        var behaviour = new Behaviour();
        behaviour.StartCoroutine(Routine());
        Require(stage == 0, "initial wait skipped");
        behaviour.AdvanceScheduled(true, 0.1); Require(stage == 0, "frame resumed during fixed phase");
        behaviour.AdvanceScheduled(false, 0.1); Require(stage == 1, "next frame did not complete");
        behaviour.AdvanceScheduled(false, 0.1); Require(stage == 1, "fixed wait resumed in ordinary phase");
        behaviour.AdvanceScheduled(true, 0.1); Require(stage == 2, "fixed wait did not complete");
        behaviour.AdvanceScheduled(false, 0.25); Require(stage == 2, "game time finished early");
        behaviour.AdvanceScheduled(false, 0.25); Require(stage == 3, "game time did not complete");
        behaviour.AdvanceScheduled(false, 0); Require(stage == 4, "real time did not complete");
        behaviour.AdvanceScheduled(false, 0); Require(stage == 4, "condition finished early");
        condition = true; behaviour.AdvanceScheduled(false, 0); Require(stage == 5, "condition wait did not complete");
        var task = new TaskCompletionSource();
        behaviour.TrackTask(task.Task, "test cancellation contract");
        Require(behaviour.ReleaseScheduled().Single().Contains("still running"), "residual task was not diagnosed");
        Require(disposed == 1 && behaviour.Lifetime.IsCancellationRequested, "release did not cancel and dispose");
        task.SetCanceled();
        Require(behaviour.ReleaseScheduled().Length == 0, "completed task retained");
        behaviour.AdvanceScheduled(false, 1); Require(stage == 5 && disposed == 1, "retired coroutine executed");
        bool invalid = false;
        try { _ = ScriptWait.GameSeconds(double.NaN); } catch (ArgumentOutOfRangeException) { invalid = true; }
        Require(invalid, "nonfinite wait accepted");
        using (Engine.EnterEditValidation())
        {
            bool denied = false;
            try { Engine.Log("must not reach runtime"); } catch (InvalidOperationException error) { denied = error.Message.Contains("Reset/OnValidate"); }
            Require(denied, "validation could access the runtime");
            denied = false;
            try { using var nested = Engine.EnterEditValidation(); } catch (InvalidOperationException) { denied = true; }
            Require(denied, "recursive validation accepted");
        }
        Require(!Engine.IsValidating, "validation scope leaked");
        var resources = new ResourceApi();
        Engine.Attach(resources);
        try
        {
            var owner = new Behaviour();
            var load = PrefabLoad.Begin(owner, new("Assets/example.hprefab"));
            Require(!load.IsReady, "Asynchronous lease finished before native completion");
            resources.Ready = true;
            Require(load.IsReady && load.Instantiate().Handle == 123, "Ready prefab did not instantiate");
            Require(owner.ReleaseScheduled().Length == 0 && resources.Released == 1, "Owner release did not dispose prefab lease");
            load.Dispose(); Require(resources.Released == 1, "Lease released twice");
            bool stale = false;
            try { _ = load.IsReady; } catch (ObjectDisposedException) { stale = true; }
            Require(stale, "Released prefab lease reached the native host");
            var cancelledOwner = new Behaviour();
            _ = PrefabLoad.Begin(cancelledOwner, new("Assets/pending.hprefab"));
            cancelledOwner.ReleaseScheduled();
            Require(resources.Released == 2, "Pending resource lease leaked at cancellation");
        }
        finally { Engine.Detach(); }
        var retiredApi = new NativeEngineApi(0);
        Engine.Attach(retiredApi);
        Engine.Detach();
        bool retiredDenied = false;
        try { retiredApi.Log("retired transport must never enter native code"); } catch (ObjectDisposedException) { retiredDenied = true; }
        Require(retiredDenied, "Cached API remained callable after host detach");
        var physics = new PhysicsBehaviour();
        var contact = new ScriptContact(new Entity(123), ScriptContactPhase.Enter, System.Numerics.Vector3.Zero, System.Numerics.Vector3.UnitY);
        Require(ScriptPhysics.Dispatch(physics, contact, false) && ScriptPhysics.Dispatch(physics, contact, true), "Explicit physics interface was not dispatched");
        Require(physics.Collisions == 1 && physics.Triggers == 1, "Collision and trigger channels were mixed");
        Require(!ScriptPhysics.Dispatch(new Behaviour(), contact, false), "Physics adapter scanned a non-subscriber");
        physics.ReleaseScheduled();
        bool physicsCancelled = false;
        try { ScriptPhysics.Dispatch(physics, contact, false); } catch (OperationCanceledException) { physicsCancelled = true; }
        Require(physicsCancelled && physics.Collisions == 1, "Physics adapter called a released instance");
        Console.WriteLine("PASS: next frame, fixed update, game/real time, conditions, iterator disposal, cancellation and residual task diagnostics");
    }
}
