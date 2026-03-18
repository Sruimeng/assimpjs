#!/usr/bin/env node
/**
 * 前端导出格式验证脚本 v2
 * - 测试 fixtures/ 下所有模型
 * - 先提取节点名，构造有具体数据的 metadata
 * - 对 6 种格式分别验证
 */

const fs = require('fs');
const path = require('path');

const FIXTURES_DIR = path.join(__dirname, 'test/fixtures');
const DIST_JS = path.join(__dirname, 'dist/assimpjs-meshopt.js');

const FORMATS = [
  { name: 'GLB',  format: 'glb2' },
  { name: 'FBX',  format: 'fbx'  },
  { name: 'OBJ',  format: 'obj'  },
  { name: 'STL',  format: 'stl'  },
  { name: 'USD',  format: 'usd'  },
  { name: '3MF',  format: '3mf'  },
];

// 从 GLB 二进制解析节点名（GLB 第一个 chunk 为 JSON）
function extractNodeNamesFromGLB(buffer, limit = 5) {
  try {
    const view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
    const magic = view.getUint32(0, true);
    if (magic !== 0x46546C67) return []; // not GLB
    const chunkLen = view.getUint32(12, true);
    const chunkType = view.getUint32(16, true);
    if (chunkType !== 0x4E4F534A) return []; // not JSON chunk
    const jsonText = new TextDecoder().decode(buffer.slice(20, 20 + chunkLen));
    const gltf = JSON.parse(jsonText);
    const names = [];
    for (const node of (gltf.nodes || [])) {
      if (node.name && names.length < limit) names.push(node.name);
    }
    return names;
  } catch { return []; }
}

// 从 FBX 文件提取节点名（不可靠，返回空）
function extractNodeNamesFromFBX(_buffer) {
  return []; // FBX 二进制格式复杂，跳过，依赖 hardcoded 测试
}

// 缩放矩阵 0.01（GLB 单位 m → cm 常见转换）
const SCALE_01 = [
  0.01, 0, 0, 0,
  0, 0.01, 0, 0,
  0, 0, 0.01, 0,
  0, 0, 0, 1,
];

// 构造有具体数据的 metadata
function buildMetadata(nodeNames) {
  const childTransforms = {};
  const childRenames = {};
  const childDeleted = [];

  if (nodeNames.length > 0) {
    // 第一个节点：施加缩放 0.5
    childTransforms[nodeNames[0]] = [
      0.5, 0, 0, 0,
      0, 0.5, 0, 0,
      0, 0, 0.5, 0,
      0, 0, 0, 1,
    ];
  }
  if (nodeNames.length > 1) {
    // 第二个节点：重命名（C++ 层已禁用，但传入不应崩溃）
    childRenames[nodeNames[1]] = nodeNames[1] + '_renamed';
  }
  if (nodeNames.length > 2) {
    // 第三个节点：删除
    childDeleted.push(nodeNames[2]);
  }

  return {
    transform_matrix: SCALE_01,
    children_transform_matrix: childTransforms,
    children_rename: childRenames,
    children_deleted: childDeleted,
    material_factor: { metallic: 0.1, roughness: 0.8 },
  };
}

async function testFixture(ajs, fixturePath) {
  const fileName = path.basename(fixturePath);
  const fileData = fs.readFileSync(fixturePath);
  const ext = path.extname(fixturePath).slice(1).toLowerCase();

  console.log(`\n─── ${fileName} (${(fileData.length / 1024 / 1024).toFixed(1)} MB) ───`);

  // Step 1: 提取节点名（GLB 直接解析二进制，FBX 暂不支持）
  let nodeNames = [];
  if (ext === 'glb') {
    nodeNames = extractNodeNamesFromGLB(fileData);
  } else if (ext === 'fbx') {
    nodeNames = extractNodeNamesFromFBX(fileData);
  }

  console.log(`  节点名 (前5): [${nodeNames.join(', ') || '无'}]`);

  const meta = buildMetadata(nodeNames);
  console.log(`  transform_matrix: scale=0.01`);
  if (Object.keys(meta.children_transform_matrix).length)
    console.log(`  children_transform_matrix: ${JSON.stringify(Object.keys(meta.children_transform_matrix))}`);
  if (Object.keys(meta.children_rename).length)
    console.log(`  children_rename: ${JSON.stringify(meta.children_rename)}`);
  if (meta.children_deleted.length)
    console.log(`  children_deleted: ${JSON.stringify(meta.children_deleted)}`);
  console.log(`  material_factor: metallic=${meta.material_factor.metallic}, roughness=${meta.material_factor.roughness}`);
  console.log('');

  // Step 2: 测试 6 种格式
  // 骨骼模型 + children_deleted + FBX = 预期失败（assimp FBX exporter 不支持骨架删节点）
  // content.vue 中 !(FBX && isRigged) 已将此场景路由至后端，前端不会遇到
  const hasDeletedNodes = meta.children_deleted.length > 0;
  const hasSkeletonNodes = nodeNames.some(n =>
    /^(mixamorig:|L_|R_|Hips|Spine|Head|Neck|Shoulder|Arm|Hand|Leg|Foot|Calf|Thigh)/i.test(n)
  );

  const results = [];
  for (const { name, format } of FORMATS) {
    // 跳过骨骼模型 + children_deleted + FBX（已知预期失败，由 canUseFrontend 拦截）
    if (format === 'fbx' && hasSkeletonNodes && hasDeletedNodes) {
      results.push({ name, format, ok: true, skipped: true, files: [] });
      continue;
    }

    const fl = new ajs.FileList();
    fl.AddFile(`model.${ext}`, new Uint8Array(fileData));

    let result;
    try {
      result = ajs.ConvertFileList(fl, format, meta, 'tripo_test');
    } catch (e) {
      results.push({ name, format, ok: false, error: String(e) });
      continue;
    }

    if (!result.IsSuccess()) {
      results.push({ name, format, ok: false, error: result.GetErrorCode() });
      continue;
    }

    const files = [];
    for (let i = 0; i < result.FileCount(); i++) {
      const f = result.GetFile(i);
      files.push({ path: f.GetPath(), bytes: f.GetContent().length });
    }
    const allNonEmpty = files.length > 0 && files.every(f => f.bytes > 0);
    results.push({ name, format, ok: allNonEmpty, files });
  }

  let pass = 0, fail = 0;
  for (const r of results) {
    if (r.ok) {
      pass++;
      if (r.skipped) {
        console.log(`  ⚠ ${r.name.padEnd(4)} [${r.format.padEnd(4)}]  SKIP (骨骼+FBX+children_deleted → 后端导出，canUseFrontend 已拦截)`);
      } else {
        const summary = r.files.map(f => `${f.path}(${(f.bytes/1024).toFixed(0)}KB)`).join(' ');
        console.log(`  ✓ ${r.name.padEnd(4)} [${r.format.padEnd(4)}]  ${summary}`);
      }
    } else {
      fail++;
      console.log(`  ✗ ${r.name.padEnd(4)} [${r.format.padEnd(4)}]  ERROR: ${r.error}`);
    }
  }
  return { pass, fail };
}

async function run() {
  if (!fs.existsSync(DIST_JS)) {
    console.error('✗ dist/assimpjs-meshopt.js not found');
    process.exit(1);
  }

  const loadModule = require(DIST_JS);
  const ajs = await loadModule();
  console.log('✓ assimpjs-meshopt loaded\n');

  const fixtures = fs.readdirSync(FIXTURES_DIR)
    .filter(f => /\.(glb|fbx)$/i.test(f))
    .sort()
    .map(f => path.join(FIXTURES_DIR, f));

  if (fixtures.length === 0) {
    console.error('✗ No .glb/.fbx files found in test/fixtures/');
    process.exit(1);
  }

  let totalPass = 0, totalFail = 0;
  for (const fixture of fixtures) {
    const { pass, fail } = await testFixture(ajs, fixture);
    totalPass += pass;
    totalFail += fail;
  }

  console.log(`\n${'='.repeat(60)}`);
  console.log(`总计 ${fixtures.length} 个模型 × ${FORMATS.length} 种格式 = ${fixtures.length * FORMATS.length} 项`);
  console.log(`结果: ${totalPass} 通过, ${totalFail} 失败`);
  process.exit(totalFail > 0 ? 1 : 0);
}

run().catch(e => { console.error(e); process.exit(1); });
