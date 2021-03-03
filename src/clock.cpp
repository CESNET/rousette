#include <boost/asio/io_service.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/signals2.hpp>
#include <chrono>
#include <list>
#include <nghttp2/asio_http2_server.h>
#include <spdlog/spdlog.h>
#include <thread>

using namespace nghttp2::asio_http2;
using namespace std::literals;

using Signal = boost::signals2::signal<void(const std::string& message)>;

class Client {
    const server::response& res;
    enum State {
        HasEvents,
        WaitingForEvents,
    };
    std::atomic<State> state;

    std::list<std::string> queue;
    mutable std::mutex mtx;
    boost::signals2::scoped_connection subscription;

    size_t send_chunk(uint8_t* destination, std::size_t len, uint32_t* data_flags [[maybe_unused]])
    {
        std::size_t written{0};
        std::lock_guard lock{mtx};
        if (state != HasEvents) throw std::logic_error{std::to_string(__LINE__)};
        while (!queue.empty()) {
            auto num = std::min(queue.front().size(), len - written);
            std::copy_n(queue.front().begin(), num, destination + written);
            written += num;
            if (num < queue.front().size()) {
                queue.front() = queue.front().substr(num);
                spdlog::debug("{} send_chunk: partial write", (void*)this);
                return written;
            }
            queue.pop_front();
            spdlog::debug("{} send_chunk: sent one event", (void*)this);
        }
        state = WaitingForEvents;
        return written;
    }

public:
    Client(const server::request& req, const server::response& res, Signal& signal)
    : res{res}
    , state{WaitingForEvents}
    , subscription{signal.connect([this](const auto& msg) {
        enqueue(msg);
    })}
    {
        spdlog::warn("{}: {} {} {}", (void*)this, boost::lexical_cast<std::string>(req.remote_endpoint()), req.method(), req.uri().raw_path);
        res.write_head(200, {{"content-type", {"text/event-stream", false}}});
    }

    void onClose([[maybe_unused]] const uint32_t ec)
    {
        spdlog::error("{} onClose", (void*)this);
        subscription.disconnect();
    }

    ssize_t process(uint8_t* destination, std::size_t len, uint32_t* data_flags)
    {
        spdlog::trace("{} process", (void*)this);
        switch (state) {
        case HasEvents:
            return send_chunk(destination, len, data_flags);
        case WaitingForEvents:
            return NGHTTP2_ERR_DEFERRED;
        }
        __builtin_unreachable();
    }

    void enqueue(const std::string& what)
    {
        {
            std::lock_guard lock{mtx};
            queue.push_back("data: " + what + "\n\n");
        }
        state = HasEvents;
        res.resume();
    }
};

int main(int argc [[maybe_unused]], char** argv [[maybe_unused]])
{
    spdlog::set_level(spdlog::level::trace);

    Signal sig;
    std::thread timer{[&sig]() {
        for (int i = 0; /* forever */; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{666});
            spdlog::info("tick: {}", i);
            sig("ping #" + std::to_string(i));
        }
    }};

    server::http2 server;
    server.num_threads(4);

    server.handle("/events", [&sig](const server::request& req, const server::response& res) {
        auto client = std::make_shared<Client>(req, res, sig);

        res.on_close([client](const auto ec) {
            client->onClose(ec);
        });
        res.end([client](uint8_t* destination, std::size_t len, uint32_t* data_flags) {
            return client->process(destination, len, data_flags);
        });
    });

    server.handle("/", [](const auto& req, const auto& resp) {
        spdlog::warn("{} {} {}", boost::lexical_cast<std::string>(req.remote_endpoint()), req.method(), req.uri().raw_path);
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
