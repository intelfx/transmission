// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "libtransmission/proxy-url.h"
#include "libtransmission/utils.h"
#include "libtransmission/web-utils.h"

using namespace std::literals;

namespace
{
struct SchemeInfo
{
    std::string_view name;
    tr_proxy_url::Scheme scheme;
    uint16_t default_port;
};

// Same schemes and default ports as CURLOPT_PROXY.
auto constexpr Schemes = std::array<SchemeInfo, 6U>{ {
    { "http"sv, tr_proxy_url::Scheme::Http, 1080U },
    { "https"sv, tr_proxy_url::Scheme::Https, 443U },
    { "socks4"sv, tr_proxy_url::Scheme::Socks4, 1080U },
    { "socks4a"sv, tr_proxy_url::Scheme::Socks4a, 1080U },
    { "socks5"sv, tr_proxy_url::Scheme::Socks5, 1080U },
    { "socks5h"sv, tr_proxy_url::Scheme::Socks5h, 1080U },
} };
} // namespace

std::optional<tr_proxy_url> tr_proxy_url::parse(std::string_view url)
{
    url = tr_strv_strip(url);

    auto ret = tr_proxy_url{};
    auto default_port = uint16_t{ 1080U };

    // scheme (curl assumes http:// if there is none)
    if (auto const pos = url.find("://"sv); pos != std::string_view::npos)
    {
        auto const name = tr_strlower(url.substr(0, pos));
        auto const it = std::find_if(
            std::begin(Schemes),
            std::end(Schemes),
            [&name](auto const& info) { return info.name == name; });
        if (it == std::end(Schemes))
        {
            return {};
        }

        ret.scheme = it->scheme;
        default_port = it->default_port;
        url.remove_prefix(pos + 3U);
    }

    // ignore any trailing path
    url = url.substr(0, url.find('/'));

    // userinfo
    if (auto const pos = url.rfind('@'); pos != std::string_view::npos)
    {
        auto const userinfo = url.substr(0, pos);
        url.remove_prefix(pos + 1U);

        auto const colon = userinfo.find(':');
        ret.username = tr_urlPercentDecode(userinfo.substr(0, colon));
        if (colon != std::string_view::npos)
        {
            ret.password = tr_urlPercentDecode(userinfo.substr(colon + 1U));
        }
    }

    // host
    if (tr_strv_starts_with(url, '['))
    {
        auto const pos = url.find(']');
        if (pos == std::string_view::npos)
        {
            return {};
        }

        ret.host = url.substr(1U, pos - 1U);
        url.remove_prefix(pos + 1U);
    }
    else
    {
        auto const pos = url.find(':');
        ret.host = url.substr(0, pos);
        url.remove_prefix(std::size(ret.host));
    }

    if (std::empty(ret.host))
    {
        return {};
    }

    // port
    if (std::empty(url))
    {
        ret.port = default_port;
    }
    else if (tr_strv_starts_with(url, ':'))
    {
        url.remove_prefix(1U);
        auto const port = tr_num_parse<uint16_t>(url);
        if (!port || *port == 0U)
        {
            return {};
        }

        ret.port = *port;
    }
    else
    {
        return {};
    }

    return ret;
}
