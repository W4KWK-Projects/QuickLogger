# Sizing: disk space and memory

What QuickLogger needs in disk and memory, and what to back up.

The numbers below come from real runs: the actual FCC and Census data (September 2026), plus simulated net history for two example installations, measured after SQLite `VACUUM`. They're for planning only. Real sizes depend mostly on how much text goes into each check-in's remarks and comments.

**Two example installations**

| | A: small club | B: large group |
|---|---|---|
| Users (logins) | 10 | 50 |
| Nets | 5, all weekly | 15: 10 weekly, 5 daily |
| Check-ins per net | 25 | 50 |
| Net sessions per year | 260 | 2,345 |
| Check-ins per year | 6,500 | 117,250 |

**Size of `quicklogger.db`**

| | A | B |
|---|---|---|
| New install (station data only) | 98 MB | 98 MB |
| After 1 year | about 105 MB | about 110 MB |
| After 5 years | about 125 MB | about 155 MB |
| After 10 years | about 150 MB | about 215 MB |

The number of users hardly matters. Each login adds a settings file of about 50 bytes and one small row. What grows the database is:

- **Check-in history**, about 60 bytes per check-in including its net session: roughly 0.4 MB a year for A and 7 MB a year for B.
- **The FCC station data**, about 95 MB of the starting size. It grows as new callsigns are issued, because licenses that lapse are kept rather than removed. That's an estimate of about 5 MB a year, and it's most of A's growth. A new-license rate at the high end would put 10 years at up to about 185 MB for A and 250 MB for B.

**Everything else on disk**

| | Size |
|---|---|
| `uls_cache/` (downloaded FCC and Census files, kept between refreshes) | about 760 MB, not growing |
| `quicklogger.db-wal` (SQLite's write log) | a few MB; up to about 15 MB during the first data load |
| The QuickLogger program | about 1–2 MB |
| `exports/` | a few KB per exported log; only grows if you export |

So even installation B needs only about **1 GB after 10 years**, and most of that is `uls_cache/`. You can delete `uls_cache/` while QuickLogger isn't running to get that space back. It fills up again at the next weekly refresh.

**Memory.** Each session, local or over SSH, uses about 5 MB of its own, whatever it's doing; typing a callsign with the widest Nearby Radius (250 miles) adds about 2 MB. Sessions read the database through a memory map, so the operating system keeps one copy of the pages being read, shared by every session, rather than each session copying them. The station-data updater idles at about 2 MB. It imports the FCC data (at first launch, then for under a minute once a week) in a separate short-lived process that peaks at about 130 MB and returns all of it to the system when it finishes. (On Windows the import runs inside the QuickLogger process, so that memory stays in use until QuickLogger exits.)

**Recommendation.**

- **Disk:** anything with 2 GB free is plenty for either example for a decade. An SSD or a good-quality SD card is fine; the weekly refresh writes a few hundred MB.
- **Memory:** 1 GB of RAM covers the weekly import plus several simultaneous SSH users, so hardware in the Raspberry Pi 3 class (1 GB) or better should handle installation B. QuickLogger hasn't been run on a Pi yet.
- **Backups:** back up `quicklogger.db`. It's the only file that can't be downloaded again. Copy it while QuickLogger isn't running, or use SQLite's `.backup` command while it is.
