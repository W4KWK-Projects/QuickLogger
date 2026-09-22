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

    // The distinct 3-digit ZIP prefixes among `centroids` whose centroid is
    // within kNearbyRadiusMiles of (origin_lat, origin_lon). This is a coarse,
    // SQL-index-friendly pre-filter (see
    // Database::SearchUlsStationsByCallsignAndZip3Prefixes) -- callers should
    // still apply an exact-distance check afterward for stations whose own
    // ZIP centroid is known, since a ZIP3 region can be larger than the
    // radius in sparsely-populated areas.
    std::vector<std::string> NearbyZip3Prefixes(double origin_lat, double origin_lon,
                                                const std::vector<ZipCentroid>& centroids);

}  // namespace ql
