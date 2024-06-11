/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10088";
#include <latch>
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/pretty_printers.h"

#define MACRO(DATA)                                                            \
    expectations.emplace_back(NAMED_REQUIRE_CALL(netconfWatcher, data(DATA))); \
    notifSession.sendNotification(*ctx.parseOp(DATA, libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);

struct NotificationWatcher {
    libyang::Context ctx;
    libyang::DataFormat dataFormat;

    NotificationWatcher(const libyang::Context& ctx, libyang::DataFormat dataFormat)
        : ctx(ctx)
        , dataFormat(dataFormat)
    {
    }

    void operator()(const std::string& msg) const
    {
        auto notifDataNode = ctx.parseOp(msg, dataFormat, dataFormat == libyang::DataFormat::JSON ? libyang::OperationType::NotificationRestconf : libyang::OperationType::NotificationNetconf);
        data(*notifDataNode.op->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::KeepEmptyCont));
    }

    MAKE_CONST_MOCK1(data, void(const std::string&));
};

struct SSEClient {
    std::optional<std::string> clientError;
    const NotificationWatcher& sseWatcher;
    std::latch& latch;
    ng_client::session client;
    std::string uri;
    ng::header_map reqHeaders;
    boost::asio::deadline_timer t;

    SSEClient(
        boost::asio::io_service& io,
        std::latch& latch,
        const NotificationWatcher& notifWatcher,
        const std::string& uri,
        const std::map<std::string, std::string>& headers,
        const boost::posix_time::seconds timeout = boost::posix_time::seconds(3))
        : sseWatcher(notifWatcher)
        , latch(latch)
        , client(io, SERVER_ADDRESS, SERVER_PORT)
        , uri(uri)
        , t(io, timeout)
    {
        for (const auto& [name, value] : headers) {
            reqHeaders.insert({name, {value, false}});
        }
    }

    void raiseErrors()
    {
        if (clientError) {
            throw std::runtime_error{"HTTP client error: " + *clientError};
        }
    }

    void start()
    {
        // client.read_timeout(boost::posix_time::seconds(2));
        t.async_wait([&](const boost::system::error_code&) { client.shutdown(); });

        client.on_connect([&](auto) {
            boost::system::error_code ec;

            auto req = client.submit(ec, "GET", SERVER_ADDRESS_AND_PORT + uri, "", reqHeaders);
            req->on_response([&](const ng_client::response& res) {
                latch.count_down();
                res.on_data([&](const uint8_t* data, std::size_t len) {
                    sseWatcher(removeSSEPrefix(std::string(reinterpret_cast<const char*>(data), len)));
                });
            });
        });

        client.on_error([&](const boost::system::error_code& ec) {
            clientError = ec.message();
        });
    }

    static std::string removeSSEPrefix(const std::string& msg)
    {
        spdlog::error("....{}", msg);
        static const std::string prefix = "data:";

        std::istringstream iss(msg);
        std::string line;
        std::string res;

        while (std::getline(iss, line)) {
            if (line.compare(0, prefix.size(), prefix) == 0) {
                res += line.substr(prefix.size());
            }
        }
        return res;
    }
};

libyang::DataNode createNotif(const libyang::Context& ctx, const std::string& notif, const std::map<std::string, std::string>& values)
{
    libyang::DataNode res = ctx.newPath("/" + notif);
    for (const auto& [k, v] : values) {
        res.newPath(k, v);
    }
    return res;
}

TEST_CASE("NETCONF notification streams")
{
    trompeloeil::sequence seq1;
    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    spdlog::set_level(spdlog::level::trace);

    boost::asio::io_service io;
    std::jthread notificationCreatingThread;
    std::latch latch(1);

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    setupRealNacm(srSess);

    std::string uri;
    libyang::DataFormat dataFormat;

    SECTION("NETCONF streams")
    {
        //SECTION("XML stream")
        //{
            //uri = "/streams/NETCONF/XML";
            //dataFormat = libyang::DataFormat::XML;
        //}
        SECTION("JSON stream")
        {
            uri = "/streams/NETCONF/json";
            dataFormat = libyang::DataFormat::JSON;
        }

        NotificationWatcher netconfWatcher(srConn.sessionStart().getContext(), dataFormat);
        SSEClient cli(io, latch, netconfWatcher, uri, {});

        notificationCreatingThread = std::jthread([&]() {
            auto notifSession = sysrepo::Connection{}.sessionStart();
            auto ctx = notifSession.getContext();

            latch.wait(); // wait until the client requests

            MACRO(R"({
  "example:notif1": {
    "message": "blabla",
    "progress": 11
  }
}
)")

            //MACRO(R"({
  //"example:notif2": {}
//}
//)");

            //MACRO(R"({
  //"example-notif:notif": {}
//}
//)");

            //MACRO(R"({
  //"example:notif1": {
    //"message": "almost finished",
    //"progress": 99
  //}
//}
//)");
        });

        cli.start();
        io.run();
        cli.raiseErrors();
    }

    SECTION("Invalid URLs")
    {
        REQUIRE(get("/streams/NETCONF/", {}) == Response{404, noContentTypeHeaders, ""});
        REQUIRE(get("/streams/NETCONF/bla", {}) == Response{404, noContentTypeHeaders, ""});
    }
}
