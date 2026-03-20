#include "assimpjs.hpp"

#include <assimp/Importer.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/matrix3x3.h>
#include <assimp/material.h>

#include <stdio.h>
#include <iostream>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

struct EmbeddedTextureFile
{
	std::string path;
	Buffer content;
};

static bool TryCreateMatrix4 (const std::vector<float>& matrix16, aiMatrix4x4& matrix);

static aiMatrix4x4 ToMatrix (const std::vector<float>& matrix16)
{
	aiMatrix4x4 m;
	TryCreateMatrix4 (matrix16, m);
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
			size = static_cast<size_t> (tex->mWidth) * static_cast<size_t> (tex->mHeight) * 4u;
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
	bool multiPart = false;
	std::unordered_map<unsigned int, std::string> partNameByMaterial; // material index -> part name
	std::unordered_map<const aiTexture*, std::string> embeddedOriginal;
	std::unordered_map<const aiTexture*, std::string> embeddedNew;
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

static std::string ComposeTexBase (const TextureNamingContext& ctx, const std::string& part)
{
	if (ctx.multiPart && !part.empty ()) {
		return ctx.project + "_" + part;
	}
	return ctx.project;
}

static void RenameMaterialTextures (
	aiScene* scene,
	TextureNamingContext& naming,
	std::vector<EmbeddedTextureFile>& embeddedFiles)
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
					naming.embeddedNew[embPtr] = newName;
				}
				aiString newPath = MakeTextureName (newName);
				mat->AddProperty (&newPath, AI_MATKEY_TEXTURE (t, ti));
			}
		};

		processType (aiTextureType_BASE_COLOR, "_basecolor");
		processType (aiTextureType_DIFFUSE, "_basecolor");
		processType (aiTextureType_NORMALS, "_normal");
		processType (aiTextureType_NORMAL_CAMERA, "_normal");
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

static void WriteUsdMesh (std::ostringstream& ss, const aiMesh* mesh, const std::string& meshName, int indent)
{
	if (mesh == nullptr) {
		return;
	}

	WriteIndent (ss, indent);
	ss << "def Mesh \"" << meshName << "\" {\n";
	WriteIndent (ss, indent + 2);
	ss << "uniform token subdivisionScheme = \"none\"\n";

	WriteIndent (ss, indent + 2);
	ss << "point3f[] points = [";
	for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
		WriteVec3 (ss, mesh->mVertices[v]);
		if (v + 1 < mesh->mNumVertices) {
			ss << ", ";
		}
	}
	ss << "]\n";

	WriteIndent (ss, indent + 2);
	ss << "int[] faceVertexCounts = [";
	for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
		const aiFace& face = mesh->mFaces[f];
		ss << face.mNumIndices;
		if (f + 1 < mesh->mNumFaces) {
			ss << ", ";
		}
	}
	ss << "]\n";

	WriteIndent (ss, indent + 2);
	ss << "int[] faceVertexIndices = [";
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
	ss << "]\n";

	if (mesh->HasNormals ()) {
		WriteIndent (ss, indent + 2);
		ss << "normal3f[] normals = [";
		for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
			WriteVec3 (ss, mesh->mNormals[v]);
			if (v + 1 < mesh->mNumVertices) {
				ss << ", ";
			}
		}
		ss << "]\n";
		WriteIndent (ss, indent + 2);
		ss << "uniform token normals:interpolation = \"vertex\"\n";
	}

	if (mesh->HasTextureCoords (0)) {
		WriteIndent (ss, indent + 2);
		ss << "float2[] primvars:st = [";
		for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
			WriteVec2 (ss, mesh->mTextureCoords[0][v]);
			if (v + 1 < mesh->mNumVertices) {
				ss << ", ";
			}
		}
		ss << "]\n";
		WriteIndent (ss, indent + 2);
		ss << "uniform token primvars:st:interpolation = \"vertex\"\n";
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

static void WriteUsdNode (std::ostringstream& ss, const aiScene* scene, const aiNode* node, std::unordered_map<std::string, size_t>& nameCounts, int indent)
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
			WriteUsdMesh (ss, mesh, meshName, indent + 2);
		}
	}

	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		WriteUsdNode (ss, scene, node->mChildren[i], nameCounts, indent + 2);
	}

	WriteIndent (ss, indent);
	ss << "}\n";
}

static bool ExportSceneUsdFallback (const aiScene* scene, const std::string& format, Result& result, const std::string& projectName)
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
			WriteUsdMesh (ss, mesh, meshName, 2);
		}
	}
	for (unsigned int i = 0; i < scene->mRootNode->mNumChildren; ++i) {
		WriteUsdNode (ss, scene, scene->mRootNode->mChildren[i], nameCounts, 2);
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
	std::string baseName = projectName.empty() ? "result" : projectName;
	img.asset_identifier = "textures/" + baseName + "_" + paramName + "." + ext;
	img.buffer_id = buf_id;
	img.decoded   = false;

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

static bool BuildTinyUsdScene (const aiScene* scene, tinyusdz::tydra::RenderScene& out, std::string& err, const std::string& projectName)
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
		rmesh.is_single_indexable = true;

		// Link to material
		if (inst.mesh->mMaterialIndex < scene->mNumMaterials) {
			rmesh.material_id = static_cast<int> (inst.mesh->mMaterialIndex);
		}

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

		out.meshes.emplace_back (std::move (rmesh));
	}

	return true;
}

#endif

static bool ExportSceneUsd (const aiScene* scene, const std::string& format, Result& result, const std::string& projectName)
{
#ifdef ASSIMPJS_ENABLE_TINYUSDZ
	tinyusdz::tydra::RenderScene renderScene;
	std::string err;
	if (!BuildTinyUsdScene (scene, renderScene, err, projectName)) {
		result.errorCode = ErrorCode::ExportError;
		return false;
	}
	std::string warn;
	std::string usdaStr;
	if (!tinyusdz::tydra::export_to_usda (renderScene, usdaStr, &warn, &err)) {
		if (format == "usda") {
			return ExportSceneUsdFallback (scene, format, result, projectName);
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
	return ExportSceneUsdFallback (scene, format, result, projectName);
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

static bool ExportScene (const aiScene* scene, const std::string& format, Result& result, const std::string& projectName)
{
	if (scene == nullptr) {
		result.errorCode = ErrorCode::ImportError;
		return false;
	}

	if (format == "usd" || format == "usda" || format == "usdc") {
		return ExportSceneUsd (scene, format, result, projectName);
	}

	const bool isGltfOutput =
		format == "gltf" || format == "gltf2" ||
		format == "glb" || format == "glb2";
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
	
	// Map dae format to collada for Assimp's internal format identifier
	std::string assimpFormat = format;
	if (format == "dae") {
		assimpFormat = "collada";
	} else if (format == "stl") {
		// Prefer binary STL to reduce memory usage and avoid stream failures.
		assimpFormat = "stlb";
	}
	
	TextureNamingContext naming;
	naming.project = projectName.empty () ? std::string ("result") : projectName;
	CollectMaterialParts (scene, naming);
	std::vector<EmbeddedTextureFile> extraFiles;
	if (format == "fbx") {
		extraFiles = ExtractEmbeddedTextures (mutableScene, "result.fbm", &naming.embeddedOriginal);
	} else if (format == "obj" || format == "gltf" || format == "gltf2") {
		extraFiles = ExtractEmbeddedTextures (mutableScene, "", &naming.embeddedOriginal);
	}
	RenameMaterialTextures (mutableScene, naming, extraFiles);
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

	if (format == "stl") {
		// GLB 是 Y-up，STL 工具通常期望 Z-up，补偿 +90° X 旋转
		aiMatrix4x4 rotX;
		aiMatrix4x4::RotationX (AI_MATH_HALF_PI, rotX);
		mutableScene->mRootNode->mTransformation = rotX * mutableScene->mRootNode->mTransformation;
	}

	if (format == "3mf") {
		// 3MF 需要 X 轴旋转 90 度 + 单位转换
		// 3MF导出器使用millimeter单位，GLB是meter单位
		// GLB的米值会直接变成3MF的毫米值（naive），需要 ×100 缩小 10x
		// 例如：GLB 0.5m → naive 0.5mm → ×100 → 50mm（相当于真实 500mm 缩小 10x）
		aiMatrix4x4 rotX;
		aiMatrix4x4::RotationX (AI_MATH_HALF_PI, rotX);
		aiMatrix4x4 scale;
		aiMatrix4x4::Scaling (aiVector3D (100.0f, 100.0f, 100.0f), scale);
		aiMatrix4x4 transform = scale * rotX;

		// 应用变换到所有mesh的顶点
		for (unsigned int i = 0; i < mutableScene->mNumMeshes; ++i) {
			aiMesh* mesh = mutableScene->mMeshes[i];
			if (mesh != nullptr) {
				for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
					mesh->mVertices[v] = transform * mesh->mVertices[v];
				}
				// 法线仅需旋转，不需缩放（均匀缩放不改变方向）
				// 用 rotX 代替 transform，避免 scale×100 后再 Normalize 的 sqrt 开销
				if (mesh->mNormals != nullptr) {
					for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
						mesh->mNormals[v] = rotX * mesh->mNormals[v];
						// rotX 是纯旋转，保持向量模长，无需 Normalize
					}
				}
			}
		}
	}

	aiReturn exportResult = aiReturn_FAILURE;
	try {
		exportResult = exporter.Export (mutableScene, assimpFormat.c_str (), fileName.c_str (), 0u, &exportProperties);
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

	if (format == "obj") {
		if (!ReplaceFileListWithZip (result.fileList, "result.zip", result)) {
			result.errorCode = ErrorCode::ExportError;
			return false;
		}
	}
	if (format == "fbx") {
		if (!ReplaceFileListWithZip (result.fileList, "result.zip", result)) {
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
	ExportScene (mutableScene, format, result, projectName);
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
	for (size_t fileIndex = 0; fileIndex < fileList.FileCount (); fileIndex++) {
		const File& file = fileList.GetFile (fileIndex);
		scene = ImportFileListByMainFile (importer, file, flags);
		if (scene != nullptr) {
			break;
		}
	}

	Result result;
	aiScene* mutableScene = const_cast<aiScene*> (scene);
	RemoveUnusedMaterials (mutableScene);
	ApplyMetadata (mutableScene, metadata);
	ExportScene (mutableScene, format, result, projectName);
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
	for (size_t fileIndex = 0; fileIndex < fileList.FileCount (); fileIndex++) {
		const File& file = fileList.GetFile (fileIndex);
		scene = ImportFileListByMainFile (importer, file, flags);
		if (scene != nullptr) {
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
	ExportScene (mutableScene, format, result, projectName);
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
	for (size_t fileIndex = 0; fileIndex < fileList.FileCount (); fileIndex++) {
		const File& file = fileList.GetFile (fileIndex);
		scene = ImportFileListByMainFile (importer, file, flags);
		if (scene != nullptr) {
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
	ExportScene (mutableScene, format, result, projectName);
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
