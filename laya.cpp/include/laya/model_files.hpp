#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

// How the files of a model are read.
//
// By default a path names an ordinary file. While an archive is mounted, a path below the
// archive's own path names an entry of that archive: with model.tar mounted, the path
// model.tar/tokenizer/tokenizer.json is the entry tokenizer/tokenizer.json. The engine therefore
// treats an archive as it treats a folder.
//
// A codec, when one is set, decodes the bytes of weight files (*.safetensors) after they are
// read, whether the file is in a folder or in an archive. Other files are never passed to it.
//
// The mounted archive and the codec are process-wide and are meant to be set for the duration of
// one model load, by a caller that serializes loads (the C API does).
namespace laya {

// Decodes `size` bytes in place. `path_utf8` is the file on disk the bytes were read from: the
// weight file itself for a model folder, the archive for a model in an archive. `file_name_utf8`
// is the weight file's name without folders. `offset` is the position of data[0] within the
// weight file, counted from the start of that file (not of an archive around it). The decoded
// data has the same length as the stored data. Returns 0 on success; any other value fails the
// load.
using weight_codec_fn = int (*)(void* user, const char* path_utf8, const char* file_name_utf8, uint64_t offset,
                                void* data, uint64_t size);

namespace model_files {

// Reads the table of contents of an uncompressed tar archive and mounts it. Throws
// std::runtime_error when the file is not such an archive.
void mount_archive(const std::filesystem::path& archive);
void unmount_archive();

void set_codec(weight_codec_fn decode, void* user);  // nullptr removes it

bool exists(const std::filesystem::path& path);

// The whole file as text. Throws std::runtime_error when it cannot be read.
std::string read_text(const std::filesystem::path& path);

// A file read at arbitrary positions.
class file {
public:
    explicit file(const std::filesystem::path& path);  // throws std::runtime_error when it cannot be opened
    uint64_t size() const { return length; }
    // Reads `size` bytes at `offset`. Returns false when the file is shorter than that or the
    // read fails. Throws std::runtime_error when the codec reports an error.
    bool read(uint64_t offset, void* data, uint64_t size);

private:
    std::ifstream stream;
    std::string name;      // file name without folders, as given to the codec
    std::string disk_path; // the file on disk that is read: the file itself, or the archive
    uint64_t start = 0;    // where the file begins in the stream
    uint64_t length = 0;
    bool decode = false;
};

}  // namespace model_files
}  // namespace laya
