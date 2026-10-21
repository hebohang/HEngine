namespace HEngine;

/// <summary>Scene-owned asynchronous prefab lease. Native workers load data; polling and instantiation use the engine thread.</summary>
public sealed class PrefabLoad : IDisposable
{
    private readonly ScriptBehaviour mOwner;
    private readonly IEngineApi mApi;
    private readonly ulong mEntity;
    private ulong mRequest;
    private PrefabLoad(ScriptBehaviour owner, ResourceReference resource)
    {
        ArgumentNullException.ThrowIfNull(owner);
        mOwner = owner; mApi = Engine.Api; mEntity = Engine.CurrentEntity;
        owner.Lifetime.ThrowIfCancellationRequested();
        mRequest = mApi.PrefabRequest(mEntity, 0, 0, resource.Id);
        if (mRequest == 0) throw new InvalidOperationException("The engine returned an empty prefab request.");
        try { owner.OwnResource(this); } catch { mApi.PrefabRequest(mEntity, mRequest, 2, ""); mRequest = 0; throw; }
    }
    /// <summary>Begins loading a project-relative stable prefab resource reference. Invalid paths and unsupported platforms throw.</summary>
    public static PrefabLoad Begin(ScriptBehaviour owner, ResourceReference resource) => new(owner, resource);
    private void Check()
    {
        if (mRequest == 0 || mOwner.Lifetime.IsCancellationRequested)
            throw new ObjectDisposedException(nameof(PrefabLoad), "Script or scene lifetime ended, or the lease was disposed.");
    }
    /// <summary>True after native loading completes. A failed load throws with its resource path.</summary>
    public bool IsReady { get { Check(); return mApi.PrefabRequest(mEntity, mRequest, 1, "") != 0; } }
    /// <summary>A coroutine wait that completes when this lease is ready; failures enter the script exception boundary.</summary>
    public ScriptWait Wait => ScriptWait.Until(() => IsReady);
    /// <summary>Creates an instance in the owner's scene. Throws if loading is incomplete or the lease is released.</summary>
    public Entity Instantiate() { Check(); return new(mApi.PrefabRequest(mEntity, mRequest, 3, "")); }
    /// <summary>Releases this lease. Shared native work may finish safely; it never calls a released script.</summary>
    public void Dispose()
    {
        if (mRequest == 0) return;
        mApi.PrefabRequest(mEntity, mRequest, 2, "");
        mRequest = 0; mOwner.DisownResource(this);
    }
}
