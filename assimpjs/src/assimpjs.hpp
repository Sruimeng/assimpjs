#ifndef ASSIMPJS_HPP
#define ASSIMPJS_HPP

#ifdef EMSCRIPTEN
#include <emscripten/bind.h>
#endif

#include "filelist.hpp"
#include "fileio.hpp"
#include "result.hpp"

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <assimp/matrix4x4.h>

struct MetadataOptions
{
	// Optional root transform. If empty => ignore
	std::vector<float> rootTransform;

	// Optional per-node 4x4 transform (name -> 16 floats)
	std::unordered_map<std::string, std::vector<float>> childTransforms;

	// Optional renames (old -> new)
	std::unordered_map<std::string, std::string> childRenames;

	// Optional deletions (node names)
	std::unordered_set<std::string> childDeleted;

	// Optional PBR factors
	bool hasMaterialFactors = false;
	float metallic = 0.0f;
	float roughness = 0.5f;

	// Optional custom generator branding driven by task id
	std::string taskId;
};

using NodeTransformMap = std::unordered_map<std::string, aiMatrix4x4>;

Result ConvertFile (
	const File& file,
	const std::string& format,
	const FileLoader& loader,
	const MetadataOptions* metadata = nullptr,
	const std::string& projectName = std::string ());

Result ConvertFileList (
	const FileList& fileList,
	const std::string& format,
	const MetadataOptions* metadata = nullptr,
	const std::string& projectName = std::string ());

Result ConvertFileListWithTransform (
	const FileList& fileList,
	const std::string& format,
	const std::vector<float>& matrix16,
	const MetadataOptions* metadata = nullptr,
	const std::string& projectName = std::string ());

Result ConvertFileListWithNodeTransforms (
	const FileList& fileList,
	const std::string& format,
	const NodeTransformMap& transformByName,
	const MetadataOptions* metadata = nullptr,
	const std::string& projectName = std::string ());

#endif
