#pragma once

#include <string>
#include <vector>

#include "models.hpp"

namespace ql
{

    // Great-circle distance between two lat/lon points, in miles.
    double DistanceMiles(double lat1, double lon1, double lat2, double lon2);

    // Threshold for "known to this net's local area" ULS autocomplete
    // candidates -- see RefreshSavedStationSuggestions in app_state.cpp.
    constexpr double kNearbyRadiusMiles = 70.0;

    // The ZIP codes among `centroids` within kNearbyRadiusMiles of
    // (origin_lat, origin_lon), with their distances, nearest first.
    std::vector<NearbyZip> NearbyZips(double origin_lat, double origin_lon,
                                      const std::vector<ZipCentroid>& centroids);

    // The distinct 3-digit ZIP prefixes among `centroids` whose centroid is
    // within kNearbyRadiusMiles of (origin_lat, origin_lon). Used to find
    // nearby stations whose own ZIP has no centroid on file (e.g. a PO Box
    // ZIP) -- see Database::SearchNearbyUlsStations.
    std::vector<std::string> NearbyZip3Prefixes(double origin_lat, double origin_lon,
                                                const std::vector<ZipCentroid>& centroids);

}  // namespace ql
