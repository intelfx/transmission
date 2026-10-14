// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::find()
#include <array>
#include <cstddef>
#include <cctype>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <small/map.hpp>

#include "libtransmission/transmission.h"

#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/torrent-files.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h"

using namespace std::literals;

namespace
{

using file_func_t = std::function<void(char const* filename)>;

bool is_folder(std::string_view path)
{
    auto const info = tr_sys_path_get_info(path);
    return info && info->isFolder();
}

bool is_empty_folder(char const* path)
{
    if (!is_folder(path))
    {
        return false;
    }

    if (auto const odir = tr_sys_dir_open(path); odir != TR_BAD_SYS_DIR)
    {
        char const* name_cstr = nullptr;
        while ((name_cstr = tr_sys_dir_read_name(odir)) != nullptr)
        {
            auto const name = std::string_view{ name_cstr };
            if (name != "." && name != "..")
            {
                tr_sys_dir_close(odir);
                return false;
            }
        }
        tr_sys_dir_close(odir);
    }

    return true;
}

void depth_first_walk(char const* path, file_func_t const& func, std::optional<int> max_depth = {})
{
    if (is_folder(path) && (!max_depth || *max_depth > 0))
    {
        if (auto const odir = tr_sys_dir_open(path); odir != TR_BAD_SYS_DIR)
        {
            char const* name_cstr = nullptr;
            while ((name_cstr = tr_sys_dir_read_name(odir)) != nullptr)
            {
                auto const name = std::string_view{ name_cstr };
                if (name == "." || name == "..")
                {
                    continue;
                }

                depth_first_walk(tr_pathbuf{ path, '/', name }.c_str(), func, max_depth ? *max_depth - 1 : max_depth);
            }

            tr_sys_dir_close(odir);
        }
    }

    func(path);
}

[[nodiscard]] bool is_valid_subpath(std::string_view path)
{
    if (std::empty(path) || path.front() == '/' || path.back() == '/' || !tr_torrent_files::is_subpath_sanitized(path))
    {
        return false;
    }

    auto segment = std::string_view{};
    while (tr_strv_sep(&path, &segment, '/'))
    {
        if (std::empty(segment) || segment == "."sv || segment == ".."sv)
        {
            return false;
        }
    }

    return true;
}

void set_move_error(tr_error* error, std::string_view old_path, std::string_view path, int code, std::string_view message)
{
    if (error != nullptr)
    {
        error->set(
            code,
            fmt::format(
                fmt::runtime(_("Couldn't move '{old_path}' to '{path}': {error} ({error_code})")),
                fmt::arg("old_path", old_path),
                fmt::arg("path", path),
                fmt::arg("error", message),
                fmt::arg("error_code", code)));
    }
}

bool is_junk_file(std::string_view filename)
{
    auto const base = tr_sys_path_basename(filename);

#ifdef __APPLE__
    // check for resource forks. <http://web.archive.org/web/20101010051608/http://support.apple.com/kb/TA20578>
    if (tr_strv_starts_with(base, "._"sv))
    {
        return true;
    }
#endif

    auto constexpr Files = std::array<std::string_view, 3>{
        ".DS_Store"sv,
        "Thumbs.db"sv,
        "desktop.ini"sv,
    };

    return std::find(std::begin(Files), std::end(Files), base) != std::end(Files);
}

} // unnamed namespace

// ---

std::optional<tr_torrent_files::FoundFile> tr_torrent_files::find(
    tr_file_index_t file_index,
    std::string_view const* paths,
    size_t n_paths) const
{
    auto filename = tr_pathbuf{};
    auto const& subpath = path(file_index);

    for (size_t path_idx = 0; path_idx < n_paths; ++path_idx)
    {
        auto const base = paths[path_idx];

        filename.assign(base, '/', subpath);
        if (auto const info = tr_sys_path_get_info(filename); info)
        {
            return FoundFile{ *info, std::move(filename), std::size(base) };
        }

        filename.assign(base, '/', subpath, PartialFileSuffix);
        if (auto const info = tr_sys_path_get_info(filename); info)
        {
            return FoundFile{ *info, std::move(filename), std::size(base) };
        }
    }

    return {};
}

bool tr_torrent_files::has_any_local_data(std::string_view const* paths, size_t n_paths) const
{
    for (tr_file_index_t i = 0, n = file_count(); i < n; ++i)
    {
        if (find(i, paths, n_paths))
        {
            return true;
        }
    }

    return false;
}

std::string_view tr_torrent_files::primary_mime_type() const
{
    // count up how many bytes there are for each mime-type in the torrent
    auto size_per_mime_type = small::unordered_map<std::string_view, size_t, 256U>{};
    for (tr_file_index_t i = 0, n = file_count(); i < n; ++i)
    {
        auto const mime_type = tr_get_mime_type_for_filename(path(i));
        size_per_mime_type[mime_type] += file_size(i);
    }

    if (std::empty(size_per_mime_type))
    {
        // https://developer.mozilla.org/en-US/docs/Web/HTTP/Basics_of_HTTP/MIME_types/Common_types
        // application/octet-stream is the default value for all other cases.
        // An unknown file type should use this type.
        auto constexpr Fallback = "application/octet-stream"sv;
        return Fallback;
    }

    auto const it = std::max_element(
        std::begin(size_per_mime_type),
        std::end(size_per_mime_type),
        [](auto const& a, auto const& b) { return a.second < b.second; });
    return it->first;
}

// ---

bool tr_torrent_files::move(
    std::string_view old_parent_in,
    std::string_view parent_in,
    std::string_view parent_name,
    tr_error* error) const
{
    auto const old_parent = tr_pathbuf{ old_parent_in };
    auto const parent = tr_pathbuf{ parent_in };
    tr_logAddTrace(fmt::format("Moving files from '{:s}' to '{:s}'", old_parent, parent), parent_name);

    if (tr_sys_path_is_same(old_parent, parent))
    {
        return true;
    }

    if (!tr_sys_dir_create(parent, TR_SYS_DIR_CREATE_PARENTS, 0777, error))
    {
        return false;
    }

    auto const paths = std::array<std::string_view, 1>{ old_parent.sv() };

    auto err = bool{};

    for (tr_file_index_t i = 0, n = file_count(); i < n; ++i)
    {
        auto const found = find(i, std::data(paths), std::size(paths));
        if (!found)
        {
            continue;
        }

        auto const& old_path = found->filename();
        auto const path = tr_pathbuf{ parent, '/', found->subpath() };
        tr_logAddTrace(fmt::format("Found file #{:d} '{:s}'", i, old_path), parent_name);

        if (tr_sys_path_is_same(old_path, path))
        {
            continue;
        }

        tr_logAddTrace(fmt::format("Moving file #{:d} to '{:s}'", i, old_path, path), parent_name);
        if (!tr_file_move(old_path, path, true, error))
        {
            err = true;
            break;
        }

        prune_empty_dirs(tr_sys_path_dirname(old_path), old_parent);
    }

    return !err;
}

std::optional<std::vector<std::string>> tr_torrent_files::resolve_paths(
    std::vector<std::optional<std::string>> const& paths,
    tr_error* error) const
{
    auto const n_files = file_count();
    if (std::size(paths) != n_files)
    {
        if (error != nullptr)
        {
            error->set(EINVAL, fmt::format("expected {:d} file paths, got {:d}", n_files, std::size(paths)));
        }
        return {};
    }

    auto ret = std::vector<std::string>{};
    ret.reserve(n_files);
    for (tr_file_index_t i = 0; i < n_files; ++i)
    {
        if (!paths[i])
        {
            ret.emplace_back(path(i));
        }
        else if (is_valid_subpath(*paths[i]))
        {
            ret.emplace_back(*paths[i]);
        }
        else
        {
            if (error != nullptr)
            {
                error->set(EINVAL, fmt::format("invalid path '{:s}' for file #{:d}", *paths[i], i));
            }
            return {};
        }
    }

    auto file_by_path = std::unordered_map<std::string_view, tr_file_index_t>{};
    file_by_path.reserve(n_files);
    for (tr_file_index_t i = 0; i < n_files; ++i)
    {
        if (auto const [iter, inserted] = file_by_path.try_emplace(ret[i], i); !inserted)
        {
            if (error != nullptr)
            {
                error->set(EINVAL, fmt::format("files #{:d} and #{:d} have the same path '{:s}'", iter->second, i, ret[i]));
            }
            return {};
        }
    }

    for (tr_file_index_t i = 0; i < n_files; ++i)
    {
        auto const sv = std::string_view{ ret[i] };
        for (auto pos = sv.find('/'); pos != std::string_view::npos; pos = sv.find('/', pos + 1))
        {
            if (auto const iter = file_by_path.find(sv.substr(0, pos)); iter != std::end(file_by_path))
            {
                if (error != nullptr)
                {
                    error->set(EINVAL, fmt::format("path '{:s}' of file #{:d} is inside file #{:d}", ret[i], i, iter->second));
                }
                return {};
            }
        }
    }

    return ret;
}

bool tr_torrent_files::relocate(
    std::vector<std::string> const& new_paths,
    std::string_view const* old_bases,
    std::string_view const* new_bases,
    size_t n_bases,
    tr_error* error) const
{
    TR_ASSERT(std::size(new_paths) == file_count());

    struct Move
    {
        std::string src;
        std::string dst;
        std::string tmp; // where `src` is parked while other files move, if needed
        std::string_view old_base;
        std::string_view new_base;

        [[nodiscard]] std::string const& from() const noexcept
        {
            return std::empty(tmp) ? src : tmp;
        }
    };

    auto moves = std::vector<Move>{};
    for (tr_file_index_t i = 0, n = file_count(); i < n; ++i)
    {
        auto const found = find(i, old_bases, n_bases);
        if (!found)
        {
            continue;
        }

        auto const base = found->base();
        auto const base_idx = static_cast<size_t>(std::find(old_bases, old_bases + n_bases, base) - old_bases);
        TR_ASSERT(base_idx < n_bases);
        auto const suffix = found->filename().sv().substr(std::size(base) + 1U + std::size(path(i)));

        auto move = Move{};
        move.src = found->filename().sv();
        move.dst = tr_pathbuf{ new_bases[base_idx], '/', new_paths[i], suffix }.sv();
        move.old_base = old_bases[base_idx];
        move.new_base = new_bases[base_idx];
        if (move.src != move.dst)
        {
            moves.emplace_back(std::move(move));
        }
    }

    // Refuse to overwrite anything but the files that are being moved away.
    auto srcs = std::unordered_set<std::string_view>{};
    srcs.reserve(std::size(moves));
    for (auto const& move : moves)
    {
        srcs.emplace(move.src);
    }

    for (auto const& move : moves)
    {
        if (tr_sys_path_exists(move.dst) && srcs.count(move.dst) == 0U && !tr_sys_path_is_same(move.src, move.dst))
        {
            set_move_error(error, move.src, move.dst, EEXIST, tr_strerror(EEXIST));
            return false;
        }
    }

    // A file that is in the way of another one (it is at that file's new path,
    // or at one of its new parent directories) is moved out of the way first.
    auto in_the_way = std::unordered_set<std::string_view>{};
    for (auto const& move : moves)
    {
        auto const dst = std::string_view{ move.dst };
        in_the_way.emplace(dst);
        for (auto pos = dst.find('/'); pos != std::string_view::npos; pos = dst.find('/', pos + 1))
        {
            in_the_way.emplace(dst.substr(0, pos));
        }
    }

    auto const undo = [&](size_t n_parked, size_t n_moved)
    {
        auto ok = true;

        for (size_t i = n_moved; i > 0; --i)
        {
            auto const& move = moves[i - 1];
            if (auto local_error = tr_error{}; !tr_file_move(move.dst, move.from(), true, &local_error))
            {
                ok = false;
                tr_logAddError(
                    fmt::format("Couldn't move '{:s}' back to '{:s}': {:s}", move.dst, move.from(), local_error.message()));
            }
            prune_empty_dirs(tr_sys_path_dirname(move.dst), move.new_base);
        }

        for (size_t i = n_parked; i > 0; --i)
        {
            auto const& move = moves[i - 1];
            if (auto local_error = tr_error{}; !std::empty(move.tmp) && !tr_sys_path_rename(move.tmp, move.src, &local_error))
            {
                ok = false;
                tr_logAddError(
                    fmt::format("Couldn't move '{:s}' back to '{:s}': {:s}", move.tmp, move.src, local_error.message()));
            }
        }

        if (!ok && error != nullptr)
        {
            error->prefix_message("Some files could not be moved back (see the log): ");
        }
    };

    for (size_t i = 0, n = std::size(moves); i < n; ++i)
    {
        auto& move = moves[i];
        if (in_the_way.count(move.src) == 0U)
        {
            continue;
        }

        for (size_t attempt = 0;; ++attempt)
        {
            move.tmp = fmt::format("{:s}.tr-rename-{:d}", move.src, attempt);
            if (in_the_way.count(move.tmp) == 0U && !tr_sys_path_exists(move.tmp))
            {
                break;
            }
        }

        tr_logAddTrace(fmt::format("Moving '{:s}' out of the way to '{:s}'", move.src, move.tmp));
        if (auto local_error = tr_error{}; !tr_sys_path_rename(move.src, move.tmp, &local_error))
        {
            set_move_error(error, move.src, move.tmp, local_error.code(), local_error.message());
            move.tmp.clear();
            undo(i, 0U);
            return false;
        }
    }

    for (size_t i = 0, n = std::size(moves); i < n; ++i)
    {
        auto const& move = moves[i];
        tr_logAddTrace(fmt::format("Moving '{:s}' to '{:s}'", move.from(), move.dst));

        auto local_error = tr_error{};
        if (tr_sys_path_exists(move.dst) && !tr_sys_path_is_same(move.from(), move.dst))
        {
            local_error.set(EEXIST, tr_strerror(EEXIST));
        }
        else
        {
            tr_file_move(move.from(), move.dst, true, &local_error);
        }

        if (local_error)
        {
            set_move_error(error, move.src, move.dst, local_error.code(), local_error.message());
            undo(n, i);
            return false;
        }
    }

    for (auto const& move : moves)
    {
        prune_empty_dirs(tr_sys_path_dirname(move.src), move.old_base);
    }

    return true;
}

void tr_torrent_files::prune_empty_dirs(std::string_view const dir_in, std::string_view const stop_at)
{
    auto const stop_at_as_dir = tr_pathbuf{ stop_at, '/' };
    auto dir = tr_pathbuf{ dir_in };

    while (tr_strv_starts_with(dir.sv(), stop_at_as_dir.sv()) && is_empty_folder(dir.c_str()) && tr_sys_path_remove(dir))
    {
        dir.popdir();
    }
}

// ---

/**
 * This convoluted code does something (seemingly) simple:
 * remove the torrent's local files.
 *
 * Fun complications:
 * 1. Try to preserve the directory hierarchy in the recycle bin.
 * 2. If there are nontorrent files, don't delete them...
 * 3. ...unless the other files are "junk", such as .DS_Store
 */
void tr_torrent_files::remove(std::string_view parent_in, std::string_view tmpdir_prefix, FileFunc const& func, tr_error* error)
    const
{
    auto const parent = tr_pathbuf{ parent_in };

    // don't try to delete local data if the directory's gone missing
    if (!tr_sys_path_exists(parent))
    {
        return;
    }

    // try to make a tmpdir
    auto tmpdir = tr_pathbuf{ parent, '/', tmpdir_prefix, "__XXXXXX"sv };
    if (!tr_sys_dir_create_temp(std::data(tmpdir), error))
    {
        return;
    }

    // move the local data to the tmpdir
    auto const paths = std::array<std::string_view, 1>{ parent.sv() };
    for (tr_file_index_t idx = 0, n_files = file_count(); idx < n_files; ++idx)
    {
        if (auto const found = find(idx, std::data(paths), std::size(paths)); found)
        {
            // if moving a file fails, give up and let the error propagate
            if (!tr_file_move(found->filename(), tr_pathbuf{ tmpdir, '/', found->subpath() }, false, error))
            {
                return;
            }
        }
    }

    // Make a list of the top-level torrent files & folders
    // because we'll need it below in the 'remove junk' phase
    auto const path = tr_pathbuf{ parent, '/', tmpdir_prefix };
    auto top_files = std::set<std::string>{ std::string{ path } };
    depth_first_walk(
        tmpdir,
        [&parent, &tmpdir, &top_files](char const* filename)
        {
            if (tmpdir != filename)
            {
                top_files.emplace(tr_pathbuf{ parent, '/', tr_sys_path_basename(filename) });
            }
        },
        1);

    auto const func_wrapper = [&tmpdir, &func](char const* filename)
    {
        if (tmpdir != filename)
        {
            func(filename);
        }
    };

    // Remove the tmpdir.
    // Since `func` might send files to a recycle bin, try to preserve
    // the folder hierarchy by removing top-level files & folders first.
    // But that can fail -- e.g. `func` might refuse to remove nonempty
    // directories -- so plan B is to remove everything bottom-up.
    depth_first_walk(tmpdir, func_wrapper, 1);
    depth_first_walk(tmpdir, func_wrapper);
    tr_sys_path_remove(tmpdir);

    // OK we've removed the local data.
    // What's left are empty folders, junk, and user-generated files.
    // Remove the first two categories and leave the third alone.
    auto const remove_junk = [](char const* filename)
    {
        if (is_empty_folder(filename) || is_junk_file(filename))
        {
            tr_sys_path_remove(filename);
        }
    };
    for (auto const& filename : top_files)
    {
        depth_first_walk(filename.c_str(), remove_junk);
    }
}

namespace
{

// `is_unix_reserved_file` and `is_win32_reserved_file` kept as `maybe_unused`
// for potential support of different filesystems on the same OS
[[nodiscard, maybe_unused]] bool is_unix_reserved_file(std::string_view in) noexcept
{
    static auto constexpr ReservedNames = std::array<std::string_view, 2>{
        "."sv,
        ".."sv,
    };
    return (std::find(std::begin(ReservedNames), std::end(ReservedNames), in) != std::end(ReservedNames));
}

// https://docs.microsoft.com/en-us/windows/win32/fileio/naming-a-file
// Do not use the following reserved names for the name of a file:
// CON, PRN, AUX, NUL, COM1, COM2, COM3, COM4, COM5, COM6, COM7, COM8,
// COM9, LPT1, LPT2, LPT3, LPT4, LPT5, LPT6, LPT7, LPT8, and LPT9.
// Also avoid these names followed immediately by an extension;
// for example, NUL.txt is not recommended.
[[nodiscard, maybe_unused]] bool is_win32_reserved_file(std::string_view in) noexcept
{
    if (std::empty(in))
    {
        return false;
    }

    // Shortcut to avoid extra work below.
    // All the paths below involve filenames that begin with one of these chars
    static auto constexpr ReservedFilesBeginWithOneOf = "ACLNP"sv;
    if (ReservedFilesBeginWithOneOf.find(toupper(in.front())) == std::string_view::npos)
    {
        return false;
    }

    auto in_upper = tr_pathbuf{ in };
    std::transform(std::begin(in_upper), std::end(in_upper), std::begin(in_upper), [](auto ch) { return toupper(ch); });
    auto const in_upper_sv = in_upper.sv();

    static auto constexpr ReservedNames = std::array<std::string_view, 22>{
        "AUX"sv,  "CON"sv,  "NUL"sv,  "PRN"sv, //
        "COM1"sv, "COM2"sv, "COM3"sv, "COM4"sv, "COM5"sv, "COM6"sv, "COM7"sv, "COM8"sv, "COM9"sv, //
        "LPT1"sv, "LPT2"sv, "LPT3"sv, "LPT4"sv, "LPT5"sv, "LPT6"sv, "LPT7"sv, "LPT8"sv, "LPT9"sv, //
    };
    if (std::find(std::begin(ReservedNames), std::end(ReservedNames), in_upper_sv) != std::end(ReservedNames))
    {
        return true;
    }

    static auto constexpr ReservedPrefixes = std::array<std::string_view, 22>{
        "AUX."sv,  "CON."sv,  "NUL."sv,  "PRN."sv, //
        "COM1."sv, "COM2."sv, "COM3."sv, "COM4."sv, "COM5."sv, "COM6."sv, "COM7."sv, "COM8."sv, "COM9."sv, //
        "LPT1."sv, "LPT2."sv, "LPT3."sv, "LPT4."sv, "LPT5."sv, "LPT6."sv, "LPT7."sv, "LPT8."sv, "LPT9."sv, //
    };
    return std::any_of(
        std::begin(ReservedPrefixes),
        std::end(ReservedPrefixes),
        [in_upper_sv](auto const& prefix) { return tr_strv_starts_with(in_upper_sv, prefix); });
}

[[nodiscard]] bool is_reserved_file(std::string_view in, bool os_specific) noexcept
{
    if (!os_specific)
    {
        return is_unix_reserved_file(in) || is_win32_reserved_file(in);
    }
#ifdef _WIN32
    return is_win32_reserved_file(in);
#else
    return is_unix_reserved_file(in);
#endif
}

// `is_unix_reserved_char` and `is_win32_reserved_char` kept as `maybe_unused`
// for potential support of different filesystems on the same OS
[[nodiscard, maybe_unused]] auto constexpr is_unix_reserved_char(unsigned char ch) noexcept
{
    return ch == '/';
}

// https://docs.microsoft.com/en-us/windows/desktop/FileIO/naming-a-file
// Use any character in the current code page for a name, including Unicode
// characters and characters in the extended character set (128–255),
// except for the following:
[[nodiscard, maybe_unused]] auto constexpr is_win32_reserved_char(unsigned char ch) noexcept
{
    switch (ch)
    {
    case '"':
    case '*':
    case '/':
    case ':':
    case '<':
    case '>':
    case '?':
    case '\\':
    case '|':
        return true;
    default:
        return ch <= 31;
    }
}

[[nodiscard]] auto constexpr is_reserved_char(unsigned char ch, bool os_specific) noexcept
{
    if (!os_specific)
    {
        return is_unix_reserved_char(ch) || is_win32_reserved_char(ch);
    }
#ifdef _WIN32
    return is_win32_reserved_char(ch);
#else
    return is_unix_reserved_char(ch);
#endif
}

// https://en.wikipedia.org/wiki/Filename#Comparison_of_filename_limitations
void append_sanitized_component(std::string_view in, tr_pathbuf& out, bool os_specific)
{
#ifdef _WIN32
    // remove leading and trailing spaces
    in = tr_strv_strip(in);

    // remove trailing periods
    while (tr_strv_ends_with(in, '.'))
    {
        in.remove_suffix(1);
    }
#endif

    // replace reserved filenames with an underscore
    if (is_reserved_file(in, os_specific))
    {
        out.append('_');
    }

    // replace reserved characters with an underscore
    auto const add_char = [os_specific](auto ch)
    {
        return is_reserved_char(ch, os_specific) ? '_' : ch;
    };
    std::transform(std::begin(in), std::end(in), std::back_inserter(out), add_char);
}

} // namespace

void tr_torrent_files::sanitize_subpath(std::string_view path, tr_pathbuf& append_me, bool os_specific)
{
    auto segment = std::string_view{};
    while (tr_strv_sep(&path, &segment, '/'))
    {
        append_sanitized_component(segment, append_me, os_specific);
        append_me.append('/');
    }

    if (auto const n = std::size(append_me); n > 0)
    {
        append_me.resize(n - 1); // remove trailing slash
    }
}
