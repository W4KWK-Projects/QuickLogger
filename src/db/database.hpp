#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../models.hpp"
#include "sqlite_statement.hpp"

struct sqlite3;

namespace ql
{

    // Owns the sqlite3 connection for QuickLogger's local database and
    // creates the schema (if it doesn't already exist) on construction.
    // The FCC's two licensee tables: amateur (uls_stations, l_amat.zip) and
    // GMRS (gmrs_stations, l_gmrs.zip), which have the same columns.
    enum class LicenseTable
    {
        kAmateur,
        kGmrs,
    };

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

        // Groups the reads made while it's in scope into one read
        // transaction: one snapshot and one lock for all of them, instead of
        // each query taking and releasing its own. For code that makes
        // several reads in a row (reloading a list, autocomplete) and no
        // writes. Does nothing if a transaction is already open.
        class ReadTransaction
        {
        public:
            explicit ReadTransaction(Database* db);
            ~ReadTransaction();

            ReadTransaction(const ReadTransaction&) = delete;
            ReadTransaction& operator=(const ReadTransaction&) = delete;

        private:
            Database* db_;
            bool began_ = false;
        };

        // Makes the writes while it's in scope one transaction: all of them,
        // or none if Commit() isn't reached (an exception unwinds it, and it
        // rolls back). One commit to disk instead of one per write. Does
        // nothing if a transaction is already open, so one can be used
        // inside another's; the outermost decides.
        class WriteTransaction
        {
        public:
            explicit WriteTransaction(Database* db);
            ~WriteTransaction();

            WriteTransaction(const WriteTransaction&) = delete;
            WriteTransaction& operator=(const WriteTransaction&) = delete;

            void Commit();

        private:
            Database* db_;
            bool began_ = false;
            bool finished_ = false;
        };

        // Stations. Insert-or-update by callsign, since a Station represents
        // everything currently known about that callsign, not a check-in log.
        void UpsertStation(const Station& station);
        // Sets every record of `callsign` (a GMRS call sign may have several
        // names) to the 6-character `grid` if its grid is blank or is the
        // 4-character square `grid` lies in, or is `zip_grid`, the 4-character
        // grid its ZIP or postal code gave it (when not blank): a grid is only
        // ever extended, or replaced where it was only a centroid's (see
        // ShouldTakePreciseGrid).
        void UpdateStationGrid(const std::string& callsign, const std::string& grid, const std::string& zip_grid = "");
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
        // A member ID follows the net, not the station: it is kept with the
        // net's saved station (net_saved_stations), and Station::member_id
        // is filled only by the reads that name a net (the saved stations,
        // that net's autocomplete, a session's stations). Everywhere else it
        // is blank. SetNetMemberId sets it exactly, blank included, on every
        // saved entry of `callsign` on that net; GetNetMemberId reads it.
        void SetNetMemberId(std::int64_t net_id, const std::string& callsign, const std::string& member_id);
        std::string GetNetMemberId(std::int64_t net_id, const std::string& callsign);
        std::optional<Station> FindStationByCallsign(const std::string& callsign);
        // The stations here with any of `callsigns` (upper case), sorted by
        // callsign: a few queries for the lot rather than one per callsign.
        std::vector<Station> FindStationsByCallsigns(const std::vector<std::string>& callsigns) const;
        // Every station checked into net instance `instance_id`, sorted by
        // callsign: one query where looking each check-in's station up in
        // turn would be one per check-in.
        std::vector<Station> GetStationsInNetInstance(std::int64_t instance_id);
        // Matches any callsign containing `substring` (case-insensitive),
        // e.g. "4FA" matches "AA4FA". This is autocomplete's second tier: any
        // station known elsewhere in the system (i.e. from another net),
        // ranked below stations known to this specific net
        // (SearchNetStationsByCallsignSubstring). A ULS tier, if added, would
        // be a separate query appended after this one, not merged into it.
        // At most `limit` of them (-1: all).
        std::vector<Station> SearchStationsByCallsignSubstring(const std::string& substring, int limit = -1);
        // Matches any callsign containing `substring` (case-insensitive) among
        // stations that have either checked into a past instance of `net_id`,
        // or been explicitly saved to it (see SaveNetStation) -- e.g. imported
        // from another logging program's history. This is autocomplete's first
        // tier: prefer callers already known to this specific net before
        // broadening to SearchStationsByCallsignSubstring (other nets). On a
        // GMRS net each name a call sign checked in or was saved under is a
        // match of its own, under that name. At most `limit` of them (-1: all).
        std::vector<Station> SearchNetStationsByCallsignSubstring(std::int64_t net_id, const std::string& substring,
                                                                  int limit = -1);
        // Associates `station.callsign` with `net_id` as "known to this net"
        // for autocomplete, without a real check-in -- for saving a station
        // from an external source (e.g. another logging program's history).
        // Also merge-upserts the Station itself (see RecordManualCheckInStation).
        // `default_remarks` is carried on the net/callsign association (not
        // the Station), and gets copied into the New Station modal's Remarks
        // field when this station is picked via autocomplete for this net.
        //
        // Every saved-station method here also takes the entry's own
        // `name`: on a GMRS net, where a call sign may be saved more than
        // once (one license covers a family), the person's name, which tells
        // the entries apart; on an Amateur Radio net blank (the default), the
        // station's own name standing. See SavedEntryName in app_state.hpp.
        void SaveNetStation(std::int64_t net_id, const Station& station, const std::string& default_remarks,
                            std::int64_t updated_at, const std::string& name = "");
        // Directly sets an already-saved station's fields and default remarks
        // to exactly what's given in `station`/`default_remarks`, including
        // blanking any of them out. Unlike SaveNetStation, this never preserves
        // a previous value -- it's for explicitly editing a station the
        // operator already saved to this net (e.g. adding details they didn't
        // have when they first saved just the callsign), not adding a new one.
        // `old_name` names the entry; it's renamed to `new_name`.
        void UpdateSavedNetStation(std::int64_t net_id, const Station& station, const std::string& default_remarks,
                                   std::int64_t updated_at, const std::string& old_name = "",
                                   const std::string& new_name = "");
        // Removes a station's saved association with a net. Never touches
        // check-in history. Like every delete here, it then drops any
        // station record nothing refers to any more (see DeleteUnusedStations).
        void RemoveSavedNetStation(std::int64_t net_id, const std::string& callsign, const std::string& name = "");
        // After a GMRS check-in's name changed from `old_name` to `new_name`:
        // `callsign`'s entry on net `net_id` under the old name is renamed,
        // or, while another check-in to the net still goes by the old name,
        // copied (default remarks too) under the new one. Nothing if
        // `callsign` wasn't saved under the old name.
        void RenameNetSavedStationEntry(std::int64_t net_id, const std::string& callsign, const std::string& old_name,
                                        const std::string& new_name);
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
        // An entry with a name of its own has that as its Station::name.
        // By call sign, then name.
        std::vector<Station> GetSavedStationsForNet(std::int64_t net_id);
        // The same, each with its entry name and default remarks (the
        // station's own name left as on file), in one query.
        std::vector<SavedNetStation> GetSavedNetEntries(std::int64_t net_id);
        // The default remarks saved for `callsign` on `net_id`, or an empty
        // string if there's no saved row or no default was set. Used to
        // prefill the New Station modal's Remarks field when autocomplete
        // picks a station that was saved (rather than genuinely checked in)
        // for this net.
        std::string GetSavedNetStationRemarks(std::int64_t net_id, const std::string& callsign,
                                              const std::string& name = "");
        // Adds `station` to the stations table, or for one already there,
        // fills in only the fields it has blank: what's known here is never
        // replaced. For merging a net from a file (see ApplyNetMerge).
        void FillStationBlanks(const Station& station, std::int64_t updated_at);
        // Saves `callsign` (already in stations) to `net_id` with
        // `default_remarks`, unless it's saved there already, in which case
        // nothing changes. True if it was added.
        bool AddNetSavedStationIfMissing(std::int64_t net_id, const std::string& callsign,
                                         const std::string& default_remarks, const std::string& name = "",
                                         const std::string& member_id = "");

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
        // Every session of every ad hoc net (Net::is_ad_hoc), newest first.
        std::vector<NetInstance> GetAdHocNetInstances();
        // The session of an ad hoc net named exactly `net_name` on
        // `instance_date` that started at `started_at`, if there is one: a
        // .qlsession already imported as an ad hoc net (see
        // ApplyAdHocSessionSlice).
        // True if net `net_id` has a session of that date and start time: the
        // same session, imported or pushed again.
        bool HasNetInstance(std::int64_t net_id, const std::string& instance_date, std::int64_t started_at);
        std::optional<NetInstance> FindAdHocSession(const std::string& net_name, const std::string& instance_date,
                                                    std::int64_t started_at);
        std::optional<NetInstance> GetNetInstanceById(std::int64_t instance_id);
        // Records that the session was pushed upstream at `pushed_at`
        // (see NetInstance::pushed_at).
        void SetNetInstancePushedAt(std::int64_t instance_id, std::int64_t pushed_at);
        // Closes an open instance as of `closed_at`. Returns false, changing
        // nothing, if it's already closed (someone else got there first, and
        // their end time stands) or no longer exists.
        // Closing it also numbers its check-ins 1, 2, 3... in order
        // (RenumberCheckIns), closing any gaps left by deletes.
        bool CloseNetInstance(std::int64_t instance_id, std::int64_t closed_at);
        // Numbers the instance's check-ins 1, 2, 3... in their current
        // order, closing the gaps deleted check-ins leave. Only for a closed
        // instance: while one is open, a check-in keeps its number, so
        // someone sharing the session who's about to pick "#5" still gets
        // the station they see as #5.
        void RenumberCheckIns(std::int64_t instance_id);
        // The nets that have at least one instance still open -- i.e. a net
        // someone is logging right now, or one whose session ended without
        // being closed (see ResumeOpenNet in app_state.hpp).
        std::vector<std::int64_t> GetNetIdsWithOpenInstances();
        // The recurring (not ad hoc) net whose newest session `callsign`
        // started (NetInstance::created_by), or nullopt if they've started
        // none.
        std::optional<std::int64_t> GetNetLastStartedBy(const std::string& callsign);
        // Sets one of the instance's three role-callsign columns (kRoleNetControl/
        // kRoleAlternateNetControl/kRoleLogger) to `callsign` -- `callsign` is
        // "" to clear it. Used by ApplyCheckInRoleDesignation to keep these
        // columns in sync with CheckIn::designated_role without a
        // general-purpose "update the whole NetInstance" method. A no-op if
        // `role` isn't one of the three (in particular, kRoleNone).
        void SetNetInstanceRoleCallsign(std::int64_t instance_id, int role, const std::string& callsign);
        // Replaces a session's notes (NetInstance::notes).
        void SetNetInstanceNotes(std::int64_t instance_id, const std::string& notes);
        // Permanently removes one net instance (e.g. logged by mistake, or a
        // test/practice run someone wants gone from history) and all of its
        // check-ins -- check_ins.net_instance_id references net_instances(id)
        // with foreign keys enforced, so the check-ins must go first. Wrapped
        // in one transaction so a failure can't leave check-ins orphaned from
        // a half-deleted instance. Then the stations nothing refers to any
        // more go too (DeleteUnusedStations), unless `remove_unused_stations`
        // is false: for deleting several in a row, then removing them once.
        void DeleteNetInstance(std::int64_t instance_id, bool remove_unused_stations = true);

        // Check-ins (one station's check-in during one NetInstance).
        std::int64_t AddCheckIn(const CheckIn& check_in);
        // The same, into session `net_instance_id` whatever
        // check_in.net_instance_id says: for copying a check-in from a file
        // into a session here without copying the CheckIn first.
        std::int64_t AddCheckIn(const CheckIn& check_in, std::int64_t net_instance_id);
        // Adds `check_in` as the next one in its session: one past the
        // highest sequence number so far (ignoring check_in.sequence_number).
        // The number is worked out inside the INSERT itself, so two people
        // logging the same session at the same moment can't both get it.
        std::int64_t AddCheckInAtNextSequence(const CheckIn& check_in);
        std::vector<CheckIn> GetCheckInsForNetInstance(std::int64_t net_instance_id);
        // Every check-in to every session of `net_id`, in one query, by
        // session then number (for comparing a whole net; see PlanNetMerge).
        std::vector<CheckIn> GetCheckInsForNet(std::int64_t net_id);

        // Every check-in `callsign` made to net `net_id`, newest session
        // first, each with its session and the net's name.
        std::vector<StationCheckInRecord> GetStationCheckInsForNet(std::int64_t net_id, const std::string& callsign);
        // Check-ins to any net by a callsign containing `substring`
        // (case-insensitive), newest first, at most `limit`.
        std::vector<StationCheckInRecord> FindCheckInsByCallsign(const std::string& substring, int limit);
        // The callsigns that have checked in to net `net_id` most often, with
        // how many times and their latest session's date.
        std::vector<CallsignTally> GetTopCallsignsForNet(std::int64_t net_id, int limit);
        // Each station saved to net `net_id` (each entry, on a GMRS net, with
        // its own name and check-ins), with how many times it has
        // checked in to that net and the date of the latest (empty if never),
        // least recent first.
        std::vector<CallsignTally> GetSavedStationActivity(std::int64_t net_id);
        // How much `callsign` has checked in anywhere, and the nets it's
        // saved to.
        StationActivity GetStationActivity(const std::string& callsign);
        // How many check-ins a session has and the newest one's id -- enough
        // to tell cheaply whether someone else has logged or deleted one.
        void GetCheckInSummary(std::int64_t net_instance_id, std::int64_t* count, std::int64_t* newest_id);
        // How many check-ins each session of net `net_id` has, by session id,
        // in one query (a session with none isn't listed). With `net_id` 0,
        // every ad hoc net's sessions.
        std::unordered_map<std::int64_t, std::int64_t> GetCheckInCounts(std::int64_t net_id);
        // Every call sign that has checked in to net `net_id` in a session
        // other than `instance_id`, once each.
        std::vector<std::string> GetCallsignsInOtherSessions(std::int64_t net_id, std::int64_t instance_id);
        // The check-ins of a few sessions, in one query, by session then
        // number.
        std::vector<CheckIn> GetCheckInsForNetInstances(const std::vector<std::int64_t>& instance_ids) const;
        void UpdateCheckIn(const CheckIn& check_in);
        // Removes one check-in entry entirely (e.g. logged in error). Does not
        // touch the Station record. In an open session the others keep their
        // numbers, leaving a gap until it's closed; in a closed one they're
        // renumbered straight away (RenumberCheckIns).
        void DeleteCheckIn(std::int64_t check_in_id);
        // Clears designated_role back to kRoleNone on every check-in in
        // `net_instance_id` currently holding `role`, except `except_check_in_id`
        // -- so handing a role to one check-in takes it away from whoever had
        // it, since only one check-in per instance can hold a given role.
        void ClearCheckInRoleForInstance(std::int64_t net_instance_id, int role, std::int64_t except_check_in_id);

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
        bool TryClaimImportRun(const std::string& source, std::int64_t now, std::int64_t stale_after_seconds);
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
        //
        // Every FCC licensee method here takes a LicenseTable: the amateur
        // table by default, or the GMRS one, which is alike in every column.
        void BulkUpsertUlsStations(const std::vector<Station>& stations, std::size_t begin, std::size_t end,
                                   std::int64_t updated_at, LicenseTable table = LicenseTable::kAmateur);
        // Deletes every ULS row whose callsign isn't in `current` -- run after
        // a full import, so a license that has expired or been cancelled
        // since the last one stops turning up in autocomplete. Returns how
        // many were deleted.
        int DeleteUlsStationsNotIn(const std::vector<Station>& current);
        int DeleteUlsStationsNotIn(const std::vector<std::string_view>& current_callsigns,
                                   LicenseTable table = LicenseTable::kAmateur);
        // Autocomplete's FCC tier: ULS stations whose callsign contains
        // `substring` (case-insensitive) and who live near the operator,
        // nearest first (then by callsign), at most `limit` of them (-1 for
        // no limit; an empty `substring` matches every station). "Near"
        // is a ZIP in `nearby_zips` (see NearbyZips in geo_utils.hpp, which
        // also supplies each one's distance), or -- listed after those, with
        // an unknown distance -- a ZIP with no centroid on file (e.g. a PO
        // Box ZIP) that starts with one of `zip3_prefixes`.
        std::vector<NearbyUlsStation> SearchNearbyUlsStations(const std::string& substring,
                                                              const std::vector<NearbyZip>& nearby_zips,
                                                              const std::vector<std::string>& zip3_prefixes, int limit,
                                                              LicenseTable table = LicenseTable::kAmateur) const;
        // Every station SearchNearbyUlsStations("") would return, in the same
        // order, as just its callsign and distance: what autocomplete keeps
        // in memory (see AppState::nearby_uls_callsigns). Read from the
        // (zip, callsign) index alone, never the table's rows.
        std::vector<NearbyUlsCallsign> ListNearbyUlsCallsigns(const std::vector<NearbyZip>& nearby_zips,
                                                              const std::vector<std::string>& zip3_prefixes,
                                                              LicenseTable table = LicenseTable::kAmateur);
        // The same for Canada's licensees (ISED), found by the first three
        // characters of their postal code: those in the nearby entries of
        // `nearby_zips` that are FSAs (the US ZIPs among them are skipped).
        // Licensees with no postal code on file are left out.
        std::vector<NearbyUlsCallsign> ListNearbyIsedCallsigns(const std::vector<NearbyZip>& nearby_zips);
        // Exact-callsign lookup against the ULS table, for resolving a
        // specific operator's info (see LogOperatorCheckIn) rather than
        // searching/ranking candidates.
        std::optional<Station> FindUlsStationByCallsign(const std::string& callsign,
                                                        LicenseTable table = LicenseTable::kAmateur);
        // The same for a few call signs at once (a screenful of autocomplete
        // matches), sorted by callsign; those not licensed are left out.
        std::vector<Station> FindUlsStationsByCallsigns(const std::vector<std::string>& callsigns,
                                                        LicenseTable table = LicenseTable::kAmateur) const;

        // FindUlsStationsByCallsigns for the ISED table.
        std::vector<Station> FindIsedStationsByCallsigns(const std::vector<std::string>& callsigns) const;

        // Canada's amateur call sign database (ISED -- see uls_import.hpp),
        // in its own table like the FCC's. Replaced wholesale on each load,
        // in one transaction: it's small (about 90,000 call signs), and a
        // call sign no longer listed simply isn't there afterwards.
        void ReplaceIsedStations(const std::vector<Station>& stations, std::int64_t updated_at);
        std::optional<Station> FindIsedStationByCallsign(const std::string& callsign);
        // Autocomplete's Canadian tier: ISED call signs starting with
        // `prefix` (case-insensitive), in order, at most `limit` of them.
        std::vector<Station> SearchIsedStationsByCallsignPrefix(const std::string& prefix, int limit);
        // The same, for a net with Canadian partial matching on: ISED call
        // signs containing `substring` anywhere, in order.
        std::vector<Station> SearchIsedStationsByCallsignSubstring(const std::string& substring, int limit);
        // A call sign's license details from whichever database has it:
        // the FCC's, else ISED's.
        std::optional<Station> FindLicensedStationByCallsign(const std::string& callsign);

        // Approximate lat/lon centroids for US ZIP codes (from the Census
        // Bureau's ZCTA gazetteer), used to estimate distance for the
        // saved-station form's ULS proximity autocomplete. Loaded once by the
        // same background worker that imports ULS data (see uls_import.hpp);
        // BulkUpsertZipCentroids is a plain overwrite-on-conflict batch
        // upsert, same performance rationale as BulkUpsertUlsStations.
        // Sessions look up what they need rather than holding the table.
        void BulkUpsertZipCentroids(const std::vector<ZipCentroid>& batch);
        std::vector<ZipCentroid> GetAllZipCentroids();
        std::optional<ZipCentroid> FindZipCentroid(const std::string& zip);
        // Every centroid inside a latitude/longitude box, from an index on
        // (lat, lon): the candidates for NearbyZips, a few hundred rows
        // rather than the whole table.
        std::vector<ZipCentroid> GetZipCentroidsInBox(double min_lat, double max_lat, double min_lon, double max_lon);
        bool HasAnyZipCentroids();
        // Gives every station in `stations` with a blank Grid Square and a
        // US ZIP that has a centroid the 4-character grid of that centroid
        // (see MaidenheadGrid4). A grid already there is never touched. Run
        // once each time the program starts. Returns how many were filled.
        int FillBlankGridSquaresFromZip();

        // ZIP-to-county data (see ZipCounty/ZipPlaceCounty in models.hpp and
        // FetchAndLoadZipCounties in uls_import.cpp). Replaced wholesale in
        // one transaction -- it's derived from Census files, never edited.
        // FindZipCounty and FindZipPlaceCounty (for a `place` as
        // NormalizePlaceName writes it) look up one, "" if there's none.
        void ReplaceZipCountyData(const std::vector<ZipCounty>& zip_counties,
                                  const std::vector<ZipPlaceCounty>& zip_place_counties);
        std::vector<ZipCounty> GetAllZipCounties();
        std::vector<ZipPlaceCounty> GetAllZipPlaceCounties();
        std::string FindZipCounty(const std::string& zip);
        std::string FindZipPlaceCounty(const std::string& zip, const std::string& place);

        bool HasAnyUlsStations(LicenseTable table = LicenseTable::kAmateur);

        // A number that changes whenever another connection (another
        // session, the data updater) commits a change to the database, and
        // never for this connection's own (SQLite's data_version): whether
        // something read earlier may since have changed under it.
        std::int64_t DataVersion();

        // Login keys for the built-in SSH server (see ssh_server.hpp), one
        // row per key -- global/shared data, like Net, even though each
        // user's AppSettings (see settings.hpp) is not. A username may have
        // any number of keys. CreateUser adds `user`'s key to its username,
        // or, if that username already has the same key (SamePublicKey --
        // whatever its comment), just updates the stored line's comment;
        // returns true if it added a key. Every function here that takes a
        // username matches it whatever its case (a callsign typed as w4kwk
        // is W4KWK).
        // A new username gets `user.view_only`; another key for an
        // existing one takes that username's access instead.
        bool CreateUser(const User& user);
        // Makes every key of `username` view-only, or full access.
        void SetUserViewOnly(const std::string& username, bool view_only);
        // Sets `username`'s call signs (see User::amateur_callsign) on
        // every key; a new username's come from CreateUser's `user`.
        void SetUserCallsigns(const std::string& username, const std::string& amateur_callsign,
                              const std::string& gmrs_callsign);
        // True if `username` is view-only (false for an unknown username).
        bool IsUserViewOnly(const std::string& username);
        // Every key `username` may log in with, oldest first.
        std::vector<User> GetUserKeys(const std::string& username);
        // Every key, by username (ignoring case), then oldest first.
        std::vector<User> ListUsers();
        void DeleteUserKey(std::int64_t id);
        // Every key of `username`.
        void DeleteUser(const std::string& username);
        // Files every key of `old_username` under `new_username` instead.
        // False, changing nothing, if `new_username` is already someone
        // else's (a change of case alone is fine).
        bool RenameUser(const std::string& old_username, const std::string& new_username);
        void UpdateUserLastLogin(std::int64_t id, std::int64_t last_login_at);
        // Replaces key `id`'s line (only its comment changes: see
        // WithPublicKeyComment).
        void UpdateUserKeyLine(std::int64_t id, const std::string& public_key);
        // Sets how key `id` fetches files (User::transfer_method).
        void UpdateUserKeyTransfer(std::int64_t id, int transfer_method);
        // Turns key `id`, or every key of `username`, off or on (see
        // User::disabled).
        void SetUserKeyDisabled(std::int64_t id, bool disabled);
        void SetUserKeysDisabled(const std::string& username, bool disabled);
        // The username that has this key (same type and data, whatever the
        // comment), or blank if no one does.
        std::string UserOfKey(const std::string& public_key);
        // Server-wide yes/no options, off until set. kOptionSelfServiceKeys
        // lets users add and remove their own keys in My Keys.
        bool ServerOptionOn(const std::string& name);
        void SetServerOption(const std::string& name, bool on);
        // The key log: one line per change to who can log in, newest first
        // when read, trimmed to the last 500.
        void LogKeyEvent(const KeyEvent& event);
        std::vector<KeyEvent> RecentKeyEvents(int limit);

    private:
        // Rolls back the open transaction, never throwing (see the
        // transactions' destructors).
        static void Rollback(Database* db);
        void CreateSchema();
        // Rebuilds a users table from before a username could have more
        // than one key (username was its primary key) in the current shape,
        // keeping every row. Part of CreateSchema's one-time upgrade.
        void UpgradeUsersTable();
        // Rebuilds net_saved_stations from before a GMRS net could save a
        // call sign more than once (its key was net and call sign) with a
        // `name` in its key, keeping every row.
        void UpgradeSavedStationsTable();
        // Replaces each net's default_location with the ZIP code in it
        // (ExtractZipCode), or blank if it has none. Part of CreateSchema's
        // one-time upgrade.
        void NormalizeNetZips();
        // Replaces each net's mode with NormalizeMode's reading of it. Part
        // of CreateSchema's one-time upgrade.
        void NormalizeNetModes();
        // Moves anything in net_seed_stations, a table from before the
        // repository's first commit, into net_saved_stations and drops it.
        // Part of CreateSchema's one-time upgrade.
        void DropOldSeedStations();
        // Gives each net's saved stations the member ID their station had,
        // once (see SetNetMemberId). Part of CreateSchema's one-time upgrade.
        void MoveMemberIdsToNets();
        // Moves each net's frequency that isn't an amateur frequency into
        // its comments (see MoveBadFrequencyToComments). Part of
        // CreateSchema's one-time upgrade.
        void NormalizeNetFrequencies();

        sqlite3* db_ = nullptr;
        // This connection's prepared statements, reused call after call.
        StatementCache statements_;
    };

}  // namespace ql
