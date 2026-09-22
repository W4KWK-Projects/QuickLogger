#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../models.hpp"

struct sqlite3;

namespace ql
{

    // Owns the sqlite3 connection for QuickLogger's local database and
    // creates the schema (if it doesn't already exist) on construction.
    class Database
    {
    public:
        // Pass ":memory:" for a private in-memory database, e.g. in tests.
        explicit Database(const std::string& path);
        ~Database();

        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        // Stations. Insert-or-update by callsign, since a Station represents
        // everything currently known about that callsign, not a check-in log.
        void UpsertStation(const Station& station);
        // Insert-or-update the editable fields the New Station modal and the
        // edit-net saved-station form collect (name, member_id, street_address,
        // city, county, state, zip, grid_square), keyed by `station.callsign`. On
        // conflict, each field only overwrites the stored value when non-empty
        // in `station`, so leaving a field blank (because the operator didn't
        // retype what's already on file) never erases previously known data.
        void RecordManualCheckInStation(const Station& station, std::int64_t updated_at);
        // Directly sets an existing Station's editable fields (the same set as
        // RecordManualCheckInStation) to exactly what's given in `station`,
        // including blanking any of them out. Unlike RecordManualCheckInStation,
        // this never preserves the previous value -- it's for the Edit Check-in
        // dialog, where the operator is making an explicit correction rather
        // than leaving a field blank because they didn't retype known data.
        void UpdateStationFields(const Station& station, std::int64_t updated_at);
        std::optional<Station> FindStationByCallsign(const std::string& callsign);
        // Matches any callsign containing `substring` (case-insensitive),
        // e.g. "4FA" matches "AA4FA". This is autocomplete's second tier: any
        // station known elsewhere in the system (i.e. from another net),
        // ranked below stations known to this specific net
        // (SearchNetStationsByCallsignSubstring) but ahead of the not-yet-built
        // third tier, QRZ/ULS -- which will be a separate query appended after
        // this one once it exists, not merged into it.
        std::vector<Station> SearchStationsByCallsignSubstring(const std::string& substring);
        // Matches any callsign containing `substring` (case-insensitive) among
        // stations that have either checked into a past instance of `net_id`,
        // or been explicitly saved to it (see SaveNetStation) -- e.g. imported
        // from another logging program's history. This is autocomplete's first
        // tier: prefer callers already known to this specific net before
        // broadening to SearchStationsByCallsignSubstring (other nets).
        std::vector<Station> SearchNetStationsByCallsignSubstring(std::int64_t net_id,
                                                                  const std::string& substring);
        // Associates `station.callsign` with `net_id` as "known to this net"
        // for autocomplete, without a real check-in -- for saving a station
        // from an external source (e.g. another logging program's history).
        // Also merge-upserts the Station itself (see RecordManualCheckInStation).
        // `default_remarks` is carried on the net/callsign association (not
        // the Station), and gets copied into the New Station modal's Remarks
        // field when this station is picked via autocomplete for this net.
        void SaveNetStation(std::int64_t net_id, const Station& station,
                            const std::string& default_remarks, std::int64_t updated_at);
        // Directly sets an already-saved station's fields and default remarks
        // to exactly what's given in `station`/`default_remarks`, including
        // blanking any of them out. Unlike SaveNetStation, this never preserves
        // a previous value -- it's for explicitly editing a station the
        // operator already saved to this net (e.g. adding details they didn't
        // have when they first saved just the callsign), not adding a new one.
        void UpdateSavedNetStation(std::int64_t net_id, const Station& station,
                                   const std::string& default_remarks, std::int64_t updated_at);
        // Removes a station's saved association with a net. Does not touch the
        // Station record itself, nor any real check-in history.
        void RemoveSavedNetStation(std::int64_t net_id, const std::string& callsign);
        // Removes `callsign` entirely: its saved association with every net
        // (not just one) and the Station record itself. Refuses (returns
        // false, changes nothing) if the station has any real check-in
        // history, since that would either violate the check_ins->stations
        // foreign key or silently destroy logged history -- callers should
        // direct the operator to RemoveSavedNetStation instead in that case.
        bool DeleteStationCompletely(const std::string& callsign);
        // Stations explicitly saved to `net_id` (not those merely known via
        // real check-in history) -- for the edit-net page's saved-station list.
        std::vector<Station> GetSavedStationsForNet(std::int64_t net_id);
        // The default remarks saved for `callsign` on `net_id`, or an empty
        // string if there's no saved row or no default was set. Used to
        // prefill the New Station modal's Remarks field when autocomplete
        // picks a station that was saved (rather than genuinely checked in)
        // for this net.
        std::string GetSavedNetStationRemarks(std::int64_t net_id, const std::string& callsign);

        // Nets (recurring net definitions).
        std::int64_t CreateNet(const Net& net);
        void UpdateNet(const Net& net);
        std::vector<Net> GetAllNets();
        std::optional<Net> GetNetById(std::int64_t net_id);

        // Net instances (one dated occurrence of a Net).
        std::int64_t CreateNetInstance(const NetInstance& instance);
        std::vector<NetInstance> GetNetInstancesForNet(std::int64_t net_id);
        std::optional<NetInstance> GetNetInstanceById(std::int64_t instance_id);
        void CloseNetInstance(std::int64_t instance_id, std::int64_t closed_at);
        // Sets one of the instance's three role-callsign columns (kRoleNetControl/
        // kRoleAlternateNetControl/kRoleLogger) to `callsign` -- `callsign` is
        // "" to clear it. Used by ApplyCheckInRoleDesignation to keep these
        // columns in sync with CheckIn::designated_role without a
        // general-purpose "update the whole NetInstance" method. A no-op if
        // `role` isn't one of the three (in particular, kRoleNone).
        void SetNetInstanceRoleCallsign(std::int64_t instance_id, int role,
                                        const std::string& callsign);
        // Permanently removes one net instance (e.g. logged by mistake, or a
        // test/practice run someone wants gone from history) and all of its
        // check-ins -- check_ins.net_instance_id references net_instances(id)
        // with foreign keys enforced, so the check-ins must go first. Wrapped
        // in one transaction so a failure can't leave check-ins orphaned from
        // a half-deleted instance.
        void DeleteNetInstance(std::int64_t instance_id);

        // Check-ins (one station's check-in during one NetInstance).
        std::int64_t AddCheckIn(const CheckIn& check_in);
        std::vector<CheckIn> GetCheckInsForNetInstance(std::int64_t net_instance_id);
        void UpdateCheckIn(const CheckIn& check_in);
        // Removes one check-in entry entirely (e.g. logged in error). Does not
        // touch the Station record or renumber other check-ins' sequence
        // numbers -- a gap in the sequence is harmless, it's just a display
        // ordinal.
        void DeleteCheckIn(std::int64_t check_in_id);
        // Clears designated_role back to kRoleNone on every check-in in
        // `net_instance_id` currently holding `role`, except `except_check_in_id`
        // -- so handing a role to one check-in takes it away from whoever had
        // it, since only one check-in per instance can hold a given role.
        void ClearCheckInRoleForInstance(std::int64_t net_instance_id, int role,
                                         std::int64_t except_check_in_id);

        // Bulk-import bookkeeping (e.g. the FCC ULS station database import),
        // so a background import's ongoing/complete status survives page
        // navigation and app restarts. `source` is a short fixed key, e.g. "uls".
        std::optional<ImportRunStatus> GetImportRunStatus(const std::string& source);
        void UpsertImportRunStatus(const ImportRunStatus& status);

        // The full FCC ULS license database, kept in its own table rather
        // than merged into `stations` -- deliberately separate so that
        // `stations` (queried on every autocomplete keystroke while logging
        // a real check-in) stays small and fast regardless of how large the
        // bulk ULS import grows. A ULS row is promoted into `stations`
        // (see RecordManualCheckInStation/SaveNetStation) only when the
        // operator actually picks it via autocomplete and logs/saves it.
        // Plain overwrite-on-conflict is correct here (unlike the
        // fill-blanks-only Station upserts) since every row in this table is
        // exclusively ULS-sourced -- there's no manual edit to protect.
        // Wraps the whole batch in one transaction for performance, so
        // callers should pass a few hundred to a few thousand at a time.
        void BulkUpsertUlsStations(const std::vector<Station>& batch, std::int64_t updated_at);
        // Matches any callsign containing `substring` (case-insensitive)
        // among ULS-imported stations whose zip code starts with one of
        // `zip3_prefixes` -- a coarse, index-friendly proximity pre-filter
        // (exact-mileage filtering happens in the caller, using
        // GetAllZipCentroids -- see AppState::zip_centroids_cache, which
        // loads it once per process run rather than re-querying). Capped to
        // a handful of results. This is the saved-station form's
        // proximity-based autocomplete tier -- see uls_import.hpp's geo helpers.
        std::vector<Station> SearchUlsStationsByCallsignAndZip3Prefixes(
            const std::string& substring, const std::vector<std::string>& zip3_prefixes);
        // Exact-callsign lookup against the ULS table, for resolving a
        // specific operator's info (see LogOperatorCheckIn) rather than
        // searching/ranking candidates.
        std::optional<Station> FindUlsStationByCallsign(const std::string& callsign);

        // Approximate lat/lon centroids for US ZIP codes (from the Census
        // Bureau's ZCTA gazetteer), used to estimate distance for the
        // saved-station form's ULS proximity autocomplete. Loaded once by the
        // same background worker that imports ULS data (see uls_import.hpp);
        // BulkUpsertZipCentroids is a plain overwrite-on-conflict batch
        // upsert, same performance rationale as BulkUpsertUlsStations.
        // GetAllZipCentroids is meant to be called once per process run and
        // cached (see AppState::zip_centroids_cache), not queried live.
        void BulkUpsertZipCentroids(const std::vector<ZipCentroid>& batch);
        std::vector<ZipCentroid> GetAllZipCentroids();
        bool HasAnyZipCentroids();

    private:
        void CreateSchema();

        sqlite3* db_ = nullptr;
    };

}  // namespace ql
