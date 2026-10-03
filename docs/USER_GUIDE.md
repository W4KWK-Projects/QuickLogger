<img src="icon.png" alt="QuickLogger icon" width="96" align="right">

# QuickLogger User Guide

How to use QuickLogger once it's running. For downloading, installing, building and setting up the built-in SSH server, see the [README](../README.md).

1. [The screen and the keys](#the-screen-and-the-keys)
2. [Settings](#settings)
3. [Recurring nets](#recurring-nets)
4. [Running a net](#running-a-net)
5. [Sharing a session](#sharing-a-session)
6. [Ad hoc nets](#ad-hoc-nets)
7. [GMRS nets](#gmrs-nets)
8. [History](#history)
9. [Pushing to and pulling from an upstream server](#pushing-to-and-pulling-from-an-upstream-server)
10. [Editing and deleting by number](#editing-and-deleting-by-number)
11. [Callsign autocomplete](#callsign-autocomplete)
12. [Saved stations](#saved-stations)
13. [Help and seldom-used keys](#help-and-seldom-used-keys)
14. [Exporting and importing](#exporting-and-importing)
15. [Wider terminals](#wider-terminals)
16. [Station data](#station-data)
17. [Tips and troubleshooting](#tips-and-troubleshooting)

## The screen and the keys

Every page has a **top bar** (the page's name, **F1 Help** and the clock; while you're on a net, its check-in count too) and a **key bar** at the bottom listing what each key does there. QuickLogger is run from the keyboard, but if your terminal passes mouse clicks through (most do, including over SSH), you can also click: a key in the key bar, or **F1 Help** in the top bar, presses that key; clicking a row in a list highlights it, and double-clicking it is the same as pressing Enter on it. Only clicks are sent, not mouse movement, so moving the mouse costs no network traffic and never moves the highlight. While QuickLogger is using the mouse, selecting text to copy needs a modifier key held while dragging (Option in iTerm2, Fn in Terminal.app).

- **F2** is the page's main action (New, Save, Check In…), **Esc** goes back or cancels, and the other F-keys do what the key bar says.
- **Tab**, **Up** and **Down** move between fields; **Enter** picks the highlighted item in a list.
- **F1** on any page explains every key the page has.
- A **red** line under a form says why something wasn't done; a **green** one confirms what was.

On first run you're taken straight to Settings, since a call sign (amateur, GMRS or both) and home postal code are required. After that you start on the **Recurring Nets** list. **F10** there quits.

## Settings

**F4** on Recurring Nets.

| Field | What it's for |
|---|---|
| Amateur Call, GMRS Call | Your amateur and GMRS call signs. At least one is required. Each is filled in when you start a net of its kind; without one, you can only watch that kind of net. An SSH user's are set in Manage Users, and Settings only shows them. |
| Postal Code | Your home 5-digit ZIP or Canadian postal code (like K1A 0B1). Nearby-station autocomplete measures distance from it when a net has no postal code of its own. Canadian distances are by the first three characters, so they're rough outside cities. |
| Nearby Radius | How far, in miles, a licensed station can be from the net's postal code (or your home one) and still be suggested by autocomplete. 1 to 250; the default is 70, which is also used if you leave it blank. Raise it for a net that covers a wide area, or lower it in a crowded city to keep suggestions local. |
| Time Format | 12-hour (3:42 PM, the default) or 24-hour (15:42), for every time shown or exported. **Left/Right** change it. |
| Update Check | At the local console only: **On** (the default) or **Off**. While on, QuickLogger asks GitHub for its latest release shortly after it starts and every 6 hours after that, and when there's a newer one the top bar says so (*v1.8.0 available*) and Settings shows where to get it. Clicking that notice opens the download page in your web browser (on a computer with a desktop; elsewhere it shows the address). It only looks; it never downloads or installs anything. SSH users never see it, since they can't update the server; a server set up with `deploy/freebsd` or `deploy/linux` updates itself. |

**F2** saves. Each SSH user has their own settings. Times are shown in the time zone of the computer QuickLogger runs on. At the local console only, **F3** refreshes the station data now, **F4** opens Manage Users (see the README's SSH section; Windows has no SSH server, so no Manage Users either) and **F5** sets the upstream server sessions are pushed to and pulled from (see [Pushing to and pulling from an upstream server](#pushing-to-and-pulling-from-an-upstream-server)).

## Recurring nets

A recurring net is one you run again and again: a weekly Skywarn net, a club's Tuesday net. Its sessions and check-ins build up its history, and the stations that check in are remembered for autocomplete.

- **Create:** **F2** on Recurring Nets. Only the name is required; Frequency, Postal Code (a 5-digit ZIP or a Canadian postal code) and Recurrence ("Tuesdays 8pm ET") are optional. A net's postal code centers nearby-station autocomplete on where the net meets.
- **Service** is Amateur Radio or GMRS, chosen with **Left/Right** when the net is created. It can't be changed afterwards. Everything below about Mode, Frequency and Offset is for Amateur Radio nets; see [GMRS nets](#gmrs-nets) for the rest.
- **Mode** is one of FM, SSB, AM, CW, D-STAR, DMR or Fusion, chosen with **Left/Right** (FM to start with). The same list is on the Ad Hoc Net page. It's what a session's ADIF export gives as its mode.
- **Names are unique:** no two recurring nets can have the same name, counting "Skywarn", "SKYWARN" and " Skywarn " as one name, whether you create a net, rename one or import one. Ad hoc nets can reuse names freely.
- **Partial Matching** (US or Canada) decides which licensed-station data autocomplete matches anywhere in a callsign, rather than only at its start; see [Callsign autocomplete](#callsign-autocomplete).
- **Frequency** is in MHz (146.940, 7.235) and must be in a US or Canadian amateur band. Only digits and a decimal point can be typed there.
- **Offset** is a repeater's offset in MHz with its sign, the way radios, CHIRP and RepeaterBook show it: **-0.6** or **+0.6** on 2 m, **+5** or **-5** on 70 cm, **-1.6** on 1.25 m. A bare "+" or "-" isn't accepted (the standard offset isn't the same everywhere on 6 m and 70 cm and up), nor is kHz: typing -600 tells you to type -0.6. With a frequency, the frequency plus the offset has to be in an amateur band too.
- **PL Tone** is a standard CTCSS tone, 67.0 to 254.1 (e.g. 100.0, 88.5). Typing 100 saves as 100.0. A DCS code or anything else goes in Comments.
- Frequency, Offset and PL Tone can each be left blank, and the same rules apply to an ad hoc net. When the terminal is wide enough (about 90 to 100 columns, depending on your role and callsign), the line at the top of a session's page shows them too, e.g. *146.940 MHz  -0.6  PL 100.0*; at 80 columns there isn't room.
- **Edit:** **F7**, then the net's number. **F2** saves and returns to the list. **Comments**, on the New Recurring Net and Edit Net pages, are for anything else about the net, such as a backup frequency or a DCS code. They're shown only on those two pages.
- **Modes from older versions:** before 1.8.0, Mode was free text. When you upgrade, or import a net exported by an older version, a mode written another way becomes the one it means ("fm" is FM, "D-Star" is D-STAR, "YSF" and "C4FM" are Fusion, "USB" and "LSB" are SSB), and anything else ("Digital", "FM & DMR") is cleared. Edit Net then shows FM with a note saying the old mode wasn't one of these, until you pick one and save.
- **Nets from older versions:** a frequency that isn't a plain amateur frequency (for example "146.940 -600 PL 100") is moved into the net's Comments as "Frequency: ..." when you upgrade, or when you import such a net, and any frequency with a decimal point found in it stays in Frequency.
- **Delete:** **F8** on its Edit Net page. It asks first: this deletes every session and check-in of the net.
- **Telling nets apart:** the list's **Type** column shows Amateur or GMRS. It also shows when each net was created, or when it was imported, and *session open* while one of its sessions hasn't been closed. The list keeps this current as others open and close sessions.

## Running a net

1. Highlight the net and press **F3** (or Enter): **Log Net** on the key bar. When you log in, the net you last started is already highlighted, so it's usually just **F3**.
2. Choose your **role**: Net Control, Alternate Net Control or Logger. (**Viewer** is for watching a session that's already open; see [Sharing a session](#sharing-a-session).)

If the net already has a session open, the list says *session open*, a line under the list says so when it's highlighted, and the key bar reads **F3/Enter Join** instead of Log Net. Press it anyway: you're asked what to do (see [Sharing a session](#sharing-a-session)).
3. Confirm your callsign (it's filled in from Settings: your GMRS one on a GMRS net) and press **F2**. You're logged as check-in #1, with your role.

**F2** opens the **New Check-In** window. Type the callsign (see [autocomplete](#callsign-autocomplete)) and fill in whatever else you have:

| Field | Notes |
|---|---|
| Callsign | Required; US or Canadian only. On a GMRS net, a GMRS call sign. |
| Name, Member ID, Street Addr, City, County, State, Postal Code, Grid Square | The station's details, kept for next time. County fills in from the ZIP, and so does a blank Grid Square (US ZIPs only): the 4-character grid of the ZIP's center. That is approximate, and near a grid edge it can be the neighboring square. Type your own to override it. Each time QuickLogger starts, it fills in any stored station's blank Grid Square the same way; a grid that's already there is never changed. |
| Signal Report, Remarks, Comment | This check-in only. Remarks start out as the station's default remarks for this net, and whatever you log becomes its new default. |
| Additional Role | Gives this station one of the session's other roles (the ones you don't hold yourself). Only one station holds each role; giving it to another moves it. |

In the window, **F2** logs and clears the form for the next station, **F3** logs and closes, and **Esc** closes without logging. **F4**, **F5** and **F6** jump straight to Remarks, Comment and the role choice, skipping the station's details, with the cursor at the end of any remark or comment already there (they work in Edit Check-In too).

The window is as wide as your terminal allows (less a margin), so long remarks and comments fit, and from 90 columns its fields are in two columns.

**Details for any licensed station:** when no matches are showing and you log a callsign, or leave the Callsign field with **Tab** or F4/F5/F6, its details are filled in from what's known about it: a station known to one of your nets, or else the FCC data at any distance (Canada's ISED data for a Canadian callsign). While matches are showing, the one marked **>** is taken instead (see [Callsign autocomplete](#callsign-autocomplete)). Only blank fields are filled; anything you typed is kept. A mobile or portable callsign (`W4KWK/M`) gets the details of the station itself. A station can check in only once per session, counting a mobile or portable callsign (`W4KWK/M`, `VE3/W4KWK`) as the same station; logging it again says which # it already is.

On the check-in list, **F3** edits and **F5** deletes a check-in by its number (see [Editing and deleting by number](#editing-and-deleting-by-number)); **Enter** edits the highlighted one. A check-in's callsign can't be changed; delete it and log it again instead. While the session is open, deleting leaves a gap in the numbers, so a station's number never changes under someone else logging the same session; when the session is closed, its check-ins are renumbered 1, 2, 3… in order. Deleting a check-in from a closed session (in History) renumbers the rest straight away. **F7** exports the session (see [Exporting and importing](#exporting-and-importing)).

**Session notes (F12)** are for what the session was about, at more length than a check-in's Remarks or Comment has room for: "Severe weather net stood up when a tornado touched down in Chattanooga. Took many check-ins from stations reporting damage to infrastructure." **F12** opens them, on a recurring or ad hoc net. Type as much as you like; lines wrap as you type, and **Enter** starts a new paragraph. **F2** saves them; **Esc** leaves them as they were, even if that's nothing. They stay editable after the session closes, with **F12** in History. They aren't in the text log, but a `.qlsession` or `.qlnet` export carries them. A Viewer or view-only user can read them but not change them. F12 appears on the key bar only when the terminal is wide enough, but always works.

**F4** closes the session when the net is over. It asks first, since a closed session can't be reopened for logging; it moves to [History](#history).

## Sharing a session

Several operators can log the same session at once (over SSH), for example a Net Control and a Logger.

- **Is a session open?** On Recurring Nets, a net being logged right now shows *session open*, and when it's highlighted the key bar reads **F3/Enter Join**.
- **Joining or watching:** highlight the net and press **F3** (or Enter), the same key that logs a net. Because a session is open, QuickLogger asks what you want to do:
  - **F2/Enter Resume:** log check-ins in the same session, alongside the others.
  - **F4 View:** only watch it. You can't change anything.
  - **F3 Close & New:** close that session and start a new one. Don't use this to join someone else's net.
- **Watching:** a **Viewer** (F4 above, or Viewer on the role page) sees the session as it's logged, and can export it, read its notes (F12) and use the look-up keys (F6, F8, F9, F10), but can't log, edit, delete or close anything. **Esc** leaves, and the session carries on.
- **Staying in step:** check-ins anyone logs appear on everyone's screen within a few seconds.
- **When someone closes it:** within a few seconds, everyone else on it, Viewers included, sees a **Net Closed** window saying when it was closed. It also names any callsign that was being typed but wasn't logged. **Enter** returns to the net list. The operator who closed it doesn't see this window.
- **Dropped connections:** a session left open by a dropped connection or closed terminal is resumed the same way.
- **View-only users:** an SSH user set up as view-only (see the README's SSH section) is always a Viewer. **F3/Enter** on a net goes straight to watching its open session, and says so if none is open. They can look at and export History, view open ad hoc sessions, and change their own Settings, but can't create, edit, import, start, log, close or delete anything. Their key bars and Help list only the keys they can use, and the net list shows "(view-only)" beside their callsign.

## Ad hoc nets

**F5** on Recurring Nets starts a one-off net: fill in its name (and anything else) and press **F2**. It runs exactly like a recurring net but never appears in the Recurring Nets list. Everything else about ad hoc nets is on this page:

- **F3** resumes an ad hoc session left open (by number), such as after a dropped connection.
- **F6** shows the history of every ad hoc net, one session per row, with the net's name.

## GMRS nets

Every net, recurring or ad hoc, is either Amateur Radio or GMRS: **Service** on New Recurring Net and Ad Hoc Net, chosen with **Left/Right**. A GMRS net works like any other, with these differences:

- **Mode** is always FM.
- **Channel** takes the place of Frequency and Offset: **Left/Right** step through channels 1 to 22 and the repeater channels 15R to 22R. A repeater channel's input, 5 MHz up, comes with it. PL Tone works as on an amateur net. The net list's Frequency column shows the channel (*Ch 20R*), and so does the line at the top of a session's page on a wide enough terminal (*GMRS 20R  462.6750 MHz  PL 141.3*).
- **Call signs** are GMRS ones: seven characters starting with K or W, three letters and four digits or four letters and three digits (WRAA123). Yours is the GMRS Call in Settings.
- **One license covers a family**, so a call sign can check in more than once a session, once per name. Logging the same call sign and name again (in any capitals) says which # it already is. Autocomplete lists each name a call sign has checked in under, and then a *(new name)* match with no name, for someone else on the same license. Saved stations work the same way: one call sign can be saved once per name.
- A check-in's **Name** is that person's. Editing it on Edit Check-In changes the check-in, and the net's saved entry for that name. The station's own details, such as the licensee's name and address, stay as they are.
- **Autocomplete** suggests GMRS licensees from the FCC data, and only GMRS call signs from your other nets. Canadian call signs aren't suggested; Canada has no GMRS.
- **Exports** have no ADIF file: ADIF is for amateur contacts.
- **Sessions stay on their service.** A GMRS session can't be imported into an amateur net, or an amateur one into a GMRS net, whether from History, by merging a `.qlnet` or by pushing.

## History

**F6** on Recurring Nets shows the highlighted net's past sessions: when each started and ended, its roles and whether it's still open, and on a wide enough terminal its number of check-ins, who started it (**Started by**, which can differ from Net Control) and whether it has session notes (**Notes**: *yes*; **F12** opens them). The bottom list shows the check-ins of the highlighted session.

- **Up/Down** choose a session.
- **F7** exports the highlighted session: its log, a `.qlsession` file for importing elsewhere and, for an amateur net, an ADIF file (see [Exporting and importing](#exporting-and-importing)).
- **F12** opens the highlighted session's notes, to read or edit (see [Running a net](#running-a-net)).
- **F6** imports a session logged somewhere else (see below).
- **F5** deletes one check-in from that session (by its #), as on the session's own page. If it held a role, the role is cleared too.
- **F4** deletes a whole closed session (by number). An open session has to be resumed and closed first. At 80 columns it isn't on the key bar, but it always works.

**Importing a session** adds one session to this net's history, without touching the rest of it. It's for a session logged on another QuickLogger: during an outage, say, a net logged on a laptop instead of the server. On the computer where it was logged, **F7** on that session (on the net's page or in History) saves its `.qlsession` file. On this one, put the file in `imports/` (or receive it over SSH with **F3**), open this net's History, press **F6**, highlight the file and press **F2**. The session and its check-ins are added as they were logged: times, roles, remarks and comments, and each station's details, and its stations are saved to this net. It goes into the net whose History you're on, even if the net had another name where it was logged (the message after importing says so). If that name has nothing in common with this net's ("Hamilton County ARES" going into TAG Skywarn, say), QuickLogger asks first, in case you're on the wrong net's History: **F2/Enter** imports it anyway, **Esc** cancels. Small differences, such as "Co." for "County", a typo, a missing word or initials ("TAG" for Tennessee Alabama Georgia), don't ask. A session this net already has, with the same date and start time, is refused. On ad hoc History, an imported session becomes a new ad hoc net with its own name. With an upstream server set, **F4** pulls all of a net's sessions instead (see [Pushing to and pulling from an upstream server](#pushing-to-and-pulling-from-an-upstream-server)).

## Pushing to and pulling from an upstream server

A QuickLogger that logs on its own, such as a laptop at a field station, can send each closed session to a central QuickLogger, its upstream. The upstream adds it to its net of the same name. This works only at the computer QuickLogger runs on, not over SSH.

**Setting up:** the upstream's operator adds you as an SSH user (Manage Users) with your public key. At this computer, log in to the upstream once with plain `ssh`, such as `ssh -p 2222 you@upstream.example.org`, so `ssh` learns its host key; **F10** quits. Then press **F5** on Settings (Upstream Server) and enter the upstream's host name, your username there and its port, and press **F2**. The port is 22 unless the upstream's operator says otherwise; QuickLogger's own SSH server uses 2222. A blank host turns pushing and pulling off.

QuickLogger keeps no keys of its own: it runs this computer's `ssh` and `scp`, with your own keys and `~/.ssh/config`. It can't ask for a passphrase, so a key that has one must be in ssh-agent (`ssh-add`). `ssh` and `scp` come with macOS, Linux and FreeBSD, and with Windows 10 and 11 (the OpenSSH Client feature).

**Close & Push:** with an upstream set, closing a net (**F4**) also offers **F3 Close & Push**. The session closes here, then goes to the upstream while you carry on; the status line says when it's done. The session stays closed here whatever becomes of the push.

**From History:** **F7** Export on a closed session also offers **F3 Push to upstream**, such as for one closed with **F2** or one whose push failed. A pushed session's Status reads *pushed*. Pushing one the upstream already has does no harm.

**When the names differ:** if the upstream has no net of the session's name but one that looks like it, QuickLogger asks. **F2/Enter** pushes it to that net, **Esc** doesn't. If nothing there looks like it, nothing is pushed.

**When a push fails:** the status line says why in one sentence, such as that the upstream couldn't be reached, refused your key, or has a host key `ssh` doesn't know. Push it again from History once that's fixed.

**Pushing a net:** like a session, with **F8** Export on Recurring Nets: the export window also offers **F3 Push to upstream**. It sends the net's closed sessions and saved stations. A net the upstream hasn't got is added. If it has a net of that name (or one that looks like it), QuickLogger shows what merging would add and asks: **F2/Enter** merges, **Esc** doesn't. A merge only adds what the upstream lacks and never changes what it has. Where there's nothing else to do with the export (no ZMODEM, no file manager), the window offers just the push.

**Pulling a net:** with an upstream set, **F4 Pull** on the Import page (**F9** on Recurring Nets) lists the upstream's nets with their number of closed sessions. Highlight one and press **F2/Enter**. QuickLogger fetches the net and imports it as a file you received: a net you don't have is added as *imported*, and one that looks like a net you have opens the **Import or Merge?** window. **Esc** cancels.

**Pulling sessions:** **F4 Pull** on a net's Import Session page (**F6** in History) lists the upstream's nets too, with the one named like this net highlighted. **F2/Enter** adds all of that net's closed sessions to this net's History. Sessions it already has, with the same date and start time, are skipped, so pulling again adds only what's new, and the message says how many of each. A session still open on the upstream isn't pulled, nor are any beyond the newest 300 (pull the whole net for those). Ad hoc History has no Pull.

Anyone with an SSH user on the upstream can pull, view-only users too; it changes nothing there. Pulling needs a user here who isn't view-only, since it imports.

## Editing and deleting by number

Edit and delete keys ask which row you mean: every row gets a number, you type it and press **Enter**. An edit opens the row straight away; a delete shows what it's about to remove and asks you to confirm with **F2/Enter** (**Esc** backs out). While choosing, **Up/Down** move the highlight (Enter with no number picks it), **Backspace** erases a digit and **Esc** cancels. Check-ins use the number in their **#** column.

| Page | Key | Picks a row to… |
|---|---|---|
| Recurring Nets | F7 | Edit a net |
| Ad Hoc Net | F3 | Resume an open ad hoc session |
| Active net | F3 / F5 | Edit / delete a check-in |
| Active net | F6 / F9 | See a station's history / card |
| Edit Net | F3 / F4 | Edit / remove a saved station |
| History | F5 / F4 | Delete a check-in / a closed session |
| Manage Users | F3 | Remove an SSH user and all their keys |
| Manage Users | F4 | Edit a user: username, access and keys (then F3 removes a key) |

**A station's details are kept while anything uses them:** a net it's saved to, or a check-in in any log. When the last of those goes, its details go too, and it stops coming up in autocomplete; that's how a mistyped callsign gets cleaned up. The delete confirmation says which will happen.

## Callsign autocomplete

In the New Check-In and Saved Station windows, matches appear as you type any part of a callsign, in either case (`kwk` finds W4KWK). As many are shown as the screen has room for (11 on a 24-line terminal, more on a taller one, never fewer than 8), in this order:

1. Stations known to **this net** (checked in before, or saved to it), marked *(this net)*.
2. Stations known to **other nets**, marked *(other net)*.
3. **Licensed stations nearby**, from the FCC data: within your Nearby Radius (70 miles unless you change it in Settings) of the net's postal code (or your home one), nearest first, marked *(ULS, ~N mi)*. Canadian stations near it come from ISED's data and are marked *(ISED ~N mi)*. Stations whose ZIP has no location on file (usually a PO Box) follow, marked *(ULS, unknown)*.
4. **Canadian callsigns**, from Canada's ISED database, in order, marked *(ISED)*. Those within your Nearby Radius come first with their distance, found from the first three characters of their postal code. The rest follow without one, so they aren't limited by your Nearby Radius.

A GMRS net's nearby licensed stations are GMRS licensees, and it has no Canadian ones (see [GMRS nets](#gmrs-nets)).~~~~

**Partial Matching** is set for each net, on New Recurring Net, Ad Hoc Net and Edit Net: **US** (the FCC data) or **Canada** (ISED's data). **Left/Right** change it. The licensed-station list it names (3 or 4 above) matches wherever what you've typed appears in a callsign; the other matches only callsigns starting with what you've typed.

- **US** (the default for new nets, and for every net from before 1.7.0): `EV` finds a nearby KQ4EVW. Canadian callsigns come up once what you've typed starts with **V** (every Canadian amateur callsign starts VA, VE, VO or VY; no US one starts with V).
- **Canada**, for a net whose stations are mostly Canadian: `3EV` finds VE3EVA. Nearby US callsigns come up only when they start with what you've typed.
- Partial Matching doesn't change the first two lists, which always match anywhere in the callsign.

**Up/Down** move the **>** marker and **Enter** picks that match, filling in the station's details. The match marked **>** is the one you get however you leave the Callsign field: **Enter**, **Tab**, F4/F5/F6, or logging straight away with **F2**/**F3**. If you type a whole callsign that's among the matches, the marker moves to it. If it's a licensed station that isn't (for example, one farther away than your Nearby Radius), it's added at the bottom of the list with its distance, for you to move down to; the marker stays on the top match. You stay in the Callsign field, so you can keep typing to narrow the list. While it's showing, the list takes the place of the window's other fields; they come back when you pick a match, clear the callsign or **Tab** away. If nothing matches, type the whole callsign: **Enter** then looks it up exactly, in the FCC data at any distance, or ISED's.

**Callsign rules:** only callsigns the US or Canada could issue are accepted, with or without a portable indicator (`W4KWK/M`, `/P`, `/QRP`, `/4`, `VE3/W4KWK`). Anything else, like a typo (`W4KW4`) or a foreign callsign, is refused when you log or save it. A GMRS net takes only GMRS call signs, and an amateur net only amateur ones.

## Saved stations

A net's saved stations are the ones it expects: they come first in its autocomplete, and each can have **default remarks** ("mobile", "EOC") filled in when it checks in. Every station that checks in is saved to the net automatically.

On **Edit Net** (**F7** on Recurring Nets), the net's details sit above its saved stations.

- **F6** opens the Saved Station window for a new station, **F3** (by number) or **Enter** for an existing one. In the window, **F2** saves and clears it for the next station, **F3** saves and closes, **Esc** closes without saving.
- **F4** removes a station from the net (by number).
- **F7** exports the list.

## Help and seldom-used keys

**F1** opens Help on any page. (A Viewer's Help lists only what a Viewer can do.) Some pages also have keys for things you won't need often. They always work, but they appear on the key bar only when there's room; Help lists them, marked with an asterisk. Each opens a window: **Up/Down** scroll it, **Esc** closes it. "By #" means it asks for a check-in's number first.

| Page | Key | What it shows |
|---|---|---|
| Active net | F6 | A station's other check-ins to this net (by #) |
| Active net | F8 | Regulars not yet heard: stations in at least half of the net's last 10 sessions (or of all of them, if fewer) who haven't checked in yet. **Enter** opens New Check-In with the highlighted one filled in. |
| Active net | F9 | Everything known about a station (by #): address, license class, check-in totals, the nets it's saved to |
| Active net | F10 | This session so far: check-ins, first-timers, and the recent average |
| Active net | F12 | The session's notes, to read or edit (see [Running a net](#running-a-net)) |
| History | F8 | The net's statistics: sessions, averages, busiest session, recent months, most frequent stations |
| History | F9 | Find a station: its check-ins to every net |
| History | F12 | The highlighted session's notes, to read or edit |
| Edit Net | F5 | Saved stations that haven't checked in to this net for six months, or ever |

## Exporting and importing

Exports are written to the `exports/` folder next to QuickLogger's database; imports are read from `imports/`.

| What | Key | File |
|---|---|---|
| A session | F7 on the active net or History | `NetName_date_log.txt`, the log; `NetName_date.qlsession`, the session exactly, for **F6** in History on another QuickLogger; and, for an amateur net, `NetName_date.adi`, the session's contacts in ADIF, for a logging program. Over SSH, ZMODEM sends them as one `NetName_date.zip`, removed afterwards |
| A net's saved stations | F7 on Edit Net | `NetName_saved_stations.txt` |
| A whole net, to share | F8 on Recurring Nets | `NetName.qlnet`: the net, its saved stations and its full history |

**Importing a net:** put the `.qlnet` file in `imports/` (or receive it with **F3**, below, or pull it from your upstream server with **F4**), press **F9** on Recurring Nets, highlight the file and press **F2**. It's added as a new net marked *imported*, so it can't overwrite one of yours. If you have a net with the same or a similar name, the **Import or Merge?** window lists them instead: **F2/Enter** imports the file as a new net anyway (not offered when a net has its very name, since no two nets may share one), **F3** merges it into the highlighted net, and **Esc** cancels.

**Merging a net** is for a net that went somewhere else and came back: say you exported it to a laptop, logged on the laptop during an outage, and exported it again to bring back. Before anything changes, a summary says what the merge will do, and **F2** carries it out (**Esc** goes back):

- The file's sessions this net doesn't have are added, with their check-ins and notes. A session that was still open in the file comes in closed, as of its last check-in, and renumbered.
- A session counts as one this net already has when their times overlap (from start to close), or, if either is still open, when they started within 30 minutes of each other. Without start times, it's the same date and the same check-ins.
- A session that's here but differs (different check-ins or notes) is listed. **Left/Right** (or Enter) chooses, for each one, to **Keep** yours (the default) or **Replace** it with the file's. A session still open here is never replaced.
- The file's saved stations that this net doesn't have are added, with their remarks. Stations already here keep their details and remarks; details missing here are filled in from the file without asking.
- A station whose details are filled in both here and in the file, but differently (a member ID of SP-41 here and SP-42 in the file, say), is listed under the sessions, with each differing detail on its own line. Differences only in capitals ("KNOXVILLE" and "Knoxville") don't count. **Left/Right** chooses **Keep** (yours, the default) or **Replace**, which takes the file's value for just the details listed. **Up/Down** moves through both lists. Each net's remarks for a station always stay its own.
- The net's own settings (name, frequency, notes, Partial Matching, postal code) don't change.

It all happens at once, or not at all if something goes wrong. It can't be undone, so look at the summary first. Merging the same file twice adds nothing the second time.

**At your own computer:** after an export, QuickLogger offers to show you the file: **F2/Enter** opens the `exports/` folder in Finder, File Explorer or your Linux/FreeBSD desktop's file manager, with the new files selected (a session export makes three) where the file manager supports it; **Esc** closes the window. Without a desktop (a text-only console), it just says where the file was saved.

**Over SSH (ZMODEM):** after an export, QuickLogger offers to send the file to your terminal. Open your terminal's receive window, then press **Enter**; **Esc** skips it and the file stays in `exports/`. To upload a `.qlnet` or `.qlsession`, press **F3** on the Import page (F9 on Recurring Nets for a net, F6 in History for a session), then send the file from your terminal. A session's export sends one `.zip` holding its log, `.qlsession` and `.adi`. The three files stay in `exports/`; the `.zip` is removed once the transfer is over (or skipped), and **F7** makes a fresh one. This needs a terminal that supports ZMODEM (such as ZOC or SecureCRT) and `lrzsz` installed where QuickLogger runs, so there's no ZMODEM on Alpine Linux, which has no `lrzsz` package, or on Windows, which has no SSH server.

**Over Mosh:** there's no ZMODEM (Mosh can't carry it), so QuickLogger doesn't offer it: copy files with `scp` or `sftp` as below. Connecting with Mosh is in the README's SSH section.

**Over SSH (SFTP):** without ZMODEM, copy files with `scp` or `sftp`, using the address, port and key you log in with. You see two folders: `/exports`, your exports (read-only), and `/imports`, for files to import.

To download an export: `scp -P 2222 you@server:/exports/Skywarn_2026-01-06.qlsession .`

To upload a net for **F9** on Recurring Nets (or a session for **F6** in History): `scp -P 2222 Skywarn.qlnet you@server:/imports/`

Uploads must be `.qlnet` or `.qlsession` files of up to 25 MB, and `/imports` holds up to 100 MB in all. A view-only user can't upload. `sftp`'s `rm` removes your own uploads. Any `scp` works, including the one built into Windows 10 and 11; it copies single files, not folders.

**Pushing sessions to this server:** another QuickLogger, or an app, can send a closed session here over SSH: it uploads the `.qlsession` to its user's `/imports`, then runs `import-session`. The session goes into the net with the same name, as **F6** in History would put it; when only a similar name matches, nothing is imported until the sender confirms that net. A session from an ad hoc net becomes a new ad hoc net. The sender has to be an SSH user who isn't view-only.

**Pulling from this server:** another QuickLogger, or an app, can ask this server for its nets (`list-nets`), have it write one net (`export-net`) or a net's closed sessions (`export-sessions`) into the user's `/exports`, and fetch the files with `scp` or `sftp`. Any SSH user may, view-only ones included, as they may take an export over ZMODEM. The commands and their answers are in [Upstream commands](IMPORT_SESSION.md).

**ADIF (`.adi`):** one record per check-in, except your own #1, in ADIF 3.1 for importing into a logging program (Log4OM, N1MM, LoTW's TQSL and the like). Each has the station's call sign; the date and time it checked in, in UTC; the frequency (the session's, or the net's) and band; the mode (D-STAR, DMR and Fusion as DIGITALVOICE with their submode); your callsign from Settings as the station callsign; the signal report as RST received; and whatever is known of the station's name (first name first: the FCC's "Shults, Roger D" becomes "Roger D Shults"), city, state, county, grid square, remarks (as COMMENT) and comment (as NOTES). Anything blank is left out. ADIF is plain ASCII, so accents are dropped there ("José" becomes "Jose"); the log and `.qlsession` keep them.

**File format:** exported logs and saved-station lists are plain text in a fixed format, the same whatever terminal they came from, so a program can read them by column position. A few header lines come first (the net's name and, for a log, its date, times, roles and status), then a blank line, a column-heading line and one line per check-in or station. Each column starts two spaces after the one before; longer values are cut to fit, and trailing spaces are dropped.

| Log column | Width | | Saved-station column | Width |
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

## Wider terminals

Everything fits an 80×24 terminal. A wider one gets more:

- **Lists** widen their columns and add more: check-ins gain the time, city and state, signal report and comment; the net list shows each net's mode, frequency and schedule; History shows each session's check-in count; saved stations add city, county, grid and default remarks; autocomplete matches add city, state and county. Once every column fits, the gaps between them widen.
- **Forms** (Edit Net and the check-in and Saved Station windows) put their fields in two columns from 90 columns up, without changing the Tab order.
- **Windows** opened with the seldom-used keys widen their tables too.

Resizing the terminal re-lays everything out at once. At 80 columns everything looks as it always has.

## Station data

QuickLogger keeps its own copy of the FCC's amateur and GMRS license databases and Canada's amateur one (from ISED, Innovation, Science and Economic Development Canada), plus Census data for working out counties and GeoNames' postal-code locations for Canada (CC BY 4.0, [geonames.org](https://www.geonames.org)), and keeps it current by itself:

- The first download starts when QuickLogger first runs and takes a minute or two. Until it's done, a yellow **Loading station data NN%** notice shows at the top of every screen and callsign lookups find no one; everything else works.
- After that, the FCC and ISED data are refreshed about weekly (**Updating station data** shows meanwhile; lookups keep working). A failed download is retried hourly.
- The FCC data (amateur and GMRS) comes from a weekly copy on QuickLogger's GitHub page, because fcc.gov turns away downloads from some cloud servers. If that copy can't be reached, QuickLogger downloads it from the FCC instead.
- **Settings** shows when the data was last updated.

**County:** FCC records have no county, so QuickLogger uses the station's ZIP. For a ZIP that crosses a county line, the station's city decides when it names a town inside that ZIP; otherwise the ZIP counts as being in whichever county most of its residents live in.

## Tips and troubleshooting

- **An F-key does nothing, or does something else:** some terminal programs keep certain F-keys for themselves (F1 for their own help, F10 for their menu, F11 for full screen). Turn that off in the terminal's settings, or see its keyboard options.
- **The screen is cut off:** QuickLogger needs at least 80×24. Make the window bigger; it adjusts straight away.
- **Esc takes a moment:** about a tenth of a second, while QuickLogger checks that it isn't the start of another key. Pressing Esc twice quickly works as two Escs.
- **A callsign isn't found:** check whether the station data is still loading (the notice at the top). The FCC and ISED data cover US and Canadian amateur licensees and US GMRS licensees; enter other stations' details by hand.
- **"Net Closed" window:** another operator closed the session you were on. Press Enter, then log the net again (F3) if you need a new session.
- **"Failed to open database 'quicklogger.db': unable to open database file" under WSL:** QuickLogger is in a Windows folder (a path starting `/mnt/c/`, such as your Windows Downloads or OneDrive), where its database can't work. Move the QuickLogger folder into your Linux home directory (for example `mv QuickLogger-<version>-linux-amd64 ~/`) and start it from there.
- **"./QuickLogger: not found" although the file is there, under WSL:** that WSL distribution isn't a regular Linux one (Docker Desktop's own distribution, or Alpine). Install Debian or Ubuntu (`wsl --install -d Debian`), install the libraries the README lists for it, and run QuickLogger there.
