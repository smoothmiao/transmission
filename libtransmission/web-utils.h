// This file Copyright © 2021-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "tr-macros.h" // tr_sha1_digest_t

struct evbuffer;

/** @brief convenience function to determine if an address is an IP address (IPv4 or IPv6) */
bool tr_addressIsIP(char const* address);

/** @brief return true if the url is a http or https or UDP url that Transmission understands */
bool tr_urlIsValidTracker(std::string_view url);

/** @brief return true if the url is a [ http, https, ftp, sftp ] url that Transmission understands */
bool tr_urlIsValid(std::string_view url);

struct tr_url_parsed_t
{
    // http://example.com:80/over/there?name=ferret#nose

    std::string_view scheme; // "http"
    std::string_view authority; // "example.com:80"
    std::string_view host; // "example.com"
    std::string_view sitename; // "example"
    std::string_view portstr; // "80"
    std::string_view path; // /"over/there"
    std::string_view query; // "name=ferret"
    std::string_view fragment; // "nose"
    std::string_view full; // "http://example.com:80/over/there?name=ferret#nose"
    int port = -1; // 80
};

std::optional<tr_url_parsed_t> tr_urlParse(std::string_view url);

// like tr_urlParse(), but with the added constraint that 'scheme'
// must be one we that Transmission supports for announce and scrape
std::optional<tr_url_parsed_t> tr_urlParseTracker(std::string_view url);

// example use: `for (auto const [key, val] : tr_url_query_view{ querystr })`
struct tr_url_query_view
{
    std::string_view const query;

    explicit tr_url_query_view(std::string_view query_in)
        : query{ query_in }
    {
    }

    struct iterator
    {
        std::pair<std::string_view, std::string_view> keyval = std::make_pair(std::string_view{ "" }, std::string_view{ "" });
        std::string_view remain = std::string_view{ "" };

        iterator& operator++();

        constexpr auto const& operator*() const
        {
            return keyval;
        }

        constexpr auto const* operator->() const
        {
            return &keyval;
        }

        constexpr bool operator==(iterator const& that) const
        {
            return this->remain == that.remain && this->keyval == that.keyval;
        }

        constexpr bool operator!=(iterator const& that) const
        {
            return !(*this == that);
        }
    };

    iterator begin() const;

    constexpr iterator end() const
    {
        return iterator{};
    }
};

void tr_http_escape(std::string& appendme, std::string_view str, bool escape_reserved);

// TODO: remove evbuffer version
void tr_http_escape(struct evbuffer* out, std::string_view str, bool escape_reserved);

void tr_http_escape_sha1(char* out, uint8_t const* sha1_digest);

void tr_http_escape_sha1(char* out, tr_sha1_digest_t const& digest);

char const* tr_webGetResponseStr(long response_code);

std::string tr_urlPercentDecode(std::string_view);
