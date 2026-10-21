// Browser presentation policy shared by projects; UI/input remain in CSS pixels.
export function createBrowserHost(canvas,{maxPixelRatio=3}={}) {
    return {maxPixelRatio,viewport:()=>({width:canvas.clientWidth,height:canvas.clientHeight,ratio:globalThis.devicePixelRatio||1})};
}
