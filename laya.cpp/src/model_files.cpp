#include "laya/model_files.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

namespace laya::model_files {
namespace {

struct entry {
    uint64_t offset = 0;  // of the first data byte, within the archive
    uint64_t size = 0;
};

struct archive_state {
    bool mounted = false;
    std::filesystem::path path;            // the archive file; also the virtual folder
    std::string root;                      // "" or "folder/": where the model starts inside it
    std::map<std::string, entry> entries;  // by name with '/' separators, without "./"
};

archive_state archive;
weight_codec_fn codec = nullptr;
void* codec_user = nullptr;

std::string shown(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// The full path of a file, for the codec; the path as given when it cannot be made absolute.
std::string full_path(const std::filesystem::path& path) {
    std::error_code failed;
    const auto absolute = std::filesystem::absolute(path, failed);
    return shown(failed ? path : absolute);
}

// ---- tar -----------------------------------------------------------------------------------

constexpr uint64_t block = 512;

uint64_t tar_number(const unsigned char* field, size_t width) {
    if (field[0] & 0x80) {  // base-256, used by GNU tar for sizes of 8 GiB and more
        uint64_t value = field[0] & 0x7f;
        for (size_t i = 1; i < width; i++) value = (value << 8) | field[i];
        return value;
    }
    uint64_t value = 0;
    size_t i = 0;
    while (i < width && field[i] == ' ') ++i;
    for (; i < width && field[i] >= '0' && field[i] <= '7'; i++) value = value * 8 + uint64_t(field[i] - '0');
    return value;
}

std::string tar_text(const unsigned char* field, size_t width) {
    size_t length = 0;
    while (length < width && field[length]) ++length;
    return std::string(reinterpret_cast<const char*>(field), length);
}

bool tar_checksum_ok(const unsigned char* header) {
    uint64_t sum = 0;
    for (size_t i = 0; i < block; i++) sum += i >= 148 && i < 156 ? uint64_t(' ') : header[i];
    return sum == tar_number(header + 148, 8);
}

// Forward slashes, no leading "./" or "/", no empty or "." parts. Empty when the name has a
// ".." part or names nothing.
std::string clean_name(const std::string& raw) {
    std::string out, part;
    auto flush = [&]() {
        if (part.empty() || part == ".") { part.clear(); return true; }
        if (part == "..") return false;
        if (!out.empty()) out += '/';
        out += part;
        part.clear();
        return true;
    };
    for (char ch : raw) {
        if (ch == '/' || ch == '\\') { if (!flush()) return {}; }
        else part += ch;
    }
    if (!flush()) return {};
    return out;
}

std::string base_name(const std::string& name) {
    const auto slash = name.find_last_of('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

// The "path" and "size" records of a pax extended header ("<length> <key>=<value>\n" ...).
void read_pax(const std::string& data, std::string& path, bool& has_size, uint64_t& size) {
    size_t at = 0;
    while (at < data.size()) {
        const auto space = data.find(' ', at);
        if (space == std::string::npos) break;
        const uint64_t length = std::strtoull(data.substr(at, space - at).c_str(), nullptr, 10);
        if (length == 0 || at + length > data.size()) break;
        const std::string record = data.substr(space + 1, at + length - space - 1);  // "key=value\n"
        const auto equals = record.find('=');
        if (equals != std::string::npos) {
            std::string value = record.substr(equals + 1);
            if (!value.empty() && value.back() == '\n') value.pop_back();
            const std::string key = record.substr(0, equals);
            if (key == "path") path = value;
            else if (key == "size") { size = std::strtoull(value.c_str(), nullptr, 10); has_size = true; }
        }
        at += length;
    }
}

std::map<std::string, entry> read_index(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open " + shown(path));
    const uint64_t total = uint64_t(stream.tellg());
    const std::string not_tar = shown(path) + " is not a folder and not an uncompressed tar archive";
    if (total < block) throw std::runtime_error(not_tar);

    std::map<std::string, entry> entries;
    std::string long_name, pax_path;
    bool pax_has_size = false;
    uint64_t pax_size = 0, at = 0;
    bool first = true;
    unsigned char header[block];
    while (at + block <= total) {
        stream.seekg(std::streamoff(at));
        stream.read(reinterpret_cast<char*>(header), block);
        if (!stream) throw std::runtime_error("Cannot read " + shown(path));
        if (std::all_of(header, header + block, [](unsigned char b) { return b == 0; })) break;  // end marker
        if (!tar_checksum_ok(header)) {
            if (first) throw std::runtime_error(not_tar);
            throw std::runtime_error("Damaged tar archive (bad header at byte " + std::to_string(at) + "): " + shown(path));
        }
        first = false;
        uint64_t size = tar_number(header + 124, 12);
        const char type = char(header[156]);
        const uint64_t data = at + block;
        if (type == 'L' || type == 'x' || type == 'g' || type == 'K') {
            // A record about the entry that follows: a GNU long name or a pax extended header.
            if (size > 1024 * 1024 || data + size > total) throw std::runtime_error("Damaged tar archive: " + shown(path));
            std::string text(size_t(size), '\0');
            stream.read(text.data(), std::streamsize(size));
            if (!stream) throw std::runtime_error("Cannot read " + shown(path));
            if (type == 'L') long_name = text.c_str();
            else if (type == 'x') read_pax(text, pax_path, pax_has_size, pax_size);
        } else {
            if (pax_has_size) size = pax_size;
            std::string name = !pax_path.empty() ? pax_path : long_name;
            if (name.empty()) {
                name = tar_text(header, 100);
                const bool posix = std::memcmp(header + 257, "ustar\0", 6) == 0;
                const std::string prefix = posix ? tar_text(header + 345, 155) : std::string();
                if (!prefix.empty()) name = prefix + "/" + name;
            }
            long_name.clear(); pax_path.clear(); pax_has_size = false;
            if (data + size > total) throw std::runtime_error("Truncated tar archive: " + shown(path));
            if (type == '0' || type == '\0' || type == '7') {
                name = clean_name(name);
                // "._name" entries are resource forks added by the macOS tar; they are not model files
                if (!name.empty() && base_name(name).rfind("._", 0) != 0) entries[name] = entry{data, size};
            }
        }
        at = data + (size + block - 1) / block * block;
    }
    if (first) throw std::runtime_error(not_tar);
    return entries;
}

// A model packed together with its folder ("laya/rl_agent_config.json", ...) is found too: when
// every entry lies below one top folder, that folder is where the model starts.
std::string single_top_folder(const std::map<std::string, entry>& entries) {
    std::string top;
    for (const auto& [name, unused] : entries) {
        (void)unused;
        const auto slash = name.find('/');
        if (slash == std::string::npos) return {};
        const std::string folder = name.substr(0, slash + 1);
        if (top.empty()) top = folder;
        else if (top != folder) return {};
    }
    return top;
}

// The entry a path names, or nullptr when the path is not inside the mounted archive.
// `inside` tells whether the path lies below the archive at all.
const entry* find_entry(const std::filesystem::path& path, bool& inside) {
    inside = false;
    if (!archive.mounted) return nullptr;
    const auto relative = path.lexically_relative(archive.path);
    if (relative.empty()) return nullptr;
    const auto first = relative.begin()->generic_u8string();
    if (first == u8"." || first == u8"..") return nullptr;  // the archive itself, or a path elsewhere
    inside = true;
    const auto text = relative.generic_u8string();
    const std::string name = clean_name(std::string(text.begin(), text.end()));
    if (name.empty()) return nullptr;
    const auto found = archive.entries.find(archive.root + name);
    return found == archive.entries.end() ? nullptr : &found->second;
}

bool is_weight_file(const std::string& name) {
    static const std::string suffix = ".safetensors";
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

void mount_archive(const std::filesystem::path& path) {
    archive_state next;
    next.entries = read_index(path);
    next.root = single_top_folder(next.entries);
    next.path = path;
    next.mounted = true;
    archive = std::move(next);
}

void unmount_archive() { archive = archive_state{}; }

void set_codec(weight_codec_fn decode, void* user) {
    codec = decode;
    codec_user = decode ? user : nullptr;
}

bool exists(const std::filesystem::path& path) {
    bool inside = false;
    if (find_entry(path, inside)) return true;
    if (inside) return false;
    std::error_code ignored;
    return std::filesystem::exists(path, ignored);
}

std::string read_text(const std::filesystem::path& path) {
    file source(path);
    if (source.size() > 1024ull * 1024 * 1024) throw std::runtime_error("File too large: " + shown(path));
    std::string text(size_t(source.size()), '\0');
    if (!source.read(0, text.data(), text.size())) throw std::runtime_error("Cannot read " + shown(path));
    return text;
}

file::file(const std::filesystem::path& path) {
    bool inside = false;
    const entry* found = find_entry(path, inside);
    const auto leaf = path.filename().u8string();
    name.assign(leaf.begin(), leaf.end());
    decode = is_weight_file(name);
    if (found) {
        stream.open(archive.path, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot open " + shown(archive.path));
        disk_path = full_path(archive.path);
        start = found->offset;
        length = found->size;
        return;
    }
    if (inside) throw std::runtime_error("Cannot open " + shown(path) + " (no such file in the archive)");
    stream.open(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open " + shown(path));
    length = uint64_t(stream.tellg());
    disk_path = full_path(path);
}

bool file::read(uint64_t offset, void* data, uint64_t size) {
    if (offset > length || size > length - offset) return false;
    if (size == 0) return true;
    stream.clear();
    stream.seekg(std::streamoff(start + offset));
    stream.read(static_cast<char*>(data), std::streamsize(size));
    if (!stream) return false;
    if (decode && codec) {
        const int status = codec(codec_user, disk_path.c_str(), name.c_str(), offset, data, size);
        if (status != 0)
            throw std::runtime_error("The codec reported error " + std::to_string(status) + " for " + name);
    }
    return true;
}

}  // namespace laya::model_files
