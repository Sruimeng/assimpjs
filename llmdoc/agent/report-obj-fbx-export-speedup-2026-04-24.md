---
id: report-obj-fbx-export-speedup-2026-04-24
type: report
status: done
target: ReleaseMeshopt
created: 2026-04-24
---

# OBJ / FBX 导出提速报告

## 目标

降低 `ReleaseMeshopt` 构建下 `OBJ` 与 `FBX` 导出耗时。

输入重点：
- `tripo_texture_92d6272b-d900-4339-bafb-a4634e5742bc_meshopt.glb`
- `tripo_mesh_seg_3f8e6ffd-77bc-4301-8cbd-cb22b17d8d64_meshopt.glb`

## 修改范围

### 1. FBX 几何导出路径

文件：
- [assimp/code/AssetLib/FBX/FBXExporter.cpp](/Users/sruim/Desktop/projects/tripo/assimpjs/assimp/code/AssetLib/FBX/FBXExporter.cpp)

修改：
- 顶点去重从 `std::map<aiVector3D, size_t>` 改为哈希表
- UV 去重从 `std::map<aiVector3D, int32_t>` 改为哈希表
- 增加 `FbxVec3Key` / `FbxVecHash`
- 增加 `flattened_vertices` / `vertex_indices` / `polygon_data` / `normal_data` / `color_data` / `uv_data` / `uv_indices` 的总量预分配
- 将小块 `insert({...})` 改为直接 `push_back`

意图：
- 去掉红黑树查找
- 降低 `vector` 扩容次数
- 降低小对象构造和临时数组开销

### 2. FBX 二进制数组压缩策略

文件：
- [assimp/code/AssetLib/FBX/FBXExportNode.cpp](/Users/sruim/Desktop/projects/tripo/assimpjs/assimp/code/AssetLib/FBX/FBXExportNode.cpp)

修改：
- `compress2(..., Z_BEST_COMPRESSION)` 改为 `compress2(..., Z_BEST_SPEED)`

意图：
- 当前导出结果本身还会再打包成 `result.zip`
- 内层 FBX 数组压缩优先速度，比继续榨极限压缩率更划算

影响：
- 导出速度明显提升
- 包体轻微变大，可接受

### 3. OBJ 导出路径

文件：
- [assimpjs/src/assimpjs.cpp](/Users/sruim/Desktop/projects/tripo/assimpjs/assimpjs/src/assimpjs.cpp)
- [assimpjs/src/assimpjs_obj.cpp](/Users/sruim/Desktop/projects/tripo/assimpjs/assimpjs/src/assimpjs_obj.cpp)

修改：
- 预扫描实例，统计 `position / uv / normal` 总量
- `unordered_map` 与 `vector` 按总量 `reserve`
- 缓存 `hasTexcoords` / `hasNormals`
- 只有存在法线时才计算 `normalMatrix`
- 仅在需要时分配 `uvIndices` / `normalIndices`
- 面片写出时改为三条固定分支：
  - `v/vt/vn`
  - `v/vt`
  - `v//vn`

意图：
- 降低热循环内重复判断
- 减少无效分配
- 降低大文本导出时的常数开销

## 测试方法

构建命令：

```sh
source ./emsdk/emsdk_env.sh >/dev/null
emmake make -C build_wasm_meshopt -j"$(sysctl -n hw.ncpu)" AssimpJS
```

完成标记：

```txt
[100%] Built target AssimpJS
```

测速方法：
- 通过 `build_wasm_meshopt/ReleaseMeshopt/assimpjs-meshopt.js`
- 调用 `ConvertFileList(..., 'obj')`
- 调用 `ConvertFileList(..., 'fbx')`
- 记录 wall-clock 时间与 `result.zip` 大小

## 对比结果

### 样本 A

输入：
`/Users/sruim/Downloads/tripo_texture_92d6272b-d900-4339-bafb-a4634e5742bc_meshopt.glb`

#### OBJ

优化前：
- `212.29ms`
- `138.21ms`
- `129.94ms`
- 输出 `2,994,577` bytes

优化后：
- `134.43ms`
- `102.44ms`
- `101.03ms`
- `101.00ms`
- `98.64ms`
- 输出 `2,994,577` bytes

结论：
- 热态从约 `130ms+` 降到约 `100ms`
- 提升约 `20% ~ 30%`
- 输出体积无变化

#### FBX

优化前：
- `256.82ms`
- `237.06ms`
- `232.51ms`
- 输出 `2,982,605 ~ 2,982,606` bytes

中间态：
- 仅做哈希去重和预分配后，收益不稳定

最终优化后：
- `88.98ms`
- `69.76ms`
- `70.19ms`
- `69.75ms`
- `70.12ms`
- 输出 `2,989,237` bytes

结论：
- 关键收益来自 `Z_BEST_SPEED`
- 热态从约 `232ms+` 降到约 `70ms`
- 提升约 `70%`
- 体积增加约 `0.22%`

### 样本 B

输入：
`/Users/sruim/Downloads/tripo_mesh_seg_3f8e6ffd-77bc-4301-8cbd-cb22b17d8d64_meshopt.glb`

输入大小：
- `15,306,752` bytes

#### OBJ

优化后，5 次：
- `23.99s`
- `21.39s`
- `18.68s`
- `20.75s`
- `20.47s`
- 输出 `44,371,576` bytes

优化前：
- 单次 `180s` 超时未完成

结论：
- 保守提速 `> 7.5x`
- 当前版本已进入 `20s` 级

#### FBX

优化前，单次：
- `125.74s`
- 输出 `48,111,804` bytes

优化后，5 次：
- `8.07s`
- `9.41s`
- `9.73s`
- `8.10s`
- `7.17s`
- 输出 `50,304,822 ~ 50,304,825` bytes

结论：
- 提速约 `15.6x`
- 体积增加约 `4.56%`

## 总结

### 生效点

收益最大：
- FBX 二进制数组压缩改为 `Z_BEST_SPEED`

收益稳定：
- FBX 顶点 / UV 去重改哈希表
- OBJ 预分配与热循环分支收紧

### 结果

小模型：
- `OBJ` 从 `130ms` 级降到 `100ms` 级
- `FBX` 从 `230ms` 级降到 `70ms` 级

大模型：
- `OBJ` 从 `>180s` 降到 `20s` 级
- `FBX` 从 `125.74s` 降到 `8s~10s` 级

### 代价

`FBX` 包体上升：
- 小模型约 `0.22%`
- 大模型约 `4.56%`

当前判断：
- 对导出时延收益来说，这个代价可接受

## 后续方向

### OBJ

剩余热点大概率在：
- `ostringstream` 大文本拼接
- 最终 `result.zip` 打包

可继续做：
- 分块缓冲写出，替代超大 `ostringstream`
- 将“导出时间”和“zip 打包时间”拆开统计

### FBX

当前路径已明显改善。

若继续压时间，可试：
- 对超大数组引入“按大小阈值决定是否压缩”
- 区分 `Vertices / Normals / UV / Index` 的压缩策略
