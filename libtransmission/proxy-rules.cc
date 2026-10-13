// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <string>
#include <string_view>

#include <fmt/format.h>

#include "libtransmission/log.h"
#include "libtransmission/proxy-rules.h"
#include "libtransmission/proxy-url.h"
#include "libtransmission/utils.h"

using namespace std::literals;

namespace
{
auto constexpr Whitespace = " \t\r"sv;

[[nodiscard]] std::string_view next_token(std::string_view* line)
{
    auto const begin = line->find_first_not_of(Whitespace);
    if (begin == std::string_view::npos)
    {
        *line = {};
        return {};
    }

    line->remove_prefix(begin);
    auto const end = line->find_first_of(Whitespace);
    auto const token = line->substr(0, end);
    line->remove_prefix(std::size(token));
    return token;
}
} // namespace

tr_proxy_rules::tr_proxy_rules(std::string_view text)
    : text_{ text }
{
    auto line = std::string_view{};
    while (tr_strv_sep(&text, &line, '\n'))
    {
        auto const original_line = tr_strv_strip(line);
        auto const pattern = next_token(&line);
        if (std::empty(pattern) || tr_strv_starts_with(pattern, '#'))
        {
            continue;
        }

        auto const target = next_token(&line);
        if (std::empty(target) || !std::empty(next_token(&line)))
        {
            tr_logAddWarn(
                fmt::format("Ignoring invalid proxy rule {:?}: expected '<pattern> <proxy-url|direct>'", original_line));
            continue;
        }

        auto& rule = rules_.emplace_back();
        rule.pattern = pattern;

        if (tr_strlower(target) == "direct"sv)
        {
            continue;
        }

        // Keep rules with unparseable proxies, so that requests which match them fail
        // instead of falling through to a less specific rule.
        if (!tr_proxy_url::parse(target))
        {
            tr_logAddWarn(fmt::format("Proxy rule {:?} has an invalid proxy URL {:?}", pattern, target));
        }

        rule.proxy_url = target;
    }
}

tr_proxy_rules::Rule const* tr_proxy_rules::match(std::string_view url, tr_request_kind kind) const
{
    auto const szurl = std::string{ url };

    for (auto const& rule : rules_)
    {
        if ((rule.kinds & mask(kind)) != 0U && tr_wildmat(szurl.c_str(), rule.pattern.c_str()))
        {
            return &rule;
        }
    }

    return nullptr;
}
