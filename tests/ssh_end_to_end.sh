#!/bin/sh
# The built-in SSH server end to end: starts a QuickLogger server on this
# machine and runs the system's real ssh, scp and sftp against it:
# Federated Logging pushes and pulls (tests/test_push_end_to_end.cpp), and SFTP and
# SCP file transfers (tests/test_files_end_to_end.cpp), over SFTP and
# legacy SCP; and which commands the server will run
# (tests/test_ssh_exec_end_to_end.cpp), and that a key added in My Keys really
# logs in and a removed one stops. A second server with Restricted on
# (tests/test_restricted_end_to_end.cpp, tests/restricted_screens.py) is pushed
# to and logged into as a full user, a Net Admin and an Admin. Last,
# tests/push_screens.py drives the real program's push
# screens (Close & Push, History's F3, the look-alike prompt) in a
# pseudo-terminal against the same server; it needs Python's pyte and is
# skipped without it. With mosh installed (and pyte), tests/mosh_screens.py
# logs in through the real mosh client.
#
#   tests/ssh_end_to_end.sh <build-dir> [port]
#
# Everything lives in a temporary folder: a key made for the run, the
# server's host key in a known_hosts of its own, and ssh, scp and sftp wrappers
# that hand both to the real ones (-F), so ~/.ssh is never read or changed.
# Needs ssh, scp, sftp, ssh-keygen and ssh-keyscan (OpenSSH 9.0 or later, for
# scp -O).
set -eu

build=$(cd "${1:?usage: $0 <build-dir> [port]}" && pwd)
port=${2:-2391}
dir=$(mktemp -d "${TMPDIR:-/tmp}/quicklogger-ssh-e2e.XXXXXX")
server_pid=""
restricted_pid=""

finish() {
    for pid in "$server_pid" "$restricted_pid"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$dir"
}
trap finish EXIT INT TERM

mkdir -p "$dir/up" "$dir/local" "$dir/local-screens" "$dir/ssh" "$dir/bin"
ssh-keygen -q -t ed25519 -N "" -C "ssh-e2e" -f "$dir/ssh/key"
printf 'callsign=W4KWK\nlocation=37415\n' > "$dir/up/settings.txt"

cat > "$dir/ssh/config" <<EOF
Host *
  IdentityFile $dir/ssh/key
  IdentitiesOnly yes
  IdentityAgent none
  UserKnownHostsFile $dir/ssh/known_hosts
  GlobalKnownHostsFile /dev/null
  StrictHostKeyChecking yes
EOF
# SCP_EXTRA lets the test ask for legacy SCP (-O).
cat > "$dir/bin/ssh" <<EOF
#!/bin/sh
exec $(command -v ssh) -F "$dir/ssh/config" "\$@"
EOF
cat > "$dir/bin/scp" <<EOF
#!/bin/sh
exec $(command -v scp) \${SCP_EXTRA:-} -F "$dir/ssh/config" "\$@"
EOF
cat > "$dir/bin/sftp" <<EOF
#!/bin/sh
exec $(command -v sftp) -F "$dir/ssh/config" "\$@"
EOF
chmod +x "$dir/bin/ssh" "$dir/bin/scp" "$dir/bin/sftp"

export QL_PUSH_E2E_DIR="$dir"
export QL_PUSH_E2E_PORT="$port"
"$build/quicklogger_tests" EndToEndSeed
"$build/quicklogger_tests" PushScreensSeed

(cd "$dir/up" && exec "$build/QuickLogger" --headless --ssh-port="$port") > "$dir/up.log" 2>&1 &
server_pid=$!
tries=0
until ssh-keyscan -p "$port" -t ed25519 127.0.0.1 > "$dir/ssh/known_hosts" 2>/dev/null && [ -s "$dir/ssh/known_hosts" ]; do
    tries=$((tries + 1))
    if [ "$tries" -ge 30 ]; then
        echo "The upstream didn't start:" >&2
        cat "$dir/up.log" >&2
        exit 1
    fi
    sleep 1
done

PATH="$dir/bin:$PATH" "$build/quicklogger_tests" EndToEndRun
PATH="$dir/bin:$PATH" "$build/quicklogger_tests" PushNetEndToEnd
PATH="$dir/bin:$PATH" "$build/quicklogger_tests" PullEndToEnd
PATH="$dir/bin:$PATH" "$build/quicklogger_tests" PushConcurrentEndToEnd
PATH="$dir/bin:$PATH" "$build/quicklogger_tests" SshExecEndToEnd
PATH="$dir/bin:$PATH" "$build/quicklogger_tests" KeysAddedInMyKeysWorkOverSsh
PATH="$dir/bin:$PATH" "$build/quicklogger_tests" SftpLibsshEndToEnd

if python3 -c "import pyte" 2>/dev/null; then
    PATH="$dir/bin:$PATH" python3 "$(dirname "$0")/push_screens.py" "$build/QuickLogger" "$dir" "$port"
elif [ -n "${QL_EXPECT_PYTE:-}" ]; then
    echo "QL_EXPECT_PYTE is set but Python's pyte isn't installed." >&2
    exit 1
else
    echo "Skipping the push screens: Python's pyte isn't installed."
fi

# A second server with Restricted on (its own database, on the next port):
# full user, Net Admin and Admin pushing over ssh and scp, then the same three
# logged in on screen. Last of the server tests, since it changes nothing the
# others use.
rport=$((port + 1))
mkdir -p "$dir/restricted"
export QL_RESTRICTED_E2E_PORT="$rport"
"$build/quicklogger_tests" RestrictedSeed
(cd "$dir/restricted" && exec "$build/QuickLogger" --headless --ssh-port="$rport") > "$dir/restricted.log" 2>&1 &
restricted_pid=$!
tries=0
until ssh-keyscan -p "$rport" -t ed25519 127.0.0.1 > "$dir/ssh/restricted_host" 2>/dev/null && [ -s "$dir/ssh/restricted_host" ]; do
    tries=$((tries + 1))
    if [ "$tries" -ge 30 ]; then
        echo "The restricted server didn't start:" >&2
        cat "$dir/restricted.log" >&2
        exit 1
    fi
    sleep 1
done
cat "$dir/ssh/restricted_host" >> "$dir/ssh/known_hosts"

PATH="$dir/bin:$PATH" "$build/quicklogger_tests" RestrictedExecEndToEnd

if python3 -c "import pyte" 2>/dev/null; then
    PATH="$dir/bin:$PATH" python3 -u "$(dirname "$0")/restricted_screens.py" "$dir" "$rport"
elif [ -n "${QL_EXPECT_PYTE:-}" ]; then
    echo "QL_EXPECT_PYTE is set but Python's pyte isn't installed." >&2
    exit 1
else
    echo "Skipping the restricted screens: Python's pyte isn't installed."
fi

if command -v mosh >/dev/null 2>&1 && command -v mosh-server >/dev/null 2>&1 && python3 -c "import pyte" 2>/dev/null; then
    PATH="$dir/bin:$PATH" python3 "$(dirname "$0")/mosh_screens.py" "$dir" "$port"
elif [ -n "${QL_EXPECT_MOSH:-}" ]; then
    echo "QL_EXPECT_MOSH is set but mosh or Python's pyte isn't installed." >&2
    exit 1
else
    echo "Skipping Mosh: mosh (or Python's pyte) isn't installed."
fi
