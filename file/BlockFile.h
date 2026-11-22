#ifndef EX07_BLOCK_FILE_H
#define EX07_BLOCK_FILE_H

#include "bufpool/DataFrame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bufman {

// The file layer's block size: every file is a sequence of 4096-byte
// blocks, and read_block/write_block take exactly this buffer size.
constexpr std::size_t kBlockSize = 4096;

struct File {
    int fd = -1;
};

// Block-level I/O only; the meaning of the bytes inside a block is the
// caller's concern (see heapfile/SlottedPage). I/O failures throw
// common::Error; calling on a closed file is a contract violation.
File open_for_append(const std::string& path);
File open_for_read(const std::string& path);
void close(File& file);
void read_block(File& file, std::uint64_t block_number,
                std::array<char, kBlockSize>& block);
void read_block(File& file, std::uint64_t block_number, char* buffer);
void write_block(File& file, std::uint64_t block_number, const char* buffer);
std::uint64_t block_count(File& file);

}

#endif // EX07_BLOCK_FILE_H
