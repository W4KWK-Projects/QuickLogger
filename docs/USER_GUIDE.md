<img src="icon.png" alt="QuickLogger icon" width="96" align="right">

# QuickLogger User Guide

How to use QuickLogger once it's running. For downloading, installing, building and setting up the built-in SSH server, see the [README](../README.md).

- [Getting started](#getting-started)
- [Running a net](#running-a-net)
- [Editing and deleting by number](#editing-and-deleting-by-number)
- [Callsign autocomplete](#callsign-autocomplete)
- [Saving stations to a net](#saving-stations-to-a-net)
- [Help and seldom-used keys](#help-and-seldom-used-keys)
- [Wider terminals](#wider-terminals)
- [Exported files](#exported-files)
- [Station data](#station-data)

## Getting started

On first run, you'll be taken straight to Settings to set your callsign and home ZIP code (required before anything else is usable). After that, you land on the Recurring Nets list. Wherever you are, **F1** (at the top right of the screen) explains the page's keys.

Settings (F4 on the Recurring Nets list) also has **Time Format**: the 12-hour clock (3:42 PM, the default) or the 24-hour clock (15:42), for every time QuickLogger shows or exports. Each SSH user chooses their own. Times are stored in UTC and shown in the time zone of the computer QuickLogger runs on.

## Running a net

On **Recurring Nets**, highlight a net and press **F3** (or Enter), pick your role, confirm your callsign, and the net starts with you logged as check-in #1. **F2** opens the New Check-In window:

| Key | What it does |
|---|---|
| F2 | Log this station and clear the form for the next one |
| F3 | Log this station and close the window |
| Esc | Close the window without logging anything |

**F4** closes the net when you're done. It asks first, since a closed session can't be reopened for logging; it stays in **History** (F6), with the times it started and ended, where you can still view and export it.

**Picking up where you left off.** If a session ends without F4 — an SSH connection drops, a terminal is closed — the session stays open, and the net shows *session open* in the list. Starting that net again offers to **Resume** it (F2/Enter), carrying on the same log, or to **close it and start a new session** (F3). The same applies if someone else is logging that net right now: resuming joins their log. If one of you closes the session, the others can't log anything more to it; the next check-in they try is refused with a message, and they're returned to the net list.

**Ad hoc nets.** **F5 (AdHoc)** starts a one-off net: fill in its name and details and press **F2**. Ad hoc nets never appear in the Recurring Nets list. Everything about them is on the Ad Hoc Net page instead: an ad hoc session left open (say, after a dropped connection) is listed there, and **F3** resumes it; **F6** shows the history of every ad hoc net, one session per row, where you can view, export and delete them just as in a recurring net's History.

**Telling nets apart.** The list shows when each net was created, or, for one brought in with **F9 (Import Net)**, when it was imported — so if you import a net with the same name as one of yours, you can tell which is which and delete the one you don't want. (Nets created before this was recorded show no date.)

## Editing and deleting by number

Wherever a list has an edit or delete key, pressing it numbers every row. Type the row's number and press **Enter**. An edit opens that row straight away. A delete (or remove) first shows what it's about to delete and asks you to confirm with **F2/Enter**, or **Esc** to back out. While you're choosing, **Up/Down** move the highlight instead (Enter with no number typed picks the highlighted row), **Backspace** erases a digit, and **Esc** cancels. Check-ins already show their number in the **#** column, so that's the number to type for them.

| Page | Key | What it does |
|---|---|---|
| Recurring Nets | F7 | Edit a net |
| Ad Hoc Net | F3 | Resume an ad hoc session that's still open |
| Active net (while logging) | F3 / F5 | Edit / delete a check-in |
| Edit Net | F9 | Open a saved station in the Saved Station window |
| Edit Net | F4 | Remove a saved station from this net |
| History | F4 | Delete one check-in from the highlighted session's log |
| History | F5 | Delete a closed net session and its log |
| Manage Users | F3 | Remove an SSH user |

On **History**, the top list is the net's past sessions and the bottom list is the check-ins of whichever session is highlighted. To delete one check-in from a past log, first highlight its session with **Up/Down**. Then press **F4**; the bottom list's **#** column holds the numbers to type. The confirmation names the station and the session's date. If that check-in held the session's Net Control, Alternate NC or Logger role, the role is cleared from the session too.

Deleting a whole recurring net is still **F8** on its Edit Net page, which asks to confirm first.

**What happens to a station's details.** A station's details (name, member ID, address, county, and so on) are kept for as long as something uses them: a net it's saved to, or a check-in in some net's log. When the last of those goes — you remove it from the only net it was saved to and it has never checked in, or you delete its last check-in — its details are deleted too, and it stops coming up in autocomplete. That's also how a mistyped callsign gets cleaned up: remove it and it's gone. The confirmation tells you beforehand which of the two will happen.

## Callsign autocomplete

Wherever you enter a station's callsign — the New Check-In window while logging a net (F2 on the active net), and the Saved Station window on a net's Edit Net page — matches appear below the field as you type. You can type any part of the callsign, in upper or lower case: `kwk` finds W4KWK. Matches are listed in this order:

1. Stations known to **this net** (they've checked in before, or are saved to it), marked *(this net)*.
2. Stations known to **other nets**, marked *(other net)*.
3. **Licensed stations nearby** from the FCC data (within about 70 miles of the net's ZIP code, or of the home ZIP in Settings if the net has none), nearest first, marked *(ULS, ~N mi)*. Stations whose ZIP has no location on file (usually a PO Box ZIP) come after those, marked *(ULS, nearby)*.

Up to 8 matches show at once; the FCC ones fill whatever room the first two groups leave. The match marked **>** is the one Enter picks. Press **Up/Down** to move the marker. You stay in the Callsign field, so you can keep typing to narrow the list. While the list is showing, it takes the place of the window's other fields, so it fits on a small screen; they come back as soon as you pick a match, clear the callsign or Tab to another field. Picking a match fills in the rest of the station's details (name, address, county, and so on).

If nothing matches, just type the whole callsign. Pressing Enter then looks it up exactly, including in the FCC data at any distance.

Every callsign field accepts only callsigns the US or Canada could issue, with or without a portable indicator such as `/M`, `/P`, `/QRP`, `/4` or `VE3/` in front. Anything else (a typo like `W4KW4`, or a callsign from another country) is refused with a message when you try to save or log it.

## Saving stations to a net

On a net's **Edit Net** page (F7 from the Recurring Nets list), the net's details sit above its list of saved stations. **F6** (Add Station) opens the Saved Station window with the cursor in the Callsign field. Type a callsign, pick a match, fill in anything else, and press **F2** (Save & Continue) to save it and clear the window for the next station, or **F3** (Save & Close) to save it and close the window. **Esc** closes it without saving. **F9** (Edit Station), or Enter on a highlighted station, opens an existing one in the same window. **F2** on the page itself saves the net's details and returns to the net list.

## Help and seldom-used keys

**F1** on any page (shown at the top right, next to the clock) opens Help, which explains every key the page has. Pages also have extra keys for things you won't need often. They always work, but they only appear on the key bar when the terminal is wide enough to fit them; Help lists them either way, marked with an asterisk. Each opens a window over the page: **Up/Down** scroll it and **Esc** closes it. Those marked "by #" ask for a check-in's number first, like Edit and Delete do.

| Page | Key | What it shows |
|---|---|---|
| Active net | F6 | A station's other check-ins to this net (by #) |
| Active net | F8 | Regulars not yet heard: stations that checked in to at least half of the net's last 10 sessions (or of all of them, if there have been fewer) but haven't checked in yet; **Enter** opens New Check-In with the highlighted one filled in |
| Active net | F9 | Everything known about a station (by #): address, license class, check-in totals, nets it's saved to |
| Active net | F10 | This session so far: count, first-timers, and the recent average |
| History | F8 | Statistics for the net: sessions, averages, busiest session, recent months, most frequent stations |
| History | F9 | Find a station: its check-ins to every net |
| Edit Net | F5 | Saved stations that haven't checked in to this net for six months, or ever |

## Wider terminals

Everything fits an 80×24 terminal. On a wider one, QuickLogger uses the extra width: lists widen their columns and add more (check-ins gain the time, city and state, signal report and comment; the net list shows each net's mode, frequency and schedule; History shows how many check-ins each session had; saved stations add city, county, grid and default remarks; callsign matches add city, state and county), and forms such as Edit Net and the check-in windows lay their fields out in two columns from 90 columns up. Once every column of a list fits, the gaps between columns widen for readability. Resizing the window re-lays everything out straight away, and at 80 columns everything looks exactly as it always has.

## Exported files

Exported net logs (F7 on the active net or History) and saved-station lists (F7 on Edit Net) are plain text in a fixed format, the same whatever terminal they were exported from, so a program can read them by column position. A few header lines come first (the net's name, and for a log its date, times, roles and status), then a blank line, a column-heading line, and one line per check-in or station. Each column starts at a fixed position, two spaces after the one before; a longer value is cut to fit, and trailing spaces are dropped.

| Net log column | Width | | Saved-station column | Width |
|---|---|---|---|---|
| # | 4 | | Callsign | 13 |
| Time | 8 | | Name | 30 |
| Callsign | 13 | | Member ID | 10 |
| Name | 30 | | City, State | 30 |
| Member ID | 10 | | County | 20 |
| City, State | 30 | | Grid | 8 |
| County | 20 | | Default Remarks | 40 |
| Role | 6 | | | |
| Signal | 6 | | | |
| Remarks | 40 | | | |
| Comment | 60 | | | |

## Station data

QuickLogger looks callsigns up in its own copy of the FCC's amateur license database, and fills in each station's county from Census data. It downloads and refreshes all of this by itself, in the background, for everyone using the instance — nobody needs to (or can) start it by hand:

- The first download starts as soon as QuickLogger launches and takes a minute or two. Until it's done, a yellow **Loading station data NN%** notice shows at the top of every screen, local or over SSH, and callsign lookups won't find anyone yet. The rest of the app works meanwhile.
- After that, the FCC data is refreshed about weekly (a yellow **Updating station data** notice shows while it runs; lookups keep working from the previous copy). A failed download is retried automatically every hour.
- **Settings** shows when the data was last updated. At the local console only, **F3** there refreshes it right away.
- Quitting the console session in the middle of a download is fine: an unfinished first download or weekly refresh starts over the next time QuickLogger runs. (A refresh asked for with F3 just waits for its regular turn.)

**How county is worked out.** FCC records have no county, so QuickLogger uses the station's ZIP code. Most ZIPs lie in one county. For the few thousand that cross a county line, the station's city decides when it names a town inside that ZIP (a Newton address in 02467 is Middlesex, a Boston one Suffolk); otherwise the ZIP counts as being in whichever county most of its residents live in.
