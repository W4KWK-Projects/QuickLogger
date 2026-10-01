#include "net_slice.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "date_utils.hpp"
#include "file_export.hpp"
#include "frequency_rules.hpp"
#include "mode_rules.hpp"
#include "text_utils.hpp"

namespace ql
{

    NetSlice GatherNetSlice(Database* db, std::int64_t net_id)
    {
        // All of its reads under one snapshot and one lock.
        Database::ReadTransaction reads(db);
        NetSlice slice;
        std::optional<Net> net = db->GetNetById(net_id);
        if (net.has_value())
        {
            slice.net = std::move(*net);
        }

        std::unordered_set<std::string> known_callsigns;
        std::vector<Station> saved = db->GetSavedStationsForNet(net_id);
        slice.saved_stations.reserve(saved.size());
        for (Station& station : saved)
        {
            NetSliceSavedStation entry;
            entry.default_remarks = db->GetSavedNetStationRemarks(net_id, station.callsign);
            known_callsigns.insert(ToUpperAscii(station.callsign));
            entry.station = std::move(station);
            slice.saved_stations.push_back(std::move(entry));
        }

        slice.instances = db->GetNetInstancesForNet(net_id);
        for (const NetInstance& instance : slice.instances)
        {
            std::vector<CheckIn> check_ins = db->GetCheckInsForNetInstance(instance.id);
            for (CheckIn& check_in : check_ins)
            {
                // Only reachable for check-in history logged before stations
                // started auto-saving on check-in -- see NetSlice::other_stations.
                if (known_callsigns.insert(ToUpperAscii(check_in.callsign)).second)
                {
                    std::optional<Station> station = db->FindStationByCallsign(check_in.callsign);
                    if (station.has_value())
                    {
                        slice.other_stations.push_back(std::move(*station));
                    }
                }
                slice.check_ins.push_back(std::move(check_in));
            }
        }

        return slice;
    }

    std::int64_t ApplyNetSlice(Database* db, const NetSlice& slice, std::int64_t imported_at)
    {
        // One transaction: the whole net or none of it, and one commit to
        // disk rather than one per row.
        Database::WriteTransaction transaction(db);
        Net net = slice.net;
        net.imported_at = imported_at;
        // A net exported by an older version may carry free text here,
        // from before this was a ZIP field (see Database::NormalizeNetZips).
        net.default_location = ExtractZipCode(net.default_location);
        // Likewise a frequency from before it was checked.
        MoveBadFrequencyToComments(&net.default_frequency, &net.comments);
        // And a mode from before it was a fixed choice.
        net.mode = NormalizeMode(net.mode);
        std::int64_t new_net_id = db->CreateNet(net);
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

        for (const NetSliceSavedStation& saved : slice.saved_stations)
        {
            db->SaveNetStation(new_net_id, saved.station, saved.default_remarks, now);
        }
        for (const Station& station : slice.other_stations)
        {
            db->RecordManualCheckInStation(station, now);
        }

        std::unordered_map<std::int64_t, std::int64_t> instance_id_map;
        for (const NetInstance& instance : slice.instances)
        {
            NetInstance copy = instance;
            copy.net_id = new_net_id;
            instance_id_map[instance.id] = db->CreateNetInstance(copy);
        }

        for (const CheckIn& check_in : slice.check_ins)
        {
            std::unordered_map<std::int64_t, std::int64_t>::const_iterator mapped =
                instance_id_map.find(check_in.net_instance_id);
            if (mapped == instance_id_map.end())
            {
                continue;
            }
            db->AddCheckIn(check_in, mapped->second);
        }

        transaction.Commit();
        return new_net_id;
    }

    bool WriteNetSliceFile(const std::string& dest_path, const NetSlice& slice, std::string* error)
    {
        std::string::size_type slash = dest_path.find_last_of('/');
        if (slash != std::string::npos)
        {
            EnsureDirectory(dest_path.substr(0, slash));
        }
        // Built at a fresh temporary path, then moved into place: writing
        // straight to dest_path would merge with a stale file there (e.g.
        // re-exporting the same net), since Database's constructor only
        // creates tables that don't already exist -- and with another session
        // exporting the same net at the same moment, both would write into
        // one file, leaving it with two nets that can't be imported.
        std::string temp_path = TemporaryPathFor(dest_path);
        try
        {
            Database dest(temp_path, /*use_wal=*/false);
            ApplyNetSlice(&dest, slice, slice.net.imported_at);
        }
        catch (const std::exception& e)
        {
            std::error_code remove_error;
            std::filesystem::remove(temp_path, remove_error);
            *error = e.what();
            return false;
        }
        return ReplaceWithFile(temp_path, dest_path, error);
    }

    std::optional<NetSlice> ReadNetSliceFile(const std::string& source_path, std::string* error)
    {
        try
        {
            Database source(source_path, /*use_wal=*/false);
            Database::ReadTransaction reads(&source);
            std::vector<Net> nets = source.GetAllNets();
            if (nets.empty())
            {
                *error = "This file has no net in it.";
                return std::nullopt;
            }
            if (nets.size() > 1)
            {
                *error = "This file has more than one net in it -- not a single net export.";
                return std::nullopt;
            }

            NetSlice slice;
            slice.net = std::move(nets[0]);

            std::unordered_set<std::string> known_callsigns;
            std::vector<Station> saved = source.GetSavedStationsForNet(slice.net.id);
            slice.saved_stations.reserve(saved.size());
            for (Station& station : saved)
            {
                NetSliceSavedStation entry;
                entry.default_remarks = source.GetSavedNetStationRemarks(slice.net.id, station.callsign);
                known_callsigns.insert(ToUpperAscii(station.callsign));
                entry.station = std::move(station);
                slice.saved_stations.push_back(std::move(entry));
            }

            slice.instances = source.GetNetInstancesForNet(slice.net.id);
            for (const NetInstance& instance : slice.instances)
            {
                std::vector<CheckIn> check_ins = source.GetCheckInsForNetInstance(instance.id);
                for (const CheckIn& check_in : check_ins)
                {
                    // A station that checked in but isn't saved to the net
                    // (e.g. removed from its saved stations since) -- its
                    // check-ins need its stations row on the importing side.
                    std::string upper = ToUpperAscii(check_in.callsign);
                    if (known_callsigns.insert(upper).second)
                    {
                        std::optional<Station> station = source.FindStationByCallsign(upper);
                        if (station.has_value())
                        {
                            slice.other_stations.push_back(std::move(*station));
                        }
                    }
                }
                slice.check_ins.insert(slice.check_ins.end(), std::make_move_iterator(check_ins.begin()),
                                       std::make_move_iterator(check_ins.end()));
            }

            return slice;
        }
        catch (const std::exception& e)
        {
            *error = e.what();
            return std::nullopt;
        }
    }

    NetSlice GatherSessionSlice(Database* db, std::int64_t instance_id)
    {
        // All of its reads under one snapshot and one lock.
        Database::ReadTransaction reads(db);
        NetSlice slice;
        std::optional<NetInstance> instance = db->GetNetInstanceById(instance_id);
        if (!instance.has_value())
        {
            return slice;
        }
        std::optional<Net> net = db->GetNetById(instance->net_id);
        if (net.has_value())
        {
            slice.net = std::move(*net);
        }
        slice.instances.push_back(std::move(*instance));
        slice.check_ins = db->GetCheckInsForNetInstance(instance_id);

        std::unordered_set<std::string> known_callsigns;
        for (const CheckIn& check_in : slice.check_ins)
        {
            if (!known_callsigns.insert(ToUpperAscii(check_in.callsign)).second)
            {
                continue;
            }
            std::optional<Station> station = db->FindStationByCallsign(check_in.callsign);
            if (station.has_value())
            {
                slice.other_stations.push_back(std::move(*station));
            }
        }
        return slice;
    }

    std::optional<NetSlice> ReadSessionSliceFile(const std::string& source_path, std::string* error)
    {
        std::optional<NetSlice> slice = ReadNetSliceFile(source_path, error);
        if (slice.has_value() && slice->instances.size() != 1)
        {
            *error = slice->instances.empty() ? "This file has no session in it."
                                              : "This file has more than one session in it -- a whole net's export "
                                                "(.qlnet), not a single session's.";
            return std::nullopt;
        }
        return slice;
    }

    std::int64_t ApplySessionSlice(Database* db, const NetSlice& slice, std::int64_t net_id, std::string* error)
    {
        if (slice.instances.size() != 1)
        {
            *error = "This file doesn't hold exactly one session.";
            return 0;
        }
        // One transaction: the whole session or none of it.
        Database::WriteTransaction transaction(db);
        const NetInstance& source = slice.instances[0];
        for (const NetInstance& existing : db->GetNetInstancesForNet(net_id))
        {
            if (existing.instance_date == source.instance_date && existing.started_at == source.started_at)
            {
                *error =
                    "This net already has that session (" + source.instance_date +
                    (source.started_at > 0 ? ", started " + FormatLocalTimeOfDay(source.started_at) : std::string()) +
                    "), so nothing was imported.";
                return 0;
            }
        }

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        // The file's stations by callsign: pointers into `slice`, not copies.
        std::unordered_map<std::string, const Station*> stations;
        stations.reserve(slice.other_stations.size() + slice.saved_stations.size());
        for (const Station& station : slice.other_stations)
        {
            stations[ToUpperAscii(station.callsign)] = &station;
        }
        for (const NetSliceSavedStation& saved : slice.saved_stations)
        {
            stations[ToUpperAscii(saved.station.callsign)] = &saved.station;
        }

        NetInstance instance = source;
        instance.net_id = net_id;
        std::int64_t instance_id = db->CreateNetInstance(instance);

        std::unordered_set<std::string> saved_callsigns;
        for (const CheckIn& check_in : slice.check_ins)
        {
            std::string upper = ToUpperAscii(check_in.callsign);
            if (saved_callsigns.insert(upper).second)
            {
                std::unordered_map<std::string, const Station*>::const_iterator found = stations.find(upper);
                // Just the callsign, if the file has nothing else on it.
                Station bare;
                if (found == stations.end())
                {
                    bare.callsign = upper;
                }
                const Station& station = found != stations.end() ? *found->second : bare;
                std::string remarks = db->GetSavedNetStationRemarks(net_id, upper);
                db->SaveNetStation(net_id, station, remarks.empty() ? check_in.remarks : remarks, now);
            }
            db->AddCheckIn(check_in, instance_id);
        }
        // Numbered without gaps once closed, as if it had been closed here.
        if (instance.status == NetInstanceStatus::kClosed)
        {
            db->RenumberCheckIns(instance_id);
        }
        transaction.Commit();
        return instance_id;
    }

    // A check-in as compared by SameCheckIns, appended to `key` (cleared
    // first) so one string is reused rather than built from temporaries.
    static void CheckInKey(const CheckIn& check_in, std::string* key)
    {
        key->clear();
        key->reserve(check_in.callsign.size() + check_in.signal_report.size() + check_in.remarks.size() +
                     check_in.comment.size() + 8);
        key->append(check_in.callsign);
        for (char& c : *key)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        key->push_back('\x1f');
        key->append(check_in.signal_report);
        key->push_back('\x1f');
        key->append(check_in.remarks);
        key->push_back('\x1f');
        key->append(check_in.comment);
        key->push_back('\x1f');
        key->append(std::to_string(check_in.designated_role));
    }

    // The check-ins' keys, sorted, for comparing two sessions' check-ins in
    // any order.
    static std::vector<std::string> SortedKeys(const std::vector<CheckIn>& check_ins)
    {
        std::vector<std::string> keys(check_ins.size());
        for (std::size_t i = 0; i < check_ins.size(); ++i)
        {
            CheckInKey(check_ins[i], &keys[i]);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    static std::vector<std::string> SortedKeys(const std::vector<const CheckIn*>& check_ins)
    {
        std::vector<std::string> keys(check_ins.size());
        for (std::size_t i = 0; i < check_ins.size(); ++i)
        {
            CheckInKey(*check_ins[i], &keys[i]);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    bool SameCheckIns(const std::vector<CheckIn>& a, const std::vector<CheckIn>& b)
    {
        return a.size() == b.size() && SortedKeys(a) == SortedKeys(b);
    }

    // Starts this far apart or less are the same session, when either one
    // has no end.
    static constexpr std::int64_t kSameStartSeconds = 30 * 60;

    static bool HasEnd(const NetInstance& instance)
    {
        return instance.status == NetInstanceStatus::kClosed && instance.closed_at > 0;
    }

    static bool BothHaveStartTimes(const NetInstance& a, const NetInstance& b)
    {
        return a.started_at > 0 && b.started_at > 0;
    }

    // For two sessions with start times: whether their times overlap.
    static bool TimesMatch(const NetInstance& a, const NetInstance& b)
    {
        if (HasEnd(a) && HasEnd(b))
        {
            return a.started_at <= b.closed_at && b.started_at <= a.closed_at;
        }
        return std::llabs(a.started_at - b.started_at) <= kSameStartSeconds;
    }

    bool SameSession(const NetInstance& a, const std::vector<CheckIn>& a_check_ins, const NetInstance& b,
                     const std::vector<CheckIn>& b_check_ins)
    {
        if (!BothHaveStartTimes(a, b))
        {
            return a.instance_date == b.instance_date && SameCheckIns(a_check_ins, b_check_ins);
        }
        return TimesMatch(a, b);
    }

    // The slice's check-ins grouped by the file's session id: pointers into
    // the slice, not copies.
    static std::unordered_map<std::int64_t, std::vector<const CheckIn*>> CheckInsBySession(const NetSlice& slice)
    {
        std::unordered_map<std::int64_t, std::vector<const CheckIn*>> grouped;
        for (const CheckIn& check_in : slice.check_ins)
        {
            grouped[check_in.net_instance_id].push_back(&check_in);
        }
        return grouped;
    }

    // `id`'s sorted check-in keys from `keys`, worked out the first time
    // they're needed.
    template <typename CheckIns>
    static const std::vector<std::string>& KeysFor(std::int64_t id, const CheckIns& check_ins,
                                                   std::unordered_map<std::int64_t, std::vector<std::string>>* keys)
    {
        std::unordered_map<std::int64_t, std::vector<std::string>>::iterator found = keys->find(id);
        if (found == keys->end())
        {
            found = keys->emplace(id, SortedKeys(check_ins)).first;
        }
        return found->second;
    }

    // The station details a merge compares: each one's label, as shown,
    // and its Station member, so a difference can be applied.
    struct DetailFieldName
    {
        const char* label;
        std::string Station::* member;
    };

    static const DetailFieldName kDetailFields[] = {
        {"name", &Station::name}, {"member ID", &Station::member_id}, {"address", &Station::street_address},
        {"city", &Station::city}, {"county", &Station::county},       {"state", &Station::state},
        {"ZIP", &Station::zip},   {"grid", &Station::grid_square},
    };

    static constexpr int kDetailFieldCount = static_cast<int>(sizeof(kDetailFields) / sizeof(kDetailFields[0]));

    static bool SameIgnoringCase(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            {
                return false;
            }
        }
        return true;
    }

    // Adds `field` to `differences` if it's filled in both here and in the
    // file, but differently.
    static void CompareDetail(int field_index, const std::string& here, const std::string& file,
                              std::vector<StationDetailDifference>* differences)
    {
        if (here.empty() || file.empty() || SameIgnoringCase(here, file))
        {
            return;
        }
        StationDetailDifference& difference = differences->emplace_back();
        difference.field = kDetailFields[field_index].label;
        difference.field_index = field_index;
        difference.here = here;
        difference.file = file;
    }

    // Orders a slice's stations by callsign.
    class StationPointerBefore
    {
    public:
        bool operator()(const Station* a, const Station* b) const
        {
            return a->callsign < b->callsign;
        }
    };

    class StationBeforeCallsign
    {
    public:
        bool operator()(const Station& station, const std::string& callsign) const
        {
            return station.callsign < callsign;
        }
    };

    // The file's stations (saved and others) also here whose details differ,
    // sorted by callsign. The ones here are read in one go.
    static std::vector<MergeStationConflict> StationConflicts(Database* db, const NetSlice& slice)
    {
        std::vector<const Station*> file_stations;
        file_stations.reserve(slice.saved_stations.size() + slice.other_stations.size());
        for (const NetSliceSavedStation& saved : slice.saved_stations)
        {
            file_stations.push_back(&saved.station);
        }
        for (const Station& station : slice.other_stations)
        {
            file_stations.push_back(&station);
        }
        std::sort(file_stations.begin(), file_stations.end(), StationPointerBefore());
        std::vector<std::string> callsigns;
        callsigns.reserve(file_stations.size());
        for (const Station* station : file_stations)
        {
            callsigns.push_back(ToUpperAscii(station->callsign));
        }
        std::vector<Station> here = db->FindStationsByCallsigns(callsigns);

        std::vector<MergeStationConflict> conflicts;
        for (std::size_t i = 0; i < file_stations.size(); ++i)
        {
            std::vector<Station>::const_iterator found =
                std::lower_bound(here.begin(), here.end(), callsigns[i], StationBeforeCallsign());
            if (found == here.end() || found->callsign != callsigns[i])
            {
                continue;
            }
            const Station& mine = *found;
            const Station& theirs = *file_stations[i];
            std::vector<StationDetailDifference> differences;
            for (int field = 0; field < kDetailFieldCount; ++field)
            {
                std::string Station::* member = kDetailFields[field].member;
                CompareDetail(field, mine.*member, theirs.*member, &differences);
            }
            if (!differences.empty())
            {
                MergeStationConflict& conflict = conflicts.emplace_back();
                conflict.file_station = &theirs;
                conflict.differences = std::move(differences);
            }
        }
        return conflicts;
    }

    // Which callsigns' check-ins differ between a session in the file and
    // the same session here (see MergeSession::callsigns_changed).
    static void DescribeCheckInChanges(const std::vector<const CheckIn*>& file, const std::vector<CheckIn>& here,
                                       MergeSession* session)
    {
        std::unordered_map<std::string, std::string> here_keys;
        here_keys.reserve(here.size());
        std::string key;
        for (const CheckIn& check_in : here)
        {
            CheckInKey(check_in, &key);
            here_keys.emplace(ToUpperAscii(check_in.callsign), key);
        }
        for (const CheckIn* check_in : file)
        {
            std::string callsign = ToUpperAscii(check_in->callsign);
            std::unordered_map<std::string, std::string>::iterator found = here_keys.find(callsign);
            if (found == here_keys.end())
            {
                session->callsigns_only_in_file.push_back(std::move(callsign));
                continue;
            }
            CheckInKey(*check_in, &key);
            if (found->second != key)
            {
                session->callsigns_changed.push_back(std::move(callsign));
            }
            here_keys.erase(found);
        }
        for (std::pair<const std::string, std::string>& left : here_keys)
        {
            session->callsigns_only_here.push_back(left.first);
        }
        std::sort(session->callsigns_changed.begin(), session->callsigns_changed.end());
        std::sort(session->callsigns_only_in_file.begin(), session->callsigns_only_in_file.end());
        std::sort(session->callsigns_only_here.begin(), session->callsigns_only_here.end());
    }

    NetMergePlan PlanNetMerge(Database* db, const NetSlice& slice, std::int64_t target_net_id)
    {
        Database::ReadTransaction reads(db);
        NetMergePlan plan;
        plan.target_net_id = target_net_id;

        std::unordered_set<std::string> saved_here;
        for (const Station& here : db->GetSavedStationsForNet(target_net_id))
        {
            saved_here.insert(ToUpperAscii(here.callsign));
        }
        for (const NetSliceSavedStation& saved : slice.saved_stations)
        {
            bool known = saved_here.count(ToUpperAscii(saved.station.callsign)) != 0;
            (known ? plan.known_saved_stations : plan.new_saved_stations) += 1;
        }

        plan.station_conflicts = StationConflicts(db, slice);

        std::vector<NetInstance> local = db->GetNetInstancesForNet(target_net_id);
        // Every check-in here, in one query, by session.
        std::unordered_map<std::int64_t, std::vector<CheckIn>> local_check_ins;
        for (CheckIn& check_in : db->GetCheckInsForNet(target_net_id))
        {
            std::int64_t session_id = check_in.net_instance_id;
            local_check_ins[session_id].push_back(std::move(check_in));
        }
        std::unordered_map<std::int64_t, std::vector<const CheckIn*>> file_check_ins = CheckInsBySession(slice);
        // Each session's sorted check-in keys, made only when compared.
        std::unordered_map<std::int64_t, std::vector<std::string>> file_keys;
        std::unordered_map<std::int64_t, std::vector<std::string>> local_keys;
        std::unordered_set<std::int64_t> matched;

        plan.sessions.reserve(slice.instances.size());
        for (std::size_t i = 0; i < slice.instances.size(); ++i)
        {
            const NetInstance& file = slice.instances[i];
            const std::vector<const CheckIn*>& file_list = file_check_ins[file.id];
            MergeSession& session = plan.sessions.emplace_back();
            session.file_index = i;
            session.file_check_ins = static_cast<int>(file_list.size());
            session.file_open = file.status == NetInstanceStatus::kOpen;
            for (const NetInstance& here : local)
            {
                if (matched.count(here.id) != 0)
                {
                    continue;
                }
                const std::vector<CheckIn>& here_list = local_check_ins[here.id];
                bool same_session =
                    BothHaveStartTimes(file, here)
                        ? TimesMatch(file, here)
                        : file.instance_date == here.instance_date && file_list.size() == here_list.size() &&
                              KeysFor(file.id, file_list, &file_keys) == KeysFor(here.id, here_list, &local_keys);
                if (!same_session)
                {
                    continue;
                }
                matched.insert(here.id);
                session.local_id = here.id;
                session.local_open = here.status == NetInstanceStatus::kOpen;
                session.local_check_ins = static_cast<int>(here_list.size());
                session.check_ins_differ =
                    file_list.size() != here_list.size() ||
                    KeysFor(file.id, file_list, &file_keys) != KeysFor(here.id, here_list, &local_keys);
                session.notes_differ = file.notes != here.notes;
                if (session.check_ins_differ)
                {
                    DescribeCheckInChanges(file_list, here_list, &session);
                }
                bool same = !session.check_ins_differ && !session.notes_differ;
                session.kind = same || session.local_open ? MergeSessionKind::kAlreadyHere : MergeSessionKind::kDiffers;
                break;
            }
        }
        return plan;
    }

    NetMergeResult ApplyNetMerge(Database* db, const NetSlice& slice, const NetMergePlan& plan)
    {
        NetMergeResult result;
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        Database::WriteTransaction transaction(db);

        // The sessions being replaced go first, so a station only they used
        // isn't left behind, nor removed after it's needed again.
        for (const MergeSession& session : plan.sessions)
        {
            if (session.kind == MergeSessionKind::kDiffers && session.replace)
            {
                db->DeleteNetInstance(session.local_id);
            }
        }

        // Stations whose details the operator chose to take from the file:
        // just the details listed as differing (read here in one go), so
        // nothing else changes.
        std::vector<std::string> replacing;
        for (const MergeStationConflict& conflict : plan.station_conflicts)
        {
            if (conflict.replace)
            {
                replacing.push_back(ToUpperAscii(conflict.file_station->callsign));
            }
        }
        if (!replacing.empty())
        {
            std::vector<Station> stations = db->FindStationsByCallsigns(replacing);
            for (const MergeStationConflict& conflict : plan.station_conflicts)
            {
                if (!conflict.replace)
                {
                    continue;
                }
                std::string callsign = ToUpperAscii(conflict.file_station->callsign);
                std::vector<Station>::iterator station =
                    std::lower_bound(stations.begin(), stations.end(), callsign, StationBeforeCallsign());
                if (station == stations.end() || station->callsign != callsign)
                {
                    continue;
                }
                for (const StationDetailDifference& difference : conflict.differences)
                {
                    (*station).*(kDetailFields[difference.field_index].member) = difference.file;
                }
                db->UpdateStationFields(*station, now);
                ++result.stations_replaced;
            }
        }

        // Stations already written (below), so the check-ins don't write
        // them again.
        std::unordered_set<std::string> have_station;
        for (const NetSliceSavedStation& saved : slice.saved_stations)
        {
            db->FillStationBlanks(saved.station, now);
            have_station.insert(ToUpperAscii(saved.station.callsign));
            if (db->AddNetSavedStationIfMissing(plan.target_net_id, saved.station.callsign, saved.default_remarks))
            {
                ++result.saved_stations_added;
            }
        }
        std::unordered_map<std::int64_t, std::vector<const CheckIn*>> file_check_ins = CheckInsBySession(slice);
        // The other stations' details, for those in sessions being added
        // (the rest would be left with nothing referring to them).
        std::unordered_set<std::string> callsigns_added;
        for (const MergeSession& session : plan.sessions)
        {
            if (session.kind == MergeSessionKind::kNew ||
                (session.kind == MergeSessionKind::kDiffers && session.replace))
            {
                for (const CheckIn* check_in : file_check_ins[slice.instances[session.file_index].id])
                {
                    callsigns_added.insert(ToUpperAscii(check_in->callsign));
                }
            }
        }
        for (const Station& station : slice.other_stations)
        {
            if (callsigns_added.count(ToUpperAscii(station.callsign)) != 0)
            {
                db->FillStationBlanks(station, now);
                have_station.insert(ToUpperAscii(station.callsign));
            }
        }

        for (const MergeSession& session : plan.sessions)
        {
            bool add = session.kind == MergeSessionKind::kNew ||
                       (session.kind == MergeSessionKind::kDiffers && session.replace);
            if (!add)
            {
                continue;
            }
            const NetInstance& file = slice.instances[session.file_index];
            const std::vector<const CheckIn*>& check_ins = file_check_ins[file.id];
            NetInstance instance = file;
            instance.net_id = plan.target_net_id;
            if (session.file_open)
            {
                std::int64_t last = file.started_at;
                for (const CheckIn* check_in : check_ins)
                {
                    last = std::max(last, check_in->checked_in_at);
                }
                instance.status = NetInstanceStatus::kClosed;
                instance.closed_at = last > 0 ? last : now;
            }
            std::int64_t instance_id = db->CreateNetInstance(instance);
            for (const CheckIn* check_in : check_ins)
            {
                // Only the callsign, if the file has nothing else on it
                // (once per station, not per check-in).
                if (have_station.insert(ToUpperAscii(check_in->callsign)).second)
                {
                    Station bare;
                    bare.callsign = check_in->callsign;
                    db->FillStationBlanks(bare, now);
                }
                db->AddCheckIn(*check_in, instance_id);
            }
            if (session.file_open)
            {
                db->RenumberCheckIns(instance_id);
            }
            ++(session.kind == MergeSessionKind::kNew ? result.sessions_added : result.sessions_replaced);
        }

        transaction.Commit();
        return result;
    }

}  // namespace ql
