// Conservative bounds: missing metadata must keep geometry visible.
export function meshBounds(mesh){
    const min=[Infinity,Infinity,Infinity],max=[-Infinity,-Infinity,-Infinity];
    for(const part of mesh.primitives){
        const a=mesh.accessorVec[part.attributes.POSITION];
        if(a?.min?.length!==3||a?.max?.length!==3)return null;
        for(let i=0;i<3;i++){min[i]=Math.min(min[i],a.min[i]);max[i]=Math.max(max[i],a.max[i]);}
    }
    return min.every(Number.isFinite)&&max.every(Number.isFinite)?{min,max}:null;
}
export function geometryBounds(mesh){
    const min=[Infinity,Infinity,Infinity],max=[-Infinity,-Infinity,-Infinity];
    for(const part of mesh.parts)for(let i=0;i<part.position.length;i+=3)for(let axis=0;axis<3;axis++){
        min[axis]=Math.min(min[axis],part.position[i+axis]);max[axis]=Math.max(max[axis],part.position[i+axis]);
    }
    return min.every(Number.isFinite)&&max.every(Number.isFinite)?{min,max}:null;
}
export function transformBounds(bounds,matrix,padding=0){
    if(!bounds)return null;
    const min=[Infinity,Infinity,Infinity],max=[-Infinity,-Infinity,-Infinity];
    for(let corner=0;corner<8;corner++){
        const x=(corner&1?bounds.max:bounds.min)[0],y=(corner&2?bounds.max:bounds.min)[1],z=(corner&4?bounds.max:bounds.min)[2];
        for(let axis=0;axis<3;axis++){
            const value=matrix[axis]*x+matrix[4+axis]*y+matrix[8+axis]*z+matrix[12+axis];
            min[axis]=Math.min(min[axis],value-padding);max[axis]=Math.max(max[axis],value+padding);
        }
    }
    return {min,max};
}
export function nearBounds(bounds,x,z,radius){
    if(!bounds)return true;
    const dx=Math.max(bounds.min[0]-x,0,x-bounds.max[0]),dz=Math.max(bounds.min[2]-z,0,z-bounds.max[2]);
    return dx*dx+dz*dz<=radius*radius;
}
export function frustumPlanes(matrix){
    const planes=[];
    for(let axis=0;axis<3;axis++)for(const sign of [-1,1]){
        const plane=Array.from({length:4},(_,i)=>matrix[i*4+3]+sign*matrix[i*4+axis]);
        const length=Math.hypot(...plane.slice(0,3));planes.push(plane.map(v=>v/length));
    }
    return planes;
}
export function visibleBounds(bounds,planes){
    if(!bounds||!planes)return true;
    return planes.every(p=>p[0]*(p[0]>=0?bounds.max[0]:bounds.min[0])+p[1]*(p[1]>=0?bounds.max[1]:bounds.min[1])+p[2]*(p[2]>=0?bounds.max[2]:bounds.min[2])+p[3]>=0);
}
