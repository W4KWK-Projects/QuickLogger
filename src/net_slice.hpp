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
    // different source file. Returns the new net's id.
    std::int64_t ApplyNetSlice(Database* db, const NetSlice& slice);

}  // namespace ql
