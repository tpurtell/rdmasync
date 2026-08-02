#!/usr/bin/env bash
# Live RoCE integration and failure-cleanup checks.

set -u

if (( $# < 2 || $# > 3 )); then
	echo "usage: $0 HOST REMOTE_RSYNC [LOCAL_RSYNC]" >&2
	exit 2
fi

host=$1
remote_rsync=$2
local_rsync=${3:-./rdmasync}
expected_rails=${RDMASYNC_EXPECT_RAILS:-1}
remote_base=/tmp/rdmasync-integration-$(id -u)-$$
tmpdir=$(mktemp -d /tmp/rdmasync-integration.XXXXXX) || exit 1
src=$tmpdir/source.bin
out=$tmpdir/output
remote_paths=()
active_pid=

fail()
{
	echo "rdma-integration: $*" >&2
	exit 1
}

cleanup()
{
	if [[ -n $active_pid ]]; then
		kill -TERM "$active_pid" >/dev/null 2>&1 || true
		wait "$active_pid" >/dev/null 2>&1 || true
	fi
	if (( ${#remote_paths[@]} )); then
		ssh "$host" rm -f -- "${remote_paths[@]}" >/dev/null 2>&1 || true
	fi
	rm -rf -- "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

wait_remote_gone()
{
	local target=$1
	local i
	for ((i=0; i<20; i++)); do
		if ! ssh "$host" "ps -C rsync -o args= | grep -F -- '$target'" \
			>/dev/null 2>&1; then
			return 0
		fi
		sleep 0.25
	done
	return 1
}

remote_sum()
{
	ssh "$host" sha256sum "$1" | awk '{print $1}'
}

check_copy()
{
	local destination=$1
	local expected actual
	expected=$(sha256sum "$src" | awk '{print $1}')
	actual=$(remote_sum "$destination") || fail "cannot checksum $destination"
	[[ $actual == "$expected" ]] || fail "content mismatch for $destination"
}

run_copy()
{
	local destination=$1
	shift
	remote_paths+=("$destination")
	"$@" >"$out" 2>&1 \
		|| { sed -n '1,160p' "$out" >&2; fail "copy to $destination failed"; }
	check_copy "$destination"
}

dd if=/dev/urandom of="$src" bs=1M count=16 status=none \
	|| fail "cannot create test source"

normal=$remote_base-normal
run_copy "$normal" env "$local_rsync" -aW --rdma=required --rdma-show-config \
	--rsync-path="$remote_rsync" "$src" "$host:$normal"
grep -q "RDMA active: $expected_rails rail" "$out" \
	|| fail "automatic rail count did not match $expected_rails"

silent=$remote_base-silent
run_copy "$silent" env "$local_rsync" -aW --rdma=required \
	--rsync-path="$remote_rsync" "$src" "$host:$silent"
if grep -qE 'RDMA (active|data):' "$out"; then
	fail "successful RDMA negotiation was printed without --rdma-show-config"
fi

one=$remote_base-one
run_copy "$one" env "$local_rsync" -aW --rdma=required --rdma-rails=1 \
	--rsync-path="$remote_rsync" "$src" "$host:$one"

enumeration=$remote_base-enumeration
run_copy "$enumeration" env "$local_rsync" -aW --rdma=auto \
	--rdma-device=rdmasync-no-such-device --rsync-path="$remote_rsync" \
	"$src" "$host:$enumeration"
grep -q 'using rsync-over-SSH' "$out" \
	|| fail "enumeration failure did not report SSH fallback"

if "$local_rsync" -aW --rdma=required \
	--rdma-device=rdmasync-no-such-device --rsync-path="$remote_rsync" \
	"$src" "$host:$remote_base-required" >"$out" 2>&1; then
	fail "required mode accepted an empty candidate set"
fi
remote_paths+=("$remote_base-required")
grep -q 'RDMA required but unavailable' "$out" \
	|| fail "required-mode failure reason is missing"

for stage in connect qp ready; do
	destination=$remote_base-$stage
	remote_paths+=("$destination")
	RSYNC_TEST_RDMA_FAIL=$stage "$local_rsync" -aW --rdma=auto \
		--rsync-path="$remote_rsync" "$src" "$host:$destination" \
		>"$out" 2>&1 \
		|| { sed -n '1,160p' "$out" >&2; fail "$stage fallback copy failed"; }
	grep -q 'using rsync-over-SSH' "$out" \
		|| fail "$stage failure did not report SSH fallback"
	check_copy "$destination"
done

listener=$remote_base-listener
remote_paths+=("$listener")
remote_fail_rsync="env RSYNC_TEST_RDMA_FAIL=listener $remote_rsync"
"$local_rsync" -aW --rdma=auto --rsync-path="$remote_fail_rsync" \
	"$src" "$host:$listener" >"$out" 2>&1 \
	|| { sed -n '1,160p' "$out" >&2; fail "listener fallback copy failed"; }
grep -q 'using rsync-over-SSH' "$out" \
	|| fail "listener failure did not report SSH fallback"
check_copy "$listener"

old_peer=$remote_base-old-peer
remote_paths+=("$old_peer")
if ssh "$host" test -x /usr/bin/rsync; then
	"$local_rsync" -aW --rdma=auto --rsync-path=/usr/bin/rsync \
		"$src" "$host:$old_peer" >"$out" 2>&1 \
		|| { sed -n '1,160p' "$out" >&2; fail "old-peer fallback failed"; }
	grep -q 'using rsync-over-SSH' "$out" || fail "old-peer warning is missing"
	check_copy "$old_peer"
fi

post_target=$remote_base-post-data
remote_paths+=("$post_target")
if RSYNC_TEST_RDMA_FAIL_AFTER_BYTES=4194304 "$local_rsync" -aW \
	--checksum-choice=none --rdma=required --rdma-discard \
	--synthetic-file-data=1G --rsync-path="$remote_rsync" \
	"$src" "$host:$post_target" >"$out" 2>&1; then
	fail "post-data failure injection returned success"
fi
grep -q 'failed after activation' "$out" \
	|| { sed -n '1,160p' "$out" >&2; fail "post-data failure reason is missing"; }
if grep -q 'using rsync-over-SSH' "$out"; then
	fail "post-data failure silently fell back to SSH"
fi
ssh "$host" test ! -e "$post_target" \
	|| fail "post-data discard unexpectedly created its destination"
wait_remote_gone "$post_target" || fail "remote rsync survived post-data failure"

ssh_target=$remote_base-ssh-death
remote_paths+=("$ssh_target")
"$local_rsync" -aW --checksum-choice=none --rdma=required --rdma-discard \
	--rdma-show-config --synthetic-file-data=1T --rsync-path="$remote_rsync" \
	"$src" "$host:$ssh_target" >"$out" 2>&1 &
active_pid=$!
for ((i=0; i<100; i++)); do
	grep -q 'RDMA active:' "$out" && break
	kill -0 "$active_pid" 2>/dev/null || break
	sleep 0.05
done
grep -q 'RDMA active:' "$out" || fail "SSH-death test never activated RDMA"
ssh_pid=
for ((i=0; i<40; i++)); do
	ssh_pid=$(pgrep -P "$active_pid" -x ssh | head -1 || true)
	[[ -n $ssh_pid ]] && break
	sleep 0.05
done
[[ -n $ssh_pid ]] || fail "cannot find transfer SSH child"
kill -TERM "$ssh_pid" 2>/dev/null || fail "cannot terminate transfer SSH child"
if wait "$active_pid"; then
	fail "transfer returned success after SSH death"
fi
active_pid=
wait_remote_gone "$ssh_target" || fail "remote rsync survived SSH death"
ssh "$host" test ! -e "$ssh_target" \
	|| fail "SSH-death discard unexpectedly created its destination"

cancel_target=$remote_base-cancel
remote_paths+=("$cancel_target")
"$local_rsync" -aW --checksum-choice=none --rdma=required --rdma-discard \
	--rdma-show-config --synthetic-file-data=1T --rsync-path="$remote_rsync" \
	"$src" "$host:$cancel_target" >"$out" 2>&1 &
cancel_pid=$!
active_pid=$cancel_pid
for ((i=0; i<100; i++)); do
	grep -q 'RDMA active:' "$out" && break
	kill -0 "$cancel_pid" 2>/dev/null || break
	sleep 0.05
done
grep -q 'RDMA active:' "$out" || fail "cancellation test never activated RDMA"
kill -INT "$cancel_pid" 2>/dev/null || fail "cannot interrupt cancellation test"
if wait "$cancel_pid"; then
	fail "interrupted transfer returned success"
fi
active_pid=

wait_remote_gone "$cancel_target" || fail "remote rsync survived cancellation"
ssh "$host" test ! -e "$cancel_target" \
	|| fail "cancelled discard unexpectedly created its destination"

echo "rdma-integration: opt-in diagnostics, activation, one-rail, fallback, post-data failure, SSH death, and cancellation verified"
