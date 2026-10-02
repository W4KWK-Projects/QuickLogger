#!/bin/sh
# The built-in SSH server end to end: starts a QuickLogger server on this
# machine and runs the system's real ssh, scp and sftp against it:
# Federated Logging pushes (tests/test_push_end_to_end.cpp), and SFTP and
# SCP file transfers (tests/test_files_end_to_end.cpp), over SFTP and
# legacy SCP; and which commands the server will run
# (tests/test_ssh_exec_end_to_end.cpp). Last, tests/push_screens.py drives the real program's push
# screens (Close & Push, History's F3, the look-alike prompt) in a
# pseudo-terminal against the same server; it needs Python's pyte and is
# skipped without it.
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

finish() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
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
PATH="$dir/bin:$PATH" "$build/quicklogger_tests" SshExecEndToEnd

if python3 -c "import pyte" 2>/dev/null; then
    PATH="$dir/bin:$PATH" python3 "$(dirname "$0")/push_screens.py" "$build/QuickLogger" "$dir" "$port"
elif [ -n "${QL_EXPECT_PYTE:-}" ]; then
    echo "QL_EXPECT_PYTE is set but Python's pyte isn't installed." >&2
    exit 1
else
    echo "Skipping the push screens: Python's pyte isn't installed."
fi
