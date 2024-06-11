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

#define EXPECT_NOTIFICATION(DATA) expectations.emplace_back(NAMED_REQUIRE_CALL(netconfWatcher, data(DATA)));
#define SEND_NOTIFICATION(DATA) notifSession.sendNotification(*ctx.parseOp(DATA, libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);
#define SEND_AND_EXPECT_NOTIFICATION(DATA) \
    EXPECT_NOTIFICATION(DATA)              \
    SEND_NOTIFICATION(DATA)

using namespace std::chrono_literals;

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
        spdlog::trace("Received data: {}", msg);
        auto notifDataNode = ctx.parseOp(msg, dataFormat, dataFormat == libyang::DataFormat::JSON ? libyang::OperationType::NotificationRestconf : libyang::OperationType::NotificationNetconf);
        data(*notifDataNode.op->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::KeepEmptyCont));
    }

    MAKE_CONST_MOCK1(data, void(const std::string&));
};

struct SSEClient {
    std::shared_ptr<ng_client::session> client;
    boost::asio::deadline_timer t;

    SSEClient(
        boost::asio::io_service& io,
        std::latch& latch,
        std::latch& latchUnproc,
        const NotificationWatcher& notifWatcher,
        const std::string& uri,
        const std::map<std::string, std::string>& headers,
        const boost::posix_time::seconds timeout = boost::posix_time::seconds(3))
        : client(std::make_shared<ng_client::session>(io, SERVER_ADDRESS, SERVER_PORT))
        , t(io, timeout)
    {
        ng::header_map reqHeaders;
        for (const auto& [name, value] : headers) {
            reqHeaders.insert({name, {value, false}});
        }

        // client.read_timeout(timeout);
        t.async_wait([&](const boost::system::error_code&) {
            auto maybeClient = std::weak_ptr<ng_client::session>{client};
            if (auto client = maybeClient.lock()) {
                client->shutdown();
            }
        });

        client->on_connect([&, uri, reqHeaders](auto) {
            boost::system::error_code ec;

            auto req = client->submit(ec, "GET", SERVER_ADDRESS_AND_PORT + uri, "", reqHeaders);
            req->on_response([&](const ng_client::response& res) {
                latch.count_down();
                res.on_data([&](const uint8_t* data, std::size_t len) {
                    notifWatcher(removeSSEPrefix(std::string(reinterpret_cast<const char*>(data), len)));
                    latchUnproc.count_down();
                });
            });
        });

        client->on_error([&](const boost::system::error_code& ec) {
            throw std::runtime_error{"HTTP client error: " + ec.message()};
        });
    }

    static std::string
    removeSSEPrefix(const std::string& msg)
    {
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

TEST_CASE("NETCONF notification streams")
{
    spdlog::set_level(spdlog::level::trace);

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    boost::asio::io_service io;
    std::jthread notificationCreatingThread;
    std::latch latch(1);

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    setupRealNacm(srSess);

    SECTION("NETCONF streams")
    {
        std::string uri;
        libyang::DataFormat dataFormat;
        std::map<std::string, std::string> headers;
        std::vector<std::string> expectedNotificationsJSON;

        std::vector<std::string> notificationsJSON;
        notificationsJSON.emplace_back(R"({
  "example:notif1": {
    "message": "blabla",
    "progress": 11
  }
}
)");
        notificationsJSON.emplace_back(R"({
  "example:notif2": {}
}
)");
        notificationsJSON.emplace_back(R"({
  "example-notif:notif": {}
}
)");
        notificationsJSON.emplace_back(R"({
  "example:notif1": {
    "message": "almost finished",
    "progress": 99
  }
}
)");

        SECTION("XML stream")
        {
            uri = "/streams/NETCONF/XML";
            dataFormat = libyang::DataFormat::XML;
            headers = {AUTH_ROOT};
            expectedNotificationsJSON = notificationsJSON;
        }

        SECTION("JSON stream")
        {
            uri = "/streams/NETCONF/json";
            dataFormat = libyang::DataFormat::JSON;

            SECTION("anonymous user, no access to example-notif module")
            {
                expectedNotificationsJSON = {notificationsJSON[0], notificationsJSON[1], notificationsJSON[3]};
            }

            SECTION("root user")
            {
                headers = {AUTH_ROOT};
                expectedNotificationsJSON = notificationsJSON;
            }
        }

        std::latch latchUnproc(expectedNotificationsJSON.size());

        NotificationWatcher netconfWatcher(srConn.sessionStart().getContext(), dataFormat);
        SSEClient cli(io, latch, latchUnproc, netconfWatcher, uri, headers);

        for (const auto& notif : expectedNotificationsJSON) {
            EXPECT_NOTIFICATION(notif);
        }

        notificationCreatingThread = std::jthread([&]() {
            auto notifSession = sysrepo::Connection{}.sessionStart();
            auto ctx = notifSession.getContext();
            latch.wait(); // wait until the client requests
            std::this_thread::sleep_for(100ms); // simulate some delays

            for (const auto& notifJson : notificationsJSON) {
                SEND_NOTIFICATION(notifJson);
            }

            latchUnproc.wait(); // stop io_service after everything is processed
            io.stop();
        });

        io.run();
    }

    SECTION("Invalid URLs")
    {
        REQUIRE(get("/streams/NETCONF/", {}) == Response{404, noContentTypeHeaders, ""});
        REQUIRE(get("/streams/NETCONF/", {AUTH_ROOT}) == Response{404, noContentTypeHeaders, ""});
        REQUIRE(get("/streams/NETCONF/bla", {}) == Response{404, noContentTypeHeaders, ""});
    }
}
