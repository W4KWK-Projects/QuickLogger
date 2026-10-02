#!/bin/sh
# Federated Logging end to end (tests/test_push_end_to_end.cpp): starts an
# upstream QuickLogger on this machine and pushes to it with the system's
# real scp and ssh, over SFTP and legacy SCP.
#
#   tests/push_end_to_end.sh <build-dir> [port]
#
# Everything lives in a temporary folder: a key made for the run, the
# upstream's host key in a known_hosts of its own, and scp and ssh wrappers
# that hand both to the real ones (-F), so ~/.ssh is never read or changed.
# Needs ssh, scp, ssh-keygen and ssh-keyscan (OpenSSH 9.0 or later, for
# scp -O).
set -eu

build=$(cd "${1:?usage: $0 <build-dir> [port]}" && pwd)
port=${2:-2391}
dir=$(mktemp -d "${TMPDIR:-/tmp}/quicklogger-push-e2e.XXXXXX")
server_pid=""

finish() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$dir"
}
trap finish EXIT INT TERM

mkdir -p "$dir/up" "$dir/local" "$dir/ssh" "$dir/bin"
ssh-keygen -q -t ed25519 -N "" -C "push-e2e" -f "$dir/ssh/key"
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
chmod +x "$dir/bin/ssh" "$dir/bin/scp"

export QL_PUSH_E2E_DIR="$dir"
export QL_PUSH_E2E_PORT="$port"
"$build/quicklogger_tests" PushEndToEndSeed

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

PATH="$dir/bin:$PATH" "$build/quicklogger_tests" PushEndToEndRun
