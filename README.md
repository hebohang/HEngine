# HEngine
HEngine

Now only support platform Windows

OpenGL 4.5

TODO:
D3D11
D3D12
Vulkan

## 命名规范

### 命名法
统一采用Pascal命名法（文件夹、类名等），第三方库除外

CMakeLists.txt 的变量命名也采用 Pascal 命名法，比如：
```
set(ProjectRootDir "${CMAKE_CURRENT_SOURCE_DIR}")
```
这里变量 ProjectRootDir 采用 Pascal 命名法，与 CMAKE 自带变量区分（比如 CMAKE_CURRENT_SOURCE_DIR ）

### include 头文件顺序
首先include同级文件，其次是同Source文件，再次为第三方依赖（确保依赖顺序）
比如 Editor 中：
```
// 同级文件（同属于 Editor ）
#include "EditorLayer.h"

// 同Source文件（位于 Runtime 中）
#include <HEngine.h>
#include <Runtime/Core/EntryPoint.h>	 

// 第三方依赖
#include <imgui/imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
```

### 代码规范
HEngine 的代码规范偏Unreal，可参考：
https://docs.unrealengine.com/4.27/zh-CN/ProductionPipelines/DevelopmentSetup/CodingStandard/

原则：（后面的规则优先级高于前面的规则）
1. 尽量不使用下划线
1. 变量一律小写字母开头
1. 非类内静态变量（全局变量、局部变量），在前面加小写字母s
1. 如果是类内部成员变量，在前面加小写字母m
1. 如果是bool类型变量，在前面加小写字母b
1. 类内成员统一放在类的最末尾（方法置于前）

## Getting Started
**1. Downloading the repository**
`git clone git@github.com:hebohang/HEngine.git`

**2. You can choose one of the following methods to build HEngine:**

2.1 Run the Win-GenProjects.bat 

2.2 Run the following commands:
```
cd HEngine
cmake -B build
cmake --build build --parallel 4
```

2.3 Visual Studio: Open Folder, then choose HEngine folder)

## Credits
* Cherno Hazel
* BoomingTech Pilot