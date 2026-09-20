#pragma once
#include <string>

namespace Models {

// Maven 坐标 "groupId:artifactId:version[:classifier]"
struct MavenCoord {
    std::string groupId;
    std::string artifactId;
    std::string version;
    std::string classifier;  // 可选，如 "natives-windows"
};

} // namespace Models
