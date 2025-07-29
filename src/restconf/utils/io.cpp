/*
 * Copyright (C) 2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <poll.h>
#include "restconf/utils/io.h"

namespace rousette::restconf::utils {
bool pipeHasData(const int fd)
{
    pollfd fds = {
        .fd = fd,
        .events = POLLIN | POLLHUP,
        .revents = 0};

    return poll(&fds, 1, 0) == 1 && fds.revents & POLLIN;
}

bool pipeIsClosedAndNoData(const int fd)
{
    pollfd fds = {
        .fd = fd,
        .events = POLLIN | POLLHUP,
        .revents = 0};

    return poll(&fds, 1, 0) == 1 && fds.revents & POLLHUP && !(fds.revents & POLLIN);
}
}
