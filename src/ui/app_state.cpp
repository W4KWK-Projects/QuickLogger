#include "app_state.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <utility>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

#include "../adif_export.hpp"
#include "../callsign_rules.hpp"
#include "../date_utils.hpp"
#include "../frequency_rules.hpp"
#include "../file_export.hpp"
#include "../geo_utils.hpp"
#include "../mode_rules.hpp"
#include "../net_slice.hpp"
#include "../public_key.hpp"
#include "../show_folder.hpp"
#include "../text_utils.hpp"
#include "../update_check.hpp"
#include "../zip_write.hpp"
#include "../zmodem_send.hpp"
#include "list_columns.hpp"

namespace ql
{

    // How many check-ins a session has, counted by the database rather than
    // read whole to count.
    static std::size_t CheckInCount(Database* db, std::int64_t instance_id)
    {
        std::int64_t count = 0;
        std::int64_t newest_id = 0;
        db->GetCheckInSummary(instance_id, &count, &newest_id);
        return static_cast<std::size_t>(count);
    }

    static bool CallsignsEqual(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            unsigned char char_a = static_cast<unsigned char>(a[i]);
            unsigned char char_b = static_cast<unsigned char>(b[i]);
            if (std::toupper(char_a) != std::toupper(char_b))
            {
                return false;
            }
        }
        return true;
    }

    // ---- Lists laid out for the terminal's width -------------------------------
    //
    // Every list below describes its columns once (see list_columns.hpp); its
    // rows are kept as cells, and turned into the lines a Menu shows by
    // laying those out for AppState::list_width. The header line comes from
    // the same layout, so it can't drift out of line with the rows. The
    // columns shown at 80 columns, and their widths, are exactly the ones
    // these lists have always had.

    // Every header below sits above an ftxui::Menu, not a plain text list.
    // Menu's default entry renderer always prepends a 2-character indicator
    // ("> " for the focused row, "  " for every other one -- see
    // DefaultOptionTransform in FTXUI's menu.cpp) to every row it renders, so
    // without this same gutter a header's labels land two columns left of the
    // data they're supposed to name.
    static constexpr int kMenuEntryIndicatorWidth = 2;

    // The room a full-width list's rows get: the terminal less the list's
    // border, the Menu's gutter and the scroll bar every page list has
    // inside its right edge (missing that last one cut a wide terminal's
    // last column by a character). Never less than at 80 columns.
    static int ScreenListWidth(int terminal_width)
    {
        return std::max(80, terminal_width) - 5;
    }
    static constexpr int kScreenListWidthAt80 = 75;

    // The same for the autocomplete matches, which sit inside a window.
    int CheckInWindowWidth(int terminal_width)
    {
        return std::min(std::max(80, terminal_width) - 10, 150);
    }

    // The match list inside that window: less its border, the list's own
    // border, the "> " gutter and the scroll bar (see ScreenListWidth).
    static int MatchListWidth(int terminal_width)
    {
        return CheckInWindowWidth(terminal_width) - 7;
    }
    static constexpr int kMatchListWidthAt80 = 63;

    std::size_t MaxCallsignMatches(const AppState* state)
    {
        int room = state->screen_height - kMatchWindowOtherRows;
        return static_cast<std::size_t>(std::min(std::max(room, 8), 60));
    }

    static std::string MenuGutter()
    {
        return std::string(kMenuEntryIndicatorWidth, ' ');
    }

    // A list's column heading as last built, and what it was built for: a
    // heading only changes when the terminal's width (or, for some lists,
    // one other thing) does, but pages ask for it on every frame.
    struct HeadingCache
    {
        int width = -1;
        int extra = -1;
        std::string text;
    };

    // True if `cache` needs building for `width` and `extra`, and marks it
    // as built for them.
    static bool HeadingNeedsBuilding(HeadingCache* cache, int width, int extra)
    {
        if (cache->width == width && cache->extra == extra)
        {
            return false;
        }
        cache->width = width;
        cache->extra = extra;
        return true;
    }

    static std::vector<std::string> FormatRows(const std::vector<std::vector<std::string>>& rows,
                                               const ListLayout& layout)
    {
        std::vector<std::string> lines;
        lines.reserve(rows.size());
        for (const std::vector<std::string>& row : rows)
        {
            lines.push_back(FormatListRow(row, layout));
        }
        return lines;
    }

    // "Chattanooga, TN", or whichever half is known.
    static std::string CityAndState(const Station& station)
    {
        std::string text;
        text.reserve(station.city.size() + 2 + station.state.size());
        text += station.city;
        if (!station.city.empty() && !station.state.empty())
        {
            text += ", ";
        }
        text += station.state;
        return text;
    }

    // Short tag for CheckIn::designated_role, shown in its own column so a
    // station holding Alternate Net Control or Logger is visible straight
    // from the check-in list, not just by opening it to edit. Blank (not
    // "None") for kRoleNone, so an undesignated check-in's row doesn't look
    // busier than one that's just never been looked at.
    static std::string RoleAbbreviation(int role)
    {
        switch (role)
        {
            case kRoleNetControl:
                return "NC";
            case kRoleAlternateNetControl:
                return "AltNC";
            case kRoleLogger:
                return "Log";
            default:
                return "";
        }
    }

    // -- Check-ins (the active net, History, exported logs) --

    // Heading, width, widest, when added, when widened, width in exports.
    static const std::vector<ListColumn>& CheckInColumns()
    {
        static const std::vector<ListColumn> columns = {
            {"#", 3, 3, 0, 0, 4},         {"Time", 8, 8, 2, 0, 8},         {"Callsign", 10, 13, 0, 99, 13},
            {"Name", 20, 24, 0, 1, 30},   {"Member ID", 10, 10, 0, 0, 10}, {"City, State", 16, 24, 3, 4, 30},
            {"County", 14, 14, 0, 0, 20}, {"Role", 6, 6, 0, 0, 6},         {"Signal", 6, 6, 5, 0, 6},
            {"Remarks", 7, 30, 0, 7, 40}, {"Comment", 8, 40, 6, 0, 60},
        };
        return columns;
    }

    // `columns` with the Time column a column wider (see
    // ScreenCheckInColumns).
    static std::vector<ListColumn> RoomyTimeColumns(std::vector<ListColumn> columns)
    {
        for (ListColumn& column : columns)
        {
            if (column.heading == "Time")
            {
                column.width = 9;
                column.max_width = 9;
            }
        }
        return columns;
    }

    // Orders stations by callsign, for looking one up in a sorted list.
    static bool StationCallsignBefore(const Station& station, const std::string& callsign)
    {
        return station.callsign < callsign;
    }

    std::vector<std::vector<std::string>> CheckInCells(Database* db, const std::vector<CheckIn>& check_ins)
    {
        std::vector<std::vector<std::string>> rows;
        if (check_ins.empty())
        {
            return rows;
        }
        rows.reserve(check_ins.size());
        // Every check-in here is from one session: its stations in one query.
        std::int64_t instance_id = check_ins.front().net_instance_id;
        std::vector<Station> stations = db->GetStationsInNetInstance(instance_id);
        static const Station kUnknownStation;
        for (const CheckIn& check_in : check_ins)
        {
            std::vector<Station>::const_iterator found =
                std::lower_bound(stations.begin(), stations.end(), check_in.callsign, StationCallsignBefore);
            const Station& station =
                found != stations.end() && found->callsign == check_in.callsign ? *found : kUnknownStation;
            // Built in place: a braced list would copy every string twice.
            std::vector<std::string>& row = rows.emplace_back();
            row.reserve(11);
            row.push_back(std::to_string(check_in.sequence_number));
            row.push_back(FormatLocalTimeOfDay(check_in.checked_in_at));
            row.push_back(check_in.callsign);
            row.push_back(station.name);
            row.push_back(station.member_id);
            row.push_back(CityAndState(station));
            row.push_back(station.county);
            row.push_back(RoleAbbreviation(check_in.designated_role));
            row.push_back(check_in.signal_report);
            row.push_back(check_in.remarks);
            row.push_back(check_in.comment);
        }
        return rows;
    }

    // From this terminal width, Time is a column wider than its text, so
    // the times stand clear of the callsigns.
    static constexpr int kRoomyCheckInTimeWidth = 100;

    // The check-in columns on screen: CheckInColumns, but with Time 9 wide
    // from kRoomyCheckInTimeWidth columns.
    static const std::vector<ListColumn>& ScreenCheckInColumns(int terminal_width)
    {
        static const std::vector<ListColumn> roomy = RoomyTimeColumns(CheckInColumns());
        return terminal_width >= kRoomyCheckInTimeWidth ? roomy : CheckInColumns();
    }

    static ListLayout CheckInLayout(int terminal_width)
    {
        return LayOutList(ScreenCheckInColumns(terminal_width), ScreenListWidth(terminal_width), kScreenListWidthAt80,
                          1);
    }

    std::vector<std::string> FormatCheckInList(const std::vector<std::vector<std::string>>& cells, int terminal_width)
    {
        return FormatRows(cells, CheckInLayout(terminal_width));
    }

    const std::string& CheckInListHeader(int terminal_width)
    {
        static HeadingCache cache;
        if (HeadingNeedsBuilding(&cache, terminal_width, 0))
        {
            cache.text =
                MenuGutter() + FormatListHeading(ScreenCheckInColumns(terminal_width), CheckInLayout(terminal_width));
        }
        return cache.text;
    }

    // -- Net sessions (History) --

    static const std::vector<ListColumn>& NetInstanceColumns(bool ad_hoc)
    {
        // Each column leaves a space after its heading, so headings never
        // run together where the gap is one space ("Alternate NC" filled
        // its column at 80 and ran into "Logger"); the role columns fit a
        // callsign with a portable suffix. Date, Start and End are a column
        // wider than their text, so at 80 columns (one-space gaps) each is
        // followed by two spaces; see NetInstanceLayout for Date once the
        // gaps widen.
        // A recurring net's own history; its name is in the page title.
        // Started by (who opened the session) and Notes (whether it has
        // Session Notes, F12) come on wide terminals, after Check-ins.
        static const std::vector<ListColumn> recurring = {
            {"Date", 11, 11, 0, 0},        {"Start", 9, 9, 0, 0},          {"End", 9, 9, 0, 0},
            {"Net Control", 12, 12, 0, 0}, {"Alternate NC", 13, 13, 0, 0}, {"Logger", 9, 9, 0, 0},
            {"Started by", 11, 11, 2, 0},  {"Check-ins", 10, 10, 1, 0},    {"Notes", 6, 6, 3, 0},
            {"Status", 6, 6, 0, 0},
        };
        // Every ad hoc net's sessions in one list: at 80 columns the net's
        // name takes the place of Alternate NC and Logger, which come back
        // when there's room.
        static const std::vector<ListColumn> every_ad_hoc = {
            {"Date", 11, 11, 0, 0}, {"Start", 9, 9, 0, 0},         {"End", 9, 9, 0, 0},
            {"Net", 23, 30, 0, 1},  {"Net Control", 12, 12, 0, 0}, {"Alternate NC", 13, 13, 3, 0},
            {"Logger", 9, 9, 4, 0}, {"Started by", 11, 11, 5, 0},  {"Check-ins", 10, 10, 1, 0},
            {"Notes", 6, 6, 6, 0},  {"Status", 6, 6, 0, 0},
        };
        return ad_hoc ? every_ad_hoc : recurring;
    }

    static std::vector<std::string> NetInstanceCells(const NetInstance& instance, const std::string& net_name,
                                                     std::int64_t check_ins, bool ad_hoc)
    {
        std::vector<std::string> cells;
        cells.reserve(ad_hoc ? 11 : 10);
        cells.push_back(instance.instance_date);
        cells.push_back(FormatLocalTimeOfDay(instance.started_at));
        cells.push_back(FormatLocalTimeOfDay(instance.closed_at));
        if (ad_hoc)
        {
            cells.push_back(net_name);
        }
        cells.push_back(instance.net_control_callsign);
        cells.push_back(instance.alternate_net_control_callsign);
        cells.push_back(instance.logger_callsign);
        cells.push_back(instance.created_by);
        cells.push_back(std::to_string(check_ins));
        cells.emplace_back(instance.notes.empty() ? "" : "yes");
        cells.emplace_back(instance.status == NetInstanceStatus::kOpen ? "OPEN" : "closed");
        return cells;
    }

    static ListLayout NetInstanceLayout(int terminal_width, bool ad_hoc)
    {
        // Once everything is shown, the gaps keep widening, up to 4, so a
        // wide terminal's row spreads out rather than bunching at the left.
        ListLayout layout =
            LayOutList(NetInstanceColumns(ad_hoc), ScreenListWidth(terminal_width), kScreenListWidthAt80, 1, 4);
        // Date's extra column is only for one-space gaps; with wider ones
        // it would sit further from Start than the other columns are apart.
        if (layout.gap > 1)
        {
            layout.widths[0] = 10;
        }
        return layout;
    }

    const std::string& NetInstanceListHeader(int terminal_width, bool ad_hoc)
    {
        static HeadingCache cache;
        if (HeadingNeedsBuilding(&cache, terminal_width, ad_hoc ? 1 : 0))
        {
            cache.text =
                MenuGutter() + FormatListHeading(NetInstanceColumns(ad_hoc), NetInstanceLayout(terminal_width, ad_hoc));
        }
        return cache.text;
    }

    // -- A net's saved stations (Edit Net, exported lists) --

    static const std::vector<ListColumn>& SavedStationColumns()
    {
        static const std::vector<ListColumn> columns = {
            // At 80 columns: Name 24 wide and City, State shown, making use
            // of the width (user, 2026-10-01) rather than leaving a third
            // of it blank.
            {"Callsign", 10, 13, 0, 99, 13},       {"Name", 24, 24, 0, 7, 30},   {"Member ID", 10, 10, 0, 0, 10},
            {"City, State", 16, 24, 0, 6, 30},     {"County", 14, 14, 3, 0, 20}, {"Grid", 6, 8, 4, 0, 8},
            {"Default Remarks", 15, 40, 5, 0, 40},
        };
        return columns;
    }

    static std::vector<std::string> SavedStationCells(const Station& station, const std::string& default_remarks)
    {
        std::vector<std::string> cells;
        cells.reserve(7);
        cells.push_back(station.callsign);
        cells.push_back(station.name);
        cells.push_back(station.member_id);
        cells.push_back(CityAndState(station));
        cells.push_back(station.county);
        cells.push_back(station.grid_square);
        cells.push_back(default_remarks);
        return cells;
    }

    static ListLayout SavedStationLayout(int terminal_width)
    {
        return LayOutList(SavedStationColumns(), ScreenListWidth(terminal_width), kScreenListWidthAt80, 1);
    }

    const std::string& SavedStationListHeader(int terminal_width)
    {
        static HeadingCache cache;
        if (HeadingNeedsBuilding(&cache, terminal_width, 0))
        {
            cache.text = MenuGutter() + FormatListHeading(SavedStationColumns(), SavedStationLayout(terminal_width));
        }
        return cache.text;
    }

    // -- SSH users and their keys (Manage Users) --

    // A row per user: their access applies to every key of theirs.
    static const std::vector<ListColumn>& UserColumns()
    {
        static const std::vector<ListColumn> columns = {
            // Room for 2.0's longer GMRS call signs (e.g. WSIP663) and more.
            {"Username", 16, 16, 0, 0},
            {"Access", 9, 9, 0, 0},
            {"Keys", 4, 4, 0, 0},
            {"Last Login", 19, 19, 0, 0},
        };
        return columns;
    }

    static std::string DescribeLastLogin(std::int64_t last_login_at)
    {
        return last_login_at > 0 ? FormatLocalDateTime(last_login_at) : std::string("never");
    }

    // `username`'s row, from their keys in `keys`: their access, how many
    // keys they have, and their latest login with any of them.
    static std::vector<std::string> UserCells(const std::string& username, const std::vector<User>& keys)
    {
        bool view_only = false;
        int count = 0;
        std::int64_t last_login_at = 0;
        for (const User& key : keys)
        {
            if (key.username != username)
            {
                continue;
            }
            view_only = view_only || key.view_only;
            ++count;
            last_login_at = std::max(last_login_at, key.last_login_at);
        }
        std::vector<std::string> cells;
        cells.reserve(4);
        cells.push_back(username);
        cells.emplace_back(view_only ? "View-Only" : "Full");
        cells.push_back(std::to_string(count));
        cells.push_back(DescribeLastLogin(last_login_at));
        return cells;
    }

    static ListLayout UserLayout(int terminal_width)
    {
        // Four short columns: spread out by three spaces even at 80
        // columns, up to four on a wider terminal.
        return LayOutList(UserColumns(), ScreenListWidth(terminal_width), kScreenListWidthAt80, 3, 4);
    }

    const std::string& UserListHeader(int terminal_width)
    {
        static HeadingCache cache;
        if (HeadingNeedsBuilding(&cache, terminal_width, 0))
        {
            cache.text = MenuGutter() + FormatListHeading(UserColumns(), UserLayout(terminal_width));
        }
        return cache.text;
    }

    // The Keys window's rows: each key's type, fingerprint and comment
    // tell them apart, as `ssh-keygen -l` would. Comment goes last, as
    // `ssh-keygen -l` puts it, taking the rest of the row.
    static const std::vector<ListColumn>& UserKeyColumns()
    {
        static const std::vector<ListColumn> columns = {
            {"Type", 7, 10, 0, 2},
            {"Fingerprint", 16, 50, 0, 1},
            {"Last Login", 19, 19, 0, 0},
            {"Comment", 16, 30, 0, 3},
        };
        return columns;
    }

    static std::vector<std::string> UserKeyCells(const User& key)
    {
        PublicKeyDescription description = DescribePublicKey(key.public_key);
        std::vector<std::string> cells;
        cells.reserve(4);
        cells.push_back(std::move(description.type));
        cells.push_back(std::move(description.fingerprint));
        cells.push_back(DescribeLastLogin(key.last_login_at));
        cells.push_back(std::move(description.comment));
        return cells;
    }

    // The Keys window's list sits inside a window as wide as the check-in
    // windows, less its border, the list's border and the Menu's gutter.
    static int UserKeyListWidth(int terminal_width)
    {
        return CheckInWindowWidth(terminal_width) - 6;
    }

    static ListLayout UserKeyLayout(int terminal_width)
    {
        // Two spaces apart even at 80 columns (Comment, last, still gets
        // 16), up to three on a wider terminal.
        return LayOutList(UserKeyColumns(), UserKeyListWidth(terminal_width), UserKeyListWidth(80), 2, 3);
    }

    const std::string& UserKeyListHeader(int terminal_width)
    {
        static HeadingCache cache;
        if (HeadingNeedsBuilding(&cache, terminal_width, 0))
        {
            cache.text = MenuGutter() + FormatListHeading(UserKeyColumns(), UserKeyLayout(terminal_width));
        }
        return cache.text;
    }

    // How many keys `username` has in AppState::manage_users.
    static int CountUserKeys(const AppState* state, const std::string& username)
    {
        int keys = 0;
        for (const User& user : state->manage_users)
        {
            keys += user.username == username ? 1 : 0;
        }
        return keys;
    }

    // "SHA256:zSpp/AdO... (wes@laptop)": enough of a key to say which it is.
    static std::string DescribeUserKey(const User& user)
    {
        PublicKeyDescription key = DescribePublicKey(user.public_key);
        std::string text = key.fingerprint.substr(0, 20) + "...";
        if (!key.comment.empty())
        {
            text += " (" + key.comment + ")";
        }
        return text;
    }

    // -- Autocomplete matches (New Check-In, Saved Station) --

    static const std::vector<ListColumn>& MatchColumns()
    {
        static const std::vector<ListColumn> columns = {
            {"Callsign", 10, 13, 0, 99}, {"Name", 20, 24, 0, 4},   {"City, State", 16, 24, 1, 3},
            {"County", 14, 14, 2, 0},    {"Source", 13, 13, 0, 0},
        };
        return columns;
    }

    static ListLayout MatchLayout(int terminal_width)
    {
        return LayOutList(MatchColumns(), MatchListWidth(terminal_width), kMatchListWidthAt80, 1);
    }

    const std::string& MatchListHeader(int terminal_width)
    {
        static HeadingCache cache;
        if (HeadingNeedsBuilding(&cache, terminal_width, 0))
        {
            cache.text = MenuGutter() + FormatListHeading(MatchColumns(), MatchLayout(terminal_width));
        }
        return cache.text;
    }

    static std::vector<std::string> FormatMatches(const std::vector<Station>& stations,
                                                  const std::vector<std::string>& sources, int terminal_width)
    {
        ListLayout layout = MatchLayout(terminal_width);
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < stations.size() && i < sources.size(); ++i)
        {
            const Station& station = stations[i];
            lines.push_back(FormatListRow(
                {station.callsign, station.name, CityAndState(station), station.county, sources[i]}, layout));
        }
        return lines;
    }

    // "(this net)" / "(other net)" for a match known to a net.
    static std::string KnownStationSource(bool is_this_net)
    {
        return is_this_net ? "(this net)" : "(other net)";
    }

    // `distance_miles` < 0 means "unknown" (the station's own ZIP has no
    // centroid on file) -- shown as "(ULS, nearby)" rather than a fabricated
    // number, since it only passed the coarser ZIP3-prefix pre-filter.
    // A Canadian license, from ISED's data (see AppendCanadianSuggestions).
    static const char* const kIsedSource = "(ISED)";

    static std::string UlsSource(double distance_miles)
    {
        if (distance_miles < 0.0)
        {
            return "(ULS, nearby)";
        }
        // Kept to the Source column's 13 characters (see MatchColumns).
        int miles = static_cast<int>(distance_miles);
        if (miles >= 1000)
        {
            return "(ULS, 999+mi)";
        }
        return "(ULS, ~" + std::to_string(miles) + (miles >= 100 ? "mi)" : " mi)");
    }

    // -- Recurring Nets --

    // Net-list names are padded to the longest (up to this) so the columns
    // after them line up.
    static constexpr int kMaxNetNameColumnWidth = 40;
    static constexpr int kMinNetNameColumnWidth = 30;
    // Columns kept clear after the longest name; and the fewest, when the
    // name narrows to make room for more columns.
    static constexpr int kNetNameBreathingRoom = 4;
    static constexpr int kNetNameNarrowRoom = 2;

    // The when-created/imported column (and "session open") comes last, as
    // it always has.
    // The Recurrence column's index in NetListColumns, and its full width:
    // a typical recurrence ("Wednesdays at 8pm ET") is about 20; more
    // than 24 would take room Offset and PL can use.
    static constexpr std::size_t kNetRecurrenceColumn = 5;
    static constexpr int kNetRecurrenceFullWidth = 24;

    // `name_width` widens toward `name_max_width` only after everything
    // else has been added and widened.
    static std::vector<ListColumn> NetListColumns(int name_width, int name_max_width)
    {
        return {
            {"Net", name_width, name_max_width, 0, 99},
            {"Mode", 6, 8, 1, 6},
            // Always shown: at 80 columns it fits beside the name and the
            // notes, whose longest text ("imported 2026-09-30") is known.
            {"Frequency", 10, 12, 0, 7},
            // A repeater's offset and PL tone, beside the frequency: added
            // together once Recurrence is at full width, before Mode and
            // Frequency widen (from 117 columns).
            {"Offset", 6, 6, 5, 0, 0, true},
            {"PL", 5, 5, 5, 0},
            {"Recurrence", 20, kNetRecurrenceFullWidth, 3, 4},
            // As wide as its longest text, "imported 2026-09-30", so it's
            // never cut (QuickLogger writes it; it's not typed in).
            {"Notes", 19, 19, 0, 0},
        };
    }

    // The Net column at its usual width, with columns dropped as the
    // terminal narrows; or, from about 101 columns down to where Recurrence
    // has to go anyway, a narrower name (net_name_min_width) so Recurrence
    // keeps its room, the name getting back whatever is left over.
    static ListLayout NetListLayout(const AppState* state)
    {
        int available = ScreenListWidth(state->list_width);
        ListLayout usual = LayOutList(NetListColumns(state->net_name_width, state->net_name_width), available,
                                      kScreenListWidthAt80, 2);
        // Recurrence at full width beside the usual name: nothing to gain.
        if (usual.widths[kNetRecurrenceColumn] >= kNetRecurrenceFullWidth ||
            state->net_name_min_width >= state->net_name_width)
        {
            return usual;
        }
        ListLayout narrow = LayOutList(NetListColumns(state->net_name_min_width, state->net_name_width), available,
                                       kScreenListWidthAt80, 2);
        return narrow.widths[kNetRecurrenceColumn] > 0 ? narrow : usual;
    }

    const std::string& NetListHeader(const AppState* state)
    {
        static HeadingCache cache;
        if (HeadingNeedsBuilding(&cache, state->list_width, state->net_name_width * 1000 + state->net_name_min_width))
        {
            cache.text = MenuGutter() + FormatListHeading(NetListColumns(state->net_name_width, state->net_name_width),
                                                          NetListLayout(state));
        }
        return cache.text;
    }

    static std::vector<std::string> NetListCells(const Net& net, bool has_open_session)
    {
        std::string when;
        if (net.imported_at > 0)
        {
            when = "imported " + FormatLocalDate(net.imported_at);
        }
        else if (net.created_at > 0)
        {
            when = "created " + FormatLocalDate(net.created_at);
        }
        if (has_open_session)
        {
            when += when.empty() ? "session open" : ", session open";
        }
        std::vector<std::string> cells;
        cells.reserve(7);
        cells.push_back(net.name);
        cells.push_back(net.mode);
        cells.push_back(net.default_frequency);
        cells.push_back(net.repeater_offset);
        cells.push_back(net.pl_tone);
        cells.push_back(net.recurrence_description);
        cells.push_back(std::move(when));
        return cells;
    }

    static std::vector<std::string> FormatNetList(const AppState* state)
    {
        std::vector<std::string> rows = FormatRows(state->net_cells, NetListLayout(state));
        // A net with nothing after its name is shown as just its name.
        for (std::string& row : rows)
        {
            row.erase(row.find_last_not_of(' ') + 1);
        }
        return rows;
    }

    static void FormatInfoTable(AppState* state);

    // Re-lays out every list (and an open window's table) for
    // AppState::list_width.
    static void RelayOutLists(AppState* state)
    {
        state->net_names = FormatNetList(state);
        state->active_display_rows = FormatCheckInList(state->active_check_in_cells, state->list_width);
        state->history_check_in_labels = FormatCheckInList(state->history_check_in_cells, state->list_width);
        state->history_instance_labels =
            FormatRows(state->history_instance_cells, NetInstanceLayout(state->list_width, state->history_ad_hoc));
        state->edit_net_saved_station_labels =
            FormatRows(state->saved_station_cells, SavedStationLayout(state->list_width));
        state->manage_users_labels = FormatRows(state->manage_users_cells, UserLayout(state->list_width));
        state->user_keys_labels = FormatRows(state->user_keys_cells, UserKeyLayout(state->list_width));
        state->modal_callsign_suggestion_labels = FormatMatches(
            state->modal_callsign_suggestions, state->modal_callsign_suggestion_sources, state->list_width);
        state->saved_station_suggestion_labels =
            FormatMatches(state->saved_station_suggestions, state->saved_station_suggestion_sources, state->list_width);
        FormatInfoTable(state);
    }

    void UpdateListWidths(AppState* state, int terminal_width)
    {
        int width = std::max(80, terminal_width);
        if (width == state->list_width)
        {
            return;
        }
        state->list_width = width;
        RelayOutLists(state);
    }

    // Appends stations from `candidates` onto `suggestions` that aren't
    // already present (by callsign), so tier 2 never duplicates a tier 1 hit.
    // Moves candidates in (they're not used again) and stops once
    // `suggestions` holds `max_suggestions`.
    static void AppendNewSuggestions(std::vector<Station>* suggestions, std::vector<Station>* candidates,
                                     std::size_t max_suggestions)
    {
        for (Station& candidate : *candidates)
        {
            if (suggestions->size() >= max_suggestions)
            {
                return;
            }
            bool already_included = false;
            for (const Station& existing : *suggestions)
            {
                if (existing.callsign == candidate.callsign)
                {
                    already_included = true;
                    break;
                }
            }
            if (!already_included)
            {
                suggestions->push_back(std::move(candidate));
            }
        }
    }

    void RefreshNets(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        std::vector<Net> all_nets = state->db->GetAllNets();
        state->nets.clear();
        state->nets.reserve(all_nets.size());
        for (Net& net : all_nets)
        {
            if (!net.is_ad_hoc)
            {
                state->nets.push_back(std::move(net));
            }
        }
        state->open_net_ids = state->db->GetNetIdsWithOpenInstances();
        const std::vector<std::int64_t>& open_net_ids = state->open_net_ids;

        // The longest name, with room to breathe after it: at least
        // kMinNetNameColumnWidth, and a few columns past the longest name.
        int name_width = 0;
        for (const Net& net : state->nets)
        {
            name_width = std::max(name_width, TextWidth(net.name));
        }
        state->net_name_width =
            std::min(std::max(name_width + kNetNameBreathingRoom, kMinNetNameColumnWidth), kMaxNetNameColumnWidth);
        state->net_name_min_width = std::min(std::max(name_width + kNetNameNarrowRoom, 3), state->net_name_width);

        state->net_cells.clear();
        for (const Net& net : state->nets)
        {
            bool has_open_session = std::find(open_net_ids.begin(), open_net_ids.end(), net.id) != open_net_ids.end();
            state->net_cells.push_back(NetListCells(net, has_open_session));
        }
        state->net_names = FormatNetList(state);

        if (state->selected_net_index >= static_cast<int>(state->nets.size()))
        {
            state->selected_net_index = 0;
        }
    }

    void HighlightLastLoggedNet(AppState* state)
    {
        if (state->settings.callsign.empty())
        {
            return;
        }
        std::optional<std::int64_t> net_id = state->db->GetNetLastStartedBy(state->settings.callsign);
        if (!net_id.has_value())
        {
            return;
        }
        for (std::size_t i = 0; i < state->nets.size(); ++i)
        {
            if (state->nets[i].id == *net_id)
            {
                state->selected_net_index = static_cast<int>(i);
                return;
            }
        }
    }

    bool SelectedNetHasOpenSession(const AppState* state)
    {
        if (state->selected_net_index < 0 || state->selected_net_index >= static_cast<int>(state->nets.size()))
        {
            return false;
        }
        std::int64_t net_id = state->nets[static_cast<std::size_t>(state->selected_net_index)].id;
        return std::find(state->open_net_ids.begin(), state->open_net_ids.end(), net_id) != state->open_net_ids.end();
    }

    // "2026-09-24 03:42 PM", or just the date if no start time was recorded.
    static std::string DescribeSessionStart(const NetInstance& instance)
    {
        std::string when = instance.instance_date;
        std::string start_time = FormatLocalTimeOfDay(instance.started_at);
        if (!start_time.empty())
        {
            when += " " + start_time;
        }
        return when;
    }

    // When a closed session ended: "05:10 PM", with its date in front if
    // that's not the day it started (a net that ran past midnight). Empty
    // while it's open.
    static std::string DescribeSessionEnd(const NetInstance& instance)
    {
        if (instance.closed_at <= 0)
        {
            return "";
        }
        std::string end_date = FormatLocalDate(instance.closed_at);
        std::string end_time = FormatLocalTimeOfDay(instance.closed_at);
        return end_date == instance.instance_date ? end_time : end_date + " " + end_time;
    }

    static std::string CountCheckIns(std::size_t count)
    {
        return std::to_string(count) + (count == 1 ? " check-in" : " check-ins");
    }

    static void ShowConfirmPrompt(AppState* state, ConfirmPrompt prompt, const std::string& title,
                                  const std::vector<std::string>& lines)
    {
        state->confirm_prompt = prompt;
        state->confirm_prompt_title = title;
        state->confirm_prompt_lines = lines;
        state->show_confirm_prompt = true;
        state->form_error.clear();
        state->status_message.clear();
    }

    void CancelConfirmPrompt(AppState* state)
    {
        state->show_confirm_prompt = false;
        state->confirm_prompt = ConfirmPrompt::kNone;
    }

    // Asks whether to resume `session` of AppState::start_net, which is still
    // open, or close it and start a new one (ConfirmPrompt::kResumeNet).
    void ViewOpenNet(AppState* state);

    static void OfferToResume(AppState* state, const NetInstance& session)
    {
        state->resume_instance = session;
        if (state->view_only_user)
        {
            // Watching is all a view-only user can do, so there's nothing
            // to ask.
            ViewOpenNet(state);
            return;
        }
        std::size_t check_ins = CheckInCount(state->db, session.id);
        ShowConfirmPrompt(state, ConfirmPrompt::kResumeNet, "Session Still Open",
                          {state->start_net.name + " has a session that's still open: started " +
                               DescribeSessionStart(session) + ", " + CountCheckIns(check_ins) + ".",
                           "Resume it to keep logging -- if someone else is logging it right now, you'll "
                           "both be adding to the same log. Or close it and start a new session, or just "
                           "view it without changing anything."});
    }

    bool RefuseViewOnly(AppState* state, const std::string& what)
    {
        if (!state->view_only_user)
        {
            return false;
        }
        state->status_message.clear();
        state->form_error = "View-only users can't " + what + ".";
        return true;
    }

    bool CanManageUsers(const AppState* state)
    {
#if defined(QUICKLOGGER_WITH_SSH)
        return state->is_console_session;
#else
        (void)state;
        return false;
#endif
    }

    void StartSelectedNet(AppState* state)
    {
        if (state->nets.empty())
        {
            state->form_error =
                state->view_only_user ? "There are no recurring nets yet." : "Create a recurring net first.";
            return;
        }
        state->start_net = state->nets[state->selected_net_index];
        if (state->view_only_user)
        {
            // Only ever to watch its open session, if it has one.
            ViewStartNet(state);
            if (!state->form_error.empty())
            {
                state->form_error = "No session of " + state->start_net.name + " is open to view.";
            }
            return;
        }

        // Newest first, so this is the most recent session left open.
        std::vector<NetInstance> sessions = state->db->GetNetInstancesForNet(state->start_net.id);
        for (const NetInstance& session : sessions)
        {
            if (session.status == NetInstanceStatus::kOpen)
            {
                OfferToResume(state, session);
                return;
            }
        }

        ResetStartNetFlow(state);
        state->page = kPageSelectRole;
    }

    // "Tailgate Net  started 2026-09-24 06:10 PM, 3 check-ins".
    static std::string FormatOpenAdHocSession(const Net& net, const NetInstance& session, std::size_t check_ins)
    {
        return net.name + "  started " + DescribeSessionStart(session) + ", " + CountCheckIns(check_ins);
    }

    void RefreshOpenAdHocSessions(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        state->open_ad_hoc_sessions.clear();
        state->open_ad_hoc_labels.clear();
        for (const NetInstance& session : state->db->GetAdHocNetInstances())
        {
            if (session.status != NetInstanceStatus::kOpen)
            {
                continue;
            }
            std::optional<Net> net = state->db->GetNetById(session.net_id);
            std::size_t check_ins = CheckInCount(state->db, session.id);
            state->open_ad_hoc_sessions.push_back(session);
            state->open_ad_hoc_labels.push_back(
                FormatOpenAdHocSession(net.has_value() ? *net : Net(), session, check_ins));
        }
        if (state->selected_open_ad_hoc_index >= static_cast<int>(state->open_ad_hoc_sessions.size()))
        {
            state->selected_open_ad_hoc_index = 0;
        }
    }

    void StartAdHocNet(AppState* state)
    {
        if (RefuseViewOnly(state, "start net sessions"))
        {
            return;
        }
        if (state->new_net_name.empty())
        {
            state->form_error = "Net name is required.";
            return;
        }
        if (!CheckNetRadio(state, state->new_net_frequency, state->new_net_offset, &state->new_net_tone) ||
            !CheckNetZip(state, state->new_net_location))
        {
            return;
        }

        Net net;
        net.name = state->new_net_name;
        net.mode = NetModes()[static_cast<std::size_t>(state->new_net_mode_index)];
        net.default_frequency = state->new_net_frequency;
        net.repeater_offset = state->new_net_offset;
        net.pl_tone = state->new_net_tone;
        net.default_location = state->new_net_location;
        net.partial_match_canada = state->new_net_partial_match_index == 1;
        net.created_at = static_cast<std::int64_t>(std::time(nullptr));
        net.is_ad_hoc = true;
        net.id = state->db->CreateNet(net);
        state->start_net = net;

        ResetCreateNetForm(state);
        ResetStartNetFlow(state);
        state->page = kPageSelectRole;
    }

    static void LeaveActiveNet(AppState* state);

    // Joins AppState::resume_instance, to log it or (`viewing`) just watch.
    static void JoinOpenSession(AppState* state, bool viewing)
    {
        CancelConfirmPrompt(state);
        std::optional<NetInstance> session = state->db->GetNetInstanceById(state->resume_instance.id);
        if (!session.has_value() || session->status != NetInstanceStatus::kOpen)
        {
            RefreshNets(state);
            state->form_error = "That session has been closed or deleted in the meantime.";
            return;
        }

        state->active_instance = *session;
        std::optional<Net> net = state->db->GetNetById(session->net_id);
        state->active_net_name = net.has_value() ? net->name : "";
        state->active_net_zip = net.has_value() ? net->default_location : "";
        state->active_net_partial_match_canada = net.has_value() && net->partial_match_canada;
        state->active_net_radio = net.has_value() ? DescribeNetRadio(*net) : "";
        state->active_net_is_ad_hoc = net.has_value() && net->is_ad_hoc;
        // The header shows who started it, in which role.
        state->selected_role_index = session->operator_role;
        if (session->operator_role == kRoleAlternateNetControl)
        {
            state->operator_callsign = session->alternate_net_control_callsign;
        }
        else if (session->operator_role == kRoleLogger)
        {
            state->operator_callsign = session->logger_callsign;
        }
        else
        {
            state->operator_callsign = session->net_control_callsign;
        }
        state->viewing_only = viewing;
        if (viewing)
        {
            state->selected_role_index = kRoleViewer;
            state->operator_callsign = state->settings.callsign;
        }
        state->show_new_station_modal = false;
        state->show_edit_checkin_modal = false;
        state->selected_check_in_index = 0;
        ClearModalFields(state);
        RefreshActiveCheckIns(state);
        state->status_message = viewing ? "Watching the " + DescribeSessionStart(*session) + " session."
                                        : "Resumed the " + DescribeSessionStart(*session) + " session.";
        state->page = kPageActiveNet;
    }

    void ResumeOpenNet(AppState* state)
    {
        // A view-only user only ever watches.
        JoinOpenSession(state, state->view_only_user);
    }

    void ViewOpenNet(AppState* state)
    {
        JoinOpenSession(state, true);
    }

    void ViewStartNet(AppState* state)
    {
        for (const NetInstance& session : state->db->GetNetInstancesForNet(state->start_net.id))
        {
            if (session.status == NetInstanceStatus::kOpen)
            {
                state->resume_instance = session;
                ViewOpenNet(state);
                return;
            }
        }
        state->form_error =
            "No session of " + state->start_net.name + " is open to watch. Choose another role to start one.";
    }

    void StopViewing(AppState* state)
    {
        LeaveActiveNet(state);
        state->viewing_only = false;
        state->form_error.clear();
        state->status_message.clear();
    }

    void CloseOpenNetAndStartNew(AppState* state)
    {
        if (RefuseViewOnly(state, "close or start net sessions"))
        {
            CancelConfirmPrompt(state);
            return;
        }
        CancelConfirmPrompt(state);
        // Nobody closed this session when it ended, so "now" could be days
        // later. Its last check-in is the best record of when it ended (or
        // its start, if nothing was logged).
        std::int64_t ended_at = state->resume_instance.started_at;
        for (const CheckIn& check_in : state->db->GetCheckInsForNetInstance(state->resume_instance.id))
        {
            ended_at = std::max(ended_at, check_in.checked_in_at);
        }
        if (ended_at <= 0)
        {
            ended_at = static_cast<std::int64_t>(std::time(nullptr));
        }
        state->db->CloseNetInstance(state->resume_instance.id, ended_at);
        RefreshNets(state);
        ResetStartNetFlow(state);
        state->page = kPageSelectRole;
    }

    // Where the active net's history is: ad hoc nets aren't on the net list.
    static const char* HistoryKeyDescription(const AppState* state)
    {
        return state->active_net_is_ad_hoc ? "F6 on the Ad Hoc Net page" : "F6 on the net list";
    }

    void RequestCloseActiveNet(AppState* state)
    {
        if (RefuseViewOnly(state, "close net sessions"))
        {
            return;
        }
        std::string name = state->active_net_name.empty() ? "this net" : state->active_net_name;
        ShowConfirmPrompt(state, ConfirmPrompt::kCloseNet, "Close Net",
                          {"Close " + name + " (" + CountCheckIns(state->active_check_ins.size()) + ")?",
                           std::string("It moves to History (") + HistoryKeyDescription(state) +
                               "), where you can still view and export it. A closed session can't be "
                               "reopened for logging."});
    }

    // Leaves the active net for the net list, closing any dialog on it.
    static void LeaveActiveNet(AppState* state)
    {
        state->watched_instance_id = 0;
        state->active_check_ins.clear();
        state->active_check_in_cells.clear();
        state->active_display_rows.clear();
        state->show_new_station_modal = false;
        state->show_edit_checkin_modal = false;
        ClearModalFields(state);
        ResetStartNetFlow(state);
        RefreshNets(state);
        state->page = kPageNetList;
    }

    void CloseActiveNet(AppState* state)
    {
        CancelConfirmPrompt(state);
        if (RefuseViewOnly(state, "close net sessions"))
        {
            return;
        }
        std::string closed_name = state->active_net_name;
        bool closed_here =
            state->db->CloseNetInstance(state->active_instance.id, static_cast<std::int64_t>(std::time(nullptr)));
        std::size_t check_ins = CheckInCount(state->db, state->active_instance.id);
        std::string history = HistoryKeyDescription(state);
        if (!closed_here)
        {
            // Someone sharing the session closed (or deleted) it first.
            // (Their end time is the one that stands -- see CloseNetInstance.)
            ShowSessionClosedPrompt(state, "");
            return;
        }
        LeaveActiveNet(state);
        state->form_error.clear();
        state->status_message =
            "Closed " + closed_name + " (" + CountCheckIns(check_ins) + "). It's in History (" + history + ").";
    }

    bool EnsureActiveSessionOpen(AppState* state, const std::string& unlogged_callsign)
    {
        std::optional<NetInstance> session = state->db->GetNetInstanceById(state->active_instance.id);
        if (session.has_value() && session->status == NetInstanceStatus::kOpen)
        {
            return true;
        }
        ShowSessionClosedPrompt(state, unlogged_callsign);
        return false;
    }

    void ShowSessionClosedPrompt(AppState* state, const std::string& unlogged_callsign)
    {
        std::optional<NetInstance> session = state->db->GetNetInstanceById(state->active_instance.id);
        std::string unlogged = unlogged_callsign;
        if (unlogged.empty() && state->show_new_station_modal)
        {
            unlogged = state->modal_station.callsign;
        }

        // Nothing more can happen here: close whatever is open over the
        // page, and stop watching the session for changes.
        state->watched_instance_id = 0;
        state->show_new_station_modal = false;
        state->show_edit_checkin_modal = false;
        ClearModalFields(state);
        if (state->info_window != InfoWindow::kNone)
        {
            CloseInfoWindow(state);
        }
        if (state->show_row_delete_confirm_modal)
        {
            CancelRowDelete(state);
        }
        if (state->row_pick_action != RowPickAction::kNone)
        {
            CancelRowPick(state);
        }
        CancelConfirmPrompt(state);

        std::vector<std::string> lines;
        if (session.has_value())
        {
            std::string ended = DescribeSessionEnd(*session);
            lines.push_back("Another user has closed this net" + (ended.empty() ? std::string() : " at " + ended) +
                            ".");
        }
        else
        {
            lines.push_back("Another user has deleted this net's session.");
        }
        if (!unlogged.empty())
        {
            lines.push_back(unlogged + " was not logged.");
        }
        lines.push_back("You will be returned to the Recurring Nets list when you press Enter.");
        ShowConfirmPrompt(state, ConfirmPrompt::kSessionClosed, session.has_value() ? "Net Closed" : "Session Deleted",
                          lines);
    }

    void LeaveClosedSession(AppState* state)
    {
        CancelConfirmPrompt(state);
        LeaveActiveNet(state);
        state->viewing_only = false;
        state->form_error.clear();
        state->status_message.clear();
    }

    void OpenSettingsForm(AppState* state)
    {
        state->settings_form = state->settings;
        state->settings_time_format_index = state->settings.use_24_hour_clock ? 1 : 0;
        state->settings_update_check_index = state->settings.check_for_updates ? 0 : 1;
        state->settings_radius_text = std::to_string(state->settings.nearby_radius_miles);
    }

    bool SaveSettingsForm(AppState* state)
    {
        if (!state->callsign_editable)
        {
            state->settings_form.callsign = state->ssh_username;
        }
        if (state->settings_form.callsign.empty())
        {
            state->form_error = "Your callsign is required.";
            return false;
        }
        if (!CheckCallsign(state, state->settings_form.callsign))
        {
            return false;
        }
        if (state->settings_form.location.size() != 5)
        {
            state->form_error = "Your ZIP code is required and must be 5 digits.";
            return false;
        }

        // A blank field means the default, which its placeholder shows.
        int radius = kDefaultNearbyRadiusMiles;
        if (!state->settings_radius_text.empty())
        {
            radius = state->settings_radius_text.size() > 3 ? 0 : std::stoi(state->settings_radius_text);
        }
        if (radius < kMinNearbyRadiusMiles || radius > kMaxNearbyRadiusMiles)
        {
            state->form_error = "Nearby Radius must be " + std::to_string(kMinNearbyRadiusMiles) + " to " +
                                std::to_string(kMaxNearbyRadiusMiles) + " miles.";
            return false;
        }

        state->settings_form.use_24_hour_clock = state->settings_time_format_index == 1;
        state->settings_form.check_for_updates = state->settings_update_check_index == 0;
        state->settings_form.nearby_radius_miles = radius;
        SaveSettings(state->settings_path, state->settings_form);
        state->settings = state->settings_form;
        SetUse24HourClock(state->settings.use_24_hour_clock);
        SetUpdateCheckEnabled(state->is_console_session && state->settings.check_for_updates);
        state->form_error.clear();
        return true;
    }

    bool CheckCallsign(AppState* state, const std::string& callsign)
    {
        if (!IsValidCallsign(callsign))
        {
            state->form_error = callsign + " isn't a valid US or Canadian call sign.";
            return false;
        }
        return true;
    }

    std::string DescribeNetRadio(const Net& net)
    {
        std::string text;
        if (!net.default_frequency.empty())
        {
            text = net.default_frequency + " MHz";
        }
        if (!net.repeater_offset.empty())
        {
            text += (text.empty() ? "Offset " : "  ") + net.repeater_offset;
        }
        if (!net.pl_tone.empty())
        {
            text += (text.empty() ? "" : "  ") + std::string("PL ") + net.pl_tone;
        }
        return text;
    }

    bool CheckNetRadio(AppState* state, const std::string& frequency, const std::string& offset, std::string* tone)
    {
        std::string problem = FrequencyProblem(frequency);
        if (problem.empty())
        {
            problem = OffsetProblem(offset, frequency);
        }
        if (problem.empty())
        {
            problem = ToneProblem(*tone);
        }
        if (!problem.empty())
        {
            state->form_error = problem;
            return false;
        }
        *tone = NormalizeTone(*tone);
        return true;
    }

    bool CheckNetZip(AppState* state, const std::string& zip)
    {
        if (!zip.empty() && !IsFiveDigitZip(zip))
        {
            state->form_error = "ZIP code must be 5 digits, or left blank.";
            return false;
        }
        return true;
    }

    std::string ExistingNetNamed(AppState* state, const std::string& name, std::int64_t except_net_id)
    {
        for (const Net& net : state->db->GetAllNets())
        {
            if (!net.is_ad_hoc && net.id != except_net_id && NetNamesAreTheSame(net.name, name))
            {
                return net.name;
            }
        }
        return "";
    }

    void ResetCreateNetForm(AppState* state)
    {
        state->new_net_name.clear();
        state->new_net_mode_index = 0;
        state->new_net_frequency.clear();
        state->new_net_offset.clear();
        state->new_net_tone.clear();
        state->new_net_location.clear();
        state->new_net_recurrence.clear();
        state->new_net_comments.clear();
        state->new_net_partial_match_index = 0;
        state->form_error.clear();
    }

    void ResetStartNetFlow(AppState* state)
    {
        state->selected_role_index = kRoleNetControl;
        // Prefill with the operator's own callsign from Settings; they can
        // still edit it if a different person is filling this particular role.
        state->operator_callsign = state->settings.callsign;
        state->form_error.clear();
    }

    void RefreshActiveCheckIns(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        state->active_check_ins = state->db->GetCheckInsForNetInstance(state->active_instance.id);
        state->active_check_in_cells = CheckInCells(state->db, state->active_check_ins);
        state->active_display_rows = FormatCheckInList(state->active_check_in_cells, state->list_width);

        if (state->selected_check_in_index >= static_cast<int>(state->active_check_ins.size()))
        {
            state->selected_check_in_index = 0;
        }

        std::int64_t newest_id = 0;
        for (const CheckIn& check_in : state->active_check_ins)
        {
            newest_id = std::max(newest_id, check_in.id);
        }
        state->shown_check_in_count = static_cast<std::int64_t>(state->active_check_ins.size());
        state->shown_newest_check_in_id = newest_id;
        state->watched_instance_id = state->active_instance.id;
    }

    void RefreshActiveCheckInsFromOthers(AppState* state, std::int64_t instance_id)
    {
        if (state->page != kPageActiveNet || state->active_instance.id != instance_id ||
            state->row_pick_action != RowPickAction::kNone || state->show_row_delete_confirm_modal ||
            state->show_edit_checkin_modal)
        {
            return;
        }
        RefreshActiveCheckIns(state);
        if (state->screen != nullptr)
        {
            state->screen->PostEvent(ftxui::Event::Custom);
        }
    }

    void LogOperatorCheckIn(AppState* state)
    {
        if (RefuseViewOnly(state, "log check-ins"))
        {
            return;
        }
        Station operator_station;
        operator_station.callsign = state->operator_callsign;

        std::optional<Station> known = state->db->FindStationByCallsign(state->operator_callsign);
        if (known.has_value())
        {
            operator_station = *known;
        }
        else
        {
            std::optional<Station> uls = state->db->FindLicensedStationByCallsign(state->operator_callsign);
            if (uls.has_value())
            {
                operator_station = *uls;
            }
        }
        operator_station.callsign = state->operator_callsign;
        BackfillCountyFromZip(state, &operator_station);

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        // SaveNetStation (rather than a bare RecordManualCheckInStation) so
        // logging a check-in also associates the station with this net --
        // otherwise it only ever showed up in future autocomplete for this
        // net (SearchNetStationsByCallsignSubstring also matches on real
        // check-in history), never in the edit-net page's saved-station
        // list or its export, which both query net_saved_stations only.
        std::string existing_remarks =
            state->db->GetSavedNetStationRemarks(state->active_instance.net_id, operator_station.callsign);
        state->db->SaveNetStation(state->active_instance.net_id, operator_station, existing_remarks, now);

        CheckIn check_in;
        check_in.net_instance_id = state->active_instance.id;
        check_in.callsign = state->operator_callsign;
        check_in.sequence_number = 1;
        check_in.checked_in_at = now;
        check_in.designated_role = state->selected_role_index;
        state->db->AddCheckIn(check_in);

        RefreshActiveCheckIns(state);
    }

    void RemoveSelectedCheckIn(AppState* state)
    {
        if (RefuseViewOnly(state, "delete check-ins"))
        {
            return;
        }
        if (state->active_check_ins.empty())
        {
            state->form_error = "No check-ins to remove.";
            return;
        }

        const CheckIn& selected = state->active_check_ins[state->selected_check_in_index];
        if (selected.designated_role != kRoleNone)
        {
            state->db->SetNetInstanceRoleCallsign(state->active_instance.id, selected.designated_role, "");
            std::optional<NetInstance> refreshed = state->db->GetNetInstanceById(state->active_instance.id);
            if (refreshed.has_value())
            {
                state->active_instance = *refreshed;
            }
        }
        state->db->DeleteCheckIn(selected.id);
        state->form_error.clear();
        RefreshActiveCheckIns(state);
    }

    // The two roles besides `operator_role`, in canonical order -- always
    // exactly two, since the operator claims exactly one of the three.
    static std::vector<int> AssignableCheckInRoles(int operator_role)
    {
        std::vector<int> roles;
        if (operator_role != kRoleNetControl)
        {
            roles.push_back(kRoleNetControl);
        }
        if (operator_role != kRoleAlternateNetControl)
        {
            roles.push_back(kRoleAlternateNetControl);
        }
        if (operator_role != kRoleLogger)
        {
            roles.push_back(kRoleLogger);
        }
        return roles;
    }

    std::vector<std::string> RoleChoiceLabels(const AppState* state)
    {
        std::vector<std::string> labels{"No additional role"};
        for (int role : AssignableCheckInRoles(state->active_instance.operator_role))
        {
            labels.push_back(state->role_short_labels[role]);
        }
        return labels;
    }

    int RoleChoiceIndexFromRole(const AppState* state, int role)
    {
        if (role == kRoleNone)
        {
            return 0;
        }
        std::vector<int> roles = AssignableCheckInRoles(state->active_instance.operator_role);
        for (std::size_t i = 0; i < roles.size(); ++i)
        {
            if (roles[i] == role)
            {
                return static_cast<int>(i) + 1;
            }
        }
        return 0;
    }

    int RoleFromRoleChoiceIndex(const AppState* state, int index)
    {
        if (index <= 0)
        {
            return kRoleNone;
        }
        std::vector<int> roles = AssignableCheckInRoles(state->active_instance.operator_role);
        std::size_t choice = static_cast<std::size_t>(index - 1);
        return choice < roles.size() ? roles[choice] : kRoleNone;
    }

    void ApplyCheckInRoleDesignation(AppState* state, std::int64_t check_in_id, int old_role, int new_role,
                                     const std::string& callsign)
    {
        if (old_role == new_role || state->view_only_user)
        {
            return;
        }
        if (new_role != kRoleNone && new_role == state->active_instance.operator_role)
        {
            return;
        }

        if (old_role != kRoleNone)
        {
            state->db->SetNetInstanceRoleCallsign(state->active_instance.id, old_role, "");
        }
        if (new_role != kRoleNone)
        {
            // Only one check-in per instance can hold a given role -- handing
            // it to this one silently takes it away from whichever check-in
            // held it before.
            state->db->ClearCheckInRoleForInstance(state->active_instance.id, new_role, check_in_id);
            state->db->SetNetInstanceRoleCallsign(state->active_instance.id, new_role, callsign);
        }

        std::optional<NetInstance> refreshed = state->db->GetNetInstanceById(state->active_instance.id);
        if (refreshed.has_value())
        {
            state->active_instance = *refreshed;
        }
    }

    void ClearModalFields(AppState* state)
    {
        state->modal_station = Station();
        state->modal_signal_report.clear();
        state->modal_remarks.clear();
        state->modal_comment.clear();
        state->modal_callsign_suggestions.clear();
        state->modal_callsign_suggestion_labels.clear();
        state->modal_callsign_suggestion_sources.clear();
        state->selected_suggestion_index = 0;
        state->modal_role_choice_labels = RoleChoiceLabels(state);
        state->modal_role_choice_index = 0;
        state->form_error.clear();
    }

    // `*to` gets `from` if it's blank.
    static void FillIfBlank(std::string* to, const std::string& from)
    {
        if (to->empty())
        {
            *to = from;
        }
    }

    // Fills `station`'s blank fields from what's known about its callsign,
    // exactly or, failing that, without a portable indicator: a station
    // known to some net, or else the FCC data at any distance (or, for a
    // Canadian call sign, ISED's). Returns
    // whether the station was found.
    static bool FillStationFromKnown(AppState* state, Station* station)
    {
        std::string callsign = NormalizeCallsign(station->callsign);
        if (callsign.empty())
        {
            return false;
        }
        std::optional<Station> known = state->db->FindStationByCallsign(callsign);
        if (!known.has_value())
        {
            known = state->db->FindLicensedStationByCallsign(callsign);
        }
        std::string base = BaseCallsign(callsign);
        if (!known.has_value() && base != callsign)
        {
            known = state->db->FindStationByCallsign(base);
            if (!known.has_value())
            {
                known = state->db->FindLicensedStationByCallsign(base);
            }
        }
        if (!known.has_value())
        {
            return false;
        }
        FillIfBlank(&station->name, known->name);
        FillIfBlank(&station->member_id, known->member_id);
        FillIfBlank(&station->street_address, known->street_address);
        FillIfBlank(&station->city, known->city);
        FillIfBlank(&station->county, known->county);
        FillIfBlank(&station->state, known->state);
        FillIfBlank(&station->zip, known->zip);
        FillIfBlank(&station->grid_square, known->grid_square);
        FillIfBlank(&station->license_class, known->license_class);
        FillIfBlank(&station->email, known->email);
        BackfillCountyFromZip(state, station);
        BackfillGridFromZip(state, station);
        return true;
    }

    bool FillCheckInFromKnownStation(AppState* state)
    {
        if (!FillStationFromKnown(state, &state->modal_station))
        {
            return false;
        }
        FillIfBlank(&state->modal_remarks,
                    state->db->GetSavedNetStationRemarks(state->active_instance.net_id,
                                                         NormalizeCallsign(state->modal_station.callsign)));
        return true;
    }

    bool FillSavedStationFromKnownStation(AppState* state)
    {
        if (!FillStationFromKnown(state, &state->saved_station))
        {
            return false;
        }
        FillIfBlank(
            &state->saved_station_remarks,
            state->db->GetSavedNetStationRemarks(state->edit_net_id, NormalizeCallsign(state->saved_station.callsign)));
        return true;
    }

    bool LogStationCheckIn(AppState* state)
    {
        if (RefuseViewOnly(state, "log check-ins"))
        {
            return false;
        }
        state->modal_station.callsign = NormalizeCallsign(state->modal_station.callsign);
        if (state->modal_station.callsign.empty())
        {
            state->form_error = "Callsign is required.";
            return false;
        }
        if (!CheckCallsign(state, state->modal_station.callsign))
        {
            return false;
        }
        // What's known about it, even if it wasn't picked from the matches.
        FillCheckInFromKnownStation(state);
        if (!EnsureActiveSessionOpen(state, state->modal_station.callsign))
        {
            return false;
        }
        // Once per session, counting W4KWK/M as W4KWK. Read fresh: someone
        // else sharing the session may have logged it.
        std::string base = BaseCallsign(state->modal_station.callsign);
        for (const CheckIn& existing : state->db->GetCheckInsForNetInstance(state->active_instance.id))
        {
            if (BaseCallsign(existing.callsign) == base)
            {
                state->form_error =
                    state->modal_station.callsign + " is already in this session's log, as " +
                    (existing.callsign == state->modal_station.callsign ? std::string() : existing.callsign + " ") +
                    "#" + std::to_string(existing.sequence_number) + ".";
                return false;
            }
        }

        BackfillCountyFromZip(state, &state->modal_station);
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        // SaveNetStation (rather than a bare RecordManualCheckInStation) so
        // logging a check-in also associates the station with this net, the
        // same reasoning as LogOperatorCheckIn above -- and this check-in's
        // own remarks become the saved station's new default remarks,
        // prefilled next time it's picked via autocomplete for this net.
        state->db->SaveNetStation(state->active_instance.net_id, state->modal_station, state->modal_remarks, now);

        CheckIn check_in;
        check_in.net_instance_id = state->active_instance.id;
        check_in.callsign = state->modal_station.callsign;
        check_in.signal_report = state->modal_signal_report;
        check_in.remarks = state->modal_remarks;
        check_in.comment = state->modal_comment;
        check_in.checked_in_at = now;
        check_in.designated_role = RoleFromRoleChoiceIndex(state, state->modal_role_choice_index);
        // Numbered one past the highest so far (not the count: after a
        // delete the numbers have a gap), worked out by the database so two
        // people logging this same session (see ResumeOpenNet) can't collide.
        check_in.id = state->db->AddCheckInAtNextSequence(check_in);

        if (check_in.designated_role != kRoleNone)
        {
            ApplyCheckInRoleDesignation(state, check_in.id, kRoleNone, check_in.designated_role, check_in.callsign);
        }

        RefreshActiveCheckIns(state);
        state->form_error.clear();
        return true;
    }

    void OpenEditCheckInForm(AppState* state, const CheckIn& check_in)
    {
        if (RefuseViewOnly(state, "edit check-ins"))
        {
            return;
        }
        state->edit_checkin_original = check_in;

        std::optional<Station> station = state->db->FindStationByCallsign(check_in.callsign);
        state->edit_checkin_station = station.has_value() ? *station : Station();
        state->edit_checkin_station.callsign = check_in.callsign;
        state->edit_checkin_signal_report = check_in.signal_report;
        state->edit_checkin_remarks = check_in.remarks;
        state->edit_checkin_comment = check_in.comment;
        state->edit_checkin_role_choice_labels = RoleChoiceLabels(state);
        state->edit_checkin_role_choice_index = RoleChoiceIndexFromRole(state, check_in.designated_role);

        state->form_error.clear();
        state->show_edit_checkin_modal = true;
    }

    void SaveEditCheckInForm(AppState* state)
    {
        if (RefuseViewOnly(state, "edit check-ins"))
        {
            return;
        }
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        state->edit_checkin_station.callsign = state->edit_checkin_original.callsign;
        state->db->UpdateStationFields(state->edit_checkin_station, now);

        int old_role = state->edit_checkin_original.designated_role;
        int new_role = RoleFromRoleChoiceIndex(state, state->edit_checkin_role_choice_index);
        // The operator's own check-in holds the role they started the net
        // in, which isn't among the choices the form offers (see
        // RoleChoiceLabels) -- so the form reads "No additional role" for it,
        // and saving that would strip the operator's role from the net.
        if (old_role != kRoleNone && old_role == state->active_instance.operator_role)
        {
            new_role = old_role;
        }

        CheckIn check_in = state->edit_checkin_original;
        check_in.signal_report = state->edit_checkin_signal_report;
        check_in.remarks = state->edit_checkin_remarks;
        check_in.comment = state->edit_checkin_comment;
        check_in.designated_role = new_role;
        state->db->UpdateCheckIn(check_in);
        ApplyCheckInRoleDesignation(state, check_in.id, old_role, new_role, check_in.callsign);

        RefreshActiveCheckIns(state);
    }

    void RefreshNetHistory(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        state->history_instances.clear();
        state->history_instance_cells.clear();
        state->history_instance_labels.clear();

        std::unordered_map<std::int64_t, std::string> names;
        if (state->history_ad_hoc)
        {
            for (const Net& net : state->db->GetAllNets())
            {
                names[net.id] = net.name;
            }
            state->history_instances = state->db->GetAdHocNetInstances();
        }
        else
        {
            if (state->selected_net_index >= static_cast<int>(state->nets.size()))
            {
                return;
            }
            state->history_instances = state->db->GetNetInstancesForNet(state->nets[state->selected_net_index].id);
        }
        for (const NetInstance& instance : state->history_instances)
        {
            std::int64_t check_ins = 0;
            std::int64_t newest_id = 0;
            state->db->GetCheckInSummary(instance.id, &check_ins, &newest_id);
            state->history_instance_cells.push_back(
                NetInstanceCells(instance, names[instance.net_id], check_ins, state->history_ad_hoc));
        }
        state->history_instance_labels =
            FormatRows(state->history_instance_cells, NetInstanceLayout(state->list_width, state->history_ad_hoc));

        if (state->selected_history_index >= static_cast<int>(state->history_instances.size()))
        {
            state->selected_history_index = 0;
        }

        RefreshHistoryCheckIns(state);
    }

    void RefreshHistoryCheckIns(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        state->history_check_in_labels.clear();
        state->history_check_in_cells.clear();
        state->history_check_ins.clear();
        state->selected_history_check_in_index = 0;

        if (state->selected_history_index >= static_cast<int>(state->history_instances.size()))
        {
            return;
        }

        const NetInstance& selected = state->history_instances[state->selected_history_index];
        state->history_check_ins = state->db->GetCheckInsForNetInstance(selected.id);
        state->history_check_in_cells = CheckInCells(state->db, state->history_check_ins);
        state->history_check_in_labels = FormatCheckInList(state->history_check_in_cells, state->list_width);
    }

    void DeleteSelectedNetInstance(AppState* state)
    {
        if (RefuseViewOnly(state, "delete net sessions"))
        {
            return;
        }
        if (state->history_instances.empty())
        {
            state->form_error = "No net instance to delete.";
            return;
        }

        const NetInstance& selected = state->history_instances[state->selected_history_index];
        if (selected.status == NetInstanceStatus::kOpen)
        {
            state->form_error =
                "That session is still open. Resume it (F3 on the net list), close it with F4, "
                "then delete it.";
            return;
        }

        state->db->DeleteNetInstance(selected.id);
        state->form_error.clear();
        RefreshNetHistory(state);
    }

    // Writes `lines` to `path`, then -- only if that succeeded -- also tries
    // to hand the same file to a remote, SSH'd-in user via ZMODEM (see
    // zmodem_send.hpp), since a plain local write alone leaves them with no
    // way to get the file off the machine QuickLogger runs on. Sets
    // AppState::status_message/form_error to describe whichever of the two
    // steps is the more relevant outcome to report: a failed local write is
    // still always an error; ZMODEM being unavailable or timing out is a
    // secondary note on an otherwise-successful export, not a failure of
    // the export itself, so it's folded into status_message rather than
    // form_error.
    // "a", "a and b", "a, b and c".
    static std::string ListPaths(const std::vector<std::string>& paths)
    {
        std::string text;
        for (std::size_t i = 0; i < paths.size(); ++i)
        {
            if (i > 0)
            {
                text += i + 1 == paths.size() ? " and " : ", ";
            }
            text += paths[i];
        }
        return text;
    }

    void OfferZmodemSend(AppState* state, const std::string& path)
    {
        OfferZmodemSendFiles(state, {path});
    }

    void OfferZmodemSendFiles(AppState* state, const std::vector<std::string>& paths)
    {
        state->zmodem_zip_contents.clear();
        // The files are already on this computer: nothing to send, but the
        // folder can be opened for the operator.
        if (IsLocalTerminal(state->is_console_session))
        {
            state->status_message = "Saved to " + ListPaths(paths) + ".";
            if (CanShowInFileManager())
            {
                state->zmodem_action = ZmodemAction::kShowFolder;
                state->zmodem_send_paths = paths;
                state->show_zmodem_confirm_modal = true;
            }
            return;
        }
        // No ZMODEM on this system at all (Windows, Alpine), so there's
        // nothing to install.
        if (NoZmodemOnThisSystem())
        {
            state->status_message = "Saved to " + ListPaths(paths) + ".";
            return;
        }
        if (!ZmodemSendAvailable())
        {
            state->status_message = "Saved to " + ListPaths(paths) + " (install 'sz'/lrzsz for ZMODEM download).";
            return;
        }

        // Don't send yet -- the transfer hijacks the real terminal for its
        // raw protocol bytes and can't show anything meaningful while it's
        // in flight, so the operator needs a chance to get their client's
        // receive dialog ready (or back out) *before* that happens, not be
        // dropped into it with no warning. ConfirmZmodemAction/
        // CancelZmodemAction (wired to the modal's F2/Enter and Esc) do the
        // actual send.
        state->status_message = "Saved to " + ListPaths(paths) + ".";
        state->zmodem_action = ZmodemAction::kSend;
        state->zmodem_send_paths = paths;
        state->show_zmodem_confirm_modal = true;
    }

    // Writes an export's lines to `path`; on failure, says why in
    // form_error and returns false.
    static bool WriteExportLines(AppState* state, const std::string& path, std::vector<std::string> lines)
    {
        // Rows whose last columns are empty would otherwise end in padding.
        for (std::string& line : lines)
        {
            line.erase(line.find_last_not_of(' ') + 1);
        }
        std::string error;
        if (!WriteExportFile(path, lines, &error))
        {
            state->status_message.clear();
            state->form_error = error;
            return false;
        }
        state->form_error.clear();
        return true;
    }

    static void ExportLinesToFile(AppState* state, const std::string& path, std::vector<std::string> lines)
    {
        if (WriteExportLines(state, path, std::move(lines)))
        {
            OfferZmodemSend(state, path);
        }
    }

    // A path's file name alone.
    static std::string BaseFileName(const std::string& path)
    {
        std::string::size_type slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // Once ZMODEM is done with a session export's .zip, removes it and
    // returns the files that stay (see AppState::zmodem_zip_contents);
    // otherwise the files that were offered.
    static std::vector<std::string> SavedAfterZmodem(AppState* state)
    {
        if (state->zmodem_zip_contents.empty())
        {
            return state->zmodem_send_paths;
        }
        std::error_code ignored;
        for (const std::string& path : state->zmodem_send_paths)
        {
            std::filesystem::remove(path, ignored);
        }
        std::vector<std::string> saved = std::move(state->zmodem_zip_contents);
        state->zmodem_zip_contents.clear();
        return saved;
    }

    void ConfirmZmodemAction(AppState* state)
    {
        state->show_zmodem_confirm_modal = false;
        std::string error;

        if (state->zmodem_action == ZmodemAction::kShowFolder)
        {
            if (!ShowInFileManager(state->zmodem_send_paths, &error))
            {
                state->status_message = "Saved to " + ListPaths(state->zmodem_send_paths) + " (" + error + ").";
            }
            return;
        }
        if (state->zmodem_action == ZmodemAction::kSend)
        {
            bool sent = SendFilesViaZmodem(state->screen, state->zmodem_send_paths, &error);
            // What's left in exports/ to name: without the .zip, once it's
            // removed.
            std::vector<std::string> saved = SavedAfterZmodem(state);
            if (sent)
            {
                state->status_message = state->zmodem_send_paths.size() == 1 && saved != state->zmodem_send_paths
                                            ? "Sent " + BaseFileName(state->zmodem_send_paths[0]) +
                                                  " via ZMODEM; saved to " + ListPaths(saved) + "."
                                            : "Saved to " + ListPaths(saved) + " and sent via ZMODEM.";
            }
            else
            {
                state->status_message = "Saved to " + ListPaths(saved) + " (" + error + ")";
            }
            return;
        }

        // Receiving is only ever for importing, which a view-only user
        // can't do (StartZmodemReceive refuses too).
        if (RefuseViewOnly(state, "import files"))
        {
            return;
        }
        if (ReceiveFileViaZmodem(state->screen, SessionImportsDir(state->db_path, state->ssh_username), &error))
        {
            RefreshImportNetFiles(state);
            state->form_error.clear();
            state->status_message = "Received a file. Highlight it below and press F2 to import.";
        }
        else
        {
            state->status_message.clear();
            state->form_error = error;
        }
    }

    void CancelZmodemAction(AppState* state)
    {
        state->show_zmodem_confirm_modal = false;
        if (state->zmodem_action == ZmodemAction::kShowFolder)
        {
            return;
        }
        if (state->zmodem_action == ZmodemAction::kSend)
        {
            state->status_message = "Saved to " + ListPaths(SavedAfterZmodem(state)) + " (ZMODEM skipped).";
        }
        else
        {
            state->status_message = "ZMODEM receive skipped.";
        }
    }

    void RequestDeleteNet(AppState* state)
    {
        if (RefuseViewOnly(state, "delete nets"))
        {
            return;
        }
        state->show_delete_net_confirm_modal = true;
    }

    void ConfirmDeleteNet(AppState* state)
    {
        state->show_delete_net_confirm_modal = false;
        if (RefuseViewOnly(state, "delete nets"))
        {
            return;
        }
        std::string deleted_name = state->edit_net_name;
        state->db->DeleteNetCompletely(state->edit_net_id);
        RefreshNets(state);
        state->form_error.clear();
        state->status_message = "Deleted \"" + deleted_name + "\" and all of its history.";
        state->page = kPageNetList;
    }

    void CancelDeleteNet(AppState* state)
    {
        state->show_delete_net_confirm_modal = false;
    }

    // ---- Picking a row by number -------------------------------------------

    PickList RowPickListFor(RowPickAction action)
    {
        switch (action)
        {
            case RowPickAction::kEditNet:
                return PickList::kNets;
            case RowPickAction::kEditCheckIn:
            case RowPickAction::kDeleteCheckIn:
            case RowPickAction::kViewStationHistory:
            case RowPickAction::kViewStationCard:
                return PickList::kActiveCheckIns;
            case RowPickAction::kEditSavedStation:
            case RowPickAction::kRemoveSavedStation:
                return PickList::kSavedStations;
            case RowPickAction::kDeleteNetInstance:
                return PickList::kNetInstances;
            case RowPickAction::kDeleteHistoryCheckIn:
                return PickList::kHistoryCheckIns;
            case RowPickAction::kRemoveUser:
            case RowPickAction::kEditUser:
                return PickList::kUsers;
            case RowPickAction::kRemoveUserKey:
                return PickList::kUserKeys;
            case RowPickAction::kResumeAdHocSession:
            case RowPickAction::kViewAdHocSession:
                return PickList::kOpenAdHocSessions;
            case RowPickAction::kNone:
                break;
        }
        return PickList::kNone;
    }

    // Whether picking a row for `action` goes on to change something (not
    // for a view-only user: see RefuseViewOnly). Resuming an ad hoc
    // session doesn't count: a view-only user resumes it to watch.
    static bool RowPickChangesSomething(RowPickAction action)
    {
        switch (action)
        {
            case RowPickAction::kResumeAdHocSession:
            case RowPickAction::kViewAdHocSession:
            case RowPickAction::kViewStationHistory:
            case RowPickAction::kViewStationCard:
            case RowPickAction::kNone:
                return false;
            case RowPickAction::kEditUser:
            case RowPickAction::kRemoveUserKey:
            case RowPickAction::kEditNet:
            case RowPickAction::kEditCheckIn:
            case RowPickAction::kDeleteCheckIn:
            case RowPickAction::kEditSavedStation:
            case RowPickAction::kRemoveSavedStation:
            case RowPickAction::kDeleteNetInstance:
            case RowPickAction::kDeleteHistoryCheckIn:
            case RowPickAction::kRemoveUser:
                break;
        }
        return true;
    }

    static std::size_t PickListSize(const AppState* state, PickList list)
    {
        switch (list)
        {
            case PickList::kNets:
                return state->nets.size();
            case PickList::kActiveCheckIns:
                return state->active_check_ins.size();
            case PickList::kSavedStations:
                return state->edit_net_saved_stations.size();
            case PickList::kNetInstances:
                return state->history_instances.size();
            case PickList::kHistoryCheckIns:
                return state->history_check_ins.size();
            case PickList::kUsers:
                return state->manage_user_names.size();
            case PickList::kUserKeys:
                return state->user_keys.size();
            case PickList::kOpenAdHocSessions:
                return state->open_ad_hoc_sessions.size();
            case PickList::kNone:
                break;
        }
        return 0;
    }

    // The variable holding the list's highlighted row -- set to the picked
    // row, so the existing "act on the selected row" functions do the work.
    static int* PickListSelection(AppState* state, PickList list)
    {
        switch (list)
        {
            case PickList::kNets:
                return &state->selected_net_index;
            case PickList::kActiveCheckIns:
                return &state->selected_check_in_index;
            case PickList::kSavedStations:
                return &state->selected_saved_station_index;
            case PickList::kNetInstances:
                return &state->selected_history_index;
            case PickList::kHistoryCheckIns:
                return &state->selected_history_check_in_index;
            case PickList::kUsers:
                return &state->selected_user_index;
            case PickList::kUserKeys:
                return &state->selected_user_key_index;
            case PickList::kOpenAdHocSessions:
                return &state->selected_open_ad_hoc_index;
            case PickList::kNone:
                break;
        }
        return nullptr;
    }

    // The nouns used in prompts and messages.
    static const char* PickListNoun(PickList list)
    {
        switch (list)
        {
            case PickList::kNets:
                return "net";
            case PickList::kActiveCheckIns:
                return "check-in";
            case PickList::kSavedStations:
                return "station";
            case PickList::kNetInstances:
                return "net session";
            case PickList::kHistoryCheckIns:
                return "check-in";
            case PickList::kUsers:
                return "user";
            case PickList::kUserKeys:
                return "key";
            case PickList::kOpenAdHocSessions:
                return "open session";
            case PickList::kNone:
                break;
        }
        return "row";
    }

    static const char* RowPickVerb(RowPickAction action)
    {
        switch (action)
        {
            case RowPickAction::kEditNet:
            case RowPickAction::kEditCheckIn:
            case RowPickAction::kEditSavedStation:
            case RowPickAction::kEditUser:
                return "Edit";
            case RowPickAction::kRemoveSavedStation:
            case RowPickAction::kRemoveUser:
            case RowPickAction::kRemoveUserKey:
                return "Remove";
            case RowPickAction::kResumeAdHocSession:
                return "Resume";

            case RowPickAction::kViewAdHocSession:
            case RowPickAction::kViewStationHistory:
            case RowPickAction::kViewStationCard:
                return "View";

            case RowPickAction::kDeleteCheckIn:
            case RowPickAction::kDeleteNetInstance:
            case RowPickAction::kDeleteHistoryCheckIn:
            case RowPickAction::kNone:
                break;
        }
        return "Delete";
    }

    void StartRowPick(AppState* state, RowPickAction action)
    {
        if (state->view_only_user && RowPickChangesSomething(action) &&
            RefuseViewOnly(state, "change anything but their own settings"))
        {
            return;
        }
        PickList list = RowPickListFor(action);
        std::size_t count = PickListSize(state, list);
        if (count == 0)
        {
            state->status_message.clear();
            std::string verb = RowPickVerb(action);
            verb[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(verb[0])));
            state->form_error = std::string("There's no ") + PickListNoun(list) + " to " + verb + ".";
            return;
        }

        // Check-ins already show their own number in the # column, so
        // that's the number to type; every other list is numbered 1, 2, 3...
        state->row_pick_numbers.clear();
        state->row_pick_numbers.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            int number = static_cast<int>(i) + 1;
            if (list == PickList::kActiveCheckIns)
            {
                number = state->active_check_ins[i].sequence_number;
            }
            else if (list == PickList::kHistoryCheckIns)
            {
                number = state->history_check_ins[i].sequence_number;
            }
            state->row_pick_numbers.push_back(number);
        }
        int widest = 9;
        for (int number : state->row_pick_numbers)
        {
            widest = std::max(widest, number);
        }
        std::size_t width = std::to_string(widest).size();
        state->row_pick_number_texts.clear();
        state->row_pick_number_texts.reserve(state->row_pick_numbers.size());
        for (int number : state->row_pick_numbers)
        {
            std::string digits = std::to_string(number);
            state->row_pick_number_texts.push_back(std::string(width - digits.size(), ' ') + digits + " ");
        }
        state->row_pick_action = action;
        state->row_pick_digits.clear();
        state->form_error.clear();
        state->status_message.clear();
    }

    void CancelRowPick(AppState* state)
    {
        state->row_pick_action = RowPickAction::kNone;
        state->row_pick_digits.clear();
        state->row_pick_numbers.clear();
        state->row_pick_number_texts.clear();
    }

    // The history page's check-in pane follows the highlighted session.
    static void SyncPickListSideEffects(AppState* state)
    {
        if (RowPickListFor(state->row_pick_action) == PickList::kNetInstances)
        {
            RefreshHistoryCheckIns(state);
        }
    }

    // Moves the list's highlight to the row numbered as typed, if there is one.
    static void HighlightTypedRow(AppState* state)
    {
        int* selection = PickListSelection(state, RowPickListFor(state->row_pick_action));
        if (selection == nullptr || state->row_pick_digits.empty())
        {
            return;
        }
        int typed = 0;
        for (char digit : state->row_pick_digits)
        {
            typed = typed * 10 + (digit - '0');
        }
        for (std::size_t i = 0; i < state->row_pick_numbers.size(); ++i)
        {
            if (state->row_pick_numbers[i] == typed)
            {
                *selection = static_cast<int>(i);
                SyncPickListSideEffects(state);
                return;
            }
        }
    }

    void MoveRowPickHighlight(AppState* state, int delta)
    {
        PickList list = RowPickListFor(state->row_pick_action);
        int* selection = PickListSelection(state, list);
        int count = static_cast<int>(PickListSize(state, list));
        if (selection == nullptr || count == 0)
        {
            return;
        }
        int moved = *selection + delta;
        *selection = moved < 0 ? 0 : (moved >= count ? count - 1 : moved);
        state->row_pick_digits.clear();
        SyncPickListSideEffects(state);
    }

    void HighlightRowPickRow(AppState* state, int index)
    {
        PickList list = RowPickListFor(state->row_pick_action);
        int* selection = PickListSelection(state, list);
        if (selection == nullptr || index < 0 || index >= static_cast<int>(PickListSize(state, list)))
        {
            return;
        }
        MoveRowPickHighlight(state, index - *selection);
    }

    std::string RowPickVerbFor(RowPickAction action)
    {
        return RowPickVerb(action);
    }

    void TypeRowPickDigit(AppState* state, char digit)
    {
        if (state->row_pick_digits.size() < 4)
        {
            state->row_pick_digits += digit;
            state->form_error.clear();
            HighlightTypedRow(state);
        }
    }

    void EraseRowPickDigit(AppState* state)
    {
        if (!state->row_pick_digits.empty())
        {
            state->row_pick_digits.pop_back();
            HighlightTypedRow(state);
        }
    }

    std::string RowPickPrompt(const AppState* state)
    {
        PickList list = RowPickListFor(state->row_pick_action);
        std::string noun = PickListNoun(list);
        return std::string(RowPickVerb(state->row_pick_action)) + " which " + noun +
               "? Type its number, then Enter (Esc cancels).";
    }

    // Opens the confirmation for deleting row `index` of the action's list,
    // or explains why it can't be deleted.
    static void OpenRowDeleteConfirm(AppState* state, RowPickAction action, int index)
    {
        state->row_delete_lines.clear();
        switch (action)
        {
            case RowPickAction::kDeleteCheckIn:
            {
                const CheckIn& check_in = state->active_check_ins[index];
                std::optional<Station> station = state->db->FindStationByCallsign(check_in.callsign);
                std::string who = check_in.callsign;
                if (station.has_value() && !station->name.empty())
                {
                    who += " (" + station->name + ")";
                }
                state->row_delete_title = "Delete Check-In";
                state->row_delete_lines.emplace_back("Delete check-in #" + std::to_string(check_in.sequence_number) +
                                                     ": " + who + "?");
                state->row_delete_lines.emplace_back("It's removed from this net's log.");
                break;
            }
            case RowPickAction::kRemoveSavedStation:
            {
                const Station& station = state->edit_net_saved_stations[index];
                state->row_delete_title = "Remove Saved Station";
                state->row_delete_lines.emplace_back("Remove " + station.callsign +
                                                     (station.name.empty() ? "" : " (" + station.name + ")") +
                                                     " from this net's saved stations?");
                if (state->db->IsStationUsedOutsideNet(station.callsign, state->edit_net_id))
                {
                    state->row_delete_lines.emplace_back(
                        "It's saved to another net or has checked in, so its details stay and "
                        "it still comes up in autocomplete.");
                }
                else
                {
                    state->row_delete_lines.emplace_back(
                        "It isn't saved to any other net and has never checked in, so its "
                        "details (member ID, address...) are deleted too.");
                }
                break;
            }
            case RowPickAction::kDeleteHistoryCheckIn:
            {
                const CheckIn& check_in = state->history_check_ins[index];
                std::optional<Station> station = state->db->FindStationByCallsign(check_in.callsign);
                std::string who = check_in.callsign;
                if (station.has_value() && !station->name.empty())
                {
                    who += " (" + station->name + ")";
                }
                std::string when;
                if (state->selected_history_index < static_cast<int>(state->history_instances.size()))
                {
                    when = " from the " + state->history_instances[state->selected_history_index].instance_date +
                           " session";
                }
                state->row_delete_title = "Delete Check-In";
                state->row_delete_lines.push_back("Delete check-in #" + std::to_string(check_in.sequence_number) +
                                                  ": " + who + when + "?");
                state->row_delete_lines.emplace_back("It's removed from that net's log.");
                break;
            }
            case RowPickAction::kDeleteNetInstance:
            {
                const NetInstance& instance = state->history_instances[index];
                if (instance.status == NetInstanceStatus::kOpen)
                {
                    state->form_error = std::string("That session is still open. Resume it (F3 on the ") +
                                        (state->history_ad_hoc ? "Ad Hoc Net page" : "net list") +
                                        "), close it with F4, then delete it.";
                    return;
                }
                std::size_t check_ins = CheckInCount(state->db, instance.id);
                std::string ended = DescribeSessionEnd(instance);
                state->row_delete_title = "Delete Net Session";
                state->row_delete_lines.emplace_back("Delete the " + DescribeSessionStart(instance) + " session (" +
                                                     (ended.empty() ? "" : "ended " + ended + ", ") +
                                                     CountCheckIns(check_ins) + ")?");
                state->row_delete_lines.emplace_back("Its whole log is deleted.");
                break;
            }
            case RowPickAction::kRemoveUser:
            {
                const std::string& username = state->manage_user_names[index];
                int keys = CountUserKeys(state, username);
                state->row_delete_title = "Remove SSH User";
                state->row_delete_lines.emplace_back(
                    "Remove " + username +
                    (keys == 1 ? " and their key?" : " and all " + std::to_string(keys) + " of their keys?"));
                state->row_delete_lines.emplace_back("They won't be able to log in over SSH.");
                break;
            }
            case RowPickAction::kRemoveUserKey:
            {
                const User& key = state->user_keys[index];
                if (state->user_keys.size() == 1)
                {
                    state->row_delete_title = "Remove SSH User";
                    state->row_delete_lines.emplace_back("Remove " + key.username + "'s only key? That removes " +
                                                         key.username + " too.");
                    state->row_delete_lines.emplace_back("They won't be able to log in over SSH.");
                }
                else
                {
                    state->row_delete_title = "Remove SSH Key";
                    state->row_delete_lines.emplace_back("Remove " + key.username + "'s key " + DescribeUserKey(key) +
                                                         "?");
                    state->row_delete_lines.emplace_back("They can still log in with their other keys.");
                }
                break;
            }
            case RowPickAction::kNone:
            case RowPickAction::kEditNet:
            case RowPickAction::kEditCheckIn:
            case RowPickAction::kEditSavedStation:
            case RowPickAction::kResumeAdHocSession:
            case RowPickAction::kViewAdHocSession:
            case RowPickAction::kViewStationHistory:
            case RowPickAction::kViewStationCard:
            case RowPickAction::kEditUser:
                return;
        }
        state->row_delete_lines.emplace_back("This can't be undone.");
        state->row_delete_action = action;
        state->row_delete_index = index;
        state->show_row_delete_confirm_modal = true;
    }

    void FinishRowPick(AppState* state)
    {
        RowPickAction action = state->row_pick_action;
        PickList list = RowPickListFor(action);
        int* selection = PickListSelection(state, list);
        if (selection == nullptr)
        {
            CancelRowPick(state);
            return;
        }

        int index = *selection;
        if (!state->row_pick_digits.empty())
        {
            // Only digits can be typed in pick mode (see HandleRowPickKey),
            // at most a few of them.
            int typed = 0;
            for (char digit : state->row_pick_digits)
            {
                typed = typed * 10 + (digit - '0');
            }
            index = -1;
            for (std::size_t i = 0; i < state->row_pick_numbers.size(); ++i)
            {
                if (state->row_pick_numbers[i] == typed)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
            if (index < 0)
            {
                state->form_error =
                    "There's no " + std::string(PickListNoun(list)) + " #" + state->row_pick_digits + ".";
                state->row_pick_digits.clear();
                return;
            }
        }
        if (index < 0 || index >= static_cast<int>(PickListSize(state, list)))
        {
            CancelRowPick(state);
            return;
        }

        CancelRowPick(state);
        state->form_error.clear();
        if (RowPickChangesSomething(action) && RefuseViewOnly(state, "change anything but their own settings"))
        {
            return;
        }
        *selection = index;
        switch (action)
        {
            case RowPickAction::kEditNet:
                OpenEditNetForm(state, state->nets[index]);
                state->page = kPageEditNet;
                if (state->edit_net_name_input)
                {
                    state->edit_net_name_input->TakeFocus();
                }
                return;
            case RowPickAction::kEditCheckIn:
                OpenEditCheckInForm(state, state->active_check_ins[index]);
                return;
            case RowPickAction::kViewStationHistory:
                OpenStationHistory(state, state->active_check_ins[index]);
                return;
            case RowPickAction::kViewStationCard:
                OpenStationCard(state, state->active_check_ins[index].callsign);
                return;
            case RowPickAction::kResumeAdHocSession:
            case RowPickAction::kViewAdHocSession:
            {
                // (OfferToResume only ever views, for a view-only user.)
                const NetInstance& session = state->open_ad_hoc_sessions[index];
                std::optional<Net> net = state->db->GetNetById(session.net_id);
                if (!net.has_value())
                {
                    RefreshOpenAdHocSessions(state);
                    state->form_error = "That net has been deleted in the meantime.";
                    return;
                }
                state->start_net = *net;
                OfferToResume(state, session);
                return;
            }
            case RowPickAction::kEditSavedStation:
                LoadSavedStationIntoForm(state, state->edit_net_saved_stations[index]);
                return;
            case RowPickAction::kEditUser:
                OpenUserKeys(state, index);
                return;
            default:
                if (list == PickList::kNetInstances)
                {
                    RefreshHistoryCheckIns(state);
                }
                OpenRowDeleteConfirm(state, action, index);
                return;
        }
    }

    void ConfirmRowDelete(AppState* state)
    {
        RowPickAction action = state->row_delete_action;
        int index = state->row_delete_index;
        state->show_row_delete_confirm_modal = false;
        if (RefuseViewOnly(state, "delete anything"))
        {
            return;
        }
        state->row_delete_action = RowPickAction::kNone;
        state->row_delete_index = -1;

        int* selection = PickListSelection(state, RowPickListFor(action));
        if (selection == nullptr || index < 0 || index >= static_cast<int>(PickListSize(state, RowPickListFor(action))))
        {
            return;
        }
        *selection = index;
        switch (action)
        {
            case RowPickAction::kDeleteCheckIn:
                RemoveSelectedCheckIn(state);
                state->status_message = "Check-in deleted.";
                break;
            case RowPickAction::kRemoveSavedStation:
                RemoveSelectedSavedNetStation(state);
                break;
            case RowPickAction::kDeleteHistoryCheckIn:
                DeleteSelectedHistoryCheckIn(state);
                state->status_message = "Check-in deleted.";
                break;
            case RowPickAction::kDeleteNetInstance:
                DeleteSelectedNetInstance(state);
                if (state->form_error.empty())
                {
                    state->status_message = "Net session deleted.";
                }
                break;
            case RowPickAction::kRemoveUser:
                RemoveSelectedUser(state);
                break;
            case RowPickAction::kRemoveUserKey:
                RemoveSelectedUserKey(state);
                break;
            default:
                break;
        }
        // Keep the highlight on a row that still exists.
        std::size_t remaining = PickListSize(state, RowPickListFor(action));
        if (*selection >= static_cast<int>(remaining))
        {
            *selection = remaining == 0 ? 0 : static_cast<int>(remaining) - 1;
        }
    }

    void CancelRowDelete(AppState* state)
    {
        state->show_row_delete_confirm_modal = false;
        state->row_delete_action = RowPickAction::kNone;
        state->row_delete_index = -1;
    }

    // Reloads the Keys window's list for AppState::user_keys_username.
    static void RefreshUserKeys(AppState* state)
    {
        state->user_keys = state->db->GetUserKeys(state->user_keys_username);
        state->user_keys_cells.clear();
        for (const User& key : state->user_keys)
        {
            state->user_keys_cells.push_back(UserKeyCells(key));
        }
        state->user_keys_labels = FormatRows(state->user_keys_cells, UserKeyLayout(state->list_width));
        if (state->selected_user_key_index >= static_cast<int>(state->user_keys.size()))
        {
            state->selected_user_key_index = 0;
        }
    }

    void RefreshUsers(AppState* state)
    {
        state->manage_users = state->db->ListUsers();
        state->manage_user_names.clear();
        state->manage_users_cells.clear();
        for (const User& user : state->manage_users)
        {
            // ListUsers returns each user's keys together.
            if (state->manage_user_names.empty() || state->manage_user_names.back() != user.username)
            {
                state->manage_user_names.push_back(user.username);
                state->manage_users_cells.push_back(UserCells(user.username, state->manage_users));
            }
        }
        state->manage_users_labels = FormatRows(state->manage_users_cells, UserLayout(state->list_width));
        if (state->selected_user_index >= static_cast<int>(state->manage_user_names.size()))
        {
            state->selected_user_index = 0;
        }
        if (state->show_user_keys_modal)
        {
            RefreshUserKeys(state);
        }
    }

    void OpenUserKeys(AppState* state, int index)
    {
        if (index < 0 || index >= static_cast<int>(state->manage_user_names.size()))
        {
            return;
        }
        state->selected_user_index = index;
        state->user_keys_username = state->manage_user_names[index];
        state->rename_username = state->user_keys_username;
        state->edit_user_access_index = state->db->IsUserViewOnly(state->user_keys_username) ? 1 : 0;
        state->selected_user_key_index = 0;
        state->new_key_text.clear();
        state->form_error.clear();
        state->status_message.clear();
        state->show_user_keys_modal = true;
        RefreshUserKeys(state);
    }

    void CloseUserKeys(AppState* state)
    {
        state->show_user_keys_modal = false;
        state->user_keys_username.clear();
        state->user_keys.clear();
        state->user_keys_cells.clear();
        state->user_keys_labels.clear();
        state->new_key_text.clear();
        state->rename_username.clear();
        state->form_error.clear();
    }

    void SaveEditedUser(AppState* state)
    {
        if (RefuseViewOnly(state, "manage users") || !state->show_user_keys_modal)
        {
            return;
        }
        std::string old_username = state->user_keys_username;
        std::string new_username = NormalizeCallsign(state->rename_username);
        state->rename_username = new_username;
        state->status_message.clear();
        bool renaming = new_username != old_username;
        if (renaming && !UsernameIsCallsign(new_username))
        {
            state->form_error = new_username.empty() || new_username.find('/') != std::string::npos
                                    ? "A username is the user's call sign alone, without /M, "
                                      "/P or the like."
                                    : "A username is the user's call sign: " + new_username +
                                          " isn't a valid US or Canadian call sign.";
            return;
        }
        if (renaming && !state->db->RenameUser(old_username, new_username))
        {
            state->form_error = new_username + " is already a user.";
            return;
        }
        std::string error;
        if (renaming)
        {
            // Their settings, exports and received files follow them.
            MoveSshUserFiles(state->db_path, old_username, new_username, &error);
        }
        bool view_only = state->edit_user_access_index == 1;
        bool access_changed = state->db->IsUserViewOnly(new_username) != view_only;
        if (access_changed)
        {
            state->db->SetUserViewOnly(new_username, view_only);
        }

        CloseUserKeys(state);
        RefreshUsers(state);
        for (std::size_t i = 0; i < state->manage_user_names.size(); ++i)
        {
            if (state->manage_user_names[i] == new_username)
            {
                state->selected_user_index = static_cast<int>(i);
                break;
            }
        }
        state->form_error = error;
        std::string access = view_only ? "view-only" : "a full user";
        if (renaming && access_changed)
        {
            state->status_message =
                "Renamed " + old_username + " to " + new_username + ", now " + access + ", from their next login.";
        }
        else if (renaming)
        {
            state->status_message = "Renamed " + old_username + " to " + new_username + ", from their next login.";
        }
        else if (access_changed)
        {
            state->status_message = new_username + " is now " + access + ", from their next login.";
        }
        else
        {
            state->status_message = "No changes to " + new_username + ".";
        }
    }

    void AddKeyToShownUser(AppState* state)
    {
        if (RefuseViewOnly(state, "manage users") || !state->show_user_keys_modal)
        {
            return;
        }
        std::string public_key;
        std::string key_error;
        if (!ValidatePublicKey(state->new_key_text, &public_key, &key_error))
        {
            state->status_message.clear();
            state->form_error = key_error;
            return;
        }
        User key;
        key.username = state->user_keys_username;
        key.public_key = public_key;
        key.created_at = static_cast<std::int64_t>(std::time(nullptr));
        bool added = state->db->CreateUser(key);
        state->new_key_text.clear();
        RefreshUsers(state);
        state->form_error.clear();
        state->status_message =
            added ? "Added a key for \"" + key.username + "\"." : "\"" + key.username + "\" already has that key.";
    }

    void RemoveSelectedUserKey(AppState* state)
    {
        if (RefuseViewOnly(state, "manage users") || state->user_keys.empty())
        {
            return;
        }
        User key = state->user_keys[state->selected_user_key_index];
        bool last_key = state->user_keys.size() == 1;
        state->db->DeleteUserKey(key.id);
        if (last_key)
        {
            CloseUserKeys(state);
        }
        RefreshUsers(state);
        state->form_error.clear();
        state->status_message = last_key ? "Removed \"" + key.username + "\" with their last key."
                                         : "Removed a key of \"" + key.username + "\" (" + DescribeUserKey(key) + ").";
    }

    bool UsernameIsCallsign(const std::string& username)
    {
        return username.find('/') == std::string::npos && IsValidCallsign(username);
    }

    void AddUserFromForm(AppState* state)
    {
        if (RefuseViewOnly(state, "manage users"))
        {
            return;
        }
        state->new_user_username = NormalizeCallsign(state->new_user_username);
        if (state->new_user_username.empty() || state->new_user_public_key.empty())
        {
            state->status_message.clear();
            state->form_error = "Username and public key are both required.";
            return;
        }
        if (!UsernameIsCallsign(state->new_user_username))
        {
            state->status_message.clear();
            state->form_error = state->new_user_username.find('/') != std::string::npos
                                    ? "A username is the user's call sign alone, without /M, /P or the like."
                                    : "A username is the user's call sign: " + state->new_user_username +
                                          " isn't a valid US or Canadian call sign.";
            return;
        }
        std::string public_key;
        std::string key_error;
        if (!ValidatePublicKey(state->new_user_public_key, &public_key, &key_error))
        {
            state->status_message.clear();
            state->form_error = key_error;
            return;
        }

        User user;
        user.username = state->new_user_username;
        user.public_key = public_key;
        user.created_at = static_cast<std::int64_t>(std::time(nullptr));
        user.view_only = state->new_user_access_index == 1;
        bool had_keys = CountUserKeys(state, user.username) > 0;
        bool added = state->db->CreateUser(user);

        state->new_user_username.clear();
        state->new_user_public_key.clear();
        state->new_user_access_index = 0;
        RefreshUsers(state);
        state->form_error.clear();
        // Another key keeps the username's access, whatever the form said.
        std::string access = state->db->IsUserViewOnly(user.username) ? " (view-only)" : " (full access)";
        if (!added)
        {
            state->status_message = "\"" + user.username + "\" already has that key.";
        }
        else if (had_keys)
        {
            state->status_message = "Added another key for \"" + user.username + "\"" + access + ".";
        }
        else
        {
            state->status_message = "Added \"" + user.username + "\"" + access + ".";
        }
    }

    void RemoveSelectedUser(AppState* state)
    {
        if (RefuseViewOnly(state, "manage users") || state->manage_user_names.empty())
        {
            return;
        }
        std::string username = state->manage_user_names[state->selected_user_index];
        state->db->DeleteUser(username);
        RefreshUsers(state);
        state->form_error.clear();
        state->status_message = "Removed \"" + username + "\".";
    }

    // The operator's own callsign on `instance`: the one in the role they
    // started it in.
    static const std::string& OperatorCallsign(const NetInstance& instance)
    {
        if (instance.operator_role == kRoleAlternateNetControl)
        {
            return instance.alternate_net_control_callsign;
        }
        if (instance.operator_role == kRoleLogger)
        {
            return instance.logger_callsign;
        }
        return instance.net_control_callsign;
    }

    // Writes `instance`'s check-ins, all but the operator's own, to `path`
    // as ADIF (see BuildAdif), from the exporting user's callsign.
    static bool WriteSessionAdif(AppState* state, const NetInstance& instance, const std::vector<CheckIn>& check_ins,
                                 const std::string& path, std::string* error)
    {
        // The contacts point into `check_ins` and `stations`, which outlive
        // BuildAdif.
        std::vector<AdifContact> contacts;
        std::vector<Station> stations;
        Net net;
        {
            Database::ReadTransaction reads(state->db);
            std::optional<Net> found = state->db->GetNetById(instance.net_id);
            if (found.has_value())
            {
                net = std::move(*found);
            }
            const std::string& operator_callsign = OperatorCallsign(instance);
            // The session's stations in one query, as CheckInCells does.
            stations = state->db->GetStationsInNetInstance(instance.id);
            contacts.reserve(check_ins.size());
            for (const CheckIn& check_in : check_ins)
            {
                if (CallsignsEqual(check_in.callsign, operator_callsign))
                {
                    continue;
                }
                AdifContact& contact = contacts.emplace_back();
                contact.check_in = &check_in;
                std::vector<Station>::const_iterator found =
                    std::lower_bound(stations.begin(), stations.end(), check_in.callsign, StationCallsignBefore);
                if (found != stations.end() && found->callsign == check_in.callsign)
                {
                    contact.station = &*found;
                }
            }
        }
        const std::string& frequency = instance.frequency.empty() ? net.default_frequency : instance.frequency;
        std::string adif = BuildAdif(contacts, net.mode, frequency, ToUpperAscii(state->settings.callsign),
                                     instance.instance_date, static_cast<std::int64_t>(std::time(nullptr)));
        std::string temp_path = TemporaryPathFor(path);
        {
            std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
            out.write(adif.data(), static_cast<std::streamsize>(adif.size()));
            if (!out)
            {
                *error = "couldn't write the file";
                return false;
            }
        }
        return ReplaceWithFile(temp_path, path, error);
    }

    void ExportNetLog(AppState* state, const std::string& net_name, const NetInstance& instance,
                      const std::vector<CheckIn>& check_ins)
    {
        std::vector<std::string> lines;
        lines.push_back("Net: " + net_name);
        lines.push_back("Date: " + instance.instance_date);
        if (instance.started_at > 0)
        {
            lines.push_back("Start Time: " + FormatLocalTimeOfDay(instance.started_at));
        }
        if (instance.closed_at > 0)
        {
            lines.push_back("End Time: " + DescribeSessionEnd(instance));
        }
        lines.push_back("Net Control: " + instance.net_control_callsign);
        lines.push_back("Alternate NC: " + instance.alternate_net_control_callsign);
        lines.push_back("Logger: " + instance.logger_callsign);
        lines.push_back(std::string("Status: ") + (instance.status == NetInstanceStatus::kOpen ? "OPEN" : "closed"));
        lines.emplace_back("");
        // A fixed format: every column, each at its set export width,
        // whatever the terminal or the data (see ExportListLayout).
        std::vector<std::vector<std::string>> cells = CheckInCells(state->db, check_ins);
        ListLayout layout = ExportListLayout(CheckInColumns(), 1);
        lines.push_back(FormatListHeading(CheckInColumns(), layout));
        std::vector<std::string> rows = FormatRows(cells, layout);
        lines.insert(lines.end(), rows.begin(), rows.end());

        std::string stem = SessionExportsDir(state->db_path, state->ssh_username) + "/" +
                           SanitizeFilenameComponent(net_name) + "_" +
                           SanitizeFilenameComponent(instance.instance_date);
        std::string log_path = stem + "_log.txt";
        if (!WriteExportLines(state, log_path, lines))
        {
            return;
        }
        // And the session exactly, for History's F6 Import on another
        // QuickLogger (see GatherSessionSlice).
        std::string session_path = stem + ".qlsession";
        std::string error;
        if (!WriteNetSliceFile(session_path, GatherSessionSlice(state->db, instance.id), &error))
        {
            state->status_message.clear();
            state->form_error = "Saved " + log_path + ", but not " + session_path + ": " + error;
            return;
        }
        // And for a logging program (see BuildAdif).
        std::string adif_path = stem + ".adi";
        if (!WriteSessionAdif(state, instance, check_ins, adif_path, &error))
        {
            state->status_message.clear();
            state->form_error = "Saved " + log_path + " and " + session_path + ", but not " + adif_path + ": " + error;
            return;
        }
        std::vector<std::string> paths{log_path, session_path, adif_path};
        // Sent over ZMODEM as one .zip, so there's one file to receive. With
        // no ZMODEM (at the console, on Windows, or without sz), the files
        // are all there is.
        if (IsLocalTerminal(state->is_console_session) || NoZmodemOnThisSystem() || !ZmodemSendAvailable())
        {
            OfferZmodemSendFiles(state, paths);
            return;
        }
        std::string zip_path = stem + ".zip";
        if (!WriteZipArchive(zip_path, paths, static_cast<std::int64_t>(std::time(nullptr)), &error))
        {
            state->status_message.clear();
            state->form_error = "Saved " + ListPaths(paths) + ", but not " + zip_path + ": " + error;
            return;
        }
        OfferZmodemSendFiles(state, {zip_path});
        state->zmodem_zip_contents = paths;
        state->status_message = "Saved to " + ListPaths(paths) + ".";
    }

    void ExportSavedStations(AppState* state, const std::string& net_name, const std::vector<Station>& saved_stations)
    {
        Database::ReadTransaction reads(state->db);
        std::vector<std::string> lines;
        lines.push_back("Net: " + net_name);
        lines.emplace_back("Saved Stations:");
        lines.emplace_back("");
        // Every column, as in ExportNetLog.
        std::vector<std::vector<std::string>> cells;
        for (const Station& station : saved_stations)
        {
            cells.push_back(
                SavedStationCells(station, state->db->GetSavedNetStationRemarks(state->edit_net_id, station.callsign)));
        }
        ListLayout layout = ExportListLayout(SavedStationColumns(), 1);
        lines.push_back(FormatListHeading(SavedStationColumns(), layout));
        std::vector<std::string> rows = FormatRows(cells, layout);
        lines.insert(lines.end(), rows.begin(), rows.end());

        std::string path = SessionExportsDir(state->db_path, state->ssh_username) + "/" +
                           SanitizeFilenameComponent(net_name) + "_saved_stations.txt";
        ExportLinesToFile(state, path, lines);
    }

    void ExportNetSlice(AppState* state, const Net& net)
    {
        NetSlice slice = GatherNetSlice(state->db, net.id);
        std::string path = SessionExportsDir(state->db_path, state->ssh_username) + "/" +
                           SanitizeFilenameComponent(net.name) + ".qlnet";

        std::string error;
        if (!WriteNetSliceFile(path, slice, &error))
        {
            state->status_message.clear();
            state->form_error = error;
            return;
        }

        state->form_error.clear();
        OfferZmodemSend(state, path);
    }

    void RefreshImportNetFiles(AppState* state)
    {
        state->import_net_files = ListFilesWithExtension(SessionImportsDir(state->db_path, state->ssh_username),
                                                         state->import_session ? ".qlsession" : ".qlnet");
        if (state->selected_import_file_index >= static_cast<int>(state->import_net_files.size()))
        {
            state->selected_import_file_index = 0;
        }
    }

    static void ImportNetSlice(AppState* state, bool names_checked);

    void ImportSelectedNetSlice(AppState* state)
    {
        ImportNetSlice(state, false);
    }

    void ImportSelectedNetSliceAnyway(AppState* state)
    {
        if (state->merge_name_taken)
        {
            return;
        }
        state->merge_stage = MergeStage::kNone;
        state->show_merge_modal = false;
        ImportNetSlice(state, true);
    }

    // Orders nets with `*name` (NetNamesAreTheSame) before the rest. Holds
    // a pointer, as the sort may copy it many times; the name outlives it.
    class SameNameFirst
    {
    public:
        explicit SameNameFirst(const std::string* name) : name_(name) {}
        bool operator()(const Net& a, const Net& b) const
        {
            return NetNamesAreTheSame(a.name, *name_) && !NetNamesAreTheSame(b.name, *name_);
        }

    private:
        const std::string* name_;
    };

    // Opens the Import or Merge window for `slice` (read from the
    // highlighted file), offering the nets here in `alike`.
    static void OpenNetMergeChoice(AppState* state, NetSlice slice, std::vector<Net> alike, bool name_taken)
    {
        state->merge_slice = std::move(slice);
        state->merge_candidates = std::move(alike);
        state->merge_candidate_labels.clear();
        // Its very name first: the likeliest one.
        std::stable_sort(state->merge_candidates.begin(), state->merge_candidates.end(),
                         SameNameFirst(&state->merge_slice.net.name));
        for (const Net& net : state->merge_candidates)
        {
            std::size_t sessions = state->db->GetNetInstancesForNet(net.id).size();
            state->merge_candidate_labels.push_back(net.name + "  (" + std::to_string(sessions) +
                                                    (sessions == 1 ? " session" : " sessions") + ")");
        }
        state->selected_merge_candidate = 0;
        state->merge_name_taken = name_taken;
        state->merge_stage = MergeStage::kChooseNet;
        state->show_merge_modal = true;
        state->form_error.clear();
        state->status_message.clear();
    }

    void MoveMergeHighlight(AppState* state, int delta)
    {
        bool summary = state->merge_stage == MergeStage::kSummary;
        int* index = summary ? &state->selected_merge_conflict : &state->selected_merge_candidate;
        // In the summary, the sessions that differ, then the stations.
        int count =
            static_cast<int>(summary ? state->merge_conflicts.size() + state->merge_plan.station_conflicts.size()
                                     : state->merge_candidates.size());
        if (count > 0)
        {
            *index = std::clamp(*index + delta, 0, count - 1);
        }
    }

    // `callsigns` joined with ", ", the first three and how many more.
    static void AppendCallsigns(const std::vector<std::string>& callsigns, std::string* text)
    {
        std::size_t shown = std::min<std::size_t>(callsigns.size(), 3);
        for (std::size_t i = 0; i < shown; ++i)
        {
            text->append(i == 0 ? "" : ", ");
            text->append(callsigns[i]);
        }
        if (callsigns.size() > shown)
        {
            text->append(" +");
            text->append(std::to_string(callsigns.size() - shown));
        }
    }

    // What differs in a session: "check-ins: K4AAA differs; K4ZZZ only
    // in file; notes differ".
    static std::string WhatDiffers(const MergeSession& session)
    {
        std::string text;
        if (session.check_ins_differ)
        {
            text.append("check-ins: ");
            bool first = true;
            if (!session.callsigns_changed.empty())
            {
                AppendCallsigns(session.callsigns_changed, &text);
                text.append(session.callsigns_changed.size() == 1 ? " differs" : " differ");
                first = false;
            }
            if (!session.callsigns_only_in_file.empty())
            {
                text.append(first ? "" : "; ");
                AppendCallsigns(session.callsigns_only_in_file, &text);
                text.append(" only in file");
                first = false;
            }
            if (!session.callsigns_only_here.empty())
            {
                text.append(first ? "" : "; ");
                AppendCallsigns(session.callsigns_only_here, &text);
                text.append(" only here");
            }
        }
        if (session.notes_differ)
        {
            text.append(text.empty() ? "notes differ" : "; notes differ");
        }
        return text;
    }

    void ChooseMergeTarget(AppState* state)
    {
        if (state->merge_stage != MergeStage::kChooseNet || state->merge_candidates.empty())
        {
            return;
        }
        const Net& target = state->merge_candidates[static_cast<std::size_t>(state->selected_merge_candidate)];
        state->merge_target_name = target.name;
        state->merge_plan = PlanNetMerge(state->db, state->merge_slice, target.id);
        state->merge_conflicts.clear();
        state->merge_conflict_texts.clear();
        for (std::size_t i = 0; i < state->merge_plan.sessions.size(); ++i)
        {
            const MergeSession& session = state->merge_plan.sessions[i];
            if (session.kind == MergeSessionKind::kDiffers)
            {
                state->merge_conflicts.push_back(i);
                const NetInstance& file = state->merge_slice.instances[session.file_index];
                MergeConflictText text;
                text.when = file.instance_date;
                if (file.started_at > 0)
                {
                    text.when.append("  ");
                    text.when.append(FormatLocalTimeOfDay(file.started_at));
                }
                text.what = WhatDiffers(session);
                state->merge_conflict_texts.push_back(std::move(text));
            }
        }
        state->merge_station_callsign_texts.clear();
        state->merge_station_difference_texts.clear();
        for (const MergeStationConflict& conflict : state->merge_plan.station_conflicts)
        {
            state->merge_station_callsign_texts.push_back("  " + conflict.file_station->callsign);
            std::vector<std::string> differences;
            for (const StationDetailDifference& difference : conflict.differences)
            {
                differences.push_back("         " + std::string(difference.field) + ": " + difference.here + " here, " +
                                      difference.file + " in file");
            }
            state->merge_station_difference_texts.push_back(std::move(differences));
        }
        state->selected_merge_conflict = 0;
        state->merge_stage = MergeStage::kSummary;
    }

    void ToggleMergeReplace(AppState* state)
    {
        if (state->merge_stage != MergeStage::kSummary)
        {
            return;
        }
        std::size_t index = static_cast<std::size_t>(state->selected_merge_conflict);
        std::size_t sessions = state->merge_conflicts.size();
        if (index < sessions)
        {
            MergeSession& session = state->merge_plan.sessions[state->merge_conflicts[index]];
            session.replace = !session.replace;
        }
        else if (index - sessions < state->merge_plan.station_conflicts.size())
        {
            MergeStationConflict& station = state->merge_plan.station_conflicts[index - sessions];
            station.replace = !station.replace;
        }
    }

    // "3 sessions", "1 saved station".
    static std::string Count(int count, const char* singular, const char* plural)
    {
        return std::to_string(count) + " " + (count == 1 ? singular : plural);
    }

    void ConfirmNetMerge(AppState* state)
    {
        if (state->merge_stage != MergeStage::kSummary || RefuseViewOnly(state, "import nets"))
        {
            return;
        }
        NetMergeResult result;
        try
        {
            result = ApplyNetMerge(state->db, state->merge_slice, state->merge_plan);
        }
        catch (const std::exception& e)
        {
            BackOutOfNetMerge(state);
            BackOutOfNetMerge(state);
            state->status_message.clear();
            state->form_error = std::string("Merge failed, so nothing was changed: ") + e.what();
            return;
        }
        std::string message = "Merged into " + state->merge_target_name + ": ";
        message.append(Count(result.sessions_added, "session", "sessions"));
        message.append(" and ");
        message.append(Count(result.saved_stations_added, "saved station", "saved stations"));
        message.append(" added");
        if (result.sessions_replaced > 0)
        {
            message.append(", ");
            message.append(Count(result.sessions_replaced, "session", "sessions"));
            message.append(" replaced");
        }
        if (result.stations_replaced > 0)
        {
            message.append(", ");
            message.append(Count(result.stations_replaced, "station's details", "stations' details"));
            message.append(" taken from the file");
        }
        message.push_back('.');
        // The plan points into the file's data: both go together.
        state->merge_stage = MergeStage::kNone;
        state->show_merge_modal = false;
        state->merge_plan = NetMergePlan();
        state->merge_slice = NetSlice();
        RefreshNets(state);
        state->form_error.clear();
        state->status_message = std::move(message);
        state->page = kPageNetList;
    }

    void BackOutOfNetMerge(AppState* state)
    {
        if (state->merge_stage == MergeStage::kSummary)
        {
            state->merge_stage = MergeStage::kChooseNet;
            return;
        }
        state->merge_stage = MergeStage::kNone;
        state->show_merge_modal = false;
        state->merge_plan = NetMergePlan();
        state->merge_slice = NetSlice();
    }

    static void ImportNetSlice(AppState* state, bool names_checked)
    {
        if (RefuseViewOnly(state, "import nets"))
        {
            return;
        }
        if (state->import_net_files.empty())
        {
            state->status_message.clear();
            state->form_error = "No net-export files to import yet.";
            return;
        }

        std::string path = SessionImportsDir(state->db_path, state->ssh_username) + "/" +
                           state->import_net_files[state->selected_import_file_index];
        std::string error;
        std::optional<NetSlice> slice = ReadNetSliceFile(path, &error);
        if (!slice.has_value())
        {
            state->status_message.clear();
            state->form_error = error;
            return;
        }

        // No two recurring nets may share a name, so one with the same name
        // can only be merged into; and a net that only looks like it is
        // probably the same one (imported before, or set up by hand), so
        // the operator chooses: merge, or import it as a new net anyway.
        bool taken = !ExistingNetNamed(state, slice->net.name).empty();
        if (taken || !names_checked)
        {
            std::vector<Net> alike;
            for (const Net& net : state->nets)
            {
                if (NetNamesAreTheSame(slice->net.name, net.name) ||
                    NetNamesLookAlike(slice->net.name, net.name, false))
                {
                    alike.push_back(net);
                }
            }
            if (!alike.empty())
            {
                OpenNetMergeChoice(state, std::move(*slice), std::move(alike), taken);
                return;
            }
        }

        try
        {
            ApplyNetSlice(state->db, *slice, static_cast<std::int64_t>(std::time(nullptr)));
        }
        catch (const std::exception& e)
        {
            RefreshNets(state);
            state->status_message.clear();
            state->form_error = std::string("Import failed: ") + e.what();
            return;
        }
        RefreshNets(state);
        state->form_error.clear();
        state->status_message = "Imported \"" + slice->net.name + "\". It's listed with today's date as \"imported\".";
        state->page = kPageNetList;
    }

    void OpenSessionImport(AppState* state)
    {
        if (RefuseViewOnly(state, "import sessions"))
        {
            return;
        }
        state->import_session = true;
        state->import_session_ad_hoc = state->history_ad_hoc;
        state->import_session_net_id = 0;
        state->import_session_net_name.clear();
        if (!state->history_ad_hoc)
        {
            if (state->selected_net_index >= static_cast<int>(state->nets.size()))
            {
                return;
            }
            const Net& net = state->nets[state->selected_net_index];
            state->import_session_net_id = net.id;
            state->import_session_net_name = net.name;
        }
        state->selected_import_file_index = 0;
        RefreshImportNetFiles(state);
        state->form_error.clear();
        state->status_message.clear();
        state->page = kPageImportNet;
    }

    void LeaveImportPage(AppState* state)
    {
        state->form_error.clear();
        state->status_message.clear();
        if (state->import_session)
        {
            state->import_session = false;
            RefreshNetHistory(state);
            state->page = kPageNetHistory;
            return;
        }
        state->page = kPageNetList;
    }

    static void ImportSession(AppState* state, bool name_checked);

    void ImportSelectedSession(AppState* state)
    {
        ImportSession(state, false);
    }

    void ImportSelectedSessionAnyway(AppState* state)
    {
        CancelConfirmPrompt(state);
        ImportSession(state, true);
    }

    static void ImportSession(AppState* state, bool name_checked)
    {
        if (RefuseViewOnly(state, "import sessions"))
        {
            return;
        }
        if (state->import_net_files.empty())
        {
            state->status_message.clear();
            state->form_error = "No session files to import yet.";
            return;
        }

        std::string path = SessionImportsDir(state->db_path, state->ssh_username) + "/" +
                           state->import_net_files[state->selected_import_file_index];
        std::string error;
        std::optional<NetSlice> slice = ReadSessionSliceFile(path, &error);
        if (!slice.has_value())
        {
            state->status_message.clear();
            state->form_error = error;
            return;
        }

        // Logged under a name nothing like this net's: probably the wrong
        // net's History, so ask first.
        if (!name_checked && !state->import_session_ad_hoc &&
            !NetNamesLookAlike(slice->net.name, state->import_session_net_name))
        {
            ShowConfirmPrompt(
                state, ConfirmPrompt::kImportOtherNet, "Import Into This Net?",
                {"This session was logged as \"" + slice->net.name + "\", not " + state->import_session_net_name + ".",
                 "If it belongs to another net, press Esc and import it from "
                 "that net's History instead."});
            return;
        }

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        std::int64_t net_id = state->import_session_net_id;
        std::string net_name = state->import_session_net_name;
        std::int64_t instance_id = 0;
        // One transaction for all of it: a new ad hoc net is kept only if its
        // session is imported too (ApplySessionSlice's own joins this one).
        Database::WriteTransaction transaction(state->db);
        try
        {
            if (state->import_session_ad_hoc)
            {
                // Imported before: an ad hoc net of the same name with the
                // same session.
                const NetInstance& session = slice->instances[0];
                for (const NetInstance& existing : state->db->GetAdHocNetInstances())
                {
                    std::optional<Net> existing_net = state->db->GetNetById(existing.net_id);
                    if (existing_net.has_value() && existing_net->name == slice->net.name &&
                        existing.instance_date == session.instance_date && existing.started_at == session.started_at)
                    {
                        state->status_message.clear();
                        state->form_error =
                            "The ad hoc net " + slice->net.name + " already has that session, so nothing was imported.";
                        return;
                    }
                }
                // A new ad hoc net, as it was defined where it was logged.
                Net net = slice->net;
                net.is_ad_hoc = true;
                net.imported_at = now;
                net.default_location = ExtractZipCode(net.default_location);
                MoveBadFrequencyToComments(&net.default_frequency, &net.comments);
                net_id = state->db->CreateNet(net);
                net_name = net.name;
            }
            instance_id = ApplySessionSlice(state->db, *slice, net_id, &error);
            if (instance_id != 0)
            {
                transaction.Commit();
            }
        }
        catch (const std::exception& e)
        {
            error = std::string("Import failed: ") + e.what();
        }
        if (instance_id == 0)
        {
            state->status_message.clear();
            state->form_error = error;
            return;
        }

        const NetInstance& session = slice->instances[0];
        std::string message = "Imported the session of " + session.instance_date;
        if (!state->import_session_ad_hoc && slice->net.name != net_name)
        {
            message += " (logged as \"" + slice->net.name + "\")";
        }
        message += " into " + net_name + ".";

        state->import_session = false;
        RefreshNetHistory(state);
        for (std::size_t i = 0; i < state->history_instances.size(); ++i)
        {
            if (state->history_instances[i].id == instance_id)
            {
                state->selected_history_index = static_cast<int>(i);
                RefreshHistoryCheckIns(state);
                break;
            }
        }
        state->form_error.clear();
        state->status_message = message;
        state->page = kPageNetHistory;
    }

    void StartZmodemReceive(AppState* state)
    {
        if (RefuseViewOnly(state, "import files"))
        {
            return;
        }
        // Nobody on the other end of a local terminal to send one, and no
        // ZMODEM at all on some systems (Windows, Alpine).
        if (IsLocalTerminal(state->is_console_session) || NoZmodemOnThisSystem())
        {
            return;
        }
        state->zmodem_action = ZmodemAction::kReceive;
        state->show_zmodem_confirm_modal = true;
    }

    static void AppendNearbyUlsSuggestions(AppState* state, const std::string& typed, const std::string& net_zip,
                                           bool partial, std::size_t max_suggestions, std::vector<Station>* suggestions,
                                           std::vector<std::string>* sources);
    static void AppendCanadianSuggestions(AppState* state, const std::string& typed, bool partial,
                                          std::size_t max_suggestions, std::vector<Station>* suggestions,
                                          std::vector<std::string>* sources);
    static bool LooksCanadian(const std::string& typed);

    // Once the matches for `typed` are listed, makes the one marked ">" the
    // one meant -- it's what's taken however the operator leaves the
    // callsign field (see ApplySelectedCallsignSuggestion). If `typed` is
    // itself one of the matches, it's marked. If it's a known or licensed
    // call sign that didn't make the list (a station beyond the Nearby
    // Radius, say), it's added at the bottom, with where it came from, for
    // the operator to move down to; the ">" stays on the best match.
    static void MarkTypedCallsignMatch(AppState* state, const std::string& typed, std::size_t max_suggestions,
                                       std::vector<Station>* suggestions, std::vector<std::string>* sources,
                                       int* selected)
    {
        if (suggestions->empty())
        {
            return;
        }
        std::string callsign = NormalizeCallsign(typed);
        for (std::size_t i = 0; i < suggestions->size(); ++i)
        {
            if ((*suggestions)[i].callsign == callsign)
            {
                *selected = static_cast<int>(i);
                return;
            }
        }

        std::string source;
        std::optional<Station> known = state->db->FindStationByCallsign(callsign);
        if (known.has_value())
        {
            source = KnownStationSource(false);
        }
        else
        {
            known = state->db->FindLicensedStationByCallsign(callsign);
            if (!known.has_value())
            {
                return;
            }
            source = LooksCanadian(callsign) ? kIsedSource : "(ULS)";
            std::optional<ZipCentroid> origin;
            if (!state->nearby_zips_origin.empty())
            {
                origin = ZipCentroid{state->nearby_zips_origin, state->nearby_zips_origin_lat,
                                     state->nearby_zips_origin_lon};
            }
            std::optional<ZipCentroid> station_zip =
                known->zip.size() >= 5 ? state->db->FindZipCentroid(known->zip.substr(0, 5)) : std::nullopt;
            if (!LooksCanadian(callsign) && origin.has_value() && station_zip.has_value())
            {
                source = UlsSource(DistanceMiles(origin->lat, origin->lon, station_zip->lat, station_zip->lon));
            }
        }
        if (suggestions->size() >= max_suggestions)
        {
            suggestions->pop_back();
            sources->pop_back();
        }
        suggestions->push_back(*known);
        sources->push_back(source);
    }

    void RefreshCallsignSuggestions(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        state->modal_callsign_suggestions.clear();
        state->modal_callsign_suggestion_labels.clear();
        state->modal_callsign_suggestion_sources.clear();
        state->selected_suggestion_index = 0;

        if (state->modal_station.callsign.empty())
        {
            return;
        }

        const std::size_t kMaxSuggestions = MaxCallsignMatches(state);
        const int max_suggestions = static_cast<int>(kMaxSuggestions);

        // Tier 1: callers already known to this specific net (real check-ins
        // or SaveNetStation). No more than can be shown.
        state->modal_callsign_suggestions = state->db->SearchNetStationsByCallsignSubstring(
            state->active_instance.net_id, state->modal_station.callsign, max_suggestions);
        std::size_t tier1_count = state->modal_callsign_suggestions.size();

        // Tier 2: callers known to other nets (see
        // SearchStationsByCallsignSubstring). It includes tier 1's, which
        // are dropped as repeats, so enough to fill the rest after those.
        std::vector<Station> other_matches = state->db->SearchStationsByCallsignSubstring(
            state->modal_station.callsign, max_suggestions + static_cast<int>(tier1_count));
        AppendNewSuggestions(&state->modal_callsign_suggestions, &other_matches, kMaxSuggestions);

        if (state->modal_callsign_suggestions.size() > kMaxSuggestions)
        {
            state->modal_callsign_suggestions.resize(kMaxSuggestions);
            tier1_count = std::min(tier1_count, kMaxSuggestions);
        }

        for (std::size_t i = 0; i < state->modal_callsign_suggestions.size(); ++i)
        {
            bool is_this_net = i < tier1_count;
            state->modal_callsign_suggestion_sources.push_back(KnownStationSource(is_this_net));
        }

        // Tier 3: licensed stations near the net, from the FCC data.
        AppendNearbyUlsSuggestions(state, state->modal_station.callsign, state->active_net_zip,
                                   !state->active_net_partial_match_canada, kMaxSuggestions,
                                   &state->modal_callsign_suggestions, &state->modal_callsign_suggestion_sources);
        // Tier 4: Canadian call signs, from ISED's data.
        AppendCanadianSuggestions(state, state->modal_station.callsign, state->active_net_partial_match_canada,
                                  kMaxSuggestions, &state->modal_callsign_suggestions,
                                  &state->modal_callsign_suggestion_sources);
        MarkTypedCallsignMatch(state, state->modal_station.callsign, kMaxSuggestions,
                               &state->modal_callsign_suggestions, &state->modal_callsign_suggestion_sources,
                               &state->selected_suggestion_index);
        state->modal_callsign_suggestion_labels = FormatMatches(
            state->modal_callsign_suggestions, state->modal_callsign_suggestion_sources, state->list_width);
    }

    void ApplySelectedCallsignSuggestion(AppState* state)
    {
        if (state->modal_callsign_suggestions.empty())
        {
            return;
        }

        state->modal_station = state->modal_callsign_suggestions[state->selected_suggestion_index];
        // Fill County in now, so it shows in the form as soon as the station
        // is picked rather than only once it's saved.
        BackfillCountyFromZip(state, &state->modal_station);
        BackfillGridFromZip(state, &state->modal_station);

        std::string default_remarks =
            state->db->GetSavedNetStationRemarks(state->active_instance.net_id, state->modal_station.callsign);
        if (!default_remarks.empty())
        {
            state->modal_remarks = std::move(default_remarks);
        }

        state->modal_callsign_suggestions.clear();
        state->modal_callsign_suggestion_labels.clear();
        state->modal_callsign_suggestion_sources.clear();
    }

    void OpenEditNetForm(AppState* state, const Net& net)
    {
        if (RefuseViewOnly(state, "edit nets"))
        {
            return;
        }
        state->edit_net_id = net.id;
        state->edit_net_name = net.name;
        int mode_index = NetModeIndex(net.mode);
        state->edit_net_mode_index = mode_index < 0 ? 0 : mode_index;
        state->edit_net_mode_was_blank = mode_index < 0;
        state->edit_net_frequency = net.default_frequency;
        state->edit_net_offset = net.repeater_offset;
        state->edit_net_tone = net.pl_tone;
        state->edit_net_location = net.default_location;
        state->edit_net_recurrence = net.recurrence_description;
        state->edit_net_comments = net.comments;
        state->edit_net_partial_match_index = net.partial_match_canada ? 1 : 0;

        CloseSavedStationForm(state);
        state->status_message.clear();

        RefreshEditNetSavedStations(state);
        RefreshNearbyZips(state, state->edit_net_location);
    }

    bool SaveEditNetForm(AppState* state)
    {
        if (RefuseViewOnly(state, "edit nets"))
        {
            return false;
        }
        if (state->edit_net_name.empty())
        {
            state->form_error = "Net name is required.";
            return false;
        }
        if (!CheckNetRadio(state, state->edit_net_frequency, state->edit_net_offset, &state->edit_net_tone) ||
            !CheckNetZip(state, state->edit_net_location))
        {
            return false;
        }
        // Only a rename is checked, so a net that already shared its name
        // (from before this rule) can still be saved as it is.
        std::optional<Net> before = state->db->GetNetById(state->edit_net_id);
        if (!before.has_value() || !NetNamesAreTheSame(before->name, state->edit_net_name))
        {
            std::string taken = ExistingNetNamed(state, state->edit_net_name, state->edit_net_id);
            if (!taken.empty())
            {
                state->form_error = "You already have a net named \"" + taken + "\".";
                return false;
            }
        }

        Net net;
        net.id = state->edit_net_id;
        net.name = state->edit_net_name;
        net.mode = NetModes()[static_cast<std::size_t>(state->edit_net_mode_index)];
        net.default_frequency = state->edit_net_frequency;
        net.repeater_offset = state->edit_net_offset;
        net.pl_tone = state->edit_net_tone;
        net.default_location = state->edit_net_location;
        net.recurrence_description = state->edit_net_recurrence;
        net.comments = state->edit_net_comments;
        net.partial_match_canada = state->edit_net_partial_match_index == 1;
        state->db->UpdateNet(net);

        RefreshNets(state);
        state->form_error.clear();
        state->status_message = "Saved " + net.name + ".";
        state->page = kPageNetList;
        return true;
    }

    void RefreshEditNetSavedStations(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        state->edit_net_saved_stations = state->db->GetSavedStationsForNet(state->edit_net_id);

        state->saved_station_cells.clear();
        for (const Station& station : state->edit_net_saved_stations)
        {
            state->saved_station_cells.push_back(
                SavedStationCells(station, state->db->GetSavedNetStationRemarks(state->edit_net_id, station.callsign)));
        }
        state->edit_net_saved_station_labels =
            FormatRows(state->saved_station_cells, SavedStationLayout(state->list_width));

        if (state->selected_saved_station_index >= static_cast<int>(state->edit_net_saved_stations.size()))
        {
            state->selected_saved_station_index = 0;
        }
    }

    bool SaveNetStationForm(AppState* state)
    {
        if (RefuseViewOnly(state, "save stations to nets"))
        {
            return false;
        }
        state->saved_station.callsign = NormalizeCallsign(state->saved_station.callsign);
        if (state->saved_station.callsign.empty())
        {
            state->form_error = "Callsign is required.";
            return false;
        }
        if (!CheckCallsign(state, state->saved_station.callsign))
        {
            return false;
        }
        bool already_saved = false;
        for (const Station& existing : state->edit_net_saved_stations)
        {
            if (CallsignsEqual(existing.callsign, state->saved_station.callsign))
            {
                already_saved = true;
                break;
            }
        }
        // A new one gets what's known about it, even if it wasn't picked
        // from the matches. (One already saved is being edited: a field
        // cleared there is meant to be cleared.)
        if (!already_saved)
        {
            FillSavedStationFromKnownStation(state);
        }

        BackfillCountyFromZip(state, &state->saved_station);
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (already_saved)
        {
            state->db->UpdateSavedNetStation(state->edit_net_id, state->saved_station, state->saved_station_remarks,
                                             now);
        }
        else
        {
            state->db->SaveNetStation(state->edit_net_id, state->saved_station, state->saved_station_remarks, now);
        }

        state->status_message = "Saved " + state->saved_station.callsign + ".";
        state->saved_station = Station();
        state->saved_station_remarks.clear();
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
        state->saved_station_suggestion_sources.clear();
        state->form_error.clear();
        RefreshEditNetSavedStations(state);

        // Re-focus the callsign field for the next entry -- adding several
        // stations in a row is the common case here, and without this the
        // operator would have to Tab all the way back down from wherever
        // focus landed after the save.
        if (state->saved_station_callsign_input)
        {
            state->saved_station_callsign_input->TakeFocus();
        }
        return true;
    }

    void LoadSavedStationIntoForm(AppState* state, const Station& saved)
    {
        CloseSavedStationForm(state);
        state->saved_station = saved;
        state->saved_station_remarks = state->db->GetSavedNetStationRemarks(state->edit_net_id, saved.callsign);
        state->show_saved_station_modal = true;
        if (state->saved_station_callsign_input)
        {
            state->saved_station_callsign_input->TakeFocus();
        }
    }

    void OpenNewSavedStationForm(AppState* state)
    {
        if (RefuseViewOnly(state, "save stations to nets"))
        {
            return;
        }
        CloseSavedStationForm(state);
        state->status_message.clear();
        state->show_saved_station_modal = true;
        if (state->saved_station_callsign_input)
        {
            state->saved_station_callsign_input->TakeFocus();
        }
    }

    void CloseSavedStationForm(AppState* state)
    {
        state->saved_station = Station();
        state->saved_station_remarks.clear();
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
        state->saved_station_suggestion_sources.clear();
        state->form_error.clear();
        state->show_saved_station_modal = false;
    }

    void RemoveSelectedSavedNetStation(AppState* state)
    {
        if (RefuseViewOnly(state, "remove saved stations"))
        {
            return;
        }
        if (state->edit_net_saved_stations.empty())
        {
            state->form_error = "No saved stations to remove.";
            return;
        }

        std::string callsign = state->edit_net_saved_stations[state->selected_saved_station_index].callsign;
        state->db->RemoveSavedNetStation(state->edit_net_id, callsign);
        state->form_error.clear();
        state->status_message = state->db->FindStationByCallsign(callsign).has_value()
                                    ? "Removed " + callsign + " from this net's saved stations."
                                    : "Removed " + callsign +
                                          "; it wasn't used anywhere else, so its details were "
                                          "deleted too.";
        RefreshEditNetSavedStations(state);
    }

    void DeleteSelectedHistoryCheckIn(AppState* state)
    {
        if (RefuseViewOnly(state, "delete check-ins"))
        {
            return;
        }
        if (state->history_check_ins.empty() ||
            state->selected_history_index >= static_cast<int>(state->history_instances.size()))
        {
            state->form_error = "No check-in to delete.";
            return;
        }

        const CheckIn& check_in = state->history_check_ins[state->selected_history_check_in_index];
        if (check_in.designated_role != kRoleNone)
        {
            state->db->SetNetInstanceRoleCallsign(state->history_instances[state->selected_history_index].id,
                                                  check_in.designated_role, "");
        }
        state->db->DeleteCheckIn(check_in.id);
        state->form_error.clear();
        RefreshNetHistory(state);
    }

    void RefreshNearbyZips(AppState* state, const std::string& net_zip)
    {
        // The same ZIPs and radius as last time, with an origin found: all
        // as it was. (With none found, ZIP data loaded since may have one.)
        if (!state->nearby_zips_origin.empty() && net_zip == state->nearby_zips_net_zip &&
            state->settings.location == state->nearby_zips_home_zip &&
            state->settings.nearby_radius_miles == state->nearby_zips_radius && !state->nearby_zips.empty())
        {
            return;
        }
        state->nearby_zips_net_zip = net_zip;
        state->nearby_zips_home_zip = state->settings.location;
        std::optional<ZipCentroid> origin;
        if (IsFiveDigitZip(net_zip))
        {
            origin = state->db->FindZipCentroid(net_zip);
        }
        if (!origin.has_value() && IsFiveDigitZip(state->settings.location))
        {
            origin = state->db->FindZipCentroid(state->settings.location);
        }
        std::string origin_zip = origin.has_value() ? origin->zip : "";

        int radius = state->settings.nearby_radius_miles;
        if (state->nearby_zips_origin == origin_zip && state->nearby_zips_radius == radius &&
            !state->nearby_zips.empty())
        {
            return;
        }
        state->nearby_zips.clear();
        state->nearby_zip3_prefixes.clear();
        state->nearby_zips_origin = origin_zip;
        state->nearby_zips_radius = radius;
        // The nearby licensees loaded for the old ZIPs no longer apply.
        state->nearby_uls_origin.clear();
        if (!origin.has_value())
        {
            return;
        }
        state->nearby_zips_origin_lat = origin->lat;
        state->nearby_zips_origin_lon = origin->lon;

        // Only the centroids in a box around the origin that holds the
        // whole radius (a degree of latitude is about 69 miles; a degree of
        // longitude, that times the cosine of the latitude) are candidates.
        double lat_degrees = radius / 69.0;
        double cos_lat = std::cos(origin->lat * 3.14159265358979323846 / 180.0);
        double lon_degrees = cos_lat > 0.01 ? radius / (69.0 * cos_lat) : 180.0;
        std::vector<ZipCentroid> candidates = state->db->GetZipCentroidsInBox(
            origin->lat - lat_degrees, origin->lat + lat_degrees, origin->lon - lon_degrees, origin->lon + lon_degrees);
        state->nearby_zips = NearbyZips(origin->lat, origin->lon, radius, candidates);
        state->nearby_zip3_prefixes = NearbyZip3Prefixes(origin->lat, origin->lon, radius, candidates);
    }

    // How long AppState::nearby_uls_callsigns is trusted before it's
    // reloaded, so a long session picks up the weekly station data refresh.
    static constexpr std::int64_t kNearbyUlsReloadSeconds = 3600;

    // Autocomplete's last tier, shared by the New Station modal and the
    // saved-station form: ULS-imported stations matching `typed` whose ZIP is
    // within the operator's Nearby Radius of the net's ZIP (`net_zip`) or,
    // failing that, the operator's own, nearest first (see RefreshNearbyZips
    // and Database::SearchNearbyUlsStations). Matched in memory against
    // AppState::nearby_uls_callsigns, so only the matches are read from the
    // database. Appended after whatever `suggestions` already holds (the
    // this-net and other-nets tiers), skipping callsigns already there,
    // until `max_suggestions` is reached. Nothing is added if neither ZIP is
    // recognized. With `partial` (Partial Matching set to US) a call sign
    // matches wherever `typed` appears in it; without, only if it starts
    // with it.
    // True if `upper` is part of `candidate`'s callsign (anywhere, or only
    // at its start unless `anywhere`). Checked in place on the fixed,
    // zero-padded 8 bytes: this runs for every nearby licensee on every
    // keystroke, so no strlen, string or string_view is made per candidate.
    static bool NearbyCallsignMatches(const NearbyUlsCallsign& candidate, const std::string& upper, bool anywhere)
    {
        constexpr std::size_t kField = sizeof(candidate.callsign);
        std::size_t length = upper.size();
        if (length == 0 || length > kField)
        {
            return false;
        }
        const char* callsign = candidate.callsign;
        char first = upper[0];
        std::size_t last_start = anywhere ? kField - length : 0;
        for (std::size_t start = 0; start <= last_start; ++start)
        {
            // A zero byte is the end of the callsign: nothing further along.
            if (callsign[start] == '\0')
            {
                return false;
            }
            if (callsign[start] == first && std::memcmp(callsign + start, upper.data(), length) == 0)
            {
                return true;
            }
        }
        return false;
    }

    static void AppendNearbyUlsSuggestions(AppState* state, const std::string& typed, const std::string& net_zip,
                                           bool partial, std::size_t max_suggestions, std::vector<Station>* suggestions,
                                           std::vector<std::string>* sources)
    {
        if (suggestions->size() >= max_suggestions)
        {
            return;
        }
        RefreshNearbyZips(state, net_zip);
        if (state->nearby_zips.empty())
        {
            return;
        }

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (state->nearby_uls_origin != state->nearby_zips_origin ||
            now - state->nearby_uls_loaded_at > kNearbyUlsReloadSeconds)
        {
            state->nearby_uls_callsigns =
                state->db->ListNearbyUlsCallsigns(state->nearby_zips, state->nearby_zip3_prefixes);
            state->nearby_uls_callsigns.shrink_to_fit();
            state->nearby_uls_origin = state->nearby_zips_origin;
            state->nearby_uls_loaded_at = now;
        }

        std::string upper = ToUpperAscii(typed);
        for (const NearbyUlsCallsign& candidate : state->nearby_uls_callsigns)
        {
            if (suggestions->size() >= max_suggestions)
            {
                break;
            }
            if (!NearbyCallsignMatches(candidate, upper, partial))
            {
                continue;
            }
            bool already_known = false;
            for (const Station& existing : *suggestions)
            {
                if (existing.callsign == candidate.callsign)
                {
                    already_known = true;
                    break;
                }
            }
            if (already_known)
            {
                continue;
            }
            // Gone if the station data was refreshed since the list loaded.
            std::optional<Station> station = state->db->FindUlsStationByCallsign(candidate.callsign);
            if (!station.has_value())
            {
                continue;
            }
            suggestions->push_back(*station);
            sources->push_back(UlsSource(candidate.miles));
        }
    }

    // True if `typed` can only be (the start of) a Canadian call sign: in
    // ISED's data they all start VA, VE, VO or VY, and no US call sign
    // starts with V.
    static bool LooksCanadian(const std::string& typed)
    {
        std::string upper = NormalizeCallsign(typed);
        return !upper.empty() && upper[0] == 'V';
    }

    // Autocomplete's Canadian tier, after the FCC one, shared like it by the
    // New Station modal and the saved-station form: ISED's call signs
    // matching what's typed, in order -- no distance, as ISED's data has no
    // location to measure from. With `partial` (Partial Matching set to
    // Canada) that's call signs containing it anywhere; without, only
    // those starting with it, looked up only for a call sign that looks
    // Canadian (LooksCanadian). Skips callsigns already in `suggestions`;
    // stops at `max_suggestions`.
    static void AppendCanadianSuggestions(AppState* state, const std::string& typed, bool partial,
                                          std::size_t max_suggestions, std::vector<Station>* suggestions,
                                          std::vector<std::string>* sources)
    {
        std::string normalized = NormalizeCallsign(typed);
        if (suggestions->size() >= max_suggestions || normalized.empty() || (!partial && !LooksCanadian(typed)))
        {
            return;
        }
        int limit = static_cast<int>(max_suggestions);
        std::vector<Station> matches = partial ? state->db->SearchIsedStationsByCallsignSubstring(normalized, limit)
                                               : state->db->SearchIsedStationsByCallsignPrefix(normalized, limit);
        for (const Station& match : matches)
        {
            if (suggestions->size() >= max_suggestions)
            {
                break;
            }
            bool already_known = false;
            for (const Station& existing : *suggestions)
            {
                already_known = already_known || existing.callsign == match.callsign;
            }
            if (!already_known)
            {
                suggestions->push_back(match);
                sources->push_back(kIsedSource);
            }
        }
    }

    void RefreshSavedStationSuggestions(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
        state->saved_station_suggestion_sources.clear();
        state->selected_saved_station_suggestion_index = 0;

        if (state->saved_station.callsign.empty())
        {
            return;
        }

        const std::size_t kMaxSuggestions = MaxCallsignMatches(state);

        // Tier 1: callers already known to this specific net (real check-ins
        // or previously saved) -- same query as the New Station modal's tier 1.
        const int max_suggestions = static_cast<int>(kMaxSuggestions);
        state->saved_station_suggestions = state->db->SearchNetStationsByCallsignSubstring(
            state->edit_net_id, state->saved_station.callsign, max_suggestions);
        std::size_t tier1_count = state->saved_station_suggestions.size();

        // Tier 2: callers known to other nets; as in RefreshCallsignSuggestions,
        // enough to fill the rest after tier 1's repeats.
        std::vector<Station> other_matches = state->db->SearchStationsByCallsignSubstring(
            state->saved_station.callsign, max_suggestions + static_cast<int>(tier1_count));
        AppendNewSuggestions(&state->saved_station_suggestions, &other_matches, kMaxSuggestions);

        if (state->saved_station_suggestions.size() > kMaxSuggestions)
        {
            state->saved_station_suggestions.resize(kMaxSuggestions);
            tier1_count = std::min(tier1_count, kMaxSuggestions);
        }

        for (std::size_t i = 0; i < state->saved_station_suggestions.size(); ++i)
        {
            bool is_this_net = i < tier1_count;
            state->saved_station_suggestion_sources.push_back(KnownStationSource(is_this_net));
        }

        // Tier 3: nearby ULS-imported stations.
        AppendNearbyUlsSuggestions(state, state->saved_station.callsign, state->edit_net_location,
                                   state->edit_net_partial_match_index == 0, kMaxSuggestions,
                                   &state->saved_station_suggestions, &state->saved_station_suggestion_sources);
        // And Canadian call signs, from ISED's data.
        AppendCanadianSuggestions(state, state->saved_station.callsign, state->edit_net_partial_match_index == 1,
                                  kMaxSuggestions, &state->saved_station_suggestions,
                                  &state->saved_station_suggestion_sources);
        MarkTypedCallsignMatch(state, state->saved_station.callsign, kMaxSuggestions, &state->saved_station_suggestions,
                               &state->saved_station_suggestion_sources,
                               &state->selected_saved_station_suggestion_index);
        state->saved_station_suggestion_labels =
            FormatMatches(state->saved_station_suggestions, state->saved_station_suggestion_sources, state->list_width);
    }

    void ApplySelectedSavedStationSuggestion(AppState* state)
    {
        if (state->saved_station_suggestions.empty())
        {
            FillSavedStationFromKnownStation(state);
            return;
        }
        // The match marked ">" (see MarkTypedCallsignMatch).
        state->saved_station = state->saved_station_suggestions[state->selected_saved_station_suggestion_index];
        // Fill County in now, so it shows in the form as soon as the station
        // is picked rather than only once it's saved. (ULS records never
        // carry a county, so a ULS suggestion always needs this.)
        BackfillCountyFromZip(state, &state->saved_station);
        BackfillGridFromZip(state, &state->saved_station);
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
        state->saved_station_suggestion_sources.clear();
    }

    void BackfillCountyFromZip(AppState* state, Station* station)
    {
        if (!station->county.empty() || station->zip.empty())
        {
            return;
        }

        // A handful of already-persisted stations predate uls_import.cpp's
        // NormalizeZip5 fix and still carry a 9-digit ZIP+4 (e.g.
        // "374152623") rather than a plain 5-digit ZIP -- the tables are
        // keyed on the latter, so truncate defensively here too rather than
        // only at import time.
        std::string zip5 = station->zip.size() > 5 ? station->zip.substr(0, 5) : station->zip;

        if (!station->city.empty())
        {
            std::string county = state->db->FindZipPlaceCounty(zip5, NormalizePlaceName(station->city));
            if (!county.empty())
            {
                station->county = county;
                return;
            }
        }
        station->county = state->db->FindZipCounty(zip5);
    }

    void BackfillGridFromZip(AppState* state, Station* station)
    {
        if (!station->grid_square.empty() || station->zip.size() < 5)
        {
            return;
        }
        // A US ZIP is 5 digits, or 9 with the +4; a Canadian postal code
        // is not, and has no centroid.
        std::string zip5 = station->zip.substr(0, 5);
        for (char c : zip5)
        {
            if (c < '0' || c > '9')
            {
                return;
            }
        }
        std::optional<ZipCentroid> centroid = state->db->FindZipCentroid(zip5);
        if (centroid.has_value())
        {
            station->grid_square = MaidenheadGrid4(centroid->lat, centroid->lon);
        }
    }

    // ---- The seldom-used windows ------------------------------------------------

    // The room a window's table gets: the terminal less the window's and
    // the table's borders, the marker column and a margin. Never less than
    // at 80 columns.
    static int InfoTableWidth(int terminal_width)
    {
        return std::max(80, terminal_width) - 10;
    }
    static constexpr int kInfoTableWidthAt80 = 70;

    // The open window's columns, each but the last widening no further than
    // its longest entry needs, so on a wide terminal the room left over goes
    // to the last column (Remarks, or Comment) rather than to blank space
    // after short net names. The last column takes all of that room.
    static std::vector<ListColumn> FitInfoColumns(const AppState* state, int available)
    {
        std::vector<ListColumn> columns = state->info_columns;
        for (std::size_t column = 0; column + 1 < columns.size(); ++column)
        {
            int needed = TextWidth(columns[column].heading);
            for (std::size_t row = 0; row < state->info_cells.size(); ++row)
            {
                if (column >= state->info_cells[row].size())
                {
                    continue;
                }
                int width = TextWidth(state->info_cells[row][column]);
                if (column == state->info_tag_column && row < state->info_cell_tags.size())
                {
                    width += TextWidth(state->info_cell_tags[row]);
                }
                needed = std::max(needed, width);
            }
            columns[column].max_width = std::max(columns[column].width, std::min(columns[column].max_width, needed));
        }
        if (!columns.empty())
        {
            columns.back().max_width = std::max(columns.back().max_width, available);
        }
        return columns;
    }

    // Lays the open window's table out for AppState::list_width (it widens
    // with the terminal, like the lists).
    static void FormatInfoTable(AppState* state)
    {
        state->info_header.clear();
        state->info_rows.clear();
        if (state->info_columns.empty())
        {
            return;
        }
        int width = InfoTableWidth(state->list_width);
        ListLayout layout = LayOutList(FitInfoColumns(state, width), width, kInfoTableWidthAt80, 2);
        // Cut to the table's width: a long last column would otherwise make
        // the list scroll sideways to show it, hiding the first columns.
        // On a wider terminal, padded to the width the columns take, so the
        // window is as wide as its last column. (At 80 it's as it always
        // was.)
        state->info_header = CutToWidth(FormatListHeading(state->info_columns, layout), width);
        int used = 0;
        for (int column_width : layout.widths)
        {
            used += column_width > 0 ? column_width + (used > 0 ? layout.gap : 0) : 0;
        }
        int header_width = TextWidth(state->info_header);
        if (width > kInfoTableWidthAt80 && header_width < used)
        {
            state->info_header.append(static_cast<std::size_t>(used - header_width), ' ');
        }
        if (state->info_cell_tags.empty())
        {
            state->info_rows = FormatRows(state->info_cells, layout);
        }
        else
        {
            std::vector<std::vector<std::string>> tagged = state->info_cells;
            std::size_t column = state->info_tag_column;
            int room = column < layout.widths.size() ? layout.widths[column] : 0;
            for (std::size_t i = 0; i < tagged.size() && i < state->info_cell_tags.size(); ++i)
            {
                const std::string& tag = state->info_cell_tags[i];
                if (tag.empty() || column >= tagged[i].size())
                {
                    continue;
                }
                std::string& text = tagged[i][column];
                int tag_width = TextWidth(tag);
                if (room > tag_width && TextWidth(text) + tag_width > room)
                {
                    text = CutToWidth(text, room - tag_width);
                }
                text += tag;
            }
            state->info_rows = FormatRows(tagged, layout);
        }
        for (std::string& row : state->info_rows)
        {
            row = CutToWidth(row, width);
        }
    }

    static void ShowInfoWindow(AppState* state, InfoWindow window, const std::string& title,
                               std::vector<std::string> summary, std::vector<ListColumn> columns,
                               std::vector<std::vector<std::string>> cells)
    {
        state->info_window = window;
        state->show_info_window = true;
        state->info_title = title;
        state->info_summary = std::move(summary);
        state->info_columns = std::move(columns);
        state->info_cells = std::move(cells);
        state->info_cell_tags.clear();
        state->info_selected = 0;
        state->form_error.clear();
        state->status_message.clear();
        FormatInfoTable(state);
    }

    void CloseInfoWindow(AppState* state)
    {
        state->info_window = InfoWindow::kNone;
        state->show_info_window = false;
        state->info_summary.clear();
        state->info_header.clear();
        state->info_rows.clear();
        state->info_columns.clear();
        state->info_cells.clear();
        state->info_cell_tags.clear();
        state->info_stations.clear();
        state->info_query.clear();
        state->info_selected = 0;
    }

    void MoveInfoSelection(AppState* state, int delta)
    {
        int last = static_cast<int>(state->info_rows.size()) - 1;
        int moved = state->info_selected + delta;
        state->info_selected = moved < 0 ? 0 : (moved > last ? std::max(last, 0) : moved);
    }

    static std::string StationName(Database* db, const std::string& callsign)
    {
        std::optional<Station> station = db->FindStationByCallsign(callsign);
        return station.has_value() ? station->name : "";
    }

    // "13 of the last 20", "1 of 1".
    static std::string OutOf(int part, int whole)
    {
        return std::to_string(part) + " of " + std::to_string(whole);
    }

    // The active net's other sessions, newest first.
    static std::vector<NetInstance> OtherSessions(AppState* state)
    {
        std::vector<NetInstance> sessions;
        for (const NetInstance& session : state->db->GetNetInstancesForNet(state->active_instance.net_id))
        {
            if (session.id != state->active_instance.id)
            {
                sessions.push_back(session);
            }
        }
        return sessions;
    }

    void OpenStationHistory(AppState* state, const CheckIn& check_in)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        std::vector<StationCheckInRecord> records =
            state->db->GetStationCheckInsForNet(state->active_instance.net_id, check_in.callsign);
        std::vector<std::vector<std::string>> rows;
        std::vector<std::int64_t> sessions_in;
        std::string first_date;
        for (const StationCheckInRecord& record : records)
        {
            if (record.instance.id == state->active_instance.id)
            {
                continue;
            }
            sessions_in.push_back(record.instance.id);
            first_date = record.instance.instance_date;
            rows.push_back({record.instance.instance_date, FormatLocalTimeOfDay(record.instance.started_at),
                            std::to_string(record.check_in.sequence_number),
                            RoleAbbreviation(record.check_in.designated_role), record.check_in.signal_report,
                            record.check_in.remarks, record.check_in.comment});
        }

        std::vector<std::string> summary;
        std::string name = StationName(state->db, check_in.callsign);
        if (!name.empty())
        {
            summary.push_back(name);
        }
        if (rows.empty())
        {
            summary.emplace_back("No other check-ins to this net.");
        }
        else
        {
            // Of the net's most recent sessions (not counting this one), how
            // many this station was in.
            std::vector<NetInstance> sessions = OtherSessions(state);
            int recent = 0;
            int recent_in = 0;
            for (std::size_t i = 0; i < sessions.size() && recent < 20; ++i)
            {
                ++recent;
                if (std::find(sessions_in.begin(), sessions_in.end(), sessions[i].id) != sessions_in.end())
                {
                    ++recent_in;
                }
            }
            summary.push_back("Checked in to " + OutOf(recent_in, recent) +
                              " of this net's most recent other sessions; " + std::to_string(rows.size()) +
                              " in all since " + first_date + ".");
        }
        ShowInfoWindow(state, InfoWindow::kStationHistory, "Station History: " + check_in.callsign, std::move(summary),
                       {{"Date", 10, 10, 0, 0},
                        {"Start", 8, 8, 0, 0},
                        {"#", 3, 3, 0, 0},
                        {"Role", 5, 5, 0, 0},
                        {"Signal", 6, 6, 1, 0},
                        {"Remarks", 20, 24, 0, 3},
                        {"Comment", 12, 40, 2, 0}},
                       std::move(rows));
    }

    void OpenRegulars(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        std::vector<NetInstance> sessions = OtherSessions(state);
        if (sessions.size() > 10)
        {
            sessions.resize(10);
        }
        std::vector<std::string> callsigns;
        std::vector<int> counts;
        std::vector<std::string> last_seen;
        for (const NetInstance& session : sessions)
        {
            std::vector<std::string> in_session;
            for (const CheckIn& check_in : state->db->GetCheckInsForNetInstance(session.id))
            {
                if (std::find(in_session.begin(), in_session.end(), check_in.callsign) != in_session.end())
                {
                    continue;  // Counted once per session.
                }
                in_session.push_back(check_in.callsign);
                std::vector<std::string>::iterator found =
                    std::find(callsigns.begin(), callsigns.end(), check_in.callsign);
                if (found == callsigns.end())
                {
                    callsigns.push_back(check_in.callsign);
                    counts.push_back(1);
                    last_seen.push_back(session.instance_date);  // Newest first.
                }
                else
                {
                    ++counts[static_cast<std::size_t>(found - callsigns.begin())];
                }
            }
        }

        // Regulars: at least half the sessions looked at, not yet here.
        std::vector<std::size_t> regulars;
        int total = static_cast<int>(sessions.size());
        for (std::size_t i = 0; i < callsigns.size(); ++i)
        {
            bool here = false;
            for (const CheckIn& check_in : state->active_check_ins)
            {
                here = here || check_in.callsign == callsigns[i];
            }
            if (!here && counts[i] * 2 >= total)
            {
                regulars.push_back(i);
            }
        }
        // Most regular first.
        for (std::size_t i = 1; i < regulars.size(); ++i)
        {
            std::size_t j = i;
            while (j > 0 && (counts[regulars[j]] > counts[regulars[j - 1]] ||
                             (counts[regulars[j]] == counts[regulars[j - 1]] &&
                              callsigns[regulars[j]] < callsigns[regulars[j - 1]])))
            {
                std::swap(regulars[j], regulars[j - 1]);
                --j;
            }
        }

        std::vector<std::vector<std::string>> rows;
        state->info_stations.clear();
        for (std::size_t index : regulars)
        {
            std::optional<Station> found = state->db->FindStationByCallsign(callsigns[index]);
            Station station = found.has_value() ? *found : Station();
            station.callsign = callsigns[index];
            rows.push_back({station.callsign, station.name, OutOf(counts[index], total), last_seen[index]});
            state->info_stations.push_back(station);
        }

        std::vector<std::string> summary;
        if (total == 0)
        {
            summary.emplace_back("This net has no earlier sessions yet.");
        }
        else if (rows.empty())
        {
            summary.push_back("Every station that checked in to at least half of the last " + std::to_string(total) +
                              " sessions has checked in.");
        }
        else
        {
            summary.push_back("Checked in to at least half of the last " + std::to_string(total) +
                              " sessions, but not yet to this one. Enter checks the highlighted "
                              "one in.");
        }
        std::vector<Station> stations = std::move(state->info_stations);
        ShowInfoWindow(
            state, InfoWindow::kRegulars, "Regulars Not Yet Heard", std::move(summary),
            {{"Callsign", 10, 10, 0, 0}, {"Name", 24, 30, 0, 1}, {"Sessions", 8, 8, 0, 0}, {"Last Seen", 10, 10, 0, 0}},
            std::move(rows));
        state->info_stations = std::move(stations);
    }

    void CheckInSelectedRegular(AppState* state)
    {
        if (state->viewing_only || state->view_only_user || state->info_stations.empty() ||
            state->info_selected >= static_cast<int>(state->info_stations.size()))
        {
            return;
        }
        // Moved out: closing the window clears the list anyway.
        Station station = std::move(state->info_stations[static_cast<std::size_t>(state->info_selected)]);
        CloseInfoWindow(state);
        if (!EnsureActiveSessionOpen(state, ""))
        {
            return;
        }
        ClearModalFields(state);
        state->modal_station = std::move(station);
        BackfillCountyFromZip(state, &state->modal_station);
        state->modal_remarks =
            state->db->GetSavedNetStationRemarks(state->active_instance.net_id, state->modal_station.callsign);
        state->show_new_station_modal = true;
        if (state->modal_callsign_input)
        {
            state->modal_callsign_input->TakeFocus();
        }
    }

    void OpenStationCard(AppState* state, const std::string& callsign)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        std::optional<Station> known = state->db->FindStationByCallsign(callsign);
        std::optional<Station> licensed = state->db->FindLicensedStationByCallsign(callsign);
        Station station = known.has_value() ? *known : (licensed.has_value() ? *licensed : Station());
        StationActivity activity = state->db->GetStationActivity(callsign);

        std::vector<std::string> summary;
        std::string address = station.street_address;
        std::string place = CityAndState(station);
        if (!station.zip.empty())
        {
            place += (place.empty() ? "" : " ") + station.zip;
        }
        summary.push_back("Name:           " + station.name);
        summary.push_back("Address:        " + address + (address.empty() || place.empty() ? "" : ", ") + place);
        summary.push_back("County:         " + station.county);
        summary.push_back("Grid Square:    " + station.grid_square);
        summary.push_back("Member ID:      " + station.member_id);
        summary.push_back("License Class:  " + (licensed.has_value() && !licensed->license_class.empty()
                                                    ? licensed->license_class
                                                    : std::string("(not in the FCC or ISED data)")));
        if (activity.check_ins == 0)
        {
            summary.emplace_back("Check-ins:      none yet");
        }
        else
        {
            summary.push_back("Check-ins:      " + std::to_string(activity.check_ins) + " (first " +
                              FormatLocalDate(activity.first_at) + ", latest " + FormatLocalDate(activity.last_at) +
                              ")");
        }
        std::string nets;
        for (const std::string& net : activity.saved_to_nets)
        {
            nets += (nets.empty() ? "" : ", ") + net;
        }
        summary.push_back("Saved to:       " + (nets.empty() ? std::string("no nets") : nets));
        ShowInfoWindow(state, InfoWindow::kStationCard, "Station: " + callsign, std::move(summary), {}, {});
    }

    void OpenSessionSummary(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        std::vector<std::vector<std::string>> rows;
        for (const CheckIn& check_in : state->active_check_ins)
        {
            bool first_time = true;
            for (const StationCheckInRecord& record :
                 state->db->GetStationCheckInsForNet(state->active_instance.net_id, check_in.callsign))
            {
                first_time = first_time && record.instance.id == state->active_instance.id;
            }
            if (first_time)
            {
                rows.push_back({std::to_string(check_in.sequence_number), check_in.callsign,
                                StationName(state->db, check_in.callsign)});
            }
        }

        std::vector<std::string> summary;
        summary.push_back(CountCheckIns(state->active_check_ins.size()) + " so far" +
                          (state->active_instance.started_at > 0
                               ? " (started " + FormatLocalTimeOfDay(state->active_instance.started_at) + ")."
                               : std::string(".")));
        std::vector<NetInstance> sessions = OtherSessions(state);
        if (sessions.size() > 10)
        {
            sessions.resize(10);
        }
        if (!sessions.empty())
        {
            std::int64_t sum = 0;
            std::int64_t most = 0;
            for (const NetInstance& session : sessions)
            {
                std::int64_t count = 0;
                std::int64_t newest_id = 0;
                state->db->GetCheckInSummary(session.id, &count, &newest_id);
                sum += count;
                most = std::max(most, count);
            }
            char average[32];
            std::snprintf(average, sizeof(average), "%.1f",
                          static_cast<double>(sum) / static_cast<double>(sessions.size()));
            summary.push_back("The last " + std::to_string(sessions.size()) + " sessions averaged " + average +
                              " check-ins (most " + std::to_string(most) + ").");
        }
        summary.push_back(rows.empty() ? "No first-timers yet."
                                       : std::to_string(rows.size()) + " checking in to this net for the first time:");
        // Decided before the call: `rows` is moved into it, and the order the
        // arguments are evaluated in isn't fixed.
        bool no_first_timers = rows.empty();
        ShowInfoWindow(
            state, InfoWindow::kSessionSummary, "Session Summary: " + state->active_net_name, std::move(summary),
            no_first_timers
                ? std::vector<ListColumn>()
                : std::vector<ListColumn>({{"#", 3, 3, 0, 0}, {"Callsign", 10, 10, 0, 0}, {"Name", 30, 30, 0, 0}}),
            std::move(rows));
    }

    void OpenNetStatistics(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        if (state->history_ad_hoc || state->selected_net_index >= static_cast<int>(state->nets.size()))
        {
            return;
        }
        const Net& net = state->nets[static_cast<std::size_t>(state->selected_net_index)];
        std::vector<NetInstance> sessions = state->db->GetNetInstancesForNet(net.id);

        std::vector<std::string> summary;
        if (sessions.empty())
        {
            summary.emplace_back("No sessions yet.");
            ShowInfoWindow(state, InfoWindow::kNetStatistics, "Net Statistics: " + net.name, std::move(summary), {},
                           {});
            return;
        }
        std::int64_t sum = 0;
        std::int64_t most = -1;
        std::string most_date;
        // Months, newest first: "2026-09", sessions, check-ins.
        std::vector<std::string> months;
        std::vector<int> month_sessions;
        std::vector<std::int64_t> month_check_ins;
        for (const NetInstance& session : sessions)
        {
            std::int64_t count = 0;
            std::int64_t newest_id = 0;
            state->db->GetCheckInSummary(session.id, &count, &newest_id);
            sum += count;
            if (count > most)
            {
                most = count;
                most_date = session.instance_date;
            }
            std::string month = session.instance_date.substr(0, 7);
            if (months.empty() || months.back() != month)
            {
                months.push_back(month);
                month_sessions.push_back(0);
                month_check_ins.push_back(0);
            }
            ++month_sessions.back();
            month_check_ins.back() += count;
        }
        char average[32];
        std::snprintf(average, sizeof(average), "%.1f",
                      static_cast<double>(sum) / static_cast<double>(sessions.size()));
        summary.push_back(std::to_string(sessions.size()) + " sessions, " + sessions.back().instance_date + " to " +
                          sessions.front().instance_date + "; " + std::to_string(sum) + " check-ins in all.");
        summary.push_back(std::string("Average ") + average + " check-ins a session; most " + std::to_string(most) +
                          ", on " + most_date + ".");
        std::string by_month = "Recent months:";
        for (std::size_t i = 0; i < months.size() && i < 6; ++i)
        {
            by_month += (i == 0 ? " " : "; ") + months[i] + " " + std::to_string(month_sessions[i]) +
                        (month_sessions[i] == 1 ? " session/" : " sessions/") + std::to_string(month_check_ins[i]);
        }
        summary.push_back(by_month + " check-ins.");
        summary.emplace_back("Most frequent stations:");

        std::vector<std::vector<std::string>> rows;
        for (const CallsignTally& tally : state->db->GetTopCallsignsForNet(net.id, 15))
        {
            rows.push_back(
                {tally.callsign, StationName(state->db, tally.callsign), std::to_string(tally.count), tally.last_date});
        }
        ShowInfoWindow(
            state, InfoWindow::kNetStatistics, "Net Statistics: " + net.name, std::move(summary),
            {{"Callsign", 10, 10, 0, 0}, {"Name", 24, 30, 0, 1}, {"Check-ins", 9, 9, 0, 0}, {"Last", 10, 10, 0, 0}},
            std::move(rows));
    }

    // How many check-ins the station search shows at most.
    static constexpr int kStationSearchLimit = 200;

    void RefreshStationSearch(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        state->info_query = NormalizeCallsign(state->info_query);
        state->info_cells.clear();
        state->info_cell_tags.clear();
        state->info_selected = 0;
        state->info_summary.clear();
        if (state->info_query.size() < 3)
        {
            state->info_summary.emplace_back(
                "Type at least 3 characters of a callsign to see its check-ins to every net.");
            FormatInfoTable(state);
            return;
        }
        std::vector<StationCheckInRecord> records =
            state->db->FindCheckInsByCallsign(state->info_query, kStationSearchLimit);
        for (const StationCheckInRecord& record : records)
        {
            state->info_cells.push_back({record.instance.instance_date, record.net_name, record.check_in.callsign,
                                         StationName(state->db, record.check_in.callsign),
                                         RoleAbbreviation(record.check_in.designated_role), record.check_in.remarks});
            // Ad hoc nets aren't on the Recurring Nets list, so say which
            // these are (see the Ad Hoc page for them).
            state->info_cell_tags.push_back(record.net_is_ad_hoc ? " (ad hoc)" : "");
        }
        state->info_tag_column = 1;
        state->info_summary.push_back(records.empty() ? std::string("No check-ins found.")
                                      : records.size() >= static_cast<std::size_t>(kStationSearchLimit)
                                          ? "The newest " + std::to_string(kStationSearchLimit) + " check-ins found:"
                                          : std::to_string(records.size()) + " check-ins found:");
        FormatInfoTable(state);
    }

    void OpenStationSearch(AppState* state)
    {
        ShowInfoWindow(state, InfoWindow::kStationSearch, "Find a Station", {},
                       {{"Date", 10, 10, 0, 0},
                        {"Net", 16, 30, 0, 2},
                        {"Callsign", 10, 13, 0, 99},
                        {"Name", 20, 24, 1, 3},
                        {"Role", 5, 5, 0, 0},
                        {"Remarks", 10, 40, 0, 4}},
                       {});
        state->info_query.clear();
        RefreshStationSearch(state);
    }

    void OpenQuietStations(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        std::string cutoff = FormatLocalDate(static_cast<std::int64_t>(std::time(nullptr)) - 182 * 24 * 3600);
        std::vector<CallsignTally> tallies = state->db->GetSavedStationActivity(state->edit_net_id);
        std::vector<std::vector<std::string>> rows;
        for (const CallsignTally& tally : tallies)
        {
            if (!tally.last_date.empty() && tally.last_date >= cutoff)
            {
                continue;
            }
            rows.push_back({tally.callsign, StationName(state->db, tally.callsign),
                            tally.last_date.empty() ? "never" : tally.last_date, std::to_string(tally.count)});
        }
        std::vector<std::string> summary;
        summary.push_back(OutOf(static_cast<int>(rows.size()), static_cast<int>(tallies.size())) +
                          " saved stations haven't checked in to this net since " + cutoff +
                          ". To remove one, close this window and use F4.");
        ShowInfoWindow(
            state, InfoWindow::kQuietStations, "Quiet Saved Stations", std::move(summary),
            {{"Callsign", 10, 10, 0, 0}, {"Name", 24, 30, 0, 1}, {"Last", 10, 10, 0, 0}, {"Check-ins", 9, 9, 0, 0}},
            std::move(rows));
    }

    // One line of the Help window: a key and what it does; `extra` marks a
    // seldom-used key that's only on the key bar of a wide enough terminal.
    struct HelpLine
    {
        const char* key;
        const char* what;
        bool extra;
    };

    static std::vector<HelpLine> HelpFor(const AppState* state)
    {
        // A view-only user's pages, with only the keys they have (see
        // AppState::view_only_user).
        if (state->view_only_user)
        {
            switch (state->page)
            {
                case kPageNetList:
                    return {
                        {"F3/Enter", "View the highlighted net's open session.", false},
                        {"F4", "Settings: your callsign, home ZIP and time format.", false},
                        {"F5", "Ad hoc nets: view an open one, or see their history.", false},
                        {"F6", "History of the highlighted net: view and export.", false},
                        {"F8", "Export the highlighted net to a file to share.", false},
                        {"F10", "Quit.", false},
                        {"Up/Down", "Move the highlight.", false},
                    };
                case kPageAdHocNet:
                    return {
                        {"F3", "View an ad hoc session that's open (by number).", false},
                        {"F6", "History of every ad hoc net.", false},
                        {"Esc", "Back to the net list.", false},
                    };
                case kPageNetHistory:
                    return {
                        {"Up/Down", "Choose a session; its check-ins show below.", false},
                        {"F7", "Export the highlighted session: log, .qlsession, ADIF (.adi).", false},
                        {"F8", "Statistics for this net.", true},
                        {"F9", "Find a station's check-ins to every net.", true},
                        {"F12", "Read the highlighted session's notes.", true},
                        {"Esc", "Back.", false},
                    };
                case kPageSettings:
                    return {
                        {"F2", "Save your settings.", false},
                        {"Esc", "Cancel.", false},
                        {"Left/Right", "Change the time format.", false},
                    };
                default:
                    break;
            }
        }
        switch (state->page)
        {
            case kPageNetList:
                return {
                    {"F2", "Create a new recurring net.", false},
                    {"F3/Enter", "Log the highlighted net, or join or view its open session.", false},
                    {"F4", "Settings: your callsign, home ZIP and time format.", false},
                    {"F5", "Ad hoc nets: log one, resume one, or see their history.", false},
                    {"F6", "History of the highlighted net: view, export, delete.", false},
                    {"F7", "Edit a net (by number): its details and saved stations.", false},
                    {"F8", "Export the highlighted net to a file to share.", false},
                    {"F9", "Import a net from a file.", false},
                    {"F10", "Quit.", false},
                    {"Up/Down", "Move the highlight.", false},
                };
            case kPageCreateNet:
                return {
                    {"F2", "Save the new net.", false},
                    {"Esc", "Cancel.", false},
                    {"Tab", "Move to the next field (Up/Down too).", false},
                    {"Left/Right", "Change the Mode, or Partial Matching: US or Canada.", false},
                };
            case kPageSelectRole:
                return {
                    {"Up/Down", "Choose your role, or Viewer to watch an open session.", false},
                    {"F2/Enter", "Continue.", false},
                    {"Esc", "Back to the net list.", false},
                };
            case kPageEnterCallsign:
                return {
                    {"F2/Enter", "Start the log, with you checked in as #1.", false},
                    {"Esc", "Back to choosing a role.", false},
                };
            case kPageActiveNet:
                if (state->viewing_only)
                {
                    return {
                        {"F7", "Export this session: its log, .qlsession and ADIF (.adi).", false},
                        {"Esc", "Stop watching; back to the net list.", false},
                        {"F6", "A station's other check-ins to this net (by #).", true},
                        {"F8", "Regulars who haven't checked in yet.", true},
                        {"F9", "Everything known about a station (by #).", true},
                        {"F10", "This session so far: first-timers, recent average.", true},
                        {"F12", "Read the session's notes.", true},
                        {"Up/Down", "Move the highlight.", false},
                    };
                }
                return {
                    {"F2", "Check in a station (the New Check-In window).", false},
                    {"F3", "Edit a check-in, chosen by its #.", false},
                    {"F4", "Close this session; it moves to History.", false},
                    {"F5", "Delete a check-in, chosen by its #.", false},
                    {"F7", "Export this session: its log, .qlsession and ADIF (.adi).", false},
                    {"F6", "A station's other check-ins to this net (by #).", true},
                    {"F8", "Regulars who haven't checked in yet; Enter logs one.", true},
                    {"F9", "Everything known about a station (by #).", true},
                    {"F10", "This session so far: first-timers, recent average.", true},
                    {"F12", "Session notes: what this session was about, at length.", true},
                    {"Up/Down", "Move the highlight; Enter edits that check-in.", false},
                };
            case kPageSettings:
            {
                std::vector<HelpLine> lines = {
                    {"F2", "Save your settings.", false},
                    {"Esc", "Cancel.", false},
                    {"F3", "Refresh the station data now (console only).", false},
                };
#if defined(QUICKLOGGER_WITH_SSH)
                lines.push_back({"F4", "Manage SSH users (console only).", false});
#endif
                lines.push_back(
                    {"Left/Right",
                     state->is_console_session ? "Change the time format or Update Check." : "Change the time format.",
                     false});
                return lines;
            }
            case kPageAdHocNet:
                return {
                    {"F2", "Log an ad hoc net with the details entered.", false},
                    {"F3", "Resume an ad hoc session left open (by number).", false},
                    {"F6", "History of every ad hoc net.", false},
                    {"Esc", "Back to the net list.", false},
                    {"Left/Right", "Change the Mode, or Partial Matching: US or Canada.", false},
                };
            case kPageNetHistory:
                return {
                    {"Up/Down", "Choose a session; its check-ins show below.", false},
                    {"F5", "Delete a check-in from that session (by #).", false},
                    {"F6", "Import a session exported elsewhere (.qlsession).", false},
                    {"F7", "Export the highlighted session: log, .qlsession, ADIF (.adi).", false},
                    {"F4", "Delete a closed session (by number).", true},
                    {"F8", "Statistics for this net.", true},
                    {"F9", "Find a station's check-ins to every net.", true},
                    {"F12", "The highlighted session's notes, to read or edit.", true},
                    {"Esc", "Back.", false},
                };
            case kPageEditNet:
                return {
                    {"F2", "Save the net's details and return to the list.", false},
                    {"F3", "Edit a saved station (by number, or Enter).", false},
                    {"F4", "Remove a saved station (by number).", false},
                    {"F6", "Add a saved station.", false},
                    {"F7", "Export this net's saved stations to a file.", false},
                    {"F8", "Delete this net and all its history.", false},
                    {"F5", "Saved stations that haven't checked in lately.", true},
                    {"Esc", "Back without saving.", false},
                    {"Left/Right", "Change the Mode, or Partial Matching: US or Canada.", false},
                };
            case kPageImportNet:
                if (IsLocalTerminal(state->is_console_session) || NoZmodemOnThisSystem())
                {
                    return {
                        {"F2/Enter", "Import the highlighted file.", false},
                        {"Esc", state->import_session ? "Back to History." : "Back.", false},
                    };
                }
                return {
                    {"F2/Enter", "Import the highlighted file.", false},
                    {"F3", "Receive a file from your terminal (ZMODEM).", false},
                    {"Esc", state->import_session ? "Back to History." : "Back.", false},
                };
            case kPageManageUsers:
                if (state->show_user_keys_modal)
                {
                    return {
                        {"F2", "Save the username and access; close the window.", false},
                        {"F3", "Remove one of this user's keys (by number).", false},
                        {"F4/Enter", "Add the key pasted in, for this user.", false},
                        {"Esc", "Close without saving the username or access.", false},
                        {"Left/Right", "Change the access.", false},
                    };
                }
                return {
                    {"F2", "Add the SSH user entered (or another key for them).", false},
                    {"F3", "Remove an SSH user and all their keys (by number).", false},
                    {"F4/Enter", "Edit a user: username, access and keys (by number).", false},
                    {"Esc", "Back to Settings.", false},
                };
            default:
                return {};
        }
    }

    static void OpenSessionNotes(AppState* state, const NetInstance& instance, const std::string& net_name,
                                 bool read_only)
    {
        // Fresh from the database: someone else in a shared session may
        // have changed them.
        std::optional<NetInstance> current = state->db->GetNetInstanceById(instance.id);
        state->session_notes_instance_id = instance.id;
        state->session_notes_title = "Session Notes: " + net_name + ", " + instance.instance_date;
        state->session_notes_text = current.has_value() ? current->notes : instance.notes;
        state->session_notes_cursor = static_cast<int>(state->session_notes_text.size());
        state->session_notes_read_only = read_only;
        state->show_session_notes_modal = true;
    }

    void OpenActiveSessionNotes(AppState* state)
    {
        OpenSessionNotes(state, state->active_instance, state->active_net_name,
                         state->viewing_only || state->view_only_user);
    }

    void OpenHistorySessionNotes(AppState* state)
    {
        if (state->history_instances.empty())
        {
            return;
        }
        std::size_t index = static_cast<std::size_t>(
            std::clamp(state->selected_history_index, 0, static_cast<int>(state->history_instances.size()) - 1));
        const NetInstance& instance = state->history_instances[index];
        std::optional<Net> net = state->db->GetNetById(instance.net_id);
        OpenSessionNotes(state, instance, net.has_value() ? net->name : std::string(), state->view_only_user);
    }

    void SaveSessionNotes(AppState* state)
    {
        if (state->session_notes_read_only)
        {
            CloseSessionNotes(state);
            return;
        }
        // The window's working copy isn't needed once saved: moved, not copied.
        std::string notes = std::move(state->session_notes_text);
        state->session_notes_text.clear();
        notes.erase(notes.find_last_not_of(" \n\r\t") + 1);
        state->db->SetNetInstanceNotes(state->session_notes_instance_id, notes);
        if (state->active_instance.id == state->session_notes_instance_id)
        {
            state->active_instance.notes = notes;
        }
        for (NetInstance& instance : state->history_instances)
        {
            if (instance.id == state->session_notes_instance_id)
            {
                instance.notes = notes;
            }
        }
        state->show_session_notes_modal = false;
        state->form_error.clear();
        state->status_message = "Session notes saved.";
    }

    void CloseSessionNotes(AppState* state)
    {
        state->show_session_notes_modal = false;
    }

    void OpenUpdatePage(AppState* state)
    {
        std::string version = AvailableUpdate();
        if (version.empty())
        {
            return;
        }
        if (IsLocalTerminal(state->is_console_session) && CanShowInFileManager())
        {
            std::string error;
            if (!OpenInBrowser(kReleasesPageUrl, &error))
            {
                state->status_message.clear();
                state->form_error = "Couldn't open " + std::string(kReleasesPageUrl) + " (" + error + ").";
                return;
            }
            state->form_error.clear();
            state->status_message = "Opened QuickLogger " + version + "'s download page.";
            return;
        }
        state->form_error.clear();
        state->status_message = "QuickLogger " + version + " is out: " + std::string(kReleasesPageUrl);
    }

    void OpenHelp(AppState* state)
    {
        std::vector<std::vector<std::string>> rows;
        bool any_extra = false;
        for (const HelpLine& line : HelpFor(state))
        {
            rows.push_back({line.key, std::string(line.what) + (line.extra ? " *" : "")});
            any_extra = any_extra || line.extra;
        }
        std::vector<std::string> summary;
        if (any_extra)
        {
            summary.emplace_back(
                "* Seldom-used keys: they always work, and appear on the key bar when the "
                "terminal is wide enough to show them.");
        }
        ShowInfoWindow(state, InfoWindow::kHelp, "Help", std::move(summary),
                       {{"Key", 10, 10, 0, 0}, {"What it does", 1, 1, 0, 0}}, std::move(rows));
    }

}  // namespace ql
