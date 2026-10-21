// Explicit package policy: do not inherit a previous preview's vConsole switch.
export function applyDebugSettings(wx,{debug=false}={}) {
    if(typeof wx.setEnableDebug==='function')wx.setEnableDebug({enableDebug:debug===true,
        fail:error=>console.warn('[HE debug settings]',error.errMsg)});
}
