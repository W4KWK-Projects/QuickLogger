#pragma once

#include <string>
#include <vector>

#include "models.hpp"

namespace ql
{

    // Great-circle distance between two lat/lon points, in miles.
    double DistanceMiles(double lat1, double lon1, double lat2, double lon2);

    // The ZIP codes among `centroids` within `radius_miles` of
    // (origin_lat, origin_lon), with their distances, nearest first. The
    // radius is the operator's Nearby Radius setting
    // (AppSettings::nearby_radius_miles).
    std::vector<NearbyZip> NearbyZips(double origin_lat, double origin_lon, double radius_miles,
                                      const std::vector<ZipCentroid>& centroids);

    // The distinct 3-digit ZIP prefixes among `centroids` whose centroid is
    // within `radius_miles` of (origin_lat, origin_lon). Used to find nearby
    // stations whose own ZIP has no centroid on file (e.g. a PO Box ZIP) --
    // see Database::SearchNearbyUlsStations.
    std::vector<std::string> NearbyZip3Prefixes(double origin_lat, double origin_lon,
                                                double radius_miles,
                                                const std::vector<ZipCentroid>& centroids);

}  // namespace ql
