/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10088";
#include <latch>
#include <libyang-cpp/Time.hpp>
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
        spdlog::trace("Client received data: {}", msg);
        auto notifDataNode = ctx.parseOp(msg, dataFormat, dataFormat == libyang::DataFormat::JSON ? libyang::OperationType::NotificationRestconf : libyang::OperationType::NotificationNetconf);
        data(*notifDataNode.op->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::Shrink));
    }

    MAKE_CONST_MOCK1(data, void(const std::string&));
};

struct SSEClient {
    std::shared_ptr<ng_client::session> client;
    boost::asio::deadline_timer t;

    SSEClient(
        boost::asio::io_service& io,
        std::latch& clientCanConnect,
        std::latch& clientRequest,
        std::latch& unprocessedNotifications,
        const NotificationWatcher& notification,
        const std::string& uri,
        const std::map<std::string, std::string>& headers,
        const boost::posix_time::seconds silenceTimeout = boost::posix_time::seconds(3))
        : client(std::make_shared<ng_client::session>(io, SERVER_ADDRESS, SERVER_PORT))
        , t(io, silenceTimeout)
    {
        ng::header_map reqHeaders;
        for (const auto& [name, value] : headers) {
            reqHeaders.insert({name, {value, false}});
        }

        t.async_wait([&](const boost::system::error_code&) {
            auto maybeClient = std::weak_ptr<ng_client::session>{client};
            if (auto client = maybeClient.lock()) {
                client->shutdown();
            }
        });

        client->on_connect([&, uri, reqHeaders](auto) {
            boost::system::error_code ec;

            clientCanConnect.wait();
            auto req = client->submit(ec, "GET", SERVER_ADDRESS_AND_PORT + uri, "", reqHeaders);
            req->on_response([&](const ng_client::response& res) {
                clientRequest.count_down();
                res.on_data([&](const uint8_t* data, std::size_t len) {
                    for (const auto& event : parseEvents(std::string(reinterpret_cast<const char*>(data), len))) {
                        notification(event);
                        unprocessedNotifications.count_down();
                    }
                });
            });
        });

        client->on_error([&](const boost::system::error_code& ec) {
            throw std::runtime_error{"HTTP client error: " + ec.message()};
        });
    }

    static std::vector<std::string> parseEvents(const std::string& msg)
    {
        static const std::string prefix = "data:";

        std::vector<std::string> res;
        std::istringstream iss(msg);
        std::string line;
        std::string event;

        while (std::getline(iss, line)) {
            if (line.compare(0, prefix.size(), prefix) == 0) {
                event += line.substr(prefix.size());
            } else if (line.empty()) {
                res.emplace_back(std::move(event));
                event.clear();
            }
        }
        return res;
    }
};

TEST_CASE("NETCONF notification streams")
{
    spdlog::set_level(spdlog::level::trace);

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    setupRealNacm(srSess);

    std::vector<std::string> notificationsJSON{
        R"({"example:eventA":{"message":"blabla","progress":11}})",
        R"({"example:eventB":{}})",
        R"({"example-notif:something-happened":{}})",
        R"({"example:eventA":{"message":"almost finished","progress":99}})",
    };
    std::vector<std::string> expectedNotificationsJSON;

    SECTION("NETCONF streams")
    {
        std::string uri;
        libyang::DataFormat dataFormat;
        std::map<std::string, std::string> headers;

        SECTION("XML stream")
        {
            dataFormat = libyang::DataFormat::XML;
            headers = {AUTH_ROOT};

            SECTION("No filter")
            {
                uri = "/streams/NETCONF/XML";
                expectedNotificationsJSON = notificationsJSON;
            }
            SECTION("Filter")
            {
                uri = "/streams/NETCONF/XML?filter=/example:eventA";
                expectedNotificationsJSON = {notificationsJSON[0], notificationsJSON[3]};
            }
        }

        SECTION("JSON stream")
        {
            uri = "/streams/NETCONF/json";
            dataFormat = libyang::DataFormat::JSON;

            SECTION("anonymous user cannot read example-notif module")
            {
                expectedNotificationsJSON = {notificationsJSON[0], notificationsJSON[1], notificationsJSON[3]};
            }

            SECTION("root user")
            {
                headers = {AUTH_ROOT};
                expectedNotificationsJSON = notificationsJSON;
            }
        }

        boost::asio::io_service io;

        std::latch clientCanConnect(1);
        std::latch clientRequest(1);
        std::latch unprocessedTasks(expectedNotificationsJSON.size());

        std::jthread notificationThread = std::jthread([&]() {
            auto notifSession = sysrepo::Connection{}.sessionStart();
            auto ctx = notifSession.getContext();

            // wait until the client requests
            clientCanConnect.count_down();
            clientRequest.wait();

            SEND_NOTIFICATION(notificationsJSON[0]);
            SEND_NOTIFICATION(notificationsJSON[1]);
            std::this_thread::sleep_for(33ms); // simulate some delays
            SEND_NOTIFICATION(notificationsJSON[2]);
            std::this_thread::sleep_for(125ms);
            SEND_NOTIFICATION(notificationsJSON[3]);

            // stop io_service after everything is processed
            unprocessedTasks.wait();
            io.stop();
        });

        NotificationWatcher netconfWatcher(srConn.sessionStart().getContext(), dataFormat);
        SSEClient cli(io, clientCanConnect, clientRequest, unprocessedTasks, netconfWatcher, uri, headers);

        for (const auto& notif : expectedNotificationsJSON) {
            EXPECT_NOTIFICATION(notif);
        }

        io.run();
    }

    SECTION("Invalid URLs")
    {
        REQUIRE(get("/streams/NETCONF/", {}) == Response{404, noContentTypeHeaders, ""});
        REQUIRE(get("/streams/NETCONF/", {AUTH_ROOT}) == Response{404, noContentTypeHeaders, ""});
        REQUIRE(get("/streams/NETCONF/bla", {}) == Response{404, noContentTypeHeaders, ""});
    }

    SECTION("Invalid parameters")
    {
        REQUIRE(get("/streams/NETCONF/XML?filter=.878", {}) == Response{400, noContentTypeHeaders, ""});
        REQUIRE(get("/streams/NETCONF/XML?filter=", {}) == Response{400, noContentTypeHeaders, ""});
    }

    SECTION("Replay support")
    {
        srConn.setModuleReplaySupport("example", true);
        srConn.setModuleReplaySupport("example-notif", true);

        std::string uri = "/streams/NETCONF/XML";

        boost::asio::io_service io;

        std::latch clientCanConnect(1);
        std::latch clientRequests(1);
        std::unique_ptr<std::latch> unprocessedTasks;
        std::jthread notificationThread;

        SECTION("Start time")
        {
            expectedNotificationsJSON = notificationsJSON;
            unprocessedTasks = std::make_unique<std::latch>(expectedNotificationsJSON.size());
            uri += "?start-time=" + libyang::yangTimeFormat(std::chrono::system_clock::now(), libyang::TimezoneInterpretation::Local);

            SECTION("All notifications before client connects")
            {
                notificationThread = std::jthread([&]() {
                    auto notifSession = sysrepo::Connection{}.sessionStart();
                    auto ctx = notifSession.getContext();

                    SEND_NOTIFICATION(notificationsJSON[0]);
                    SEND_NOTIFICATION(notificationsJSON[1]);
                    SEND_NOTIFICATION(notificationsJSON[2]);
                    SEND_NOTIFICATION(notificationsJSON[3]);
                    clientCanConnect.count_down();
                    clientRequests.wait();

                    unprocessedTasks->wait();
                    io.stop();
                });
            }
            SECTION("Some notifications before client connects")
            {
                notificationThread = std::jthread([&]() {
                    auto notifSession = sysrepo::Connection{}.sessionStart();
                    auto ctx = notifSession.getContext();

                    SEND_NOTIFICATION(notificationsJSON[0]);
                    SEND_NOTIFICATION(notificationsJSON[1]);
                    std::this_thread::sleep_for(250ms);
                    clientCanConnect.count_down();
                    clientRequests.wait();
                    std::this_thread::sleep_for(250ms);
                    SEND_NOTIFICATION(notificationsJSON[2]);
                    SEND_NOTIFICATION(notificationsJSON[3]);

                    unprocessedTasks->wait();
                    io.stop();
                });
            }
        }
        SECTION("Start and stop time")
        {
            auto notifSession = sysrepo::Connection{}.sessionStart();
            auto ctx = notifSession.getContext();

            expectedNotificationsJSON = {notificationsJSON[2]};
            unprocessedTasks = std::make_unique<std::latch>(expectedNotificationsJSON.size());

            SEND_NOTIFICATION(notificationsJSON[0]);
            SEND_NOTIFICATION(notificationsJSON[1]);

            std::this_thread::sleep_for(50ms);

            auto start = std::chrono::system_clock::now();
            SEND_NOTIFICATION(notificationsJSON[2]);
            auto end = std::chrono::system_clock::now();

            std::this_thread::sleep_for(50ms);

            uri += "?start-time=" + libyang::yangTimeFormat(start, libyang::TimezoneInterpretation::Local) + "&stop-time=" + libyang::yangTimeFormat(end, libyang::TimezoneInterpretation::Local);

            notificationThread = std::jthread([&]() {
                auto notifSession = sysrepo::Connection{}.sessionStart();
                auto ctx = notifSession.getContext();
                clientCanConnect.count_down();
                clientRequests.wait();
                SEND_NOTIFICATION(notificationsJSON[3]);
                unprocessedTasks->wait();
                io.stop();
            });
        }

        NotificationWatcher netconfWatcher(srConn.sessionStart().getContext(), libyang::DataFormat::XML);
        SSEClient cli(io, clientCanConnect, clientRequests, *unprocessedTasks, netconfWatcher, uri, {AUTH_ROOT});

        for (const auto& notif : expectedNotificationsJSON) {
            EXPECT_NOTIFICATION(notif);
        }

        io.run();
    }
}
