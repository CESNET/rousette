/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <csignal>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Session.hpp>
#include <unistd.h>
#include "sr/AllEvents.h"

using namespace rousette::sr;

int main(int argc [[maybe_unused]], char* argv [[maybe_unused]] [])
{
    spdlog::set_level(spdlog::level::trace);

    auto sess = sysrepo::Connection{}.sessionStart();
    auto e = AllEvents{
        sess,
        /* AllEvents::WithAttributes::All, */
        AllEvents::WithAttributes::RemoveEmptyOperationAndOrigin,
        /* AllEvents::WithAttributes::None, */
    };

    signal(SIGTERM, [](int) {});
    signal(SIGINT, [](int) {});
    pause();

    return 0;
}
