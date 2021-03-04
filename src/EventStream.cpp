/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/lexical_cast.hpp>
#include <nghttp2/asio_http2_server.h>
#include <nghttp2/nghttp2.h>
#include <numeric>
#include <spdlog/spdlog.h>
#include "EventStream.h"

using namespace nghttp2::asio_http2;

/** @short After constructing, make sure to call activate() immediately. */
EventStream::EventStream(const server::request& req, const server::response& res)
    : res{res}
    , peer{boost::lexical_cast<std::string>(req.remote_endpoint())}
{
    spdlog::info("{}: {} {}", peer, req.method(), req.uri().raw_path);
}

/** @short Start event processing and data delivery

This cannot be a part of the constructor because of enable_shared_from_this<> semantics. When in constructor,
shared_from_this() throws bad_weak_ptr, so we need a two-phase construction.
*/
void EventStream::activate(Signal& signal)
{
    subscription = signal.connect([this](const auto& msg) {
        enqueue(msg);
    });

    auto client = shared_from_this();
    res.write_head(200, {{"content-type", {"text/event-stream", false}}});

    res.on_close([client](const auto ec) {
        spdlog::debug("{}: closed ({})", client->peer, nghttp2_http2_strerror(ec));
        client->subscription.disconnect();
    });

    res.end([client](uint8_t* destination, std::size_t len, uint32_t* data_flags) {
        return client->process(destination, len, data_flags);
    });
}

size_t EventStream::send_chunk(uint8_t* destination, std::size_t len, uint32_t* data_flags [[maybe_unused]])
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
            // spdlog::trace("{}: send_chunk: partial write: {}", peer, num);
            return written;
        }
        queue.pop_front();
        spdlog::debug("{}: sent one event", peer);
    }
    state = WaitingForEvents;
    return written;
}

ssize_t EventStream::process(uint8_t* destination, std::size_t len, uint32_t* data_flags)
{
    switch (state) {
    case HasEvents:
        return send_chunk(destination, len, data_flags);
    case WaitingForEvents:
        spdlog::trace("{}: sleeping", peer);
        return NGHTTP2_ERR_DEFERRED;
    }
    __builtin_unreachable();
}

void EventStream::enqueue(const std::string& what)
{
    {
        std::lock_guard lock{mtx};
        auto len = std::accumulate(queue.begin(), queue.end(), 0, [](const auto a, const auto b){
                return a + b.size();
                });
        spdlog::trace("{}: new event, ∑ queue size = {}", peer, len);
        queue.push_back("data: " + what + "\n\n");
    }
    state = HasEvents;
    res.resume();
}
