#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <iostream>
#include <fstream>
#include <iomanip>

struct DiagReport {
  bool valid_magic = false;
  uint32_t version = 0;
  bool import_success = false;
  std::string error_msg;
  uint32_t num_meshes = 0;
  uint32_t num_materials = 0;
  uint32_t num_animations = 0;
  uint32_t total_vertices = 0;
  uint32_t total_faces = 0;
  bool has_bones = false;
  bool has_pbr = false;
};

bool checkMagic(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;

  char magic[23];
  file.read(magic, 23);
  return file.gcount() == 23 &&
         std::string(magic, 21) == "Kaydara FBX Binary  ";
}

uint32_t readVersion(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return 0;

  file.seekg(23);
  uint32_t version;
  file.read(reinterpret_cast<char*>(&version), 4);
  return version;
}

bool hasBones(const aiScene* scene) {
  if (!scene) return false;
  for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
    if (scene->mMeshes[i]->mNumBones > 0) return true;
  }
  return false;
}

bool hasPBR(const aiScene* scene) {
  if (!scene) return false;
  for (uint32_t i = 0; i < scene->mNumMaterials; ++i) {
    aiMaterial* mat = scene->mMaterials[i];
    aiString name;
    if (mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
      std::string n(name.C_Str());
      if (n.find("PBR") != std::string::npos ||
          n.find("pbr") != std::string::npos) return true;
    }
    float metallic, roughness;
    if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS ||
        mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
      return true;
    }
  }
  return false;
}

void outputJSON(const DiagReport& report) {
  std::cout << "{\n";
  std::cout << "  \"valid_magic\": " << (report.valid_magic ? "true" : "false") << ",\n";
  std::cout << "  \"version\": " << report.version << ",\n";
  std::cout << "  \"import_success\": " << (report.import_success ? "true" : "false") << ",\n";
  std::cout << "  \"error_msg\": \"" << report.error_msg << "\",\n";
  std::cout << "  \"num_meshes\": " << report.num_meshes << ",\n";
  std::cout << "  \"num_materials\": " << report.num_materials << ",\n";
  std::cout << "  \"num_animations\": " << report.num_animations << ",\n";
  std::cout << "  \"total_vertices\": " << report.total_vertices << ",\n";
  std::cout << "  \"total_faces\": " << report.total_faces << ",\n";
  std::cout << "  \"has_bones\": " << (report.has_bones ? "true" : "false") << ",\n";
  std::cout << "  \"has_pbr\": " << (report.has_pbr ? "true" : "false") << "\n";
  std::cout << "}\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <fbx_file>\n";
    return 1;
  }

  DiagReport report;
  std::string path(argv[1]);

  report.valid_magic = checkMagic(path);
  report.version = readVersion(path);

  Assimp::Importer importer;
  const aiScene* scene = importer.ReadFile(path,
    aiProcess_Triangulate |
    aiProcess_GenNormals |
    aiProcess_ValidateDataStructure);

  if (!scene) {
    report.error_msg = importer.GetErrorString();
    outputJSON(report);
    return 0;
  }

  report.import_success = true;
  report.num_meshes = scene->mNumMeshes;
  report.num_materials = scene->mNumMaterials;
  report.num_animations = scene->mNumAnimations;
  report.has_bones = hasBones(scene);
  report.has_pbr = hasPBR(scene);

  for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
    aiMesh* mesh = scene->mMeshes[i];
    report.total_vertices += mesh->mNumVertices;
    report.total_faces += mesh->mNumFaces;
  }

  outputJSON(report);
  return 0;
}
