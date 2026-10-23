#include "Arduino.h"
#include "TestUtil.h"
#include "mesh/raspihttp/McpMath.h"
#include <cmath>
#include <cstdlib>
#include <string>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

struct TimeZoneGuard {
    const char *current = getenv("TZ");
    bool wasSet = current != nullptr;
    std::string previous = current ? current : "";
    ~TimeZoneGuard()
    {
        if (wasSet)
            setenv("TZ", previous.c_str(), 1);
        else
            unsetenv("TZ");
        tzset();
    }
};

void test_distanceKm_antipodal_coordinates_are_finite()
{
    const double distance = meshtastic_mcp::distanceKm(0.0, 0.0, 0.0, 180.0);
    TEST_ASSERT_TRUE(std::isfinite(distance));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 20015.086796020572, distance);
}

void test_distanceKm_rounding_above_one_is_clamped()
{
    const double distance =
        meshtastic_mcp::distanceKm(-22.553456309704742, -21.973813183957276, 22.55345632655772, 158.02618737292795);
    TEST_ASSERT_TRUE(std::isfinite(distance));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 20015.086796020572, distance);
}

void test_distanceKm_near_antipodal_coordinates_are_finite()
{
    const double distance = meshtastic_mcp::distanceKm(51.5, 0.0, -41.3, 174.8);
    TEST_ASSERT_TRUE(std::isfinite(distance));
    TEST_ASSERT_TRUE(distance > 18000.0 && distance < 20015.1);
}

void test_localDayBounds_handles_dst_transition_days()
{
    TimeZoneGuard restoreTimezone;
    setenv("TZ", "Europe/Berlin", 1);
    tzset();

    time_t start, end;
    TEST_ASSERT_TRUE(meshtastic_mcp::localDayBounds("2026-03-29", start, end));
    TEST_ASSERT_EQUAL_INT64(23 * 3600, end - start);
    TEST_ASSERT_TRUE(meshtastic_mcp::localDayBounds("2026-10-25", start, end));
    TEST_ASSERT_EQUAL_INT64(25 * 3600, end - start);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_distanceKm_antipodal_coordinates_are_finite);
    RUN_TEST(test_distanceKm_rounding_above_one_is_clamped);
    RUN_TEST(test_distanceKm_near_antipodal_coordinates_are_finite);
    RUN_TEST(test_localDayBounds_handles_dst_transition_days);
    exit(UNITY_END());
}

void loop() {}
