#include "environment.hpp"

#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef __linux__
#include <sched.h>
#include <sys/utsname.h>
#endif

#ifndef LOB_BUILD_TYPE
#define LOB_BUILD_TYPE "unknown"
#endif
#ifndef LOB_CXX_FLAGS
#define LOB_CXX_FLAGS "unknown"
#endif
#ifndef LOB_SANITIZERS
#define LOB_SANITIZERS 0
#endif

namespace lob::bench {

namespace {

std::string first_line_with(const std::string& path, const std::string& key) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(key, 0) == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string v = line.substr(colon + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                return v;
            }
        }
    }
    return {};
}

std::string read_first_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

}  // namespace

Environment collect_environment() {
    Environment e;
    e.cpu_model = first_line_with("/proc/cpuinfo", "model name");
    if (e.cpu_model.empty()) e.cpu_model = "unknown";
    e.logical_cpus = std::thread::hardware_concurrency();

#ifdef __linux__
    utsname u{};
    if (uname(&u) == 0) e.os = std::string(u.sysname) + " " + u.release + " " + u.machine;
#endif
    if (e.os.empty()) e.os = "unknown";

#if defined(__clang__)
    e.compiler = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    e.compiler = std::string("gcc ") + __VERSION__;
#else
    e.compiler = "unknown";
#endif
    e.build_type = LOB_BUILD_TYPE;
    e.cxx_flags = LOB_CXX_FLAGS;
    e.sanitizers = LOB_SANITIZERS != 0;
#ifdef NDEBUG
    e.asserts_enabled = false;
#else
    e.asserts_enabled = true;
#endif

    e.cpu_governor = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (e.cpu_governor.empty()) e.cpu_governor = "unknown";

    const std::time_t now = std::time(nullptr);
    char buf[32];
    std::tm tm{};
#ifdef __linux__
    gmtime_r(&now, &tm);
#endif
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    e.timestamp_utc = buf;
    return e;
}

bool pin_to_cpu(int cpu) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof set, &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

long read_rss_kb() {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream ss(line.substr(6));
            long kb = -1;
            ss >> kb;
            return kb;
        }
    }
    return -1;
}

}  // namespace lob::bench
