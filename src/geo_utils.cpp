#include "geo_utils.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

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

    std::vector<std::string> NearbyZip3Prefixes(double origin_lat, double origin_lon,
                                                double radius_miles,
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
