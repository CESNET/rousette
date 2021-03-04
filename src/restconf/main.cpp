/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Session.hpp>
#include "restconf/Server.h"

int main(int argc [[maybe_unused]], char* argv [[maybe_unused]] [])
{
    spdlog::set_level(spdlog::level::trace);

    auto conn = std::make_shared<sysrepo::Connection>();
    auto server = rousette::restconf::Server{conn};
    server.listen_and_serve("::1", "10080");

    return 0;
}
