/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <nghttp2/asio_http2_server.h>
#include <nghttp2/nghttp2.h>
#include <numeric>
#include <regex>
#include <spdlog/spdlog.h>
#include "http/EventStream.h"
#include "http/utils.hpp"

using namespace nghttp2::asio_http2;

namespace rousette::http {

constexpr auto FIELD_DATA = "data";

/** @short After constructing, make sure to call activate() immediately. */
EventStream::EventStream(const server::request& req,
                         const server::response& res,
                         Termination& termination,
                         EventSignal& signal,
                         const std::chrono::seconds keepAlivePingInterval,
                         const std::optional<std::string>& initialEvent)
    : res{res}
    , ping{res.io_service()}
    , peer{peer_from_request(req)}
    , m_keepAlivePingInterval(keepAlivePingInterval)
{
    if (initialEvent) {
        enqueue(FIELD_DATA, *initialEvent);
    }

    eventSub = signal.connect([this](const auto& msg) {
        enqueue(FIELD_DATA, msg);
    });

    terminateSub = termination.connect([this]() {
        spdlog::trace("{}: will terminate", peer);
        std::lock_guard lock{mtx};
        if (state == Closed) { // we are late to the party, res is already gone
            return;
        }

        state = WantToClose;
        boost::asio::post(this->res.io_service(), [weak = std::weak_ptr<EventStream>{shared_from_this()}]() {
            if (auto myself = weak.lock()) {
                std::lock_guard lock{myself->mtx};
                if (myself->state == WantToClose) { // resume unless somebody closed it before this was picked up by the event loop
                    myself->res.resume();
                }
            }
        });
    });
}

/** @short Start event processing and data delivery

This cannot be a part of the constructor because of enable_shared_from_this<> semantics. When in constructor,
shared_from_this() throws bad_weak_ptr, so we need a two-phase construction.
*/
void EventStream::activate()
{
    start_ping();

    auto myself = shared_from_this();
    res.write_head(200, {
        {"content-type", {"text/event-stream", false}},
        {"access-control-allow-origin", {"*", false}},
    });

    res.on_close([myself](const auto ec) {
        spdlog::debug("{}: closed ({})", myself->peer, nghttp2_http2_strerror(ec));
        std::lock_guard lock{myself->mtx};
        myself->ping.cancel();
        myself->eventSub.disconnect();
        myself->terminateSub.disconnect();
        myself->state = Closed;
    });

    res.end([myself](uint8_t* destination, std::size_t len, uint32_t* data_flags) {
        return myself->process(destination, len, data_flags);
    });
}

size_t EventStream::send_chunk(uint8_t* destination, std::size_t len, uint32_t* data_flags [[maybe_unused]])
{
    std::size_t written{0};
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
    std::lock_guard lock{mtx};
    switch (state) {
    case HasEvents:
        return send_chunk(destination, len, data_flags);
    case WaitingForEvents:
        spdlog::trace("{}: sleeping", peer);
        return NGHTTP2_ERR_DEFERRED;
    case WantToClose:
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    case Closed:
        throw std::logic_error{"response already closed"};
    }
    __builtin_unreachable();
}

void EventStream::enqueue(const std::string& fieldName, const std::string& what)
{
    std::string buf;
    buf.reserve(what.size());
    const std::regex newline{"\n"};
    for (auto it = std::sregex_token_iterator{what.begin(), what.end(), newline, -1};
            it != std::sregex_token_iterator{}; ++it) {
        buf += fieldName;
        buf += ": ";
        buf += *it;
        buf += '\n';
    }
    buf += '\n';

    std::lock_guard lock{mtx};
    if (state == Closed || state == WantToClose) {
        spdlog::trace("{}: enqueue: already disconnected", peer);
        return;
    }
    auto len = std::accumulate(queue.begin(), queue.end(), 0, [](const auto a, const auto b){
            return a + b.size();
            });
    spdlog::trace("{}: new event, ∑ queue size = {}", peer, len);
    queue.push_back(buf);
    state = HasEvents;
    boost::asio::post(res.io_service(), [&res = this->res]() { res.resume(); });
}

void EventStream::start_ping()
{
    ping.expires_from_now(boost::posix_time::seconds(m_keepAlivePingInterval.count()));
    ping.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
        auto myself = weak.lock();
        if (!myself) {
            spdlog::trace("ping: client already gone");
            return;
        }

        if (ec == boost::asio::error::operation_aborted) {
            spdlog::trace("{}: ping scheduler cancelled", myself->peer);
            return;
        }

        myself->enqueue("", "\n");
        spdlog::trace("{}: keep-alive ping enqueued", myself->peer);
        myself->start_ping();
    });
}

/** @brief Create a new EventStream instance and activate it immediately.
 *
 * The stream is created with the given parameters and activated as if the activate() method was called.
 *   ```
 *   auto a = make_shared<EventStream>(...);
 *   a->activate();
 *   ```
 */
std::shared_ptr<EventStream> EventStream::create(const nghttp2::asio_http2::server::request& req,
                                                 const nghttp2::asio_http2::server::response& res,
                                                 Termination& terminate,
                                                 EventSignal& signal,
                                                 const std::chrono::seconds keepAlivePingInterval,
                                                 const std::optional<std::string>& initialEvent)
{
    auto stream = std::shared_ptr<EventStream>(new EventStream(req, res, terminate, signal, keepAlivePingInterval, initialEvent));
    stream->activate();
    return stream;
}
}
