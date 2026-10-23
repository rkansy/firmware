#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

namespace meshtastic_mcp
{
inline bool localDayBounds(const std::string &value, time_t &start, time_t &end)
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        return false;
    int year = 0, month = 0, day = 0;
    if (sscanf(value.c_str(), "%4d-%2d-%2d", &year, &month, &day) != 3)
        return false;
    struct tm local = {};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_isdst = -1;
    start = mktime(&local);
    if (start == (time_t)-1)
        return false;
    struct tm check;
    localtime_r(&start, &check);
    if (check.tm_year != year - 1900 || check.tm_mon != month - 1 || check.tm_mday != day)
        return false;
    check.tm_mday++;
    check.tm_isdst = -1;
    end = mktime(&check);
    return end != (time_t)-1;
}

inline double distanceKm(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double pi = 3.14159265358979323846;
    const double p1 = lat1 * pi / 180.0;
    const double p2 = lat2 * pi / 180.0;
    const double dp = (lat2 - lat1) * pi / 180.0;
    const double dl = (lon2 - lon1) * pi / 180.0;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * 6371.0 * std::asin(std::sqrt(std::clamp(a, 0.0, 1.0)));
}
} // namespace meshtastic_mcp
