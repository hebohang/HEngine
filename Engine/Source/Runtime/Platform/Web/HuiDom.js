import {normalizeHui,normalizeColor} from './HuiDocument.js';

// Browser backend for the same UIRes consumed by native RmlUi and mini-game canvas.
export class HuiDom {
    constructor(source,onClick,{container=document.body}={}) {
        this.source=normalizeHui(source);this.elements=new Map();this.onClick=onClick;
        this.host=document.createElement('div');this.host.dataset.heInputBlock='';
        Object.assign(this.host.style,{position:'fixed',inset:'0',zIndex:'100',overflow:'hidden',display:'none'});
        const make=n=>{
            const element=document.createElement(n.Type==='Button'?'button':n.Type==='Image'?'img':'div');
            element.dataset.huiElement=n.ElementId;
            const s=n.Style,l=n.Layout,css=element.style;
            Object.assign(css,{position:'absolute',boxSizing:'border-box',margin:'0',padding:'0',overflow:'hidden',backgroundColor:normalizeColor(s.BackgroundColor),color:normalizeColor(s.TextColor),fontSize:s.FontSize+'px',fontFamily:'sans-serif',borderRadius:s.BorderRadius+'px',border:`${s.BorderWidth}px solid ${normalizeColor(s.BorderColor)}`,pointerEvents:n.bInteractable?'auto':'none',textAlign:'left',whiteSpace:'nowrap',lineHeight:'normal',display:n.bVisible?'block':'none'});
            if(s.BackgroundImage){css.backgroundImage=`url("${s.BackgroundImage}")`;css.backgroundSize='100% 100%';}
            if(n.Type==='Image')element.src=n.ImagePath;
            else element.textContent=n.Text;
            if(n.ParentId<0)Object.assign(css,{left:'0',top:'0',width:'100%',height:'100%'});
            else for(let axis=0;axis<2;axis++){
                const near=axis?'top':'left',far=axis?'bottom':'right',size=axis?'height':'width';
                if(l.AnchorMin[axis]!==l.AnchorMax[axis]){css[near]=`calc(${l.AnchorMin[axis]*100}% + ${l.OffsetMin[axis]}px)`;css[far]=`calc(${(1-l.AnchorMax[axis])*100}% - ${l.OffsetMax[axis]}px)`;}
                else{css[near]=`calc(${l.AnchorMin[axis]*100}% + ${l.OffsetMin[axis]-l.Pivot[axis]*Math.max(0,l.OffsetMax[axis])}px)`;css[size]=Math.max(0,l.OffsetMax[axis])+'px';}
            }
            if(n.Type==='Button')element.addEventListener('click',()=>this.onClick(n.ElementId));
            this.elements.set(n.Id,element);return element;
        };
        this.surface=document.createElement('div');Object.assign(this.surface.style,{position:'absolute',transformOrigin:'0 0'});
        for(const n of this.source.Nodes)make(n);
        for(const n of this.source.Nodes)(n.ParentId<0?this.surface:this.elements.get(n.ParentId)).append(this.elements.get(n.Id));
        this.host.append(this.surface);container.append(this.host);
        this.resize=()=>{const scale=Math.max(.5,Math.min(3,innerWidth/1280,innerHeight/720));Object.assign(this.surface.style,{width:innerWidth/scale+'px',height:innerHeight/scale+'px',transform:`scale(${scale})`});};
        addEventListener('resize',this.resize);this.resize();
    }
    show(visible){this.host.style.display=visible?'block':'none';}
    dispose(){removeEventListener('resize',this.resize);this.host.remove();}
}
