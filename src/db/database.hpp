#pragma once

#include <cstddef>
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
        // `use_wal` defaults to true for the app's own quicklogger.db, where
        // it lets the UI thread's reads proceed while a background import
        // holds a writer transaction open on a second connection (see the
        // journal_mode pragma in the .cpp). Pass false for a short-lived,
        // single-connection file meant to be handed to someone else whole
        // (e.g. a net-slice export, see net_slice.hpp) -- WAL mode leaves
        // "-wal"/"-shm" sidecar files next to it that a plain file copy
        // would silently leave behind, and there's no second connection
        // for WAL to help with anyway.
        explicit Database(const std::string& path, bool use_wal = true);
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
        // (SearchNetStationsByCallsignSubstring). A ULS tier, if added, would
        // be a separate query appended after this one, not merged into it.
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
        // Removes a station's saved association with a net. Never touches
        // check-in history. Like every delete here, it then drops any
        // station record nothing refers to any more (see DeleteUnusedStations).
        void RemoveSavedNetStation(std::int64_t net_id, const std::string& callsign);
        // Whether `callsign` is saved to some net other than `net_id`, or has
        // checked in anywhere -- i.e. whether its station record would
        // survive being removed from `net_id`'s saved stations.
        bool IsStationUsedOutsideNet(const std::string& callsign, std::int64_t net_id);
        // A station record (name, member ID, address...) lives only as long
        // as something refers to it: a net it's saved to, or a check-in in
        // some log. This deletes the records nothing refers to any more --
        // e.g. a mistyped callsign once it's removed from the net it was
        // saved to -- so they stop turning up in autocomplete. Called at the
        // end of every delete below. Returns how many were deleted.
        int DeleteUnusedStations();
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
        // Deletes `net_id` entirely: every check-in under every instance of
        // it, every instance itself, its saved-station associations, and the
        // net row itself -- in that order, so foreign keys never point at an
        // already-deleted row. Does NOT touch the `stations` table itself
        // directly -- a station may be known to other nets too -- but, like
        // every delete, finishes with DeleteUnusedStations, which drops the
        // records only this net referred to.
        void DeleteNetCompletely(std::int64_t net_id);

        // Net instances (one dated occurrence of a Net).
        std::int64_t CreateNetInstance(const NetInstance& instance);
        std::vector<NetInstance> GetNetInstancesForNet(std::int64_t net_id);
        std::optional<NetInstance> GetNetInstanceById(std::int64_t instance_id);
        void CloseNetInstance(std::int64_t instance_id, std::int64_t closed_at);
        // The nets that have at least one instance still open -- i.e. a net
        // someone is logging right now, or one whose session ended without
        // being closed (see ResumeOpenNet in app_state.hpp).
        std::vector<std::int64_t> GetNetIdsWithOpenInstances();
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

        // Background-data bookkeeping (see ImportRunStatus in models.hpp and
        // data_updater.hpp). `source` is a short fixed key, e.g. "uls".
        std::optional<ImportRunStatus> GetImportRunStatus(const std::string& source);
        // Writes everything except requested_at (see RequestImportRun).
        void UpsertImportRunStatus(const ImportRunStatus& status);
        // Atomically marks `source`'s row "running" as of `now` -- but only
        // if it isn't already running, or it is but its heartbeat is older
        // than `stale_after_seconds` (the process running it died). A single
        // UPSERT with a WHERE on its DO UPDATE, so two processes racing for
        // the same job can't both win. Returns whether this call got the
        // claim; a caller that gets false must not do the job.
        bool TryClaimImportRun(const std::string& source, std::int64_t now,
                               std::int64_t stale_after_seconds);
        // Progress report for a claimed, running job: phase text, percent,
        // records so far, and a fresh heartbeat.
        void UpdateImportProgress(const std::string& source, const std::string& phase, int percent,
                                  std::int64_t records_imported, std::int64_t now);
        // Records that someone asked for `source` to be refreshed now (the
        // updater notices requested_at is newer than the last run's start).
        void RequestImportRun(const std::string& source, std::int64_t now);

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
        // Writes stations[begin, end) in one transaction for performance, so
        // callers should pass a few thousand at a time. Rows whose data is
        // unchanged are left alone.
        void BulkUpsertUlsStations(const std::vector<Station>& stations, std::size_t begin,
                                   std::size_t end, std::int64_t updated_at);
        // Deletes every ULS row whose callsign isn't in `current` -- run after
        // a full import, so a license that has expired or been cancelled
        // since the last one stops turning up in autocomplete. Returns how
        // many were deleted.
        int DeleteUlsStationsNotIn(const std::vector<Station>& current);
        // Autocomplete's FCC tier: ULS stations whose callsign contains
        // `substring` (case-insensitive) and who live near the operator,
        // nearest first (then by callsign), at most `limit` of them. "Near"
        // is a ZIP in `nearby_zips` (see NearbyZips in geo_utils.hpp, which
        // also supplies each one's distance), or -- listed after those, with
        // an unknown distance -- a ZIP with no centroid on file (e.g. a PO
        // Box ZIP) that starts with one of `zip3_prefixes`.
        std::vector<NearbyUlsStation> SearchNearbyUlsStations(
            const std::string& substring, const std::vector<NearbyZip>& nearby_zips,
            const std::vector<std::string>& zip3_prefixes, int limit);
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

        // ZIP-to-county data (see ZipCounty/ZipPlaceCounty in models.hpp and
        // FetchAndLoadZipCounties in uls_import.cpp). Replaced wholesale in
        // one transaction -- it's derived from Census files, never edited.
        // The getters are meant to be called once per process and cached
        // (see AppState::zip_county_by_zip), not queried live.
        void ReplaceZipCountyData(const std::vector<ZipCounty>& zip_counties,
                                  const std::vector<ZipPlaceCounty>& zip_place_counties);
        std::vector<ZipCounty> GetAllZipCounties();
        std::vector<ZipPlaceCounty> GetAllZipPlaceCounties();

        bool HasAnyUlsStations();

        // Login identities for the built-in SSH server (see ssh_server.hpp)
        // -- global/shared data, like Net, even though each user's
        // AppSettings (see settings.hpp) is not. `username` is the primary
        // key; a second CreateUser for the same username overwrites its
        // public key rather than erroring, matching this app's general
        // upsert-by-natural-key style elsewhere.
        void CreateUser(const User& user);
        std::optional<User> GetUserByUsername(const std::string& username);
        std::vector<User> ListUsers();
        void DeleteUser(const std::string& username);
        void UpdateUserLastLogin(const std::string& username, std::int64_t last_login_at);

    private:
        void CreateSchema();

        sqlite3* db_ = nullptr;
    };

}  // namespace ql
