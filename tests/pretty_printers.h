/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#pragma once

#include <optional>
#include <sstream>
#include <trompeloeil.hpp>

namespace trompeloeil {
template <>
struct printer<std::optional<std::string>> {
    static void print(std::ostream& os, const std::optional<std::string>& o)
    {
        if (o) {
            os << *o;
        } else {
            os << "std::nullopt";
        }
    }
};
}
