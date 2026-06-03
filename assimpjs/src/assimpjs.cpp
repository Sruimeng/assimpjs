#include "assimpjs.hpp"

#include <assimp/Importer.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/config.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/matrix3x3.h>
#include <assimp/material.h>

#include <stdio.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../assimp/contrib/meshoptimizer/meshoptimizer.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../assimp/contrib/stb/stb_image.h"

extern "C" void* tdefl_write_image_to_png_file_in_memory (const void* pImage, int w, int h, int num_chans, size_t* pLen_out);
extern "C" void mz_free (void* p);

#ifdef ASSIMPJS_ENABLE_TINYUSDZ
#include "tydra/render-data.hh"
#include "tydra/usd-export.hh"
#include "tinyusdz.hh"
#include "usdc-writer.hh"
#endif

#ifndef ASSIMP_BUILD_NO_3MF_EXPORTER
#ifdef ASSIMP_USE_HUNTER
#include <zip/zip.h>
#else
#include "../../assimp/contrib/zip/src/zip.h"
#endif
#endif

static unsigned int GetImportFlagsForFormat (const std::string& format)
{
	unsigned int flags =
		aiProcess_Triangulate |
		aiProcess_GenUVCoords |
		aiProcess_JoinIdenticalVertices |
		aiProcess_LimitBoneWeights |
		aiProcess_SortByPType;

	// JoinIdenticalVertices 是 O(n log n) 空间哈希，大网格代价高。
	// 对已有良好顶点共享的格式（GLB/GLTF 输入）或无顶点共享概念的格式禁用。
	if (format == "glb" || format == "glb2" ||
	    format == "gltf" || format == "gltf2" ||
	    format == "stl" || format == "stlb" ||
	    format == "3mf" ||
	    format == "usd" || format == "usda" || format == "usdc" || format == "usdz") {
		flags &= ~aiProcess_JoinIdenticalVertices;
	}

	// FBX and OBJ support polygons natively; skip triangulation to preserve quad faces.
	if (format == "fbx" || format == "obj") {
		flags &= ~aiProcess_Triangulate;
	}

	if (format == "fbx" || format == "obj") {
		flags |= aiProcess_EmbedTextures;
	}
	return flags;
}

static bool TryCreateMatrix4 (const std::vector<float>& matrix16, aiMatrix4x4& matrix)
{
	if (matrix16.size () != 16) {
		return false;
	}
	// Input is column-major (GLTF/JS convention); aiMatrix4x4 is row-major — transpose.
	matrix = aiMatrix4x4 (
		matrix16[0], matrix16[4], matrix16[8],  matrix16[12],
		matrix16[1], matrix16[5], matrix16[9],  matrix16[13],
		matrix16[2], matrix16[6], matrix16[10], matrix16[14],
		matrix16[3], matrix16[7], matrix16[11], matrix16[15]
	);
	return true;
}

static bool ApplyTransformToRootNode (const aiScene* scene, const aiMatrix4x4& transform)
{
	if (scene == nullptr || scene->mRootNode == nullptr) {
		return false;
	}
	aiScene* mutableScene = const_cast<aiScene*> (scene);
	mutableScene->mRootNode->mTransformation = transform * mutableScene->mRootNode->mTransformation;
	return true;
}

static bool ApplyTransformsToNodesByName (const aiNode* node, const NodeTransformMap& transformByName)
{
	if (node == nullptr) {
		return false;
	}
	bool applied = false;
	aiNode* mutableNode = const_cast<aiNode*> (node);
	auto it = transformByName.find (mutableNode->mName.C_Str ());
	if (it != transformByName.end ()) {
		mutableNode->mTransformation = it->second;
		applied = true;
	}
	for (unsigned int childIndex = 0; childIndex < mutableNode->mNumChildren; ++childIndex) {
		if (ApplyTransformsToNodesByName (mutableNode->mChildren[childIndex], transformByName)) {
			applied = true;
		}
	}
	return applied;
}

static void RenameNodes (aiNode* node, const std::unordered_map<std::string, std::string>& renameMap, std::unordered_set<std::string>& used)
{
	if (node == nullptr) {
		return;
	}
	std::string oldName = node->mName.C_Str ();
	auto it = renameMap.find (oldName);
	if (it != renameMap.end ()) {
		std::string desired = it->second;
		std::string finalName = desired;
		int suffix = 1;
		while (used.find (finalName) != used.end ()) {
			finalName = desired + "_" + std::to_string (suffix++);
		}
		node->mName = aiString (finalName);
	}
	used.insert (node->mName.C_Str ());
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		RenameNodes (node->mChildren[i], renameMap, used);
	}
}

static bool DeleteNodesByName (aiNode* node, const std::unordered_set<std::string>& toDelete)
{
	if (node == nullptr) {
		return false;
	}
	bool removed = false;
	unsigned int write = 0;
	for (unsigned int read = 0; read < node->mNumChildren; ++read) {
		aiNode* child = node->mChildren[read];
		if (child != nullptr && toDelete.find (child->mName.C_Str ()) != toDelete.end ()) {
			removed = true;
			continue;
		}
		node->mChildren[write++] = child;
	}
	node->mNumChildren = write;
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		if (DeleteNodesByName (node->mChildren[i], toDelete)) {
			removed = true;
		}
	}
	return removed;
}

static void SyncMeshNamesFromNodes (const aiNode* node, aiScene* scene)
{
	if (!node) return;
	const std::string name (node->mName.C_Str ());
	const bool isMeaningful = !name.empty () && name != "RootNode" && name != "Scene";
	if (isMeaningful) {
		for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
			unsigned int meshIdx = node->mMeshes[i];
			if (meshIdx < scene->mNumMeshes) {
				scene->mMeshes[meshIdx]->mName = node->mName;
			}
		}
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		SyncMeshNamesFromNodes (node->mChildren[i], scene);
	}
}

static void ApplyMaterialFactors (aiScene* scene, float metallic, float roughness)
{
	if (scene == nullptr) {
		return;
	}
	// 仅在场景只有一个材质时应用 metadata 中的 metallic/roughness
	// 多材质场景中，每个材质应保留自己的值
	if (scene->mNumMaterials != 1) {
		return;
	}
	aiMaterial* mat = scene->mMaterials[0];
	if (mat == nullptr) {
		return;
	}
	mat->AddProperty (&metallic, 1, AI_MATKEY_METALLIC_FACTOR);
	mat->AddProperty (&roughness, 1, AI_MATKEY_ROUGHNESS_FACTOR);
}

static const aiScene* ImportFileListByMainFile (Assimp::Importer& importer, const File& file, unsigned int flags)
{
	try {
		const aiScene* scene = importer.ReadFile (file.path, flags);
		return scene;
	} catch (...) {
		return nullptr;
	}
	return nullptr;
}

static std::string GetFileNameFromFormat (const std::string& format, const std::string& projectName)
{
	std::string fileName = projectName.empty() ? "result" : projectName;
	if (format == "assjson") {
		fileName += ".json";
	} else if (format == "gltf" || format == "gltf2") {
		fileName += ".gltf";
	} else if (format == "glb" || format == "glb2") {
		fileName += ".glb";
	} else if (format == "obj") {
		fileName += ".obj";
	} else if (format == "ply") {
		fileName += ".ply";
	} else if (format == "stl") {
		fileName += ".stl";
	} else if (format == "stlb") {
		fileName += ".stl";
	} else if (format == "fbx") {
		fileName += ".fbx";
	} else if (format == "dae") {
		fileName += ".dae";
	} else if (format == "x") {
		fileName += ".x";
	} else if (format == "x3d") {
		fileName += ".x3d";
	} else if (format == "3mf") {
		fileName += ".3mf";
	} else if (format == "usd") {
		fileName += ".usd";
	} else if (format == "usda") {
		fileName += ".usda";
	} else if (format == "usdc") {
		fileName += ".usdc";
	} else if (format == "usdz") {
		fileName += ".usdz";
	} else if (format == "3ds") {
		fileName += ".3ds";
	} else if (format == "stp" || format == "step") {
		fileName += ".stp";
	} else if (format == "m3d") {
		fileName += ".m3d";
	} else if (format == "ogex") {
		fileName += ".ogex";
	} else if (format == "assbin") {
		fileName += ".assbin";
	} else if (format == "assxml") {
		fileName += ".assxml";
	}
	return fileName;
}

static void ReleaseMeshTextureCoordNames (aiMesh* mesh)
{
	if (mesh == nullptr || mesh->mTextureCoordsNames == nullptr) {
		return;
	}
	for (unsigned int i = 0; i < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++i) {
		delete mesh->mTextureCoordsNames[i];
		mesh->mTextureCoordsNames[i] = nullptr;
	}
	delete[] mesh->mTextureCoordsNames;
	mesh->mTextureCoordsNames = nullptr;
}

static void ReleaseMeshBones (aiMesh* mesh)
{
	if (mesh == nullptr || mesh->mNumBones == 0 || mesh->mBones == nullptr) {
		return;
	}
	std::unordered_set<const aiBone*> bones;
	for (unsigned int i = 0; i < mesh->mNumBones; ++i) {
		if (mesh->mBones[i] != nullptr) {
			bones.insert (mesh->mBones[i]);
		}
	}
	for (const aiBone* bone : bones) {
		delete bone;
	}
	delete[] mesh->mBones;
	mesh->mBones = nullptr;
	mesh->mNumBones = 0;
}

static void ReleaseMeshAnimMeshes (aiMesh* mesh)
{
	if (mesh == nullptr || mesh->mNumAnimMeshes == 0 || mesh->mAnimMeshes == nullptr) {
		return;
	}
	for (unsigned int i = 0; i < mesh->mNumAnimMeshes; ++i) {
		delete mesh->mAnimMeshes[i];
	}
	delete[] mesh->mAnimMeshes;
	mesh->mAnimMeshes = nullptr;
	mesh->mNumAnimMeshes = 0;
}

static bool SimplifyMeshForStl (aiMesh* mesh, float targetRatio, float targetError)
{
	if (mesh == nullptr || mesh->mVertices == nullptr || mesh->mFaces == nullptr) {
		return false;
	}
	if (mesh->mNumVertices < 3 || mesh->mNumFaces < 2) {
		return false;
	}

	std::vector<unsigned int> indices;
	indices.reserve (static_cast<size_t> (mesh->mNumFaces) * 3);
	for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
		const aiFace& face = mesh->mFaces[faceIndex];
		if (face.mNumIndices != 3 || face.mIndices == nullptr) {
			return false;
		}
		for (unsigned int i = 0; i < 3; ++i) {
			if (face.mIndices[i] >= mesh->mNumVertices) {
				return false;
			}
			indices.push_back (face.mIndices[i]);
		}
	}

	if (indices.size () < 6) {
		return false;
	}

	size_t targetIndexCount = static_cast<size_t> (indices.size () * targetRatio);
	targetIndexCount -= targetIndexCount % 3;
	targetIndexCount = std::max<size_t> (3, targetIndexCount);
	if (targetIndexCount >= indices.size ()) {
		return false;
	}

	std::vector<unsigned int> simplifiedIndices (indices.size ());
	size_t simplifiedIndexCount = meshopt_simplify (
		simplifiedIndices.data (),
		indices.data (),
		indices.size (),
		&(mesh->mVertices[0].x),
		mesh->mNumVertices,
		sizeof (aiVector3D),
		targetIndexCount,
		targetError,
		meshopt_SimplifyLockBorder,
		nullptr
	);
	if (simplifiedIndexCount >= indices.size ()) {
		simplifiedIndexCount = meshopt_simplifySloppy (
			simplifiedIndices.data (),
			indices.data (),
			indices.size (),
			&(mesh->mVertices[0].x),
			mesh->mNumVertices,
			sizeof (aiVector3D),
			targetIndexCount,
			targetError,
			nullptr
		);
	}
	if (simplifiedIndexCount < 3 || simplifiedIndexCount >= indices.size ()) {
		return false;
	}

	simplifiedIndices.resize (simplifiedIndexCount);
	std::vector<aiVector3D> simplifiedVertices (mesh->mNumVertices);
	const size_t simplifiedVertexCount = meshopt_optimizeVertexFetch (
		simplifiedVertices.data (),
		simplifiedIndices.data (),
		simplifiedIndices.size (),
		mesh->mVertices,
		mesh->mNumVertices,
		sizeof (aiVector3D)
	);
	if (simplifiedVertexCount < 3) {
		return false;
	}
	simplifiedVertices.resize (simplifiedVertexCount);

	aiVector3D* newVertices = new aiVector3D[simplifiedVertexCount];
	for (size_t i = 0; i < simplifiedVertexCount; ++i) {
		newVertices[i] = simplifiedVertices[i];
	}

	const unsigned int newFaceCount = static_cast<unsigned int> (simplifiedIndices.size () / 3);
	aiFace* newFaces = new aiFace[newFaceCount];
	for (unsigned int faceIndex = 0; faceIndex < newFaceCount; ++faceIndex) {
		aiFace& face = newFaces[faceIndex];
		face.mNumIndices = 3;
		face.mIndices = new unsigned int[3];
		face.mIndices[0] = simplifiedIndices[faceIndex * 3 + 0];
		face.mIndices[1] = simplifiedIndices[faceIndex * 3 + 1];
		face.mIndices[2] = simplifiedIndices[faceIndex * 3 + 2];
	}

	delete[] mesh->mVertices;
	mesh->mVertices = newVertices;
	delete[] mesh->mNormals;
	mesh->mNormals = nullptr;
	delete[] mesh->mTangents;
	mesh->mTangents = nullptr;
	delete[] mesh->mBitangents;
	mesh->mBitangents = nullptr;
	for (unsigned int i = 0; i < AI_MAX_NUMBER_OF_COLOR_SETS; ++i) {
		delete[] mesh->mColors[i];
		mesh->mColors[i] = nullptr;
	}
	for (unsigned int i = 0; i < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++i) {
		delete[] mesh->mTextureCoords[i];
		mesh->mTextureCoords[i] = nullptr;
		mesh->mNumUVComponents[i] = 0;
	}
	ReleaseMeshTextureCoordNames (mesh);
	ReleaseMeshBones (mesh);
	ReleaseMeshAnimMeshes (mesh);
	delete[] mesh->mFaces;
	mesh->mFaces = newFaces;
	mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
	mesh->mNumVertices = static_cast<unsigned int> (simplifiedVertexCount);
	mesh->mNumFaces = newFaceCount;

	aiVector3D minVertex = mesh->mVertices[0];
	aiVector3D maxVertex = mesh->mVertices[0];
	for (unsigned int i = 1; i < mesh->mNumVertices; ++i) {
		const aiVector3D& vertex = mesh->mVertices[i];
		minVertex.x = std::min (minVertex.x, vertex.x);
		minVertex.y = std::min (minVertex.y, vertex.y);
		minVertex.z = std::min (minVertex.z, vertex.z);
		maxVertex.x = std::max (maxVertex.x, vertex.x);
		maxVertex.y = std::max (maxVertex.y, vertex.y);
		maxVertex.z = std::max (maxVertex.z, vertex.z);
	}
	mesh->mAABB.mMin = minVertex;
	mesh->mAABB.mMax = maxVertex;
	return true;
}

static bool SimplifySceneForStl (aiScene* scene)
{
	if (scene == nullptr || scene->mNumMeshes == 0 || scene->mMeshes == nullptr) {
		return false;
	}

	constexpr float kTargetRatio = 0.5f;
	constexpr float kTargetError = 1.0f;
	bool simplified = false;
	for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
		if (SimplifyMeshForStl (scene->mMeshes[i], kTargetRatio, kTargetError)) {
			simplified = true;
		}
	}
	return simplified;
}

struct EmbeddedTextureFile
{
	std::string path;
	Buffer content;
};

static bool TryCreateMatrix4 (const std::vector<float>& matrix16, aiMatrix4x4& matrix);

static aiMatrix4x4 ToMatrix (const std::vector<float>& matrix16)
{
	aiMatrix4x4 m;
	if (!TryCreateMatrix4 (matrix16, m)) {
		m = aiMatrix4x4 ();
	}
	return m;
}

static bool NodeHasMeshes (const aiNode* node)
{
	if (node == nullptr) {
		return false;
	}
	if (node->mNumMeshes > 0) {
		return true;
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		if (NodeHasMeshes (node->mChildren[i])) {
			return true;
		}
	}
	return false;
}

static std::vector<EmbeddedTextureFile> ExtractEmbeddedTextures (aiScene* scene, const std::string& folderPrefix, std::unordered_map<const aiTexture*, std::string>* outNameByTexture = nullptr)
{
	std::vector<EmbeddedTextureFile> files;
	if (scene == nullptr || scene->mNumTextures == 0 || scene->mTextures == nullptr) {
		return files;
	}

	std::string prefix = folderPrefix;
	if (!prefix.empty () && prefix.back () != '/') {
		prefix.push_back ('/');
	}

	std::unordered_map<const aiTexture*, std::string> nameByTexture;
	nameByTexture.reserve (scene->mNumTextures);
	std::unordered_set<std::string> usedNames;
	usedNames.reserve (scene->mNumTextures * 2);

	for (unsigned int i = 0; i < scene->mNumTextures; ++i) {
		const aiTexture* tex = scene->mTextures[i];
		if (tex == nullptr) {
			continue;
		}

		std::string ext;
		if (tex->achFormatHint[0] != '\0') {
			ext = tex->achFormatHint;
			for (char& c : ext) {
				c = static_cast<char> (std::tolower (static_cast<unsigned char>(c)));
			}
		} else {
			ext = (tex->mHeight == 0) ? "bin" : "raw";
		}

		std::string baseName = "assimpjs_tex_" + std::to_string (i);
		std::string fileName = baseName + "." + ext;
		if (usedNames.find (fileName) != usedNames.end ()) {
			fileName = baseName + "_" + std::to_string (usedNames.size ()) + "." + ext;
		}
		fileName = prefix + fileName;
		usedNames.insert (fileName);
		nameByTexture[tex] = fileName;
		if (outNameByTexture != nullptr) {
			(*outNameByTexture)[tex] = fileName;
		}

		size_t size = 0;
		const std::uint8_t* src = nullptr;
		if (tex->mHeight == 0) {
			size = static_cast<size_t> (tex->mWidth);
			src = reinterpret_cast<const std::uint8_t*> (tex->pcData);
		} else {
			constexpr size_t MAX_TEXTURE_SIZE = 1024 * 1024 * 1024;
			size_t width = static_cast<size_t> (tex->mWidth);
			size_t height = static_cast<size_t> (tex->mHeight);
			if (width > MAX_TEXTURE_SIZE / 4 || height > MAX_TEXTURE_SIZE / (width * 4)) {
				continue;
			}
			size = width * height * 4u;
			src = reinterpret_cast<const std::uint8_t*> (tex->pcData);
		}
		if (src == nullptr || size == 0) {
			continue;
		}
		Buffer data (src, src + size);
		files.push_back ({ fileName, std::move (data) });
	}

	if (nameByTexture.empty ()) {
		return files;
	}

	for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
		aiMaterial* mat = scene->mMaterials[i];
		if (mat == nullptr) {
			continue;
		}
		for (unsigned int tt = aiTextureType_DIFFUSE; tt < aiTextureType_UNKNOWN; ++tt) {
			const aiTextureType texType = static_cast<aiTextureType> (tt);
			const unsigned int texCount = mat->GetTextureCount (texType);
			for (unsigned int ti = 0; ti < texCount; ++ti) {
				aiString texPath;
				if (mat->GetTexture (texType, ti, &texPath) != aiReturn_SUCCESS) {
					continue;
				}
				auto embedded = scene->GetEmbeddedTextureAndIndex (texPath.C_Str ());
				if (embedded.first == nullptr) {
					continue;
				}
				auto nameIt = nameByTexture.find (embedded.first);
				if (nameIt == nameByTexture.end ()) {
					continue;
				}
				aiString newPath (nameIt->second);
				mat->AddProperty (&newPath, AI_MATKEY_TEXTURE (texType, ti));
			}
		}
	}

	return files;
}

static aiString MakeTextureName (const std::string& name)
{
	return aiString (name);
}

struct TextureNamingContext
{
	std::string project;
	std::string folderPrefix;
	bool multiPart = false;
	std::unordered_map<unsigned int, std::string> partNameByMaterial; // material index -> part name
	std::unordered_map<const aiTexture*, std::string> embeddedOriginal;
	std::unordered_map<const aiTexture*, std::string> embeddedNew;
};

struct RenamedTextureFile
{
	std::string sourcePath;
	std::string outputPath;
};

static std::string ToPartName (const std::string& raw)
{
	if (raw.empty ()) {
		return "part";
	}
	std::string res = raw;
	for (char& c : res) {
		if (!(std::isalnum (static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
			c = '_';
		}
	}
	return res;
}

static std::string ToLowerAscii (const std::string& value)
{
	std::string result = value;
	for (char& c : result) {
		c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
	}
	return result;
}

static bool EndsWith (const std::string& value, const std::string& suffix)
{
	return value.size () >= suffix.size () && value.compare (value.size () - suffix.size (), suffix.size (), suffix) == 0;
}

static std::string StripTextureRoleSuffix (const std::string& value)
{
	static const std::vector<std::string> suffixes = {
		"_normal_bake", "_base_color", "_basecolor", "_roughness", "_metalness",
		"_metallic", "_occlusion", "_diffuse", "_albedo", "_normal",
		"_orm", "_rma", "_rm", "_ao"
	};
	const std::string lower = ToLowerAscii (value);
	for (const std::string& suffix : suffixes) {
		if (EndsWith (lower, suffix)) {
			return value.substr (0, value.size () - suffix.size ());
		}
	}
	return value;
}

static std::string NormalizeTextureBaseName (const std::string& rawName)
{
	const std::string base = rawName.empty () ? std::string ("result") : rawName;
	std::vector<std::string> parts;
	size_t start = 0;
	while (start <= base.size ()) {
		const size_t sep = base.find ('-', start);
		const std::string part = base.substr (start, sep == std::string::npos ? std::string::npos : sep - start);
		if (!part.empty ()) {
			parts.push_back (StripTextureRoleSuffix (part));
		}
		if (sep == std::string::npos) {
			break;
		}
		start = sep + 1;
	}
	if (parts.size () > 1) {
		bool sameRoot = !parts[0].empty ();
		for (size_t i = 1; i < parts.size (); ++i) {
			if (parts[i] != parts[0]) {
				sameRoot = false;
				break;
			}
		}
		if (sameRoot) {
			return parts[0];
		}
	}
	return StripTextureRoleSuffix (base);
}

static std::string ComposeTexBase (const TextureNamingContext& ctx, const std::string& part)
{
	const std::string projectBase = NormalizeTextureBaseName (ctx.project);
	if (ctx.multiPart && !part.empty ()) {
		return projectBase + "_" + part;
	}
	return projectBase;
}

static std::string ApplyTextureFolderPrefix (const TextureNamingContext& ctx, const std::string& fileName)
{
	if (ctx.folderPrefix.empty ()) {
		return fileName;
	}
	std::string prefix = ctx.folderPrefix;
	if (prefix.back () != '/') {
		prefix.push_back ('/');
	}
	return prefix + fileName;
}

static void RenameMaterialTextures (
	aiScene* scene,
	TextureNamingContext& naming,
	std::vector<EmbeddedTextureFile>& embeddedFiles,
	std::vector<RenamedTextureFile>* renamedExternalFiles = nullptr)
{
	if (scene == nullptr) {
		return;
	}
	for (unsigned int mi = 0; mi < scene->mNumMaterials; ++mi) {
		aiMaterial* mat = scene->mMaterials[mi];
		if (mat == nullptr) {
			continue;
		}
		std::string partName;
		auto it = naming.partNameByMaterial.find (mi);
		if (it != naming.partNameByMaterial.end ()) {
			partName = it->second;
		}
		std::string base = ComposeTexBase (naming, partName);

		auto processType = [&](aiTextureType t, const std::string& suffix) {
			const unsigned int texCount = mat->GetTextureCount (t);
			for (unsigned int ti = 0; ti < texCount; ++ti) {
				aiString texPath;
				if (mat->GetTexture (t, ti, &texPath) != aiReturn_SUCCESS) {
					continue;
				}
				std::string newName = base + suffix;
				// preserve extension if present
				std::string pathStr = texPath.C_Str ();
				size_t dot = pathStr.find_last_of ('.');
				std::string ext = (dot != std::string::npos) ? pathStr.substr (dot) : std::string ();
				// For embedded texture (*N) paths with no extension, derive from achFormatHint
				if (ext.empty ()) {
					const aiTexture* embTex = scene->GetEmbeddedTexture (pathStr.c_str ());
					if (embTex != nullptr && embTex->achFormatHint[0] != '\0') {
						ext = std::string (".") + embTex->achFormatHint;
						for (char& c : ext) {
							if (c != '.') c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
						}
					}
				}
				if (!ext.empty ()) {
					newName += ext;
				}
				std::string outputPath = ApplyTextureFolderPrefix (naming, newName);
				// Find embedded texture by matching path in embeddedOriginal map
				const aiTexture* embPtr = nullptr;
				for (const auto& kv : naming.embeddedOriginal) {
					if (kv.second == pathStr) {
						embPtr = kv.first;
						break;
					}
				}
				// Fallback: direct lookup for *N embedded paths (when embeddedOriginal not populated)
				if (embPtr == nullptr) {
					auto res = scene->GetEmbeddedTextureAndIndex (pathStr.c_str ());
					if (res.first != nullptr) {
						embPtr = res.first;
					}
				}
				if (embPtr != nullptr) {
					naming.embeddedNew[embPtr] = outputPath;
				} else if (renamedExternalFiles != nullptr && pathStr != outputPath) {
					bool found = false;
					for (const RenamedTextureFile& item : *renamedExternalFiles) {
						if (item.sourcePath == pathStr && item.outputPath == outputPath) {
							found = true;
							break;
						}
					}
					if (!found) {
						renamedExternalFiles->push_back ({ pathStr, outputPath });
					}
				}
				aiString newPath = MakeTextureName (outputPath);
				mat->AddProperty (&newPath, AI_MATKEY_TEXTURE (t, ti));
			}
		};

		processType (aiTextureType_BASE_COLOR, "_basecolor");
		processType (aiTextureType_DIFFUSE, "_basecolor");
		processType (aiTextureType_NORMALS, "_normal");
		processType (aiTextureType_NORMAL_CAMERA, "_normal");
		std::string roughnessPath;
		std::string packedPath;
		auto getTexturePath = [&] (aiTextureType t, std::string& outPath) -> bool {
			aiString path;
			if (mat->GetTexture (t, 0, &path) != aiReturn_SUCCESS || path.length == 0) {
				return false;
			}
			outPath = path.C_Str ();
			return !outPath.empty ();
		};
		const bool roughnessIsPacked =
			getTexturePath (aiTextureType_DIFFUSE_ROUGHNESS, roughnessPath) &&
			getTexturePath (aiTextureType_GLTF_METALLIC_ROUGHNESS, packedPath) &&
			roughnessPath == packedPath;
		processType (aiTextureType_DIFFUSE_ROUGHNESS, roughnessIsPacked ? "_rm" : "_roughness");
		processType (aiTextureType_METALNESS, "_metallic");
		processType (aiTextureType_LIGHTMAP, "_ao");
		processType (aiTextureType_AMBIENT_OCCLUSION, "_ao");
		processType (aiTextureType_GLTF_METALLIC_ROUGHNESS, "_rm");
		processType (aiTextureType_UNKNOWN, "_rm"); // catch ORM packed in UNKNOWN
	}

	// rewrite embedded file names to match
	for (auto& file : embeddedFiles) {
		for (auto& kv : naming.embeddedOriginal) {
			const aiTexture* ptr = kv.first;
			const std::string& oldName = kv.second;
			auto newIt = naming.embeddedNew.find (ptr);
			if (newIt != naming.embeddedNew.end () && file.path == oldName) {
				file.path = newIt->second;
			}
		}
	}
}

static const File* FindTextureSourceFile (const FileList& fileList, const std::string& texturePath)
{
	const File* exact = fileList.GetFile (texturePath);
	if (exact != nullptr) {
		return exact;
	}
	const std::string textureName = GetFileName (texturePath);
	if (textureName.empty ()) {
		return nullptr;
	}
	for (size_t i = 0; i < fileList.FileCount (); ++i) {
		const File& file = fileList.GetFile (i);
		if (GetFileName (file.path) == textureName) {
			return &file;
		}
	}
	return nullptr;
}

static void UpdateEmbeddedTextureFilenames (aiScene* scene, const TextureNamingContext& naming)
{
	if (scene == nullptr) {
		return;
	}
	for (const auto& kv : naming.embeddedNew) {
		aiTexture* tex = const_cast<aiTexture*> (kv.first);
		if (tex != nullptr) {
			tex->mFilename = aiString (kv.second);
		}
	}
}

static bool HasMultipleMeshParts (const aiScene* scene)
{
	if (scene == nullptr || scene->mRootNode == nullptr) {
		return false;
	}
	int meshNodeCount = 0;
	std::vector<const aiNode*> stack = { scene->mRootNode };
	while (!stack.empty ()) {
		const aiNode* n = stack.back ();
		stack.pop_back ();
		if (n->mNumMeshes > 0) {
			++meshNodeCount;
			if (meshNodeCount > 1) {
				return true;
			}
		}
		for (unsigned int i = 0; i < n->mNumChildren; ++i) {
			stack.push_back (n->mChildren[i]);
		}
	}
	return false;
}

static bool ReplaceFileListWithZip (const FileList& fileList, const std::string& zipName, Result& result)
{
#ifndef ASSIMP_BUILD_NO_3MF_EXPORTER
	zip_t* zip = zip_stream_open (nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
	if (zip == nullptr) {
		return false;
	}

	bool ok = true;
	for (size_t i = 0; i < fileList.FileCount (); ++i) {
		const File& file = fileList.GetFile (i);
		if (file.path.empty ()) {
			continue;
		}
		if (zip_entry_open (zip, file.path.c_str ()) < 0) {
			ok = false;
			break;
		}
		const void* data = file.content.empty ()
			? static_cast<const void*> ("")
			: static_cast<const void*> (file.content.data ());
		if (zip_entry_write (zip, data, file.content.size ()) < 0) {
			ok = false;
			break;
		}
		zip_entry_close (zip);
	}

	void* buf = nullptr;
	size_t bufsize = 0;
	if (ok) {
		if (zip_stream_copy (zip, &buf, &bufsize) < 0 || buf == nullptr || bufsize == 0) {
			ok = false;
		}
	}
	zip_stream_close (zip);

	if (!ok) {
		std::free (buf);
		return false;
	}

	Buffer zipBuffer (static_cast<std::uint8_t*> (buf), static_cast<std::uint8_t*> (buf) + bufsize);
	std::free (buf);

	result.fileList = FileList ();
	result.fileList.AddFile (zipName, zipBuffer);
	return true;
#else
	(void)fileList;
	(void)zipName;
	(void)result;
	return false;
#endif
}

static std::string SanitizeUsdIdentifier (const std::string& input, size_t fallbackIndex, std::unordered_map<std::string, size_t>& counts)
{
	std::string name = input;
	if (name.empty ()) {
		name = "mesh";
	}
	for (char& c : name) {
		if (!(std::isalnum (static_cast<unsigned char>(c)) || c == '_')) {
			c = '_';
		}
	}
	if (!(std::isalpha (static_cast<unsigned char>(name[0])) || name[0] == '_')) {
		name = "_" + name;
	}
	size_t& count = counts[name];
	std::string unique = name;
	if (count > 0) {
		unique += "_" + std::to_string (count);
	}
	++count;
	if (unique == name && fallbackIndex > 0) {
		unique += "_" + std::to_string (fallbackIndex);
	}
	return unique;
}

static void WriteVec3 (std::ostringstream& ss, const aiVector3D& v)
{
	ss << "(" << v.x << " " << v.y << " " << v.z << ")";
}

static void WriteVec2 (std::ostringstream& ss, const aiVector3D& v)
{
	ss << "(" << v.x << " " << v.y << ")";
}

static void WriteIndent (std::ostringstream& ss, int indent)
{
	for (int i = 0; i < indent; ++i) {
		ss << ' ';
	}
}

static bool IsIdentityMatrix (const aiMatrix4x4& m)
{
	const float eps = 1e-6f;
	return std::fabs (m.a1 - 1.0f) < eps && std::fabs (m.b2 - 1.0f) < eps && std::fabs (m.c3 - 1.0f) < eps && std::fabs (m.d4 - 1.0f) < eps &&
		std::fabs (m.a2) < eps && std::fabs (m.a3) < eps && std::fabs (m.a4) < eps &&
		std::fabs (m.b1) < eps && std::fabs (m.b3) < eps && std::fabs (m.b4) < eps &&
			std::fabs (m.c1) < eps && std::fabs (m.c2) < eps && std::fabs (m.c4) < eps &&
			std::fabs (m.d1) < eps && std::fabs (m.d2) < eps && std::fabs (m.d3) < eps;
}

struct ObjMeshInstance
{
	const aiMesh* mesh;
	aiMatrix4x4 transform;
	std::string name;
};

static void CollectObjMeshInstances (const aiScene* scene, const aiNode* node, const aiMatrix4x4& parent, std::vector<ObjMeshInstance>& out)
{
	if (scene == nullptr || node == nullptr) {
		return;
	}
	aiMatrix4x4 current = parent * node->mTransformation;
	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		unsigned int meshIndex = node->mMeshes[i];
		if (meshIndex >= scene->mNumMeshes) {
			continue;
		}
		const aiMesh* mesh = scene->mMeshes[meshIndex];
		std::string name = node->mName.C_Str ();
		if (name.empty () && mesh != nullptr) {
			name = mesh->mName.C_Str ();
		}
		out.push_back ({ mesh, current, name });
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		CollectObjMeshInstances (scene, node->mChildren[i], current, out);
	}
}

struct ObjVec3Key
{
	uint32_t x;
	uint32_t y;
	uint32_t z;

	bool operator== (const ObjVec3Key& o) const
	{
		return x == o.x && y == o.y && z == o.z;
	}
};

struct ObjVec2Key
{
	uint32_t x;
	uint32_t y;

	bool operator== (const ObjVec2Key& o) const
	{
		return x == o.x && y == o.y;
	}
};

struct ObjKeyHash
{
	template <typename T>
	std::size_t operator() (const T& k) const
	{
		std::size_t h = static_cast<std::size_t> (2166136261u);
		const uint32_t* words = reinterpret_cast<const uint32_t*> (&k);
		for (size_t i = 0; i < sizeof (T) / sizeof (uint32_t); ++i) {
			h ^= static_cast<std::size_t> (words[i]);
			h *= static_cast<std::size_t> (16777619u);
		}
		return h;
	}
};

static uint32_t FloatBits (float value)
{
	uint32_t bits = 0;
	std::memcpy (&bits, &value, sizeof (bits));
	return bits;
}

static ObjVec3Key MakeObjVec3Key (const aiVector3D& value)
{
	return { FloatBits (value.x), FloatBits (value.y), FloatBits (value.z) };
}

static ObjVec2Key MakeObjVec2Key (const aiVector3D& value)
{
	return { FloatBits (value.x), FloatBits (value.y) };
}

static std::string GetObjMaterialName (const aiMaterial* mat, unsigned int materialIndex, std::unordered_map<std::string, size_t>& counts)
{
	aiString aiName;
	std::string raw = "mat_" + std::to_string (materialIndex);
	if (mat != nullptr && mat->Get (AI_MATKEY_NAME, aiName) == AI_SUCCESS && aiName.length > 0) {
		raw = aiName.C_Str ();
	}
	return SanitizeUsdIdentifier (raw, materialIndex, counts);
}

static bool TryGetMaterialTexturePath (const aiMaterial* mat, aiTextureType type, std::string& outPath)
{
	if (mat == nullptr) {
		return false;
	}
	aiString path;
	if (mat->GetTexture (type, 0, &path) != AI_SUCCESS || path.length == 0) {
		return false;
	}
	outPath = path.C_Str ();
	return !outPath.empty ();
}

static float RoughnessToLegacyShininess (float roughness)
{
	const float clamped = std::max (0.0f, std::min (1.0f, roughness));
	const float gloss = 1.0f - clamped;
	return gloss * gloss * 1000.0f;
}

static bool TryCopyTextureUvTransform (aiMaterial* mat, aiTextureType sourceType, aiTextureType targetType)
{
	if (mat == nullptr) {
		return false;
	}
	aiUVTransform trafo;
	unsigned int max = sizeof (aiUVTransform);
	if (aiGetMaterialFloatArray (mat, AI_MATKEY_UVTRANSFORM (sourceType, 0), reinterpret_cast<ai_real*> (&trafo), &max) != aiReturn_SUCCESS) {
		return false;
	}
	mat->AddProperty (&trafo, 1, AI_MATKEY_UVTRANSFORM (targetType, 0));
	return true;
}

static bool TryCopyTextureUvSource (aiMaterial* mat, aiTextureType sourceType, aiTextureType targetType)
{
	if (mat == nullptr) {
		return false;
	}
	int uvIndex = 0;
	if (mat->Get (AI_MATKEY_UVWSRC (sourceType, 0), uvIndex) != aiReturn_SUCCESS) {
		return false;
	}
	mat->AddProperty (&uvIndex, 1, AI_MATKEY_UVWSRC (targetType, 0));
	return true;
}

static void CopyTextureUvMetadata (aiMaterial* mat, aiTextureType sourceType, aiTextureType targetType)
{
	TryCopyTextureUvTransform (mat, sourceType, targetType);
	TryCopyTextureUvSource (mat, sourceType, targetType);
}

static bool PathLooksLikeRoughness (const std::string& path)
{
	const std::string lower = ToLowerAscii (path);
	return lower.find ("rough") != std::string::npos && lower.find ("gloss") == std::string::npos;
}

static bool PathLooksLikeOcclusion (const std::string& path)
{
	const std::string lower = ToLowerAscii (path);
	return lower.find ("ao") != std::string::npos || lower.find ("occlusion") != std::string::npos;
}

struct TextureSlotRef
{
	aiTextureType type = aiTextureType_NONE;
	std::string path;
};

static bool TryFindTextureSlot (const aiMaterial* mat, aiTextureType type, TextureSlotRef& out)
{
	std::string path;
	if (!TryGetMaterialTexturePath (mat, type, path)) {
		return false;
	}
	out.type = type;
	out.path = path;
	return true;
}

static bool TryFindRoughnessTextureSlot (const aiMaterial* mat, TextureSlotRef& out)
{
	if (TryFindTextureSlot (mat, aiTextureType_DIFFUSE_ROUGHNESS, out)) {
		return true;
	}
	TextureSlotRef shininess;
	if (TryFindTextureSlot (mat, aiTextureType_SHININESS, shininess) && PathLooksLikeRoughness (shininess.path)) {
		out = shininess;
		return true;
	}
	TextureSlotRef unknown;
	if (TryFindTextureSlot (mat, aiTextureType_UNKNOWN, unknown) && PathLooksLikeRoughness (unknown.path)) {
		out = unknown;
		return true;
	}
	return false;
}

static bool TryFindMetallicTextureSlot (const aiMaterial* mat, TextureSlotRef& out)
{
	if (TryFindTextureSlot (mat, aiTextureType_METALNESS, out)) {
		return true;
	}
	TextureSlotRef unknown;
	if (TryFindTextureSlot (mat, aiTextureType_UNKNOWN, unknown)) {
		const std::string lower = ToLowerAscii (unknown.path);
		if (lower.find ("metal") != std::string::npos) {
			out = unknown;
			return true;
		}
	}
	return false;
}

static bool TryFindOcclusionTextureSlot (const aiMaterial* mat, TextureSlotRef& out)
{
	if (TryFindTextureSlot (mat, aiTextureType_LIGHTMAP, out)) {
		return true;
	}
	if (TryFindTextureSlot (mat, aiTextureType_AMBIENT_OCCLUSION, out)) {
		return true;
	}
	TextureSlotRef ambient;
	if (TryFindTextureSlot (mat, aiTextureType_AMBIENT, ambient) && PathLooksLikeOcclusion (ambient.path)) {
		out = ambient;
		return true;
	}
	TextureSlotRef unknown;
	if (TryFindTextureSlot (mat, aiTextureType_UNKNOWN, unknown) && PathLooksLikeOcclusion (unknown.path)) {
		out = unknown;
		return true;
	}
	return false;
}

struct RgbaImage
{
	int width = 0;
	int height = 0;
	std::vector<std::uint8_t> pixels;

	bool IsValid () const
	{
		return width > 0 && height > 0 && pixels.size () == static_cast<size_t> (width) * static_cast<size_t> (height) * 4u;
	}
};

static bool DecodeImageBytes (const std::uint8_t* data, size_t size, RgbaImage& out)
{
	if (data == nullptr || size == 0 || size > static_cast<size_t> (std::numeric_limits<int>::max ())) {
		return false;
	}
	int width = 0;
	int height = 0;
	int channels = 0;
	stbi_uc* decoded = stbi_load_from_memory (data, static_cast<int> (size), &width, &height, &channels, 4);
	if (decoded == nullptr || width <= 0 || height <= 0) {
		if (decoded != nullptr) {
			stbi_image_free (decoded);
		}
		return false;
	}
	const size_t byteCount = static_cast<size_t> (width) * static_cast<size_t> (height) * 4u;
	out.width = width;
	out.height = height;
	out.pixels.assign (decoded, decoded + byteCount);
	stbi_image_free (decoded);
	return out.IsValid ();
}

static bool DecodeAiTexture (const aiTexture* tex, RgbaImage& out)
{
	if (tex == nullptr || tex->pcData == nullptr) {
		return false;
	}
	if (tex->mHeight == 0) {
		const std::uint8_t* data = reinterpret_cast<const std::uint8_t*> (tex->pcData);
		return DecodeImageBytes (data, static_cast<size_t> (tex->mWidth), out);
	}
	const size_t texelCount = static_cast<size_t> (tex->mWidth) * static_cast<size_t> (tex->mHeight);
	if (tex->mWidth == 0 || tex->mHeight == 0 || texelCount > static_cast<size_t> (std::numeric_limits<int>::max ())) {
		return false;
	}
	out.width = static_cast<int> (tex->mWidth);
	out.height = static_cast<int> (tex->mHeight);
	out.pixels.resize (texelCount * 4u);
	for (size_t i = 0; i < texelCount; ++i) {
		const aiTexel& src = tex->pcData[i];
		out.pixels[i * 4u + 0u] = src.r;
		out.pixels[i * 4u + 1u] = src.g;
		out.pixels[i * 4u + 2u] = src.b;
		out.pixels[i * 4u + 3u] = src.a;
	}
	return out.IsValid ();
}

static bool LoadMaterialTextureImage (const aiScene* scene, const FileList* sourceFiles, const std::string& path, RgbaImage& out)
{
	if (path.empty ()) {
		return false;
	}
	if (scene != nullptr) {
		const aiTexture* embedded = scene->GetEmbeddedTexture (path.c_str ());
		if (embedded != nullptr && DecodeAiTexture (embedded, out)) {
			return true;
		}
	}
	if (sourceFiles != nullptr) {
		const File* file = FindTextureSourceFile (*sourceFiles, path);
		if (file != nullptr && !file->content.empty ()) {
			return DecodeImageBytes (file->content.data (), file->content.size (), out);
		}
	}
	return false;
}

static std::uint8_t ClampToByte (float value)
{
	const float clamped = std::max (0.0f, std::min (1.0f, value));
	return static_cast<std::uint8_t> (std::round (clamped * 255.0f));
}

static std::uint8_t SampleImageLuma (const RgbaImage& image, int x, int y, int targetWidth, int targetHeight)
{
	if (!image.IsValid () || targetWidth <= 0 || targetHeight <= 0) {
		return 255;
	}
	const int sx = std::min (image.width - 1, std::max (0, (x * image.width) / targetWidth));
	const int sy = std::min (image.height - 1, std::max (0, (y * image.height) / targetHeight));
	const size_t index = (static_cast<size_t> (sy) * static_cast<size_t> (image.width) + static_cast<size_t> (sx)) * 4u;
	const float r = static_cast<float> (image.pixels[index + 0u]);
	const float g = static_cast<float> (image.pixels[index + 1u]);
	const float b = static_cast<float> (image.pixels[index + 2u]);
	return static_cast<std::uint8_t> (std::round (r * 0.2126f + g * 0.7152f + b * 0.0722f));
}

static bool EncodePngRgba (const std::vector<std::uint8_t>& pixels, int width, int height, std::vector<std::uint8_t>& out)
{
	if (width <= 0 || height <= 0 || pixels.size () != static_cast<size_t> (width) * static_cast<size_t> (height) * 4u) {
		return false;
	}
	size_t pngSize = 0;
	void* png = tdefl_write_image_to_png_file_in_memory (pixels.data (), width, height, 4, &pngSize);
	if (png == nullptr || pngSize == 0) {
		return false;
	}
	const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*> (png);
	out.assign (bytes, bytes + pngSize);
	mz_free (png);
	return !out.empty ();
}

static std::string AddEmbeddedPngTexture (aiScene* scene, const std::string& name, const std::vector<std::uint8_t>& pngBytes)
{
	if (scene == nullptr || pngBytes.empty () || pngBytes.size () > static_cast<size_t> (std::numeric_limits<unsigned int>::max ())) {
		return std::string ();
	}
	auto* tex = new aiTexture ();
	tex->mWidth = static_cast<unsigned int> (pngBytes.size ());
	tex->mHeight = 0;
	std::strncpy (tex->achFormatHint, "png", sizeof (tex->achFormatHint) - 1u);
	tex->mFilename = aiString (name);
	const size_t texelCount = (pngBytes.size () + sizeof (aiTexel) - 1u) / sizeof (aiTexel);
	tex->pcData = new aiTexel[texelCount];
	std::memset (tex->pcData, 0, texelCount * sizeof (aiTexel));
	std::memcpy (tex->pcData, pngBytes.data (), pngBytes.size ());

	const unsigned int newIndex = scene->mNumTextures;
	auto** newTextures = new aiTexture*[static_cast<size_t> (newIndex) + 1u];
	for (unsigned int i = 0; i < scene->mNumTextures; ++i) {
		newTextures[i] = scene->mTextures[i];
	}
	newTextures[newIndex] = tex;
	delete[] scene->mTextures;
	scene->mTextures = newTextures;
	scene->mNumTextures = newIndex + 1u;
	return "*" + std::to_string (newIndex);
}

static void SetTexturePath (aiMaterial* mat, aiTextureType type, const std::string& path)
{
	if (mat == nullptr || path.empty ()) {
		return;
	}
	aiString aiPath (path);
	mat->AddProperty (&aiPath, AI_MATKEY_TEXTURE (type, 0));
}

static void RemoveTextureSlot (aiMaterial* mat, aiTextureType type)
{
	if (mat == nullptr) {
		return;
	}
	mat->RemoveProperty (AI_MATKEY_TEXTURE (type, 0));
	mat->RemoveProperty (AI_MATKEY_UVTRANSFORM (type, 0));
	mat->RemoveProperty (AI_MATKEY_UVWSRC (type, 0));
}

static void PrepareStandardPbrTextureSlots (aiScene* scene)
{
	if (scene == nullptr || scene->mMaterials == nullptr) {
		return;
	}
	for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
		aiMaterial* mat = scene->mMaterials[i];
		if (mat == nullptr) {
			continue;
		}

		TextureSlotRef roughness;
		if (TryFindRoughnessTextureSlot (mat, roughness) && roughness.type != aiTextureType_DIFFUSE_ROUGHNESS) {
			SetTexturePath (mat, aiTextureType_DIFFUSE_ROUGHNESS, roughness.path);
			CopyTextureUvMetadata (mat, roughness.type, aiTextureType_DIFFUSE_ROUGHNESS);
			if (roughness.type == aiTextureType_SHININESS && PathLooksLikeRoughness (roughness.path)) {
				RemoveTextureSlot (mat, aiTextureType_SHININESS);
			}
		}

		TextureSlotRef metallic;
		if (TryFindMetallicTextureSlot (mat, metallic) && metallic.type != aiTextureType_METALNESS) {
			SetTexturePath (mat, aiTextureType_METALNESS, metallic.path);
			CopyTextureUvMetadata (mat, metallic.type, aiTextureType_METALNESS);
		}

		TextureSlotRef occlusion;
		if (TryFindOcclusionTextureSlot (mat, occlusion) && occlusion.type != aiTextureType_LIGHTMAP) {
			SetTexturePath (mat, aiTextureType_LIGHTMAP, occlusion.path);
			CopyTextureUvMetadata (mat, occlusion.type, aiTextureType_LIGHTMAP);
		}
	}
}

static void PrepareGltfPackedPbrTextures (aiScene* scene, const FileList* sourceFiles, const std::string& projectName)
{
	if (scene == nullptr || scene->mMaterials == nullptr) {
		return;
	}

	for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
		aiMaterial* mat = scene->mMaterials[i];
		if (mat == nullptr) {
			continue;
		}

		TextureSlotRef existingPacked;
		const bool hasExistingPacked = TryFindTextureSlot (mat, aiTextureType_GLTF_METALLIC_ROUGHNESS, existingPacked);
		TextureSlotRef roughness;
		TextureSlotRef metallic;
		TextureSlotRef occlusion;
		const bool hasRoughness = TryFindRoughnessTextureSlot (mat, roughness);
		const bool hasMetallic = TryFindMetallicTextureSlot (mat, metallic);
		const bool hasOcclusion = TryFindOcclusionTextureSlot (mat, occlusion);

		if (hasExistingPacked && (!hasRoughness || roughness.path == existingPacked.path) && (!hasMetallic || metallic.path == existingPacked.path)) {
			if (hasOcclusion && occlusion.type == aiTextureType_AMBIENT_OCCLUSION) {
				SetTexturePath (mat, aiTextureType_LIGHTMAP, occlusion.path);
				CopyTextureUvMetadata (mat, occlusion.type, aiTextureType_LIGHTMAP);
			}
			continue;
		}
		if (hasRoughness && hasMetallic && roughness.path == metallic.path) {
			if (roughness.type != aiTextureType_DIFFUSE_ROUGHNESS) {
				SetTexturePath (mat, aiTextureType_DIFFUSE_ROUGHNESS, roughness.path);
				CopyTextureUvMetadata (mat, roughness.type, aiTextureType_DIFFUSE_ROUGHNESS);
			}
			continue;
		}
		if (!hasRoughness && !hasMetallic) {
			if (hasOcclusion && occlusion.type == aiTextureType_AMBIENT_OCCLUSION) {
				SetTexturePath (mat, aiTextureType_LIGHTMAP, occlusion.path);
				CopyTextureUvMetadata (mat, occlusion.type, aiTextureType_LIGHTMAP);
			}
			continue;
		}

		RgbaImage roughnessImage;
		RgbaImage metallicImage;
		RgbaImage occlusionImage;
		const bool loadedRoughness = hasRoughness && LoadMaterialTextureImage (scene, sourceFiles, roughness.path, roughnessImage);
		const bool loadedMetallic = hasMetallic && LoadMaterialTextureImage (scene, sourceFiles, metallic.path, metallicImage);
		const bool loadedOcclusion = hasOcclusion && LoadMaterialTextureImage (scene, sourceFiles, occlusion.path, occlusionImage);
		if (!loadedRoughness && !loadedMetallic) {
			continue;
		}

		int width = loadedRoughness ? roughnessImage.width : metallicImage.width;
		int height = loadedRoughness ? roughnessImage.height : metallicImage.height;
		if (width <= 0 || height <= 0) {
			continue;
		}

		float roughnessFactor = 1.0f;
		if (mat->Get (AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) != AI_SUCCESS) {
			roughnessFactor = 1.0f;
		}
		float metallicFactor = 0.0f;
		if (mat->Get (AI_MATKEY_METALLIC_FACTOR, metallicFactor) != AI_SUCCESS) {
			metallicFactor = 0.0f;
		}
		const std::uint8_t defaultRoughness = ClampToByte (roughnessFactor);
		const std::uint8_t defaultMetallic = ClampToByte (metallicFactor);

		std::vector<std::uint8_t> packed (static_cast<size_t> (width) * static_cast<size_t> (height) * 4u, 255);
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const size_t index = (static_cast<size_t> (y) * static_cast<size_t> (width) + static_cast<size_t> (x)) * 4u;
				packed[index + 0u] = loadedOcclusion ? SampleImageLuma (occlusionImage, x, y, width, height) : 255;
				packed[index + 1u] = loadedRoughness ? SampleImageLuma (roughnessImage, x, y, width, height) : defaultRoughness;
				packed[index + 2u] = loadedMetallic ? SampleImageLuma (metallicImage, x, y, width, height) : defaultMetallic;
				packed[index + 3u] = 255;
			}
		}

		std::vector<std::uint8_t> pngBytes;
		if (!EncodePngRgba (packed, width, height, pngBytes)) {
			continue;
		}
		const std::string base = NormalizeTextureBaseName (projectName.empty () ? "result" : projectName);
		const std::string embeddedPath = AddEmbeddedPngTexture (scene, base + "_orm_" + std::to_string (i) + ".png", pngBytes);
		if (embeddedPath.empty ()) {
			continue;
		}

		SetTexturePath (mat, aiTextureType_DIFFUSE_ROUGHNESS, embeddedPath);
		SetTexturePath (mat, aiTextureType_GLTF_METALLIC_ROUGHNESS, embeddedPath);
		CopyTextureUvMetadata (mat, loadedRoughness ? roughness.type : metallic.type, aiTextureType_DIFFUSE_ROUGHNESS);

		if (loadedOcclusion) {
			SetTexturePath (mat, aiTextureType_LIGHTMAP, embeddedPath);
			CopyTextureUvMetadata (mat, occlusion.type, aiTextureType_LIGHTMAP);
		}

		const float one = 1.0f;
		mat->AddProperty (&one, 1, AI_MATKEY_METALLIC_FACTOR);
		mat->AddProperty (&one, 1, AI_MATKEY_ROUGHNESS_FACTOR);
	}
}

static void PrepareFbxLegacyPbrFallbacks (aiScene* scene)
{
	if (scene == nullptr || scene->mMaterials == nullptr) {
		return;
	}

	for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
		aiMaterial* mat = scene->mMaterials[i];
		if (mat == nullptr) {
			continue;
		}

		std::string metallicPath;
		aiTextureType metallicSourceType = aiTextureType_NONE;
		if (TryGetMaterialTexturePath (mat, aiTextureType_GLTF_METALLIC_ROUGHNESS, metallicPath)) {
			metallicSourceType = aiTextureType_GLTF_METALLIC_ROUGHNESS;
		} else if (TryGetMaterialTexturePath (mat, aiTextureType_METALNESS, metallicPath)) {
			metallicSourceType = aiTextureType_METALNESS;
		} else if (TryGetMaterialTexturePath (mat, aiTextureType_UNKNOWN, metallicPath)) {
			metallicSourceType = aiTextureType_UNKNOWN;
		}
		if (!metallicPath.empty () && mat->GetTextureCount (aiTextureType_SPECULAR) == 0) {
			aiString path (metallicPath);
			mat->AddProperty (&path, AI_MATKEY_TEXTURE (aiTextureType_SPECULAR, 0));
			TryCopyTextureUvTransform (mat, metallicSourceType, aiTextureType_SPECULAR);
		}

		std::string roughnessPath;
		aiTextureType roughnessSourceType = aiTextureType_NONE;
		if (TryGetMaterialTexturePath (mat, aiTextureType_GLTF_METALLIC_ROUGHNESS, roughnessPath)) {
			roughnessSourceType = aiTextureType_GLTF_METALLIC_ROUGHNESS;
		} else if (TryGetMaterialTexturePath (mat, aiTextureType_DIFFUSE_ROUGHNESS, roughnessPath)) {
			roughnessSourceType = aiTextureType_DIFFUSE_ROUGHNESS;
		} else if (TryGetMaterialTexturePath (mat, aiTextureType_UNKNOWN, roughnessPath)) {
			roughnessSourceType = aiTextureType_UNKNOWN;
		}
		if (!roughnessPath.empty () && mat->GetTextureCount (aiTextureType_SHININESS) == 0) {
			aiString path (roughnessPath);
			mat->AddProperty (&path, AI_MATKEY_TEXTURE (aiTextureType_SHININESS, 0));
			TryCopyTextureUvTransform (mat, roughnessSourceType, aiTextureType_SHININESS);
		}

		if (mat->GetTextureCount (aiTextureType_LIGHTMAP) > 0 && mat->GetTextureCount (aiTextureType_AMBIENT) == 0) {
			std::string aoPath;
			if (TryGetMaterialTexturePath (mat, aiTextureType_LIGHTMAP, aoPath)) {
				aiString path (aoPath);
				mat->AddProperty (&path, AI_MATKEY_TEXTURE (aiTextureType_AMBIENT, 0));
				TryCopyTextureUvTransform (mat, aiTextureType_LIGHTMAP, aiTextureType_AMBIENT);
			}
		}

		ai_real metallicFactor = 0.0f;
		if (mat->Get (AI_MATKEY_METALLIC_FACTOR, metallicFactor) == AI_SUCCESS) {
			aiColor3D specularColor (metallicFactor, metallicFactor, metallicFactor);
			mat->AddProperty (&specularColor, 1, AI_MATKEY_COLOR_SPECULAR);
			mat->AddProperty (&metallicFactor, 1, AI_MATKEY_REFLECTIVITY);
		}

		ai_real roughnessFactor = 1.0f;
		if (mat->Get (AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == AI_SUCCESS) {
			const float shininess = RoughnessToLegacyShininess (roughnessFactor);
			mat->AddProperty (&shininess, 1, AI_MATKEY_SHININESS);
		}
	}
}

static bool ExportSceneObjCustom (const aiScene* scene, Result& result, const std::string& projectName, const MetadataOptions* metadata = nullptr)
{
	if (scene == nullptr || scene->mRootNode == nullptr) {
		result.errorCode = ErrorCode::ExportError;
		return false;
	}

	std::vector<ObjMeshInstance> instances;
	CollectObjMeshInstances (scene, scene->mRootNode, aiMatrix4x4 (), instances);
	if (instances.empty ()) {
		result.errorCode = ErrorCode::ExportError;
		return false;
	}

	size_t totalVertexCount = 0;
	size_t totalUvVertexCount = 0;
	size_t totalNormalVertexCount = 0;
	for (const ObjMeshInstance& instance : instances) {
		if (instance.mesh == nullptr) {
			continue;
		}
		totalVertexCount += instance.mesh->mNumVertices;
		if (instance.mesh->HasTextureCoords (0)) {
			totalUvVertexCount += instance.mesh->mNumVertices;
		}
		if (instance.mesh->HasNormals ()) {
			totalNormalVertexCount += instance.mesh->mNumVertices;
		}
	}

	std::unordered_map<ObjVec3Key, size_t, ObjKeyHash> positionMap;
	std::unordered_map<ObjVec2Key, size_t, ObjKeyHash> uvMap;
	std::unordered_map<ObjVec3Key, size_t, ObjKeyHash> normalMap;
	std::vector<aiVector3D> positions;
	std::vector<aiVector3D> uvs;
	std::vector<aiVector3D> normals;
	positionMap.reserve (totalVertexCount);
	uvMap.reserve (totalUvVertexCount);
	normalMap.reserve (totalNormalVertexCount);
	positions.reserve (totalVertexCount);
	uvs.reserve (totalUvVertexCount);
	normals.reserve (totalNormalVertexCount);

	std::ostringstream obj;
	std::ostringstream mtl;
	obj << std::fixed << std::setprecision (9);
	mtl << std::fixed << std::setprecision (9);

	const std::string objFileName = GetFileNameFromFormat ("obj", projectName);
	const std::string mtlFileName = projectName.empty () ? "result.mtl" : projectName + ".mtl";
	obj << "mtllib " << mtlFileName << "\n";
	if (metadata != nullptr && !metadata->taskId.empty ()) {
		obj << "# Beijing VAST-" << metadata->taskId << "-AIGC Content\n";
	}

	std::unordered_map<std::string, size_t> materialNameCounts;
	std::vector<std::string> materialNames (scene->mNumMaterials);
	for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex) {
		const aiMaterial* mat = scene->mMaterials[materialIndex];
		std::string materialName = GetObjMaterialName (mat, materialIndex, materialNameCounts);
		materialNames[materialIndex] = materialName;

		mtl << "newmtl " << materialName << "\n";
		aiColor4D baseColor (0.8f, 0.8f, 0.8f, 1.0f);
		if (mat != nullptr) {
			aiColor4D color;
			if (mat->Get (AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS) {
				baseColor = color;
			} else if (mat->Get (AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
				baseColor = color;
			}
			float opacity = 1.0f;
			if (mat->Get (AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
				baseColor.a = opacity;
			}
		}
		mtl << "Kd " << baseColor.r << " " << baseColor.g << " " << baseColor.b << "\n";
		mtl << "d " << baseColor.a << "\n";
		mtl << "illum 2\n";

		std::string texturePath;
		if (TryGetMaterialTexturePath (mat, aiTextureType_BASE_COLOR, texturePath) ||
		    TryGetMaterialTexturePath (mat, aiTextureType_DIFFUSE, texturePath)) {
			mtl << "map_Kd " << texturePath << "\n";
		}
		mtl << "\n";
	}

	auto appendPosition = [&](const aiVector3D& value) -> size_t {
		ObjVec3Key key = MakeObjVec3Key (value);
		auto it = positionMap.find (key);
		if (it != positionMap.end ()) {
			return it->second;
		}
		positions.push_back (value);
		size_t index = positions.size ();
		positionMap.emplace (key, index);
		obj << "v " << value.x << " " << value.y << " " << value.z << "\n";
		return index;
	};

	auto appendUV = [&](const aiVector3D& value) -> size_t {
		ObjVec2Key key = MakeObjVec2Key (value);
		auto it = uvMap.find (key);
		if (it != uvMap.end ()) {
			return it->second;
		}
		uvs.push_back (value);
		size_t index = uvs.size ();
		uvMap.emplace (key, index);
		obj << "vt " << value.x << " " << value.y << "\n";
		return index;
	};

	auto appendNormal = [&](const aiVector3D& value) -> size_t {
		ObjVec3Key key = MakeObjVec3Key (value);
		auto it = normalMap.find (key);
		if (it != normalMap.end ()) {
			return it->second;
		}
		normals.push_back (value);
		size_t index = normals.size ();
		normalMap.emplace (key, index);
		obj << "vn " << value.x << " " << value.y << " " << value.z << "\n";
		return index;
	};

	size_t unnamedCount = 0;
	for (const ObjMeshInstance& instance : instances) {
		if (instance.mesh == nullptr) {
			continue;
		}
		std::string objectName = instance.name.empty () ? "object_" + std::to_string (++unnamedCount) : ToPartName (instance.name);
		obj << "o " << objectName << "\n";
		obj << "g " << objectName << "\n";
		if (instance.mesh->mMaterialIndex < materialNames.size ()) {
			obj << "usemtl " << materialNames[instance.mesh->mMaterialIndex] << "\n";
		}

		const bool hasTexcoords = instance.mesh->HasTextureCoords (0);
		const bool hasNormals = instance.mesh->HasNormals ();
		aiMatrix3x3 normalMatrix (instance.transform);
		if (hasNormals) {
			normalMatrix.Inverse ();
			normalMatrix.Transpose ();
		}

		std::vector<size_t> positionIndices (instance.mesh->mNumVertices, 0);
		std::vector<size_t> uvIndices;
		std::vector<size_t> normalIndices;
		if (hasTexcoords) {
			uvIndices.resize (instance.mesh->mNumVertices, 0);
		}
		if (hasNormals) {
			normalIndices.resize (instance.mesh->mNumVertices, 0);
		}

		for (unsigned int vertexIndex = 0; vertexIndex < instance.mesh->mNumVertices; ++vertexIndex) {
			aiVector3D transformedPosition = instance.transform * instance.mesh->mVertices[vertexIndex];
			positionIndices[vertexIndex] = appendPosition (transformedPosition);

			if (hasTexcoords) {
				uvIndices[vertexIndex] = appendUV (instance.mesh->mTextureCoords[0][vertexIndex]);
			}

			if (hasNormals) {
				aiVector3D transformedNormal = normalMatrix * instance.mesh->mNormals[vertexIndex];
				transformedNormal.Normalize ();
				normalIndices[vertexIndex] = appendNormal (transformedNormal);
			}
		}

		for (unsigned int faceIndex = 0; faceIndex < instance.mesh->mNumFaces; ++faceIndex) {
			const aiFace& face = instance.mesh->mFaces[faceIndex];
			if (face.mNumIndices == 0) {
				continue;
			}
			obj << "f";
			for (unsigned int indexIndex = 0; indexIndex < face.mNumIndices; ++indexIndex) {
				const unsigned int vertexIndex = face.mIndices[indexIndex];
				const size_t positionIndex = positionIndices[vertexIndex];
				obj << " " << positionIndex;
				if (hasTexcoords && hasNormals) {
					obj << "/" << uvIndices[vertexIndex] << "/" << normalIndices[vertexIndex];
				} else if (hasTexcoords) {
					obj << "/" << uvIndices[vertexIndex];
				} else if (hasNormals) {
					obj << "//" << normalIndices[vertexIndex];
				}
			}
			obj << "\n";
		}
	}

	const std::string objText = obj.str ();
	const std::string mtlText = mtl.str ();
	result.fileList.AddFile (objFileName, Buffer (objText.begin (), objText.end ()));
	result.fileList.AddFile (mtlFileName, Buffer (mtlText.begin (), mtlText.end ()));
	result.errorCode = ErrorCode::NoError;
	return true;
}

static void WriteMatrix4 (std::ostringstream& ss, const aiMatrix4x4& m)
{
	ss << "((" << m.a1 << " " << m.a2 << " " << m.a3 << " " << m.a4 << "), ";
	ss << "(" << m.b1 << " " << m.b2 << " " << m.b3 << " " << m.b4 << "), ";
	ss << "(" << m.c1 << " " << m.c2 << " " << m.c3 << " " << m.c4 << "), ";
	ss << "(" << m.d1 << " " << m.d2 << " " << m.d3 << " " << m.d4 << "))";
}

static void WriteXformOps (std::ostringstream& ss, const aiMatrix4x4& transform, int indent)
{
	if (IsIdentityMatrix (transform)) {
		return;
	}
	WriteIndent (ss, indent);
	ss << "matrix4d xformOp:transform = ";
	WriteMatrix4 (ss, transform);
	ss << "\n";
	WriteIndent (ss, indent);
	ss << "uniform token[] xformOpOrder = [\"xformOp:transform\"]\n";
}

struct UsdSharedPositionMesh
{
	std::vector<aiVector3D> points;
	std::vector<uint32_t> faceVertexCounts;
	std::vector<uint32_t> faceVertexIndices;
	std::vector<aiVector3D> faceVaryingNormals;
	std::vector<aiVector3D> faceVaryingTexcoords;
	bool hasNormals = false;
	bool hasTexcoords = false;
};

static bool BuildUsdSharedPositionMesh (const aiMesh* mesh, const aiMatrix4x4& transform, bool applyTransform, UsdSharedPositionMesh& out)
{
	if (mesh == nullptr) {
		return false;
	}

	out.points.clear ();
	out.faceVertexCounts.clear ();
	out.faceVertexIndices.clear ();
	out.faceVaryingNormals.clear ();
	out.faceVaryingTexcoords.clear ();
	out.hasNormals = mesh->HasNormals ();
	out.hasTexcoords = mesh->HasTextureCoords (0);

	std::unordered_map<ObjVec3Key, uint32_t, ObjKeyHash> pointMap;
	pointMap.reserve (mesh->mNumVertices);

	aiMatrix3x3 normalMatrix;
	if (applyTransform && out.hasNormals) {
		normalMatrix = aiMatrix3x3 (transform);
		normalMatrix.Inverse ();
		normalMatrix.Transpose ();
	}

	for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
		const aiFace& face = mesh->mFaces[faceIndex];
		if (face.mNumIndices == 0) {
			continue;
		}
		out.faceVertexCounts.push_back (face.mNumIndices);
		for (unsigned int indexIndex = 0; indexIndex < face.mNumIndices; ++indexIndex) {
			const unsigned int vertexIndex = face.mIndices[indexIndex];
			if (vertexIndex >= mesh->mNumVertices) {
				return false;
			}

			aiVector3D position = mesh->mVertices[vertexIndex];
			if (applyTransform) {
				position = transform * position;
			}

			ObjVec3Key key = MakeObjVec3Key (position);
			auto it = pointMap.find (key);
			uint32_t pointIndex = 0;
			if (it != pointMap.end ()) {
				pointIndex = it->second;
			} else {
				pointIndex = static_cast<uint32_t> (out.points.size ());
				out.points.push_back (position);
				pointMap.emplace (key, pointIndex);
			}
			out.faceVertexIndices.push_back (pointIndex);

			if (out.hasNormals) {
				aiVector3D normal = mesh->mNormals[vertexIndex];
				if (applyTransform) {
					normal = normalMatrix * normal;
					normal.Normalize ();
				}
				out.faceVaryingNormals.push_back (normal);
			}

			if (out.hasTexcoords) {
				out.faceVaryingTexcoords.push_back (mesh->mTextureCoords[0][vertexIndex]);
			}
		}
	}

	return true;
}

static void WriteUsdMesh (std::ostringstream& ss, const aiMesh* mesh, const std::string& meshName, int indent, bool mergeSharedPositions)
{
	if (mesh == nullptr) {
		return;
	}

	UsdSharedPositionMesh sharedMesh;
	if (mergeSharedPositions && !BuildUsdSharedPositionMesh (mesh, aiMatrix4x4 (), false, sharedMesh)) {
		return;
	}

	WriteIndent (ss, indent);
	ss << "def Mesh \"" << meshName << "\" {\n";
	WriteIndent (ss, indent + 2);
	ss << "uniform token subdivisionScheme = \"none\"\n";

	WriteIndent (ss, indent + 2);
	ss << "point3f[] points = [";
	if (mergeSharedPositions) {
		for (size_t v = 0; v < sharedMesh.points.size (); ++v) {
			WriteVec3 (ss, sharedMesh.points[v]);
			if (v + 1 < sharedMesh.points.size ()) {
				ss << ", ";
			}
		}
	} else {
		for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
			WriteVec3 (ss, mesh->mVertices[v]);
			if (v + 1 < mesh->mNumVertices) {
				ss << ", ";
			}
		}
	}
	ss << "]\n";

	WriteIndent (ss, indent + 2);
	ss << "int[] faceVertexCounts = [";
	if (mergeSharedPositions) {
		for (size_t f = 0; f < sharedMesh.faceVertexCounts.size (); ++f) {
			ss << sharedMesh.faceVertexCounts[f];
			if (f + 1 < sharedMesh.faceVertexCounts.size ()) {
				ss << ", ";
			}
		}
	} else {
		for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
			const aiFace& face = mesh->mFaces[f];
			ss << face.mNumIndices;
			if (f + 1 < mesh->mNumFaces) {
				ss << ", ";
			}
		}
	}
	ss << "]\n";

	WriteIndent (ss, indent + 2);
	ss << "int[] faceVertexIndices = [";
	if (mergeSharedPositions) {
		for (size_t i = 0; i < sharedMesh.faceVertexIndices.size (); ++i) {
			ss << sharedMesh.faceVertexIndices[i];
			if (i + 1 < sharedMesh.faceVertexIndices.size ()) {
				ss << ", ";
			}
		}
	} else {
		for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
			const aiFace& face = mesh->mFaces[f];
			for (unsigned int j = 0; j < face.mNumIndices; ++j) {
				ss << face.mIndices[j];
				bool last = (f + 1 == mesh->mNumFaces) && (j + 1 == face.mNumIndices);
				if (!last) {
					ss << ", ";
				}
			}
		}
	}
	ss << "]\n";

	if (mergeSharedPositions ? sharedMesh.hasNormals : mesh->HasNormals ()) {
		WriteIndent (ss, indent + 2);
		ss << "normal3f[] normals = [";
		if (mergeSharedPositions) {
			for (size_t v = 0; v < sharedMesh.faceVaryingNormals.size (); ++v) {
				WriteVec3 (ss, sharedMesh.faceVaryingNormals[v]);
				if (v + 1 < sharedMesh.faceVaryingNormals.size ()) {
					ss << ", ";
				}
			}
		} else {
			for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
				WriteVec3 (ss, mesh->mNormals[v]);
				if (v + 1 < mesh->mNumVertices) {
					ss << ", ";
				}
			}
		}
		ss << "]\n";
		WriteIndent (ss, indent + 2);
		ss << "uniform token normals:interpolation = \"" << (mergeSharedPositions ? "faceVarying" : "vertex") << "\"\n";
	}

	if (mergeSharedPositions ? sharedMesh.hasTexcoords : mesh->HasTextureCoords (0)) {
		WriteIndent (ss, indent + 2);
		ss << "float2[] primvars:st = [";
		if (mergeSharedPositions) {
			for (size_t v = 0; v < sharedMesh.faceVaryingTexcoords.size (); ++v) {
				WriteVec2 (ss, sharedMesh.faceVaryingTexcoords[v]);
				if (v + 1 < sharedMesh.faceVaryingTexcoords.size ()) {
					ss << ", ";
				}
			}
		} else {
			for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
				WriteVec2 (ss, mesh->mTextureCoords[0][v]);
				if (v + 1 < mesh->mNumVertices) {
					ss << ", ";
				}
			}
		}
		ss << "]\n";
		WriteIndent (ss, indent + 2);
		ss << "uniform token primvars:st:interpolation = \"" << (mergeSharedPositions ? "faceVarying" : "vertex") << "\"\n";
	}

	WriteIndent (ss, indent);
	ss << "}\n";
}

static bool NodeHasExportableContent (const aiNode* node)
{
	if (node == nullptr) {
		return false;
	}
	if (node->mNumMeshes > 0) {
		return true;
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		if (NodeHasExportableContent (node->mChildren[i])) {
			return true;
		}
	}
	return false;
}

static void WriteUsdNode (std::ostringstream& ss, const aiScene* scene, const aiNode* node, std::unordered_map<std::string, size_t>& nameCounts, int indent, bool mergeSharedPositions)
{
	if (node == nullptr || scene == nullptr || !NodeHasExportableContent (node)) {
		return;
	}

	std::string rawNodeName = node->mName.C_Str ();
	if (rawNodeName.empty ()) {
		rawNodeName = "node";
	}
	std::string nodeName = SanitizeUsdIdentifier (rawNodeName, 0, nameCounts);
	WriteIndent (ss, indent);
	ss << "def Xform \"" << nodeName << "\" {\n";
	WriteXformOps (ss, node->mTransformation, indent + 2);

	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		unsigned int meshIndex = node->mMeshes[i];
		if (meshIndex < scene->mNumMeshes) {
			const aiMesh* mesh = scene->mMeshes[meshIndex];
			std::string meshName = (mesh != nullptr) ? mesh->mName.C_Str () : "";
			if (meshName.empty ()) {
				meshName = nodeName;
			}
			meshName = SanitizeUsdIdentifier (meshName, meshIndex, nameCounts);
			WriteUsdMesh (ss, mesh, meshName, indent + 2, mergeSharedPositions);
		}
	}

	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		WriteUsdNode (ss, scene, node->mChildren[i], nameCounts, indent + 2, mergeSharedPositions);
	}

	WriteIndent (ss, indent);
	ss << "}\n";
}

static bool ExportSceneUsdFallback (const aiScene* scene, const std::string& format, Result& result, const std::string& projectName, const MetadataOptions* metadata = nullptr, bool mergeSharedPositions = false)
{
	if (scene == nullptr || scene->mRootNode == nullptr) {
		result.errorCode = ErrorCode::ImportError;
		return false;
	}
	if (scene->mNumMeshes == 0) {
		result.errorCode = ErrorCode::ExportError;
		return false;
	}

	std::ostringstream ss;
	ss.setf (std::ios::fixed);
	ss << std::setprecision (8);
	ss << "#usda 1.0\n";
	ss << "(\n";
	if (metadata != nullptr && !metadata->taskId.empty ()) {
		ss << "    documentation = \"Beijing VAST-" << metadata->taskId << "-AIGC Content\"\n";
	}
	ss << "    upAxis = \"Y\"\n";
	ss << ")\n";
	ss << "def Xform \"Scene\" {\n";

	std::unordered_map<std::string, size_t> nameCounts;
	WriteXformOps (ss, scene->mRootNode->mTransformation, 2);
	for (unsigned int i = 0; i < scene->mRootNode->mNumMeshes; ++i) {
		unsigned int meshIndex = scene->mRootNode->mMeshes[i];
		if (meshIndex < scene->mNumMeshes) {
			const aiMesh* mesh = scene->mMeshes[meshIndex];
			std::string meshName = (mesh != nullptr) ? mesh->mName.C_Str () : "";
			if (meshName.empty ()) {
				meshName = "root_mesh";
			}
			meshName = SanitizeUsdIdentifier (meshName, meshIndex, nameCounts);
			WriteUsdMesh (ss, mesh, meshName, 2, mergeSharedPositions);
		}
	}
	for (unsigned int i = 0; i < scene->mRootNode->mNumChildren; ++i) {
		WriteUsdNode (ss, scene, scene->mRootNode->mChildren[i], nameCounts, 2, mergeSharedPositions);
	}

	ss << "}\n";

	std::string contentStr = ss.str ();
	Buffer content (contentStr.begin (), contentStr.end ());
	result.fileList.AddFile (GetFileNameFromFormat (format, projectName), content);
	result.errorCode = ErrorCode::NoError;
	return true;
}

#ifdef ASSIMPJS_ENABLE_TINYUSDZ

struct MeshInstance
{
	const aiMesh* mesh;
	aiMatrix4x4 transform;
	std::string name;
};

// ── USDZ packing helpers ────────────────────────────────────────────────────

static uint32_t CalcCrc32 (const uint8_t* data, size_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	while (len--) {
		crc ^= *data++;
		for (int k = 0; k < 8; ++k)
			crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
	}
	return crc ^ 0xFFFFFFFFu;
}

static void AppendLE16 (std::vector<uint8_t>& v, uint16_t x)
{
	v.push_back (static_cast<uint8_t> (x));
	v.push_back (static_cast<uint8_t> (x >> 8));
}

static void AppendLE32 (std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back (static_cast<uint8_t> (x));
	v.push_back (static_cast<uint8_t> (x >> 8));
	v.push_back (static_cast<uint8_t> (x >> 16));
	v.push_back (static_cast<uint8_t> (x >> 24));
}

struct USDZEntry { std::string name; std::vector<uint8_t> data; };

// Build a USDZ zip (uncompressed, file data 64-byte aligned) from a list of
// entries.  The USDC payload must be the first entry.
static bool CreateUSDZ (const std::vector<USDZEntry>& files, std::vector<uint8_t>& out)
{
	struct CdRec { std::string name; uint32_t crc, size, offset; };
	std::vector<CdRec> cd;
	cd.reserve (files.size ());

	for (const auto& f : files) {
		uint32_t crc = CalcCrc32 (f.data.data (), f.data.size ());
		uint32_t sz  = static_cast<uint32_t> (f.data.size ());
		uint32_t hdr = static_cast<uint32_t> (out.size ());

		// Local file header: 30 fixed bytes + filename + extra (for alignment).
		out.push_back (0x50); out.push_back (0x4B);
		out.push_back (0x03); out.push_back (0x04);
		AppendLE16 (out, 20);  // version needed
		AppendLE16 (out, 0);   // flags
		AppendLE16 (out, 0);   // method: STORE
		AppendLE16 (out, 0);   // mod time
		AppendLE16 (out, 0);   // mod date
		AppendLE32 (out, crc);
		AppendLE32 (out, sz);
		AppendLE32 (out, sz);
		AppendLE16 (out, static_cast<uint16_t> (f.name.size ()));
		size_t xpos = out.size ();
		AppendLE16 (out, 0);   // extra length placeholder
		out.insert (out.end (), f.name.begin (), f.name.end ());

		// Pad extra field so file data starts at a 64-byte boundary.
		size_t pad = (64 - out.size () % 64) % 64;
		out[xpos]     = static_cast<uint8_t> (pad);
		out[xpos + 1] = static_cast<uint8_t> (pad >> 8);
		out.insert (out.end (), pad, 0u);

		out.insert (out.end (), f.data.begin (), f.data.end ());
		cd.push_back ({ f.name, crc, sz, hdr });
	}

	uint32_t cd_off = static_cast<uint32_t> (out.size ());
	for (const auto& e : cd) {
		out.push_back (0x50); out.push_back (0x4B);
		out.push_back (0x01); out.push_back (0x02);
		AppendLE16 (out, 20); AppendLE16 (out, 20);
		AppendLE16 (out, 0);  AppendLE16 (out, 0);
		AppendLE16 (out, 0);  AppendLE16 (out, 0);
		AppendLE32 (out, e.crc);
		AppendLE32 (out, e.size);
		AppendLE32 (out, e.size);
		AppendLE16 (out, static_cast<uint16_t> (e.name.size ()));
		AppendLE16 (out, 0);  // extra len
		AppendLE16 (out, 0);  // comment len
		AppendLE16 (out, 0);  // disk start
		AppendLE16 (out, 0);  // internal attrs
		AppendLE32 (out, 0);  // external attrs
		AppendLE32 (out, e.offset);
		out.insert (out.end (), e.name.begin (), e.name.end ());
	}

	uint32_t cd_sz = static_cast<uint32_t> (out.size ()) - cd_off;
	out.push_back (0x50); out.push_back (0x4B);
	out.push_back (0x05); out.push_back (0x06);
	AppendLE16 (out, 0);
	AppendLE16 (out, 0);
	AppendLE16 (out, static_cast<uint16_t> (cd.size ()));
	AppendLE16 (out, static_cast<uint16_t> (cd.size ()));
	AppendLE32 (out, cd_sz);
	AppendLE32 (out, cd_off);
	AppendLE16 (out, 0);
	return true;
}

// ── USD material / texture helpers ─────────────────────────────────────────

// Returns index into out.textures for the given aiMaterial texture slot,
// creating BufferData / TextureImage / UVTexture entries as needed.
// Returns -1 if the slot is not present or cannot be embedded.
static int32_t AddTexture (
	const aiScene*                           scene,
	const aiMaterial*                        mat,
	aiTextureType                            texType,
	const std::string&                       paramName,
	std::unordered_map<std::string, int32_t>& cache,
	tinyusdz::tydra::RenderScene&            rs,
	const std::string&                       projectName)
{
	aiString texPath;
	if (mat->GetTexture (texType, 0, &texPath) != AI_SUCCESS) {
		return -1;
	}

	const std::string key = texPath.C_Str ();
	auto it = cache.find (key);
	if (it != cache.end ()) {
		return it->second;
	}

	const aiTexture* aitex = scene->GetEmbeddedTexture (texPath.C_Str ());
	if (aitex == nullptr || aitex->mHeight != 0) {
		// Skip external / uncompressed textures.
		cache[key] = -1;
		return -1;
	}

	// Determine file extension from format hint.
	std::string ext = "png";
	if (aitex->achFormatHint[0] != '\0') {
		std::string hint (aitex->achFormatHint);
		if (hint == "jpg" || hint == "jpeg") ext = "jpg";
		else if (hint == "png")              ext = "png";
	}

	// Store raw compressed bytes in a BufferData.
	tinyusdz::tydra::BufferData buf;
	buf.componentType = tinyusdz::tydra::ComponentType::UInt8;
	const uint8_t* raw = reinterpret_cast<const uint8_t*> (aitex->pcData);
	buf.data.assign (raw, raw + aitex->mWidth);

	int64_t buf_id = static_cast<int64_t> (rs.buffers.size ());
	rs.buffers.emplace_back (std::move (buf));

	// TextureImage — asset_identifier is the filename inside the USDZ.
	tinyusdz::tydra::TextureImage img;
	std::string baseName = NormalizeTextureBaseName (projectName.empty() ? "result" : projectName);
	img.asset_identifier = "textures/" + baseName + "_" + paramName + "." + ext;
	img.buffer_id = buf_id;
	const bool isColorTexture = paramName == "baseColor" || paramName == "emissive";
	img.colorSpace = isColorTexture ? tinyusdz::tydra::ColorSpace::sRGB : tinyusdz::tydra::ColorSpace::Raw;
	img.usdColorSpace = img.colorSpace;

	int64_t img_id = static_cast<int64_t> (rs.images.size ());
	rs.images.emplace_back (std::move (img));

	// UVTexture node.
	tinyusdz::tydra::UVTexture uvtex;
	uvtex.prim_name        = "tex_" + paramName + "_" + std::to_string (buf_id);
	uvtex.texture_image_id = img_id;
	uvtex.varname_uv       = "st";
	// Declare output channels for UsdUVTexture
	uvtex.authoredOutputChannels.insert (tinyusdz::tydra::UVTexture::Channel::RGB);
	uvtex.authoredOutputChannels.insert (tinyusdz::tydra::UVTexture::Channel::R);
	uvtex.authoredOutputChannels.insert (tinyusdz::tydra::UVTexture::Channel::G);
	uvtex.authoredOutputChannels.insert (tinyusdz::tydra::UVTexture::Channel::B);
	uvtex.authoredOutputChannels.insert (tinyusdz::tydra::UVTexture::Channel::A);

	int32_t tex_id = static_cast<int32_t> (rs.textures.size ());
	rs.textures.emplace_back (std::move (uvtex));

	cache[key] = tex_id;
	return tex_id;
}

static void CollectMeshInstances (const aiScene* scene, const aiNode* node, const aiMatrix4x4& parent, std::vector<MeshInstance>& out)
{
	aiMatrix4x4 current = parent * node->mTransformation;
	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		unsigned int meshIndex = node->mMeshes[i];
		if (meshIndex < scene->mNumMeshes) {
			const aiMesh* mesh = scene->mMeshes[meshIndex];
			std::string name = node->mName.C_Str ();
			if (name.empty () && mesh != nullptr) {
				name = mesh->mName.C_Str ();
			}
			out.push_back ({mesh, current, name});
		}
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		CollectMeshInstances (scene, node->mChildren[i], current, out);
	}
}

static bool BuildTinyUsdScene (const aiScene* scene, tinyusdz::tydra::RenderScene& out, std::string& err, const std::string& projectName, const MetadataOptions* metadata = nullptr, bool mergeSharedPositions = false)
{
	if (scene == nullptr || scene->mRootNode == nullptr) {
		err = "scene is null";
		return false;
	}

	std::vector<MeshInstance> instances;
	CollectMeshInstances (scene, scene->mRootNode, aiMatrix4x4 (), instances);
	if (instances.empty ()) {
		err = "scene has no meshes";
		return false;
	}

	out.meta.upAxis = "Y";
	if (metadata != nullptr && !metadata->taskId.empty ()) {
		out.meta.comment = "Beijing VAST-" + metadata->taskId + "-AIGC Content";
	}

	// ── Build materials ──────────────────────────────────────────────────────
	std::unordered_map<std::string, int32_t> texCache; // aiTex path → texture_id
	std::unordered_map<std::string, size_t>  matNameCounts;
	out.materials.reserve (scene->mNumMaterials);

	for (unsigned int mi = 0; mi < scene->mNumMaterials; ++mi) {
		const aiMaterial* aimat = scene->mMaterials[mi];

		// Material prim name
		aiString aiName;
		std::string rawMatName;
		if (aimat->Get (AI_MATKEY_NAME, aiName) == AI_SUCCESS && aiName.length > 0) {
			rawMatName = aiName.C_Str ();
		} else {
			rawMatName = "mat_" + std::to_string (mi);
		}
		size_t matFb = mi;
		std::string matPrimName = SanitizeUsdIdentifier (rawMatName, matFb, matNameCounts);

		tinyusdz::tydra::RenderMaterial rmat;
		rmat.name         = matPrimName;
		rmat.display_name = rawMatName;

		tinyusdz::tydra::PreviewSurfaceShader pss;

		// Base colour (PBR metallic-roughness base color, or diffuse fallback)
		{
			aiColor4D col;
			bool gotColor = false;
			if (aimat->Get (AI_MATKEY_BASE_COLOR, col) == AI_SUCCESS) {
				gotColor = true;
			} else if (aimat->Get (AI_MATKEY_COLOR_DIFFUSE, col) == AI_SUCCESS) {
				gotColor = true;
			}
			if (gotColor) {
				pss.diffuseColor.value[0] = col.r;
				pss.diffuseColor.value[1] = col.g;
				pss.diffuseColor.value[2] = col.b;
				// opacity from alpha
				pss.opacity.value = col.a;
			}
		}

		// Base colour texture
		{
			int32_t tid = AddTexture (scene, aimat, aiTextureType_BASE_COLOR,
				"baseColor", texCache, out, projectName);
			if (tid < 0) {
				// fall back to legacy diffuse slot
				tid = AddTexture (scene, aimat, aiTextureType_DIFFUSE,
					"baseColor", texCache, out, projectName);
			}
			if (tid >= 0) {
				pss.diffuseColor.texture_id = tid;
			}
		}

		// Metallic factor
		{
			float val = 0.0f;
			if (aimat->Get (AI_MATKEY_METALLIC_FACTOR, val) == AI_SUCCESS) {
				pss.metallic.value = val;
			}
		}

		// Metallic texture
		{
			int32_t tid = AddTexture (scene, aimat, aiTextureType_METALNESS,
				"metallic", texCache, out, projectName);
			if (tid >= 0) {
				pss.metallic.texture_id = tid;
			}
		}

		// Roughness factor
		{
			float val = 0.5f;
			if (aimat->Get (AI_MATKEY_ROUGHNESS_FACTOR, val) == AI_SUCCESS) {
				pss.roughness.value = val;
			}
		}

		// Roughness texture
		{
			int32_t tid = AddTexture (scene, aimat, aiTextureType_DIFFUSE_ROUGHNESS,
				"roughness", texCache, out, projectName);
			if (tid >= 0) {
				pss.roughness.texture_id = tid;
			}
		}

		// Ambient occlusion texture
		{
			int32_t tid = AddTexture (scene, aimat, aiTextureType_LIGHTMAP,
				"occlusion", texCache, out, projectName);
			if (tid >= 0) {
				pss.occlusion.texture_id = tid;
			}
		}

		// Emissive colour
		{
			aiColor3D em;
			if (aimat->Get (AI_MATKEY_COLOR_EMISSIVE, em) == AI_SUCCESS) {
				pss.emissiveColor.value[0] = em.r;
				pss.emissiveColor.value[1] = em.g;
				pss.emissiveColor.value[2] = em.b;
			}
		}

		// Emissive texture
		{
			int32_t tid = AddTexture (scene, aimat, aiTextureType_EMISSIVE,
				"emissive", texCache, out, projectName);
			if (tid >= 0) {
				pss.emissiveColor.texture_id = tid;
			}
		}

		// Normal map
		{
			int32_t tid = AddTexture (scene, aimat, aiTextureType_NORMALS,
				"normal", texCache, out, projectName);
			if (tid >= 0) {
				pss.normal.texture_id = tid;
			}
		}

		// Opacity
		{
			float op = 1.0f;
			if (aimat->Get (AI_MATKEY_OPACITY, op) == AI_SUCCESS) {
				pss.opacity.value = op;
			}
		}

		rmat.surfaceShader = pss;
		out.materials.emplace_back (std::move (rmat));
	}

	// ── Build meshes ─────────────────────────────────────────────────────────
	out.meshes.reserve (instances.size ());
	std::unordered_map<std::string, size_t> nameCounts;
	size_t fallbackIndex = 0;

	for (const MeshInstance& inst : instances) {
		if (inst.mesh == nullptr) {
			continue;
		}

		tinyusdz::tydra::RenderMesh rmesh;
		std::string rawName = inst.name;
		if (rawName.empty ()) {
			rawName = inst.mesh->mName.C_Str ();
		}
		if (rawName.empty ()) {
			rawName = "mesh";
		}
		rmesh.prim_name = SanitizeUsdIdentifier (rawName, fallbackIndex++, nameCounts);
		rmesh.display_name = rawName;
		rmesh.is_single_indexable = !mergeSharedPositions;

		// Link to material
		if (inst.mesh->mMaterialIndex < scene->mNumMaterials) {
			rmesh.material_id = static_cast<int> (inst.mesh->mMaterialIndex);
		}

		if (mergeSharedPositions) {
			UsdSharedPositionMesh sharedMesh;
			if (!BuildUsdSharedPositionMesh (inst.mesh, inst.transform, true, sharedMesh)) {
				err = "failed to build shared-position usd mesh";
				return false;
			}

			rmesh.points.resize (sharedMesh.points.size ());
			for (size_t v = 0; v < sharedMesh.points.size (); ++v) {
				const aiVector3D& p = sharedMesh.points[v];
				rmesh.points[v][0] = p.x * 100.0f;
				rmesh.points[v][1] = p.y * 100.0f;
				rmesh.points[v][2] = p.z * 100.0f;
			}

			rmesh.usdFaceVertexCounts = sharedMesh.faceVertexCounts;
			rmesh.usdFaceVertexIndices = sharedMesh.faceVertexIndices;

			if (sharedMesh.hasNormals) {
				std::vector<float> normalData;
				normalData.reserve (sharedMesh.faceVaryingNormals.size () * 3);
				for (const aiVector3D& n : sharedMesh.faceVaryingNormals) {
					normalData.push_back (n.x);
					normalData.push_back (n.y);
					normalData.push_back (n.z);
				}
				tinyusdz::tydra::VertexAttribute normals;
				normals.format      = tinyusdz::tydra::VertexAttributeFormat::Vec3;
				normals.variability = tinyusdz::tydra::VertexVariability::FaceVarying;
				normals.elementSize = 1;
				normals.set_buffer (reinterpret_cast<const std::uint8_t*> (normalData.data ()),
					normalData.size () * sizeof (float));
				rmesh.normals = std::move (normals);
			}

			if (sharedMesh.hasTexcoords) {
				std::vector<float> uvData;
				uvData.reserve (sharedMesh.faceVaryingTexcoords.size () * 2);
				for (const aiVector3D& uvValue : sharedMesh.faceVaryingTexcoords) {
					uvData.push_back (uvValue.x);
					uvData.push_back (uvValue.y);
				}
				tinyusdz::tydra::VertexAttribute uv;
				uv.name        = "st";
				uv.format      = tinyusdz::tydra::VertexAttributeFormat::Vec2;
				uv.variability = tinyusdz::tydra::VertexVariability::FaceVarying;
				uv.elementSize = 1;
				uv.set_buffer (reinterpret_cast<const std::uint8_t*> (uvData.data ()),
					uvData.size () * sizeof (float));
				rmesh.texcoords[0] = std::move (uv);
			}
		} else {
			rmesh.points.resize (inst.mesh->mNumVertices);
			for (unsigned int v = 0; v < inst.mesh->mNumVertices; ++v) {
				aiVector3D p = inst.transform * inst.mesh->mVertices[v];
				rmesh.points[v][0] = p.x * 100.0f;
				rmesh.points[v][1] = p.y * 100.0f;
				rmesh.points[v][2] = p.z * 100.0f;
			}

			rmesh.usdFaceVertexCounts.reserve (inst.mesh->mNumFaces);
			for (unsigned int f = 0; f < inst.mesh->mNumFaces; ++f) {
				const aiFace& face = inst.mesh->mFaces[f];
				rmesh.usdFaceVertexCounts.push_back (face.mNumIndices);
				for (unsigned int j = 0; j < face.mNumIndices; ++j) {
					rmesh.usdFaceVertexIndices.push_back (face.mIndices[j]);
				}
			}

			if (inst.mesh->HasNormals ()) {
				aiMatrix3x3 normalMatrix (inst.transform);
				normalMatrix.Inverse ();
				normalMatrix.Transpose ();
				std::vector<float> normalData;
				normalData.reserve (inst.mesh->mNumVertices * 3);
				for (unsigned int v = 0; v < inst.mesh->mNumVertices; ++v) {
					aiVector3D n = normalMatrix * inst.mesh->mNormals[v];
					n.Normalize ();
					normalData.push_back (n.x);
					normalData.push_back (n.y);
					normalData.push_back (n.z);
				}
				tinyusdz::tydra::VertexAttribute normals;
				normals.format      = tinyusdz::tydra::VertexAttributeFormat::Vec3;
				normals.variability = tinyusdz::tydra::VertexVariability::Vertex;
				normals.elementSize = 1;
				normals.set_buffer (reinterpret_cast<const std::uint8_t*> (normalData.data ()),
					normalData.size () * sizeof (float));
				rmesh.normals = std::move (normals);
			}

			if (inst.mesh->HasTextureCoords (0)) {
				std::vector<float> uvData;
				uvData.reserve (inst.mesh->mNumVertices * 2);
				for (unsigned int v = 0; v < inst.mesh->mNumVertices; ++v) {
					uvData.push_back (inst.mesh->mTextureCoords[0][v].x);
					uvData.push_back (inst.mesh->mTextureCoords[0][v].y);
				}
				tinyusdz::tydra::VertexAttribute uv;
				uv.name        = "st";
				uv.format      = tinyusdz::tydra::VertexAttributeFormat::Vec2;
				uv.variability = tinyusdz::tydra::VertexVariability::Vertex;
				uv.elementSize = 1;
				uv.set_buffer (reinterpret_cast<const std::uint8_t*> (uvData.data ()),
					uvData.size () * sizeof (float));
				rmesh.texcoords[0] = std::move (uv);
			}
		}

		out.meshes.emplace_back (std::move (rmesh));
	}

	return true;
}

#endif

static bool ExportSceneUsd (const aiScene* scene, const std::string& format, Result& result, const std::string& projectName, const MetadataOptions* metadata = nullptr, bool mergeSharedPositions = false)
{
#ifdef ASSIMPJS_ENABLE_TINYUSDZ
	aiScene* mutableScene = const_cast<aiScene*> (scene);
	PrepareStandardPbrTextureSlots (mutableScene);
	tinyusdz::tydra::RenderScene renderScene;
	std::string err;
	if (!BuildTinyUsdScene (mutableScene, renderScene, err, projectName, metadata, mergeSharedPositions)) {
		result.errorCode = ErrorCode::ExportError;
		return false;
	}
	std::string warn;
	std::string usdaStr;
	if (!tinyusdz::tydra::export_to_usda (renderScene, usdaStr, &warn, &err)) {
		if (format == "usda") {
			return ExportSceneUsdFallback (scene, format, result, projectName, metadata, mergeSharedPositions);
		}
		result.errorCode = ErrorCode::ExportError;
		return false;
	}

	// usdz: always package as USDZ zip (with or without textures).
	if (format == "usdz") {
		std::vector<USDZEntry> entries;
		std::vector<uint8_t> usdaBytes (usdaStr.begin (), usdaStr.end ());
		std::string baseName = projectName.empty () ? "model" : projectName;
		entries.push_back ({baseName + ".usda", usdaBytes});
		for (size_t i = 0; i < renderScene.images.size (); ++i) {
			const auto& img = renderScene.images[i];
			if (img.buffer_id < 0 ||
				static_cast<size_t> (img.buffer_id) >= renderScene.buffers.size ()) {
				continue;
			}
			const auto& buf = renderScene.buffers[static_cast<size_t> (img.buffer_id)];
			entries.push_back ({img.asset_identifier, buf.data});
		}
		std::vector<uint8_t> usdz;
		if (CreateUSDZ (entries, usdz)) {
			Buffer content (usdz.begin (), usdz.end ());
			result.fileList.AddFile (GetFileNameFromFormat ("usdz", projectName), content);
			result.errorCode = ErrorCode::NoError;
			return true;
		}
		result.errorCode = ErrorCode::ExportError;
		return false;
	}

	if (format == "usd" || format == "usdc") {
		// If there are embedded textures, package USDA + textures as USDZ.
		// (Skip USDC conversion to avoid data loss in LoadUSDAFromMemory roundtrip)
		if (!renderScene.images.empty ()) {
			std::vector<USDZEntry> entries;
			std::vector<uint8_t> usdaBytes (usdaStr.begin (), usdaStr.end ());
			std::string baseName = projectName.empty() ? "model" : projectName;
			entries.push_back ({baseName + ".usda", usdaBytes});
			for (size_t i = 0; i < renderScene.images.size (); ++i) {
				const auto& img = renderScene.images[i];
				if (img.buffer_id < 0 ||
					static_cast<size_t> (img.buffer_id) >= renderScene.buffers.size ()) {
					continue;
				}
				const auto& buf = renderScene.buffers[static_cast<size_t> (img.buffer_id)];
				entries.push_back ({img.asset_identifier, buf.data});
			}
			std::vector<uint8_t> usdz;
			if (CreateUSDZ (entries, usdz)) {
				Buffer content (usdz.begin (), usdz.end ());
				result.fileList.AddFile (baseName + ".usdz", content);
				result.errorCode = ErrorCode::NoError;
				return true;
			}
			// CreateUSDZ failed — fall through to plain USDA output.
			Buffer content (usdaStr.begin (), usdaStr.end ());
			result.fileList.AddFile (GetFileNameFromFormat ("usd", projectName), content);
			result.errorCode = ErrorCode::NoError;
			return true;
		}

		// No textures: try USDC conversion for binary format.
		tinyusdz::Stage stage;
		tinyusdz::USDLoadOptions loadOptions;
		loadOptions.load_assets = false;
		loadOptions.do_composition = false;
		if (!tinyusdz::LoadUSDAFromMemory (
				reinterpret_cast<const std::uint8_t*> (usdaStr.data ()),
				usdaStr.size (),
				std::string (),
				&stage,
				&warn,
				&err,
				loadOptions)) {
			if (format == "usdc") {
				result.errorCode = ErrorCode::ExportError;
				return false;
			}
			Buffer content (usdaStr.begin (), usdaStr.end ());
			result.fileList.AddFile (GetFileNameFromFormat ("usd", projectName), content);
			result.errorCode = ErrorCode::NoError;
			return true;
		}
		std::vector<std::uint8_t> usdc;
		if (!tinyusdz::usdc::SaveAsUSDCToMemory (stage, &usdc, &warn, &err)) {
			if (format == "usdc") {
				result.errorCode = ErrorCode::ExportError;
				return false;
			}
			Buffer content (usdaStr.begin (), usdaStr.end ());
			result.fileList.AddFile (GetFileNameFromFormat ("usd", projectName), content);
			result.errorCode = ErrorCode::NoError;
			return true;
		}

		Buffer content (usdc.begin (), usdc.end ());
		result.fileList.AddFile (GetFileNameFromFormat (format, projectName), content);
		result.errorCode = ErrorCode::NoError;
		return true;
	}

	Buffer content (usdaStr.begin (), usdaStr.end ());
	result.fileList.AddFile (GetFileNameFromFormat (format, projectName), content);
	result.errorCode = ErrorCode::NoError;
	return true;
#else
	return ExportSceneUsdFallback (scene, format, result, projectName, metadata, mergeSharedPositions);
#endif
}

static void CollectMaterialParts (const aiScene* scene, TextureNamingContext& ctx)
{
	if (scene == nullptr || scene->mRootNode == nullptr) {
		return;
	}
	bool multi = HasMultipleMeshParts (scene);
	ctx.multiPart = multi;
	std::unordered_map<unsigned int, std::string> first;
	std::vector<const aiNode*> stack = { scene->mRootNode };
	while (!stack.empty ()) {
		const aiNode* n = stack.back (); stack.pop_back ();
		if (n->mNumMeshes > 0) {
			std::string part = ToPartName (n->mName.C_Str ());
			for (unsigned int i = 0; i < n->mNumMeshes; ++i) {
				unsigned int mi = n->mMeshes[i];
				if (mi < scene->mNumMeshes) {
					const aiMesh* mesh = scene->mMeshes[mi];
					unsigned int matIdx = mesh ? mesh->mMaterialIndex : 0;
					if (first.find (matIdx) == first.end ()) {
						first[matIdx] = part;
					}
				}
			}
		}
		for (unsigned int i = 0; i < n->mNumChildren; ++i) {
			stack.push_back (n->mChildren[i]);
		}
	}
	ctx.partNameByMaterial = std::move (first);
}

static std::string GetExtFromPath (const std::string& path)
{
	auto pos = path.rfind ('.');
	if (pos == std::string::npos) return "";
	std::string ext = path.substr (pos + 1);
	for (auto& c : ext) c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
	return ext;
}

static bool ExportScene (const aiScene* scene, const std::string& format, Result& result, const std::string& projectName, const MetadataOptions* metadata = nullptr, const std::string& inputFormat = "", const FileList* sourceFiles = nullptr)
{
	if (scene == nullptr) {
		result.errorCode = ErrorCode::ImportError;
		return false;
	}

	// Scene graph validation (Checkpoint 3)
	if (scene->mNumMeshes == 0) {
		std::cerr << "[ExportScene] No meshes in scene" << std::endl;
		result.errorCode = ErrorCode::ImportError;
		return false;
	}

	for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
		const aiMesh* mesh = scene->mMeshes[i];
		if (mesh == nullptr) {
			std::cerr << "[ExportScene] Null mesh at index " << i << std::endl;
			result.errorCode = ErrorCode::ImportError;
			return false;
		}
		if (mesh->mNumVertices == 0) {
			std::cerr << "[ExportScene] Empty mesh at index " << i << std::endl;
			result.errorCode = ErrorCode::ImportError;
			return false;
		}
		if (mesh->mNumFaces == 0) {
			std::cerr << "[ExportScene] No faces in mesh " << i << std::endl;
			result.errorCode = ErrorCode::ImportError;
			return false;
		}
	}

	const bool inputIsGltf = (inputFormat == "glb" || inputFormat == "glb2" ||
	                          inputFormat == "gltf" || inputFormat == "gltf2");
	if (format == "usd" || format == "usda" || format == "usdc" || format == "usdz") {
		return ExportSceneUsd (scene, format, result, projectName, metadata, inputIsGltf);
	}

	const bool isGltfOutput =
		format == "gltf" || format == "gltf2" ||
		format == "glb" || format == "glb2";
	const bool isFbxOutput = format == "fbx";
	const bool isObjOutput = format == "obj";
	const bool isStlOutput = format == "stl";
	const bool isThreeMfOutput = format == "3mf";
	aiScene* mutableScene = const_cast<aiScene*> (scene);
	const unsigned int originalFlags = mutableScene->mFlags;
	if (isGltfOutput) {
		// Avoid MakeVerboseFormat on export, which can deindex meshes when
		// JoinIdenticalVertices was already applied at import time.
		mutableScene->mFlags &= ~AI_SCENE_FLAGS_NON_VERBOSE_FORMAT;
	}

	Assimp::Exporter exporter;
	FileListIOSystemWriteAdapter* exportIOSystem = new FileListIOSystemWriteAdapter (result.fileList);
	exporter.SetIOHandler (exportIOSystem);

	Assimp::ExportProperties exportProperties;
	exportProperties.SetPropertyBool ("JSON_SKIP_WHITESPACES", true);
	std::string fileName = GetFileNameFromFormat (format, projectName);
	unsigned int exportPostprocess = 0u;
	
	// Map dae format to collada for Assimp's internal format identifier
	std::string assimpFormat = format;
	if (format == "dae") {
		assimpFormat = "collada";
	} else if (format == "fbx") {
		assimpFormat = "fbx";
	} else if (format == "stl") {
		// Prefer binary STL to reduce memory usage and avoid stream failures.
		assimpFormat = "stlb";
	}
	
	if (isGltfOutput || isObjOutput) {
		PrepareStandardPbrTextureSlots (mutableScene);
	}
	if (isGltfOutput) {
		PrepareGltfPackedPbrTextures (mutableScene, sourceFiles, projectName);
	}

	TextureNamingContext naming;
	naming.project = projectName.empty () ? std::string ("result") : projectName;
	if (isFbxOutput) {
		const size_t dotPos = fileName.find_last_of ('.');
		naming.folderPrefix = fileName.substr (0, dotPos) + ".fbm";
	}
	CollectMaterialParts (scene, naming);
	std::vector<EmbeddedTextureFile> extraFiles;
	std::vector<RenamedTextureFile> renamedExternalFiles;
	if (isFbxOutput) {
		extraFiles = ExtractEmbeddedTextures (mutableScene, naming.folderPrefix, &naming.embeddedOriginal);
	} else if (format == "obj" || format == "gltf" || format == "gltf2") {
		extraFiles = ExtractEmbeddedTextures (mutableScene, "", &naming.embeddedOriginal);
	}
	RenameMaterialTextures (mutableScene, naming, extraFiles, &renamedExternalFiles);
	if (isFbxOutput) {
		PrepareFbxLegacyPbrFallbacks (mutableScene);
	}
	// For GLB: update embedded texture mFilename so GLTF exporter can find them by name
	if (format == "glb" || format == "glb2") {
		UpdateEmbeddedTextureFilenames (mutableScene, naming);
	}

	// 对所有非 GLB 格式同步节点名到 mesh（GLB 本身保留节点名，不需要同步）
	if (format != "glb" && format != "glb2" && format != "gltf" && format != "gltf2") {
		// 从子节点开始遍历，跳过根节点（根节点名通常为 "result"/"Scene" 等无意义值）
		for (unsigned int i = 0; i < scene->mRootNode->mNumChildren; ++i) {
			SyncMeshNamesFromNodes (scene->mRootNode->mChildren[i], mutableScene);
		}
	}

	if (inputIsGltf && isStlOutput) {
		SimplifySceneForStl (mutableScene);
	}

	if (format == "stl") {
		// GLB 是 Y-up，STL 工具通常期望 Z-up，补偿 +90° X 旋转
		aiMatrix4x4 rotX;
		aiMatrix4x4::RotationX (AI_MATH_HALF_PI, rotX);
		mutableScene->mRootNode->mTransformation = rotX * mutableScene->mRootNode->mTransformation;
	}

	if (format == "3mf") {
		constexpr float kThreeMfScale = 500.0f;
		aiMatrix4x4 rotX;
		aiMatrix4x4::RotationX (AI_MATH_HALF_PI, rotX);
		aiMatrix4x4 scale;
		aiMatrix4x4::Scaling (aiVector3D (kThreeMfScale, kThreeMfScale, kThreeMfScale), scale);
		aiMatrix4x4 transform = scale * rotX;

		for (unsigned int i = 0; i < mutableScene->mNumMeshes; ++i) {
			aiMesh* mesh = mutableScene->mMeshes[i];
			if (mesh != nullptr) {
				for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
					mesh->mVertices[v] = transform * mesh->mVertices[v];
				}
				if (mesh->mNormals != nullptr) {
					for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
						mesh->mNormals[v] = rotX * mesh->mNormals[v];
					}
				}
			}
		}
	}

	// Inject custom metadata into scene metadata for exporters that read aiScene metadata
	if (metadata != nullptr && !metadata->taskId.empty ()) {
		std::string generator = "Beijing VAST-" + metadata->taskId + "-AIGC Content";
		if (isGltfOutput) {
			if (mutableScene->mMetaData == nullptr) {
				mutableScene->mMetaData = aiMetadata::Alloc (1);
				mutableScene->mMetaData->Set (0, "custom_generator", aiString (generator));
			} else {
				mutableScene->mMetaData->Add ("custom_generator", aiString (generator));
			}
		}
		if (isFbxOutput || isObjOutput || isStlOutput) {
			if (mutableScene->mMetaData == nullptr) {
				mutableScene->mMetaData = aiMetadata::Alloc (1);
				mutableScene->mMetaData->Set (0, "Creator", aiString (generator));
			} else {
				mutableScene->mMetaData->Add ("Creator", aiString (generator));
			}
		}
		if (isThreeMfOutput) {
			if (mutableScene->mMetaData == nullptr) {
				mutableScene->mMetaData = aiMetadata::Alloc (1);
				mutableScene->mMetaData->Set (0, "Application", aiString (generator));
			} else {
				mutableScene->mMetaData->Add ("Application", aiString (generator));
			}
		}
	}

	if (inputIsGltf && format == "obj") {
		if (!ExportSceneObjCustom (mutableScene, result, projectName, metadata)) {
			result.errorCode = ErrorCode::ExportError;
			return false;
		}
		for (const auto& extra : extraFiles) {
			if (result.fileList.GetFile (extra.path) == nullptr) {
				result.fileList.AddFile (extra.path, extra.content);
			}
		}
		std::string zipName = GetFileNameFromFormat ("obj", projectName);
		zipName = zipName.substr (0, zipName.find_last_of ('.')) + ".zip";
		if (!ReplaceFileListWithZip (result.fileList, zipName, result)) {
			result.errorCode = ErrorCode::ExportError;
			return false;
		}
		result.errorCode = ErrorCode::NoError;
		return true;
	}
	if (inputIsGltf && isFbxOutput) {
		constexpr ai_real kCreaseAngleDegrees = 60.0f;
		exportProperties.SetPropertyFloat (AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, kCreaseAngleDegrees);
		exportProperties.SetPropertyBool ("assimpjs.fbx.join_position_vertices", true);
		exportPostprocess |= aiProcess_ForceGenNormals;
		exportPostprocess |= aiProcess_GenSmoothNormals;
		exportPostprocess |= aiProcess_JoinIdenticalVertices;
	}
	if (inputIsGltf && isGltfOutput) {
		// glTF uses a single shared index for all vertex attributes, so the only
		// safe merge here is exact render-vertex deduplication.
		exportPostprocess |= aiProcess_JoinIdenticalVertices;
	}
	if (inputIsGltf && format == "3mf") {
		exportProperties.SetPropertyBool ("assimpjs.3mf.join_position_vertices", true);
	}
	if (inputIsGltf && isStlOutput) {
		exportPostprocess |= aiProcess_ForceGenNormals;
		exportPostprocess |= aiProcess_GenNormals;
	}

	aiReturn exportResult = aiReturn_FAILURE;
	try {
		exportResult = exporter.Export (mutableScene, assimpFormat.c_str (), fileName.c_str (), exportPostprocess, &exportProperties);
	} catch (const std::exception&) {
		exportResult = aiReturn_FAILURE;
	} catch (...) {
		exportResult = aiReturn_FAILURE;
	}
	if (isGltfOutput) {
		mutableScene->mFlags = originalFlags;
	}
	if (exportResult != aiReturn_SUCCESS) {
		result.errorCode = ErrorCode::ExportError;
		return false;
	}

	for (const auto& extra : extraFiles) {
		if (result.fileList.GetFile (extra.path) == nullptr) {
			result.fileList.AddFile (extra.path, extra.content);
		}
	}
	if (sourceFiles != nullptr) {
		for (const RenamedTextureFile& renamed : renamedExternalFiles) {
			if (result.fileList.GetFile (renamed.outputPath) != nullptr) {
				continue;
			}
			const File* sourceFile = FindTextureSourceFile (*sourceFiles, renamed.sourcePath);
			if (sourceFile != nullptr) {
				result.fileList.AddFile (renamed.outputPath, sourceFile->content);
			}
		}
	}

	if (format == "obj") {
		std::string zipName = GetFileNameFromFormat ("obj", projectName);
		zipName = zipName.substr(0, zipName.find_last_of('.')) + ".zip";
		if (!ReplaceFileListWithZip (result.fileList, zipName, result)) {
			result.errorCode = ErrorCode::ExportError;
			return false;
		}
	}
	if (isFbxOutput) {
		std::string zipName = GetFileNameFromFormat ("fbx", projectName);
		zipName = zipName.substr(0, zipName.find_last_of('.')) + ".zip";
		if (!ReplaceFileListWithZip (result.fileList, zipName, result)) {
			result.errorCode = ErrorCode::ExportError;
			return false;
		}
	}

	result.errorCode = ErrorCode::NoError;
	return true;
}

static void RemoveUnusedMaterials (aiScene* scene)
{
	if (scene == nullptr || scene->mNumMaterials == 0) {
		return;
	}

	// 标记使用的材质
	std::vector<bool> usedMaterials (scene->mNumMaterials, false);
	for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
		if (scene->mMeshes[i] != nullptr) {
			unsigned int matIdx = scene->mMeshes[i]->mMaterialIndex;
			if (matIdx < scene->mNumMaterials) {
				usedMaterials[matIdx] = true;
			}
		}
	}

	// 计算未使用的材质数量
	unsigned int numUnused = 0;
	for (bool used : usedMaterials) {
		if (!used) {
			++numUnused;
		}
	}

	if (numUnused == 0) {
		return; // 所有材质都在使用
	}

	// 创建旧索引到新索引的映射
	std::vector<unsigned int> oldToNew (scene->mNumMaterials);
	unsigned int newIdx = 0;
	for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
		if (usedMaterials[i]) {
			oldToNew[i] = newIdx++;
		}
	}

	// 创建新的材质数组
	unsigned int newNumMaterials = scene->mNumMaterials - numUnused;
	aiMaterial** newMaterials = new aiMaterial*[newNumMaterials];
	newIdx = 0;
	for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
		if (usedMaterials[i]) {
			newMaterials[newIdx++] = scene->mMaterials[i];
		} else {
			delete scene->mMaterials[i];
		}
	}

	// 更新场景
	delete[] scene->mMaterials;
	scene->mMaterials = newMaterials;
	scene->mNumMaterials = newNumMaterials;

	// 更新 mesh 的材质索引
	for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
		if (scene->mMeshes[i] != nullptr) {
			unsigned int oldIdx = scene->mMeshes[i]->mMaterialIndex;
			if (oldIdx < oldToNew.size () && usedMaterials[oldIdx]) {
				scene->mMeshes[i]->mMaterialIndex = oldToNew[oldIdx];
			}
		}
	}
}

static bool ApplyMetadata (aiScene* scene, const MetadataOptions* meta)
{
	if (scene == nullptr || meta == nullptr) {
		return true;
	}

	if (!meta->rootTransform.empty ()) {
		aiMatrix4x4 m;
		if (TryCreateMatrix4 (meta->rootTransform, m)) {
			ApplyTransformToRootNode (scene, m);
		}
	}

	if (!meta->childTransforms.empty ()) {
		NodeTransformMap map;
		for (const auto& kv : meta->childTransforms) {
			aiMatrix4x4 m;
			if (TryCreateMatrix4 (kv.second, m)) {
				map.emplace (kv.first, m);
			}
		}
		if (!map.empty ()) {
			ApplyTransformsToNodesByName (scene->mRootNode, map);
		}
	}

	// children_rename is intentionally disabled.

	if (!meta->childDeleted.empty ()) {
		DeleteNodesByName (scene->mRootNode, meta->childDeleted);
	}

	if (meta->hasMaterialFactors) {
		ApplyMaterialFactors (scene, meta->metallic, meta->roughness);
	}

	return true;
}

Result ConvertFile (const File& file, const std::string& format, const FileLoader& loader, const MetadataOptions* metadata, const std::string& projectName)
{
	Assimp::Importer importer;
	importer.SetIOHandler (new DelayLoadedIOSystemReadAdapter (file, loader));
	const unsigned int flags = GetImportFlagsForFormat (format);
	const aiScene* scene = ImportFileListByMainFile (importer, file, flags);

	Result result;
	aiScene* mutableScene = const_cast<aiScene*> (scene);
	RemoveUnusedMaterials (mutableScene);
	ApplyMetadata (mutableScene, metadata);
	ExportScene (mutableScene, format, result, projectName, metadata, GetExtFromPath (file.path), nullptr);
	return result;
}

Result ConvertFileList (const FileList& fileList, const std::string& format, const MetadataOptions* metadata, const std::string& projectName)
{
	if (fileList.FileCount () == 0) {
		return Result (ErrorCode::NoFilesFound);
	}

	Assimp::Importer importer;
	importer.SetIOHandler (new FileListIOSystemReadAdapter (fileList));
	const unsigned int flags = GetImportFlagsForFormat (format);

	const aiScene* scene = nullptr;
	std::string inputPath;
	for (size_t fileIndex = 0; fileIndex < fileList.FileCount (); fileIndex++) {
		const File& file = fileList.GetFile (fileIndex);
		scene = ImportFileListByMainFile (importer, file, flags);
		if (scene != nullptr) {
			inputPath = file.path;
			break;
		}
	}

	Result result;
	aiScene* mutableScene = const_cast<aiScene*> (scene);
	RemoveUnusedMaterials (mutableScene);
	ApplyMetadata (mutableScene, metadata);
	ExportScene (mutableScene, format, result, projectName, metadata, GetExtFromPath (inputPath), &fileList);
	return result;
}

Result ConvertFileListWithTransform (const FileList& fileList, const std::string& format, const std::vector<float>& matrix16, const MetadataOptions* metadata, const std::string& projectName)
{
	if (fileList.FileCount () == 0) {
		return Result (ErrorCode::NoFilesFound);
	}

	aiMatrix4x4 transform;
	if (!TryCreateMatrix4 (matrix16, transform)) {
		return Result (ErrorCode::UnknownError);
	}

	Assimp::Importer importer;
	importer.SetIOHandler (new FileListIOSystemReadAdapter (fileList));
	const unsigned int flags = GetImportFlagsForFormat (format);

	const aiScene* scene = nullptr;
	std::string inputPath;
	for (size_t fileIndex = 0; fileIndex < fileList.FileCount (); fileIndex++) {
		const File& file = fileList.GetFile (fileIndex);
		scene = ImportFileListByMainFile (importer, file, flags);
		if (scene != nullptr) {
			inputPath = file.path;
			break;
		}
	}

	Result result;
	if (!ApplyTransformToRootNode (scene, transform)) {
		result.errorCode = ErrorCode::ImportError;
		return result;
	}
	aiScene* mutableScene = const_cast<aiScene*> (scene);
	ApplyMetadata (mutableScene, metadata);
	ExportScene (mutableScene, format, result, projectName, metadata, GetExtFromPath (inputPath), &fileList);
	return result;
}

Result ConvertFileListWithNodeTransforms (const FileList& fileList, const std::string& format, const NodeTransformMap& transformByName, const MetadataOptions* metadata, const std::string& projectName)
{
	if (fileList.FileCount () == 0) {
		return Result (ErrorCode::NoFilesFound);
	}

	Assimp::Importer importer;
	importer.SetIOHandler (new FileListIOSystemReadAdapter (fileList));
	const unsigned int flags = GetImportFlagsForFormat (format);

	const aiScene* scene = nullptr;
	std::string inputPath;
	for (size_t fileIndex = 0; fileIndex < fileList.FileCount (); fileIndex++) {
		const File& file = fileList.GetFile (fileIndex);
		scene = ImportFileListByMainFile (importer, file, flags);
		if (scene != nullptr) {
			inputPath = file.path;
			break;
		}
	}

	Result result;
	if (scene == nullptr) {
		result.errorCode = ErrorCode::ImportError;
		return result;
	}
	ApplyTransformsToNodesByName (scene->mRootNode, transformByName);
	aiScene* mutableScene = const_cast<aiScene*> (scene);
	ApplyMetadata (mutableScene, metadata);
	ExportScene (mutableScene, format, result, projectName, metadata, GetExtFromPath (inputPath), &fileList);
	return result;
}

#ifdef EMSCRIPTEN

static bool TryReadMetadata (const emscripten::val& input, MetadataOptions& meta);

Result ConvertFileEmscripten (
	const std::string& name,
	const std::string& format,
	const emscripten::val& content,
	const emscripten::val& existsFunc,
	const emscripten::val& loadFunc,
	const emscripten::val& metadataInput = emscripten::val::undefined (),
	const std::string& projectName = std::string ())
{
	class FileLoaderEmscripten : public FileLoader
	{
	public:
		FileLoaderEmscripten (const emscripten::val& existsFunc, const emscripten::val& loadFunc) :
			existsFunc (existsFunc),
			loadFunc (loadFunc)
		{
		}

		virtual bool Exists (const char* pFile) const override
		{
			if (existsFunc.isUndefined () || existsFunc.isNull ()) {
				return false;
			}
			std::string fileName = GetFileName (pFile);
			emscripten::val exists = existsFunc (fileName);
			return exists.as<bool> ();
		}

		virtual Buffer Load (const char* pFile) const override
		{
			if (loadFunc.isUndefined () || loadFunc.isNull ()) {
				return {};
			}
			std::string fileName = GetFileName (pFile);
			emscripten::val fileBuffer = loadFunc (fileName);

			emscripten::val Uint8Array = emscripten::val::global("Uint8Array");
			bool isUint8Array = fileBuffer.instanceof(Uint8Array);
			if (isUint8Array) {
				unsigned int length = fileBuffer["length"].as<unsigned int>();
				Buffer result(length);
				emscripten::val memory = emscripten::val::module_property("HEAPU8");
				unsigned int offset = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(result.data()));
				emscripten::val memoryView = memory.call<emscripten::val>("subarray", offset, offset + length);
				memoryView.call<void>("set", fileBuffer);
				return result;
			}
			return emscripten::vecFromJSArray<std::uint8_t> (fileBuffer);
		}

	private:
		const emscripten::val& existsFunc;
		const emscripten::val& loadFunc;
	};

	Buffer buffer;
	{
		emscripten::val Uint8Array = emscripten::val::global("Uint8Array");
		bool isUint8Array = content.instanceof(Uint8Array);
		if (isUint8Array) {
			unsigned int length = content["length"].as<unsigned int>();
			buffer.resize(length);
			emscripten::val memory = emscripten::val::module_property("HEAPU8");
			unsigned int offset = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(buffer.data()));
			emscripten::val memoryView = memory.call<emscripten::val>("subarray", offset, offset + length);
			memoryView.call<void>("set", content);
		} else {
			buffer = emscripten::vecFromJSArray<std::uint8_t>(content);
		}
	}
	File file (name, buffer);
	FileLoaderEmscripten loader (existsFunc, loadFunc);
	MetadataOptions metadata;
	bool hasMeta = TryReadMetadata (metadataInput, metadata);
	return ConvertFile (file, format, loader, hasMeta ? &metadata : nullptr, projectName);
}

Result ConvertFileEmscriptenV1 (
	const std::string& name,
	const std::string& format,
	const emscripten::val& content,
	const emscripten::val& existsFunc,
	const emscripten::val& loadFunc)
{
	return ConvertFileEmscripten (name, format, content, existsFunc, loadFunc, emscripten::val::undefined (), std::string ());
}

Result ConvertFileListEmscripten (
	const FileList& fileList,
	const std::string& format,
	const emscripten::val& metadataInput = emscripten::val::undefined (),
	const std::string& projectName = std::string ())
{
	MetadataOptions metadata;
	bool hasMeta = TryReadMetadata (metadataInput, metadata);
	return ConvertFileList (fileList, format, hasMeta ? &metadata : nullptr, projectName);
}

Result ConvertFileListEmscriptenV1 (
	const FileList& fileList,
	const std::string& format)
{
	return ConvertFileList (fileList, format, nullptr, std::string ());
}

static bool TryReadTransformMatrix (const emscripten::val& matrixInput, std::vector<float>& matrix16)
{
	if (matrixInput.isUndefined () || matrixInput.isNull ()) {
		return false;
	}

	emscripten::val lengthValue = matrixInput["length"];
	if (lengthValue.isUndefined () || lengthValue.isNull ()) {
		return false;
	}

	unsigned int length = 0;
	try {
		length = lengthValue.as<unsigned int> ();
	} catch (...) {
		return false;
	}
	if (length != 16) {
		return false;
	}

	matrix16.clear ();
	matrix16.reserve (16);
	for (unsigned int index = 0; index < 16; ++index) {
		double value = 0.0;
		try {
			value = matrixInput[index].as<double> ();
		} catch (...) {
			return false;
		}
		if (!std::isfinite (value)) {
			return false;
		}
		matrix16.push_back (static_cast<float> (value));
	}
	return true;
}

static bool TryReadNodeTransformMap (const emscripten::val& transformInput, NodeTransformMap& transformByName)
{
	if (transformInput.isUndefined () || transformInput.isNull ()) {
		return false;
	}

	emscripten::val objectValue = emscripten::val::global ("Object");
	emscripten::val keys = objectValue.call<emscripten::val> ("keys", transformInput);
	if (keys.isUndefined () || keys.isNull ()) {
		return false;
	}

	emscripten::val lengthValue = keys["length"];
	if (lengthValue.isUndefined () || lengthValue.isNull ()) {
		return false;
	}

	unsigned int length = 0;
	try {
		length = lengthValue.as<unsigned int> ();
	} catch (...) {
		return false;
	}

	transformByName.clear ();
	transformByName.reserve (length);
	for (unsigned int index = 0; index < length; ++index) {
		std::string nodeName;
		try {
			nodeName = keys[index].as<std::string> ();
		} catch (...) {
			return false;
		}
		std::vector<float> matrix16;
		if (!TryReadTransformMatrix (transformInput[nodeName], matrix16)) {
			return false;
		}
		aiMatrix4x4 matrix;
		if (!TryCreateMatrix4 (matrix16, matrix)) {
			return false;
		}
		transformByName.emplace (nodeName, matrix);
	}

	return true;
}

static bool TryReadString (const emscripten::val& v, std::string& out)
{
	if (v.isUndefined () || v.isNull ()) {
		return false;
	}
	try {
		out = v.as<std::string> ();
	} catch (...) {
		return false;
	}
	return true;
}

static bool TryReadFloat (const emscripten::val& v, float& out)
{
	if (v.isUndefined () || v.isNull ()) {
		return false;
	}
	try {
		double d = v.as<double> ();
		if (!std::isfinite (d)) {
			return false;
		}
		out = static_cast<float> (d);
	} catch (...) {
		return false;
	}
	return true;
}

static bool TryReadMetadata (const emscripten::val& input, MetadataOptions& meta)
{
	if (input.isUndefined () || input.isNull ()) {
		return false;
	}
	bool any = false;
	if (input.hasOwnProperty ("transform_matrix")) {
		TryReadTransformMatrix (input["transform_matrix"], meta.rootTransform);
		any = any || !meta.rootTransform.empty ();
	}
	if (input.hasOwnProperty ("children_transform_matrix")) {
		emscripten::val ctm = input["children_transform_matrix"];
		emscripten::val keys = emscripten::val::global ("Object").call<emscripten::val> ("keys", ctm);
		unsigned int len = keys["length"].as<unsigned int> ();
		for (unsigned int i = 0; i < len; ++i) {
			std::string key = keys[i].as<std::string> ();
			std::vector<float> m;
			if (TryReadTransformMatrix (ctm[key], m)) {
				meta.childTransforms.emplace (key, m);
			}
		}
		any = any || !meta.childTransforms.empty ();
	}
	if (input.hasOwnProperty ("children_rename")) {
		emscripten::val cr = input["children_rename"];
		emscripten::val keys = emscripten::val::global ("Object").call<emscripten::val> ("keys", cr);
		unsigned int len = keys["length"].as<unsigned int> ();
		for (unsigned int i = 0; i < len; ++i) {
			std::string key = keys[i].as<std::string> ();
			std::string val;
			if (TryReadString (cr[key], val)) {
				meta.childRenames.emplace (key, val);
			}
		}
		any = any || !meta.childRenames.empty ();
	}
	if (input.hasOwnProperty ("children_deleted")) {
		emscripten::val cd = input["children_deleted"];
		unsigned int len = 0;
		try { len = cd["length"].as<unsigned int> (); } catch (...) { len = 0; }
		for (unsigned int i = 0; i < len; ++i) {
			std::string name;
			if (TryReadString (cd[i], name)) {
				meta.childDeleted.insert (name);
			}
		}
		any = any || !meta.childDeleted.empty ();
	}
	if (input.hasOwnProperty ("task_id")) {
		if (TryReadString (input["task_id"], meta.taskId)) {
			any = true;
		}
	}
	if (input.hasOwnProperty ("material_factor")) {
		emscripten::val mf = input["material_factor"];
		float m = 0.0f, r = 0.5f;
		bool hm = TryReadFloat (mf["metallic"], m);
		bool hr = TryReadFloat (mf["roughness"], r);
		if (hm || hr) {
			meta.hasMaterialFactors = true;
			meta.metallic = m;
			meta.roughness = r;
			any = true;
		}
	}
	return any;
}

Result ConvertFileListWithTransformEmscripten (
	const FileList& fileList,
	const std::string& format,
	const emscripten::val& matrixInput,
	const emscripten::val& metadataInput = emscripten::val::undefined (),
	const std::string& projectName = std::string ())
{
	std::vector<float> matrix16;
	if (!TryReadTransformMatrix (matrixInput, matrix16)) {
		return Result (ErrorCode::UnknownError);
	}
	MetadataOptions metadata;
	bool hasMeta = TryReadMetadata (metadataInput, metadata);
	return ConvertFileListWithTransform (fileList, format, matrix16, hasMeta ? &metadata : nullptr, projectName);
}

Result ConvertFileListWithTransformEmscriptenV1 (
	const FileList& fileList,
	const std::string& format,
	const emscripten::val& matrixInput)
{
	std::vector<float> matrix16;
	if (!TryReadTransformMatrix (matrixInput, matrix16)) {
		return Result (ErrorCode::UnknownError);
	}
	return ConvertFileListWithTransform (fileList, format, matrix16, nullptr, std::string ());
}

Result ConvertFileListWithNodeTransformsEmscripten (
	const FileList& fileList,
	const std::string& format,
	const emscripten::val& transformInput,
	const emscripten::val& metadataInput = emscripten::val::undefined (),
	const std::string& projectName = std::string ())
{
	NodeTransformMap transformByName;
	if (!TryReadNodeTransformMap (transformInput, transformByName)) {
		return Result (ErrorCode::UnknownError);
	}
	MetadataOptions metadata;
	bool hasMeta = TryReadMetadata (metadataInput, metadata);
	return ConvertFileListWithNodeTransforms (fileList, format, transformByName, hasMeta ? &metadata : nullptr, projectName);
}

Result ConvertFileListWithNodeTransformsEmscriptenV1 (
	const FileList& fileList,
	const std::string& format,
	const emscripten::val& transformInput)
{
	NodeTransformMap transformByName;
	if (!TryReadNodeTransformMap (transformInput, transformByName)) {
		return Result (ErrorCode::UnknownError);
	}
	return ConvertFileListWithNodeTransforms (fileList, format, transformByName, nullptr, std::string ());
}

EMSCRIPTEN_BINDINGS (assimpjs)
{
	emscripten::class_<File> ("File")
		.constructor<> ()
		.function ("GetPath", &File::GetPath)
		.function ("GetContent", &File::GetContentEmscripten)
	;

	emscripten::class_<FileList> ("FileList")
		.constructor<> ()
		.function ("AddFile", &FileList::AddFileEmscripten)
	;

	emscripten::class_<Result> ("Result")
		.constructor<> ()
		.function ("IsSuccess", &Result::IsSuccess)
		.function ("GetErrorCode", &Result::GetErrorCode)
		.function ("FileCount", &Result::FileCount)
		.function ("GetFile", &Result::GetFile)
	;

	emscripten::function ("ConvertFile", &ConvertFileEmscriptenV1);
	emscripten::function ("ConvertFile", &ConvertFileEmscripten);
	emscripten::function ("ConvertFileList", &ConvertFileListEmscriptenV1);
	emscripten::function ("ConvertFileList", &ConvertFileListEmscripten);
	emscripten::function ("ConvertFileListWithTransform", &ConvertFileListWithTransformEmscriptenV1);
	emscripten::function ("ConvertFileListWithTransform", &ConvertFileListWithTransformEmscripten);
	emscripten::function ("ConvertFileListWithNodeTransforms", &ConvertFileListWithNodeTransformsEmscriptenV1);
	emscripten::function ("ConvertFileListWithNodeTransforms", &ConvertFileListWithNodeTransformsEmscripten);
}

#endif
