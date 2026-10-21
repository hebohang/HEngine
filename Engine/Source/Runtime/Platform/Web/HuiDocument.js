import {CanvasUI} from './CanvasUI.js';

const types=new Set(['Canvas','Panel','Text','Image','Button']);
const styleDefaults={BackgroundColor:'rgba(32,32,32,220)',TextColor:'#ffffffff',BackgroundImage:'',FontPath:'Assets/Fonts/opensans/OpenSans-Regular.ttf',FontSize:24,BorderRadius:0,BorderWidth:0,BorderColor:'#00000000',ClassName:'',ExtraStyle:''};
const layoutDefaults={AnchorMin:[0,0],AnchorMax:[0,0],Pivot:[.5,.5],OffsetMin:[0,0],OffsetMax:[0,0]};

// UIRes v1 is the authoring contract, shared with UIRes.h and the Editor UI panel.
// Reject unsupported presentation instead of silently exporting a different UI.
export function normalizeHui(source,label='UI document') {
    const fail=message=>{throw Error(`${label}: ${message}`);};
    if(source.Version!==1||!Array.isArray(source.Nodes)||!source.Nodes.length)fail('expected UIRes v1 with authored Nodes');
    const ids=new Set(),elements=new Set();
    const nodes=source.Nodes.map(raw=>{
        const n={...raw,Type:raw.Type??'Panel',ElementId:raw.ElementId||`node_${raw.Id}`,bVisible:raw.bVisible??true,bInteractable:raw.bInteractable??true,Text:raw.Text??'',ImagePath:raw.ImagePath??'',Layout:{...layoutDefaults,...raw.Layout},Style:{...styleDefaults,...raw.Style}};
        if(!Number.isInteger(n.Id)||ids.has(n.Id))fail(`duplicate/invalid node Id ${n.Id}`);
        if(!Number.isInteger(n.ParentId))fail(`${n.ElementId}: invalid ParentId`);
        if(!/^[a-zA-Z_][\w-]*$/.test(n.ElementId)||elements.has(n.ElementId))fail(`duplicate/invalid ElementId ${n.ElementId}`);
        if(!types.has(n.Type))fail(`${n.ElementId}: unsupported widget ${n.Type}`);
        if(typeof n.Text!=='string'||typeof n.bVisible!=='boolean'||typeof n.bInteractable!=='boolean')fail(`${n.ElementId}: invalid text or visibility/input flags`);
        for(const name of ['ClassName','ExtraStyle','FontPath','BackgroundImage'])if(typeof n.Style[name]!=='string')fail(`${n.ElementId}: invalid ${name}`);
        for(const path of [n.ImagePath,n.Style.BackgroundImage])if(typeof path!=='string'||(path&&(!path.startsWith('Assets/')||path.includes('..')||/[\\'"()\r\n]/.test(path))))fail(`${n.ElementId}: unsafe asset path`);
        for(const [name,value] of Object.entries(n.Layout)){
            if(!(name in layoutDefaults))fail(`${n.ElementId}: unsupported layout ${name}`);
            if(!Array.isArray(value)||value.length!==2||value.some(v=>!Number.isFinite(v)))fail(`${n.ElementId}: invalid ${name}`);
        }
        for(const name of ['FontSize','BorderRadius','BorderWidth'])if(!Number.isFinite(n.Style[name])||n.Style[name]<0)fail(`${n.ElementId}: invalid ${name}`);
        if(n.Style.ExtraStyle.trim())fail(`${n.ElementId}: ExtraStyle is not portable; use structured UI style fields`);
        if(n.Style.ClassName.trim())fail(`${n.ElementId}: ClassName requires an external stylesheet and is not portable`);
        if(n.Style.FontPath)fail(`${n.ElementId}: custom FontPath is not supported by the shared UI backends yet; use an empty FontPath for platform text`);
        if(/[\r\n]/.test(n.Text))fail(`${n.ElementId}: multiline text is not supported by the shared UI backends yet`);
        if(n.ImagePath&&n.Type!=='Image')fail(`${n.ElementId}: ImagePath requires an Image widget`);
        for(const name of ['BackgroundColor','TextColor','BorderColor'])normalizeColor(n.Style[name],`${label}: ${n.ElementId}.${name}`);
        ids.add(n.Id);elements.add(n.ElementId);return n;
    });
    const roots=nodes.filter(n=>n.ParentId<0);
    if(roots.length!==1||roots[0].Type!=='Canvas')fail('expected exactly one root Canvas');
    const byId=new Map(nodes.map(n=>[n.Id,n]));
    for(const n of nodes){
        const visited=new Set([n.Id]);let parent=n.ParentId;
        while(parent>=0){if(!byId.has(parent)||visited.has(parent))fail(`${n.ElementId}: missing parent or hierarchy cycle`);visited.add(parent);parent=byId.get(parent).ParentId;}
    }
    return {...source,Nodes:nodes};
}

export function normalizeColor(value,label='color') {
    if(typeof value!=='string')throw Error(`${label}: invalid color`);
    if(/^#[\da-f]{6}([\da-f]{2})?$/i.test(value)||/^#[\da-f]{3}$/i.test(value)||value==='transparent')return value;
    const m=value.match(/^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)(?:\s*,\s*([\d.]+))?\s*\)$/);
    if(!m||m.slice(1,4).some(v=>Number(v)>255)||Number(m[4]??1)>255)throw Error(`${label}: unsupported color ${value}`);
    const alpha=Number(m[4]??1);return `rgba(${Number(m[1])},${Number(m[2])},${Number(m[3])},${alpha>1?alpha/255:alpha})`;
}

// Matches ResolveUILayoutRect: each stretched/fixed axis is resolved independently.
export function resolveHuiRect(layout,parent) {
    const position=[],size=[];
    for(let axis=0;axis<2;axis++){
        const near=parent[axis]*layout.AnchorMin[axis],far=parent[axis]*layout.AnchorMax[axis];
        const stretch=layout.AnchorMin[axis]!==layout.AnchorMax[axis];
        size[axis]=Math.max(0,stretch?far-near+layout.OffsetMax[axis]-layout.OffsetMin[axis]:layout.OffsetMax[axis]);
        position[axis]=near+layout.OffsetMin[axis]-(stretch?0:layout.Pivot[axis]*size[axis]);
    }
    return {x:position[0],y:position[1],w:size[0],h:size[1]};
}

export class HuiDocument {
    constructor(source,label) {
        this.source=normalizeHui(source,label);this.nodes=new Map(this.source.Nodes.map(n=>[n.ElementId,n]));this.images=new Map();
        this.children=new Map();for(const n of this.source.Nodes){if(!this.children.has(n.ParentId))this.children.set(n.ParentId,[]);this.children.get(n.ParentId).push(n);}
    }
    async loadAssets(loadImage) {
        for(const path of new Set(this.source.Nodes.flatMap(n=>[n.ImagePath,n.Style.BackgroundImage]).filter(Boolean)))this.images.set(path,await loadImage(path));
    }
    setText(id,text){this.get(id).Text=String(text);}
    setVisible(id,visible){this.get(id).bVisible=!!visible;}
    get(id){const n=this.nodes.get(id);if(!n)throw Error(`UI element not found: ${id}`);return n;}
    paint(context,width,height,{density=Math.max(.5,Math.min(3,width/1280,height/720)),backdrop=null}={}) {
        if(!Number.isFinite(density)||density<=0)throw Error('Invalid UI density');
        if(density!==1){
            context.save();try{context.scale(density,density);
                const scaledBackdrop=backdrop?{x:backdrop.x/density,y:backdrop.y/density,width:backdrop.width/density,height:backdrop.height/density}:null;
                return this.paint(context,width/density,height/density,{density:1,backdrop:scaledBackdrop}).map(t=>({...t,x:t.x*density,y:t.y*density,w:t.w*density,h:t.h*density}));
            }finally{context.restore();}
        }
        const ui=new CanvasUI(context),targets=[];
        const intersect=(a,b)=>{const x=Math.max(a.x,b.x),y=Math.max(a.y,b.y);return {x,y,w:Math.max(0,Math.min(a.x+a.w,b.x+b.w)-x),h:Math.max(0,Math.min(a.y+a.h,b.y+b.h)-y)};};
        const draw=(node,parent,clip)=>{
            if(!node.bVisible)return;
            const local=node.ParentId<0?{x:0,y:0,w:width,h:height}:resolveHuiRect(node.Layout,[parent.w,parent.h]);
            const rect={...local,x:parent.x+local.x,y:parent.y+local.y},visible=intersect(rect,clip),s=node.Style;
            if(visible.w<=0||visible.h<=0)return;
            context.save();try{
                context.beginPath();context.rect(visible.x,visible.y,visible.w,visible.h);context.clip();
                if(node.ParentId>=0||!backdrop)ui.panel(rect.x,rect.y,rect.w,rect.h,normalizeColor(s.BackgroundColor),s.BorderRadius);
                if(s.BorderWidth>0){context.strokeStyle=normalizeColor(s.BorderColor);context.lineWidth=s.BorderWidth;context.stroke();}
                for(const path of [s.BackgroundImage,node.ImagePath]){const image=this.images.get(path);if(image)context.drawImage(image,rect.x,rect.y,rect.w,rect.h);}
                if(node.Text){context.textBaseline='top';ui.text(node.Text,rect.x,rect.y,{size:s.FontSize,color:normalizeColor(s.TextColor),font:'sans-serif'});}
                if(node.bInteractable)targets.push({...visible,element:node.ElementId,action:node.Type==='Button'?node.ElementId:null,value:''});
                for(const child of this.children.get(node.Id)||[])draw(child,rect,visible);
            }finally{context.restore();}
        };
        const viewport={x:0,y:0,w:width,h:height},root=this.source.Nodes.find(n=>n.ParentId<0);
        if(backdrop&&root.bVisible)ui.panel(backdrop.x,backdrop.y,backdrop.width,backdrop.height,normalizeColor(root.Style.BackgroundColor));
        draw(root,viewport,viewport);return targets;
    }
}
