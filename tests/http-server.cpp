/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <nghttp2/asio_http2_client.h>
#include <iostream>
#include <sstream>
#include "trompeloeil_doctest.h"
#include "restconf/Server.h"

namespace std {
std::ostream& operator<<(std::ostream& s, const std::optional<std::string>& x) {
    if (!x) {
        return s << "nullopt{}";
    }
    return s << "optional{" << *x << "}";
}
}

TEST_CASE("subtree path validity") {
    auto data = std::initializer_list<std::pair<std::string, std::optional<std::string>>> {
        {{}, {}},
        {"/restconf/data", {}},
        {"/restconf/data/foo", {}},
        {"/restconf/data/foo:", {}},
        {"/restconf/data/foo:*", "foo:*"},
        {"/restconf/data/foo:*/bar", {}},
        {"/restconf/data/:bar", {}},
        {"/restconf/data/foo:bar", "foo:bar"},
        {"/restconf/data/foo:bar/baz", "foo:bar/baz"},
        {"/restconf/data/foo:bar/meh:baz", "foo:bar/meh:baz"},
        {"/restconf/data/foo:bar/yay/meh:baz", "foo:bar/yay/meh:baz"},
        {"/restconf/data/foo:bar/:baz", {}},
    };
    for (const auto& x : data) {
        const auto& input = x.first;
        const auto& xpath = x.second;
        CAPTURE(input);
        CAPTURE(xpath);
        REQUIRE(rousette::restconf::as_subtree_path(input) == xpath);
    }
}

TEST_CASE("allowed paths for anonymous read") {

    auto allowed = {
        "czechlight-roadm-device:*",
        "czechlight-roadm-device:spectrum-scan",
        "czechlight-roadm-device:something/complex",
        "czechlight-system:firmware",
    };
    auto rejected = {
        "foo:*",
        "foo:bar",
        "unrelated:wtf/czechlight-roadm-device:something/complex",
        "czechlight-system:authentication",
        "czechlight-system:*",
        "ietf-netconf-server:*",
    };

    for (const auto path : allowed) {
        CAPTURE(path);
        REQUIRE(rousette::restconf::allow_anonymous_read_for(path));
    }

    for (const auto path : rejected) {
        CAPTURE(path);
        REQUIRE(!rousette::restconf::allow_anonymous_read_for(path));
    }
}

std::string retrieveData(auto xpath)
{
    using namespace std::string_literals;
    namespace ng_client = nghttp2::asio_http2::client;

    boost::asio::io_service io_service;
    ng_client::session client(io_service, "::1", "10080");

    std::string resData;

    client.on_connect([&client, &xpath, &resData](auto) {
        boost::system::error_code ec;

        auto req = client.submit(ec, "GET", "http://[::1]:10080/restconf/data"s + xpath);
        req->on_response([&resData](const ng_client::response& res) {
            res.on_data([&resData](const uint8_t *data, std::size_t len) {
                std::ostringstream oss;
                oss.write(reinterpret_cast<const char*>(data), len);
                resData = oss.str();
            });
        });
        req->on_close([&client](auto) {
            client.shutdown();
        });
    });

    client.on_error([](const boost::system::error_code& ec) {
        std::cerr << "error: " << ec.message() << std::endl;
    });

    io_service.run();
    std::cerr << "resData" << " = " << resData << "\n";

    return resData;
}

TEST_CASE("NACM") {
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto server = rousette::restconf::Server{srConn};
    server.listen_and_serve("::1", "10080");

    std::string xpath;
    std::string expected;

    DOCTEST_SUBCASE("/example-nacm:*")
    {
        xpath = "/example-nacm:*";
        expected = "{}";
    }

    REQUIRE(retrieveData(xpath) == expected);

    server.stop();
}
