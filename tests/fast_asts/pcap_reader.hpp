#pragma once

// Minimal reader for classic pcap files (Ethernet, micro- or nanosecond
// timestamps): hands every IPv4 UDP datagram to a callback. Test tooling only.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pcap {

struct Datagram {
    std::uint64_t time_ns;
    std::uint32_t destination; // host order
    std::uint16_t port;
    const std::uint8_t* data;
    std::size_t size;
};

inline std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}
inline std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} | std::uint32_t{p[1]} << 8 | std::uint32_t{p[2]} << 16 | std::uint32_t{p[3]} << 24;
}

struct File {
    const std::uint8_t* data{};
    std::size_t size{};

    bool open(const char* path) {
        const int fd = ::open(path, O_RDONLY);
        struct stat info {};
        if (fd < 0 || ::fstat(fd, &info) != 0) {
            std::perror(path);
            return false;
        }
        size = static_cast<std::size_t>(info.st_size);
        void* mapping = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (mapping == MAP_FAILED) {
            std::perror("mmap");
            return false;
        }
        data = static_cast<const std::uint8_t*>(mapping);
        return true;
    }

    template <class Handler> bool for_each(Handler&& handler) const {
        if (size < 24)
            return false;
        const std::uint32_t magic = le32(data);
        const bool nanoseconds = magic == 0xA1B23C4Du;
        if (!nanoseconds && magic != 0xA1B2C3D4u) {
            std::fprintf(stderr, "unsupported capture format (magic %08x)\n", magic);
            return false;
        }
        if (le32(data + 20) != 1) {
            std::fprintf(stderr, "unsupported link type %u\n", le32(data + 20));
            return false;
        }
        std::size_t offset = 24;
        while (offset + 16 <= size) {
            const std::uint64_t time_ns = std::uint64_t{le32(data + offset)} * 1'000'000'000ull +
                                          std::uint64_t{le32(data + offset + 4)} * (nanoseconds ? 1 : 1000);
            const std::uint32_t captured = le32(data + offset + 8);
            const std::uint8_t* frame = data + offset + 16;
            offset += 16 + std::size_t{captured};
            if (offset > size)
                break;
            std::size_t at = 12;
            while (at + 2 <= captured && be16(frame + at) == 0x8100)
                at += 4;
            if (at + 2 > captured || be16(frame + at) != 0x0800)
                continue;
            at += 2;
            if (at + 20 > captured || frame[at + 9] != 17)
                continue;
            const std::uint32_t destination = be32(frame + at + 16);
            at += std::size_t{frame[at] & 0x0Fu} * 4;
            if (at + 8 > captured)
                continue;
            const std::size_t length = be16(frame + at + 4);
            if (length < 8 || at + length > captured)
                continue;
            handler(Datagram{time_ns, destination, be16(frame + at + 2), frame + at + 8, length - 8});
        }
        return true;
    }
};

inline std::string address(std::uint32_t destination, std::uint16_t port) {
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u:%u", destination >> 24, (destination >> 16) & 255,
                  (destination >> 8) & 255, destination & 255, port);
    return text;
}

} // namespace pcap
