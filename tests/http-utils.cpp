/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <experimental/iterator>
#include "http/utils.hpp"
#include "tests/pretty_printers.h"

using namespace std::string_literals;

namespace doctest {

template <>
struct StringMaker<rousette::http::ProtoAndHost> {
    static String convert(const rousette::http::ProtoAndHost& obj)
    {
        return ("ProtoAndHost{"s + obj.proto.value_or("nullopt") + ", " + obj.host.value_or("nullopt") + "}").c_str();
    }
};
}

TEST_CASE("Accept header")
{
    for (const auto& [input, expected] : {
             std::pair<std::string, std::vector<std::string>>{"*/*", {"*/*"}},
             {"text/*", {"text/*"}},
             {"text/plain", {"text/plain"}},
             {"teXt/PLaIn", {"text/plain"}},
             {"application/yang-data+json", {"application/yang-data+json"}},
             {"application/yang-data+xml+json", {}},
             {"application/yang-data+xml;q=0.5", {"application/yang-data+xml"}},
             {"application/yang-data+xml;q=0.52", {"application/yang-data+xml"}},
             {"application/yang-data+xml;q=1", {"application/yang-data+xml"}},
             {"*/*", {"*/*"}},
             {"*/haha", {}},
             {"*/*;q=1", {"*/*"}},
             {"* /*;q=1", {}},
             {"text/html, application/json", {"text/html", "application/json"}},
             {" text/html;q=0.8 , application/json;q=0.5", {"text/html", "application/json"}},
             {"invalidtype", {}},
             {"invalid//type", {}},
             {"invalid+type", {}},
             {"invalid / type", {}},
             {"", {}},
             {"text/*, text/plain, text/plain;format=flowed, */*", {"text/plain", "text/plain", "text/*", "*/*"}},
             {"application/vnd.example.v2+xml", {"application/vnd.example.v2+xml"}},
             {"text/*, application/json", {"application/json", "text/*"}},
             {"text/html; charset=utf-8", {"text/html"}},
             {"text/html; Charset=utf-8", {"text/html"}},
             {"application/json; q=0.8, text/plain; charset=utf-8", {"text/plain", "application/json"}},
             {"application/*json", {}},
             {"text/html; charset=", {}},
             {R"(text/html; charset="utf-8")", {"text/html"}},
             {"text/html; charset=utf-8, application/json; q=0.7", {"text/html", "application/json"}},
             {"text/html; q=0.9, text/plain; q=0.5", {"text/html", "text/plain"}},
             {"application/*; q=0.5, text/*; charset=utf-8", {"text/*", "application/*"}},
             {"application/xml; charset, q=0.7", {}},
             {"*/*;q=0.1, application/yang-data+xml;q=0.5,text/*;q=0.6", {"text/*", "application/yang-data+xml", "*/*"}},
             {"audio/*; q=0.2, audio/basic", {"audio/basic", "audio/*"}},
             {"text/plain; q=0.5, text/html,   text/x-dvi; q=0.8, text/x-c", {"text/html", "text/x-c", "text/x-dvi", "text/plain"}},
             {"audio/*; q=q, audio/basic", {}},
             {"application/xml; q=0.8, text/plain;q=0.9", {"text/plain", "application/xml"}},
             {"application/xml; q=0.8;q=1, text/plain;charset=utf-8;q=0.9", {"text/plain", "application/xml"}},
             {"  application/xml; q=0.8, text/plain   ;   charset=utf-8; q=0.9  ", {"text/plain", "application/xml"}},
             {"text/plain;q=1" + std::string(500, '0'), {}},
             {"text/plain;Q=1", {"text/plain"}},
         }) {
        CAPTURE(input);
        REQUIRE(rousette::http::parseAcceptHeader(input) == expected);
    };
}

TEST_CASE("Get Host from forwarded")
{
    using rousette::http::ProtoAndHost;
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;proto=https;by=203.0.113.43") == ProtoAndHost{"https", std::nullopt});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;proto=http;host=example.net;by=203.0.113.43") == ProtoAndHost{"http"s, "example.net"s});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;proto=https;by=203.0.113.43;host=example.net") == ProtoAndHost{"https"s, "example.net"s});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;by=203.0.113.43;host=192.0.2.1") == ProtoAndHost{std::nullopt, "192.0.2.1"s});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;proto=http;by=203.0.113.43;host=\"::1\"") == ProtoAndHost{"http", "::1"s});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;proto=https;by=203.0.113.43;host=\"::1\", proto=http;host=192.0.2.1;for=192.0.2.61") == ProtoAndHost{"https"s, "::1"s});
    REQUIRE(rousette::http::parseForwardedHeader("host=proto=https") == ProtoAndHost{});
    REQUIRE(rousette::http::parseForwardedHeader("") == ProtoAndHost{});
}
