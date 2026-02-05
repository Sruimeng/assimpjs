# Test Fixtures

- `BoxTextured-meshopt-nofallback.glb` is generated from
  `assimp/test/models/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb` using
  `gltf-transform optimize --compress meshopt` and then repackaged to remove
  the unused fallback buffer so it can load in the meshopt importer tests.
