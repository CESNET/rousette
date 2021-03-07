/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <cstdio>
#include <cstdlib>
#include <inttypes.h>
#include <spdlog/sinks/systemd_sink.h>
#include <spdlog/sinks/ansicolor_sink.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Session.hpp>
#include "restconf/Server.h"

namespace {
/** @short Is stderr connected to journald? Not thread safe. */
bool is_journald_active()
{
    const auto stream = ::getenv("JOURNAL_STREAM");
    if (!stream) {
        return false;
    }
    uintmax_t dev;
    uintmax_t inode;
    if (::sscanf(stream, "%" SCNuMAX ":%" SCNuMAX, &dev, &inode) != 2) {
        return false;
    }
    struct stat buf;
    if (fstat(STDERR_FILENO, &buf)) {
        return false;
    }
    return static_cast<uintmax_t>(buf.st_dev) == dev && static_cast<uintmax_t>(buf.st_ino) == inode;
}

/** @short Provide better levels, see https://github.com/gabime/spdlog/pull/1292#discussion_r340777258 */
template<typename Mutex>
class journald_sink : public spdlog::sinks::systemd_sink<Mutex> {
public:
    journald_sink()
    {
        this->syslog_levels_ = {/* spdlog::level::trace      */ LOG_DEBUG,
              /* spdlog::level::debug      */ LOG_INFO,
              /* spdlog::level::info       */ LOG_NOTICE,
              /* spdlog::level::warn       */ LOG_WARNING,
              /* spdlog::level::err        */ LOG_ERR,
              /* spdlog::level::critical   */ LOG_CRIT,
              /* spdlog::level::off        */ LOG_ALERT};
    }
};
}

int main(int argc [[maybe_unused]], char* argv [[maybe_unused]] [])
{
    if (is_journald_active()) {
        auto sink = std::make_shared<journald_sink<std::mutex>>();
        auto logger = std::make_shared<spdlog::logger>("", sink);
        spdlog::set_default_logger(logger);
    }
    spdlog::set_level(spdlog::level::trace);

    auto conn = std::make_shared<sysrepo::Connection>();
    auto server = rousette::restconf::Server{conn};
    server.listen_and_serve("::1", "10080");

    return 0;
}
