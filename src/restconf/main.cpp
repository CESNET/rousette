/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <inttypes.h>

#include "configure.cmake.h" /* Expose HAVE_SYSTEMD */

#ifdef HAVE_SYSTEMD
   #include <spdlog/sinks/systemd_sink.h>
#endif
#include <spdlog/sinks/syslog_sink.h>
#include <spdlog/sinks/ansicolor_sink.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <docopt.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo.h>
#include "restconf/Server.h"
static const char usage[] =
  R"(Rousette - RESTCONF server
Usage:
  rousette [--syslog] [--timeout <SECONDS>] [--sysrepo-log-level=<N>] [--help]
Options:
  -h --help                         Show this screen.
  -t --timeout <SECONDS>            Change default timeout in sysrepo (if not set, use sysrepo internal).
  --syslog                          Log to syslog.
  --sysrepo-log-level=<N>           Log level for the sysrepo library [default: 2]
                                    (0 -> critical, 1 -> error, 2 -> warning, 3 -> info,
                                    4 -> debug, 5 -> trace)
)";
#ifdef HAVE_SYSTEMD

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
#endif

namespace {
spdlog::level::level_enum parseLogLevel(const std::string& name, const docopt::value& option)
{
    long x;
    try {
        x = option.asLong();
    } catch (std::runtime_error&) {
        throw std::runtime_error(name + " log level: expecting integer");
    }
    static_assert(spdlog::level::trace < spdlog::level::off, "spdlog::level levels have changed");
    static_assert(spdlog::level::off == 6, "spdlog::level levels have changed");
    if (x < 0 || x > 5)
        throw std::runtime_error(name + " log level invalid or out-of-range");

    return static_cast<spdlog::level::level_enum>(5 - x);
}
}

extern "C" {
/** @short Propagate sysrepo events to spdlog */
static void spdlog_sr_log_cb(sr_log_level_t level, const char* message)
{
    // Thread safety note: this is, as far as I know, thread safe:
    // - the static initialization itself is OK
    // - all loggers which we instantiate are thread-safe
    // - std::shared_ptr::operator-> is const, and all const members of that class are documented to be thread-safe
    static auto log = spdlog::get("sysrepo");
    assert(log);
    switch (level) {
    case SR_LL_NONE:
    case SR_LL_ERR:
        log->error(message);
        break;
    case SR_LL_WRN:
        log->warn(message);
        break;
    case SR_LL_INF:
        log->info(message);
        break;
    case SR_LL_DBG:
        log->debug(message);
        break;
    }
}
}

int main(int argc, char* argv [])
{
    auto args = docopt::docopt(usage, {argv + 1, argv + argc}, true,""/* version */, true);
    auto timeout = std::chrono::milliseconds{0};

    if (args["--timeout"]) {
        timeout = std::chrono::milliseconds{args["--timeout"].asLong() * 1000};
    }
    if (args["--syslog"].asBool()) {
        auto syslog_sink = std::make_shared<spdlog::sinks::syslog_sink_mt>("rousette", LOG_PID, LOG_USER, true);
        auto logger = std::make_shared<spdlog::logger>("rousette", syslog_sink);
        spdlog::set_default_logger(logger);
#ifdef HAVE_SYSTEMD
    } else if (is_journald_active()) {
        auto sink = std::make_shared<journald_sink<std::mutex>>();
        auto logger = std::make_shared<spdlog::logger>("rousette", sink);
        spdlog::set_default_logger(logger);
#endif
    } else {
        auto stdout_sink = std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("rousette", stdout_sink);
        spdlog::set_default_logger(logger);
    }
    spdlog::set_level(spdlog::level::trace);

    spdlog::register_logger(std::make_shared<spdlog::logger>("sysrepo", spdlog::default_logger()->sinks().front()));
    spdlog::get("sysrepo")->set_level(parseLogLevel("Sysrepo library", args["--sysrepo-log-level"]));
    sr_log_set_cb(spdlog_sr_log_cb);

    /* We will parse URIs using boost::spirit's alnum/alpha/... matchers which are locale-dependent.
     * Let's use something stable no matter what the system is using
     */
    if (!std::setlocale(LC_CTYPE, "C.UTF-8")) {
        throw std::runtime_error("Could not set locale C.UTF-8");
    }

    auto conn = sysrepo::Connection{};
    auto server = rousette::restconf::Server{conn, "::1", "10080", timeout};
    signal(SIGTERM, [](int) {});
    signal(SIGINT, [](int) {});
    pause();

    return 0;
}
