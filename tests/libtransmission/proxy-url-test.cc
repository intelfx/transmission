// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <string_view>

#include <libtransmission/proxy-url.h>

#include "gtest/gtest.h"

using namespace std::literals;
using Scheme = tr_proxy_url::Scheme;

TEST(ProxyUrlTest, parsesSchemeHostAndPort)
{
    auto const parsed = tr_proxy_url::parse("socks5://proxy.example:9050"sv);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(Scheme::Socks5, parsed->scheme);
    EXPECT_EQ("proxy.example"sv, parsed->host);
    EXPECT_EQ(9050U, parsed->port);
    EXPECT_FALSE(parsed->username);
    EXPECT_FALSE(parsed->password);
    EXPECT_TRUE(parsed->supports_udp());
    EXPECT_FALSE(parsed->resolves_remotely());
}

TEST(ProxyUrlTest, usesDefaultPort)
{
    auto parsed = tr_proxy_url::parse("socks5://127.0.0.1"sv);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(1080U, parsed->port);

    parsed = tr_proxy_url::parse("https://proxy.example"sv);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(443U, parsed->port);
}

TEST(ProxyUrlTest, defaultsToHttp)
{
    auto const parsed = tr_proxy_url::parse("proxy.example:3128"sv);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(Scheme::Http, parsed->scheme);
    EXPECT_EQ("proxy.example"sv, parsed->host);
    EXPECT_EQ(3128U, parsed->port);
    EXPECT_FALSE(parsed->supports_udp());
}

TEST(ProxyUrlTest, schemeIsCaseInsensitive)
{
    auto const parsed = tr_proxy_url::parse("SOCKS5H://proxy.example"sv);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(Scheme::Socks5h, parsed->scheme);
    EXPECT_TRUE(parsed->supports_udp());
    EXPECT_TRUE(parsed->resolves_remotely());
}

TEST(ProxyUrlTest, parsesUserinfo)
{
    auto parsed = tr_proxy_url::parse("socks5://us%40er:p%3Ass:word@proxy.example:1081"sv);
    ASSERT_TRUE(parsed);
    EXPECT_EQ("us@er"sv, parsed->username.value_or(""));
    EXPECT_EQ("p:ss:word"sv, parsed->password.value_or(""));
    EXPECT_EQ("proxy.example"sv, parsed->host);
    EXPECT_EQ(1081U, parsed->port);

    parsed = tr_proxy_url::parse("socks5://user@proxy.example"sv);
    ASSERT_TRUE(parsed);
    EXPECT_EQ("user"sv, parsed->username.value_or(""));
    EXPECT_FALSE(parsed->password);
}

TEST(ProxyUrlTest, parsesIpv6Literal)
{
    auto parsed = tr_proxy_url::parse("socks5://[::1]:1080"sv);
    ASSERT_TRUE(parsed);
    EXPECT_EQ("::1"sv, parsed->host);
    EXPECT_EQ(1080U, parsed->port);

    parsed = tr_proxy_url::parse("socks5://[2001:db8::1]"sv);
    ASSERT_TRUE(parsed);
    EXPECT_EQ("2001:db8::1"sv, parsed->host);
    EXPECT_EQ(1080U, parsed->port);
}

TEST(ProxyUrlTest, ignoresTrailingPath)
{
    auto const parsed = tr_proxy_url::parse("http://proxy.example:8080/"sv);
    ASSERT_TRUE(parsed);
    EXPECT_EQ("proxy.example"sv, parsed->host);
    EXPECT_EQ(8080U, parsed->port);
}

TEST(ProxyUrlTest, rejectsInvalid)
{
    EXPECT_FALSE(tr_proxy_url::parse(""sv));
    EXPECT_FALSE(tr_proxy_url::parse("socks5://"sv));
    EXPECT_FALSE(tr_proxy_url::parse("ftp://proxy.example"sv));
    EXPECT_FALSE(tr_proxy_url::parse("socks5://proxy.example:0"sv));
    EXPECT_FALSE(tr_proxy_url::parse("socks5://proxy.example:65536"sv));
    EXPECT_FALSE(tr_proxy_url::parse("socks5://proxy.example:port"sv));
    EXPECT_FALSE(tr_proxy_url::parse("socks5://[::1"sv));
    EXPECT_FALSE(tr_proxy_url::parse("socks5://[::1]x"sv));
}
