#include "shader_lab/lab.hpp"
#define XXH_INLINE_ALL
#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <xxhash.h>
#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace sl {
std::string path_text(const fs::path &p) {
    auto s = p.generic_u8string();
    return {s.begin(), s.end()};
}
fs::path path_from(std::string_view p) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(p.data()), p.size()));
}
bool contains(Bytes b, uint64_t o, uint64_t n) {
    return o <= b.size() && n <= b.size() - o;
}
uint64_t integer(Bytes b, uint64_t o, unsigned w) {
    if (w > 8 || !contains(b, o, w))
        throw std::runtime_error("truncated integer");
    uint64_t v = 0;
    for (unsigned i = 0; i < w; ++i)
        v |= uint64_t(b[size_t(o + i)]) << (8 * i);
    return v;
}
std::string hex(uint64_t v, unsigned w) {
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(int(w)) << v;
    return s.str();
}
MappedFile::MappedFile(const fs::path &p) {
#ifdef _WIN32
    auto f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open file: " + path_text(p));
    file_ = f;
    LARGE_INTEGER n{};
    if (!GetFileSizeEx(f, &n) || n.QuadPart < 0 || uint64_t(n.QuadPart) > SIZE_MAX) {
        CloseHandle(f);
        file_ = nullptr;
        throw std::runtime_error("invalid file size");
    }
    size_ = size_t(n.QuadPart);
    if (!size_)
        return;
    mapping_ = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_)
        data_ = static_cast<const uint8_t *>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    if (!data_) {
        if (mapping_)
            CloseHandle(mapping_);
        CloseHandle(f);
        mapping_ = file_ = nullptr;
        throw std::runtime_error("cannot map file");
    }
#else
    fd_ = open(p.c_str(), O_RDONLY);
    struct stat s{};
    if (fd_ < 0 || fstat(fd_, &s) || s.st_size < 0) {
        if (fd_ >= 0)
            close(fd_);
        fd_ = -1;
        throw std::runtime_error("cannot open file");
    }
    size_ = size_t(s.st_size);
    if (!size_)
        return;
    auto m = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (m == MAP_FAILED) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("cannot map file");
    }
    data_ = static_cast<const uint8_t *>(m);
#endif
}
MappedFile::~MappedFile() {
#ifdef _WIN32
    if (data_)
        UnmapViewOfFile(data_);
    if (mapping_)
        CloseHandle(mapping_);
    if (file_)
        CloseHandle(file_);
#else
    if (data_)
        munmap(const_cast<uint8_t *>(data_), size_);
    if (fd_ >= 0)
        close(fd_);
#endif
}
std::vector<uint8_t> read_bytes(const fs::path &p, uint64_t limit) {
    MappedFile f(p);
    auto b = f.bytes();
    if (b.size() > limit)
        throw std::runtime_error("input exceeds bounded read limit");
    return {b.begin(), b.end()};
}
void write_bytes(const fs::path &p, Bytes b) {
    if (!p.parent_path().empty())
        fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
    f.close();
    if (!f)
        throw std::runtime_error("write failed: " + path_text(p));
}
void write_text(const fs::path &p, std::string_view s) {
    write_bytes(p, {reinterpret_cast<const uint8_t *>(s.data()), s.size()});
}
void atomic_json(const fs::path &p, const json &j) {
    auto temp = p;
    temp += ".tmp";
    write_text(temp, j.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("atomic report replacement failed");
#else
    fs::rename(temp, p);
#endif
}
json read_json(const fs::path &p) {
    std::ifstream f(p);
    if (!f)
        throw std::runtime_error("cannot read JSON: " + path_text(p));
    json j;
    f >> j;
    return j;
}
std::string xxh3(Bytes b) {
    return hex(XXH3_64bits(b.data(), b.size()), 16);
}
// SHA-256 content identity, independently checked against standard vectors in tests.
std::string sha256(Bytes b) {
#ifdef _WIN32
    // Use the platform's optimized SHA-256 for file-sized inputs; retain the portable
    // implementation for small records and non-Windows builds. Identity is unchanged.
    if (b.size() >= 4096) {
        struct Provider {
            BCRYPT_ALG_HANDLE handle = nullptr;
            Provider() {
                if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
                    throw std::runtime_error("SHA-256 provider initialization failed");
            }
            ~Provider() {
                BCryptCloseAlgorithmProvider(handle, 0);
            }
        };
        static const Provider provider;
        struct Hash {
            BCRYPT_HASH_HANDLE handle = nullptr;
            ~Hash() {
                if (handle)
                    BCryptDestroyHash(handle);
            }
        } hash;
        if (BCryptCreateHash(provider.handle, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("SHA-256 hash initialization failed");
        size_t offset = 0;
        while (offset < b.size()) {
            const auto length = ULONG(std::min<size_t>(b.size() - offset, 1024 * 1024 * 1024));
            if (BCryptHashData(hash.handle, const_cast<PUCHAR>(b.data() + offset), length, 0) < 0)
                throw std::runtime_error("SHA-256 hashing failed");
            offset += length;
        }
        std::array<uint8_t, 32> digest{};
        if (BCryptFinishHash(hash.handle, digest.data(), ULONG(digest.size()), 0) < 0)
            throw std::runtime_error("SHA-256 finalization failed");
        std::string result;
        for (auto byte : digest)
            result += hex(byte, 2);
        return result;
    }
#endif
    constexpr std::array<uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto block = [&](const uint8_t *p) {
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 |
                   uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            auto x = w[i - 15], y = w[i - 2];
            w[i] = w[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + w[i - 7] +
                   (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
        }
        auto [a, c, d, e, f, g, j, l] = h;
        for (int i = 0; i < 64; ++i) {
            auto t = l + (std::rotr(f, 6) ^ std::rotr(f, 11) ^ std::rotr(f, 25)) +
                     ((f & g) ^ (~f & j)) + k[i] + w[i];
            auto t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
                      ((a & c) ^ (a & d) ^ (c & d));
            l = j;
            j = g;
            g = f;
            f = e + t;
            e = d;
            d = c;
            c = a;
            a = t + t2;
        }
        std::array<uint32_t, 8> v = {a, c, d, e, f, g, j, l};
        for (int i = 0; i < 8; ++i)
            h[i] += v[i];
    };
    size_t offset = 0;
    while (b.size() - offset >= 64) {
        block(b.data() + offset);
        offset += 64;
    }
    uint8_t tail[128]{};
    auto remain = b.size() - offset;
    for (size_t i = 0; i < remain; ++i)
        tail[i] = b[offset + i];
    tail[remain] = 0x80;
    auto n = remain < 56 ? 64 : 128;
    auto bits = uint64_t(b.size()) * 8;
    for (int i = 0; i < 8; ++i)
        tail[n - 1 - i] = uint8_t(bits >> (8 * i));
    block(tail);
    if (n == 128)
        block(tail + 64);
    std::string out;
    for (auto v : h)
        out += hex(v, 8);
    return out;
}
std::string hash_file(const fs::path &p) {
    MappedFile f(p);
    return sha256(f.bytes());
}
std::string html_escape(std::string_view s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&':
            o += "&amp;";
            break;
        case '<':
            o += "&lt;";
            break;
        case '>':
            o += "&gt;";
            break;
        case '\"':
            o += "&quot;";
            break;
        case '\'':
            o += "&#39;";
            break;
        default:
            o += c;
        }
    }
    return o;
}
bool is_within(const fs::path &p, const fs::path &root) {
    auto a = fs::weakly_canonical(p), b = fs::weakly_canonical(root);
    auto ai = a.begin();
    for (auto bi = b.begin(); bi != b.end(); ++bi, ++ai) {
        if (ai == a.end())
            return false;
#ifdef _WIN32
        if (_wcsicmp(ai->c_str(), bi->c_str()) != 0)
            return false;
#else
        if (*ai != *bi)
            return false;
#endif
    }
    return true;
}
} // namespace sl
