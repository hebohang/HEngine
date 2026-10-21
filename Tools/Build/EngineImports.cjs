'use strict';
const path = require('node:path');

// Stable project imports resolved by the selected engine, regardless of where
// the project is stored. Shared by the exporter and synchronous UI tests.
function engineAliases(engineRoot = path.resolve(__dirname, '../..')) {
    return {
        '@hengine/web': path.join(engineRoot, 'Engine/Source/Runtime/Platform/Web'),
        '@hengine/wechat': path.join(engineRoot, 'Engine/Source/Runtime/Platform/WeChat'),
    };
}

function engineSiteImports(engineRoot = path.resolve(__dirname, '../..')) {
    return {name: 'engine-site-imports', setup(build) {
        build.onResolve({filter: /^\.\/engine\//}, args => ({
            path: path.join(engineRoot, 'Engine/Source/Runtime/Platform/Web', args.path.slice(9)),
        }));
    }};
}

module.exports = {engineAliases, engineSiteImports};
