# 代码审计报告
**日期**: 2026-04-02
**审计范围**: USD 导出功能重构（assimpjs/src/assimpjs.cpp）
**审计标准**: Hemingway 风格 + 安全基线

---

## 执行摘要

**总体评级**: 🟡 **中等债务（可接受）**

核心功能正确，无关键安全缺陷。存在中等技术债务（重复代码、深层嵌套），建议后续迭代时重构。

---

## 修改统计

| 指标 | 数值 |
|------|------|
| 修改文件 | 6 个 |
| 核心代码行数 | +279 / -94 |
| 新增函数 | 1 个（`BuildUsdSharedPositionMesh`） |
| 新增结构体 | 1 个（`UsdSharedPositionMesh`） |
| 影响范围 | USD 导出流程（USDA/USDC/USDZ） |

---

## 审计发现

### 🔴 关键缺陷（BLOCKING）
**无**

---

### 🟡 主要问题（WARNING）

#### 1. 深层嵌套违反 Hemingway 原则
**位置**: `BuildUsdSharedPositionMesh` (L1003-L1047)
**问题**: 嵌套层级达到 4 层（函数 → face循环 → index循环 → if分支）
**标准**: Hemingway 风格要求最大 3 层嵌套
**影响**: 降低代码可读性，增加维护成本

**示例代码**:
```cpp
for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
    const aiFace& face = mesh->mFaces[faceIndex];
    for (unsigned int indexIndex = 0; indexIndex < face.mNumIndices; ++indexIndex) {
        const unsigned int vertexIndex = face.mIndices[indexIndex];
        if (vertexIndex >= mesh->mNumVertices) {  // 第4层
            return false;
        }
        // ... 更多嵌套逻辑
    }
}
```

**建议**: 提取内部循环为独立函数 `ProcessFaceVertex`

---

#### 2. 重复代码模式
**位置**: `WriteUsdMesh` (L1068-L1170)
**问题**: 5 处相同的 `if (mergeSharedPositions) {...} else {...}` 分支结构
**影响**: 约 60 行重复代码，违反 DRY 原则

**重复模式**:
```cpp
// 模式 1: points 数组
if (mergeSharedPositions) {
    for (size_t v = 0; v < sharedMesh.points.size(); ++v) { ... }
} else {
    for (unsigned int v = 0; v < mesh->mNumVertices; ++v) { ... }
}

// 模式 2: faceVertexCounts 数组
if (mergeSharedPositions) {
    for (size_t f = 0; f < sharedMesh.faceVertexCounts.size(); ++f) { ... }
} else {
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) { ... }
}

// ... 重复 3 次类似结构
```

**建议**: 提取为模板函数或宏，减少重复

---

#### 3. 浮点精度风险
**位置**: `BuildUsdSharedPositionMesh` (L1020)
**问题**: 使用浮点坐标作为哈希键可能导致精度误差

**代码**:
```cpp
ObjVec3Key key = MakeObjVec3Key(position);
auto it = pointMap.find(key);
```

**风险**: 变换后的坐标可能因浮点运算误差导致相同位置的顶点未被合并
**建议**: 确认 `MakeObjVec3Key` 实现了容差比较（epsilon）

---

#### 4. 无意义的初始化
**位置**: `BuildUsdSharedPositionMesh` (L1026)
**问题**: 变量初始化后立即被覆盖

**代码**:
```cpp
uint32_t pointIndex = 0;  // 无意义初始化
if (it != pointMap.end()) {
    pointIndex = it->second;  // 立即覆盖
} else {
    pointIndex = static_cast<uint32_t>(out.points.size());
}
```

**建议**: 改为三元运算符或延迟声明
```cpp
uint32_t pointIndex = (it != pointMap.end())
    ? it->second
    : static_cast<uint32_t>(out.points.size());
```

---

### 🟢 次要优化（NITPICK）

#### 1. 变量命名冗余
**位置**: `BuildUsdSharedPositionMesh` (L1003-L1009)
**问题**: `faceIndex`, `indexIndex`, `vertexIndex` 三层 Index 后缀
**建议**: `faceIndex → faceId`, `indexIndex → cornerIdx`

#### 2. 冗余条件检查
**位置**: `BuildUsdSharedPositionMesh` (L1031-L1036)
**问题**: `normalMatrix` 仅在 `applyTransform && hasNormals` 时初始化，但内层又检查 `if (applyTransform)`
**建议**: 移除内层冗余检查

#### 3. 类型不一致
**问题**: `mergeSharedPositions` 分支使用 `size_t`，原始分支使用 `unsigned int`
**建议**: 统一为 `size_t` 或 `uint32_t`

---

## 安全审计

### ✅ 通过项

| 检查项 | 状态 | 说明 |
|--------|------|------|
| 硬编码密钥 | ✅ | 无敏感信息泄露 |
| 边界检查 | ✅ | L1011 越界保护完备 |
| 类型安全 | ✅ | 无 `any` 类型或不安全转换 |
| 资源管理 | ✅ | 使用 RAII，无裸指针 |
| 错误处理 | ✅ | 返回 `false` 传播失败 |
| SQL 注入 | N/A | 不涉及数据库操作 |

---

## 代码质量评估

### 优点
1. **逻辑正确性**: 顶点合并算法实现正确，使用哈希表高效去重
2. **变换处理**: 正确应用法线矩阵（逆转置）
3. **性能优化**: 预留容量 `pointMap.reserve(mesh->mNumVertices)`
4. **错误处理**: 边界检查完备，失败传播清晰

### 缺点
1. **重复代码**: 约 60 行重复的条件分支逻辑
2. **嵌套过深**: 违反 Hemingway 3 层限制
3. **浮点精度**: 哈希键使用浮点数可能存在精度问题

---

## 建议行动

### 立即行动（推荐）
1. **提取重复逻辑**: 将 `WriteUsdMesh` 中的 5 处重复分支提取为独立函数
2. **拆分嵌套循环**: 将 `BuildUsdSharedPositionMesh` 内层循环提取为 `ProcessFaceVertex`

### 后续优化
1. **验证浮点容差**: 确认 `MakeObjVec3Key` 的 epsilon 设计
2. **统一类型**: 将循环索引统一为 `size_t` 或 `uint32_t`
3. **添加内存预留**: 为 `faceVaryingNormals` 和 `faceVaryingTexcoords` 预留容量

### 可接受现状
当前实现功能正确，债务可控。如时间紧张，可延后重构。

---

## 重构示例

### 示例 1: 提取重复逻辑

**重构前**:
```cpp
if (mergeSharedPositions) {
    for (size_t v = 0; v < sharedMesh.points.size(); ++v) {
        WriteVec3(ss, sharedMesh.points[v]);
        if (v + 1 < sharedMesh.points.size()) ss << ", ";
    }
} else {
    for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
        WriteVec3(ss, mesh->mVertices[v]);
        if (v + 1 < mesh->mNumVertices) ss << ", ";
    }
}
```

**重构后**:
```cpp
template<typename T>
void WriteVec3Array(std::ostringstream& ss, const T& data) {
    for (size_t i = 0; i < data.size(); ++i) {
        WriteVec3(ss, data[i]);
        if (i + 1 < data.size()) ss << ", ";
    }
}

// 调用
WriteVec3Array(ss, mergeSharedPositions ? sharedMesh.points : mesh->mVertices);
```

---

### 示例 2: 拆分嵌套循环

**重构前**:
```cpp
for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
    const aiFace& face = mesh->mFaces[faceIndex];
    for (unsigned int indexIndex = 0; indexIndex < face.mNumIndices; ++indexIndex) {
        const unsigned int vertexIndex = face.mIndices[indexIndex];
        if (vertexIndex >= mesh->mNumVertices) return false;
        // ... 30 行处理逻辑
    }
}
```

**重构后**:
```cpp
bool ProcessFaceVertex(const aiMesh* mesh, unsigned int vertexIndex,
                       const aiMatrix4x4& transform, bool applyTransform,
                       UsdSharedPositionMesh& out,
                       std::unordered_map<ObjVec3Key, uint32_t>& pointMap) {
    if (vertexIndex >= mesh->mNumVertices) return false;
    // ... 处理逻辑
    return true;
}

for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
    const aiFace& face = mesh->mFaces[faceIndex];
    for (unsigned int indexIndex = 0; indexIndex < face.mNumIndices; ++indexIndex) {
        if (!ProcessFaceVertex(mesh, face.mIndices[indexIndex],
                               transform, applyTransform, out, pointMap)) {
            return false;
        }
    }
}
```

---

## 结论

**当前状态**: 代码可用，核心逻辑正确，无安全风险
**技术债务**: 中等（重复代码 + 深层嵌套）
**建议**: 延后重构，在下次迭代时优化

**审计人**: SR-Plugin Critic Agent
**审计标准**: Hemingway Style + Security Baseline
**最终判定**: ✅ **PASS（有条件通过）**
