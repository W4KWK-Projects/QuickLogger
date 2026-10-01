# import-session: pushing a session upstream

QuickLogger's built-in SSH server runs two commands of its own, for Federated Logging: another QuickLogger (or an app) that logged a session pushes it to a central "upstream" QuickLogger. This page is the interface, for anyone writing such a client. It's versioned: this is **interface 1**.

A push takes two steps, both as an SSH user added in Manage Users who isn't view-only:

1. Upload the session's `.qlsession` file (F7 Export) into your `/imports` with scp or sftp: `scp -P 2222 Skywarn_2026-09-14.qlsession you@upstream:/imports/`
2. Import it: `ssh -p 2222 you@upstream import-session Skywarn_2026-09-14.qlsession`

## Commands

Apart from copying files with scp, the SSH server runs only these two. It never starts a shell or another program: anything else is answered with `status: error` and exit status 1, and nothing runs.

```
version
import-session [--confirm-net "<net name>"] <file>
```

The command line is split into words at spaces, without a shell. A net name with spaces goes in double quotes (`\"` and `\\` inside them stand for `"` and `\`). Outside quotes, only letters, digits and `. _ - / + = : , @ %` are allowed, so `import-session x; ls` is an error, not two commands.

The ssh command on your own computer goes through your own shell first, which removes quotes before ssh sees them. Put the whole command in single quotes so the double quotes reach QuickLogger:

```
ssh -p 2222 you@upstream 'import-session --confirm-net "Sky Warn" Skywarn_2026-09-14.qlsession'
```

A view-only user is refused every command (`status: refused`, exit 4).

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

Which net it goes into:

- **A recurring net's session** goes into the net here with the same name, capitals and spacing aside.
- If there's none, but nets here have names that look like it ("TAG Skywarn" and "Skywarn Weekly Net"), the answer is `needs-confirmation`, naming the first of them alphabetically. Nothing is imported. To import it there, run the command again with `--confirm-net` and that name.
- `--confirm-net` must name a net here exactly (capitals and spacing aside), and that net's name must look like the session's own net name, or the session isn't imported.
- If no net here looks like it, the answer is `no-match`.
- **An ad hoc net's session** becomes a new ad hoc net, as it was defined where it was logged. `--confirm-net` isn't used for one.

Once the session is imported, or found to be here already, the file is deleted from `/imports`. Otherwise it stays, so the same file can be used again with `--confirm-net`.

## Output

Every command writes its result to standard output in the same form. The first line is exactly:

```
QUICKLOGGER-RESULT 1
```

The number is the interface version. Then come `key: value` lines, one per key, always in this order, a key left out when it doesn't apply:

| Key | Value |
|---|---|
| `status` | `imported`, `already-imported`, `needs-confirmation`, `no-match`, `refused` or `error` (`ok` for `version`) |
| `net` | The net's name on the upstream QuickLogger |
| `net-id` | That net's id there, a whole number |
| `session` | When the session started, in UTC, 24-hour: `2026-09-14 23:30 UTC`. A session logged before start times were recorded has just its date: `2026-09-14` |
| `message` | One sentence, for people |

Each value is on one line: a line break in a net name or a message becomes a space. A client should read keys by name and ignore any it doesn't know.

`net` and `net-id` are given for `imported` and `already-imported` (the net it's in), for `needs-confirmation` (the net to confirm), and for `refused` when `--confirm-net` named a net that doesn't look like the session's.

## Statuses and exit statuses

ssh exits with the command's exit status.

| Status | Exit | Meaning | File in /imports |
|---|---|---|---|
| `imported` | 0 | The session was added. | Deleted |
| `already-imported` | 0 | The net already had it, so pushing again is harmless. | Deleted |
| `needs-confirmation` | 2 | No net has the session's net name, but `net` looks like it. Run again with `--confirm-net`. | Kept |
| `no-match` | 3 | No net here looks like the session's net, or `--confirm-net` names no net here. | Kept |
| `refused` | 4 | A view-only user; a file name that isn't a `.qlsession` in your `/imports`; no such file; over 25 MB; a file that isn't a QuickLogger session; or `--confirm-net` naming a net that doesn't look like the session's. | Kept |
| `error` | 1 | An unknown command, a malformed command line, or something that went wrong on the upstream QuickLogger. | Kept |

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
