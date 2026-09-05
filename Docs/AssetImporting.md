# glTF / Mesh 资源导入

HEngine 的模型导入由 `HAssetImporter` 和 `AssimpImporter` 完成。导入结果是可提交的
`.hmesh`、`.bin`、`.hmat` 与纹理文件；Editor 和 Game 只读取这些引擎资源，不在运行时依赖
源 `.gltf`、外部模型目录或构建机路径。

## glTF 保真规则

- 保留 `aiProcess_FlipUVs`。Assimp 的 glTF reader 会先把 glTF UV 转成 Assimp 坐标约定，
  HEngine 的图片上传不做垂直翻转，因此 post-process 的第二次转换用于恢复源 glTF UV。
- 无蒙皮、无动画的静态模型会烘焙完整 node hierarchy。源文件中的单位缩放、旋转、平移和
  mesh instance 因此进入 `.hmesh`，导入后以 identity transform 放入场景即可复现源布局。
- 只生成实际被 primitive 引用的材质；同一源纹理只复制一次，并允许多个材质共享引用。
- glTF 的 min/mag filter 与 wrap S/T 会保存进 `.hmat`。缺失字段继续使用资源类型的默认值。
- `MASK` 材质生成 mip 时保留 alpha coverage，同时把通过 alpha test 的最近 texel RGB 扩散到
  透明 padding。alpha 本身不变，可避免黑色透明区污染叶片、布边等轮廓。

这些规则不对文件名或具体模型做特殊处理。Sponza、其他 glTF 和后续重新导入的静态模型走
同一条路径。

## 可复现导入

先构建独立导入工具：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 `
  -Target HAssetImporter -Configuration RelWithDebInfo

out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe `
  C:/WorkSpace/HEngineDev/Project `
  E:/Models/MyModel/glTF/MyModel.gltf `
  C:/WorkSpace/HEngineDev/Project/Assets/Models/.import-staging
```

三个参数依次为 project root、source model 和 output directory，且都必须是绝对路径；output
directory 必须已经存在并位于项目或引擎的 `Assets` 下。导入前建议使用空的 staging 目录，
确认生成资源和引用闭包后再替换正式目录。不要在包含旧导入
产物的目录上直接覆盖，因为自动避让重名会产生 `_0`、`_1` 纹理和陈旧未引用材质。

导入后的最低检查项：

1. `.hmesh` 中没有源目录绝对路径，所有 material/texture 引用均位于项目内。
2. primitive 使用的每个 `.hmat` 都存在，且没有额外的未引用默认材质。
3. 纹理数量与源 glTF 的唯一 image 集一致，共享纹理没有重复副本。
4. Editor 分别以 `-GraphicsApi DX12` 和 `-GraphicsApi Vulkan` 打开同一场景，并检查
   base-color debug view、法线方向、MASK 边缘和正常光照视图。

## 当前边界

- 目前材质路径使用 `TEXCOORD_0`；需要其他 UV set 的模型应在扩展导入 ABI 后再启用。
- `KHR_texture_transform` 尚未进入 HEngine material ABI。导入含该扩展的模型时应明确报出并
  补充支持，不能通过手工改 UV 静默代偿。
- `.hmat` 已保留每个纹理的 sampler 元数据；当前 PBR renderer 仍使用后端一致的共享 material
  sampler。Sponza 使用 REPEAT，与该共享 sampler 一致。需要同一 draw 中混用 Clamp/Mirror 的
  资产，应先把 sampler selection 纳入 material descriptor ABI。
