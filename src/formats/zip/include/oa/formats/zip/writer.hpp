// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The streaming zip writer: an archive written a piece at a time through a
// hook, however large, as a package (.oamod, .oalang, .oamap, .oamut) is made. It
// stores or deflates each entry as the bytes arrive, completes that entry's
// local header in place, and writes the central directory at the end. The
// same entries and options give the same bytes. Nothing it holds grows with
// an entry's size: one fixed output piece, the directory, and the open
// entry's name.
//
// It needs a positioned write. A pipe cannot carry it, because each local
// header's CRC-32 and sizes are written back once the entry's data ends.
#pragma once

#include "oa/formats/zip.hpp"

#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::zip {

/// Where a streamed archive's bytes go.
struct OutputHooks {
    void* context{}; ///< passed back to write_at
    /// Writes `bytes` at `offset` in the archive, replacing any bytes already
    /// there. False when they cannot all be written. Null writes nothing, so
    /// every write fails.
    bool (*write_at)(void* context, uint64_t offset, std::span<const uint8_t> bytes){};
};

/// When the 64-bit extension's records are written.
enum class Zip64 : uint8_t {
    when_needed, ///< a size, offset or count at the writer's 64-bit thresholds
    always,      ///< every entry, and the end of the archive
};

/// Choices that change the bytes a streaming writer produces.
struct WriterOptions {
    int deflate_level{9};            ///< zlib's level, 0 to 9; the default is its tightest
    Zip64 zip64{Zip64::when_needed}; ///< when the 64-bit extension is written
};

/// Writes one zip archive through a positioned hook.
///
/// Entries are written in the order they are begun. A stored entry's bytes
/// are written as given. A deflated entry is compressed with zlib's raw
/// deflate as the bytes arrive. `finish` writes the central directory and
/// the end record. After a failed write, or an entry that ends at the wrong
/// size, every later call fails.
class StreamWriter {
  public:

    /// Begins an archive that writes through `output`.
    ///
    /// @param output where the archive's bytes go; it must outlive the writer
    /// @param options the deflate level and when the 64-bit extension is written
    explicit StreamWriter(OutputHooks output, WriterOptions options = {});

    StreamWriter(const StreamWriter&) = delete;
    StreamWriter& operator=(const StreamWriter&) = delete;

    /// Ends the open entry's deflate stream, when one was started.
    ~StreamWriter();

    /// Begins an entry. `bytes` is the size its data will have, uncompressed.
    ///
    /// The name must be safe, at most `max_name_bytes`, well-formed UTF-8 and
    /// not a name already begun with ASCII case ignored. A name that ends in
    /// '/' is a folder and must declare no bytes and be stored. The local
    /// header is written with a zero CRC-32 and zero sizes, filled in by
    /// `end_entry`.
    ///
    /// @param name the entry's name, '/' between folders
    /// @param bytes the uncompressed size the entry will have
    /// @param method stored or deflated
    /// @param[out] error the status, and the entry's name
    /// @return true when the entry can be written
    [[nodiscard]] bool
    begin_entry(std::string_view name, uint64_t bytes, Method method, ZipError& error);

    /// Writes the next bytes of the open entry, in order.
    ///
    /// @param data the next uncompressed bytes; empty writes nothing
    /// @param[out] error the status when the write fails
    /// @return true when the bytes were accepted
    [[nodiscard]] bool write(std::span<const uint8_t> data, ZipError& error);

    /// Ends the open entry, checks it has its declared size, and writes its
    /// CRC-32 and sizes back into the local header.
    ///
    /// @param[out] error the status when the entry cannot be ended
    /// @return true when the entry was ended
    [[nodiscard]] bool end_entry(ZipError& error);

    /// Writes a folder: `name` must end in '/', and the entry holds nothing.
    ///
    /// @param name the folder's name, ending in '/'
    /// @param[out] error the status when the folder cannot be written
    /// @return true when the folder was written
    [[nodiscard]] bool add_folder(std::string_view name, ZipError& error);

    /// Writes the central directory and the end record.
    ///
    /// No entry may be open. A second call fails.
    ///
    /// @param[out] error the status when the archive cannot be finished
    /// @return true when the archive was finished
    [[nodiscard]] bool finish(ZipError& error);

    /// Returns how many bytes of the archive have been written.
    ///
    /// The count includes a stored entry's bytes and the gap a positioned
    /// write leaves, so it is the archive's size so far.
    ///
    /// @return the archive's size so far, in bytes
    [[nodiscard]] uint64_t bytes_written() const noexcept;

  private:

    /// The raw deflate stream of the open entry.
    struct DeflateStream;

    /// Reports a remembered failure, when one is set.
    ///
    /// @param[out] error the remembered failure
    /// @return true when a failure was remembered
    [[nodiscard]] bool stuck(ZipError& error) const;

    /// Records a failure, and remembers it when `remember` is set.
    ///
    /// @param[out] error the error to fill
    /// @param status why the call failed
    /// @param offset the byte offset of the record at fault
    /// @param entry the entry's name; empty for the archive
    /// @param remember true to refuse every later call with this failure
    /// @return false, for the caller to return
    bool fail_at(
        ZipError& error, ZipStatus status, uint64_t offset, std::string_view entry, bool remember
    );

    /// Writes bytes at the end of the archive.
    ///
    /// @param bytes the bytes; empty writes nothing
    /// @param[out] error the status when the hook refuses
    /// @return true when the bytes were written
    [[nodiscard]] bool emit(std::span<const uint8_t> bytes, ZipError& error);

    /// Writes bytes over an earlier part of the archive.
    ///
    /// @param offset where the bytes start
    /// @param bytes the bytes
    /// @param[out] error the status when the hook refuses
    /// @return true when the bytes were written
    [[nodiscard]] bool patch(uint64_t offset, std::span<const uint8_t> bytes, ZipError& error);

    /// Checks a name and a declared size against the writer's rules.
    ///
    /// @param name the entry's name
    /// @param bytes the uncompressed size the entry will have
    /// @param method stored or deflated
    /// @param[out] folded the name with ASCII capitals lowered, when it is accepted
    /// @param[out] error the status when the name is refused
    /// @return true when the entry may be begun
    [[nodiscard]] bool check_name(
        std::string_view name, uint64_t bytes, Method method, std::string& folded, ZipError& error
    );

    /// Starts zlib's raw deflate for the open entry.
    ///
    /// @param[out] error the status when deflate cannot be started
    /// @return true when it was started
    [[nodiscard]] bool open_deflate(ZipError& error);

    /// Feeds the open deflate stream and writes each output piece.
    ///
    /// @param data uncompressed input; empty when `finish_stream` ends it
    /// @param finish_stream true to end the stream
    /// @param[out] error the status when deflate or the hook fails
    /// @return true when the input was taken, and the stream ended when asked
    [[nodiscard]] bool
    deflate_span(std::span<const uint8_t> data, bool finish_stream, ZipError& error);

    /// Ends and frees the open deflate stream.
    void close_deflate() noexcept;

    /// Writes the open entry's local header.
    ///
    /// @param name the entry's name
    /// @param[out] error the status when the hook refuses
    /// @return true when the header was written
    [[nodiscard]] bool write_local_header(std::string_view name, ZipError& error);

    /// Writes the open entry's CRC-32 and sizes into its local header.
    ///
    /// @param[out] error the status when the hook refuses
    /// @return true when the header was completed
    [[nodiscard]] bool finish_local_header(ZipError& error);

    /// Appends the open entry's central directory record.
    void keep_central_record();

    OutputHooks output_{};
    WriterOptions options_{};
    uint64_t written_{};
    uint64_t declared_{};
    uint64_t uncompressed_{};
    uint64_t compressed_{};
    uint64_t local_offset_{};
    uint64_t entry_count_{};
    uint64_t stuck_offset_{};
    uint32_t crc_{};
    uint16_t flags_{};
    uint16_t version_{};
    uint16_t name_bytes_{};
    Method method_{Method::stored};
    ZipStatus stuck_status_{ZipStatus::ok};
    bool open_{};
    bool finished_{};
    bool zip64_entry_{};
    std::string open_name_{};
    std::string stuck_entry_{};
    std::set<std::string> folded_names_{};
    std::vector<uint8_t> central_{};
    std::unique_ptr<DeflateStream> deflate_{};
};

} // namespace oa::formats::zip
