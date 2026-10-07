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
#include <sstream>
#include <unordered_set>
#include <utility>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

#include "../adif_export.hpp"
#include "../callsign_rules.hpp"
#include "../server_address.hpp"
#include "../date_utils.hpp"
#include "../frequency_rules.hpp"
#include "../file_export.hpp"
#include "../geo_utils.hpp"
#include "../precise_grid.hpp"
#include "../gmrs_channels.hpp"
#include "../mode_rules.hpp"
#include "../net_slice.hpp"
#include "../public_key.hpp"
#include "../show_folder.hpp"
#include "../text_utils.hpp"
#include "../uls_import.hpp"
#include "../update_check.hpp"
#include "../zip_write.hpp"
#include "../zmodem_send.hpp"
#include "chrome.hpp"
#include "list_columns.hpp"
#include "pull_runner.hpp"
#include "push_runner.hpp"

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
            // On a GMRS net, the person this check-in was logged under.
            row.push_back(check_in.name.empty() ? station.name : check_in.name);
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
        cells.emplace_back(instance.status == NetInstanceStatus::kOpen ? "OPEN"
                           : instance.pushed_at > 0                    ? "pushed"
                                                                       : "closed");
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

    // What a username may be, as Manage Users says it.
    static const char* const kUsernameRule =
        "A username has 1 to 32 letters, digits, dots, hyphens and underscores, starting with a letter or digit.";

    // A row per user: their access applies to every key of theirs.
    static const std::vector<ListColumn>& UserColumns()
    {
        static const std::vector<ListColumn> columns = {
            // A login name, which needn't be a call sign.
            {"Username", 16, 16, 0, 0}, {"Amateur", 9, 9, 0, 0}, {"GMRS", 7, 7, 0, 0},
            {"Access", 9, 9, 0, 0},     {"Keys", 4, 4, 0, 0},    {"Last Login", 19, 19, 0, 0},
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
        int disabled = 0;
        std::int64_t last_login_at = 0;
        const User* first = nullptr;
        for (const User& key : keys)
        {
            if (key.username != username)
            {
                continue;
            }
            first = first == nullptr ? &key : first;
            view_only = view_only || key.view_only;
            ++count;
            disabled += key.disabled ? 1 : 0;
            last_login_at = std::max(last_login_at, key.last_login_at);
        }
        std::vector<std::string> cells;
        cells.reserve(6);
        cells.push_back(username);
        cells.push_back(first != nullptr ? first->amateur_callsign : std::string());
        cells.push_back(first != nullptr ? first->gmrs_callsign : std::string());
        // Every key off: no login at all. Some off: "working/total".
        cells.emplace_back(disabled == count && count > 0 ? "Disabled" : view_only ? "View-Only" : "Full");
        cells.push_back(disabled > 0 && disabled < count
                            ? std::to_string(count - disabled) + "/" + std::to_string(count)
                            : std::to_string(count));
        cells.push_back(DescribeLastLogin(last_login_at));
        return cells;
    }

    static ListLayout UserLayout(int terminal_width)
    {
        // Six short columns: all of them fit at 80 columns two spaces apart,
        // spread out up to four on a wider terminal.
        return LayOutList(UserColumns(), ScreenListWidth(terminal_width), kScreenListWidthAt80, 2, 4);
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
            // Date and who added it: the admin ("admin") or the user ("user").
            {"Added", 15, 15, 1, 0},
            {"Comment", 16, 30, 0, 3},
        };
        return columns;
    }

    // "2026-10-06 user": when a key was added and by whom; just the "by" for
    // a key from before dates were kept.
    static std::string DescribeKeyAdded(const User& key)
    {
        std::string date = FormatLocalDate(key.created_at);
        return (date.empty() ? std::string() : date + " ") + (key.added_by_user ? "user" : "admin");
    }

    static std::vector<std::string> UserKeyCells(const User& key)
    {
        PublicKeyDescription description = DescribePublicKey(key.public_key);
        std::vector<std::string> cells;
        cells.reserve(5);
        cells.push_back(std::move(description.type));
        cells.push_back(std::move(description.fingerprint));
        cells.push_back(DescribeLastLogin(key.last_login_at));
        cells.push_back(DescribeKeyAdded(key));
        cells.push_back(key.disabled ? "[off] " + description.comment : std::move(description.comment));
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

    // The My Keys window's rows: comment, type, Transfer method, the end of
    // the fingerprint, and which key this login used. At 80 columns the
    // widths add up to what the window holds; on a wider terminal the
    // comment and the fingerprint widen, and the gaps. The last column is
    // the "<- this login" mark, so the Key column is never the last.
    static const std::vector<ListColumn>& MyKeyColumns()
    {
        static const std::vector<ListColumn> columns = {
            {"Comment", 12, 20, 0, 2}, {"Type", 7, 7, 0, 0}, {"Transfer", 8, 8, 0, 0},
            {"Key", 15, 50, 0, 1},     {"", 13, 13, 0, 0},
        };
        return columns;
    }

    static ListLayout MyKeyLayout(int terminal_width)
    {
        return LayOutList(MyKeyColumns(), MatchListWidth(terminal_width), MatchListWidth(80), 2, 8);
    }

    const std::string& MyKeyListHeader(int terminal_width)
    {
        static HeadingCache cache;
        if (HeadingNeedsBuilding(&cache, terminal_width, 0))
        {
            cache.text = MenuGutter() + FormatListHeading(MyKeyColumns(), MyKeyLayout(terminal_width));
        }
        return cache.text;
    }

    // AppState::my_keys_labels for the keys loaded, laid out for the
    // terminal's width. The Key column shows the end of the fingerprint, as
    // much of it as the column has room for.
    static void FormatMyKeyLabels(AppState* state)
    {
        ListLayout layout = MyKeyLayout(state->list_width);
        // Its width in the Key column, less the "..." that says it's the end.
        std::size_t tail = layout.widths[3] > 3 ? static_cast<std::size_t>(layout.widths[3] - 3) : 0;
        state->my_keys_labels.clear();
        state->my_keys_labels.reserve(state->my_keys.size());
        for (const User& key : state->my_keys)
        {
            PublicKeyDescription description = DescribePublicKey(key.public_key);
            std::vector<std::string> cells;
            cells.reserve(5);
            cells.push_back(description.comment.empty() ? "(no comment)" : std::move(description.comment));
            cells.push_back(std::move(description.type));
            cells.push_back(key.transfer_method == kTransferZmodem ? "ZMODEM"
                            : key.transfer_method == kTransferSftp ? "SFTP"
                                                                   : "Ask");
            const std::string& fingerprint = description.fingerprint;
            if (static_cast<int>(fingerprint.size()) <= layout.widths[3])
            {
                cells.push_back(fingerprint);
            }
            else
            {
                cells.push_back(fingerprint.size() > 16 ? "..." + fingerprint.substr(fingerprint.size() - tail) : "");
            }
            cells.push_back(key.id == state->ssh_key_id ? "<- this login" : key.disabled ? "disabled" : "");
            state->my_keys_labels.push_back(FormatListRow(cells, layout));
        }
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

    // The same for a key line, for the key log.
    static std::string DescribeKeyLine(const std::string& line)
    {
        // The comment first: it is what a person recognizes, and the key
        // log's last column is narrow at 80 columns.
        PublicKeyDescription key = DescribePublicKey(line);
        return (key.comment.empty() ? std::string("(no comment)") : key.comment) + " " + key.type + " " +
               key.fingerprint;
    }

    // Writes one line of the key log: the console's admin, or the SSH user
    // at My Keys, did `action` to `username`'s keys.
    static void RecordKeyEvent(AppState* state, const std::string& action, const std::string& username,
                               const std::string& detail)
    {
        KeyEvent event;
        event.at = static_cast<std::int64_t>(std::time(nullptr));
        event.actor = state->is_console_session ? "console" : state->ssh_username;
        event.action = action;
        event.username = username;
        event.detail = detail;
        state->db->LogKeyEvent(event);
    }

    // The key log window's rows: when, who, and what they did.
    static const std::vector<ListColumn>& KeyLogColumns()
    {
        static const std::vector<ListColumn> columns = {
            {"When", 19, 19, 0, 0},
            {"By", 10, 10, 0, 0},
            {"What", 20, 120, 0, 1},
        };
        return columns;
    }

    static ListLayout KeyLogLayout(int terminal_width)
    {
        return LayOutList(KeyLogColumns(), MatchListWidth(terminal_width), MatchListWidth(80), 2, 3);
    }

    const std::string& KeyLogListHeader(int terminal_width)
    {
        static HeadingCache cache;
        if (HeadingNeedsBuilding(&cache, terminal_width, 0))
        {
            cache.text = MenuGutter() + FormatListHeading(KeyLogColumns(), KeyLogLayout(terminal_width));
        }
        return cache.text;
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

    // On a GMRS net, after the last of this net's names for the call sign
    // typed (one license covers a family), a match with that station's
    // details and no name, for checking in someone else on it. Dropping
    // the last match if there's no room.
    static void OfferNewGmrsName(NetService service, const std::string& typed, std::size_t max_suggestions,
                                 std::vector<Station>* suggestions, std::vector<std::string>* sources)
    {
        if (service != NetService::kGmrs || IsWildcardCallsign(typed))
        {
            return;
        }
        std::string callsign = NormalizeCallsign(typed);
        std::size_t after = 0;
        for (std::size_t i = 0; i < suggestions->size(); ++i)
        {
            if ((*suggestions)[i].callsign == callsign && (*sources)[i] == KnownStationSource(true))
            {
                after = i + 1;
            }
        }
        if (after == 0)
        {
            return;
        }
        Station someone_new = (*suggestions)[after - 1];
        someone_new.name.clear();
        if (suggestions->size() >= max_suggestions)
        {
            suggestions->pop_back();
            sources->pop_back();
        }
        after = std::min(after, suggestions->size());
        suggestions->insert(suggestions->begin() + static_cast<std::ptrdiff_t>(after), std::move(someone_new));
        sources->insert(sources->begin() + static_cast<std::ptrdiff_t>(after), "(new name)");
    }

    // `distance_miles` < 0 means "unknown" (the station's own ZIP has no
    // centroid on file) -- shown as "(ULS, unknown)" rather than a fabricated
    // number, since it only passed the coarser ZIP3-prefix pre-filter.
    // A Canadian license, from ISED's data (see AppendCanadianSuggestions).
    static const char* const kIsedSource = "(ISED)";

    static std::string IsedSource(double distance_miles)
    {
        if (distance_miles < 0.0)
        {
            return kIsedSource;
        }
        int miles = static_cast<int>(distance_miles);
        if (miles >= 1000)
        {
            return "(ISED 999+mi)";
        }
        return "(ISED ~" + std::to_string(miles) + (miles >= 100 ? "mi)" : " mi)");
    }

    static std::string UlsSource(double distance_miles)
    {
        if (distance_miles < 0.0)
        {
            return "(ULS, unknown)";
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
    // The most the name starts at: what's left at 80 columns beside the
    // always-shown Type (7), Frequency (10) and Notes (19), with their gaps.
    // It widens back toward kMaxNetNameColumnWidth when there's room.
    static constexpr int kNetNameWidthAt80 = 33;

    // The when-created/imported column (and "session open") comes last, as
    // it always has.
    // The Recurrence column's index in NetListColumns, and its full width:
    // a typical recurrence ("Wednesdays at 8pm ET") is about 20; more
    // than 24 would take room Offset and PL can use.
    static constexpr std::size_t kNetRecurrenceColumn = 6;
    static constexpr int kNetRecurrenceFullWidth = 24;

    // `name_width` widens toward `name_max_width` only after everything
    // else has been added and widened.
    static std::vector<ListColumn> NetListColumns(int name_width, int name_max_width)
    {
        return {
            {"Net", name_width, name_max_width, 0, 99},
            // "Amateur" or "GMRS", at every width; at 80 columns its room comes out of the name's (see
            // kNetNameWidthAt80).
            {"Type", 7, 7, 0, 0, 7},
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
        ListLayout usual =
            LayOutList(NetListColumns(std::min(state->net_name_width, kNetNameWidthAt80), state->net_name_width),
                       available, kScreenListWidthAt80, 2);
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
        cells.emplace_back(net.service == NetService::kGmrs ? "GMRS" : "Amateur");
        cells.push_back(net.mode);
        // A GMRS net by its channel ("Ch 22R"): channel 22 and its repeater
        // pair share a frequency.
        int channel =
            net.service == NetService::kGmrs ? FindGmrsChannel(net.default_frequency, net.repeater_offset) : -1;
        if (channel >= 0)
        {
            cells.push_back("Ch " + std::string(GmrsChannels()[static_cast<std::size_t>(channel)].name));
        }
        else
        {
            cells.push_back(net.default_frequency);
        }
        cells.push_back(net.repeater_offset);
        cells.push_back(net.pl_tone);
        cells.push_back(net.recurrence_description);
        cells.push_back(std::move(when));
        return cells;
    }

    static std::vector<std::string> FormatNetList(const AppState* state)
    {
        std::vector<std::string> rows = FormatRows(state->net_cells, NetListLayout(state));
        // No trailing blanks when nothing follows the Type.
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
        FormatMyKeyLabels(state);
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
    // already present (by callsign), so tier 2 never duplicates a tier 1 hit,
    // leaving out call signs of the other service (a GMRS one on an Amateur
    // Radio net, say). Moves candidates in (they're not used again) and
    // stops once `suggestions` holds `max_suggestions`.
    static void AppendNewSuggestions(std::vector<Station>* suggestions, std::vector<Station>* candidates,
                                     std::size_t max_suggestions, NetService service)
    {
        for (Station& candidate : *candidates)
        {
            if (suggestions->size() >= max_suggestions)
            {
                return;
            }
            if (IsValidGmrsCallsign(candidate.callsign) != (service == NetService::kGmrs))
            {
                continue;
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

    bool CanPushUpstream(const AppState* state)
    {
        return state->is_console_session && state->push_runner != nullptr && !state->settings.upstream_host.empty();
    }

    void OpenUpstreamWindow(AppState* state)
    {
        if (!state->is_console_session)
        {
            return;
        }
        state->upstream_host_text = state->settings.upstream_host;
        state->upstream_user_text = state->settings.upstream_user;
        state->upstream_port_text = std::to_string(state->settings.upstream_port);
        state->upstream_host_cursor = static_cast<int>(state->upstream_host_text.size());
        state->upstream_user_cursor = static_cast<int>(state->upstream_user_text.size());
        state->upstream_port_cursor = static_cast<int>(state->upstream_port_text.size());
        state->upstream_focus = 0;
        // Looked for once here, not on every frame.
        state->upstream_tools_available = UpstreamToolsAvailable();
        state->form_error.clear();
        state->status_message.clear();
        state->show_upstream_window = true;
    }

    void CloseUpstreamWindow(AppState* state)
    {
        state->show_upstream_window = false;
        state->form_error.clear();
    }

    // `text` without spaces or tabs around it.
    static std::string Spaceless(const std::string& text)
    {
        std::string::size_type start = text.find_first_not_of(" \t");
        if (start == std::string::npos)
        {
            return std::string();
        }
        std::string::size_type end = text.find_last_not_of(" \t");
        return text.substr(start, end - start + 1);
    }

    void SaveUpstreamWindow(AppState* state)
    {
        std::string host = Spaceless(state->upstream_host_text);
        std::string user = Spaceless(state->upstream_user_text);
        std::string port_text = Spaceless(state->upstream_port_text);
        int port = kDefaultUpstreamPort;
        if (!host.empty())
        {
            if (!IsValidUpstreamHost(host))
            {
                state->form_error = "A host name has only letters, digits, dots, hyphens and underscores.";
                return;
            }
            if (user.empty())
            {
                state->form_error = "Your username on the upstream is required.";
                return;
            }
            if (!IsValidUpstreamUser(user))
            {
                state->form_error = "A username has only letters, digits, dots, hyphens and underscores.";
                return;
            }
            if (!port_text.empty())
            {
                port = port_text.size() > 5 ? 0 : std::stoi(port_text);
                if (port < 1 || port > 65535)
                {
                    state->form_error = "The port must be 1 to 65535.";
                    return;
                }
            }
        }
        else
        {
            user.clear();
        }
        // Into the saved settings and the Settings page's working copy
        // alike, so the page's own F2 doesn't put the old upstream back.
        state->settings.upstream_host = host;
        state->settings.upstream_user = user;
        state->settings.upstream_port = port;
        state->settings_form.upstream_host = host;
        state->settings_form.upstream_user = user;
        state->settings_form.upstream_port = port;
        SaveSettings(state->settings_path, state->settings);
        state->show_upstream_window = false;
        state->form_error.clear();
        state->status_message =
            host.empty() ? "No upstream server: sessions aren't pushed." : "Sessions can be pushed to " + host + ".";
    }

    // The name F7 Export gives a session's files, without the extension:
    // the net's name and the session's date.
    static std::string SessionFileStem(const std::string& net_name, const NetInstance& instance)
    {
        return SanitizeFilenameComponent(net_name) + "_" + SanitizeFilenameComponent(instance.instance_date);
    }

    static void RemovePushFile(AppState* state)
    {
        if (!state->push_local_path.empty())
        {
            std::error_code ignored;
            std::filesystem::remove(state->push_local_path, ignored);
            state->push_local_path.clear();
        }
    }

    // True if a push can start now; else says why in form_error.
    static bool CheckPushPossible(AppState* state)
    {
        if (!CanPushUpstream(state))
        {
            state->form_error = "Set up an upstream server in Settings (F5) first.";
            return false;
        }
        if (state->push_running)
        {
            state->form_error = "A push is already running.";
            return false;
        }
        if (!UpstreamToolsAvailable())
        {
            state->form_error = kNoSshMessage;
            return false;
        }
        return true;
    }

    bool StartPush(AppState* state, std::int64_t instance_id, const std::string& confirm_net)
    {
        if (!CheckPushPossible(state))
        {
            return false;
        }
        std::optional<NetInstance> instance = state->db->GetNetInstanceById(instance_id);
        std::optional<Net> net = instance.has_value() ? state->db->GetNetById(instance->net_id) : std::nullopt;
        if (!net.has_value())
        {
            state->form_error = "That session no longer exists.";
            return false;
        }
        if (instance->status != NetInstanceStatus::kClosed)
        {
            state->form_error = "Only a closed session can be pushed.";
            return false;
        }

        // Uploaded under F7's name, so pushing it again replaces the
        // earlier upload; made here under a name of its own, so it never
        // replaces an F7 export.
        std::string remote_name = SessionFileStem(net->name, *instance) + ".qlsession";
        std::string exports = ExportsDir(state->db_path);
        std::string local_path = TemporaryPathFor(exports + "/" + remote_name);
        std::string error;
        if (!EnsureDirectory(exports) ||
            !WriteNetSliceFile(local_path, GatherSessionSlice(state->db, instance_id), &error))
        {
            std::error_code ignored;
            std::filesystem::remove(local_path, ignored);
            state->form_error = "Couldn't write the session to push: " + (error.empty() ? exports : error);
            return false;
        }

        RemovePushFile(state);
        state->push_running = true;
        state->push_is_net = false;
        state->push_instance_id = instance_id;
        state->push_local_path = local_path;
        state->push_remote_name = remote_name;
        state->push_session_net = net->name;
        Upstream upstream;
        upstream.host = state->settings.upstream_host;
        upstream.user = state->settings.upstream_user;
        upstream.port = state->settings.upstream_port;
        state->push_runner->Start(upstream, local_path, remote_name, confirm_net, net->name);
        state->form_error.clear();
        state->status_message = "Pushing to " + upstream.host + "...";
        return true;
    }

    // `slice` without its open sessions and their check-ins: a session still
    // being logged isn't finished, so it isn't pushed.
    static NetSlice WithoutOpenSessions(NetSlice slice)
    {
        std::unordered_set<std::int64_t> open;
        std::vector<NetInstance> closed;
        closed.reserve(slice.instances.size());
        for (NetInstance& instance : slice.instances)
        {
            if (instance.status == NetInstanceStatus::kClosed)
            {
                closed.push_back(std::move(instance));
            }
            else
            {
                open.insert(instance.id);
            }
        }
        slice.instances = std::move(closed);
        std::vector<CheckIn> kept;
        kept.reserve(slice.check_ins.size());
        for (CheckIn& check_in : slice.check_ins)
        {
            if (open.count(check_in.net_instance_id) == 0)
            {
                kept.push_back(std::move(check_in));
            }
        }
        slice.check_ins = std::move(kept);
        return slice;
    }

    bool StartNetPush(AppState* state, std::int64_t net_id, const std::string& confirm_net)
    {
        if (!CheckPushPossible(state))
        {
            return false;
        }
        std::optional<Net> net = state->db->GetNetById(net_id);
        if (!net.has_value())
        {
            state->form_error = "That net no longer exists.";
            return false;
        }

        // Made under a name of its own next to F8's export, so it never
        // replaces one.
        std::string remote_name = SanitizeFilenameComponent(net->name) + ".qlnet";
        std::string exports = ExportsDir(state->db_path);
        std::string local_path = TemporaryPathFor(exports + "/" + remote_name);
        std::string error;
        if (!EnsureDirectory(exports) ||
            !WriteNetSliceFile(local_path, WithoutOpenSessions(GatherNetSlice(state->db, net_id)), &error))
        {
            std::error_code ignored;
            std::filesystem::remove(local_path, ignored);
            state->form_error = "Couldn't write the net to push: " + (error.empty() ? exports : error);
            return false;
        }

        RemovePushFile(state);
        state->push_running = true;
        state->push_is_net = true;
        state->push_net_id = net_id;
        state->push_local_path = local_path;
        state->push_remote_name = remote_name;
        state->push_session_net = net->name;
        Upstream upstream;
        upstream.host = state->settings.upstream_host;
        upstream.user = state->settings.upstream_user;
        upstream.port = state->settings.upstream_port;
        state->push_runner->Start(upstream, local_path, remote_name, confirm_net, net->name, true);
        state->form_error.clear();
        state->status_message = "Pushing to " + upstream.host + "...";
        return true;
    }

    // The upstream kept the last push's upload to ask about it; it won't be
    // used, so it can go.
    static void DiscardKeptUpload(const AppState* state)
    {
        if (state->push_runner == nullptr || state->push_remote_name.empty())
        {
            return;
        }
        Upstream upstream;
        upstream.host = state->settings.upstream_host;
        upstream.user = state->settings.upstream_user;
        upstream.port = state->settings.upstream_port;
        state->push_runner->Discard(upstream, state->push_remote_name);
    }

    void FinishPush(AppState* state, const PushResult& result)
    {
        state->push_running = false;
        RemovePushFile(state);
        if (result.kind == PushResultKind::kPushed)
        {
            if (!state->push_is_net)
            {
                state->db->SetNetInstancePushedAt(state->push_instance_id,
                                                  static_cast<std::int64_t>(std::time(nullptr)));
            }
            if (state->page == kPageNetHistory)
            {
                RefreshNetHistory(state);
            }
            state->form_error.clear();
            state->status_message = result.message;
            return;
        }
        if (result.kind == PushResultKind::kNeedsConfirmation)
        {
            // Something else is being asked right now: don't stack a second
            // question on it.
            if (state->show_confirm_prompt)
            {
                state->status_message.clear();
                state->form_error = state->push_is_net
                                        ? "Not pushed: " + state->settings.upstream_host +
                                              " has a net to merge into. Push it again with F8 Export."
                                        : "Not pushed: " + state->settings.upstream_host + " has no net named " +
                                              state->push_session_net + ". Push it from History (F7 Export) to choose.";
                DiscardKeptUpload(state);
                return;
            }
            state->push_upstream_net = result.upstream_net;
            if (state->push_is_net)
            {
                std::string has = NetNamesAreTheSame(result.upstream_net, state->push_session_net)
                                      ? state->settings.upstream_host + " has " + result.upstream_net + " already."
                                      : state->settings.upstream_host + " has no net named " + state->push_session_net +
                                            ". Its net " + result.upstream_net + " looks like it.";
                ShowConfirmPrompt(state, ConfirmPrompt::kPushToNet, "Merge Into " + result.upstream_net + "?",
                                  {has, "Merging it " + result.merge_summary + ".",
                                   "Merge this net into " + result.upstream_net + "?"});
                return;
            }
            ShowConfirmPrompt(state, ConfirmPrompt::kPushToNet, "Push to " + result.upstream_net + "?",
                              {state->settings.upstream_host + " has no net named " + state->push_session_net +
                                   ". Its net " + result.upstream_net + " looks like it.",
                               "Push this session to " + result.upstream_net + "?"});
            return;
        }
        state->status_message.clear();
        state->form_error = result.message;
    }

    void ConfirmPushToNet(AppState* state)
    {
        CancelConfirmPrompt(state);
        if (state->push_is_net)
        {
            StartNetPush(state, state->push_net_id, state->push_upstream_net);
            return;
        }
        StartPush(state, state->push_instance_id, state->push_upstream_net);
    }

    void DeclinePushToNet(AppState* state)
    {
        CancelConfirmPrompt(state);
        DiscardKeptUpload(state);
        state->form_error.clear();
        state->status_message = state->push_is_net ? "Not pushed. Push it again with F8 Export."
                                                   : "Not pushed. Push it from History (F7 Export) later.";
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
        // Without a call sign for its service, as for a view-only user,
        // only watching.
        if (state->view_only_user || OwnCallsign(state, state->start_net.service).empty())
        {
            std::string why;
            if (!state->view_only_user)
            {
                RefuseWithoutCallsign(state, state->start_net.service);
                why = state->form_error;
                state->form_error.clear();
            }
            // Only ever to watch its open session, if it has one.
            ViewStartNet(state);
            if (!state->form_error.empty())
            {
                state->form_error = "No session of " + state->start_net.name + " is open to view.";
                if (!why.empty())
                {
                    state->form_error = why;
                }
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
        Net net;
        if (!ReadNewNetRadio(state, &net) || !CheckNetZip(state, state->new_net_location) ||
            RefuseWithoutCallsign(state, net.service))
        {
            return;
        }
        net.name = state->new_net_name;
        net.default_location = NormalizeZipOrPostalCode(state->new_net_location);
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
        state->active_net_service = net.has_value() ? net->service : NetService::kAmateur;
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
            state->operator_callsign = OwnCallsign(state, state->active_net_service);
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

    bool CloseActiveNet(AppState* state)
    {
        CancelConfirmPrompt(state);
        if (RefuseViewOnly(state, "close net sessions"))
        {
            return false;
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
            return false;
        }
        LeaveActiveNet(state);
        state->form_error.clear();
        state->status_message =
            "Closed " + closed_name + " (" + CountCheckIns(check_ins) + "). It's in History (" + history + ").";
        return true;
    }

    void CloseActiveNetAndPush(AppState* state)
    {
        std::int64_t instance_id = state->active_instance.id;
        if (!CloseActiveNet(state))
        {
            return;
        }
        std::string closed = state->status_message;
        if (StartPush(state, instance_id, ""))
        {
            state->status_message = closed + " " + state->status_message;
        }
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
            lines.emplace_back("Another user has deleted this net's session.");
        }
        if (!unlogged.empty())
        {
            lines.push_back(unlogged + " was not logged.");
        }
        lines.emplace_back("You will be returned to the Recurring Nets list when you press Enter.");
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
        ServerAddress set_address;
        set_address.host = state->settings.server_address;
        set_address.port = state->settings.server_port;
        state->settings_server_text = FormatServerAddress(set_address);
        state->settings_server_placeholder = FormatServerAddress(ResolveServerAddress("", 0, ""));
    }

    bool SaveSettingsForm(AppState* state)
    {
        if (!state->callsign_editable)
        {
            // An SSH user's are Manage Users' to set.
            state->settings_form.callsign = state->settings.callsign;
            state->settings_form.gmrs_callsign = state->settings.gmrs_callsign;
        }
        else if (!CheckUserCallsigns(state, &state->settings_form.callsign, &state->settings_form.gmrs_callsign))
        {
            return false;
        }
        if (!IsZipOrPostalCode(state->settings_form.location))
        {
            state->form_error = "Your postal code is required: 5-digit ZIP or Canadian postal code.";
            return false;
        }
        state->settings_form.location = NormalizeZipOrPostalCode(state->settings_form.location);

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

        if (state->is_console_session)
        {
            ServerAddress address;
            if (!ParseServerAddress(state->settings_server_text, &address))
            {
                state->form_error = "Server must be a host name or address, with an optional :port (1-65535).";
                return false;
            }
            state->settings_form.server_address = address.host;
            state->settings_form.server_port = address.port;
        }

        state->settings_form.use_24_hour_clock = state->settings_time_format_index == 1;
        state->settings_form.check_for_updates = state->settings_update_check_index == 0;
        state->settings_form.nearby_radius_miles = radius;
        SaveSettings(state->settings_path, state->settings_form);
        state->settings = state->settings_form;
        bool clock_changed = Use24HourClock() != state->settings.use_24_hour_clock;
        SetUse24HourClock(state->settings.use_24_hour_clock);
        SetUpdateCheckEnabled(state->is_console_session && state->settings.check_for_updates);
        if (clock_changed)
        {
            ReadStationDataStatus(state);
        }
        state->form_error.clear();
        return true;
    }

    void ShowStationDataStatus(AppState* state, const std::string& notice, bool is_problem, const std::string& status)
    {
        SetStationDataNotice(notice, is_problem);
        state->station_status_lines.clear();
        std::size_t start = 0;
        while (start < status.size())
        {
            std::size_t end = status.find('\n', start);
            if (end == std::string::npos)
            {
                end = status.size();
            }
            state->station_status_lines.emplace_back(status, start, end - start);
            start = end + 1;
        }
    }

    void ReadStationDataStatus(AppState* state)
    {
        try
        {
            Database::ReadTransaction reads(state->db);
            std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
            bool is_problem = false;
            std::string notice = DescribeStationDataNotice(state->db, now, &is_problem);
            ShowStationDataStatus(state, notice, is_problem, DescribeStationDataStatus(state->db, now));
        }
        // NOLINTNEXTLINE(bugprone-empty-catch): busy right now, ScreenTicker shows it shortly.
        catch (const std::exception&)
        {
            // Busy right now: ScreenTicker shows it shortly.
        }
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

    bool CheckNetCallsign(AppState* state, const std::string& callsign, NetService service)
    {
        if (service != NetService::kGmrs)
        {
            return CheckCallsign(state, callsign);
        }
        if (!IsValidGmrsCallsign(callsign))
        {
            state->form_error = callsign + " isn't a valid GMRS call sign.";
            return false;
        }
        return true;
    }

    std::string DescribeNetRadio(const Net& net)
    {
        int channel =
            net.service == NetService::kGmrs ? FindGmrsChannel(net.default_frequency, net.repeater_offset) : -1;
        if (channel >= 0)
        {
            // "GMRS 20R  462.6750 MHz  PL 141.3": the channel says the rest.
            std::string text = "GMRS " + std::string(GmrsChannels()[static_cast<std::size_t>(channel)].name) + "  " +
                               net.default_frequency + " MHz";
            if (!net.pl_tone.empty())
            {
                text += "  PL " + net.pl_tone;
            }
            return text;
        }
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

    const char* ServiceLabel(NetService service)
    {
        return service == NetService::kGmrs ? "GMRS" : "Amateur Radio";
    }

    void SetNewNetService(AppState* state)
    {
        state->new_net_gmrs = state->new_net_service_index == 1;
        state->new_net_amateur = !state->new_net_gmrs;
    }

    // A GMRS net's radio: FM on `channel` (an index into GmrsChannels()),
    // with `tone`, checked; US data for Partial Matching (GMRS licenses are
    // the FCC's alone).
    static bool FillGmrsRadio(AppState* state, int channel, std::string* tone, Net* net)
    {
        std::string problem = ToneProblem(*tone);
        if (!problem.empty())
        {
            state->form_error = problem;
            return false;
        }
        *tone = NormalizeTone(*tone);
        const GmrsChannel& chosen = GmrsChannels()[static_cast<std::size_t>(channel)];
        net->service = NetService::kGmrs;
        net->mode = "FM";
        net->default_frequency = chosen.frequency;
        net->repeater_offset = chosen.offset;
        net->pl_tone = *tone;
        net->partial_match_canada = false;
        return true;
    }

    bool ReadNewNetRadio(AppState* state, Net* net)
    {
        if (state->new_net_gmrs)
        {
            return FillGmrsRadio(state, state->new_net_gmrs_channel, &state->new_net_tone, net);
        }
        if (!CheckNetRadio(state, state->new_net_frequency, state->new_net_offset, &state->new_net_tone))
        {
            return false;
        }
        net->service = NetService::kAmateur;
        net->mode = NetModes()[static_cast<std::size_t>(state->new_net_mode_index)];
        net->default_frequency = state->new_net_frequency;
        net->repeater_offset = state->new_net_offset;
        net->pl_tone = state->new_net_tone;
        net->partial_match_canada = state->new_net_partial_match_index == 1;
        return true;
    }

    bool ReadEditNetRadio(AppState* state, Net* net)
    {
        if (state->edit_net_gmrs)
        {
            return FillGmrsRadio(state, state->edit_net_gmrs_channel, &state->edit_net_tone, net);
        }
        if (!CheckNetRadio(state, state->edit_net_frequency, state->edit_net_offset, &state->edit_net_tone))
        {
            return false;
        }
        net->service = NetService::kAmateur;
        net->mode = NetModes()[static_cast<std::size_t>(state->edit_net_mode_index)];
        net->default_frequency = state->edit_net_frequency;
        net->repeater_offset = state->edit_net_offset;
        net->pl_tone = state->edit_net_tone;
        net->partial_match_canada = state->edit_net_partial_match_index == 1;
        return true;
    }

    bool CheckNetZip(AppState* state, const std::string& zip)
    {
        if (!zip.empty() && !IsZipOrPostalCode(zip))
        {
            state->form_error = "Postal code must be a 5-digit ZIP or Canadian postal code, or left blank.";
            return false;
        }
        return true;
    }

    std::string ExistingNetNamed(const AppState* state, const std::string& name, std::int64_t except_net_id)
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
        state->new_net_service_index = 0;
        state->new_net_gmrs_channel = 0;
        SetNewNetService(state);
        state->form_error.clear();
    }

    std::string SavedEntryName(NetService service, const std::string& name)
    {
        return service == NetService::kGmrs ? Spaceless(name) : std::string();
    }

    // A check-in's call sign and name as one string, the name compared
    // without regard to case. A check-in on an Amateur Radio net has no
    // name, so its key is its call sign.
    static std::string CheckInKey(const std::string& callsign, const std::string& name)
    {
        return name.empty() ? callsign : callsign + '\n' + ToUpperAscii(name);
    }

    std::optional<Station> FindLicensee(Database* db, const std::string& callsign, NetService service)
    {
        return service == NetService::kGmrs ? db->FindUlsStationByCallsign(callsign, LicenseTable::kGmrs)
                                            : db->FindLicensedStationByCallsign(callsign);
    }

    const std::string& OwnCallsign(const AppState* state, NetService service)
    {
        return service == NetService::kGmrs ? state->settings.gmrs_callsign : state->settings.callsign;
    }

    bool RefuseWithoutCallsign(AppState* state, NetService service)
    {
        if (!OwnCallsign(state, service).empty())
        {
            return false;
        }
        state->status_message.clear();
        state->form_error = std::string("You have no ") + ServiceLabel(service) + " call sign, so you can only watch " +
                            ServiceLabel(service) + " nets." +
                            (state->callsign_editable ? " Add one in Settings (F4)." : "");
        return true;
    }

    void ResetStartNetFlow(AppState* state)
    {
        state->selected_role_index = kRoleNetControl;
        // Prefill with the operator's own call sign for the net's service;
        // they can still edit it if a different person is filling this
        // particular role.
        state->operator_callsign = OwnCallsign(state, state->start_net.service);
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
            std::optional<Station> uls = FindLicensee(state->db, state->operator_callsign, state->active_net_service);
            if (uls.has_value())
            {
                operator_station = *uls;
            }
        }
        operator_station.callsign = state->operator_callsign;
        BackfillCountyFromZip(state, &operator_station);
        BackfillGridFromZip(state, &operator_station);
        // On a GMRS net, named as every check-in there is.
        std::string person = SavedEntryName(state->active_net_service, operator_station.name);

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        // SaveNetStation (rather than a bare RecordManualCheckInStation) so
        // logging a check-in also associates the station with this net --
        // otherwise it only ever showed up in future autocomplete for this
        // net (SearchNetStationsByCallsignSubstring also matches on real
        // check-in history), never in the edit-net page's saved-station
        // list or its export, which both query net_saved_stations only.
        std::string existing_remarks =
            state->db->GetSavedNetStationRemarks(state->active_instance.net_id, operator_station.callsign, person);
        state->db->SaveNetStation(state->active_instance.net_id, operator_station, existing_remarks, now, person);
        // The exact grid, later, in the background: no form is open for the
        // operator, so the answer goes to the stored station (see
        // ApplyPreciseGrid).
        RequestPreciseGrid(state, operator_station);

        CheckIn check_in;
        check_in.net_instance_id = state->active_instance.id;
        check_in.callsign = state->operator_callsign;
        check_in.name = person;
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
    static bool FillStationFromKnown(AppState* state, Station* station, NetService service)
    {
        std::string callsign = NormalizeCallsign(station->callsign);
        if (callsign.empty() || IsWildcardCallsign(station->callsign))
        {
            return false;
        }
        std::optional<Station> known = state->db->FindStationByCallsign(callsign);
        if (!known.has_value())
        {
            known = FindLicensee(state->db, callsign, service);
        }
        std::string base = BaseCallsign(callsign);
        if (!known.has_value() && base != callsign)
        {
            known = state->db->FindStationByCallsign(base);
            if (!known.has_value())
            {
                known = FindLicensee(state->db, base, service);
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
        if (!FillStationFromKnown(state, &state->modal_station, state->active_net_service))
        {
            return false;
        }
        FillIfBlank(&state->modal_remarks,
                    state->db->GetSavedNetStationRemarks(
                        state->active_instance.net_id, NormalizeCallsign(state->modal_station.callsign),
                        SavedEntryName(state->active_net_service, state->modal_station.name)));
        // A member ID is this net's alone: another net's is never brought in.
        FillIfBlank(
            &state->modal_station.member_id,
            state->db->GetNetMemberId(state->active_instance.net_id, NormalizeCallsign(state->modal_station.callsign)));
        return true;
    }

    bool FillSavedStationFromKnownStation(AppState* state)
    {
        NetService service = state->edit_net_gmrs ? NetService::kGmrs : NetService::kAmateur;
        if (!FillStationFromKnown(state, &state->saved_station, service))
        {
            return false;
        }
        FillIfBlank(
            &state->saved_station_remarks,
            state->db->GetSavedNetStationRemarks(state->edit_net_id, NormalizeCallsign(state->saved_station.callsign),
                                                 SavedEntryName(service, state->saved_station.name)));
        FillIfBlank(&state->saved_station.member_id,
                    state->db->GetNetMemberId(state->edit_net_id, NormalizeCallsign(state->saved_station.callsign)));
        return true;
    }

    bool LogStationCheckIn(AppState* state)
    {
        if (RefuseViewOnly(state, "log check-ins"))
        {
            return false;
        }
        if (IsWildcardCallsign(state->modal_station.callsign))
        {
            state->form_error = "A ? is a guess. Pick a match, or type the full callsign.";
            return false;
        }
        state->modal_station.callsign = NormalizeCallsign(state->modal_station.callsign);
        if (state->modal_station.callsign.empty())
        {
            state->form_error = "Callsign is required.";
            return false;
        }
        if (!CheckNetCallsign(state, state->modal_station.callsign, state->active_net_service))
        {
            return false;
        }
        // What's known about it, even if it wasn't picked from the matches.
        FillCheckInFromKnownStation(state);
        if (!EnsureActiveSessionOpen(state, state->modal_station.callsign))
        {
            return false;
        }
        // The check below and everything logged, in one transaction: one
        // commit, and no one else can log it in between.
        Database::WriteTransaction writes(state->db);
        // Once per session, counting W4KWK/M as W4KWK. Read fresh: someone
        // else sharing the session may have logged it. On a GMRS net, once
        // per call sign and name: one license covers a whole family.
        bool gmrs = state->active_net_service == NetService::kGmrs;
        std::string person = SavedEntryName(state->active_net_service, state->modal_station.name);
        std::string base = BaseCallsign(state->modal_station.callsign);
        for (const CheckIn& existing : state->db->GetCheckInsForNetInstance(state->active_instance.id))
        {
            if (BaseCallsign(existing.callsign) != base ||
                (gmrs && ToUpperAscii(existing.name) != ToUpperAscii(person)))
            {
                continue;
            }
            std::string who = state->modal_station.callsign + (gmrs && !person.empty() ? " (" + person + ")" : "");
            state->form_error =
                who + " is already in this session's log, as " +
                (existing.callsign == state->modal_station.callsign ? std::string() : existing.callsign + " ") + "#" +
                std::to_string(existing.sequence_number) + ".";
            return false;
        }

        BackfillCountyFromZip(state, &state->modal_station);
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        // SaveNetStation (rather than a bare RecordManualCheckInStation) so
        // logging a check-in also associates the station with this net, the
        // same reasoning as LogOperatorCheckIn above -- and this check-in's
        // own remarks become the saved station's new default remarks,
        // prefilled next time it's picked via autocomplete for this net.
        state->db->SaveNetStation(state->active_instance.net_id, state->modal_station, state->modal_remarks, now,
                                  person);

        CheckIn check_in;
        check_in.net_instance_id = state->active_instance.id;
        check_in.callsign = state->modal_station.callsign;
        check_in.name = person;
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

        writes.Commit();
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
        state->edit_checkin_station.member_id =
            state->db->GetNetMemberId(state->active_instance.net_id, check_in.callsign);
        if (state->active_net_service == NetService::kGmrs)
        {
            state->edit_checkin_station.name = check_in.name;
        }
        // A blank grid takes the ZIP's 4-character one at once, and a
        // 4-character one is refined to 6 in a moment (see ApplyPreciseGrid).
        BackfillGridFromZip(state, &state->edit_checkin_station);
        RequestPreciseGrid(state, state->edit_checkin_station);
        state->edit_checkin_signal_report = check_in.signal_report;
        state->edit_checkin_remarks = check_in.remarks;
        state->edit_checkin_comment = check_in.comment;
        state->edit_checkin_role_choice_labels = RoleChoiceLabels(state);
        state->edit_checkin_role_choice_index = RoleChoiceIndexFromRole(state, check_in.designated_role);

        state->form_error.clear();
        state->show_edit_checkin_modal = true;
    }

    bool SaveEditCheckInForm(AppState* state)
    {
        if (RefuseViewOnly(state, "edit check-ins"))
        {
            return false;
        }
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        const std::string& callsign = state->edit_checkin_original.callsign;
        state->edit_checkin_station.callsign = callsign;
        // On a GMRS net the Name is this check-in's; the station keeps the
        // licensee's.
        bool gmrs = state->active_net_service == NetService::kGmrs;
        std::string person = SavedEntryName(state->active_net_service, state->edit_checkin_station.name);
        const std::string& old_person = state->edit_checkin_original.name;
        bool renamed = gmrs && person != old_person;
        if (renamed && ToUpperAscii(person) != ToUpperAscii(old_person))
        {
            std::string base = BaseCallsign(callsign);
            for (const CheckIn& other : state->db->GetCheckInsForNetInstance(state->active_instance.id))
            {
                if (other.id != state->edit_checkin_original.id && BaseCallsign(other.callsign) == base &&
                    ToUpperAscii(other.name) == ToUpperAscii(person))
                {
                    state->form_error = callsign;
                    if (!person.empty())
                    {
                        state->form_error += " (" + person + ")";
                    }
                    state->form_error += " is already in this session's log, as #";
                    state->form_error += std::to_string(other.sequence_number);
                    state->form_error += ".";
                    return false;
                }
            }
        }

        Database::WriteTransaction writes(state->db);
        Station shared = state->edit_checkin_station;
        if (gmrs)
        {
            std::optional<Station> before = state->db->FindStationByCallsign(callsign);
            shared.name = before.has_value() ? before->name : std::string();
        }
        state->db->UpdateStationFields(shared, now);
        state->db->SetNetMemberId(state->active_instance.net_id, callsign, state->edit_checkin_station.member_id);

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
        if (gmrs)
        {
            check_in.name = person;
        }
        state->db->UpdateCheckIn(check_in);
        if (renamed)
        {
            state->db->RenameNetSavedStationEntry(state->active_instance.net_id, callsign, old_person, person);
        }
        ApplyCheckInRoleDesignation(state, check_in.id, old_role, new_role, check_in.callsign);
        writes.Commit();

        RefreshActiveCheckIns(state);
        state->form_error.clear();
        return true;
    }

    void RefreshNetHistory(AppState* state)
    {
        // Its reads share one snapshot and one lock.
        Database::ReadTransaction reads(state->db);
        state->history_instances.clear();
        state->history_instance_cells.clear();
        state->history_instance_labels.clear();
        state->history_check_ins_read.clear();

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
        // Every session's count in one query.
        std::unordered_map<std::int64_t, std::int64_t> counts =
            state->db->GetCheckInCounts(state->history_ad_hoc ? 0 : state->nets[state->selected_net_index].id);
        for (const NetInstance& instance : state->history_instances)
        {
            std::unordered_map<std::int64_t, std::int64_t>::const_iterator count = counts.find(instance.id);
            state->history_instance_cells.push_back(NetInstanceCells(
                instance, names[instance.net_id], count == counts.end() ? 0 : count->second, state->history_ad_hoc));
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
        // Before reading: a change made meanwhile makes what's kept out of
        // date at once.
        std::int64_t version = state->db->DataVersion();
        state->history_check_ins = state->db->GetCheckInsForNetInstance(selected.id);
        state->history_check_in_cells = CheckInCells(state->db, state->history_check_ins);
        state->history_check_in_labels = FormatCheckInList(state->history_check_in_cells, state->list_width);
        // Kept for moving back to it; an open session can still change.
        if (selected.status == NetInstanceStatus::kClosed)
        {
            if (state->history_check_ins_read.size() >= 32 || version != state->history_check_ins_read_version)
            {
                state->history_check_ins_read.clear();
                state->history_check_ins_read_version = version;
            }
            state->history_check_ins_read[selected.id] = {state->history_check_ins, state->history_check_in_cells};
        }
        else
        {
            state->history_check_ins_read.erase(selected.id);
        }
    }

    void ShowHistoryCheckIns(AppState* state)
    {
        // Someone else changed something: what's kept may be out of date.
        if (!state->history_check_ins_read.empty() && state->db->DataVersion() != state->history_check_ins_read_version)
        {
            state->history_check_ins_read.clear();
        }
        if (state->selected_history_index < static_cast<int>(state->history_instances.size()))
        {
            const NetInstance& selected = state->history_instances[state->selected_history_index];
            std::unordered_map<std::int64_t,
                               std::pair<std::vector<CheckIn>, std::vector<std::vector<std::string>>>>::const_iterator
                read = state->history_check_ins_read.find(selected.id);
            if (selected.status == NetInstanceStatus::kClosed && read != state->history_check_ins_read.end())
            {
                state->history_check_ins = read->second.first;
                state->history_check_in_cells = read->second.second;
                state->history_check_in_labels = FormatCheckInList(state->history_check_in_cells, state->list_width);
                state->selected_history_check_in_index = 0;
                return;
            }
        }
        RefreshHistoryCheckIns(state);
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

    void OfferZmodemSend(AppState* state, const std::string& path, std::int64_t push_net_id,
                         std::int64_t push_instance_id)
    {
        OfferZmodemSendFiles(state, {path}, push_net_id, push_instance_id);
    }

    // The export modal with only a push to offer (see kPushOnly), after a
    // "Saved to" message; none if there's no push either.
    static void OfferPushOnly(AppState* state, const std::vector<std::string>& paths)
    {
        if (ExportOffersPush(state))
        {
            state->zmodem_action = ZmodemAction::kPushOnly;
            state->zmodem_send_paths = paths;
            state->show_zmodem_confirm_modal = true;
        }
    }

    static std::string RemoteCopyMessage(const AppState* state, const std::vector<std::string>& paths,
                                         const std::string& before = "");

    static void SendByProbe(AppState* state, const std::vector<std::string>& paths);

    void OfferZmodemSendFiles(AppState* state, const std::vector<std::string>& paths, std::int64_t push_net_id,
                              std::int64_t push_instance_id, const std::vector<std::string>* zip_contents)
    {
        state->zmodem_zip_contents.clear();
        // Only with an upstream to push to, from the console.
        bool can_push = CanPushUpstream(state);
        state->export_push_net_id = can_push ? push_net_id : 0;
        state->export_push_instance_id = can_push ? push_instance_id : 0;
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
                return;
            }
            OfferPushOnly(state, paths);
            return;
        }
        // Mosh can't carry ZMODEM (see AppState::over_mosh).
        if (state->over_mosh)
        {
            state->status_message = RemoteCopyMessage(state, paths);
            OfferPushOnly(state, paths);
            return;
        }
        // No ZMODEM on this system at all (Windows), so there's
        // nothing to install.
        if (NoZmodemOnThisSystem() || SessionPrefersSftp(state))
        {
            state->status_message = RemoteCopyMessage(state, paths);
            OfferPushOnly(state, paths);
            return;
        }
        if (!ZmodemSendAvailable())
        {
            state->status_message = RemoteCopyMessage(state, paths);
            OfferPushOnly(state, paths);
            return;
        }

        // A key on Ask: no window. A short wait says whether the terminal
        // speaks ZMODEM, and what it says is kept for next time.
        if (zip_contents != nullptr)
        {
            state->zmodem_zip_contents = *zip_contents;
        }
        if (state->ssh_transfer_method == kTransferAsk)
        {
            SendByProbe(state, paths);
            return;
        }
        // A key on ZMODEM: don't send yet -- the transfer hijacks the real
        // terminal for its raw protocol bytes and can't show anything
        // meaningful while it's in flight, and a terminal that doesn't
        // answer by itself needs its receive started first. So the operator
        // gets a chance to do that (or back out) *before* it happens.
        // ConfirmZmodemAction/CancelZmodemAction (wired to the modal's
        // F2/Enter and Esc) do the actual send.
        state->status_message = "Saved to " + ListPaths(zip_contents != nullptr ? *zip_contents : paths) + ".";
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

    static bool ResolveScpTarget(const AppState* state, std::string* user_at_host, std::string* port_option);

    // Who and where an SSH user's scp commands go: "W4KWK@host" and
    // " -P 2200" (empty for port 22), from the server address in the
    // console's Settings, or a guess (see server_address.hpp). False if
    // there is no address to give.
    static bool ScpTarget(const AppState* state, std::string* user_at_host, std::string* port_option)
    {
        // Worked out once per session: it reads the console's settings and
        // may ask DNS.
        if (!state->scp_target_ready)
        {
            state->scp_target_ready = true;
            state->scp_target_found = ResolveScpTarget(state, &state->scp_user_at_host, &state->scp_port_option);
        }
        if (state->scp_target_found)
        {
            *user_at_host = state->scp_user_at_host;
            *port_option = state->scp_port_option;
        }
        return state->scp_target_found;
    }

    static bool ResolveScpTarget(const AppState* state, std::string* user_at_host, std::string* port_option)
    {
        std::string connected_to;
        const char* connection = std::getenv("SSH_CONNECTION");
        if (connection != nullptr)
        {
            std::istringstream words(connection);
            std::string skipped;
            words >> skipped >> skipped >> connected_to;
        }
        AppSettings console = LoadSettings(state->console_settings_path);
        ServerAddress address = ResolveServerAddress(console.server_address, console.server_port, connected_to);
        if (address.host.empty() || state->ssh_username.empty())
        {
            return false;
        }
        *user_at_host = state->ssh_username + "@" + address.host;
        *port_option = address.port != 22 ? " -P " + std::to_string(address.port) : "";
        return true;
    }

    // The scp command an SSH user types to fetch their saved exports, the
    // files by their /exports names. Empty if there is no address to give.
    static std::string ScpCommandFor(const AppState* state, const std::vector<std::string>& paths)
    {
        std::string user_at_host;
        std::string port_option;
        if (paths.empty() || !ScpTarget(state, &user_at_host, &port_option))
        {
            return "";
        }
        std::string command = "scp" + port_option;
        for (const std::string& path : paths)
        {
            std::string name = BaseFileName(path);
            std::string remote = user_at_host + ":/exports/" + name;
            command += name.find_first_of(" \t'\"") == std::string::npos ? " " + remote : " '" + remote + "'";
        }
        return command + " ./";
    }

    const std::string& ScpUploadCommand(AppState* state)
    {
        // Once per session: working out the address can ask DNS.
        if (!state->scp_upload_command_ready)
        {
            state->scp_upload_command_ready = true;
            std::string user_at_host;
            std::string port_option;
            if (ScpTarget(state, &user_at_host, &port_option))
            {
                state->scp_upload_command = "scp" + port_option + " <FILE> " + user_at_host + ":/imports/";
            }
        }
        return state->scp_upload_command;
    }

    // What an SSH user is told after an export they can only fetch with scp.
    static std::string RemoteCopyMessage(const AppState* state, const std::vector<std::string>& paths,
                                         const std::string& before)
    {
        std::string command = ScpCommandFor(state, paths);
        // The command goes on its own line, shown as a block (see StatusLine).
        return command.empty() ? before + "Saved to " + ListPaths(paths) + "."
                               : before + "Saved. Copy it with this command:\n" + command;
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

    bool ExportOffersPush(const AppState* state)
    {
        return state->export_push_net_id != 0 || state->export_push_instance_id != 0;
    }

    void PushExport(AppState* state)
    {
        std::int64_t net_id = state->export_push_net_id;
        std::int64_t instance_id = state->export_push_instance_id;
        state->export_push_net_id = 0;
        state->export_push_instance_id = 0;
        state->show_zmodem_confirm_modal = false;
        // A session's .zip for ZMODEM isn't needed now.
        if (state->zmodem_action == ZmodemAction::kSend)
        {
            state->status_message = "Saved to " + ListPaths(SavedAfterZmodem(state)) + ".";
        }
        if (net_id != 0)
        {
            StartNetPush(state, net_id, "");
        }
        else if (instance_id != 0)
        {
            StartPush(state, instance_id, "");
        }
    }

    // The real ZMODEM sender, or the one a test has put in its place.
    static bool SendFiles(AppState* state, const std::vector<std::string>& paths, int start_timeout_seconds,
                          std::string* error)
    {
        return state->zmodem_send != nullptr ? state->zmodem_send(state->screen, paths, error, start_timeout_seconds)
                                             : SendFilesViaZmodem(state->screen, paths, error, start_timeout_seconds);
    }

    // Keeps what a transfer showed about this key's terminal as its Transfer
    // method (kTransfer*), for this session and, for a key QuickLogger
    // knows (not a login by the system's own sshd), in the database. It can
    // be changed in My Keys. True if it was saved for next time.
    static bool LearnTransferMethod(AppState* state, int method)
    {
        state->ssh_transfer_method = method;
        if (state->ssh_key_id == 0 || state->db == nullptr)
        {
            return false;
        }
        try
        {
            state->db->UpdateUserKeyTransfer(state->ssh_key_id, method);
        }
        catch (const std::exception&)
        {
            return false;  // Busy right now; the session still remembers it.
        }
        return true;
    }

    // What a ZMODEM send came to. `learn`: it was a probe on an Ask key, so
    // the answer is kept as the key's Transfer method: ZMODEM if the terminal
    // answered and took the files, SFTP if it never answered. A transfer that
    // started and then failed or was cancelled says nothing about it.
    static void FinishZmodemSend(AppState* state, bool sent, const std::string& error, bool learn)
    {
        // A terminal that never answered doesn't speak ZMODEM: the files
        // stay (the .zip too) and the scp command is given instead.
        if (!sent && error.find("no receiver responded") != std::string::npos)
        {
            state->zmodem_zip_contents.clear();
            std::string before = "No ZMODEM from your terminal. ";
            if (learn && LearnTransferMethod(state, kTransferSftp))
            {
                before += "This key now shows scp commands; change it in My Keys (F4 on Settings). ";
            }
            state->status_message = RemoteCopyMessage(state, state->zmodem_send_paths, before);
            return;
        }
        // What's left in exports/ to name: without the .zip, once it's
        // removed.
        std::vector<std::string> saved = SavedAfterZmodem(state);
        if (sent)
        {
            // Just the names: the server's folders are no use to the person
            // who has the files now.
            std::string names;
            for (const std::string& path : state->zmodem_send_paths)
            {
                names += names.empty() ? "" : ", ";
                names += BaseFileName(path);
            }
            state->status_message = "Sent " + names + " via ZMODEM.";
            if (learn && LearnTransferMethod(state, kTransferZmodem))
            {
                state->status_message += " This key now sends by ZMODEM; change it in My Keys.";
            }
        }
        else
        {
            state->status_message = "Saved to " + ListPaths(saved) + " (" + error + ")";
        }
    }

    // An SSH user's key is on Ask: no window. The terminal is asked at once
    // whether it speaks ZMODEM, for a few seconds (one that auto-detects it
    // answers in a fraction of one); the files go if it does.
    static void SendByProbe(AppState* state, const std::vector<std::string>& paths)
    {
        state->zmodem_send_paths = paths;
        std::string error;
        bool sent = SendFiles(state, paths, kZmodemProbeSeconds, &error);
        FinishZmodemSend(state, sent, error, true);
    }

    bool HasScpAddress(const AppState* state)
    {
        std::string user_at_host;
        std::string port_option;
        return ScpTarget(state, &user_at_host, &port_option);
    }

    void ConfirmZmodemAction(AppState* state)
    {
        state->show_zmodem_confirm_modal = false;
        std::string error;

        if (state->zmodem_action == ZmodemAction::kPushOnly)
        {
            PushExport(state);
            return;
        }
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
            // The key says ZMODEM (the window was shown, and the terminal was
            // given time to be told to receive): nothing is learned.
            bool sent = SendFiles(state, state->zmodem_send_paths, kZmodemWaitSeconds, &error);
            FinishZmodemSend(state, sent, error, false);
            return;
        }

        // Receiving is only ever for importing, which a view-only user
        // can't do (StartZmodemReceive refuses too).
        if (RefuseViewOnly(state, "import files"))
        {
            return;
        }
        std::string imports_dir = SessionImportsDir(state->db_path, state->ssh_username);
        bool received = state->zmodem_receive != nullptr ? state->zmodem_receive(state->screen, imports_dir, &error)
                                                         : ReceiveFileViaZmodem(state->screen, imports_dir, &error);
        if (received)
        {
            RefreshImportNetFiles(state);
            state->form_error.clear();
            state->status_message = "Received a file. Highlight it below and press F2 to import.";
            // A key on Ask whose terminal just sent by ZMODEM speaks it.
            if (state->ssh_transfer_method == kTransferAsk && LearnTransferMethod(state, kTransferZmodem))
            {
                state->status_message += " This key now sends by ZMODEM; change it in My Keys.";
            }
        }
        else
        {
            state->status_message.clear();
            state->form_error = error;
            // A terminal that never sent anything doesn't speak ZMODEM:
            // say how to upload instead.
            const std::string& upload = ScpUploadCommand(state);
            if (!state->ssh_username.empty() && !upload.empty() &&
                error.find("no sender responded") != std::string::npos)
            {
                state->form_error.clear();
                state->status_message = "Nothing arrived. Upload a file with this command:\n" + upload;
            }
        }
    }

    void CancelZmodemAction(AppState* state)
    {
        state->show_zmodem_confirm_modal = false;
        if (state->zmodem_action == ZmodemAction::kShowFolder || state->zmodem_action == ZmodemAction::kPushOnly)
        {
            return;
        }
        if (state->zmodem_action == ZmodemAction::kSend)
        {
            // An SSH user who skips ZMODEM is told how to fetch the files
            // with scp instead, the .zip included, as when nothing answers.
            if (!ScpCommandFor(state, state->zmodem_send_paths).empty())
            {
                state->zmodem_zip_contents.clear();
                state->status_message = RemoteCopyMessage(state, state->zmodem_send_paths, "ZMODEM skipped. ");
            }
            else
            {
                state->status_message = "Saved to " + ListPaths(SavedAfterZmodem(state)) + " (ZMODEM skipped).";
            }
        }
        else
        {
            const std::string& upload = ScpUploadCommand(state);
            state->status_message = upload.empty() || state->ssh_username.empty()
                                        ? "ZMODEM receive skipped."
                                        : "ZMODEM receive skipped. Upload a file with this command:\n" + upload;
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
                // On a GMRS net, saved here under another name too?
                int entries = 0;
                for (const Station& other : state->edit_net_saved_stations)
                {
                    entries += other.callsign == station.callsign ? 1 : 0;
                }
                if (entries > 1 || state->db->IsStationUsedOutsideNet(station.callsign, state->edit_net_id))
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
                LoadSavedStationIntoForm(state, index);
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
        state->manage_self_service_on = state->db->ServerOptionOn(kOptionSelfServiceKeys);
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

    bool CanEditOwnKeys(const AppState* state)
    {
        return !state->is_console_session && !state->ssh_username.empty();
    }

    static void RefreshMyKeys(AppState* state)
    {
        state->my_keys = state->db->GetUserKeys(state->ssh_username);
        FormatMyKeyLabels(state);
        if (state->selected_my_key_index >= static_cast<int>(state->my_keys.size()))
        {
            state->selected_my_key_index = 0;
        }
    }

    void LoadMyKeyComment(AppState* state)
    {
        if (state->selected_my_key_index < 0 || state->selected_my_key_index >= static_cast<int>(state->my_keys.size()))
        {
            state->my_key_comment_text.clear();
            return;
        }
        state->my_key_comment_text = DescribePublicKey(state->my_keys[state->selected_my_key_index].public_key).comment;
        int method = state->my_keys[state->selected_my_key_index].transfer_method;
        state->my_key_transfer_index = method >= kTransferAsk && method <= kTransferSftp ? method : kTransferAsk;
    }

    void OpenMyKeys(AppState* state)
    {
        if (!CanEditOwnKeys(state))
        {
            return;
        }
        Database::ReadTransaction reads(state->db);
        state->selected_my_key_index = 0;
        RefreshMyKeys(state);
        for (std::size_t i = 0; i < state->my_keys.size(); ++i)
        {
            if (state->my_keys[i].id == state->ssh_key_id)
            {
                state->selected_my_key_index = static_cast<int>(i);
            }
        }
        LoadMyKeyComment(state);
        state->form_error.clear();
        state->status_message.clear();
        state->my_keys_focus = 0;
        state->my_key_new_text.clear();
        state->my_key_remove_armed_id = 0;
        state->my_keys_self_service = SelfServiceKeysOn(state);
        state->show_my_keys_window = true;
    }

    void LoadSessionTransferMethod(AppState* state)
    {
        state->ssh_transfer_method = kTransferAsk;
        if (!CanEditOwnKeys(state) || state->ssh_key_id == 0)
        {
            return;
        }
        for (const User& key : state->db->GetUserKeys(state->ssh_username))
        {
            if (key.id == state->ssh_key_id)
            {
                state->ssh_transfer_method = key.transfer_method;
            }
        }
    }

    bool SessionPrefersSftp(const AppState* state)
    {
        return !state->is_console_session && state->ssh_transfer_method == kTransferSftp;
    }

    bool CanReceiveZmodem(const AppState* state)
    {
        return !(IsLocalTerminal(state->is_console_session) || NoZmodemOnThisSystem() || state->over_mosh ||
                 SessionPrefersSftp(state));
    }

    void CloseMyKeys(AppState* state)
    {
        state->show_my_keys_window = false;
        state->my_keys.clear();
        state->my_keys_labels.clear();
        state->my_key_comment_text.clear();
        state->my_key_new_text.clear();
        state->my_key_remove_armed_id = 0;
        state->form_error.clear();
    }

    void SaveMyKeyComment(AppState* state)
    {
        if (!state->show_my_keys_window || state->selected_my_key_index < 0 ||
            state->selected_my_key_index >= static_cast<int>(state->my_keys.size()))
        {
            return;
        }
        const User& key = state->my_keys[state->selected_my_key_index];
        std::string line;
        std::string error;
        if (!WithPublicKeyComment(key.public_key, state->my_key_comment_text, &line, &error))
        {
            state->form_error = error;
            return;
        }
        {
            // The two changes together: one commit, and both or neither.
            Database::WriteTransaction writes(state->db);
            state->db->UpdateUserKeyLine(key.id, line);
            state->db->UpdateUserKeyTransfer(key.id, state->my_key_transfer_index);
            writes.Commit();
        }
        if (key.id == state->ssh_key_id)
        {
            state->ssh_transfer_method = state->my_key_transfer_index;
        }
        Database::ReadTransaction reads(state->db);
        RefreshMyKeys(state);
        LoadMyKeyComment(state);
        state->form_error.clear();
        state->status_message = "Saved.";
    }

    bool SelfServiceKeysOn(AppState* state)
    {
        return state->db->ServerOptionOn(kOptionSelfServiceKeys);
    }

    void AddMyKey(AppState* state)
    {
        if (!state->show_my_keys_window || !CanEditOwnKeys(state))
        {
            return;
        }
        state->my_key_remove_armed_id = 0;
        state->status_message.clear();
        if (!SelfServiceKeysOn(state))
        {
            state->form_error = "Adding and removing keys is off on this server. Ask the admin.";
            return;
        }
        std::string public_key;
        std::string key_error;
        if (!ValidatePublicKey(state->my_key_new_text, &public_key, &key_error))
        {
            state->form_error = key_error;
            return;
        }
        {
            Database::WriteTransaction writes(state->db);
            std::vector<User> keys = state->db->GetUserKeys(state->ssh_username);
            if (keys.empty())
            {
                state->form_error = "Your account isn't on file.";
                return;
            }
            if (static_cast<int>(keys.size()) >= kMaxKeysPerUser)
            {
                state->form_error =
                    "You have " + std::to_string(kMaxKeysPerUser) + " keys, the most. Remove one first.";
                return;
            }
            std::string owner = state->db->UserOfKey(public_key);
            if (!owner.empty())
            {
                state->form_error = ToUpperAscii(owner) == ToUpperAscii(keys[0].username)
                                        ? "You already have that key."
                                        : "That key belongs to another user.";
                return;
            }
            User key;
            key.username = keys[0].username;
            key.public_key = public_key;
            key.created_at = static_cast<std::int64_t>(std::time(nullptr));
            key.added_by_user = true;
            state->db->CreateUser(key);
            RecordKeyEvent(state, "added", key.username, DescribeKeyLine(public_key));
            writes.Commit();
        }
        state->my_key_new_text.clear();
        Database::ReadTransaction reads(state->db);
        RefreshMyKeys(state);
        LoadMyKeyComment(state);
        state->form_error.clear();
        state->status_message = "Added. Log in with it to try it; F3 removes keys.";
    }

    void RemoveMyKey(AppState* state)
    {
        if (!state->show_my_keys_window || !CanEditOwnKeys(state))
        {
            return;
        }
        state->status_message.clear();
        if (!SelfServiceKeysOn(state))
        {
            state->my_key_remove_armed_id = 0;
            state->form_error = "Adding and removing keys is off on this server. Ask the admin.";
            return;
        }
        if (state->selected_my_key_index < 0 || state->selected_my_key_index >= static_cast<int>(state->my_keys.size()))
        {
            return;
        }
        User key = state->my_keys[state->selected_my_key_index];
        if (state->ssh_key_id == 0)
        {
            state->my_key_remove_armed_id = 0;
            state->form_error = "This login didn't say which key it used, so no key can be removed from it.";
            return;
        }
        if (key.id == state->ssh_key_id)
        {
            state->my_key_remove_armed_id = 0;
            state->form_error = "That's the key this login used. Log in with another to remove it.";
            return;
        }
        if (key.disabled)
        {
            state->my_key_remove_armed_id = 0;
            state->form_error = "The admin turned that key off; only they can remove it.";
            return;
        }
        if (state->my_key_remove_armed_id != key.id)
        {
            state->my_key_remove_armed_id = key.id;
            state->form_error.clear();
            state->status_message = "F3 again removes " + DescribeUserKey(key) + ". Any other key cancels.";
            return;
        }
        state->my_key_remove_armed_id = 0;
        {
            Database::WriteTransaction writes(state->db);
            // Re-read: another session may have changed the keys.
            int working_others = 0;
            bool still_there = false;
            for (const User& other : state->db->GetUserKeys(state->ssh_username))
            {
                still_there = still_there || other.id == key.id;
                working_others += other.id != key.id && !other.disabled ? 1 : 0;
            }
            if (!still_there)
            {
                state->form_error = "That key is already gone.";
            }
            else if (working_others == 0)
            {
                state->form_error = "That's your last working key. Add another first.";
            }
            else
            {
                state->db->DeleteUserKey(key.id);
                RecordKeyEvent(state, "removed", key.username, DescribeKeyLine(key.public_key));
                writes.Commit();
                state->form_error.clear();
                state->status_message = "Removed.";
            }
        }
        Database::ReadTransaction reads(state->db);
        RefreshMyKeys(state);
        LoadMyKeyComment(state);
    }

    void ToggleSelfServiceKeys(AppState* state)
    {
        if (RefuseViewOnly(state, "manage users") || !state->is_console_session)
        {
            return;
        }
        bool on = !state->db->ServerOptionOn(kOptionSelfServiceKeys);
        {
            Database::WriteTransaction writes(state->db);
            state->db->SetServerOption(kOptionSelfServiceKeys, on);
            RecordKeyEvent(state, on ? "self-service on" : "self-service off", "", "");
            writes.Commit();
        }
        state->manage_self_service_on = on;
        state->form_error.clear();
        state->status_message = on ? "Self-service on: users add and remove their own keys in My Keys."
                                   : "Self-service off: only you add and remove keys.";
    }

    void ToggleSelectedUserKeyDisabled(AppState* state)
    {
        if (RefuseViewOnly(state, "manage users") || state->user_keys.empty())
        {
            return;
        }
        User key = state->user_keys[state->selected_user_key_index];
        {
            Database::WriteTransaction writes(state->db);
            state->db->SetUserKeyDisabled(key.id, !key.disabled);
            RecordKeyEvent(state, key.disabled ? "enabled" : "disabled", key.username, DescribeKeyLine(key.public_key));
            writes.Commit();
        }
        RefreshUsers(state);
        state->form_error.clear();
        state->status_message = key.disabled ? "Turned a key of \"" + key.username + "\" back on."
                                             : "Turned off a key of \"" + key.username + "\"; it can't log in.";
    }

    void ToggleAllUserKeysDisabled(AppState* state)
    {
        if (RefuseViewOnly(state, "manage users") || state->user_keys.empty())
        {
            return;
        }
        bool any_enabled = false;
        for (const User& key : state->user_keys)
        {
            any_enabled = any_enabled || !key.disabled;
        }
        std::string username = state->user_keys_username;
        {
            Database::WriteTransaction writes(state->db);
            state->db->SetUserKeysDisabled(username, any_enabled);
            RecordKeyEvent(state, any_enabled ? "disabled" : "enabled", username, "all keys");
            writes.Commit();
        }
        RefreshUsers(state);
        state->form_error.clear();
        state->status_message = any_enabled ? "Turned off every key of \"" + username + "\"; they can't log in."
                                            : "Turned every key of \"" + username + "\" back on.";
    }

    std::string UserKeyDetail(const AppState* state)
    {
        int index = state->selected_user_key_index;
        if (index < 0 || index >= static_cast<int>(state->user_keys.size()) ||
            index >= static_cast<int>(state->user_keys_cells.size()))
        {
            return std::string();
        }
        const User& key = state->user_keys[index];
        // Cell 3 is "2026-10-06 user" (see DescribeKeyAdded).
        return "Highlighted key added: " + state->user_keys_cells[index][3] + (key.disabled ? ". Turned off." : ".");
    }

    std::string UserKeyOffLabel(const AppState* state)
    {
        int index = state->selected_user_key_index;
        bool disabled =
            index >= 0 && index < static_cast<int>(state->user_keys.size()) && state->user_keys[index].disabled;
        return disabled ? "Turn On" : "Turn Off";
    }

    std::string UserKeysOffLabel(const AppState* state)
    {
        for (const User& key : state->user_keys)
        {
            if (!key.disabled)
            {
                return "All Off";
            }
        }
        return "All On";
    }

    void OpenKeyLog(AppState* state)
    {
        Database::ReadTransaction reads(state->db);
        state->key_log_events = state->db->RecentKeyEvents(100);
        ListLayout layout = KeyLogLayout(state->list_width);
        state->key_log_labels.clear();
        state->key_log_labels.reserve(state->key_log_events.size());
        for (const KeyEvent& event : state->key_log_events)
        {
            std::string what = event.action;
            if (!event.username.empty())
            {
                what += " " + event.username;
            }
            if (!event.detail.empty())
            {
                what += ": " + event.detail;
            }
            state->key_log_labels.push_back(
                FormatListRow({FormatLocalDateTime(event.at), event.actor, std::move(what)}, layout));
        }
        state->selected_key_log_index = 0;
        state->status_message.clear();
        state->show_key_log_window = true;
    }

    // `text` as one CSV field: quoted if it has a comma, quote or newline.
    static std::string CsvField(const std::string& text)
    {
        if (text.find_first_of(",\"\r\n") == std::string::npos)
        {
            return text;
        }
        std::string quoted = "\"";
        for (char c : text)
        {
            quoted += c == '"' ? "\"\"" : std::string(1, c);
        }
        return quoted + "\"";
    }

    void ExportKeyLog(AppState* state)
    {
        if (!state->show_key_log_window)
        {
            return;
        }
        // The whole log (the window shows the newest 100), oldest first, one
        // line a change, for a spreadsheet.
        std::vector<KeyEvent> events;
        {
            Database::ReadTransaction reads(state->db);
            events = state->db->RecentKeyEvents(1000);
        }
        std::vector<std::string> lines;
        lines.emplace_back("When,By,Action,User,Detail");
        for (auto event = events.rbegin(); event != events.rend(); ++event)
        {
            lines.push_back(CsvField(FormatLocalDateTime(event->at)) + "," + CsvField(event->actor) + "," +
                            CsvField(event->action) + "," + CsvField(event->username) + "," + CsvField(event->detail));
        }
        std::string path =
            SessionExportsDir(state->db_path, state->ssh_username) + "/key_log_" + CurrentDateIso8601() + ".csv";
        state->form_error.clear();
        ExportLinesToFile(state, path, std::move(lines));
    }

    void CloseKeyLog(AppState* state)
    {
        state->show_key_log_window = false;
        state->key_log_events.clear();
        state->key_log_labels.clear();
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
        // Its reads share one snapshot and one lock; access comes with the keys.
        Database::ReadTransaction reads(state->db);
        std::vector<User> keys = state->db->GetUserKeys(state->user_keys_username);
        state->edit_user_access_index = !keys.empty() && keys[0].view_only ? 1 : 0;
        state->edit_user_amateur_callsign = keys.empty() ? std::string() : keys[0].amateur_callsign;
        state->edit_user_gmrs_callsign = keys.empty() ? std::string() : keys[0].gmrs_callsign;
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
        std::string new_username = Spaceless(state->rename_username);
        state->rename_username = new_username;
        state->status_message.clear();
        bool renaming = new_username != old_username;
        if (renaming && !IsValidUsername(new_username))
        {
            state->form_error = kUsernameRule;
            return;
        }
        std::string amateur = state->edit_user_amateur_callsign;
        std::string gmrs = state->edit_user_gmrs_callsign;
        if (!CheckUserCallsigns(state, &amateur, &gmrs))
        {
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
        std::vector<User> keys = state->db->GetUserKeys(new_username);
        bool callsigns_changed =
            !keys.empty() && (keys[0].amateur_callsign != amateur || keys[0].gmrs_callsign != gmrs);
        if (callsigns_changed)
        {
            state->db->SetUserCallsigns(new_username, amateur, gmrs);
            access_changed = true;
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
                "Renamed " + old_username + " to " + new_username + " and saved, from their next login.";
        }
        else if (renaming)
        {
            state->status_message = "Renamed " + old_username + " to " + new_username + ", from their next login.";
        }
        else if (access_changed)
        {
            state->status_message = "Saved " + new_username + " (" + access + "), from their next login.";
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
        if (added)
        {
            RecordKeyEvent(state, "added", key.username, DescribeKeyLine(public_key));
        }
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
        RecordKeyEvent(state, "removed", key.username, DescribeKeyLine(key.public_key));
        if (last_key)
        {
            CloseUserKeys(state);
        }
        RefreshUsers(state);
        state->form_error.clear();
        state->status_message = last_key ? "Removed \"" + key.username + "\" with their last key."
                                         : "Removed a key of \"" + key.username + "\" (" + DescribeUserKey(key) + ").";
    }

    bool IsValidUsername(const std::string& username)
    {
        if (username.empty() || username.size() > 32 || std::isalnum(static_cast<unsigned char>(username[0])) == 0)
        {
            return false;
        }
        for (char c : username)
        {
            if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '.' && c != '_' && c != '-')
            {
                return false;
            }
        }
        return true;
    }

    bool CheckUserCallsigns(AppState* state, std::string* amateur, std::string* gmrs)
    {
        *amateur = NormalizeCallsign(*amateur);
        *gmrs = NormalizeCallsign(*gmrs);
        if (amateur->empty() && gmrs->empty())
        {
            state->form_error = "At least one call sign is required: Amateur Radio or GMRS.";
            return false;
        }
        if (!amateur->empty() && (amateur->find('/') != std::string::npos || !IsValidCallsign(*amateur)))
        {
            state->form_error =
                *amateur + " isn't a valid US or Canadian amateur call sign (without /M, /P or the like).";
            return false;
        }
        if (!gmrs->empty() && !IsValidGmrsCallsign(*gmrs))
        {
            state->form_error = *gmrs + " isn't a valid GMRS call sign.";
            return false;
        }
        return true;
    }

    void AddUserFromForm(AppState* state)
    {
        if (RefuseViewOnly(state, "manage users"))
        {
            return;
        }
        state->new_user_username = Spaceless(state->new_user_username);
        if (state->new_user_username.empty() || state->new_user_public_key.empty())
        {
            state->status_message.clear();
            state->form_error = "Username and public key are both required.";
            return;
        }
        if (!IsValidUsername(state->new_user_username))
        {
            state->status_message.clear();
            state->form_error = kUsernameRule;
            return;
        }
        // Another key for a user keeps their call signs.
        bool existing = !state->db->GetUserKeys(state->new_user_username).empty();
        if (!existing && !CheckUserCallsigns(state, &state->new_user_amateur_callsign, &state->new_user_gmrs_callsign))
        {
            state->status_message.clear();
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
        user.amateur_callsign = state->new_user_amateur_callsign;
        user.gmrs_callsign = state->new_user_gmrs_callsign;
        bool had_keys = existing;
        bool added = state->db->CreateUser(user);
        if (added)
        {
            RecordKeyEvent(state, "added", user.username, DescribeKeyLine(public_key));
        }

        state->new_user_username.clear();
        state->new_user_public_key.clear();
        state->new_user_amateur_callsign.clear();
        state->new_user_gmrs_callsign.clear();
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
        int keys = CountUserKeys(state, username);
        state->db->DeleteUser(username);
        RecordKeyEvent(state, "user removed", username, std::to_string(keys) + (keys == 1 ? " key" : " keys"));
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
                std::vector<Station>::const_iterator station_at =
                    std::lower_bound(stations.begin(), stations.end(), check_in.callsign, StationCallsignBefore);
                if (station_at != stations.end() && station_at->callsign == check_in.callsign)
                {
                    contact.station = &*station_at;
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

        std::string stem =
            SessionExportsDir(state->db_path, state->ssh_username) + "/" + SessionFileStem(net_name, instance);
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
        std::vector<std::string> paths{log_path, session_path};
        // A closed session can be pushed upstream from the window that follows.
        std::int64_t push_instance_id = instance.status == NetInstanceStatus::kClosed ? instance.id : 0;
        // And for a logging program (see BuildAdif), except a GMRS session:
        // ADIF is for amateur contacts.
        std::optional<Net> net = state->db->GetNetById(instance.net_id);
        if (!net.has_value() || net->service != NetService::kGmrs)
        {
            std::string adif_path = stem + ".adi";
            if (!WriteSessionAdif(state, instance, check_ins, adif_path, &error))
            {
                state->status_message.clear();
                state->form_error =
                    "Saved " + log_path + " and " + session_path + ", but not " + adif_path + ": " + error;
                return;
            }
            paths.push_back(adif_path);
        }
        // For someone at another computer, the three files go as one .zip: one
        // ZMODEM receive, or one short scp command. At the console, the
        // files are all there is.
        if (IsLocalTerminal(state->is_console_session))
        {
            OfferZmodemSendFiles(state, paths, 0, push_instance_id);
            return;
        }
        std::string zip_path = stem + ".zip";
        if (!WriteZipArchive(zip_path, paths, static_cast<std::int64_t>(std::time(nullptr)), &error))
        {
            state->status_message.clear();
            state->form_error = "Saved " + ListPaths(paths) + ", but not " + zip_path + ": " + error;
            return;
        }
        // After a ZMODEM send the .zip is removed, leaving the files. For scp
        // both stay: QuickLogger can't tell when the .zip has been fetched.
        OfferZmodemSendFiles(state, {zip_path}, 0, push_instance_id, &paths);
    }

    void ExportSavedStations(AppState* state, const std::string& net_name)
    {
        std::vector<std::string> lines;
        lines.push_back("Net: " + net_name);
        lines.emplace_back("Saved Stations:");
        lines.emplace_back("");
        // Every column, as in ExportNetLog, from the cells the list was
        // made from (see RefreshEditNetSavedStations): nothing to read again.
        ListLayout layout = ExportListLayout(SavedStationColumns(), 1);
        lines.push_back(FormatListHeading(SavedStationColumns(), layout));
        std::vector<std::string> rows = FormatRows(state->saved_station_cells, layout);
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
        OfferZmodemSend(state, path, net.id);
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
        state->merge_station_callsign_texts.reserve(state->merge_plan.station_conflicts.size());
        state->merge_station_difference_texts.reserve(state->merge_plan.station_conflicts.size());
        for (const MergeStationConflict& conflict : state->merge_plan.station_conflicts)
        {
            state->merge_station_callsign_texts.push_back("  " + conflict.file_station->callsign);
            std::vector<std::string> differences;
            differences.reserve(conflict.differences.size());
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
        // Merging only ever joins nets of one service (Amateur Radio or
        // GMRS); a name taken on the other one can't be merged into or
        // shared.
        if (taken)
        {
            for (const Net& net : state->nets)
            {
                if (NetNamesAreTheSame(slice->net.name, net.name) && net.service != slice->net.service)
                {
                    state->status_message.clear();
                    state->form_error = "You already have a net named \"" + net.name + "\" on " +
                                        ServiceLabel(net.service) + "; this one is on " +
                                        ServiceLabel(slice->net.service) + ". Rename yours to import it.";
                    return;
                }
            }
        }
        if (taken || !names_checked)
        {
            std::vector<Net> alike;
            for (const Net& net : state->nets)
            {
                if (net.service == slice->net.service && (NetNamesAreTheSame(slice->net.name, net.name) ||
                                                          NetNamesLookAlike(slice->net.name, net.name, false)))
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
        state->status_message = "Imported \"" + slice->net.name + R"(". It's listed with today's date as "imported".)";
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

        // Never across services: an Amateur Radio session doesn't go into a
        // GMRS net's History, or the other way round.
        if (!state->import_session_ad_hoc)
        {
            std::optional<Net> target = state->db->GetNetById(state->import_session_net_id);
            if (target.has_value() && target->service != slice->net.service)
            {
                state->status_message.clear();
                state->form_error = "This session was logged on " + std::string(ServiceLabel(slice->net.service)) +
                                    ", and " + target->name + " is on " + ServiceLabel(target->service) + ".";
                return;
            }
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
                instance_id = ApplyAdHocSessionSlice(state->db, *slice, now, &net_id, &error);
                net_name = slice->net.name;
            }
            else
            {
                instance_id = ApplySessionSlice(state->db, *slice, net_id, &error);
            }
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

    // ---- Pulling from the upstream ----------------------------------------

    bool CanPullUpstream(const AppState* state)
    {
        return state->is_console_session && state->pull_runner != nullptr && !state->settings.upstream_host.empty();
    }

    static Upstream UpstreamFromSettings(const AppState* state)
    {
        Upstream upstream;
        upstream.host = state->settings.upstream_host;
        upstream.user = state->settings.upstream_user;
        upstream.port = state->settings.upstream_port;
        return upstream;
    }

    static void RemovePullDirectory(AppState* state)
    {
        if (state->pull_sessions && !state->pull_dir.empty())
        {
            std::error_code ignored;
            std::filesystem::remove_all(state->pull_dir, ignored);
        }
        state->pull_dir.clear();
    }

    static void CloseTheWindow(AppState* state)
    {
        state->pull_stage = PullStage::kNone;
        state->show_pull_modal = false;
        state->pull_nets.clear();
        state->pull_net_labels.clear();
        state->selected_pull_net = 0;
    }

    void OpenPullWindow(AppState* state)
    {
        if (RefuseViewOnly(state, "import files"))
        {
            return;
        }
        if (!CanPullUpstream(state))
        {
            state->status_message.clear();
            state->form_error = "Set up an upstream server in Settings (F5) first.";
            return;
        }
        if (state->import_session && state->import_session_ad_hoc)
        {
            state->status_message.clear();
            state->form_error = "Sessions are pulled into a recurring net's History.";
            return;
        }
        if (!UpstreamToolsAvailable())
        {
            state->status_message.clear();
            state->form_error = kNoSshMessage;
            return;
        }
        state->pull_sessions = state->import_session;
        CloseTheWindow(state);
        state->pull_stage = PullStage::kListing;
        state->show_pull_modal = true;
        ++state->pull_generation;
        state->form_error.clear();
        state->status_message.clear();
        state->pull_runner->ListNets(UpstreamFromSettings(state), state->pull_generation);
    }

    // How a net reads in the Pull window: "Skywarn  (12 sessions)", with its
    // service if it isn't Amateur Radio.
    static std::string PullNetLabel(const UpstreamNet& net)
    {
        std::string label = net.name + "  (" + Count(net.sessions, "session", "sessions") + ")";
        if (net.service == "gmrs")
        {
            label += "  GMRS";
        }
        return label;
    }

    void FinishPullList(AppState* state, const PullResult& result, int generation)
    {
        if (generation != state->pull_generation || state->pull_stage != PullStage::kListing)
        {
            return;
        }
        if (result.kind != PullResultKind::kNets)
        {
            CloseTheWindow(state);
            state->status_message.clear();
            state->form_error = result.message;
            return;
        }

        // The sessions of a net only go into a net on the same service.
        std::optional<Net> target;
        if (state->pull_sessions)
        {
            target = state->db->GetNetById(state->import_session_net_id);
        }
        state->pull_nets.clear();
        state->pull_net_labels.clear();
        state->selected_pull_net = 0;
        bool selected_same = false;
        bool selected_alike = false;
        for (const UpstreamNet& net : result.nets)
        {
            if (target.has_value() && (net.service == "gmrs") != (target->service == NetService::kGmrs))
            {
                continue;
            }
            // Its own name first, else the first one that looks like it.
            int index = static_cast<int>(state->pull_nets.size());
            if (state->pull_sessions && !selected_same && NetNamesAreTheSame(net.name, state->import_session_net_name))
            {
                state->selected_pull_net = index;
                selected_same = true;
            }
            else if (state->pull_sessions && !selected_same && !selected_alike &&
                     NetNamesLookAlike(state->import_session_net_name, net.name))
            {
                state->selected_pull_net = index;
                selected_alike = true;
            }
            state->pull_nets.push_back(net);
            state->pull_net_labels.push_back(PullNetLabel(net));
        }
        if (state->pull_nets.empty())
        {
            CloseTheWindow(state);
            state->status_message.clear();
            state->form_error = state->settings.upstream_host + " has no nets" +
                                (target.has_value() ? std::string(" on ") + ServiceLabel(target->service) : "") + ".";
            return;
        }
        state->pull_stage = PullStage::kChoosing;
    }

    void MovePullHighlight(AppState* state, int delta)
    {
        if (state->pull_stage == PullStage::kChoosing && !state->pull_nets.empty())
        {
            state->selected_pull_net =
                std::clamp(state->selected_pull_net + delta, 0, static_cast<int>(state->pull_nets.size()) - 1);
            state->form_error.clear();
        }
    }

    void ChoosePullNet(AppState* state)
    {
        if (state->pull_stage != PullStage::kChoosing || state->pull_nets.empty())
        {
            return;
        }
        const UpstreamNet& net = state->pull_nets[static_cast<std::size_t>(state->selected_pull_net)];
        if (state->pull_sessions && net.sessions == 0)
        {
            state->form_error = net.name + " has no closed sessions to pull.";
            return;
        }
        state->pull_net_name = net.name;
        std::string imports = SessionImportsDir(state->db_path, state->ssh_username);
        // A net's file goes where received files go, to be imported like
        // one; the sessions' go in a folder of their own, which the list of
        // files to import never shows, and which goes when they're in.
        state->pull_dir = state->pull_sessions ? imports + "/.pull" : imports;
        if (state->pull_sessions)
        {
            std::error_code ignored;
            std::filesystem::remove_all(state->pull_dir, ignored);
        }
        state->form_error.clear();
        state->pull_stage = PullStage::kFetching;
        ++state->pull_generation;
        state->pull_runner->Fetch(UpstreamFromSettings(state), net.name, state->pull_sessions, state->pull_dir,
                                  state->pull_generation);
    }

    // Adds the pulled sessions to the net whose History opened the import
    // page, skipping the ones it has.
    static void ImportPulledSessions(AppState* state, const PullResult& result)
    {
        std::optional<Net> target = state->db->GetNetById(state->import_session_net_id);
        if (!target.has_value())
        {
            state->status_message.clear();
            state->form_error = "That net no longer exists.";
            return;
        }
        int added = 0;
        int already = 0;
        int other_service = 0;
        int unreadable = 0;
        try
        {
            Database::WriteTransaction transaction(state->db);
            // Oldest first, so they're numbered in the order they happened.
            for (std::size_t i = result.files.size(); i > 0; --i)
            {
                std::string error;
                std::optional<NetSlice> slice = ReadSessionSliceFile(result.files[i - 1], &error);
                if (!slice.has_value())
                {
                    ++unreadable;
                    continue;
                }
                if (slice->net.service != target->service)
                {
                    ++other_service;
                    continue;
                }
                const NetInstance& source = slice->instances[0];
                if (state->db->HasNetInstance(target->id, source.instance_date, source.started_at))
                {
                    ++already;
                    continue;
                }
                if (ApplySessionSlice(state->db, *slice, target->id, &error) == 0)
                {
                    ++unreadable;
                    continue;
                }
                ++added;
            }
            transaction.Commit();
        }
        catch (const std::exception& e)
        {
            state->status_message.clear();
            state->form_error = std::string("Import failed, so nothing was added: ") + e.what();
            return;
        }

        std::string message;
        if (added == 0)
        {
            message =
                "Nothing new: " + state->settings.upstream_host + "'s sessions of " + result.net + " are here already.";
        }
        else
        {
            message = "Added " + Count(added, "session", "sessions") + " of " + result.net + " from " +
                      state->settings.upstream_host + "; " + std::to_string(already) + " here already.";
        }
        if (other_service > 0)
        {
            message += " " + Count(other_service, "session", "sessions") + " on the other service skipped.";
        }
        if (unreadable > 0)
        {
            message += " " + Count(unreadable, "session", "sessions") + " couldn't be read.";
        }
        if (result.not_sent > 0)
        {
            message += " " + std::to_string(result.not_sent) + " not sent (still open, or over the limit).";
        }
        state->import_session = false;
        RefreshNetHistory(state);
        state->form_error.clear();
        state->status_message = std::move(message);
        state->page = kPageNetHistory;
    }

    void FinishPullFiles(AppState* state, const PullResult& result, int generation)
    {
        if (generation != state->pull_generation || state->pull_stage != PullStage::kFetching)
        {
            return;
        }
        CloseTheWindow(state);
        if (result.kind != PullResultKind::kFiles)
        {
            RemovePullDirectory(state);
            state->status_message.clear();
            state->form_error = result.message;
            return;
        }
        if (result.files.empty())
        {
            RemovePullDirectory(state);
            state->status_message.clear();
            state->form_error =
                state->settings.upstream_host + " has no closed sessions of " + result.net + " to send.";
            return;
        }

        if (state->pull_sessions)
        {
            ImportPulledSessions(state, result);
            RemovePullDirectory(state);
            return;
        }
        // The net's file is among the received ones now: highlight it and
        // import it as any is, which asks about a net it looks like.
        const std::string& path = result.files[0];
        std::string name = path.substr(path.find_last_of('/') + 1);
        RefreshImportNetFiles(state);
        for (std::size_t i = 0; i < state->import_net_files.size(); ++i)
        {
            if (state->import_net_files[i] == name)
            {
                state->selected_import_file_index = static_cast<int>(i);
            }
        }
        state->pull_dir.clear();
        state->form_error.clear();
        state->status_message = "Pulled " + result.net + " from " + state->settings.upstream_host + ".";
        ImportSelectedNetSlice(state);
    }

    void ClosePullWindow(AppState* state)
    {
        if (state->pull_runner != nullptr)
        {
            state->pull_runner->Cancel();
        }
        ++state->pull_generation;
        CloseTheWindow(state);
        RemovePullDirectory(state);
        state->form_error.clear();
    }

    void StartZmodemReceive(AppState* state)
    {
        if (RefuseViewOnly(state, "import files"))
        {
            return;
        }
        // Nobody on the other end of a local terminal to send one, no
        // ZMODEM at all on Windows, and none over
        // Mosh.
        if (!CanReceiveZmodem(state))
        {
            return;
        }
        state->zmodem_action = ZmodemAction::kReceive;
        state->show_zmodem_confirm_modal = true;
    }

    // A match for wildcard matching, to sort by how tightly it fits: its
    // span, and where it was in the list it came from.
    struct WildcardRanked
    {
        int span;
        std::size_t index;
    };

    // Tightest span first; equals in the order they came.
    static bool WildcardRankedBefore(const WildcardRanked& a, const WildcardRanked& b)
    {
        return a.span != b.span ? a.span < b.span : a.index < b.index;
    }

    // How many matches wildcard matching fetches from the database before
    // ranking them and keeping the best (RankWildcardMatches): more than can
    // be shown, as the best fit may not be first by callsign.
    static constexpr int kWildcardFetch = 500;

    // Keeps the best `keep` of `stations`, which all match the wildcard
    // `typed`: those fitting it in the fewest characters first (a plain
    // contiguous match fits in as many as were typed), equals in the order
    // given. With `anchored` the first character typed starts the callsign.
    static void RankWildcardMatches(std::vector<Station>* stations, const std::string& typed, bool anchored,
                                    std::size_t keep)
    {
        std::string letters = NormalizeCallsign(typed);
        std::vector<WildcardRanked> ranked;
        for (std::size_t i = 0; i < stations->size(); ++i)
        {
            const std::string& callsign = (*stations)[i].callsign;
            int span = WildcardSpan(letters, callsign.data(), callsign.size(), anchored);
            if (span >= 0)
            {
                ranked.push_back({span, i});
            }
        }
        std::sort(ranked.begin(), ranked.end(), WildcardRankedBefore);
        std::vector<Station> kept;
        for (std::size_t i = 0; i < ranked.size() && i < keep; ++i)
        {
            kept.push_back(std::move((*stations)[ranked[i].index]));
        }
        *stations = std::move(kept);
    }

    static void AppendNearbyUlsSuggestions(AppState* state, const std::string& typed, const std::string& net_zip,
                                           bool partial, NetService service, std::size_t max_suggestions,
                                           std::vector<Station>* suggestions, std::vector<std::string>* sources);
    static void AppendNearbyIsedSuggestions(AppState* state, const std::string& typed, bool partial,
                                            std::size_t max_suggestions, std::vector<Station>* suggestions,
                                            std::vector<std::string>* sources);
    static void AppendCanadianSuggestions(const AppState* state, const std::string& typed, bool partial,
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
    static void MarkTypedCallsignMatch(AppState* state, const std::string& typed, NetService service,
                                       std::size_t max_suggestions, std::vector<Station>* suggestions,
                                       std::vector<std::string>* sources, int* selected)
    {
        // A wildcard is no call sign: the best fit stays on top.
        if (suggestions->empty() || IsWildcardCallsign(typed))
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
            known = FindLicensee(state->db, callsign, service);
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
            std::string station_key = ZipCentroidKey(known->zip);
            std::optional<ZipCentroid> station_zip =
                station_key.empty() ? std::nullopt : state->db->FindZipCentroid(station_key);
            if (origin.has_value() && station_zip.has_value())
            {
                double miles = DistanceMiles(origin->lat, origin->lon, station_zip->lat, station_zip->lon);
                source = LooksCanadian(callsign) ? IsedSource(miles) : UlsSource(miles);
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

        // A "?" without two characters to go on matches nothing yet.
        const std::string& typed = state->modal_station.callsign;
        bool wildcard = IsWildcardCallsign(typed);
        if (wildcard && !WildcardHasEnough(typed))
        {
            return;
        }
        const std::size_t kMaxSuggestions = MaxCallsignMatches(state);
        const int max_suggestions = static_cast<int>(kMaxSuggestions);
        // Wildcard matches are ranked, so more than can be shown are fetched.
        const int fetch = wildcard ? kWildcardFetch : max_suggestions;

        // Tier 1: callers already known to this specific net (real check-ins
        // or SaveNetStation). No more than can be shown.
        state->modal_callsign_suggestions =
            state->db->SearchNetStationsByCallsignSubstring(state->active_instance.net_id, typed, fetch);
        if (wildcard)
        {
            RankWildcardMatches(&state->modal_callsign_suggestions, typed, false, kMaxSuggestions);
        }
        std::size_t tier1_count = state->modal_callsign_suggestions.size();

        // Tier 2: callers known to other nets (see
        // SearchStationsByCallsignSubstring). It includes tier 1's, which
        // are dropped as repeats, so enough to fill the rest after those.
        std::vector<Station> other_matches = state->db->SearchStationsByCallsignSubstring(
            typed, wildcard ? fetch : max_suggestions + static_cast<int>(tier1_count));
        if (wildcard)
        {
            RankWildcardMatches(&other_matches, typed, false, kMaxSuggestions + tier1_count);
        }
        AppendNewSuggestions(&state->modal_callsign_suggestions, &other_matches, kMaxSuggestions,
                             state->active_net_service);

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
        OfferNewGmrsName(state->active_net_service, state->modal_station.callsign, kMaxSuggestions,
                         &state->modal_callsign_suggestions, &state->modal_callsign_suggestion_sources);

        // Tier 3: licensed stations near the net, from the FCC data (its
        // GMRS licensees, on a GMRS net).
        AppendNearbyUlsSuggestions(state, state->modal_station.callsign, state->active_net_zip,
                                   !state->active_net_partial_match_canada, state->active_net_service, kMaxSuggestions,
                                   &state->modal_callsign_suggestions, &state->modal_callsign_suggestion_sources);
        // Tier 4: Canadian call signs, from ISED's data -- those near the net
        // first, then the rest. Canada has no GMRS.
        if (state->active_net_service != NetService::kGmrs)
        {
            AppendNearbyIsedSuggestions(state, state->modal_station.callsign, state->active_net_partial_match_canada,
                                        kMaxSuggestions, &state->modal_callsign_suggestions,
                                        &state->modal_callsign_suggestion_sources);
            AppendCanadianSuggestions(state, state->modal_station.callsign, state->active_net_partial_match_canada,
                                      kMaxSuggestions, &state->modal_callsign_suggestions,
                                      &state->modal_callsign_suggestion_sources);
        }
        MarkTypedCallsignMatch(state, state->modal_station.callsign, state->active_net_service, kMaxSuggestions,
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
        RequestPreciseGrid(state, state->modal_station);

        std::string default_remarks =
            state->db->GetSavedNetStationRemarks(state->active_instance.net_id, state->modal_station.callsign,
                                                 SavedEntryName(state->active_net_service, state->modal_station.name));
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
        state->edit_net_gmrs = net.service == NetService::kGmrs;
        state->edit_net_amateur = !state->edit_net_gmrs;
        int channel = FindGmrsChannel(net.default_frequency, net.repeater_offset);
        state->edit_net_gmrs_channel = channel < 0 ? 0 : channel;

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
        Net net;
        if (!ReadEditNetRadio(state, &net) || !CheckNetZip(state, state->edit_net_location))
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

        net.id = state->edit_net_id;
        net.name = state->edit_net_name;
        net.default_location = NormalizeZipOrPostalCode(state->edit_net_location);
        net.recurrence_description = state->edit_net_recurrence;
        net.comments = state->edit_net_comments;
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
        std::vector<SavedNetStation> entries = state->db->GetSavedNetEntries(state->edit_net_id);
        state->edit_net_saved_stations.clear();
        state->edit_net_saved_entry_names.clear();
        state->edit_net_saved_remarks.clear();
        state->saved_station_cells.clear();
        for (SavedNetStation& entry : entries)
        {
            if (!entry.name.empty())
            {
                entry.station.name = entry.name;
            }
            state->saved_station_cells.push_back(SavedStationCells(entry.station, entry.default_remarks));
            state->edit_net_saved_stations.push_back(std::move(entry.station));
            state->edit_net_saved_entry_names.push_back(std::move(entry.name));
            state->edit_net_saved_remarks.push_back(std::move(entry.default_remarks));
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
        if (IsWildcardCallsign(state->saved_station.callsign))
        {
            state->form_error = "A ? is a guess. Pick a match, or type the full callsign.";
            return false;
        }
        state->saved_station.callsign = NormalizeCallsign(state->saved_station.callsign);
        if (state->saved_station.callsign.empty())
        {
            state->form_error = "Callsign is required.";
            return false;
        }
        if (!CheckNetCallsign(state, state->saved_station.callsign,
                              state->edit_net_gmrs ? NetService::kGmrs : NetService::kAmateur))
        {
            return false;
        }
        // Already saved: the same call sign, or on a GMRS net the same
        // call sign and name (one license covers a family).
        NetService service = state->edit_net_gmrs ? NetService::kGmrs : NetService::kAmateur;
        bool loaded = CallsignsEqual(state->saved_station_loaded_callsign, state->saved_station.callsign);
        std::string person = SavedEntryName(service, state->saved_station.name);
        int found = -1;
        for (std::size_t i = 0; i < state->edit_net_saved_stations.size() && found < 0; ++i)
        {
            if (CallsignsEqual(state->edit_net_saved_stations[i].callsign, state->saved_station.callsign) &&
                ToUpperAscii(state->edit_net_saved_entry_names[i]) == ToUpperAscii(person))
            {
                found = static_cast<int>(i);
            }
        }
        std::string old_name;
        bool already_saved = found >= 0 || (loaded && service == NetService::kGmrs);
        if (found >= 0)
        {
            old_name = state->edit_net_saved_entry_names[static_cast<std::size_t>(found)];
            if (loaded && ToUpperAscii(old_name) != ToUpperAscii(state->saved_station_loaded_name))
            {
                state->form_error = state->saved_station.callsign + " (" + person + ") is already saved to this net.";
                return false;
            }
        }
        else if (already_saved)
        {
            old_name = state->saved_station_loaded_name;  // Renamed.
        }
        // A new one gets what's known about it, even if it wasn't picked
        // from the matches. (One already saved is being edited: a field
        // cleared there is meant to be cleared.)
        if (!already_saved)
        {
            FillSavedStationFromKnownStation(state);
            person = SavedEntryName(service, state->saved_station.name);
        }

        BackfillCountyFromZip(state, &state->saved_station);
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (already_saved)
        {
            state->db->UpdateSavedNetStation(state->edit_net_id, state->saved_station, state->saved_station_remarks,
                                             now, old_name, person);
        }
        else
        {
            state->db->SaveNetStation(state->edit_net_id, state->saved_station, state->saved_station_remarks, now,
                                      person);
        }

        state->status_message =
            "Saved " + state->saved_station.callsign + (person.empty() ? "" : " (" + person + ")") + ".";
        state->saved_station = Station();
        state->saved_station_remarks.clear();
        state->saved_station_loaded_callsign.clear();
        state->saved_station_loaded_name.clear();
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

    void LoadSavedStationIntoForm(AppState* state, std::size_t index)
    {
        if (index >= state->edit_net_saved_stations.size())
        {
            return;
        }
        CloseSavedStationForm(state);
        state->saved_station = state->edit_net_saved_stations[index];
        state->saved_station_remarks = state->edit_net_saved_remarks[index];
        state->saved_station_loaded_callsign = state->saved_station.callsign;
        state->saved_station_loaded_name = state->edit_net_saved_entry_names[index];
        // As when it is logged in a session: a blank grid gets the ZIP's, and
        // a 4-character one is refined in the background to the street's
        // (shown in the form; kept only if the station is saved).
        BackfillGridFromZip(state, &state->saved_station);
        RequestPreciseGrid(state, state->saved_station);
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
        state->saved_station_loaded_callsign.clear();
        state->saved_station_loaded_name.clear();
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

        std::size_t index = static_cast<std::size_t>(state->selected_saved_station_index);
        std::string callsign = state->edit_net_saved_stations[index].callsign;
        state->db->RemoveSavedNetStation(state->edit_net_id, callsign, state->edit_net_saved_entry_names[index]);
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
        std::string net_key = ZipCentroidKey(net_zip);
        if (!net_key.empty())
        {
            origin = state->db->FindZipCentroid(net_key);
        }
        std::string home_key = ZipCentroidKey(state->settings.location);
        if (!origin.has_value() && !home_key.empty())
        {
            origin = state->db->FindZipCentroid(home_key);
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
        state->nearby_ised_origin.clear();
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

    static bool NearbyCallsignMatches(const NearbyUlsCallsign& candidate, const std::string& upper, bool anywhere);

    // How many characters of `candidate`'s callsign the wildcard `letters`
    // span (see WildcardSpan), -1 if they don't fit in order. Checked in
    // place, like NearbyCallsignMatches.
    static int NearbyWildcardSpan(const NearbyUlsCallsign& candidate, const std::string& letters, bool anchored)
    {
        const void* end = std::memchr(candidate.callsign, '\0', sizeof(candidate.callsign));
        std::size_t length = end == nullptr
                                 ? sizeof(candidate.callsign)
                                 : static_cast<std::size_t>(static_cast<const char*>(end) - candidate.callsign);
        return WildcardSpan(letters, candidate.callsign, length, anchored);
    }

    // The shared end of the nearby tiers: the `candidates` (nearest first)
    // that match `typed`, up to `max_suggestions` in all, skipping
    // call signs already in `suggestions`, and their records in one query --
    // from ISED's table when `canadian`, else the FCC's `table`. With a "?"
    // typed (see IsWildcardCallsign) they're the ones the characters typed
    // fit in order; still nearest first, however loose the fit.
    static void AppendNearbyMatches(Database* db, const std::vector<NearbyUlsCallsign>& candidates,
                                    const std::string& typed, bool partial, bool canadian, LicenseTable table,
                                    std::size_t max_suggestions, std::vector<Station>* suggestions,
                                    std::vector<std::string>* sources)
    {
        // The matches, nearest first, then their records in one query.
        bool wildcard = IsWildcardCallsign(typed);
        if (wildcard && !WildcardHasEnough(typed))
        {
            return;
        }
        // Only the form being matched with is made, once, not per candidate.
        std::string upper = wildcard ? std::string() : ToUpperAscii(typed);
        std::string letters = wildcard ? NormalizeCallsign(typed) : std::string();
        std::vector<const NearbyUlsCallsign*> matches;
        std::vector<std::string> match_callsigns;
        for (const NearbyUlsCallsign& candidate : candidates)
        {
            if (suggestions->size() + matches.size() >= max_suggestions)
            {
                break;
            }
            bool fits = wildcard ? NearbyWildcardSpan(candidate, letters, !partial) >= 0
                                 : NearbyCallsignMatches(candidate, upper, partial);
            if (!fits)
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
            if (!already_known)
            {
                matches.push_back(&candidate);
            }
        }
        for (const NearbyUlsCallsign* match : matches)
        {
            match_callsigns.emplace_back(match->callsign);
        }
        std::vector<Station> records = canadian ? db->FindIsedStationsByCallsigns(match_callsigns)
                                                : db->FindUlsStationsByCallsigns(match_callsigns, table);
        for (const NearbyUlsCallsign* match : matches)
        {
            std::string callsign = match->callsign;
            std::vector<Station>::const_iterator found =
                std::lower_bound(records.begin(), records.end(), callsign, StationCallsignBefore);
            // Gone if the station data was refreshed since the list loaded.
            if (found != records.end() && found->callsign == callsign)
            {
                suggestions->push_back(*found);
                sources->push_back(canadian ? IsedSource(match->miles) : UlsSource(match->miles));
            }
        }
    }

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
                                           bool partial, NetService service, std::size_t max_suggestions,
                                           std::vector<Station>* suggestions, std::vector<std::string>* sources)
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
        LicenseTable table = service == NetService::kGmrs ? LicenseTable::kGmrs : LicenseTable::kAmateur;
        if (state->nearby_uls_origin != state->nearby_zips_origin || state->nearby_uls_table != table ||
            now - state->nearby_uls_loaded_at > kNearbyUlsReloadSeconds)
        {
            state->nearby_uls_callsigns =
                state->db->ListNearbyUlsCallsigns(state->nearby_zips, state->nearby_zip3_prefixes, table);
            state->nearby_uls_callsigns.shrink_to_fit();
            state->nearby_uls_origin = state->nearby_zips_origin;
            state->nearby_uls_table = table;
            state->nearby_uls_loaded_at = now;
        }

        AppendNearbyMatches(state->db, state->nearby_uls_callsigns, typed, partial, false, table, max_suggestions,
                            suggestions, sources);
    }

    // Canadian licensees near the same origin, from ISED's data: the same
    // tier as the FCC's, with distances by FSA (see Database::
    // ListNearbyIsedCallsigns). Only loaded when what's typed can match a
    // Canadian call sign: it looks like one, or Partial Matching is Canada.
    static void AppendNearbyIsedSuggestions(AppState* state, const std::string& typed, bool partial,
                                            std::size_t max_suggestions, std::vector<Station>* suggestions,
                                            std::vector<std::string>* sources)
    {
        if (suggestions->size() >= max_suggestions || state->nearby_zips.empty() || (!partial && !LooksCanadian(typed)))
        {
            return;
        }
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (state->nearby_ised_origin != state->nearby_zips_origin ||
            now - state->nearby_ised_loaded_at > kNearbyUlsReloadSeconds)
        {
            state->nearby_ised_callsigns = state->db->ListNearbyIsedCallsigns(state->nearby_zips);
            state->nearby_ised_callsigns.shrink_to_fit();
            state->nearby_ised_origin = state->nearby_zips_origin;
            state->nearby_ised_loaded_at = now;
        }
        AppendNearbyMatches(state->db, state->nearby_ised_callsigns, typed, partial, true, LicenseTable::kAmateur,
                            max_suggestions, suggestions, sources);
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
    static void AppendCanadianSuggestions(const AppState* state, const std::string& typed, bool partial,
                                          std::size_t max_suggestions, std::vector<Station>* suggestions,
                                          std::vector<std::string>* sources)
    {
        // With a "?" typed (see IsWildcardCallsign) the database is given it
        // as typed, and the matches are ranked; anchored at the start when
        // only a prefix is wanted.
        bool wildcard = IsWildcardCallsign(typed);
        std::string normalized = NormalizeCallsign(typed);
        if (suggestions->size() >= max_suggestions || normalized.empty() || (wildcard && !WildcardHasEnough(typed)) ||
            (!partial && !LooksCanadian(typed)))
        {
            return;
        }
        int limit = wildcard ? kWildcardFetch : static_cast<int>(max_suggestions);
        std::vector<Station> matches =
            partial ? state->db->SearchIsedStationsByCallsignSubstring(wildcard ? typed : normalized, limit)
                    : state->db->SearchIsedStationsByCallsignPrefix(wildcard ? typed : normalized, limit);
        if (wildcard)
        {
            RankWildcardMatches(&matches, typed, !partial, max_suggestions);
        }
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
                sources->emplace_back(kIsedSource);
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

        // A "?" without two characters to go on matches nothing yet.
        const std::string& typed = state->saved_station.callsign;
        bool wildcard = IsWildcardCallsign(typed);
        if (wildcard && !WildcardHasEnough(typed))
        {
            return;
        }
        const std::size_t kMaxSuggestions = MaxCallsignMatches(state);

        // Tier 1: callers already known to this specific net (real check-ins
        // or previously saved) -- same query as the New Station modal's tier 1.
        const int max_suggestions = static_cast<int>(kMaxSuggestions);
        const int fetch = wildcard ? kWildcardFetch : max_suggestions;
        state->saved_station_suggestions =
            state->db->SearchNetStationsByCallsignSubstring(state->edit_net_id, typed, fetch);
        if (wildcard)
        {
            RankWildcardMatches(&state->saved_station_suggestions, typed, false, kMaxSuggestions);
        }
        std::size_t tier1_count = state->saved_station_suggestions.size();

        // Tier 2: callers known to other nets; as in RefreshCallsignSuggestions,
        // enough to fill the rest after tier 1's repeats.
        std::vector<Station> other_matches = state->db->SearchStationsByCallsignSubstring(
            typed, wildcard ? fetch : max_suggestions + static_cast<int>(tier1_count));
        if (wildcard)
        {
            RankWildcardMatches(&other_matches, typed, false, kMaxSuggestions + tier1_count);
        }
        NetService service = state->edit_net_gmrs ? NetService::kGmrs : NetService::kAmateur;
        AppendNewSuggestions(&state->saved_station_suggestions, &other_matches, kMaxSuggestions, service);

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
        OfferNewGmrsName(service, state->saved_station.callsign, kMaxSuggestions, &state->saved_station_suggestions,
                         &state->saved_station_suggestion_sources);

        // Tier 3: nearby ULS-imported stations.
        AppendNearbyUlsSuggestions(state, state->saved_station.callsign, state->edit_net_location,
                                   state->edit_net_partial_match_index == 0, service, kMaxSuggestions,
                                   &state->saved_station_suggestions, &state->saved_station_suggestion_sources);
        // And Canadian call signs, from ISED's data.
        if (service != NetService::kGmrs)
        {
            AppendNearbyIsedSuggestions(state, state->saved_station.callsign, state->edit_net_partial_match_index == 1,
                                        kMaxSuggestions, &state->saved_station_suggestions,
                                        &state->saved_station_suggestion_sources);
            AppendCanadianSuggestions(state, state->saved_station.callsign, state->edit_net_partial_match_index == 1,
                                      kMaxSuggestions, &state->saved_station_suggestions,
                                      &state->saved_station_suggestion_sources);
        }
        MarkTypedCallsignMatch(state, state->saved_station.callsign, service, kMaxSuggestions,
                               &state->saved_station_suggestions, &state->saved_station_suggestion_sources,
                               &state->selected_saved_station_suggestion_index);
        state->saved_station_suggestion_labels =
            FormatMatches(state->saved_station_suggestions, state->saved_station_suggestion_sources, state->list_width);
    }

    void ApplySelectedSavedStationSuggestion(AppState* state)
    {
        if (state->saved_station_suggestions.empty())
        {
            if (FillSavedStationFromKnownStation(state))
            {
                RequestPreciseGrid(state, state->saved_station);
            }
            return;
        }
        // The match marked ">" (see MarkTypedCallsignMatch).
        state->saved_station = state->saved_station_suggestions[state->selected_saved_station_suggestion_index];
        // Fill County in now, so it shows in the form as soon as the station
        // is picked rather than only once it's saved. (ULS records never
        // carry a county, so a ULS suggestion always needs this.)
        BackfillCountyFromZip(state, &state->saved_station);
        BackfillGridFromZip(state, &state->saved_station);
        RequestPreciseGrid(state, state->saved_station);
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
        state->saved_station_suggestion_sources.clear();
    }

    void BackfillCountyFromZip(const AppState* state, Station* station)
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

    void BackfillGridFromZip(const AppState* state, Station* station)
    {
        if (!station->grid_square.empty())
        {
            return;
        }
        // A US ZIP's centroid, or a Canadian postal code's FSA centroid.
        std::string key = ZipCentroidKey(station->zip);
        if (key.empty())
        {
            return;
        }
        std::optional<ZipCentroid> centroid = state->db->FindZipCentroid(key);
        if (centroid.has_value())
        {
            station->grid_square = MaidenheadGrid4(centroid->lat, centroid->lon);
        }
    }

    void RequestPreciseGrid(AppState* state, const Station& station)
    {
        if (state->grid_lookup == nullptr || (!station.grid_square.empty() && station.grid_square.size() != 4) ||
            !CanLookUpGrid(station))
        {
            return;
        }
        state->grid_lookup->Request(MakeGridRequest(station));
    }

    // The 4-character grid a ZIP or postal code gives a station that has
    // none (see BackfillGridFromZip), or "" if it has no known centroid.
    static std::string ZipGrid4(AppState* state, const std::string& zip)
    {
        if (state->db == nullptr)
        {
            return std::string();
        }
        std::string key = ZipCentroidKey(zip);
        std::optional<ZipCentroid> centroid = key.empty() ? std::nullopt : state->db->FindZipCentroid(key);
        return centroid.has_value() ? MaidenheadGrid4(centroid->lat, centroid->lon) : std::string();
    }

    void ApplyPreciseGrid(AppState* state, const std::string& callsign, const std::string& grid)
    {
        bool changed = false;
        if (state->show_new_station_modal && state->modal_station.callsign == callsign &&
            ShouldTakePreciseGrid(state->modal_station.grid_square, grid, ZipGrid4(state, state->modal_station.zip)))
        {
            state->modal_station.grid_square = grid;
            changed = true;
        }
        if (state->show_saved_station_modal && state->saved_station.callsign == callsign &&
            ShouldTakePreciseGrid(state->saved_station.grid_square, grid, ZipGrid4(state, state->saved_station.zip)))
        {
            state->saved_station.grid_square = grid;
            changed = true;
        }
        if (state->show_edit_checkin_modal && state->edit_checkin_original.callsign == callsign &&
            ShouldTakePreciseGrid(state->edit_checkin_station.grid_square, grid,
                                  ZipGrid4(state, state->edit_checkin_station.zip)))
        {
            state->edit_checkin_station.grid_square = grid;
            changed = true;
        }
        // And the stored station, wherever it was asked for from (the
        // operator's own, at the start of a session, has no form): its grid
        // is only ever extended, never changed (see UpdateStationGrid).
        if (state->db != nullptr)
        {
            std::optional<Station> stored = state->db->FindStationByCallsign(callsign);
            state->db->UpdateStationGrid(callsign, grid, stored.has_value() ? ZipGrid4(state, stored->zip) : "");
        }
        if (changed && state->screen != nullptr)
        {
            state->screen->PostEvent(ftxui::Event::Custom);
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

    // The names on file for `callsigns`, by callsign, read in one query.
    static std::unordered_map<std::string, std::string> StationNames(const Database* db,
                                                                     const std::vector<std::string>& callsigns)
    {
        std::vector<std::string> upper;
        upper.reserve(callsigns.size());
        for (const std::string& callsign : callsigns)
        {
            upper.push_back(ToUpperAscii(callsign));
        }
        std::unordered_map<std::string, std::string> names;
        for (Station& station : db->FindStationsByCallsigns(upper))
        {
            names[station.callsign] = std::move(station.name);
        }
        return names;
    }

    static const std::string& NameOf(const std::unordered_map<std::string, std::string>& names,
                                     const std::string& callsign)
    {
        static const std::string kNone;
        std::unordered_map<std::string, std::string>::const_iterator found = names.find(ToUpperAscii(callsign));
        return found == names.end() ? kNone : found->second;
    }

    // "13 of the last 20", "1 of 1".
    static std::string OutOf(int part, int whole)
    {
        return std::to_string(part) + " of " + std::to_string(whole);
    }

    // The active net's other sessions, newest first.
    static std::vector<NetInstance> OtherSessions(const AppState* state)
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
        // Each is a call sign, or on a GMRS net a call sign and name (see
        // CheckInKey), with that check-in's call sign and name alongside.
        std::vector<std::string> keys;
        std::vector<std::string> callsigns;
        std::vector<std::string> names;
        std::vector<int> counts;
        std::vector<std::string> last_seen;
        // Each key's place in those, and the sessions' check-ins in one query.
        std::unordered_map<std::string, std::size_t> index_of;
        std::unordered_map<std::int64_t, std::size_t> session_of;
        std::vector<std::int64_t> session_ids;
        for (std::size_t i = 0; i < sessions.size(); ++i)
        {
            session_of[sessions[i].id] = i;
            session_ids.push_back(sessions[i].id);
        }
        std::vector<std::vector<const CheckIn*>> by_session(sessions.size());
        std::vector<CheckIn> session_check_ins = state->db->GetCheckInsForNetInstances(session_ids);
        for (const CheckIn& check_in : session_check_ins)
        {
            by_session[session_of[check_in.net_instance_id]].push_back(&check_in);
        }
        for (std::size_t s = 0; s < sessions.size(); ++s)
        {
            std::unordered_set<std::string> in_session;
            for (const CheckIn* check_in : by_session[s])
            {
                std::string key = CheckInKey(check_in->callsign, check_in->name);
                if (!in_session.insert(key).second)
                {
                    continue;  // Counted once per session.
                }
                std::unordered_map<std::string, std::size_t>::iterator found = index_of.find(key);
                if (found == index_of.end())
                {
                    index_of.emplace(key, keys.size());
                    keys.push_back(key);
                    callsigns.push_back(check_in->callsign);
                    names.push_back(check_in->name);
                    counts.push_back(1);
                    last_seen.push_back(sessions[s].instance_date);  // Newest first.
                }
                else
                {
                    ++counts[found->second];
                }
            }
        }

        // Regulars: at least half the sessions looked at, not yet here.
        std::vector<std::size_t> regulars;
        int total = static_cast<int>(sessions.size());
        std::unordered_set<std::string> here;
        for (const CheckIn& check_in : state->active_check_ins)
        {
            here.insert(CheckInKey(check_in.callsign, check_in.name));
        }
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            if (here.count(keys[i]) == 0 && counts[i] * 2 >= total)
            {
                regulars.push_back(i);
            }
        }
        // Most regular first.
        for (std::size_t i = 1; i < regulars.size(); ++i)
        {
            std::size_t j = i;
            while (j > 0 &&
                   (counts[regulars[j]] > counts[regulars[j - 1]] ||
                    (counts[regulars[j]] == counts[regulars[j - 1]] && keys[regulars[j]] < keys[regulars[j - 1]])))
            {
                std::swap(regulars[j], regulars[j - 1]);
                --j;
            }
        }

        std::vector<std::vector<std::string>> rows;
        state->info_stations.clear();
        // Their details in one query.
        std::vector<std::string> regular_callsigns;
        regular_callsigns.reserve(regulars.size());
        rows.reserve(regulars.size());
        state->info_stations.reserve(regulars.size());
        for (std::size_t index : regulars)
        {
            regular_callsigns.push_back(ToUpperAscii(callsigns[index]));
        }
        std::vector<Station> on_file = state->db->FindStationsByCallsigns(regular_callsigns);
        for (std::size_t index : regulars)
        {
            std::string upper = ToUpperAscii(callsigns[index]);
            std::vector<Station>::const_iterator found =
                std::lower_bound(on_file.begin(), on_file.end(), upper, StationCallsignBefore);
            Station station = found != on_file.end() && found->callsign == upper ? *found : Station();
            station.callsign = callsigns[index];
            if (!names[index].empty())
            {
                station.name = names[index];
            }
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
            state->db->GetSavedNetStationRemarks(state->active_instance.net_id, state->modal_station.callsign,
                                                 SavedEntryName(state->active_net_service, state->modal_station.name));
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
        bool gmrs = IsValidGmrsCallsign(callsign);
        std::optional<Station> licensed =
            FindLicensee(state->db, callsign, gmrs ? NetService::kGmrs : NetService::kAmateur);
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
        summary.push_back("License Class:  " + (licensed.has_value() && gmrs ? std::string("GMRS")
                                                : licensed.has_value() && !licensed->license_class.empty()
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
        // First-timers: in no other session of this net. Each read in one
        // query, not one per check-in.
        std::unordered_set<std::string> seen_before;
        for (std::string& callsign :
             state->db->GetCallsignsInOtherSessions(state->active_instance.net_id, state->active_instance.id))
        {
            seen_before.insert(ToUpperAscii(callsign));
        }
        std::vector<const CheckIn*> first_timers;
        std::vector<std::string> first_timer_callsigns;
        first_timers.reserve(state->active_check_ins.size());
        first_timer_callsigns.reserve(state->active_check_ins.size());
        for (const CheckIn& check_in : state->active_check_ins)
        {
            if (seen_before.count(ToUpperAscii(check_in.callsign)) == 0)
            {
                first_timers.push_back(&check_in);
                first_timer_callsigns.push_back(check_in.callsign);
            }
        }
        std::unordered_map<std::string, std::string> names = StationNames(state->db, first_timer_callsigns);
        std::vector<std::vector<std::string>> rows;
        rows.reserve(first_timers.size());
        for (const CheckIn* check_in : first_timers)
        {
            rows.push_back(
                {std::to_string(check_in->sequence_number), check_in->callsign, NameOf(names, check_in->callsign)});
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
            std::unordered_map<std::int64_t, std::int64_t> counts =
                state->db->GetCheckInCounts(state->active_instance.net_id);
            for (const NetInstance& session : sessions)
            {
                std::unordered_map<std::int64_t, std::int64_t>::const_iterator found = counts.find(session.id);
                std::int64_t count = found == counts.end() ? 0 : found->second;
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
        std::unordered_map<std::int64_t, std::int64_t> counts = state->db->GetCheckInCounts(net.id);
        for (const NetInstance& session : sessions)
        {
            std::unordered_map<std::int64_t, std::int64_t>::const_iterator found = counts.find(session.id);
            std::int64_t count = found == counts.end() ? 0 : found->second;
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
        std::vector<CallsignTally> top = state->db->GetTopCallsignsForNet(net.id, 15);
        std::vector<std::string> top_callsigns;
        top_callsigns.reserve(top.size());
        rows.reserve(top.size());
        for (const CallsignTally& tally : top)
        {
            top_callsigns.push_back(tally.callsign);
        }
        std::unordered_map<std::string, std::string> names = StationNames(state->db, top_callsigns);
        for (const CallsignTally& tally : top)
        {
            rows.push_back(
                {tally.callsign, NameOf(names, tally.callsign), std::to_string(tally.count), tally.last_date});
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
        // Their names in one query, not one per row, on every keystroke.
        std::vector<std::string> callsigns;
        std::unordered_set<std::string> listed;
        for (const StationCheckInRecord& record : records)
        {
            if (listed.insert(record.check_in.callsign).second)
            {
                callsigns.push_back(record.check_in.callsign);
            }
        }
        std::unordered_map<std::string, std::string> names = StationNames(state->db, callsigns);
        for (const StationCheckInRecord& record : records)
        {
            state->info_cells.push_back({record.instance.instance_date, record.net_name, record.check_in.callsign,
                                         NameOf(names, record.check_in.callsign),
                                         RoleAbbreviation(record.check_in.designated_role), record.check_in.remarks});
            // Ad hoc nets aren't on the Recurring Nets list, so say which
            // these are (see the Ad Hoc page for them).
            state->info_cell_tags.emplace_back(record.net_is_ad_hoc ? " (ad hoc)" : "");
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
        std::string cutoff = FormatLocalDate(static_cast<std::int64_t>(std::time(nullptr)) - 182LL * 24 * 3600);
        std::vector<CallsignTally> tallies = state->db->GetSavedStationActivity(state->edit_net_id);
        std::vector<std::vector<std::string>> rows;
        for (const CallsignTally& tally : tallies)
        {
            if (!tally.last_date.empty() && tally.last_date >= cutoff)
            {
                continue;
            }
            rows.push_back({tally.callsign, tally.name, tally.last_date.empty() ? "never" : tally.last_date,
                            std::to_string(tally.count)});
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
                        {"F4", "Settings: your call signs, home ZIP and time format.", false},
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
                {
                    // Statistics are a recurring net's alone.
                    std::vector<HelpLine> lines = {
                        {"Up/Down", "Choose a session; its check-ins show below.", false},
                        {"F7", "Export the highlighted session: log, .qlsession, ADIF (.adi).", false},
                    };
                    if (!state->history_ad_hoc)
                    {
                        lines.push_back({"F8", "Statistics for this net.", true});
                    }
                    lines.push_back({"F9", "Find a station's check-ins to every net.", true});
                    lines.push_back({"F12", "Read the highlighted session's notes.", true});
                    lines.push_back({"Esc", "Back.", false});
                    return lines;
                }
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
                    {"F4", "Settings: your call signs, home ZIP and time format.", false},
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
                    {"Left/Right", "Change the Service, Mode, Channel or Partial Matching.", false},
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
                    {"F4",
                     CanPushUpstream(state) ? "Close this session (F3 there also pushes it upstream)."
                                            : "Close this session; it moves to History.",
                     false},
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
                };
                if (state->is_console_session)
                {
                    lines.push_back({"F3", "Refresh the station data now.", false});
                }
                if (CanManageUsers(state))
                {
                    lines.push_back({"F4", "Manage SSH users.", false});
                }
                if (CanEditOwnKeys(state))
                {
                    lines.push_back({"F4",
                                     "My Keys: your SSH keys, their comments and Transfer method; add or remove keys "
                                     "if the admin allows.",
                                     false});
                }
                if (state->is_console_session)
                {
                    lines.push_back({"F5", "Upstream Server: where closed sessions are pushed.", false});
                }
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
                    {"Left/Right", "Change the Service, Mode, Channel or Partial Matching.", false},
                };
            case kPageNetHistory:
            {
                std::vector<HelpLine> lines = {
                    {"Up/Down", "Choose a session; its check-ins show below.", false},
                    {"F5", "Delete a check-in from that session (by #).", false},
                    {"F6", "Import a session exported elsewhere (.qlsession).", false},
                    {"F7", "Export the highlighted session: log, .qlsession, ADIF (.adi).", false},
                    {"F4", "Delete a closed session (by number).", true},
                };
                // Statistics are a recurring net's alone.
                if (!state->history_ad_hoc)
                {
                    lines.push_back({"F8", "Statistics for this net.", true});
                }
                lines.push_back({"F9", "Find a station's check-ins to every net.", true});
                lines.push_back({"F12", "The highlighted session's notes, to read or edit.", true});
                lines.push_back({"Esc", "Back.", false});
                return lines;
            }
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
                    {"Left/Right", "Change the Mode, Channel or Partial Matching.", false},
                };
            case kPageImportNet:
            {
                std::vector<HelpLine> lines;
                lines.push_back({"F2/Enter", "Import the highlighted file.", false});
                if (CanReceiveZmodem(state))
                {
                    lines.push_back({"F3", "Receive a file from your terminal (ZMODEM).", false});
                }
                if (CanPullUpstream(state) && !(state->import_session && state->import_session_ad_hoc))
                {
                    lines.push_back({"F4",
                                     state->import_session ? "Pull a net's sessions from the upstream server."
                                                           : "Pull a net from the upstream server.",
                                     false});
                }
                lines.push_back({"Esc", state->import_session ? "Back to History." : "Back.", false});
                return lines;
            }
            case kPageManageUsers:
                if (state->show_user_keys_modal)
                {
                    return {
                        {"F2", "Save the username and access; close the window.", false},
                        {"F3", "Remove one of this user's keys (by number).", false},
                        {"F4/Enter", "Add the key pasted in, for this user.", false},
                        {"F5", "Turn the highlighted key off or on; an off key can't log in.", false},
                        {"F6", "Turn all this user's keys off, or all back on.", false},
                        {"Esc", "Close without saving the username or access.", false},
                        {"Left/Right", "Change the access.", false},
                    };
                }
                return {
                    {"F2", "Add the SSH user entered (or another key for them).", false},
                    {"F3", "Remove an SSH user and all their keys (by number).", false},
                    {"F4/Enter", "Edit a user: username, access and keys (by number).", false},
                    {"F5", "Own Keys: let users add and remove their own keys in My Keys, or not.", false},
                    {"F6", "Key Log: who added, removed or turned off keys, and when; F7 there saves it as a .csv.",
                     false},
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
