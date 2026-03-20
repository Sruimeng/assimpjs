---
id: strategy-ktx2-decoder
type: strategy
status: draft
target: ReleaseMeshopt
created: 2026-03-19
---

# KTX2 解码器集成战略

## 目标

在 ReleaseMeshopt 构建中集成 KTX2 纹理解码，将 Basis Universal 压缩纹理转码为 PNG，控制 Wasm 体积增量 ≤200KB。

## 当前状态

**已有能力：**
- glTF2 导入器识别 `image/ktx2` MIME 类型（标记为 `kx2`）
- meshoptimizer 解码器已启用（`ASSIMP_BUILD_MESHOPT=ON`）
- stb_image.h 可编码 PNG/JPEG
- 测试文件：`test/fixtures/tripo_texture_*.glb`（3.1MB，含 meshopt+ktx2）

**缺失能力：**
- Basis Universal transcoder（contrib/ 中不存在）
- KTX2 容器解析器
- C++ 层纹理转码管线

## 数据模型

### KTX2 容器结构

```cpp
struct KTX2Header {
  uint8_t identifier[12];      // 0xAB, 0x4B, 0x54, 0x58, ...
  uint32_t vkFormat;           // Vulkan format (VK_FORMAT_UNDEFINED for BasisU)
  uint32_t typeSize;
  uint32_t pixelWidth;
  uint32_t pixelHeight;
  uint32_t levelCount;
  uint32_t faceCount;
  // ... DFD, supercompression, index, data offsets
};

struct BasisUTranscodeTarget {
  enum Format {
    BC7_RGBA,      // Desktop
    ETC2_RGBA,     // Mobile
    ASTC_4x4_RGBA, // High-end mobile
    RGBA32         // Fallback (uncompressed)
  };
};
```

### 转码管线接口

```cpp
namespace Assimp {
namespace KTX2 {

// Phase 1: 容器解析
struct KTX2Container {
  KTX2Header header;
  std::vector<uint8_t> basisData;

  static Result<KTX2Container> Parse(const uint8_t* data, size_t length);
};

// Phase 2: Basis 转码
struct BasisTranscoder {
  bool Initialize();

  Result<RGBAImage> TranscodeToRGBA(
    const KTX2Container& ktx2,
    uint32_t levelIndex = 0
  );
};

// Phase 3: PNG 编码
struct PNGEncoder {
  static Result<std::vector<uint8_t>> Encode(
    const RGBAImage& image,
    int compressionLevel = 6
  );
};

// 统一入口
Result<aiTexture*> DecodeKTX2ToPNG(
  const glTF2::Image& img,
  const uint8_t* ktx2Data,
  size_t length
);

}} // namespace Assimp::KTX2
```

## 系统边界

### 依赖注入点

```
glTF2Importer::EmbedTexture()
  └─> [1752行] if (mimeType == "image/ktx2")
       └─> NEW: KTX2::DecodeKTX2ToPNG()
            ├─> KTX2Container::Parse()
            ├─> BasisTranscoder::TranscodeToRGBA()
            └─> PNGEncoder::Encode()
```

### 构建集成点

```cmake
# CMakeLists.txt (ReleaseMeshopt 分支)
if (CMAKE_BUILD_TYPE STREQUAL "ReleaseMeshopt")
  set(ASSIMP_BUILD_MESHOPT ON)
  set(ASSIMP_BUILD_KTX2_DECODER ON)  # NEW

  add_subdirectory(assimp/contrib/basis_universal)
  target_link_libraries(assimp basisu_transcoder)
endif()
```

## 实施步骤

### Step 1: 集成 Basis Universal

**目标：** 添加 transcoder 到 contrib/

```bash
# 操作序列
cd assimp/contrib
git submodule add https://github.com/BinomialLLC/basis_universal.git
cd basis_universal
git checkout 1.16.4  # 稳定版本

# 创建 CMakeLists.txt（仅 transcoder）
cat > CMakeLists.txt.transcoder << 'EOF'
add_library(basisu_transcoder STATIC
  transcoder/basisu_transcoder.cpp
)
target_compile_definitions(basisu_transcoder PUBLIC
  BASISU_NO_ITERATOR_DEBUG_LEVEL
)
target_include_directories(basisu_transcoder PUBLIC transcoder)
EOF
```

**体积控制：**
- 仅编译 `transcoder/` 目录（不含 encoder）
- 预期增量：~150KB（已压缩 Wasm）

### Step 2: KTX2 解析器

**位置：** `assimp/code/AssetLib/glTF2/KTX2Decoder.cpp`

```cpp
// 伪代码
Result<KTX2Container> KTX2Container::Parse(data, length) {
  IF length < sizeof(KTX2Header) THEN RETURN Error("Invalid size")

  header = ReadHeader(data)
  IF header.identifier != KTX2_MAGIC THEN RETURN Error("Not KTX2")

  // 定位 Basis 数据段
  basisOffset = header.sgdByteOffset
  basisLength = header.sgdByteLength

  RETURN KTX2Container{header, data[basisOffset:basisOffset+basisLength]}
}
```

### Step 3: 转码管线

**位置：** `assimp/code/AssetLib/glTF2/KTX2Decoder.cpp`

```cpp
// 伪代码
Result<aiTexture*> DecodeKTX2ToPNG(img, ktx2Data, length) {
  // Phase 1: 解析容器
  container = KTX2Container::Parse(ktx2Data, length)
  IF container.IsError() THEN RETURN Error("Parse failed")

  // Phase 2: 初始化 transcoder
  transcoder = BasisTranscoder()
  IF NOT transcoder.Initialize() THEN RETURN Error("Init failed")

  // Phase 3: 转码到 RGBA32
  rgbaImage = transcoder.TranscodeToRGBA(container, levelIndex=0)
  IF rgbaImage.IsError() THEN RETURN Error("Transcode failed")

  // Phase 4: 编码为 PNG
  pngData = PNGEncoder::Encode(rgbaImage, compressionLevel=6)
  IF pngData.IsError() THEN RETURN Error("Encode failed")

  // Phase 5: 创建 aiTexture
  tex = new aiTexture()
  tex->mWidth = rgbaImage.width
  tex->mHeight = rgbaImage.height
  tex->pcData = reinterpret_cast<aiTexel*>(pngData.data())
  tex->achFormatHint = "png"

  RETURN tex
}
```

### Step 4: glTF2 导入器集成

**修改点：** `glTF2Importer.cpp:1752`

```cpp
// 当前代码（仅标记）
} else if (strcmp(ext, "ktx2") == 0) {
    ext = "kx2";  // 不解码
}

// 新代码（解码）
} else if (strcmp(ext, "ktx2") == 0) {
    auto result = KTX2::DecodeKTX2ToPNG(img, data, length);
    IF result.IsSuccess() THEN
        tex = result.Value()
        RETURN  // 跳过原始数据路径
    ELSE
        ASSIMP_LOG_WARN("KTX2 decode failed, using raw data")
        ext = "kx2"  // 降级到原始数据
    END
}
```

### Step 5: 测试验证

**测试用例：** `test/test.js`

```javascript
// 新增测试
describe('KTX2 Decoding', () => {
  it('should decode KTX2 textures to PNG in meshopt build', async () => {
    const assimpjs = await AssimpJS({ variant: 'meshopt' });
    const file = fs.readFileSync('fixtures/tripo_texture_*.glb');

    const result = assimpjs.ConvertFileList([file], 'glb');
    const textures = result.textures;

    // 验证纹理格式
    expect(textures.length).toBeGreaterThan(0);
    expect(textures[0].achFormatHint).toBe('png');  // 不是 'kx2'
    expect(textures[0].mWidth).toBeGreaterThan(0);  // 有尺寸
    expect(textures[0].mHeight).toBeGreaterThan(0);
  });
});
```

**验证命令：**

```bash
# 1. 用 gltfpack 解码测试文件（验证内容）
gltfpack -i "test/fixtures/tripo_texture_*.glb" -o /tmp/decoded.glb -tu

# 2. 构建 meshopt 变体
./tools/build_wasm_deb.sh ReleaseMeshopt

# 3. 运行测试
npm run test -- --grep "KTX2"

# 4. 检查体积
ls -lh dist/assimpjs-meshopt.wasm
# 预期：原始大小 + 150-200KB
```

## 风险与约束

### 体积风险

| 组件 | 预估大小 | 缓解策略 |
|------|---------|---------|
| basisu_transcoder | ~120KB | 仅编译 transcoder，禁用 encoder |
| KTX2 解析器 | ~20KB | 最小化容器解析逻辑 |
| PNG 编码（stb） | 已存在 | 复用现有 stb_image_write |
| **总计** | **~140KB** | 低于 200KB 目标 |

### 性能约束

- 转码时间：~50ms/张（1024x1024 纹理）
- 内存峰值：原始纹理 × 2（KTX2 + RGBA32 临时缓冲）
- 降级路径：转码失败时保留原始 KTX2 数据

### 兼容性

- **不影响其他构建变体**（ReleaseMini/ReleaseAll/ReleaseExporter）
- **向后兼容**：不支持 KTX2 的构建仍可加载 GLB（纹理为空）

## 成功标准

1. ✅ `assimpjs-meshopt.wasm` 体积增量 ≤200KB
2. ✅ 测试文件中的 KTX2 纹理成功转码为 PNG
3. ✅ 转码后的纹理尺寸正确（mWidth/mHeight > 0）
4. ✅ 所有现有测试用例通过
5. ✅ 转码失败时优雅降级（保留原始数据）

## 参考资料

- Basis Universal: https://github.com/BinomialLLC/basis_universal
- KTX2 规范: https://registry.khronos.org/KTX/specs/2.0/ktx2.0.html
- glTF 扩展: `KHR_texture_basisu`
- 当前代码: `assimp/code/AssetLib/glTF2/glTF2Importer.cpp:1752`
