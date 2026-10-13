#!/bin/sh
# tools/skate_mp_loop.sh SLOT RUNS OUT [LOAD]
#
# Runs tools/test_linux_port.py's test_skate_multiplayer RUNS times in a row
# (port/skate/README.md, "Testing multiplayer"), as slot SLOT: its own pair of
# loopback addresses (127.0.SLOT+1.10 and .11, HALO_TEST_SKATE_NET), so that
# slots run side by side. Each run's result goes to OUT/results (pass or
# fail, slot, run); a failed run keeps its two games' logs in OUT, and every
# run's temporary folder (some 190 MB) is deleted as soon as it is done.
# With LOAD (a number), that many busy loops share the machine while it runs.
#
# A stall watcher goes with each run: if the host's log stops growing for
# 30 s while it runs (a game logs every second), the host is sent SIGQUIT,
# which dumps its core in its folder (ulimit -c unlimited, core_pattern
# "core"), and gdb writes every thread's backtrace from the core to
# OUT/hang-SLOT-RUN.txt. It is for where ptrace is not allowed (a container
# without CAP_SYS_PTRACE: gdb -p fails there, a core read offline does not).
#
# It needs what the test needs: HALO_TEST_GAME, HALO_SKATE_TEST_ASSETS and
# HALO_SKATE_TEST_HALO_MAPS; SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy
# headless. Run from the repository's root.
slot=$1; runs=$2; out=$3; load=${4:-0}
[ -n "$out" ] || { echo "usage: $0 SLOT RUNS OUT [LOAD]" >&2; exit 2; }
mkdir -p "$out"
ulimit -c unlimited
net="127.0.$((slot + 1)).10,127.0.$((slot + 1)).11"
run=1
while [ "$run" -le "$runs" ]; do
	base="$out/tmp-$slot-$run"
	rm -rf "$base"
	burners=""
	i=0
	while [ "$i" -lt "$load" ]; do
		sh -c 'while :; do :; done' &
		burners="$burners $!"
		i=$((i + 1))
	done
	HALO_TEST_SKATE_NET=$net python3 -m pytest -q -s -p no:cacheprovider tools/test_linux_port.py \
		-k test_skate_multiplayer --basetemp="$base" > "$out/run-$slot-$run.log" 2>&1 &
	test_pid=$!
	log="$base/test_skate_multiplayer0/host/run.log"
	last=-1; same=0
	while kill -0 "$test_pid" 2>/dev/null; do
		sleep 2
		[ -f "$log" ] || continue
		size=$(stat -c %s "$log")
		if [ "$size" = "$last" ]; then same=$((same + 2)); else same=0; last=$size; fi
		if [ "$same" -ge 30 ]; then
			for pid in $(pgrep -f "$HALO_TEST_GAME"); do
				if [ "$(readlink "/proc/$pid/cwd")" = "$base/test_skate_multiplayer0/host" ]; then
					{ echo "slot $slot run $run: the host (pid $pid) logged nothing for 30 s"
					  grep -E "State|Threads" "/proc/$pid/status"
					  for task in /proc/$pid/task/*; do echo "thread ${task##*/} $(cut -d' ' -f3,14,15 "$task/stat")"; done
					} > "$out/hang-$slot-$run.txt"
					kill -QUIT "$pid"
					sleep 10
					core=$(ls "$base/test_skate_multiplayer0/host"/core* 2>/dev/null | head -n 1)
					if [ -n "$core" ]; then
						gdb -batch -ex "thread apply all bt 40" "$HALO_TEST_GAME" "$core" >> "$out/hang-$slot-$run.txt" 2>&1
					else
						echo "no core dumped" >> "$out/hang-$slot-$run.txt"
					fi
				fi
			done
			same=-1000000
		fi
	done
	wait "$test_pid"
	status=$?
	[ -n "$burners" ] && kill $burners 2>/dev/null
	if [ "$status" -eq 0 ]; then
		echo "pass $slot $run" >> "$out/results"
		grep -h "skate test:" "$base/test_skate_multiplayer0/host/run.log" | grep -v "obstacles near" > "$out/shots-$slot-$run.txt" 2>/dev/null
		rm -f "$out/run-$slot-$run.log"
	else
		echo "fail $slot $run" >> "$out/results"
		for name in host join; do
			cp "$base/test_skate_multiplayer0/$name/run.log" "$out/$name-$slot-$run.log" 2>/dev/null
		done
	fi
	rm -rf "$base"
	run=$((run + 1))
done
