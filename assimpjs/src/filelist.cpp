#include "filelist.hpp"

static std::string ToLowercase (const std::string& str)
{
	std::string res = str;
	for (char& c : res) {
		c = std::tolower (c);
	}
	return res;
}

File::File () :
	path (),
	content ()
{
}

File::File (const std::string& path, const Buffer& content) :
	path (path),
	content (content)
{
}

const std::string& File::GetPath () const
{
	return path;
}

#ifdef EMSCRIPTEN

emscripten::val File::GetContentEmscripten () const
{
	emscripten::val Uint8Array = emscripten::val::global ("Uint8Array");
	return Uint8Array.new_ (emscripten::typed_memory_view (content.size (), content.data ()));
}

#endif

FileList::FileList () :
	files ()
{
}

void FileList::AddFile (const std::string& path, const Buffer& content)
{
	files.push_back (File (path, content));
}

size_t FileList::FileCount () const
{
	return files.size ();
}

File& FileList::GetFile (size_t index)
{
	return files[index];
}

File* FileList::GetFile (const std::string& path)
{
	std::string fileName = GetFileName (path);
	for (File& file : files) {
		std::string currFileName = GetFileName (file.path);
		if (currFileName == fileName) {
			return &file;
		}
	}
	return nullptr;
}

const File& FileList::GetFile (size_t index) const
{
	return const_cast<FileList*> (this)->GetFile (index);
}

const File* FileList::GetFile (const std::string& path) const
{
	return const_cast<FileList*> (this)->GetFile (path);
}

#ifdef EMSCRIPTEN

void FileList::AddFileEmscripten (const std::string& path, const emscripten::val& content)
{
	// 优化：使用 typed_memory_view 零拷贝，避免 vecFromJSArray 逐字节跨界
	// 检查输入是否为 Uint8Array
	emscripten::val Uint8Array = emscripten::val::global("Uint8Array");
	bool isUint8Array = content.instanceof(Uint8Array);

	if (isUint8Array) {
		// 快速路径：直接从 Uint8Array 批量拷贝
		unsigned int length = content["length"].as<unsigned int>();
		Buffer contentArr(length);

		// 使用 Emscripten 的内存视图 API 进行单次批量拷贝
		emscripten::val memory = emscripten::val::module_property("HEAPU8");
		unsigned int offset = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(contentArr.data()));
		emscripten::val memoryView = memory.call<emscripten::val>("subarray", offset, offset + length);
		memoryView.call<void>("set", content);

		AddFile(path, contentArr);
	} else {
		// 降级路径：普通 JS 数组，使用原有逐元素拷贝
		Buffer contentArr = emscripten::vecFromJSArray<std::uint8_t>(content);
		AddFile(path, contentArr);
	}
}

#endif

std::string GetFileName (const std::string& path)
{
	size_t lastSeparator = path.find_last_of ('/');
	if (lastSeparator == std::string::npos) {
		lastSeparator = path.find_last_of ('\\');
	}
	if (lastSeparator == std::string::npos) {
		return ToLowercase (path);
	}
	std::string fileName = path.substr (lastSeparator + 1, path.length () - lastSeparator - 1);
	return ToLowercase (fileName);
}
