// Desktop version of the MPC SDK's resource helpers.
// On the MPC a plugin keeps its files next to itself on the drive. On a computer each
// T-MU plugin gets a folder in the user's Music folder instead:
//     macOS    ~/Music/T-MU/<Plugin Name>
//     Windows  C:\Users\<you>\Music\T-MU\<Plugin Name>
// The JUCE bridge sets this before the plugin engine is created. Paths use '/' everywhere
// (Windows accepts that), so engine code written for the MPC works unchanged.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>

namespace mpc {

inline std::string& desktopFolderRef() { static std::string f; return f; }
inline void setPluginFolder(const std::string& f) { desktopFolderRef() = f; }

inline const std::string& pluginFolder() { return desktopFolderRef(); }
inline std::string contentFolder() { return pluginFolder() + "/Content"; }
inline std::string contentPath(const std::string& relative)
{
    if (relative.empty()) return contentFolder();
    return relative[0] == '/' ? contentFolder() + relative : contentFolder() + "/" + relative;
}

// fopen that understands UTF-8 paths on Windows too (implemented in the bridge)
std::FILE* openFile(const std::string& path, const char* mode);

inline bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::FILE* f = openFile(path, "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}
inline bool readTextFile(const std::string& path, std::string& out)
{
    std::vector<uint8_t> b;
    if (!readFile(path, b)) return false;
    out.assign(b.begin(), b.end());
    return true;
}

} // namespace mpc
