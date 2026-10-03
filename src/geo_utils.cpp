#include "geo_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <utility>

#include "text_utils.hpp"

namespace ql
{

    static bool NearerZip(const NearbyZip& a, const NearbyZip& b)
    {
        return a.miles < b.miles || (a.miles == b.miles && a.zip < b.zip);
    }

    static constexpr double kEarthRadiusMiles = 3958.8;

    static double DegreesToRadians(double degrees)
    {
        return degrees * 3.14159265358979323846 / 180.0;
    }

    double DistanceMiles(double lat1, double lon1, double lat2, double lon2)
    {
        double lat1_rad = DegreesToRadians(lat1);
        double lat2_rad = DegreesToRadians(lat2);
        double delta_lat = DegreesToRadians(lat2 - lat1);
        double delta_lon = DegreesToRadians(lon2 - lon1);

        double sin_lat = std::sin(delta_lat / 2.0);
        double sin_lon = std::sin(delta_lon / 2.0);
        double a = sin_lat * sin_lat + std::cos(lat1_rad) * std::cos(lat2_rad) * sin_lon * sin_lon;
        double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
        return kEarthRadiusMiles * c;
    }

    static bool IsAsciiDigit(char c)
    {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    }

    static bool IsAsciiLetter(char c)
    {
        return std::isalpha(static_cast<unsigned char>(c)) != 0;
    }

    std::string ZipCentroidKey(const std::string& zip)
    {
        if (zip.size() >= 5 && std::all_of(zip.begin(), zip.begin() + 5, IsAsciiDigit))
        {
            return zip.substr(0, 5);
        }
        if (zip.size() >= 3 && IsAsciiLetter(zip[0]) && IsAsciiDigit(zip[1]) && IsAsciiLetter(zip[2]))
        {
            return ToUpperAscii(zip.substr(0, 3));
        }
        return std::string();
    }

    std::string MaidenheadGrid4(double lat, double lon)
    {
        if (!(lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0))
        {
            return std::string();
        }
        // The poles and the antimeridian belong to the last square.
        double x = std::min(lon + 180.0, 359.999999);
        double y = std::min(lat + 90.0, 179.999999);
        int field_lon = static_cast<int>(x / 20.0);
        int field_lat = static_cast<int>(y / 10.0);
        int square_lon = static_cast<int>((x - field_lon * 20.0) / 2.0);
        int square_lat = static_cast<int>(y - field_lat * 10.0);
        std::string grid;
        grid += static_cast<char>('A' + field_lon);
        grid += static_cast<char>('A' + field_lat);
        grid += static_cast<char>('0' + square_lon);
        grid += static_cast<char>('0' + square_lat);
        return grid;
    }

    std::vector<NearbyZip> NearbyZips(double origin_lat, double origin_lon, double radius_miles,
                                      const std::vector<ZipCentroid>& centroids)
    {
        std::vector<NearbyZip> nearby;
        for (const ZipCentroid& centroid : centroids)
        {
            double miles = DistanceMiles(origin_lat, origin_lon, centroid.lat, centroid.lon);
            if (miles <= radius_miles)
            {
                NearbyZip zip;
                zip.zip = centroid.zip;
                zip.miles = miles;
                nearby.push_back(std::move(zip));
            }
        }
        std::sort(nearby.begin(), nearby.end(), NearerZip);
        return nearby;
    }

    std::vector<std::string> NearbyZip3Prefixes(double origin_lat, double origin_lon, double radius_miles,
                                                const std::vector<ZipCentroid>& centroids)
    {
        std::set<std::string> prefixes;
        for (const ZipCentroid& centroid : centroids)
        {
            if (centroid.zip.size() < 3)
            {
                continue;
            }
            if (DistanceMiles(origin_lat, origin_lon, centroid.lat, centroid.lon) <= radius_miles)
            {
                prefixes.insert(centroid.zip.substr(0, 3));
            }
        }
        return std::vector<std::string>(prefixes.begin(), prefixes.end());
    }

}  // namespace ql
