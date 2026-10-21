// Package transport for the shared WebGL renderer. No DOM/fetch emulation.
export function createPackageAssets(wx, files, viewport) {
    const fs = wx.getFileSystemManager();
    const pathFor = name => {
        const key = String(name).replace(/^\.\//, '').replaceAll('\\', '/');
        if (!Object.prototype.hasOwnProperty.call(files, key)) throw Error('Undeclared asset: ' + key);
        return files[key];
    };
    return {
        // Shared mobile renderer policy; projects need no device-specific draw code.
        platform: 'WeChat',maxPixelRatio:3,compatibilityGeometry: true,supportsReadback:false,
        viewport, pathFor,
        async fetch(name) {
            const path = pathFor(name);
            const bytes = await new Promise((resolve, reject) => fs.readFile({filePath:path, success:r=>resolve(r.data), fail:reject}));
            return {ok:true, arrayBuffer:async()=>bytes,
                json:async()=>JSON.parse(fs.readFileSync(path, 'utf8'))};
        },
        loadImage(name) {
            return new Promise((resolve, reject) => {
                const image = wx.createImage();
                image.onload = () => resolve(image);
                image.onerror = () => reject(Error('Image failed: ' + name));
                image.src = pathFor(name);
            });
        }
    };
}
