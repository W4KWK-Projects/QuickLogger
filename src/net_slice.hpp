#pragma once

#include <optional>
#include <string>
#include <vector>

#include "db/database.hpp"
#include "models.hpp"

namespace ql
{

    // A station explicitly saved to the net being exported, paired with its
    // per-net default remarks -- mirrors one net_saved_stations row (see
    // Database::GetSavedStationsForNet/GetSavedNetStationRemarks).
    struct NetSliceSavedStation
    {
        Station station;
        std::string default_remarks;
    };

    // Everything needed to reconstitute one recurring net -- its own
    // definition, every station that's either saved to it or has ever
    // checked into one of its instances, its instances, and their
    // check-ins -- on another QuickLogger installation. This is the "slice"
    // a .qlnet file holds; see GatherNetSlice/WriteNetSliceFile/
    // ReadNetSliceFile/ApplyNetSlice below for how it's produced and
    // consumed.
    struct NetSlice
    {
        Net net;
        // Stations that were explicitly saved to this net (would appear in
        // the edit-net page's saved-station list).
        std::vector<NetSliceSavedStation> saved_stations;
        // Stations that checked into this net at some point but were never
        // explicitly saved -- only possible for check-in history logged
        // before stations started auto-saving on check-in (see
        // LogStationCheckIn/LogOperatorCheckIn in app_state.cpp). Kept
        // separate from saved_stations because these shouldn't become
        // net_saved_stations rows on import, just stations rows so the
        // check-in log below has something to look their name/county up
        // against.
        std::vector<Station> other_stations;
        // `id` on each instance here is whatever it was in the *source*
        // database -- meaningless on its own, but used as the grouping key
        // that ties check_ins below to the right instance (both
        // GatherNetSlice and ReadNetSliceFile preserve this pairing;
        // ApplyNetSlice remaps it to whatever new id each instance actually
        // gets when inserted).
        std::vector<NetInstance> instances;
        // `net_instance_id` on each check-in here is the *original* instance
        // id from `instances` above, not yet remapped.
        std::vector<CheckIn> check_ins;
    };

    // Reads everything about `net_id` out of `db` into a NetSlice, ready to
    // hand to WriteNetSliceFile. This is the export side's data-gathering
    // step; it doesn't touch a file at all.
    NetSlice GatherNetSlice(Database* db, std::int64_t net_id);

    // Writes `slice` to a brand new standalone SQLite database file at
    // `dest_path` (parent directory created if needed, existing file at
    // that path overwritten) -- internally just opens a fresh Database
    // there and calls ApplyNetSlice against it, so the file has the exact
    // same schema as quicklogger.db itself, just with only this one net's
    // data populated. Returns true on success; on failure, `error` is set
    // to a short message.
    bool WriteNetSliceFile(const std::string& dest_path, const NetSlice& slice, std::string* error);

    // Reads a NetSlice back out of a file written by WriteNetSliceFile.
    // Refuses (returning nullopt, with `error` set) if the file has no net
    // in it, or more than one -- a real .qlnet export always has exactly
    // one, so more than one likely means this is a plain quicklogger.db
    // pointed at by mistake, not an actual slice export.
    std::optional<NetSlice> ReadNetSliceFile(const std::string& source_path, std::string* error);

    // Inserts `slice` into `db` as a brand new net -- never reuses any id
    // from the slice (a fresh Net row, fresh net_instances rows, etc.), so
    // this is safe to call against a database that already has its own
    // unrelated nets/instances with potentially colliding ids from a
    // different source file. The net keeps the slice's created_at and gets
    // `imported_at` as its import time: an actual import passes now, while
    // writing a .qlnet file passes the net's own imported_at through
    // unchanged. Returns the new net's id.
    std::int64_t ApplyNetSlice(Database* db, const NetSlice& slice, std::int64_t imported_at);

    // ---- One session (.qlsession) ------------------------------------------
    //
    // A .qlsession file is a NetSlice holding exactly one session: its net's
    // definition, the session, its check-ins, and the details of every
    // station in them (as other_stations). Written by F7 Export next to the
    // session's text log, so a session logged somewhere else (on a laptop
    // while the server was out of reach) can be added to a net here with
    // History's F6 Import. Same file format as .qlnet (WriteNetSliceFile).

    // Reads session `instance_id` out of `db`, ready for WriteNetSliceFile.
    NetSlice GatherSessionSlice(Database* db, std::int64_t instance_id);

    // Reads a .qlsession file; like ReadNetSliceFile, but refuses a file
    // that doesn't hold exactly one session.
    std::optional<NetSlice> ReadSessionSliceFile(const std::string& source_path,
                                                 std::string* error);

    // Adds the one session in `slice` to net `net_id` as a new session, with
    // its check-ins. Each of its stations is saved to the net as logging
    // one does, its details merged in (see Database::SaveNetStation); a
    // station already saved to the net keeps its default remarks, and a new
    // one gets its remarks from this session. Refuses (returning 0, with
    // `error` set) if the net already has a session on the same date with
    // the same start time -- most likely this one, imported before.
    // Otherwise returns the new session's id.
    std::int64_t ApplySessionSlice(Database* db, const NetSlice& slice, std::int64_t net_id,
                                   std::string* error);

    // ---- Merging a .qlnet into a net that's already here -------------------
    //
    // For a net exported from here, logged somewhere else for a while (on a
    // laptop during an outage, say) and sent back: what the file has that
    // the net here doesn't is added, and nothing here is changed except
    // where the operator chooses to replace a session.

    // How one of the file's sessions compares with the net here.
    enum class MergeSessionKind
    {
        kNew,          // Not here: added.
        kAlreadyHere,  // Here with the same check-ins and notes: left alone.
        kDiffers,      // Here, but its check-ins or notes differ: kept,
                       // unless MergeSession::replace.
    };

    struct MergeSession
    {
        std::size_t file_index = 0;  // Into NetSlice::instances.
        MergeSessionKind kind = MergeSessionKind::kNew;
        // The session here it matched (kAlreadyHere, kDiffers), and whether
        // it's still open here. An open session here is never replaced:
        // it's kAlreadyHere whatever its check-ins.
        std::int64_t local_id = 0;
        bool local_open = false;
        int file_check_ins = 0;
        int local_check_ins = 0;
        // Still open in the file; it's added closed (and renumbered).
        bool file_open = false;
        // kDiffers: what differs (either or both).
        bool check_ins_differ = false;
        bool notes_differ = false;
        // When the check-ins differ: the callsigns whose check-in differs
        // (remarks, signal report...), those only in the file, and those
        // only here, each sorted.
        std::vector<std::string> callsigns_changed;
        std::vector<std::string> callsigns_only_in_file;
        std::vector<std::string> callsigns_only_here;
        // kDiffers only: replace the session here with the file's.
        bool replace = false;
    };

    // One of a station's details, filled in both here and in the file, but
    // differently (case aside): e.g. a member ID of SP-41 here, SP-42 there.
    struct StationDetailDifference
    {
        const char* field = "";  // As shown: "member ID", "city"...
        int field_index = 0;     // Which detail (see kDetailFields in net_slice.cpp).
        std::string here;
        std::string file;
    };

    // A station known both here and in the file whose details differ.
    // Kept as it is here unless `replace`, which takes the file's values
    // for the differing details. (Details blank on one side never count:
    // a blank here is just filled in.)
    struct MergeStationConflict
    {
        const Station* file_station = nullptr;  // Into the slice.
        std::vector<StationDetailDifference> differences;
        bool replace = false;
    };

    struct NetMergePlan
    {
        std::int64_t target_net_id = 0;
        std::vector<MergeSession> sessions;  // One per session in the file, in its order.
        int new_saved_stations = 0;          // The file's saved stations not saved here yet.
        int known_saved_stations = 0;        // Those already saved here.
        // Stations in the file and here whose details differ, by callsign.
        std::vector<MergeStationConflict> station_conflicts;
    };

    // The same session, logged twice: their times overlap. Each session's
    // time is from its start to when it was closed; for one still open (or
    // closed without a recorded time), starts within 30 minutes of each
    // other. Without a start time on either, the same date and the same
    // check-ins (see SameCheckIns).
    bool SameSession(const NetInstance& a, const std::vector<CheckIn>& a_check_ins,
                     const NetInstance& b, const std::vector<CheckIn>& b_check_ins);

    // The same callsigns, signal reports, remarks, comments and roles, in
    // any order (numbers and times aside).
    bool SameCheckIns(const std::vector<CheckIn>& a, const std::vector<CheckIn>& b);

    // Compares `slice` with net `target_net_id` here, changing nothing.
    // Each session here is matched with at most one of the file's.
    NetMergePlan PlanNetMerge(Database* db, const NetSlice& slice, std::int64_t target_net_id);

    struct NetMergeResult
    {
        int sessions_added = 0;
        int sessions_replaced = 0;
        int saved_stations_added = 0;
        int stations_replaced = 0;
    };

    // Carries out `plan` (from PlanNetMerge on the same slice), all of it in
    // one transaction: the file's saved stations not saved here are added
    // with their remarks (those already here keep theirs); every station in
    // the file fills in only details missing here; kNew sessions are added,
    // and kDiffers ones marked `replace` replace the session here; the
    // station_conflicts marked `replace` take the file's details. A session
    // open in the file is added closed, at its last check-in, and
    // renumbered. The net's own settings aren't touched.
    NetMergeResult ApplyNetMerge(Database* db, const NetSlice& slice, const NetMergePlan& plan);

}  // namespace ql
