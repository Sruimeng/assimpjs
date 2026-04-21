#include "assimpjs_internal.hpp"

#include <assimp/material.h>
#include <assimp/matrix3x3.h>

#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{

struct ObjMeshInstance
{
	const aiMesh* mesh;
	aiMatrix4x4 transform;
	std::string name;
};

void CollectObjMeshInstances (
	const aiScene* scene,
	const aiNode* node,
	const aiMatrix4x4& parent,
	std::vector<ObjMeshInstance>& out)
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

std::string GetObjMaterialName (
	const aiMaterial* mat,
	unsigned int materialIndex,
	std::unordered_map<std::string, size_t>& counts)
{
	aiString aiName;
	std::string raw = "mat_" + std::to_string (materialIndex);
	if (mat != nullptr && mat->Get (AI_MATKEY_NAME, aiName) == AI_SUCCESS && aiName.length > 0) {
		raw = aiName.C_Str ();
	}
	return SanitizeUsdIdentifier (raw, materialIndex, counts);
}

bool TryGetMaterialTexturePath (const aiMaterial* mat, aiTextureType type, std::string& outPath)
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

}

bool ExportSceneObjCustom (
	const aiScene* scene,
	Result& result,
	const std::string& projectName,
	const MetadataOptions* metadata)
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

	std::unordered_map<ObjVec3Key, size_t, ObjKeyHash> positionMap;
	std::unordered_map<ObjVec2Key, size_t, ObjKeyHash> uvMap;
	std::unordered_map<ObjVec3Key, size_t, ObjKeyHash> normalMap;
	std::vector<aiVector3D> positions;
	std::vector<aiVector3D> uvs;
	std::vector<aiVector3D> normals;
	positionMap.reserve (16384);
	uvMap.reserve (16384);
	normalMap.reserve (16384);

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

		aiMatrix3x3 normalMatrix (instance.transform);
		normalMatrix.Inverse ();
		normalMatrix.Transpose ();

		std::vector<size_t> positionIndices (instance.mesh->mNumVertices, 0);
		std::vector<size_t> uvIndices (instance.mesh->mNumVertices, 0);
		std::vector<size_t> normalIndices (instance.mesh->mNumVertices, 0);

		for (unsigned int vertexIndex = 0; vertexIndex < instance.mesh->mNumVertices; ++vertexIndex) {
			aiVector3D transformedPosition = instance.transform * instance.mesh->mVertices[vertexIndex];
			positionIndices[vertexIndex] = appendPosition (transformedPosition);

			if (instance.mesh->HasTextureCoords (0)) {
				uvIndices[vertexIndex] = appendUV (instance.mesh->mTextureCoords[0][vertexIndex]);
			}

			if (instance.mesh->HasNormals ()) {
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
				const size_t uvIndex = uvIndices[vertexIndex];
				const size_t normalIndex = normalIndices[vertexIndex];
				obj << " " << positionIndex;
				if (instance.mesh->HasTextureCoords (0) || instance.mesh->HasNormals ()) {
					obj << "/";
					if (instance.mesh->HasTextureCoords (0)) {
						obj << uvIndex;
					}
					if (instance.mesh->HasNormals ()) {
						obj << "/" << normalIndex;
					}
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
