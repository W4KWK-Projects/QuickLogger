#include "geo_utils.hpp"

#include <cmath>
#include <set>

namespace ql
{

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

    std::vector<std::string> NearbyZip3Prefixes(double origin_lat, double origin_lon,
                                                const std::vector<ZipCentroid>& centroids)
    {
        std::set<std::string> prefixes;
        for (const ZipCentroid& centroid : centroids)
        {
            if (centroid.zip.size() < 3)
            {
                continue;
            }
            if (DistanceMiles(origin_lat, origin_lon, centroid.lat, centroid.lon) <=
                kNearbyRadiusMiles)
            {
                prefixes.insert(centroid.zip.substr(0, 3));
            }
        }
        return std::vector<std::string>(prefixes.begin(), prefixes.end());
    }

}  // namespace ql
