# Upstream commands: pushing and pulling

QuickLogger's built-in SSH server runs commands of its own, for Federated Logging: another QuickLogger (or an app) that logged a session pushes it to a central "upstream" QuickLogger, and pulls nets and sessions back from it. This page is the interface, for anyone writing such a client. It's versioned: this is **interface 1**.

A push takes two steps, both as an SSH user added in Manage Users who isn't view-only:

1. Upload the session's `.qlsession` file (F7 Export) into your `/imports` with scp or sftp: `scp -P 2222 Skywarn_2026-09-14.qlsession you@upstream:/imports/`
2. Import it: `ssh -p 2222 you@upstream import-session Skywarn_2026-09-14.qlsession`

A pull takes three steps, and any SSH user may make it, view-only ones included:

1. List the upstream's nets: `ssh -p 2222 you@upstream list-nets`
2. Have it write the one you want into your `/exports`: `ssh -p 2222 you@upstream 'export-net "TAG Skywarn"'` for the net as a `.qlnet`, or `export-sessions` for each of its closed sessions as a `.qlsession`. The answer names the files.
3. Fetch them with scp or sftp: `scp -P 2222 you@upstream:/exports/TAG_Skywarn.qlnet .`

## Commands

Apart from copying files with scp, the SSH server runs only these six. It never starts a shell or another program: anything else is answered with `status: error` and exit status 1, and nothing runs.

```
version
import-session [--confirm-net "<net name>"] <file>
discard-upload <file>
list-nets
export-net "<net name>"
export-sessions "<net name>"
```

The command line is split into words at spaces, without a shell. A net name with spaces goes in double quotes (`\"` and `\\` inside them stand for `"` and `\`). Outside quotes, only letters, digits and `. _ - / + = : , @ %` are allowed, so `import-session x; ls` is an error, not two commands.

The ssh command on your own computer goes through your own shell first, which removes quotes before ssh sees them. Put the whole command in single quotes so the double quotes reach QuickLogger:

```
ssh -p 2222 you@upstream 'import-session --confirm-net "Sky Warn" Skywarn_2026-09-14.qlsession'
```

A view-only user may run `version`, `list-nets`, `export-net` and `export-sessions`, which only read, as they may take an export over ZMODEM or SFTP. `import-session` and `discard-upload` are refused them (`status: refused`, exit 4).

### version

```
QUICKLOGGER-RESULT 1
status: ok
version: 2.0.0
interface: 1
```

`version` is QuickLogger's version, and `interface` this interface's number.

### import-session

`<file>` is a `.qlsession` file in your own `/imports`, given as its name (`Skywarn_2026-09-14.qlsession`) or as `/imports/Skywarn_2026-09-14.qlsession`. Names follow the SFTP upload rules: letters, digits, `.`, `_` and `-`, not starting with `.`, up to 25 MB.

The session is imported as History's **F6 Import** would: into an existing net as a new session, its stations saved to the net. A session the net already has (the same date and start time) isn't imported twice.

Which net it goes into, of those on the session's own service (Amateur Radio or GMRS; nets on the other are passed over):

- **A recurring net's session** goes into the net here with the same name, capitals and spacing aside.
- If there's none, but nets here have names that look like it ("TAG Skywarn" and "Skywarn Weekly Net"), the answer is `needs-confirmation`, naming the first of them alphabetically. Nothing is imported. To import it there, run the command again with `--confirm-net` and that name.
- `--confirm-net` must name a net here exactly (capitals and spacing aside), and that net's name must look like the session's own net name, or the session isn't imported.
- If no net here looks like it, the answer is `no-match`.
- **An ad hoc net's session** becomes a new ad hoc net, as it was defined where it was logged. `--confirm-net` isn't used for one.

The file is deleted from `/imports` once the command has answered, whatever the answer was: a push that fails leaves nothing behind. The one exception is `needs-confirmation`, which keeps it so the same file can be used again with `--confirm-net`. If the answer to that question is no, send `discard-upload` for it.

### discard-upload

`<file>` is named as for `import-session`, but may be any upload in your `/imports`. It deletes that file, for a client that was asked to confirm a net and decided against it. Nothing there is fine too.

```
QUICKLOGGER-RESULT 1
status: ok
message: Discarded Skywarn_2026-09-14.qlsession.
```

A name that isn't an upload in your `/imports` is `refused` (exit 4), and a view-only user is refused as for every command. An upstream older than this command answers `status: error` (exit 1) and keeps the file, which it deletes itself a week after the upload; a client can ignore that.

### list-nets

```
QUICKLOGGER-RESULT 1
status: ok
net: Hamilton County ARES
service: amateur
sessions: 0
net: TAG Skywarn
service: amateur
sessions: 12
message: 2 nets.
```

Every recurring net (ad hoc nets aren't listed), in alphabetical order. A `net` line starts a net, and its `service` (`amateur` or `gmrs`) and `sessions` (its closed sessions) lines follow it. A client that offers the list to a person can show the names and counts as they are.

### export-net

`export-net "<net name>"` writes the net into your `/exports` as a `.qlnet`, the file F8 Export on the net list makes: its settings, saved stations and every session with its check-ins. The net is named as `list-nets` gave it (capitals and spacing aside); a look-alike name isn't a match.

```
QUICKLOGGER-RESULT 1
status: ok
net: TAG Skywarn
net-id: 4
file: TAG_Skywarn.qlnet
message: Exported TAG Skywarn as TAG_Skywarn.qlnet.
```

Fetch `file` from `/exports`; the client imports it as it would any `.qlnet` it was sent. A net nobody here has is `no-match` (exit 3).

### export-sessions

`export-sessions "<net name>"` writes each closed session of the net into your `/exports` as a `.qlsession`, the file F7 Export in History makes, and names them, newest session first. A session still open isn't written: it isn't finished.

```
QUICKLOGGER-RESULT 1
status: ok
net: TAG Skywarn
net-id: 4
sessions: 2
not-sent: 1
file: TAG_Skywarn_2026-09-14_233000.qlsession
file: TAG_Skywarn_2026-09-07_233000.qlsession
message: Exported 2 of TAG Skywarn's sessions.
```

`sessions` is how many files there are and `not-sent` (left out when 0) how many sessions weren't written: the ones still open, and any beyond the newest 300. A file's name is the net, the session's date and its start time in UTC. A client imports each as it would a `.qlsession` it was sent, which skips one it already has; an upstream net with more than 300 closed sessions is better pulled whole with `export-net`.

A file written for a pull stays in `/exports` for a week, like any export, then the server clears it. Writing it again replaces it.

## Output

Every command writes its result to standard output in the same form. The first line is exactly:

```
QUICKLOGGER-RESULT 1
```

The number is the interface version. Then come `key: value` lines, one per key, always in this order, a key left out when it doesn't apply:

| Key | Value |
|---|---|
| `status` | `imported`, `already-imported`, `needs-confirmation`, `no-match`, `refused` or `error` (`ok` for `version`, `list-nets`, `export-net`, `export-sessions` and `discard-upload`) |
| `net` | The net's name on the upstream QuickLogger |
| `net-id` | That net's id there, a whole number |
| `session` | When the session started, in UTC, 24-hour: `2026-09-14 23:30 UTC`. A session logged before start times were recorded has just its date: `2026-09-14` |
| `service` | `list-nets`: a net's service, `amateur` or `gmrs` |
| `sessions` | `list-nets`: a net's closed sessions; `export-sessions`: how many files were written |
| `not-sent` | `export-sessions`: sessions not written, when there are any |
| `file` | `export-net` and `export-sessions`: a file now in your `/exports`, once for each |
| `message` | One sentence, for people |

Each value is on one line: a line break in a net name or a message becomes a space. A client should read keys by name and ignore any it doesn't know. `list-nets` is the one answer where a key repeats in groups: each `net` line begins a net, and the `service` and `sessions` lines after it belong to it. `export-sessions` repeats `file`, once per file.

`net` and `net-id` are given for `imported` and `already-imported` (the net it's in), for `needs-confirmation` (the net to confirm), and for `refused` when `--confirm-net` named a net that doesn't look like the session's.

## Statuses and exit statuses

ssh exits with the command's exit status.

| Status | Exit | Meaning | File in /imports |
|---|---|---|---|
| `imported` | 0 | The session was added. | Deleted |
| `already-imported` | 0 | The net already had it, so pushing again is harmless. | Deleted |
| `needs-confirmation` | 2 | No net has the session's net name, but `net` looks like it. Run again with `--confirm-net`. | Kept |
| `no-match` | 3 | No net here looks like the session's net, or `--confirm-net` names no net here. For `export-net` and `export-sessions`: no net here has that name. | Deleted |
| `refused` | 4 | A view-only user running `import-session` or `discard-upload`; a file name that isn't a `.qlsession` in your `/imports`; no such file; over 25 MB; a file that isn't a QuickLogger session; or `--confirm-net` naming a net that doesn't look like the session's, or one on the other service. | Deleted, except when the file wasn't there or the user is view-only |
| `error` | 1 | An unknown command, a malformed command line, or something that went wrong on the upstream QuickLogger. | Deleted if it was an `import-session` of an upload |

ssh itself exits 255 when it can't connect or log in; that's not one of these.

## Example

```
$ scp -P 2222 TAG_Skywarn_2026-09-14.qlsession w4kwk@upstream:/imports/
$ ssh -p 2222 w4kwk@upstream import-session TAG_Skywarn_2026-09-14.qlsession
QUICKLOGGER-RESULT 1
status: needs-confirmation
net: Sky Warn
net-id: 2
session: 2026-09-14 23:30 UTC
message: The session was logged as "TAG Skywarn"; confirm "Sky Warn" to import it there. 1 other net looks alike too.
$ echo $?
2
$ ssh -p 2222 w4kwk@upstream 'import-session --confirm-net "Sky Warn" TAG_Skywarn_2026-09-14.qlsession'
QUICKLOGGER-RESULT 1
status: imported
net: Sky Warn
net-id: 2
session: 2026-09-14 23:30 UTC
message: Imported the session into Sky Warn (logged as "TAG Skywarn").
$ echo $?
0
```

A pull, as a view-only user might make it:

```
$ ssh -p 2222 k4view@upstream list-nets
QUICKLOGGER-RESULT 1
status: ok
net: TAG Skywarn
service: amateur
sessions: 12
message: 1 net.
$ ssh -p 2222 k4view@upstream 'export-net "TAG Skywarn"'
QUICKLOGGER-RESULT 1
status: ok
net: TAG Skywarn
net-id: 4
file: TAG_Skywarn.qlnet
message: Exported TAG Skywarn as TAG_Skywarn.qlnet.
$ scp -P 2222 k4view@upstream:/exports/TAG_Skywarn.qlnet .
```
