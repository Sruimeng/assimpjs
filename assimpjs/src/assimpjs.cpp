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

	if (format == "glb" || format == "glb2") {
		flags &= ~aiProcess_JoinIdenticalVertices;
	}

	if (format == "fbx" || format == "obj") {
		flags |= aiProcess_EmbedTextures;
	}
	return flags;
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

static std::string GetFileNameFromFormat (const std::string& format)
{
	std::string fileName = "result";
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

static std::vector<EmbeddedTextureFile> ExtractEmbeddedTextures (aiScene* scene, const std::string& folderPrefix)
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

static bool ExportSceneUsdFallback (const aiScene* scene, const std::string& format, Result& result)
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
	result.fileList.AddFile (GetFileNameFromFormat (format), content);
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

static bool BuildTinyUsdScene (const aiScene* scene, tinyusdz::tydra::RenderScene& out, std::string& err)
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

		rmesh.points.resize (inst.mesh->mNumVertices);
		for (unsigned int v = 0; v < inst.mesh->mNumVertices; ++v) {
			aiVector3D p = inst.transform * inst.mesh->mVertices[v];
			rmesh.points[v][0] = p.x;
			rmesh.points[v][1] = p.y;
			rmesh.points[v][2] = p.z;
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
			normals.format = tinyusdz::tydra::VertexAttributeFormat::Vec3;
			normals.variability = tinyusdz::tydra::VertexVariability::Vertex;
			normals.elementSize = 1;
			normals.set_buffer (reinterpret_cast<const std::uint8_t*> (normalData.data ()), normalData.size () * sizeof (float));
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
			uv.name = "st";
			uv.format = tinyusdz::tydra::VertexAttributeFormat::Vec2;
			uv.variability = tinyusdz::tydra::VertexVariability::Vertex;
			uv.elementSize = 1;
			uv.set_buffer (reinterpret_cast<const std::uint8_t*> (uvData.data ()), uvData.size () * sizeof (float));
			rmesh.texcoords[0] = std::move (uv);
		}

		out.meshes.emplace_back (std::move (rmesh));
	}

	return true;
}

#endif

static bool ExportSceneUsd (const aiScene* scene, const std::string& format, Result& result)
{
#ifdef ASSIMPJS_ENABLE_TINYUSDZ
	tinyusdz::tydra::RenderScene renderScene;
	std::string err;
	if (!BuildTinyUsdScene (scene, renderScene, err)) {
		result.errorCode = ErrorCode::ExportError;
		return false;
	}
	std::string warn;
	std::string usdaStr;
	if (!tinyusdz::tydra::export_to_usda (renderScene, usdaStr, &warn, &err)) {
		if (format == "usda") {
			return ExportSceneUsdFallback (scene, format, result);
		}
		result.errorCode = ErrorCode::ExportError;
		return false;
	}

	if (format == "usd" || format == "usdc") {
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
			result.fileList.AddFile (GetFileNameFromFormat ("usd"), content);
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
			result.fileList.AddFile (GetFileNameFromFormat ("usd"), content);
			result.errorCode = ErrorCode::NoError;
			return true;
		}
		Buffer content (usdc.begin (), usdc.end ());
		result.fileList.AddFile (GetFileNameFromFormat (format), content);
		result.errorCode = ErrorCode::NoError;
		return true;
	}

	Buffer content (usdaStr.begin (), usdaStr.end ());
	result.fileList.AddFile (GetFileNameFromFormat (format), content);
	result.errorCode = ErrorCode::NoError;
	return true;
#else
	return ExportSceneUsdFallback (scene, format, result);
#endif
}

static bool ExportScene (const aiScene* scene, const std::string& format, Result& result)
{
	if (scene == nullptr) {
		result.errorCode = ErrorCode::ImportError;
		return false;
	}

	if (format == "usd" || format == "usda" || format == "usdc") {
		return ExportSceneUsd (scene, format, result);
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
	std::string fileName = GetFileNameFromFormat (format);
	
	// Map dae format to collada for Assimp's internal format identifier
	std::string assimpFormat = format;
	if (format == "dae") {
		assimpFormat = "collada";
	} else if (format == "stl") {
		// Prefer binary STL to reduce memory usage and avoid stream failures.
		assimpFormat = "stlb";
	}
	
	std::vector<EmbeddedTextureFile> extraFiles;
	if (format == "fbx") {
		extraFiles = ExtractEmbeddedTextures (mutableScene, "result.fbm");
	} else if (format == "obj") {
		extraFiles = ExtractEmbeddedTextures (mutableScene, "");
	}

	aiReturn exportResult = aiReturn_FAILURE;
	try {
		exportResult = exporter.Export (scene, assimpFormat.c_str (), fileName.c_str (), 0u, &exportProperties);
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

Result ConvertFile (const File& file, const std::string& format, const FileLoader& loader)
{
	Assimp::Importer importer;
	importer.SetIOHandler (new DelayLoadedIOSystemReadAdapter (file, loader));
	const unsigned int flags = GetImportFlagsForFormat (format);
	const aiScene* scene = ImportFileListByMainFile (importer, file, flags);

	Result result;
	ExportScene (scene, format, result);
	return result;
}

Result ConvertFileList (const FileList& fileList, const std::string& format)
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
	ExportScene (scene, format, result);
	return result;
}

#ifdef EMSCRIPTEN

Result ConvertFileEmscripten (
	const std::string& name,
	const std::string& format,
	const emscripten::val& content,
	const emscripten::val& existsFunc,
	const emscripten::val& loadFunc)
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
			return emscripten::vecFromJSArray<std::uint8_t> (fileBuffer);
		}

	private:
		const emscripten::val& existsFunc;
		const emscripten::val& loadFunc;
	};

	Buffer buffer = emscripten::vecFromJSArray<std::uint8_t> (content);
	File file (name, buffer);
	FileLoaderEmscripten loader (existsFunc, loadFunc);
	return ConvertFile (file, format, loader);
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

	emscripten::function<Result, const std::string&, const std::string&, const emscripten::val&, const emscripten::val&, const emscripten::val&> ("ConvertFile", &ConvertFileEmscripten);
	emscripten::function<Result, const FileList&, const std::string&> ("ConvertFileList", &ConvertFileList);
}

#endif
