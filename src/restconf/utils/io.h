/*
 * Copyright (C) 2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */


namespace rousette::restconf::utils {

bool pipeHasData(const int fd);
bool pipeIsClosedAndNoData(const int fd);
}
