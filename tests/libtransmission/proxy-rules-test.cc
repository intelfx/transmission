// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <string_view>

#include <libtransmission/proxy-rules.h>

#include "gtest/gtest.h"

using namespace std::literals;

TEST(ProxyRulesTest, parsesRules)
{
    auto const rules = tr_proxy_rules{
        "# comment\n"
        "*.onion/*   socks5h://127.0.0.1:9050\n"
        "\n"
        "  udp://tracker.example:*   DIRECT  \r\n"
        "*://*.example.org/*\thttp://proxy.example:3128"sv,
    };

    ASSERT_EQ(3U, std::size(rules.rules()));
    EXPECT_EQ("*.onion/*"sv, rules.rules()[0].pattern);
    EXPECT_EQ("socks5h://127.0.0.1:9050"sv, rules.rules()[0].proxy_url.value_or(""));
    EXPECT_EQ("udp://tracker.example:*"sv, rules.rules()[1].pattern);
    EXPECT_FALSE(rules.rules()[1].proxy_url);
    EXPECT_EQ("*://*.example.org/*"sv, rules.rules()[2].pattern);
    EXPECT_EQ("http://proxy.example:3128"sv, rules.rules()[2].proxy_url.value_or(""));

    for (auto const& rule : rules.rules())
    {
        EXPECT_EQ(tr_proxy_rules::DefaultKinds, rule.kinds);
    }
}

TEST(ProxyRulesTest, skipsMalformedLines)
{
    auto const rules = tr_proxy_rules{
        "lonely-pattern\n"
        "too many fields\n"
        "* direct\n"sv,
    };

    ASSERT_EQ(1U, std::size(rules.rules()));
    EXPECT_EQ("*"sv, rules.rules()[0].pattern);
}

TEST(ProxyRulesTest, keepsRulesWithInvalidProxy)
{
    auto const rules = tr_proxy_rules{ "* ftp://proxy.example"sv };

    ASSERT_EQ(1U, std::size(rules.rules()));
    EXPECT_EQ("ftp://proxy.example"sv, rules.rules()[0].proxy_url.value_or(""));
}

TEST(ProxyRulesTest, firstMatchWins)
{
    auto const rules = tr_proxy_rules{
        "http://tracker.example/* direct\n"
        "http://*.example/* socks5://127.0.0.1:1080\n"
        "* http://fallback.example\n"sv,
    };

    auto const* rule = rules.match("http://tracker.example/announce?info_hash=x"sv, tr_request_kind::Tracker);
    ASSERT_NE(nullptr, rule);
    EXPECT_FALSE(rule->proxy_url);

    rule = rules.match("http://other.example/announce"sv, tr_request_kind::Tracker);
    ASSERT_NE(nullptr, rule);
    EXPECT_EQ("socks5://127.0.0.1:1080"sv, rule->proxy_url.value_or(""));

    rule = rules.match("udp://tracker.example:6969"sv, tr_request_kind::Tracker);
    ASSERT_NE(nullptr, rule);
    EXPECT_EQ("http://fallback.example"sv, rule->proxy_url.value_or(""));
}

TEST(ProxyRulesTest, noMatch)
{
    auto const rules = tr_proxy_rules{ "*.onion/* socks5h://127.0.0.1:9050"sv };

    EXPECT_EQ(nullptr, rules.match("https://tracker.example/announce"sv, tr_request_kind::Tracker));
    EXPECT_EQ(nullptr, tr_proxy_rules{}.match("https://tracker.example/announce"sv, tr_request_kind::Tracker));
}

TEST(ProxyRulesTest, appliesToAllRequestKindsByDefault)
{
    auto const rules = tr_proxy_rules{ "* socks5://127.0.0.1:1080"sv };

    for (auto const kind : { tr_request_kind::Tracker,
                             tr_request_kind::Webseed,
                             tr_request_kind::Blocklist,
                             tr_request_kind::TorrentFile,
                             tr_request_kind::PortCheck,
                             tr_request_kind::PublicAddress,
                             tr_request_kind::Other })
    {
        EXPECT_NE(nullptr, rules.match("http://example.com/"sv, kind));
    }
}

TEST(ProxyRulesTest, preservesText)
{
    auto constexpr Text = "# keep me\n*  direct\n"sv;
    EXPECT_EQ(Text, tr_proxy_rules{ Text }.text());
    EXPECT_EQ(tr_proxy_rules{ Text }, tr_proxy_rules{ Text });
    EXPECT_NE(tr_proxy_rules{ Text }, tr_proxy_rules{ "* direct"sv });
}
