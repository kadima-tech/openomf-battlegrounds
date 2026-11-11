#include "har.h"

namespace ed {

std::vector<std::string> base_fighters(const BaseData &) { return {}; }
std::vector<uint32_t> addon_hars(const Project &) { return {}; }
HarObjects har_objects(const Project &, uint32_t) { return {}; }
uint32_t create_har_variant(Project &, const std::string &, const std::string &) { throw deo::Error("not yet"); }
HarData read_har(const Project &, const HarObjects &) { return {}; }
void write_har(Project &, const HarObjects &, const HarData &) {}

}  // namespace ed
