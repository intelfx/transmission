// This file Copyright (C) 2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef> // size_t
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h> // chmod()
#include <unistd.h> // geteuid()
#endif

#include <libtransmission/transmission.h>

#include <libtransmission/error.h>
#include <libtransmission/file.h>
#include <libtransmission/torrent-files.h>
#include <libtransmission/torrent-metainfo.h>
#include "libtransmission/tr-macros.h"
#include <libtransmission/tr-strbuf.h>
#include <libtransmission/utils.h>

#include "gtest/gtest.h"
#include "test-fixtures.h"

using namespace std::literals;

class TorrentFilesTest : public ::libtransmission::test::SandboxedTest
{
protected:
    [[nodiscard]] static std::optional<std::string> readFile(std::string_view filename)
    {
        auto contents = std::vector<char>{};
        if (!tr_file_read(filename, contents))
        {
            return {};
        }
        return std::string{ std::data(contents), std::size(contents) };
    }

    // Creates each file under `base`, holding its own subpath as contents.
    static void createFiles(std::string_view base, std::initializer_list<std::string_view> subpaths)
    {
        for (auto const subpath : subpaths)
        {
            createFileWithContents(tr_pathbuf{ base, '/', subpath }, subpath);
        }
    }

    static void expectFile(std::string_view base, std::string_view subpath, std::string_view contents)
    {
        EXPECT_EQ(std::optional<std::string>{ contents }, readFile(tr_pathbuf{ base, '/', subpath })) << subpath;
    }

    static void expectNoFile(std::string_view base, std::string_view subpath)
    {
        EXPECT_FALSE(tr_sys_path_exists(tr_pathbuf{ base, '/', subpath })) << subpath;
    }

    [[nodiscard]] bool relocate(tr_torrent_files const& files, std::vector<std::string> const& new_paths, tr_error* error) const
    {
        auto const base = sandboxDir();
        auto const bases = std::array<std::string_view, 1U>{ base };
        return files.relocate(new_paths, std::data(bases), std::data(bases), std::size(bases), error);
    }
};

TEST_F(TorrentFilesTest, add)
{
    auto constexpr Path = "/hello/world"sv;
    auto constexpr Size = size_t{ 1024 };

    auto files = tr_torrent_files{};
    EXPECT_EQ(size_t{ 0U }, files.file_count());
    EXPECT_TRUE(std::empty(files));

    auto const file_index = files.add(Path, Size);
    EXPECT_EQ(tr_file_index_t{ 0U }, file_index);
    EXPECT_EQ(size_t{ 1U }, files.file_count());
    EXPECT_EQ(Size, files.file_size(file_index));
    EXPECT_EQ(Path, files.path(file_index));
    EXPECT_FALSE(std::empty(files));
}

TEST_F(TorrentFilesTest, setPath)
{
    auto constexpr Path1 = "/hello/world"sv;
    auto constexpr Path2 = "/hello/there"sv;
    auto constexpr Size = size_t{ 2048 };

    auto files = tr_torrent_files{};
    auto const file_index = files.add(Path1, Size);
    EXPECT_EQ(Path1, files.path(file_index));
    EXPECT_EQ(Size, files.file_size(file_index));

    files.set_path(file_index, Path2);
    EXPECT_EQ(Path2, files.path(file_index));
    EXPECT_EQ(Size, files.file_size(file_index));
}

TEST_F(TorrentFilesTest, clear)
{
    auto constexpr Path1 = "/hello/world"sv;
    auto constexpr Path2 = "/hello/there"sv;
    auto constexpr Size = size_t{ 2048 };

    auto files = tr_torrent_files{};
    files.add(Path1, Size);
    EXPECT_EQ(size_t{ 1U }, files.file_count());
    files.add(Path2, Size);
    EXPECT_EQ(size_t{ 2U }, files.file_count());

    files.clear();
    EXPECT_TRUE(std::empty(files));
    EXPECT_EQ(size_t{ 0U }, files.file_count());
}

TEST_F(TorrentFilesTest, find)
{
    static auto constexpr Contents = "hello"sv;
    auto const filename = tr_pathbuf{ sandboxDir(), "/first_dir/hello.txt"sv };
    createFileWithContents(std::string{ filename }, std::data(Contents), std::size(Contents));

    auto files = tr_torrent_files{};
    auto const file_index = files.add("first_dir/hello.txt", 1024);

    auto const search_path_1 = tr_pathbuf{ sandboxDir() };
    auto const search_path_2 = tr_pathbuf{ "/tmp"sv };

    auto search_path = std::vector<std::string_view>{ search_path_1.sv(), search_path_2.sv() };
    auto found = files.find(file_index, std::data(search_path), std::size(search_path));
    EXPECT_TRUE(found.has_value());
    assert(found.has_value());
    EXPECT_EQ(filename, found->filename());

    // same search, but with the search paths reversed
    search_path = std::vector<std::string_view>{ search_path_2.sv(), search_path_1.sv() };
    found = files.find(file_index, std::data(search_path), std::size(search_path));
    EXPECT_TRUE(found.has_value());
    assert(found.has_value());
    EXPECT_EQ(filename, found->filename());

    // now make it an incomplete file
    auto const partial_filename = tr_pathbuf{ filename, tr_torrent_files::PartialFileSuffix };
    EXPECT_TRUE(tr_sys_path_rename(filename, partial_filename));
    search_path = std::vector<std::string_view>{ search_path_1.sv(), search_path_2.sv() };
    found = files.find(file_index, std::data(search_path), std::size(search_path));
    EXPECT_TRUE(found.has_value());
    assert(found.has_value());
    EXPECT_EQ(partial_filename, found->filename());

    // same search, but with the search paths reversed
    search_path = std::vector<std::string_view>{ search_path_2.sv(), search_path_1.sv() };
    found = files.find(file_index, std::data(search_path), std::size(search_path));
    EXPECT_TRUE(found.has_value());
    assert(found.has_value());
    EXPECT_EQ(partial_filename, found->filename());

    // what about if we look for a file that does not exist
    EXPECT_TRUE(tr_sys_path_remove(partial_filename));
    EXPECT_FALSE(files.find(file_index, std::data(search_path), std::size(search_path)));
}

TEST_F(TorrentFilesTest, hasAnyLocalData)
{
    static auto constexpr Contents = "hello"sv;
    auto const filename = tr_pathbuf{ sandboxDir(), "/first_dir/hello.txt"sv };
    createFileWithContents(std::string{ filename }, std::data(Contents), std::size(Contents));

    auto files = tr_torrent_files{};
    files.add("first_dir/hello.txt", 1024);

    auto const search_path_1 = tr_pathbuf{ sandboxDir() };
    auto const search_path_2 = tr_pathbuf{ "/tmp"sv };

    auto search_path = std::vector<std::string_view>{ search_path_1.sv(), search_path_2.sv() };
    EXPECT_TRUE(files.has_any_local_data(std::data(search_path), 2U));
    EXPECT_TRUE(files.has_any_local_data(std::data(search_path), 1U));
    EXPECT_FALSE(files.has_any_local_data(std::data(search_path) + 1, 1U));
    EXPECT_FALSE(files.has_any_local_data(std::data(search_path), 0U));
}

TEST_F(TorrentFilesTest, mimeType)
{
    auto const filename = tr_pathbuf{ LIBTRANSMISSION_TEST_ASSETS_DIR, "/alice_in_wonderland_librivox_archive.torrent"sv };
    auto metainfo = tr_torrent_metainfo{};
    EXPECT_TRUE(metainfo.parse_torrent_file(filename));
    EXPECT_EQ("audio/mpeg"sv, metainfo.files().primary_mime_type());
}

TEST_F(TorrentFilesTest, mimeTypeVideoMp4)
{
    auto files = tr_torrent_files{};
    files.add("name/name.mp4"sv, 4'500'000'000U);
    files.add("name/name.info"sv, 2048U);
    files.add("name/SHA512sum"sv, 139U);
    EXPECT_EQ("video/mp4"sv, files.primary_mime_type());
}

TEST_F(TorrentFilesTest, isSubpathPortable)
{
    static auto constexpr NotWin32 = TR_IF_WIN32(false, true);

    static auto constexpr Tests = std::array<std::pair<std::string_view, bool>, 18>{ {
        // never portable
        { ".", false },
        { "..", false },

        // don't end with periods
        { "foo.", NotWin32 },
        { "foo..", NotWin32 },

        // don't begin or end with whitespace
        { " foo ", NotWin32 },
        { " foo", NotWin32 },
        { "foo ", NotWin32 },

        // reserved names
        { "COM1", NotWin32 },
        { "COM1.txt", NotWin32 },
        { "Com1", NotWin32 },
        { "com1", NotWin32 },

        // reserved characters
        { "hell:o.txt", NotWin32 },
        { "hell\to.txt", NotWin32 },

        // everything else
        { ".foo", true },
        { "com99.txt", true },
        { "foo", true },
        { "hello.txt", true },
        { "hello#.txt", true },
    } };

    for (auto const& [subpath, expected] : Tests)
    {
        EXPECT_EQ(expected, tr_torrent_files::is_subpath_sanitized(subpath)) << " subpath " << subpath;
    }
}

TEST_F(TorrentFilesTest, resolvePaths)
{
    auto files = tr_torrent_files{};
    files.add("top/a/1"sv, 1U);
    files.add("top/a/2"sv, 1U);
    files.add("top/b"sv, 1U);

    using Paths = std::vector<std::optional<std::string>>;
    auto error = tr_error{};

    // nullopt keeps a file's current path
    auto resolved = files.resolve_paths(Paths{ "1", std::nullopt, "x/y/b" }, &error);
    EXPECT_FALSE(error) << error;
    EXPECT_EQ((std::optional<std::vector<std::string>>{ { "1", "top/a/2", "x/y/b" } }), resolved);

    // one path per file
    EXPECT_FALSE(files.resolve_paths(Paths{ "1", "2" }, &error));
    EXPECT_EQ(EINVAL, error.code());
    error = {};

    // only relative paths without empty, '.' or '..' components
    for (auto const* const bad : { "", "/1", "1/", "a//1", "a/./1", "a/../1", "../1", ".", ".." })
    {
        EXPECT_FALSE(files.resolve_paths(Paths{ bad, std::nullopt, std::nullopt }, &error)) << bad;
        EXPECT_EQ(EINVAL, error.code()) << bad;
        error = {};
    }

    // no two files at the same path
    EXPECT_FALSE(files.resolve_paths(Paths{ "top/b", std::nullopt, std::nullopt }, &error));
    EXPECT_EQ(EINVAL, error.code());
    error = {};

    // no file at another file's directory
    EXPECT_FALSE(files.resolve_paths(Paths{ "top/b/1", std::nullopt, std::nullopt }, &error));
    EXPECT_EQ(EINVAL, error.code());
    error = {};
    EXPECT_FALSE(files.resolve_paths(Paths{ std::nullopt, std::nullopt, "top/a" }, &error));
    EXPECT_EQ(EINVAL, error.code());
    error = {};
}

TEST_F(TorrentFilesTest, relocateAcrossDirectories)
{
    auto const base = sandboxDir();
    createFiles(base, { "top/a/1", "top/a/2", "top/b/3", "top/a/foreign" });

    auto files = tr_torrent_files{};
    files.add("top/a/1"sv, 1U);
    files.add("top/a/2"sv, 1U);
    files.add("top/b/3"sv, 1U);
    files.add("top/c/4"sv, 1U); // no local data

    auto error = tr_error{};
    EXPECT_TRUE(relocate(files, { "1", "x/y/2", "top/b/3", "z/4" }, &error));
    EXPECT_FALSE(error) << error;

    expectFile(base, "1", "top/a/1");
    expectFile(base, "x/y/2", "top/a/2");
    expectFile(base, "top/b/3", "top/b/3");
    expectNoFile(base, "z");

    // files of others are not moved, and their directories are kept
    expectFile(base, "top/a/foreign", "top/a/foreign");
    expectNoFile(base, "top/a/1");
    expectNoFile(base, "top/a/2");
}

TEST_F(TorrentFilesTest, relocatePrunesEmptiedDirectories)
{
    auto const base = sandboxDir();
    createFiles(base, { "top/a/1", "top/a/b/2" });

    auto files = tr_torrent_files{};
    files.add("top/a/1"sv, 1U);
    files.add("top/a/b/2"sv, 1U);

    auto error = tr_error{};
    EXPECT_TRUE(relocate(files, { "1", "2" }, &error));
    EXPECT_FALSE(error) << error;
    expectFile(base, "1", "top/a/1");
    expectFile(base, "2", "top/a/b/2");
    expectNoFile(base, "top");
    EXPECT_TRUE(tr_sys_path_exists(base));
}

TEST_F(TorrentFilesTest, relocateFilesInTheWay)
{
    auto const base = sandboxDir();
    createFiles(base, { "a", "b", "X", "Z/z" });

    auto files = tr_torrent_files{};
    files.add("a"sv, 1U);
    files.add("b"sv, 1U);
    files.add("X"sv, 1U);
    files.add("Z/z"sv, 1U);

    // swap two files, and turn a file into a directory of the same name
    auto error = tr_error{};
    EXPECT_TRUE(relocate(files, { "b", "a", "X/x", "X/z" }, &error));
    EXPECT_FALSE(error) << error;
    expectFile(base, "a", "b");
    expectFile(base, "b", "a");
    expectFile(base, "X/x", "X");
    expectFile(base, "X/z", "Z/z");
    expectNoFile(base, "Z");
}

TEST_F(TorrentFilesTest, relocateKeepsPartialSuffix)
{
    auto const base = sandboxDir();
    createFiles(base, { "a/1.part", "a/2" });

    auto files = tr_torrent_files{};
    files.add("a/1"sv, 1U);
    files.add("a/2"sv, 1U);

    auto error = tr_error{};
    EXPECT_TRUE(relocate(files, { "b/1", "b/2" }, &error));
    EXPECT_FALSE(error) << error;
    expectFile(base, "b/1.part", "a/1.part");
    expectFile(base, "b/2", "a/2");
    expectNoFile(base, "b/1");
}

TEST_F(TorrentFilesTest, relocateBetweenBases)
{
    auto const download_dir = tr_pathbuf{ sandboxDir(), "/Downloads"sv };
    auto const incomplete_dir = tr_pathbuf{ sandboxDir(), "/Incomplete"sv };
    auto const new_download_dir = tr_pathbuf{ sandboxDir(), "/Downloads/top"sv };
    createFiles(download_dir, { "top/1" });
    createFiles(incomplete_dir, { "top/2.part" });

    auto files = tr_torrent_files{};
    files.add("top/1"sv, 1U);
    files.add("top/2"sv, 1U);

    auto const old_bases = std::array<std::string_view, 2U>{ download_dir, incomplete_dir };
    auto const new_bases = std::array<std::string_view, 2U>{ new_download_dir, incomplete_dir };
    auto error = tr_error{};
    EXPECT_TRUE(files.relocate({ "1", "2" }, std::data(old_bases), std::data(new_bases), std::size(old_bases), &error));
    EXPECT_FALSE(error) << error;

    // the file in the download dir stays where it is,
    // but the one in the incomplete dir loses its top-level directory
    expectFile(new_download_dir, "1", "top/1");
    expectFile(incomplete_dir, "2.part", "top/2.part");
    expectNoFile(incomplete_dir, "top");
}

TEST_F(TorrentFilesTest, relocateRefusesToOverwrite)
{
    auto const base = sandboxDir();
    createFiles(base, { "a", "b", "foreign" });

    auto files = tr_torrent_files{};
    files.add("a"sv, 1U);
    files.add("b"sv, 1U);

    auto error = tr_error{};
    EXPECT_FALSE(relocate(files, { "c", "foreign" }, &error));
    EXPECT_EQ(EEXIST, error.code());
    expectFile(base, "a", "a");
    expectFile(base, "b", "b");
    expectFile(base, "foreign", "foreign");
    expectNoFile(base, "c");
}

#ifndef _WIN32
TEST_F(TorrentFilesTest, relocateRollsBackOnFailure)
{
    if (geteuid() == 0)
    {
        GTEST_SKIP() << "root ignores directory permissions";
    }

    auto const base = sandboxDir();
    createFiles(base, { "a", "b", "c" });
    auto const read_only_dir = tr_pathbuf{ base, "/read-only"sv };
    EXPECT_TRUE(tr_sys_dir_create(read_only_dir, 0, 0555));

    auto files = tr_torrent_files{};
    files.add("a"sv, 1U);
    files.add("b"sv, 1U);
    files.add("c"sv, 1U);

    // swap "a" and "b", then fail to move "c"
    auto error = tr_error{};
    EXPECT_FALSE(relocate(files, { "moved/b", "a", "read-only/c" }, &error));
    EXPECT_TRUE(error);
    EXPECT_EQ(std::string_view::npos, error.message().find("moved back")) << error;
    expectFile(base, "a", "a");
    expectFile(base, "b", "b");
    expectFile(base, "c", "c");
    expectNoFile(base, "moved");

    chmod(read_only_dir, 0755);
}
#endif
