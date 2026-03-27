# Strategy: FBX 导出全格式失败调试

## 1. 态势感知
* **Context:** 特定 FBX 模型 (`tripo_input_43cc0b6c-a016-4ff7-aa18-26e5a9affab0.fbx`) 的所有导出格式均失败。需要定位失败原因并修复。
* **Active Constitution:**
    * **Technical:**
        * FBX+PBR/Rigged 走后端导出路径
        * 前端 WASM 导出条件：非 FBX 或 (FBX 且非 PBR 且非 Rigged)
        * 3MF 格式存在缩放因子问题 (scale=1000)
        * texture_packaging 路由规则影响纹理打包
    * **Style:** **严格遵循 `skills/style-hemingway.md`** (冰山原则)
    * **Security:** **严格遵循 `skills/security-baseline.md`** (零信任)

## 2. 评估
<Assessment>
**Complexity:** Level 3 (Deep)
**Critical Risks:**
- 文件格式损坏或不符合 FBX 规范
- C++ 层 Assimp 导入器解析失败
- 前端 WASM 内存溢出或类型错误
- 后端路由判断逻辑错误
</Assessment>

## 3. 抽象规范 (逻辑)

<LogicSpec>
### 数据模型

```
FBXFile {
  magic_bytes: [0x4B, 0x61, 0x79, 0x64, 0x61, 0x72, 0x61, ...]  // "Kaydara FBX Binary"
  version: uint32
  scene_graph: {
    nodes: Node[]
    materials: Material[]
    textures: Texture[]
    animations: Animation[]
  }
}

ExportParams {
  format: string           // "glb", "fbx", "obj", "stl", "usdz", "3mf"
  texture_packaging: bool  // 纹理打包标志
  is_pbr: bool            // PBR 材质标志
  is_rigged: bool         // 骨骼绑定标志
}

ExportRoute {
  backend: format in ["fbx", "usdz"] AND (is_pbr OR is_rigged)
  frontend: NOT backend
}
```

### 调试流程伪代码

```
function diagnose_fbx_export(file_path, export_format):
  // Phase 1: 文件完整性检查
  magic = read_bytes(file_path, 0, 21)
  if magic != "Kaydara FBX Binary  ":
    return ERROR("Invalid FBX magic bytes")

  version = read_uint32(file_path, 23)
  if version < 7100:
    return ERROR("FBX version too old: " + version)

  // Phase 2: Assimp 导入测试
  scene = assimp.ImportFile(file_path, aiProcess_Triangulate | aiProcess_GenNormals)
  if scene == NULL:
    error_msg = assimp.GetErrorString()
    return ERROR("Assimp import failed: " + error_msg)

  // Phase 3: 场景图验证
  if scene.mNumMeshes == 0:
    return ERROR("No meshes in scene")

  for mesh in scene.mMeshes:
    if mesh.mNumVertices == 0:
      return ERROR("Empty mesh detected")
    if mesh.mNumFaces == 0:
      return ERROR("No faces in mesh")

  // Phase 4: 材质与纹理检查
  for material in scene.mMaterials:
    texture_count = material.GetTextureCount(aiTextureType_DIFFUSE)
    if texture_count > 0:
      texture_path = material.GetTexture(aiTextureType_DIFFUSE, 0)
      if NOT file_exists(texture_path):
        LOG_WARNING("Missing texture: " + texture_path)

  // Phase 5: 导出路由判断
  is_pbr = detect_pbr_material(scene)
  is_rigged = scene.mNumAnimations > 0 OR has_bones(scene)

  if export_format in ["fbx", "usdz"] AND (is_pbr OR is_rigged):
    route = "backend"
  else:
    route = "frontend"

  // Phase 6: 导出器测试
  if route == "frontend":
    result = wasm_export(scene, export_format)
  else:
    result = backend_export(file_path, export_format)

  if result.success:
    return SUCCESS(result.output_path)
  else:
    return ERROR("Export failed: " + result.error)
```

### 关键检查点

```
Checkpoint 1: Magic Bytes
  offset: 0
  expected: "Kaydara FBX Binary  \x1A\x00"
  length: 23 bytes

Checkpoint 2: Version
  offset: 23
  type: uint32_le
  min_supported: 7100

Checkpoint 3: Scene Graph
  nodes: scene->mRootNode != NULL
  meshes: scene->mNumMeshes > 0
  vertices: mesh->mNumVertices > 0
  faces: mesh->mNumFaces > 0

Checkpoint 4: Exporter Flags
  aiProcess_Triangulate: 确保三角化
  aiProcess_GenNormals: 生成法线
  aiProcess_JoinIdenticalVertices: 合并重复顶点
  aiProcess_ValidateDataStructure: 验证数据结构
```
</LogicSpec>

## 4. 执行计划

<ExecutionPlan>

**Block A: 文件诊断工具**
1. 创建 `/Users/sruim/Desktop/projects/tripo/assimpjs/tools/diagnose_fbx.cpp`
    * *Constraint:* 使用 Assimp C++ API，输出 JSON 格式诊断报告
    * *Output:* 包含 magic bytes、version、scene stats、material info

2. 编译诊断工具
    * *Command:* `g++ -o diagnose_fbx diagnose_fbx.cpp -lassimp -std=c++17`

3. 运行诊断
    * *Command:* `./diagnose_fbx /Users/sruim/Downloads/tripo_input_43cc0b6c-a016-4ff7-aa18-26e5a9affab0.fbx > report.json`

**Block B: 前端导出路径调试**
1. 修改 `/Users/sruim/Desktop/projects/tripo/assimpjs/assimpjs/src/assimpjs-worker.ts`
    * *Action:* 添加详细错误日志，捕获 WASM 异常
    * *Constraint:* 使用 try-catch 包裹 Module.ccall，输出堆栈信息

2. 修改 `/Users/sruim/Desktop/projects/tripo1/fe-tripo-studio/app/composables/export-runner.ts`
    * *Action:* 添加导出前参数验证日志
    * *Constraint:* 记录 format、texture_packaging、is_pbr、is_rigged

**Block C: C++ 层增强**
1. 修改 `/Users/sruim/Desktop/projects/tripo/assimpjs/assimpjs/src/assimpjs.cpp`
    * *Action:* 在 `exportFile` 函数中添加场景验证
    * *Constraint:*
        * 检查 scene->mNumMeshes > 0
        * 检查每个 mesh 的 mNumVertices > 0
        * 返回详细错误码而非通用失败

2. 修改 `/Users/sruim/Desktop/projects/tripo/assimpjs/assimp/code/AssetLib/FBX/FBXExporter.cpp`
    * *Action:* 添加导出前数据完整性检查
    * *Constraint:*
        * 验证材质索引有效性
        * 验证纹理路径存在性
        * 对于 3MF 格式，应用 scale=1000 修正

**Block D: 修复方案分支**

### 分支 D1: 如果是文件损坏
1. 使用 Assimp 的 `aiProcess_ValidateDataStructure` 标志重新导入
2. 如果验证失败，返回明确错误信息给用户

### 分支 D2: 如果是前端 WASM 内存问题
1. 在 `assimpjs-worker.ts` 中增加内存限制检查
2. 对于大文件 (>10MB)，强制走后端路由

### 分支 D3: 如果是导出器参数错误
1. 在 `FBXExporter.cpp` 中添加参数校验
2. 对于不支持的材质类型，降级到基础材质

### 分支 D4: 如果是 3MF 缩放问题
1. 在 `exportFile` 中检测 format == "3mf"
2. 应用 `aiProcess_TransformUVCoords` 和自定义缩放矩阵

</ExecutionPlan>

## 5. 验证标准

```
Test Case 1: 文件完整性
  input: tripo_input_43cc0b6c-a016-4ff7-aa18-26e5a9affab0.fbx
  expected: magic bytes valid, version >= 7100

Test Case 2: 场景导入
  input: same file
  expected: scene->mNumMeshes > 0, no NULL pointers

Test Case 3: 前端导出 (GLB)
  input: same file, format="glb"
  expected: success OR clear error message

Test Case 4: 后端导出 (FBX)
  input: same file, format="fbx", is_pbr=true
  expected: routed to backend, success

Test Case 5: 3MF 缩放
  input: same file, format="3mf"
  expected: output scale corrected to 1000x
```

## 6. 回滚策略

如果修复导致回归:
1. 恢复 `assimpjs.cpp` 到 commit `e0480930`
2. 恢复 `export-runner.ts` 到 commit `540825b1`
3. 使用 git bisect 定位引入问题的提交
