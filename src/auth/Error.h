/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#pragma once

#include <chrono>
#include <exception>
#include <optional>

namespace rousette::auth {
class Error : public std::runtime_error {
public:
    std::optional<std::chrono::microseconds> delay;

    Error(const std::string& message, std::optional<std::chrono::microseconds> delay=std::nullopt)
        : std::runtime_error{message}
        , delay(delay)
    {
    }
};
}
