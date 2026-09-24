#include "net_slice.hpp"

#include <ctime>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "file_export.hpp"
#include "text_utils.hpp"

namespace ql
{

    NetSlice GatherNetSlice(Database* db, std::int64_t net_id)
    {
        NetSlice slice;
        std::optional<Net> net = db->GetNetById(net_id);
        if (net.has_value())
        {
            slice.net = *net;
        }

        std::unordered_set<std::string> known_callsigns;
        std::vector<Station> saved = db->GetSavedStationsForNet(net_id);
        for (const Station& station : saved)
        {
            NetSliceSavedStation entry;
            entry.station = station;
            entry.default_remarks = db->GetSavedNetStationRemarks(net_id, station.callsign);
            slice.saved_stations.push_back(std::move(entry));
            known_callsigns.insert(ToUpperAscii(station.callsign));
        }

        slice.instances = db->GetNetInstancesForNet(net_id);
        for (const NetInstance& instance : slice.instances)
        {
            std::vector<CheckIn> check_ins = db->GetCheckInsForNetInstance(instance.id);
            for (const CheckIn& check_in : check_ins)
            {
                slice.check_ins.push_back(check_in);

                std::string upper = ToUpperAscii(check_in.callsign);
                if (known_callsigns.find(upper) != known_callsigns.end())
                {
                    continue;
                }
                known_callsigns.insert(upper);

                // Only reachable for check-in history logged before stations
                // started auto-saving on check-in -- see NetSlice::other_stations.
                std::optional<Station> station = db->FindStationByCallsign(check_in.callsign);
                if (station.has_value())
                {
                    slice.other_stations.push_back(*station);
                }
            }
        }

        return slice;
    }

    std::int64_t ApplyNetSlice(Database* db, const NetSlice& slice, std::int64_t imported_at)
    {
        Net net = slice.net;
        net.imported_at = imported_at;
        // A net exported by an older version may carry free text here,
        // from before this was a ZIP field (see Database::NormalizeNetZips).
        net.default_location = ExtractZipCode(net.default_location);
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
            CheckIn copy = check_in;
            copy.net_instance_id = mapped->second;
            db->AddCheckIn(copy);
        }

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
            slice.net = nets[0];

            std::unordered_set<std::string> known_callsigns;
            std::vector<Station> saved = source.GetSavedStationsForNet(slice.net.id);
            for (const Station& station : saved)
            {
                NetSliceSavedStation entry;
                entry.station = station;
                entry.default_remarks =
                    source.GetSavedNetStationRemarks(slice.net.id, station.callsign);
                slice.saved_stations.push_back(std::move(entry));
                known_callsigns.insert(ToUpperAscii(station.callsign));
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
                            slice.other_stations.push_back(*station);
                        }
                    }
                }
                slice.check_ins.insert(slice.check_ins.end(), check_ins.begin(), check_ins.end());
            }

            return slice;
        }
        catch (const std::exception& e)
        {
            *error = e.what();
            return std::nullopt;
        }
    }

}  // namespace ql
