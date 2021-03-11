/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <chrono>

namespace rousette::restconf {

template <typename Clock, typename Precision>
std::string yangDateTime(const std::chrono::time_point<Clock>& timePoint);

}
