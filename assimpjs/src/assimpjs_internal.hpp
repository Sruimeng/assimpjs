#ifndef ASSIMPJS_INTERNAL_HPP
#define ASSIMPJS_INTERNAL_HPP

#include "assimpjs.hpp"

#include <assimp/material.h>
#include <assimp/matrix4x4.h>
#include <assimp/scene.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

struct EmbeddedTextureFile
{
	std::string path;
	Buffer content;
};

struct TextureNamingContext
{
	std::string project;
	bool multiPart = false;
	std::unordered_map<unsigned int, std::string> partNameByMaterial;
	std::unordered_map<const aiTexture*, std::string> embeddedOriginal;
	std::unordered_map<const aiTexture*, std::string> embeddedNew;
};

struct ObjVec3Key
{
	uint32_t x;
	uint32_t y;
	uint32_t z;

	bool operator== (const ObjVec3Key& other) const
	{
		return x == other.x && y == other.y && z == other.z;
	}
};

struct ObjVec2Key
{
	uint32_t x;
	uint32_t y;

	bool operator== (const ObjVec2Key& other) const
	{
		return x == other.x && y == other.y;
	}
};

struct ObjKeyHash
{
	template <typename T>
	std::size_t operator() (const T& key) const
	{
		std::size_t hash = static_cast<std::size_t> (2166136261u);
		const uint32_t* words = reinterpret_cast<const uint32_t*> (&key);
		for (size_t i = 0; i < sizeof (T) / sizeof (uint32_t); ++i) {
			hash ^= static_cast<std::size_t> (words[i]);
			hash *= static_cast<std::size_t> (16777619u);
		}
		return hash;
	}
};

inline uint32_t FloatBits (float value)
{
	uint32_t bits = 0;
	std::memcpy (&bits, &value, sizeof (bits));
	return bits;
}

inline ObjVec3Key MakeObjVec3Key (const aiVector3D& value)
{
	return { FloatBits (value.x), FloatBits (value.y), FloatBits (value.z) };
}

inline ObjVec2Key MakeObjVec2Key (const aiVector3D& value)
{
	return { FloatBits (value.x), FloatBits (value.y) };
}

inline bool TryCreateMatrix4 (const std::vector<float>& matrix16, aiMatrix4x4& matrix)
{
	if (matrix16.size () != 16) {
		return false;
	}
	matrix = aiMatrix4x4 (
		matrix16[0], matrix16[4], matrix16[8], matrix16[12],
		matrix16[1], matrix16[5], matrix16[9], matrix16[13],
		matrix16[2], matrix16[6], matrix16[10], matrix16[14],
		matrix16[3], matrix16[7], matrix16[11], matrix16[15]
	);
	return true;
}

inline std::string GetFileNameFromFormat (const std::string& format, const std::string& projectName)
{
	std::string fileName = projectName.empty () ? "result" : projectName;
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
	} else if (format == "stl" || format == "stlb") {
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

inline std::string ToPartName (const std::string& raw)
{
	if (raw.empty ()) {
		return "part";
	}
	std::string result = raw;
	for (char& c : result) {
		if (!(std::isalnum (static_cast<unsigned char> (c)) || c == '_' || c == '-')) {
			c = '_';
		}
	}
	return result;
}

inline std::string SanitizeUsdIdentifier (
	const std::string& input,
	size_t fallbackIndex,
	std::unordered_map<std::string, size_t>& counts)
{
	std::string name = input;
	if (name.empty ()) {
		name = "mesh";
	}
	for (char& c : name) {
		if (!(std::isalnum (static_cast<unsigned char> (c)) || c == '_')) {
			c = '_';
		}
	}
	if (!(std::isalpha (static_cast<unsigned char> (name[0])) || name[0] == '_')) {
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

inline std::string GetExtFromPath (const std::string& path)
{
	auto pos = path.rfind ('.');
	if (pos == std::string::npos) {
		return "";
	}
	std::string ext = path.substr (pos + 1);
	for (char& c : ext) {
		c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
	}
	return ext;
}

std::vector<EmbeddedTextureFile> ExtractEmbeddedTextures (
	aiScene* scene,
	const std::string& folderPrefix,
	std::unordered_map<const aiTexture*, std::string>* outNameByTexture = nullptr);

void RenameMaterialTextures (
	aiScene* scene,
	TextureNamingContext& naming,
	std::vector<EmbeddedTextureFile>& embeddedFiles);

void UpdateEmbeddedTextureFilenames (aiScene* scene, const TextureNamingContext& naming);
void CollectMaterialParts (const aiScene* scene, TextureNamingContext& ctx);
bool ReplaceFileListWithZip (const FileList& fileList, const std::string& zipName, Result& result);

bool ExportSceneObjCustom (
	const aiScene* scene,
	Result& result,
	const std::string& projectName,
	const MetadataOptions* metadata = nullptr);

bool ExportSceneUsd (
	const aiScene* scene,
	const std::string& format,
	Result& result,
	const std::string& projectName,
	const MetadataOptions* metadata = nullptr,
	bool mergeSharedPositions = false);

bool ExportScene (
	const aiScene* scene,
	const std::string& format,
	Result& result,
	const std::string& projectName,
	const MetadataOptions* metadata = nullptr,
	const std::string& inputFormat = "");

#endif
