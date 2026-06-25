#ifndef VEDIT_DOMAIN_FILTER_CATALOG_H
#define VEDIT_DOMAIN_FILTER_CATALOG_H

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vedit {

// One parameter on a MLT filter, read from the yml metadata.
struct FilterParam {
    std::string identifier;
    std::string title;
    std::string type;         // "float"|"integer"|"string"|"boolean"|"rect"|"color"|"time"|"geometry"
    nlohmann::json default_value;
    double minimum = 0.0;
    double maximum = 0.0;
    bool mutable_ = true;
    bool animation = false;
    std::string widget;       // UI hint: "slider"|"checkbox"|"text"|"dropdown"|"spinner"|"timecode"
    std::vector<std::string> values;  // enum values for dropdown/combo
};

// One MLT filter service, discovered from the repository at init time.
struct FilterDef {
    std::string id;           // MLT service identifier, e.g. "brightness"
    std::string title;
    std::string description;
    bool is_audio = false;    // tagged "Audio"
    bool is_video = false;    // tagged "Video" (or neither = both)
    std::vector<FilterParam> params;
};

// Builds the filter catalog from the MLT repository at init time.
// The catalog is populated once (read-only after construction) and
// drives the filter.list RPC, the state.get filters.available array,
// and the GUI filter picker. Adding a new MLT filter requires zero
// control-plane code — the yml metadata is read generically (P1).
class FilterCatalog {
public:
    FilterCatalog();

    const std::vector<FilterDef>& filters() const { return filters_; }
    const FilterDef* find(const std::string& id) const;

private:
    std::vector<FilterDef> filters_;
};

} // namespace vedit

#endif
