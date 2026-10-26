// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The chosen map handed to the mutator pipeline, and the reread that makes
// every match reader see a rewritten scenario. The installed file is what
// the setup screens keep; the rewrite is read only after the match starts.
#include "oa/app/runtime.hpp"

#include "oa/data/mutators/maps.hpp"
#include "oa/formats/ota.hpp"
#include "oa/formats/tdf.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

std::optional<oa::data::mutators::ChosenMap> Runtime::chosen_map_for_mutators() {
    if (selected_map_name_runtime_.empty())
        return std::nullopt;
    oa::data::mutators::ChosenMap chosen;
    chosen.key = selected_map_name_runtime_;
    chosen.ota_path = "maps/" + chosen.key + ".ota";
    const std::size_t mark = chosen.key.rfind('@');
    if (mark != std::string::npos && mark + 1 < chosen.key.size())
        chosen.pack = chosen.key.substr(mark + 1);
    if (const std::optional<std::vector<uint8_t>> bytes = read(chosen.ota_path))
        chosen.ota = std::make_shared<const std::vector<uint8_t>>(std::move(*bytes));
    return chosen;
}

void Runtime::reread_chosen_map(bool rebind_campaign) {
    if (selected_map_name_runtime_.empty())
        return;
    const std::optional<std::vector<uint8_t>> ota_data =
        read("maps/" + selected_map_name_runtime_ + ".ota");
    if (!ota_data)
        throw std::runtime_error("the chosen map could not be read");
    const std::string_view ota_text(
        reinterpret_cast<const char*>(ota_data->data()), ota_data->size()
    );
    auto parsed = oa::formats::ota::parse(ota_text);
    if (!parsed.ok())
        throw std::runtime_error("cannot parse selected map metadata: " + parsed.error->message);
    oa::formats::tdf::OwnedDocument scenario_document;
    oa::formats::tdf::ParseError scenario_error{};
    if (!scenario_document.parse(ota_text, &scenario_error))
        throw std::runtime_error(
            "cannot parse selected scenario definitions: " +
            oa::formats::tdf::describe(scenario_error)
        );
    selected_map_metadata_ = std::move(*parsed.metadata);
    selected_ota_document_ = std::move(scenario_document);
    clear_campaign_map_cache();
    if (rebind_campaign && campaign_mission_)
        rebind_campaign_mission();
}

} // namespace oa::app
