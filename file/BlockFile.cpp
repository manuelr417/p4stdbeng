#include "file/BlockFile.h"

#include "common/Error.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace bufman {
namespace {

[[noreturn]] void fail_errno(const std::string& action) {
    throw common::Error(action + ": " + std::strerror(errno));
}

// write()/read() are allowed to transfer fewer bytes than requested (e.g. if
// a signal interrupts the call), so both helpers loop until the full block
// has been transferred, retrying on EINTR and treating a zero-byte result
// as an error rather than looping forever.
void write_all(int fd, const char* buffer, std::size_t length) {
    std::size_t completed = 0;
    while (completed < length) {
        const ssize_t written = ::write(fd, buffer + completed, length - completed);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0) {
            fail_errno("write");
        }
        if (written == 0) {
            throw common::Error("write: no progress");
        }
        completed += static_cast<std::size_t>(written);
    }
}

void read_all(int fd, char* buffer, std::size_t length) {
    std::size_t completed = 0;
    while (completed < length) {
        const ssize_t read_count = ::read(fd, buffer + completed, length - completed);
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        if (read_count < 0) {
            fail_errno("read");
        }
        if (read_count == 0) {
            throw common::Error("read: unexpected end of file");
        }
        completed += static_cast<std::size_t>(read_count);
    }
}

void seek_block(int fd, std::uint64_t block_number) {
    constexpr std::uint64_t max_off = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (block_number > max_off / kBlockSize) {
        throw common::Error("block offset exceeds off_t range");
    }
    const off_t offset = static_cast<off_t>(block_number * kBlockSize);
    if (::lseek(fd, offset, SEEK_SET) == static_cast<off_t>(-1)) {
        fail_errno("lseek");
    }
}

// Invariant maintained by this module: the file is always made up of
// complete kBlockSize blocks.

}

File open_for_append(const std::string& path) {
    File file{::open(path.c_str(), O_RDWR | O_CREAT, 0644)};
    if (file.fd < 0) {
        fail_errno("open for append");
    }
    return file;
}

File open_for_read(const std::string& path) {
    File file{::open(path.c_str(), O_RDONLY)};
    if (file.fd < 0) {
        fail_errno("open for read");
    }
    return file;
}

void close(File& file) {
    if (file.fd >= 0) {
        ::close(file.fd);
        file.fd = -1;
    }
}

std::uint64_t block_count(File& file) {
    assert(file.fd >= 0 && "block_count on a closed file");
    const off_t size = ::lseek(file.fd, 0, SEEK_END);
    if (size == static_cast<off_t>(-1)) {
        fail_errno("lseek to end");
    }
    if (size % static_cast<off_t>(kBlockSize) != 0) {
        throw common::Error("binary file size is not a multiple of 4096 bytes");
    }
    return static_cast<std::uint64_t>(size / static_cast<off_t>(kBlockSize));
}

void read_block(File& file, std::uint64_t block_number,
                std::array<char, kBlockSize>& block) {
    read_block(file, block_number, block.data());
}

void read_block(File& file, std::uint64_t block_number, char* buffer) {
    assert(file.fd >= 0 && "read_block on a closed file");
    if (block_number >= block_count(file)) {
        throw common::Error("requested block is beyond end of file");
    }
    seek_block(file.fd, block_number);
    read_all(file.fd, buffer, kBlockSize);
}

void write_block(File& file, std::uint64_t block_number, const char* buffer) {
    assert(file.fd >= 0 && "write_block on a closed file");
    seek_block(file.fd, block_number);
    write_all(file.fd, buffer, kBlockSize);
}

}
