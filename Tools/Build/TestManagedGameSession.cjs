'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const vm = require('node:vm');
const fs = require('node:fs');

const source = fs.readFileSync(path.resolve(__dirname,
    '../../Engine/Source/Runtime/Platform/Web/ManagedGameSession.js'), 'utf8');
const code = source.replace('export class ManagedGameSession', 'class ManagedGameSession') +
    '\nthis.ManagedGameSession = ManagedGameSession;';
const scope = {};
vm.runInNewContext(code, scope);
const {ManagedGameSession} = scope;

const entries = new Map([['game', 'older-save']]);
const storage = {get: key => entries.get(key) || '', set: (key, value) => entries.set(key, value)};
let revision = 0;
const game = {
    Initialize: (_catalog, save) => {
        if (save === 'older-save') throw Error('old format');
        return JSON.stringify({saveRevision: revision, save});
    },
    Command: (name, value) => JSON.stringify({saveRevision: ++revision, name, value}),
    Step: () => JSON.stringify({saveRevision: revision}),
    Save: () => `save-${revision}`,
};
entries.set('game.backup', 'recovery-save');
const session = new ManagedGameSession(game, storage, 'game');
assert.equal(session.initialize({scene: 'sample'}).save, 'recovery-save');
assert.equal(session.restoredFromBackup, true);
assert.equal(session.command('start', 'one').name, 'start');
assert.equal(entries.get('game'), 'save-1');
assert.equal(entries.get('game.backup'), 'recovery-save');
session.step(0.016, 0, 0);
assert.equal(entries.get('game'), 'save-1');
assert.equal(session.save(), 'save-1');
const fresh = new ManagedGameSession(game, storage, 'other');
fresh.initialize({}, {backupBeforeInitialize: true});
fresh.command('start');
assert.equal(entries.get('other'), 'save-2');
console.log('ManagedGameSession PASS');
