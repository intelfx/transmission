// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// A proxy URL as accepted by CURLOPT_PROXY: `[scheme://][user[:password]@]host[:port]`.
// Parsed here so that non-curl transports (e.g. the SOCKS5 UDP relay) interpret it the same way.
struct tr_proxy_url
{
    enum class Scheme : uint8_t
    {
        Http,
        Https,
        Socks4,
        Socks4a,
        Socks5,
        Socks5h,
    };

    [[nodiscard]] static std::optional<tr_proxy_url> parse(std::string_view url);

    // Whether the proxy protocol can carry UDP datagrams (SOCKS5 UDP ASSOCIATE).
    [[nodiscard]] constexpr bool supports_udp() const noexcept
    {
        return scheme == Scheme::Socks5 || scheme == Scheme::Socks5h;
    }

    // Whether destination hostnames are resolved by the proxy rather than locally.
    [[nodiscard]] constexpr bool resolves_remotely() const noexcept
    {
        return scheme == Scheme::Socks4a || scheme == Scheme::Socks5h;
    }

    Scheme scheme = Scheme::Http;
    std::string host; // without IPv6 brackets
    uint16_t port = 0;
    std::optional<std::string> username;
    std::optional<std::string> password;
};
