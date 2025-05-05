/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#pragma once
#include <optional>
#include <string>

namespace rousette::auth {
/** @brief Talk to PAM
 *
 * @param blob Raw data from the Authorization HTTP header
 * @param remoteHost Arbitrary debugging info about the remote host which triggered this action
 *
 * @return the authenticated username
 */
std::string authenticate_pam(const std::string& blob, const std::optional<std::string>& remoteHost);
}
