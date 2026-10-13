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
#include <vector>

#include "libtransmission/web.h" // for tr_request_kind

// User-defined rules for choosing a proxy based on the URL being requested.
//
// The text form has one rule per line: `<glob> <proxy-url|direct>`.
// The glob is matched against the full URL; the first matching rule wins.
// `direct` means that no proxy is used. Empty lines and lines starting with `#` are ignored.
class tr_proxy_rules
{
public:
    using KindMask = uint32_t;

    static constexpr KindMask mask(tr_request_kind kind) noexcept
    {
        return KindMask{ 1U } << static_cast<unsigned>(kind);
    }

    static constexpr auto AllKinds = ~KindMask{};

    // The kinds of requests that a rule applies to.
    // The text form has no way to set this per rule (yet), so all rules use this.
    static constexpr auto DefaultKinds = AllKinds;

    struct Rule
    {
        std::string pattern;
        std::optional<std::string> proxy_url; // nullopt: connect directly
        KindMask kinds = DefaultKinds;
    };

    tr_proxy_rules() = default;

    // Invalid lines are skipped with a warning.
    explicit tr_proxy_rules(std::string_view text);

    [[nodiscard]] Rule const* match(std::string_view url, tr_request_kind kind) const;

    [[nodiscard]] auto const& rules() const noexcept
    {
        return rules_;
    }

    // The text this was parsed from, so that formatting and comments survive a round trip
    [[nodiscard]] auto const& text() const noexcept
    {
        return text_;
    }

    [[nodiscard]] bool operator==(tr_proxy_rules const& that) const noexcept
    {
        return text_ == that.text_;
    }

    [[nodiscard]] bool operator!=(tr_proxy_rules const& that) const noexcept
    {
        return !(*this == that);
    }

private:
    std::string text_;
    std::vector<Rule> rules_;
};
