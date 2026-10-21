// Shared game protocol; callers supply the platform's synchronous storage API.
export class ManagedGameSession {
    constructor(game, storage, saveKey) {
        this.game = game;
        this.storage = storage;
        this.saveKey = saveKey;
        this.saved = '';
        this.snapshot = null;
        this.lastSaveRevision = -1;
        this.restoredFromBackup = false;
    }

    initialize(catalog, {backupBeforeInitialize = false} = {}) {
        this.saved = this.storage.get(this.saveKey) || '';
        this.restoredFromBackup = false;
        const backupKey = this.saveKey + '.backup';
        if (backupBeforeInitialize && this.saved)
            this.storage.set(backupKey, this.saved);
        try {
            this.snapshot = JSON.parse(this.game.Initialize(JSON.stringify(catalog), this.saved));
        } catch (error) {
            const backup = this.storage.get(backupKey);
            if (!backup || backup === this.saved) throw error;
            this.snapshot = JSON.parse(this.game.Initialize(JSON.stringify(catalog), backup));
            this.saved = backup;
            this.restoredFromBackup = true;
        }
        return this.snapshot;
    }

    command(name, value = '') {
        this.snapshot = JSON.parse(this.game.Command(name, value));
        this.persistIfChanged();
        return this.snapshot;
    }

    step(seconds, x, z) {
        this.snapshot = JSON.parse(this.game.Step(seconds, x, z));
        this.persistIfChanged();
        return this.snapshot;
    }

    persistIfChanged() {
        if (this.snapshot && this.snapshot.saveRevision !== this.lastSaveRevision)
            this.persist();
    }

    persist() {
        const data = this.game.Save();
        if (this.saved && this.saved !== data)
            this.storage.set(this.saveKey + '.backup', this.saved);
        this.storage.set(this.saveKey, data);
        this.saved = data;
        this.lastSaveRevision = this.snapshot?.saveRevision ?? -1;
    }

    save() { return this.game.Save(); }
}
