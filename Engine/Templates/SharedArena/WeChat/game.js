import {startPortableGame} from '@hengine/wechat/PortableHost.js';
startPortableGame(wx,require('./he-game-config.js'),()=>require('./wechat-runtime.js').createManagedRuntime(require('./runtime-config.js')))
    .catch(error=>{console.error(error);wx.showModal({title:'游戏启动失败',content:String(error.message),showCancel:false});});
