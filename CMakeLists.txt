cmake_minimum_required (VERSION 3.6)

set_property (GLOBAL PROPERTY USE_FOLDERS ON)

set (CMAKE_SUPPRESS_REGENERATION 1)
set (CMAKE_CONFIGURATION_TYPES Debug;ReleaseMini;ReleaseAll;ReleaseExporter;RelWithDebInfo;MinSizeRel)

add_definitions (-DUNICODE -D_UNICODE)

project (AssimpJS)

# Assimp

set (BUILD_SHARED_LIBS OFF CACHE BOOL "")
set (ASSIMP_BUILD_TESTS OFF CACHE BOOL "")
set (ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "")

if (CMAKE_BUILD_TYPE STREQUAL "ReleaseMini" OR CMAKE_BUILD_TYPE STREQUAL "ReleaseAll")
  # Enable all IMPORTERS by default, DISABLE all EXPORTERS by default
  set (ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT ON CACHE BOOL "")
  set (ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "")

  # Enable only essential EXPORTERS for import-focused builds
  set (ASSIMP_BUILD_ASSJSON_EXPORTER ON CACHE BOOL "")
  set (ASSIMP_BUILD_GLTF_EXPORTER ON CACHE BOOL "")

  ### SKIPPED IMPORTERS ###
  # M3D importer has undefined behaviour because of alignment issues, crashes and memory leaks
  # https://github.com/assimp/assimp/pull/4044
  set (ASSIMP_BUILD_M3D_IMPORTER OFF CACHE BOOL "")

  # Raw importer assertion failed because of a memory error
  set (ASSIMP_BUILD_RAW_IMPORTER OFF CACHE BOOL "")

  # Irrlicht importer doesn't work because of xml parsing error
  set (ASSIMP_BUILD_IRR_IMPORTER OFF CACHE BOOL "")
  set (ASSIMP_BUILD_IRRMESH_IMPORTER OFF CACHE BOOL "")

  # Terragen importer doesn't work
  set (ASSIMP_BUILD_TERRAGEN_IMPORTER OFF CACHE BOOL "")
  ### END SKIPPED IMPORTERS ###
endif()

if (CMAKE_BUILD_TYPE STREQUAL "ReleaseMini")
  # without X3D, decreases the wasm file size by 1.5mb
  set (ASSIMP_BUILD_X3D_IMPORTER OFF CACHE BOOL "")
  set (ASSIMP_BUILD_VRML_IMPORTER OFF CACHE BOOL "")

  # without IFC, decreases the wasm file size by 900kb
  set (ASSIMP_BUILD_IFC_IMPORTER OFF CACHE BOOL "")

  # without USD, decreases the wasm file size by 5.5mb
  set (ASSIMP_BUILD_USD_IMPORTER OFF CACHE BOOL "")
elseif (CMAKE_BUILD_TYPE STREQUAL "ReleaseAll")
  set (ASSIMP_BUILD_X3D_IMPORTER ON CACHE BOOL "")
  set (ASSIMP_BUILD_VRML_IMPORTER ON CACHE BOOL "")
  set (ASSIMP_BUILD_IFC_IMPORTER ON CACHE BOOL "")
  set (ASSIMP_BUILD_USD_IMPORTER ON CACHE BOOL "")
elseif (CMAKE_BUILD_TYPE STREQUAL "ReleaseExporter")
  # ReleaseExporter: MINIMAL IMPORTERS support (GLTF/GLB/ASSJSON only) but ALL EXPORTERS enabled
  
  # Disable ALL IMPORTERS by default, then selectively enable only what we need
  set (ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "")
  set (ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT ON CACHE BOOL "")
  
  # Enable only essential IMPORTERS for GLTF/GLB input and ASSJSON conversion
  set (ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "")    # For GLTF 1.0 & 2.0 + GLB support
  set (ASSIMP_BUILD_ASSJSON_IMPORTER ON CACHE BOOL "") # For internal JSON conversion
  
  # Enable most EXPORTERS (excluding problematic ones)
  set (ASSIMP_BUILD_PBRT_EXPORTER OFF CACHE BOOL "")  # Not required
  set (ASSIMP_BUILD_M3D_EXPORTER OFF CACHE BOOL "")  # Disabled due memory leaks
  set (ASSIMP_BUILD_ASSBIN_EXPORTER OFF CACHE BOOL "")  # Not a true file format
  set (ASSIMP_BUILD_ASSXML_EXPORTER OFF CACHE BOOL "")  # Not a true file format
endif()

if (${EMSCRIPTEN})
  # Set global flags so assimp library gets optimized
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG -flto -funroll-loops -finline-functions -fomit-frame-pointer")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -DNDEBUG -flto -funroll-loops -finline-functions -fomit-frame-pointer")
endif()

# build zlib from sources instead of looking for system zlib
set (ASSIMP_BUILD_ZLIB ON CACHE BOOL "")

add_subdirectory (assimp)

if (${EMSCRIPTEN})
	target_compile_options (assimp PUBLIC -fexceptions -Wno-unused-but-set-variable -Wno-unused-const-variable)
endif ()

# AssimpJS

set (AssimpJSSourcesFolder assimpjs/src)
file (GLOB
	AssimpJSSourceFiles
	${AssimpJSSourcesFolder}/*.hpp
	${AssimpJSSourcesFolder}/*.cpp
)
source_group ("Sources" FILES ${AssimpJSSourceFiles})

if (${EMSCRIPTEN})
	add_executable (AssimpJS ${AssimpJSSourceFiles})
	target_compile_options (AssimpJS PUBLIC "$<$<CONFIG:Debug>:-gsource-map>")
	target_compile_options (AssimpJS PUBLIC -fexceptions)

  # LTO for linking - AssimpJS target only
  target_link_options(AssimpJS PRIVATE -flto)

	target_link_options (AssimpJS PUBLIC -sMODULARIZE=1)
	target_link_options (AssimpJS PUBLIC -sEXPORT_NAME=assimpjs)
	target_link_options (AssimpJS PUBLIC -sNO_DISABLE_EXCEPTION_CATCHING)

	# Production: Disable assertions for smaller WASM size (enable -sASSERTIONS=1 for debugging)
	# target_link_options (AssimpJS PUBLIC -sASSERTIONS=1 -sSAFE_HEAP=1 -sWARN_UNALIGNED=1)
	target_link_options (AssimpJS PUBLIC -sASSERTIONS=0)            # Disable assertions for production
	
	# Javascript size optimizations
	target_link_options (AssimpJS PUBLIC --closure=1)               # Google Closure Compiler optimization
	
	# Dynamic memory management for variable file sizes with increased stack for large IFC files
	target_link_options (AssimpJS PUBLIC -sALLOW_MEMORY_GROWTH=1)   # Allow unlimited memory growth as needed
	target_link_options (AssimpJS PUBLIC -sSTACK_SIZE=8MB)       # Increase stack size to 8MB for large IFC files
	
  # Remove unnecessary runtime features (keeping safe ones)
	target_link_options (AssimpJS PUBLIC -sSUPPORT_LONGJMP=0)
	target_link_options (AssimpJS PUBLIC -sNO_EXIT_RUNTIME=1)       # Don't include exit handling
	
	# Essential flags for compatibility
	target_link_options (AssimpJS PUBLIC -sINVOKE_RUN=0)            # Don't auto-run
	target_link_options (AssimpJS PUBLIC --bind)                    # Enable embind
	target_link_options (AssimpJS PUBLIC -sWASM_BIGINT)
	target_link_options (AssimpJS PUBLIC -sLLD_REPORT_UNDEFINED)
	target_link_options (AssimpJS PUBLIC -sNO_FILESYSTEM=0)
else ()
	add_library (AssimpJS ${AssimpJSSourceFiles})
endif ()

target_link_libraries (AssimpJS assimp)
if (CMAKE_BUILD_TYPE STREQUAL "ReleaseMini")
  set_target_properties(AssimpJS PROPERTIES OUTPUT_NAME assimpjs-mini)
elseif (CMAKE_BUILD_TYPE STREQUAL "ReleaseAll")
  set_target_properties(AssimpJS PROPERTIES OUTPUT_NAME assimpjs-all)
elseif (CMAKE_BUILD_TYPE STREQUAL "ReleaseExporter")
  set_target_properties(AssimpJS PROPERTIES OUTPUT_NAME assimpjs-exporter)
endif()
set_target_properties (AssimpJS PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}")

# AssimpJSTest

if (${EMSCRIPTEN})
else ()
	set (AssimpJSExampleSourcesFolder assimpjs/example)
	file (GLOB
		AssimpJSExampleSourceFiles
		${AssimpJSExampleSourcesFolder}/*.hpp
		${AssimpJSExampleSourcesFolder}/*.cpp
	)
	source_group ("Sources" FILES ${AssimpJSExampleSourceFiles})
	add_executable (AssimpJSExample ${AssimpJSExampleSourceFiles})
	target_include_directories (AssimpJSExample PUBLIC ${AssimpJSSourcesFolder})
	target_link_libraries (AssimpJSExample AssimpJS)
	set_target_properties (AssimpJSExample PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}")
endif ()
