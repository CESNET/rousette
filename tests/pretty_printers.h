/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#pragma once

#include "trompeloeil_doctest.h"
#include <optional>
#include <sstream>

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

namespace doctest {
template <>
struct StringMaker<std::optional<std::string>> {
    static String convert(const std::optional<std::string>& value)
    {
        if (value) {
            return value->c_str();
        }
        return "std::nullopt";
    }
};
}
