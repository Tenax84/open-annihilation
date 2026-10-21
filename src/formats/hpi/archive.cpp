// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/hpi.hpp"

#include "oa/base/threads.hpp"

#include "oa/formats/sqsh.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <unordered_set>

namespace oa {
namespace {

constexpr uint64_t kMaxDirectorySize = 256ULL << 20;
constexpr uint32_t kMaxStoredChunkSize = 1U << 20;
constexpr std::size_t kMaxEntries = 1'000'000;
constexpr std::size_t kMaxDirectoryDepth = 128;
constexpr std::size_t kWildcardStates = 100;
constexpr uint8_t kSquashTypeLimit = 4;
constexpr uint8_t kSquashLz77 = 1;
constexpr uint8_t kSquashZlib = 2;

namespace squash_field {
constexpr std::size_t type = 0x5, scrambled = 0x6, packed_size = 0x7, unpacked_size = 0xb,
                      checksum = 0xf;
} // namespace squash_field

constexpr const char* kSquashNames[] = {
    "SQUASHERR_OK",
    "SQUASHERR_BADHEADER",
    "SQUASHERR_BADCHECKSUM",
    "SQUASHERR_BADUNPACKSIZE",
    "SQUASHERR_BADUNPACKTYPE",
    "SQUASHERR_BADPACKTYPE",
    "SQUASHERR_BADPARAMS",
};

using base::bytes::DecodeCode;
using base::bytes::DecodeError;
using base::bytes::Decoded;
using base::bytes::load_le32;

char ascii_upper(char value) noexcept {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A')) : value;
}

bool equal_nocase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (ascii_upper(a[i]) != ascii_upper(b[i]))
            return false;
    return true;
}

// Archive data transform `out = position ^ ~in ^ key`; a zero key is a no-op.
void decrypt_at(std::span<uint8_t> bytes, uint8_t key, uint64_t position) noexcept {
    if (key == 0)
        return;
    // A plain pointer keeps the per-byte loop free of calls in unoptimised builds.
    uint8_t* const data = bytes.data();
    const std::size_t size = bytes.size();
    for (std::size_t i = 0; i < size; ++i)
        data[i] = static_cast<uint8_t>(
            static_cast<uint8_t>(position + i) ^ static_cast<uint8_t>(~data[i]) ^ key
        );
}

/// Returns whether a step of opening an archive failed.
///
/// @param error the step's error; code none when it succeeded
/// @return true when it holds an error
bool failed(const DecodeError& error) noexcept {
    return error.code != DecodeCode::none;
}

} // namespace

uint8_t hpi_archive_key(uint8_t header_key) noexcept {
    if (header_key == 0)
        return 0;
    const auto promoted = static_cast<uint32_t>(header_key);
    return static_cast<uint8_t>(~((promoted >> 6U) | (promoted << 2U)));
}

int uncompress_legacy(
    std::span<uint8_t> output, uint32_t* length, std::span<const uint8_t> input
) noexcept {
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(std::min<std::size_t>(*length, output.size()));
    int status = inflateInit(&stream);
    if (status != Z_OK)
        return status;
    status = inflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END) {
        inflateEnd(&stream);
        // A stream that has not reached its end is never a success, even
        // where inflate reports progress.
        return status == Z_OK ? Z_BUF_ERROR : status;
    }
    *length = static_cast<uint32_t>(stream.total_out);
    return inflateEnd(&stream);
}

namespace {

// The two bytes that open a zlib stream, and the parts of them read here.
constexpr std::size_t kZlibHeaderBytes = 2;
constexpr unsigned kZlibMethodMask = 0x0FU;
constexpr unsigned kZlibDeflateMethod = 8;
constexpr unsigned kZlibWindowShift = 4;
constexpr unsigned kZlibLargestWindow = 7;
constexpr unsigned kZlibPresetDictionary = 0x20U;
constexpr unsigned kZlibHeaderCheck = 31;
// Inflates deflate data with no zlib header or check value around it.
constexpr int kRawDeflateWindowBits = -15;

// Reads a zlib stream that its packing tool wrote without the four-byte
// check value at its end: the two-byte header is valid, the deflate data
// ends at the input's last byte, and it wrote exactly `expected` bytes.
bool inflate_without_check_value(
    std::span<uint8_t> output, uint32_t expected, std::span<const uint8_t> input
) noexcept {
    if (input.size() <= kZlibHeaderBytes || expected > output.size())
        return false;
    const unsigned method = input[0];
    const unsigned flags = input[1];
    if ((method & kZlibMethodMask) != kZlibDeflateMethod ||
        (method >> kZlibWindowShift) > kZlibLargestWindow || (flags & kZlibPresetDictionary) != 0 ||
        ((method << 8U) | flags) % kZlibHeaderCheck != 0)
        return false;
    const auto deflated = input.subspan(kZlibHeaderBytes);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(deflated.data());
    stream.avail_in = static_cast<uInt>(deflated.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(expected);
    if (inflateInit2(&stream, kRawDeflateWindowBits) != Z_OK)
        return false;
    const bool whole = inflate(&stream, Z_FINISH) == Z_STREAM_END && stream.avail_in == 0 &&
                       stream.total_out == expected;
    return inflateEnd(&stream) == Z_OK && whole;
}

} // namespace

formats::hpi::SquashStatus
unsquash_archive_block(std::span<uint8_t> output, std::span<uint8_t> block) noexcept {
    using formats::hpi::SquashStatus;
    if (block.size() < formats::hpi::SQSHHeaderSize ||
        load_le32(block.data()) != formats::hpi::ChunkMarker)
        return SquashStatus::bad_header;
    const uint8_t type = block[squash_field::type];
    if (type >= kSquashTypeLimit)
        return SquashStatus::bad_unpack_type;
    const uint32_t packed = load_le32(block.data() + squash_field::packed_size);
    const uint32_t unpacked = load_le32(block.data() + squash_field::unpacked_size);
    // A packed size past the chunk is reported as a checksum failure.
    if (packed > block.size() - formats::hpi::SQSHHeaderSize)
        return SquashStatus::bad_checksum;
    const auto payload = block.subspan(formats::hpi::SQSHHeaderSize, packed);
    if (formats::sqsh::chunk_checksum(payload) != load_le32(block.data() + squash_field::checksum))
        return SquashStatus::bad_checksum;
    if (block[squash_field::scrambled] != 0)
        formats::sqsh::decrypt_chunk(payload);
    if (unpacked > output.size())
        return SquashStatus::bad_params;
    uint32_t produced = 0;
    if (type == kSquashLz77) {
        const auto decoded = formats::sqsh::decode_lz77_into(payload, output);
        if (decoded.status != formats::sqsh::Lz77Status::ok)
            return SquashStatus::bad_unpack_size;
        produced = static_cast<uint32_t>(decoded.written);
    } else if (type == kSquashZlib) {
        // The stream must end cleanly; `inflated` holds the most it may write
        // until then, and the count it wrote after.
        uint32_t inflated = unpacked;
        if (uncompress_legacy(output, &inflated, payload) == Z_OK)
            produced = inflated;
        // Some packing tools leave the stream's check value off; such a
        // chunk reads when it is otherwise whole.
        else if (inflate_without_check_value(output, unpacked, payload))
            produced = unpacked;
        else
            return SquashStatus::bad_unpack_size;
    } else {
        return SquashStatus::bad_unpack_size;
    }
    return produced == unpacked ? SquashStatus::ok : SquashStatus::bad_unpack_size;
}

const char* squash_status_name(formats::hpi::SquashStatus status) noexcept {
    const auto index = static_cast<std::size_t>(status);
    return index < std::size(kSquashNames) ? kSquashNames[index] : nullptr;
}

bool match_wildcard(std::string_view name, std::string_view pattern) noexcept {
    const auto pattern_at = [&](int index) {
        return index >= 0 && static_cast<std::size_t>(index) < pattern.size()
                   ? pattern[static_cast<std::size_t>(index)]
                   : '\0';
    };
    std::array<int, kWildcardStates> states{};
    int count = 1;
    for (const char raw : name) {
        const char folded = ascii_upper(raw);
        if (folded == '\0')
            break;
        int seen = 0;
        int read = 0;
        int write = count;
        // States appended for a '*' are visited within the same character.
        while (seen < count) {
            const int at = states[static_cast<std::size_t>(read)];
            const char wanted = ascii_upper(pattern_at(at));
            if (wanted == '?' || wanted == folded) {
                states[static_cast<std::size_t>(read)] = at + 1;
            } else if (wanted == '*') {
                if (count < static_cast<int>(kWildcardStates)) {
                    ++count;
                    states[static_cast<std::size_t>(write++)] = at + 1;
                }
            } else {
                --count;
                --write;
                if (count == 0)
                    return false;
                --seen;
                states[static_cast<std::size_t>(read)] = states[static_cast<std::size_t>(write)];
                --read;
            }
            ++seen;
            ++read;
        }
    }
    for (int i = 0; i < count; ++i) {
        const int at = states[static_cast<std::size_t>(i)];
        if (pattern_at(at) == '\0' || (pattern_at(at) == '*' && pattern_at(at + 1) == '\0'))
            return true;
    }
    return false;
}

struct HpiArchive::Impl {
    std::unique_ptr<ArchiveSource> source;
    uint64_t archive_size{};
    uint8_t key{};
    std::vector<ArchiveNode> nodes;
    // The lock is held only while a read copies stored bytes from the
    // source; decrypting and decompressing run outside it, so reads on
    // several threads decode at the same time.
    mutable base::threads::Mutex source_lock;

    explicit Impl(std::unique_ptr<ArchiveSource> archive_source) noexcept
        : source(std::move(archive_source)), archive_size(source->size()) {}

    /// Validates the header and trailer and resolves the directory into nodes.
    ///
    /// @return code none, or what is wrong with the archive and where
    [[nodiscard]] DecodeError load() {
        std::array<uint8_t, formats::hpi::HeaderSize> header{};
        if (source->read_at(0, header) != header.size() ||
            load_le32(header.data()) != formats::hpi::HeaderMarker)
            return DecodeError{DecodeCode::bad_signature, 0, "invalid HPI marker"};
        if (load_le32(header.data() + offsetof(formats::hpi::Header, version)) !=
            formats::hpi::VersionV1)
            return DecodeError{
                DecodeCode::unsupported_version,
                offsetof(formats::hpi::Header, version),
                "unsupported HPI version"
            };
        if (const auto trailer = check_trailer(); failed(trailer))
            return trailer;

        const uint32_t block_size =
            load_le32(header.data() + offsetof(formats::hpi::Header, directory_size));
        if (block_size < formats::hpi::HeaderSize || block_size > archive_size)
            return DecodeError{
                DecodeCode::out_of_range,
                offsetof(formats::hpi::Header, directory_size),
                "invalid HPI directory bounds"
            };
        if (block_size > kMaxDirectorySize)
            return DecodeError{
                DecodeCode::limit_exceeded,
                offsetof(formats::hpi::Header, directory_size),
                "HPI directory exceeds the 256 MiB safety limit"
            };
        std::vector<uint8_t> block(block_size);
        if (const std::size_t got = source->read_at(0, block); got != block.size())
            return DecodeError{DecodeCode::truncated, got, "truncated HPI directory"};
        key = hpi_archive_key(header[offsetof(formats::hpi::Header, decrypt_key)]);
        decrypt_at(
            std::span(block).subspan(formats::hpi::HeaderSize), key, formats::hpi::HeaderSize
        );

        const uint32_t root = load_le32(header.data() + offsetof(formats::hpi::Header, offset));
        if (root < formats::hpi::HeaderSize)
            return DecodeError{
                DecodeCode::out_of_range,
                offsetof(formats::hpi::Header, offset),
                "HPI root directory overlaps the header"
            };
        nodes.push_back(ArchiveNode{"", formats::hpi::EntryFlagDirectory});
        std::unordered_set<uint32_t> visited;
        return parse_directory(block, root, 0, visited, 0);
    }

    /// Checks that the last 36 bytes equal Trailer, apart from its four year characters.
    ///
    /// @return code none, or bad_signature at the trailer's offset
    [[nodiscard]] DecodeError check_trailer() const {
        const DecodeError missing{
            DecodeCode::bad_signature,
            archive_size < formats::hpi::Trailer.size()
                ? 0
                : archive_size - formats::hpi::Trailer.size(),
            "HPI archive has no copyright trailer"
        };
        if (archive_size < formats::hpi::Trailer.size())
            return missing;
        std::array<uint8_t, formats::hpi::Trailer.size()> tail{};
        if (source->read_at(archive_size - tail.size(), tail) != tail.size())
            return missing;
        std::copy_n(
            formats::hpi::Trailer.begin() + formats::hpi::TrailerYearOffset,
            formats::hpi::TrailerYearBytes,
            tail.begin() + formats::hpi::TrailerYearOffset
        );
        if (!std::equal(tail.begin(), tail.end(), formats::hpi::Trailer.begin()))
            return missing;
        return {};
    }

    /// Reads the NUL-terminated name at a directory-block offset.
    ///
    /// @param block decrypted directory block, header included
    /// @param offset block offset of the name
    /// @param[out] name receives the name
    /// @return code none, or out_of_range for a name outside the block or
    ///         malformed for one with no terminator
    static DecodeError name_at(std::span<const uint8_t> block, uint32_t offset, std::string& name) {
        if (offset >= block.size())
            return DecodeError{
                DecodeCode::out_of_range, offset, "HPI entry name lies outside the directory"
            };
        const auto begin = block.begin() + offset;
        const auto end = std::find(begin, block.end(), uint8_t{0});
        if (end == block.end())
            return DecodeError{DecodeCode::malformed, offset, "unterminated HPI entry name"};
        name.assign(begin, end);
        return {};
    }

    /// Resolves one directory node and its subtree into nodes.
    ///
    /// Children are appended as one contiguous run, then subdirectories are
    /// resolved depth-first.
    ///
    /// @param block decrypted directory block, header included
    /// @param offset block offset of the directory node (count, list offset)
    /// @param owner index of the node that receives the children
    /// @param[in,out] visited directory offsets already resolved
    /// @param depth nesting depth of this directory
    /// @return code none; or, at its block offset, out_of_range for a node,
    ///         list, name or file record outside the directory block, cycle
    ///         for a node reached twice, limit_exceeded for too many entries
    ///         or nesting past the depth limit, or malformed for an
    ///         unterminated name
    /// @quirk A negative count is an empty directory, as in 3.1c.
    [[nodiscard]] DecodeError parse_directory(
        std::span<const uint8_t> block,
        uint32_t offset,
        uint32_t owner,
        std::unordered_set<uint32_t>& visited,
        std::size_t depth
    ) {
        if (depth > kMaxDirectoryDepth)
            return DecodeError{
                DecodeCode::limit_exceeded, offset, "HPI directory nesting exceeds the safety limit"
            };
        // A node reached twice is a cycle or a shared node; both are rejected.
        if (!visited.insert(offset).second)
            return DecodeError{
                DecodeCode::cycle, offset, "HPI directory contains a cycle or reused node"
            };
        if (offset > block.size() || block.size() - offset < sizeof(formats::hpi::DirectoryNode))
            return DecodeError{
                DecodeCode::out_of_range, offset, "HPI directory node lies outside the directory"
            };
        const auto raw_count = static_cast<int32_t>(load_le32(block.data() + offset));
        const uint32_t list = load_le32(block.data() + offset + 4);
        // A negative count is an empty directory.
        const uint32_t count = raw_count > 0 ? static_cast<uint32_t>(raw_count) : 0;
        if (count > kMaxEntries - nodes.size())
            return DecodeError{
                DecodeCode::limit_exceeded, offset, "HPI archive contains too many entries"
            };
        if (list > block.size() ||
            (block.size() - list) / formats::hpi::DirectoryEntrySize < static_cast<uint64_t>(count))
            return DecodeError{
                DecodeCode::out_of_range,
                offset + offsetof(formats::hpi::DirectoryNode, list_offset),
                "HPI directory entry list lies outside the directory"
            };

        const auto first = static_cast<uint32_t>(nodes.size());
        nodes[owner].first_child = first;
        nodes[owner].child_count = count;
        nodes.resize(nodes.size() + count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t entry_offset = list + i * formats::hpi::DirectoryEntrySize;
            const uint8_t* entry = block.data() + entry_offset;
            ArchiveNode& node = nodes[first + i];
            if (const auto named = name_at(block, load_le32(entry), node.name); failed(named))
                return named;
            node.flags = entry[offsetof(formats::hpi::DirectoryEntry, type)];
            const uint32_t data =
                load_le32(entry + offsetof(formats::hpi::DirectoryEntry, data_offset));
            if (node.directory()) {
                node.data_offset = data;
                continue;
            }
            if (data > block.size() || block.size() - data < formats::hpi::FileEntrySize)
                return DecodeError{
                    DecodeCode::out_of_range,
                    entry_offset + offsetof(formats::hpi::DirectoryEntry, data_offset),
                    "HPI file record lies outside the directory"
                };
            node.data_offset = load_le32(block.data() + data);
            node.size = load_le32(block.data() + data + offsetof(formats::hpi::FileEntry, size));
            node.compression = block[data + offsetof(formats::hpi::FileEntry, compression)];
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (!nodes[first + i].directory())
                continue;
            const auto child =
                parse_directory(block, nodes[first + i].data_offset, first + i, visited, depth + 1);
            if (failed(child))
                return child;
        }
        return {};
    }

    [[nodiscard]] std::optional<uint32_t>
    walk(std::string_view entry_path, bool stop_before_last) const noexcept {
        uint32_t directory = 0;
        std::size_t begin = 0;
        for (;;) {
            std::size_t end = begin;
            while (end < entry_path.size() && entry_path[end] != '\\' && entry_path[end] != '/')
                ++end;
            const bool last = end == entry_path.size();
            if (last && stop_before_last)
                return directory;
            const std::string_view segment = entry_path.substr(begin, end - begin);
            const ArchiveNode& parent = nodes[directory];
            std::optional<uint32_t> match;
            for (uint32_t i = parent.child_count; i-- > 0;) {
                if (equal_nocase(nodes[parent.first_child + i].name, segment)) {
                    match = parent.first_child + i;
                    break;
                }
            }
            if (!match)
                return std::nullopt;
            if (last)
                return match;
            if (!nodes[*match].directory())
                return std::nullopt;
            directory = *match;
            begin = end + 1;
        }
    }

    /// Reads stored bytes at `offset` from the source.
    ///
    /// @param offset archive offset of the first byte
    /// @param[out] output receives the bytes
    /// @return the count read; a read past the end of the archive is short
    std::size_t read_stored(uint64_t offset, std::span<uint8_t> output) const {
        const base::threads::LockGuard guard(source_lock);
        return source->read_at(offset, output);
    }

    /// Copies file node `index`'s decoded bytes from `position` into `output`.
    ///
    /// @param index file node index
    /// @param position first decoded byte wanted
    /// @param[out] output receives the bytes
    /// @param chunks the node's chunk table, or null to read it for this call
    /// @return the count copied, short when a stored entry runs past the end
    ///         of the archive; or the error, at its archive offset, of a
    ///         chunk or size table that cannot be read or decoded, or
    ///         out_of_range for a chunk table of another node
    [[nodiscard]] Decoded<uint32_t> read_range(
        uint32_t index,
        uint32_t position,
        std::span<uint8_t> output,
        const formats::hpi::ChunkTable* chunks
    ) const {
        if (index >= nodes.size() || nodes[index].directory())
            return DecodeError{DecodeCode::out_of_range, 0, "HPI node is not a file"};
        const ArchiveNode& node = nodes[index];
        if (position >= node.size || output.empty())
            return 0u;
        const std::size_t wanted = std::min<std::size_t>(output.size(), node.size - position);
        if (node.compression == 0) {
            const uint64_t at = static_cast<uint64_t>(node.data_offset) + position;
            const std::size_t got = read_stored(at, output.first(wanted));
            decrypt_at(output.first(got), key, at);
            return static_cast<uint32_t>(got);
        }
        formats::hpi::ChunkTable read_now;
        if (chunks == nullptr) {
            auto table = chunk_table(index);
            if (!table.ok())
                return table.error;
            read_now = std::move(*table.value);
            chunks = &read_now;
        } else if (chunks->node != index || chunks->offsets.size() != chunk_count(node) + 1U) {
            return DecodeError{
                DecodeCode::out_of_range, node.data_offset, "HPI chunk table is another entry's"
            };
        }
        std::size_t copied = 0;
        return decode_blocks(
            node, *chunks, position, wanted, [&](const uint8_t* bytes, std::size_t count) {
                std::memcpy(output.data() + copied, bytes, count);
                copied += count;
            }
        );
    }

    /// Returns how many chunks a compressed file node is stored in.
    ///
    /// @param node the file node
    /// @return its decoded size in 64 KiB blocks, rounded up
    [[nodiscard]] static uint32_t chunk_count(const ArchiveNode& node) noexcept {
        return node.size / formats::hpi::BlockBytes + (node.size % formats::hpi::BlockBytes != 0);
    }

    /// Reads and decrypts a compressed file node's chunk-size table.
    ///
    /// @param index file node index; a compressed file
    /// @return where each chunk is stored; or truncated, at the node's data
    ///         offset, for a table past the end of the archive
    [[nodiscard]] Decoded<formats::hpi::ChunkTable> chunk_table(uint32_t index) const {
        const ArchiveNode& node = nodes[index];
        const uint32_t chunks = chunk_count(node);
        std::vector<uint8_t> table(static_cast<std::size_t>(chunks) * 4U);
        if (read_stored(node.data_offset, table) != table.size())
            return DecodeError{
                DecodeCode::truncated,
                node.data_offset,
                "HPI chunk size table lies past the end of the archive"
            };
        decrypt_at(table, key, node.data_offset);
        formats::hpi::ChunkTable result;
        result.node = index;
        result.offsets.resize(static_cast<std::size_t>(chunks) + 1U);
        // Summed at 64 bits, so stored sizes cannot wrap the offset back
        // into the archive; a chunk past its end reads short.
        uint64_t at = static_cast<uint64_t>(node.data_offset) + table.size();
        for (uint32_t i = 0; i < chunks; ++i) {
            result.offsets[i] = at;
            at += load_le32(table.data() + i * 4U);
        }
        result.offsets[chunks] = at;
        return result;
    }

    /// Decodes the chunks of a compressed file node that hold `wanted`
    /// bytes from `position`, handing each chunk's part to `take` in order.
    ///
    /// @param node the file node; compressed
    /// @param chunks the node's chunk table
    /// @param position first decoded byte wanted
    /// @param wanted count of bytes wanted; within the entry
    /// @param take called with each part's bytes and count
    /// @return the count handed on; or the error, at its archive offset, of
    ///         a chunk that cannot be read or decoded
    template <class Take>
    [[nodiscard]] Decoded<uint32_t> decode_blocks(
        const ArchiveNode& node,
        const formats::hpi::ChunkTable& chunks,
        uint32_t position,
        std::size_t wanted,
        Take&& take
    ) const {
        // Every byte of these buffers is written before it is read, so they
        // are allocated without being filled.
        const auto decoded_bytes =
            std::make_unique_for_overwrite<uint8_t[]>(formats::hpi::BlockBytes);
        const std::span<uint8_t> decoded(decoded_bytes.get(), formats::hpi::BlockBytes);
        std::size_t copied = 0;
        uint32_t cursor = position;
        uint32_t loaded = std::numeric_limits<uint32_t>::max();
        while (copied < wanted) {
            const uint32_t block_index = cursor / formats::hpi::BlockBytes;
            if (block_index != loaded) {
                const uint64_t at = chunks.offsets[block_index];
                // The difference is one size of the table, so it fits 32 bits.
                const auto stored = static_cast<uint32_t>(chunks.offsets[block_index + 1U] - at);
                if (stored > kMaxStoredChunkSize)
                    return DecodeError{
                        DecodeCode::limit_exceeded,
                        at,
                        "stored HPI chunk exceeds the 1 MiB safety limit"
                    };
                // Each chunk gets a buffer of exactly its size, so a read past
                // the chunk is a read past the allocation, which the sanitizer
                // build reports.
                const auto chunk_bytes = std::make_unique_for_overwrite<uint8_t[]>(stored);
                const std::span<uint8_t> chunk(chunk_bytes.get(), stored);
                if (read_stored(at, chunk) != chunk.size())
                    return DecodeError{
                        DecodeCode::truncated, at, "HPI chunk lies past the end of the archive"
                    };
                decrypt_at(chunk, key, at);
                // Bytes a short chunk leaves unwritten read as zero.
                std::memset(decoded.data(), 0, decoded.size());
                const auto status = unsquash_archive_block(decoded, chunk);
                if (status != formats::hpi::SquashStatus::ok)
                    return DecodeError{
                        DecodeCode::malformed,
                        at,
                        squash_status_name(status),
                        static_cast<uint16_t>(status)
                    };
                loaded = block_index;
            }
            const uint32_t offset = cursor % formats::hpi::BlockBytes;
            const std::size_t count =
                std::min<std::size_t>(formats::hpi::BlockBytes - offset, wanted - copied);
            take(decoded.data() + offset, count);
            copied += count;
            cursor += static_cast<uint32_t>(count);
        }
        return static_cast<uint32_t>(copied);
    }
};

std::size_t ArchiveBuffer::read_at(uint64_t offset, std::span<uint8_t> output) {
    if (offset >= bytes_.size())
        return 0;
    const std::size_t count = std::min<std::size_t>(output.size(), bytes_.size() - offset);
    std::memcpy(output.data(), bytes_.data() + offset, count);
    return count;
}

Decoded<HpiArchive> HpiArchive::open(std::unique_ptr<ArchiveSource> source) {
    if (!source)
        return DecodeError{DecodeCode::not_found, 0, "no HPI archive source"};
    auto impl = std::make_unique<Impl>(std::move(source));
    if (const auto error = impl->load(); failed(error))
        return error;
    return HpiArchive(std::move(impl));
}

Decoded<HpiArchive> HpiArchive::open(std::vector<uint8_t> bytes) {
    return open(std::make_unique<ArchiveBuffer>(std::move(bytes)));
}

HpiArchive::HpiArchive(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {
}

HpiArchive::~HpiArchive() = default;
HpiArchive::HpiArchive(HpiArchive&&) noexcept = default;
HpiArchive& HpiArchive::operator=(HpiArchive&&) noexcept = default;

std::span<const ArchiveNode> HpiArchive::nodes() const noexcept {
    return impl_->nodes;
}

std::vector<ArchiveEntry> HpiArchive::entries() const {
    std::vector<ArchiveEntry> result;
    const auto visit =
        [&](const auto& self, uint32_t directory, const std::string& prefix) -> void {
        const ArchiveNode& parent = impl_->nodes[directory];
        for (uint32_t i = 0; i < parent.child_count; ++i) {
            const auto index = parent.first_child + i;
            const ArchiveNode& node = impl_->nodes[index];
            const std::string full = prefix.empty() ? node.name : prefix + "/" + node.name;
            if (node.directory())
                self(self, index, full);
            else
                result.push_back({full, node.size});
        }
    };
    visit(visit, 0, "");
    return result;
}

std::optional<uint32_t> HpiArchive::lookup(std::string_view path) const noexcept {
    return impl_->walk(path, false);
}

std::optional<uint32_t> HpiArchive::lookup_directory(std::string_view path) const noexcept {
    return impl_->walk(path, true);
}

Decoded<uint32_t>
HpiArchive::read_node_range(uint32_t index, uint32_t position, std::span<uint8_t> output) const {
    return impl_->read_range(index, position, output, nullptr);
}

Decoded<uint32_t> HpiArchive::read_node_range(
    uint32_t index,
    uint32_t position,
    std::span<uint8_t> output,
    const formats::hpi::ChunkTable& chunks
) const {
    return impl_->read_range(index, position, output, &chunks);
}

Decoded<formats::hpi::ChunkTable> HpiArchive::read_chunk_table(uint32_t index) const {
    if (index >= impl_->nodes.size() || impl_->nodes[index].directory() ||
        impl_->nodes[index].compression == 0)
        return DecodeError{DecodeCode::out_of_range, 0, "HPI node is not a compressed file"};
    return impl_->chunk_table(index);
}

Decoded<std::vector<uint8_t>> HpiArchive::read_node(uint32_t index) const {
    if (index >= impl_->nodes.size() || impl_->nodes[index].directory())
        return DecodeError{DecodeCode::out_of_range, 0, "HPI node is not a file"};
    const ArchiveNode& node = impl_->nodes[index];
    // The size an entry claims is checked against the archive before any
    // buffer is allocated: stored bytes must lie inside it, and every chunk
    // of a compressed entry needs at least its size-table slot and header.
    if (!formats::hpi::entry_size_allowed(node.size))
        return DecodeError{
            DecodeCode::limit_exceeded, node.data_offset, "HPI entry exceeds the entry size limit"
        };
    const uint64_t chunks =
        node.size / formats::hpi::BlockBytes + (node.size % formats::hpi::BlockBytes != 0);
    const uint64_t least_stored = node.compression == 0
                                      ? node.size
                                      : chunks * (sizeof(uint32_t) + formats::hpi::SQSHHeaderSize);
    if (node.data_offset > impl_->archive_size ||
        least_stored > impl_->archive_size - node.data_offset)
        return DecodeError{
            DecodeCode::truncated, node.data_offset, "HPI entry lies past the end of the archive"
        };
    std::vector<uint8_t> bytes;
    Decoded<uint32_t> read = 0u;
    try {
        if (node.compression == 0) {
            // The archive holds every stored byte, so the buffer is no
            // larger than what it gives.
            bytes.resize(node.size);
            read = impl_->read_range(index, 0, bytes, nullptr);
        } else {
            // Room is set aside for the claimed size, and only the chunks
            // that decode fill it.
            bytes.reserve(node.size);
            auto chunks = impl_->chunk_table(index);
            if (!chunks.ok())
                return chunks.error;
            read = impl_->decode_blocks(
                node, *chunks.value, 0, node.size, [&](const uint8_t* decoded, std::size_t count) {
                    bytes.insert(bytes.end(), decoded, decoded + count);
                }
            );
        }
    } catch (const std::bad_alloc&) {
        return DecodeError{
            DecodeCode::limit_exceeded, node.data_offset, "HPI entry is larger than memory allows"
        };
    }
    if (!read.ok())
        return read.error;
    if (*read.value != node.size)
        return DecodeError{
            DecodeCode::truncated,
            static_cast<uint64_t>(node.data_offset) + *read.value,
            "truncated HPI entry"
        };
    return bytes;
}

Decoded<std::vector<uint8_t>> HpiArchive::read(std::string_view path) const {
    const auto index = lookup(path);
    if (!index || impl_->nodes[*index].directory())
        return DecodeError{DecodeCode::not_found, 0, "HPI entry not found"};
    return read_node(*index);
}

} // namespace oa
