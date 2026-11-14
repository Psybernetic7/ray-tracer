#ifndef SOLAR_H
#define SOLAR_H

#include "rtheader.h"


// Where the sun is in the sky, for a latitude, a day of the year and a time of day.
struct solar_position {
    vec3   direction;  // Unit vector toward the sun: y up, north at -z, east at +x
    double elevation;  // Degrees above the horizon; negative at night
    double azimuth;    // Compass degrees: 0 north, 90 east, 180 south, 270 west
};

// NOAA's general solar position equations (the Fourier-series fits for the sun's declination and
// the equation of time, from Spencer 1971). Accurate to well under a degree, which is far finer
// than any terrain analysis here needs, and cheap enough to evaluate thousands of times.
//
// latitude_degrees: positive north. day_of_year: 1 for 1 January, fractions allowed.
// solar_hour: local mean solar time, i.e. clock time at a longitude that sits on its time zone's
// meridian, without daylight saving. Analysis integrates over whole days, so where a site sits
// inside its time zone shifts its clock but never its sun-hours.
inline solar_position sun_position(double latitude_degrees, double day_of_year, double solar_hour) {
    // The year as an angle, so the series below are periodic in it.
    const double gamma = 2*pi/365 * (day_of_year - 1 + (solar_hour - 12) / 24);

    // How far solar noon drifts from clock noon through the year, in minutes (about +/-16).
    const double equation_of_time = 229.18 * (0.000075 + 0.001868*std::cos(gamma)
        - 0.032077*std::sin(gamma) - 0.014615*std::cos(2*gamma) - 0.040849*std::sin(2*gamma));

    // The sun's latitude on the celestial sphere: +23.44 degrees at the June solstice.
    const double declination = 0.006918 - 0.399912*std::cos(gamma) + 0.070257*std::sin(gamma)
        - 0.006758*std::cos(2*gamma) + 0.000907*std::sin(2*gamma)
        - 0.002697*std::cos(3*gamma) + 0.00148*std::sin(3*gamma);

    // Hour angle: how far the earth has turned past solar noon, 15 degrees per hour.
    const double true_solar_minutes = solar_hour * 60 + equation_of_time;
    const double hour_angle = degrees_to_radians(true_solar_minutes / 4 - 180);

    // The sun's direction in local east/north/up coordinates. Working with the vector instead
    // of the textbook zenith and azimuth formulas avoids their acos quadrant fix-ups.
    const double lat = degrees_to_radians(latitude_degrees);
    const double east  = -std::cos(declination) * std::sin(hour_angle);
    const double north = std::cos(lat)*std::sin(declination)
                       - std::sin(lat)*std::cos(declination)*std::cos(hour_angle);
    const double up    = std::sin(lat)*std::sin(declination)
                       + std::cos(lat)*std::cos(declination)*std::cos(hour_angle);

    solar_position pos;
    pos.direction = unit_vector(vec3(east, up, -north));
    pos.elevation = std::asin(std::fmin(std::fmax(up, -1.0), 1.0)) * 180 / pi;
    pos.azimuth   = std::atan2(east, north) * 180 / pi;
    if (pos.azimuth < 0)
        pos.azimuth += 360;
    return pos;
}

// Clear-sky direct normal irradiance in kW/m^2: sunlight arriving straight from the sun's disk,
// on a surface facing it. Meinel's model (1976) attenuates the solar constant by the air mass,
// with the air mass from Kasten and Young (1989) so it stays finite down at the horizon. It
// ignores cloud and diffuse sky light, which makes it an upper bound, but a consistent one for
// comparing one slope with another.
inline double clear_sky_dni(double elevation_degrees) {
    if (elevation_degrees <= 0)
        return 0;
    const double zenith = 90 - elevation_degrees;
    const double air_mass = 1 / (std::cos(degrees_to_radians(zenith))
                                 + 0.50572 * std::pow(96.07995 - zenith, -1.6364));
    return 1.353 * std::pow(0.7, std::pow(air_mass, 0.678));
}

#endif
