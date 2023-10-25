/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace rousette::auth {

class Error : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/** @brief Talk to PAM
 *
 * @param blob Raw data from the Authorization HTTP header
 * @param pamConfigDir Override systemwide PAM configuration
 * @param remoteHost Arbitrary debugging info about the remote host which triggered this action
 *
 * @return the authenticated username
 */
std::string authenticate_pam(const std::string& blob, const std::optional<std::filesystem::path>& pamConfigDir, const std::optional<std::string>& remoteHost);
}
