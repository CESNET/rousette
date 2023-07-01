/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <chrono>
#include <iostream>
#include <nghttp2/asio_http2_client.h>
#include <sstream>
#include "restconf/Server.h"
#include "UniqueResource.h"

using namespace std::string_literals;

namespace doctest {
template<>
struct StringMaker<std::pair<int, std::string>> {
    static String convert(const std::pair<int, std::string>& o) {
        return ("{"s + std::to_string(o.first) + ", \"" + o.second + "\"}").c_str();
    }
};
}

static const auto SERVER_ADDRESS = "::1";
static const auto SERVER_PORT = "10080";
static const auto SERVER_ADDRESS_AND_PORT = "http://["s + SERVER_ADDRESS + "]" + ":" + SERVER_PORT;

std::pair<int, std::string> retrieveData(auto xpath, const std::optional<std::string>& user)
{
    namespace ng = nghttp2::asio_http2;
    namespace ng_client = ng::client;

    boost::asio::io_service io_service;
    ng_client::session client(io_service, SERVER_ADDRESS, SERVER_PORT);

    std::ostringstream oss;
    int statusCode;

    client.on_connect([&client, &xpath, &oss, &user, &statusCode](auto) {
        boost::system::error_code ec;
        ng::header_map headers;

        if (user) {
            headers.insert({"x-netconf-nacm-user", {user.value(), false}});
        }

        auto req = client.submit(ec, "GET", SERVER_ADDRESS_AND_PORT + "/restconf/data"s + xpath, headers);
        req->on_response([&oss, &statusCode](const ng_client::response& res) {
            res.on_data([&oss](const uint8_t* data, std::size_t len) {
                oss.write(reinterpret_cast<const char*>(data), len);
            });
            statusCode = res.status_code();
        });
        req->on_close([&client](auto) {
            client.shutdown();
        });
    });

    client.on_error([](const boost::system::error_code& ec) {
        std::cerr << "error: " << ec.message() << std::endl;
    });

    io_service.run();

    return {statusCode, oss.str()};
}

TEST_CASE("NACM")
{
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto server = rousette::restconf::Server{srConn};
    auto guard = make_unique_resource(
            [&server](){ server.listen_and_serve(SERVER_ADDRESS, SERVER_PORT, true); },
            [&server](){ server.stop(); }
            );

    REQUIRE(retrieveData("/nacm-test:unlimited", "nobody") == std::pair<int, std::string>(200, "{\n\n}\n"));
    REQUIRE(retrieveData("/nacm-test:limited", "nobody") == std::pair<int, std::string>(200, "{\n\n}\n"));
    REQUIRE(retrieveData("/nacm-test:unlimited", "optics") == std::pair<int, std::string>(200, "{\n\n}\n"));
    REQUIRE(retrieveData("/nacm-test:limited", "optics") == std::pair<int, std::string>(200, "{\n\n}\n"));
    REQUIRE(retrieveData("/nacm-test:unlimited", std::nullopt) == std::pair<int, std::string>(200, "{\n\n}\n"));
    REQUIRE(retrieveData("/nacm-test:limited", std::nullopt) == std::pair<int, std::string>(200, "{\n\n}\n"));
}
