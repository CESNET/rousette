/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/lexical_cast.hpp>
#include <chrono>
#include <nghttp2/asio_http2_server.h>
#include <spdlog/spdlog.h>
#include <thread>
#include "http/EventStream.h"

using namespace std::literals;

int main(int argc [[maybe_unused]], char** argv [[maybe_unused]])
{
    spdlog::set_level(spdlog::level::trace);

    rousette::http::EventStream::Termination shutdown;
    rousette::http::EventStream::EventSignal sig;
    std::jthread timer{[&sig]() {
        for (int i = 0; /* forever */; ++i) {
            std::this_thread::sleep_for(666ms);
            spdlog::info("tick: {}", i);
            sig("ping #" + std::to_string(i));
        }
    }};

    nghttp2::asio_http2::server::http2 server;
    server.num_threads(4);

    server.handle("/events", [&shutdown, &sig](const auto& req, const auto& res) {
        auto client = std::make_shared<rousette::http::EventStream>(req, res, shutdown, sig);
        client->activate();
    });

    server.handle("/", [](const auto& req, const auto& resp) {
        spdlog::info("{}: {} {}", boost::lexical_cast<std::string>(req.remote_endpoint()), req.method(), req.uri().raw_path);
        resp.write_head(200, {{"content-type", {"text/html", false}}});
        resp.end(R"(<html><head><title>nghttp2 event stream</title></head>
<body><h1>events</h1><ul id="x"></ul>
<script type="text/javascript">
const ev = new EventSource("/events");
ev.onmessage = function(event) {
  const li = document.createElement("li");
  li.textContent = event.data;
  document.getElementById("x").appendChild(li);
};
</script>
</body>
</html>)");
    });

    boost::system::error_code ec;
    if (server.listen_and_serve(ec, "::", "10080")) {
        return 1;
    }
    return 0;
}
