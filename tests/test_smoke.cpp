#include <catch2/catch_test_macros.hpp>

#include "core/mlt_runtime.h"
#include "domain/project_model.h"

TEST_CASE("MltRuntime init is idempotent and reports ok", "[core]") {
    vedit::MltRuntime mlt;
    REQUIRE(mlt.init());
    REQUIRE(mlt.ok());
    REQUIRE(mlt.init()); // idempotent
    mlt.shutdown();
    REQUIRE_FALSE(mlt.ok());
}

TEST_CASE("ProjectModel constructs", "[domain]") {
    vedit::ProjectModel model;
    REQUIRE(true);
}
