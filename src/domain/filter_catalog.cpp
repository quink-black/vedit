#include "filter_catalog.h"

#include "util/logger.h"

#include <Mlt.h>

namespace vedit {

namespace {

nlohmann::json read_yml_default(const std::string& type, Mlt::Properties& param_props) {
    if (type == "float") {
        return param_props.get_double("default");
    } else if (type == "integer") {
        return param_props.get_int("default");
    } else if (type == "boolean") {
        return param_props.get_int("default") != 0;
    } else {
        // string, rect, color, time, geometry — fall back to string
        const char* v = param_props.get("default");
        return v ? nlohmann::json(v) : nlohmann::json(nullptr);
    }
}

} // namespace

FilterCatalog::FilterCatalog() {
    // MLT repository must be initialized (Factory::init() done by
    // test_main.cpp or main.cpp before this constructor runs).
    // We read the repository's filter list and extract yml metadata
    // for each registered filter service.
    Mlt::Repository* repo = Mlt::Factory::init();
    if (!repo) {
        Logger::log(Logger::Level::Error, "FilterCatalog: no MLT repository");
        return;
    }
    Mlt::Properties* filters_raw = repo->filters();
    if (!filters_raw) return;
    Mlt::Properties filters(filters_raw->get_properties());
    int count = filters.count();
    filters_.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        const char* service_id = filters.get_name(i);
        if (!service_id || !service_id[0]) continue;

        // Skip non-filter services that happen to be in the filters
        // list (e.g. internal MLT link services).
        Mlt::Properties* meta = repo->metadata(mlt_service_filter_type, service_id);
        if (!meta || !meta->is_valid()) {
            delete meta;
            continue;
        }

        FilterDef def;
        def.id = service_id;
        const char* title = meta->get("title");
        def.title = title ? title : service_id;
        const char* desc = meta->get("description");
        def.description = desc ? desc : "";

        // Tags: "Audio" and/or "Video". If neither is present, the
        // filter works on both types.
        mlt_properties tags_raw = mlt_properties_get_properties(
            meta->get_properties(), "tags");
        if (tags_raw) {
            Mlt::Properties tags(tags_raw);
            def.is_audio = tags.get_int("Audio") != 0;
            def.is_video = tags.get_int("Video") != 0;
        }

        // Enumerate parameters from the yml "parameters" child.
        mlt_properties params_raw = mlt_properties_get_properties(
            meta->get_properties(), "parameters");
        if (params_raw) {
            Mlt::Properties params(params_raw);
            int pc = params.count();
            for (int p = 0; p < pc; ++p) {
                const char* param_id = params.get_name(p);
                if (!param_id || !param_id[0]) continue;

                mlt_properties param_raw = mlt_properties_get_properties(
                    params.get_properties(), param_id);
                if (!param_raw) continue;
                Mlt::Properties param_props(param_raw);

                FilterParam fp;
                fp.identifier = param_id;
                const char* pt = param_props.get("title");
                fp.title = pt ? pt : param_id;
                const char* ptype = param_props.get("type");
                fp.type = ptype ? ptype : "string";
                fp.default_value = read_yml_default(fp.type, param_props);
                fp.minimum = param_props.get_double("minimum");
                fp.maximum = param_props.get_double("maximum");
                fp.mutable_ = param_props.get_int("mutable") != 0;
                fp.animation = param_props.get_int("animation") != 0;
                const char* widget = param_props.get("widget");
                fp.widget = widget ? widget : "";

                // Enum values for dropdown/combo types.
                mlt_properties vals_raw = mlt_properties_get_properties(
                    param_props.get_properties(), "values");
                if (vals_raw) {
                    Mlt::Properties vprops(vals_raw);
                    int vc = vprops.count();
                    for (int vi = 0; vi < vc; ++vi) {
                        const char* vname = vprops.get_name(vi);
                        if (vname) fp.values.push_back(vname);
                    }
                }

                def.params.push_back(std::move(fp));
            }
        }

        filters_.push_back(std::move(def));
        delete meta;
    }

    Logger::log(Logger::Level::Info,
        ("FilterCatalog: loaded " + std::to_string(filters_.size())
         + " filters").c_str());
}

const FilterDef* FilterCatalog::find(const std::string& id) const {
    for (const auto& f : filters_) {
        if (f.id == id) return &f;
    }
    return nullptr;
}

} // namespace vedit
