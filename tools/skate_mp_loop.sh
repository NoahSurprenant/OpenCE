#!/bin/sh
# tools/skate_mp_loop.sh SLOT RUNS OUT [LOAD]
#
# Runs tools/test_linux_port.py's test_skate_multiplayer RUNS times in a row
# (port/skate/README.md, "Testing multiplayer"), as slot SLOT: its own pair of
# loopback addresses (127.0.SLOT+1.10 and .11, HALO_TEST_SKATE_NET), so that
# slots run side by side. Each run's result goes to OUT/results (pass or
# fail, slot, run); a passed run leaves the host's "skate test:" lines in
# OUT/shots-SLOT-RUN.txt, a failed one its two games' logs and pytest's
# output, and every run's temporary folder (some 190 MB) is deleted as soon
# as it is done. With LOAD (a number), that many busy loops share the
# machine while it runs.
#
# A stall watcher goes with each run. Each game runs with
# tools/stall_backtrace.c preloaded (built here, -m32 as the game): if the
# host's log stops growing for 30 s while the test runs (a game logs every
# second), every thread of the host is sent SIGUSR2 and writes its
# backtrace to the log, and those go to OUT/hang-SLOT-RUN.txt, put through
# addr2line. Neither gdb -p nor a core from SIGQUIT works in a container
# without CAP_SYS_PTRACE, and a game run under gdb stops in it at every
# SIGSEGV the memory watch takes, too slow to be the game that hangs.
#
# It needs what the test needs: HALO_TEST_GAME (the game itself),
# HALO_SKATE_TEST_ASSETS and HALO_SKATE_TEST_HALO_MAPS;
# SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy headless; gcc with -m32,
# addr2line and python3. Run from the repository's root.
slot=$1; runs=$2; out=$3; load=${4:-0}
[ -n "$out" ] && [ -x "$HALO_TEST_GAME" ] || { echo "usage: HALO_TEST_GAME=... $0 SLOT RUNS OUT [LOAD]" >&2; exit 2; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
game=$HALO_TEST_GAME
preload="$out/stall_backtrace-$slot.so"
gcc -m32 -O1 -g -shared -fPIC -o "$preload" tools/stall_backtrace.c || exit 1
wrap="$out/halo-$slot.sh"
printf '#!/bin/sh\nLD_PRELOAD=%s exec %s "$@"\n' "$preload" "$game" > "$wrap"
chmod +x "$wrap"
net="127.0.$((slot + 1)).10,127.0.$((slot + 1)).11"
# (the games whose folder is under $1)
games_of() {
	for pid in $(pgrep -f "$game"); do
		case "$(readlink "/proc/$pid/cwd")" in "$1"/*) echo "$pid";; esac
	done
}
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
	HALO_TEST_GAME=$wrap HALO_TEST_SKATE_NET=$net python3 -m pytest -q -s -p no:cacheprovider \
		tools/test_linux_port.py -k test_skate_multiplayer --basetemp="$base" > "$out/run-$slot-$run.log" 2>&1 &
	test_pid=$!
	host="$base/test_skate_multiplayer0/host"
	last=-1; same=0
	while kill -0 "$test_pid" 2>/dev/null; do
		sleep 2
		[ -f "$host/run.log" ] || continue
		size=$(stat -c %s "$host/run.log")
		if [ "$size" = "$last" ]; then same=$((same + 2)); else same=0; last=$size; fi
		if [ "$same" -ge 30 ]; then
			hang="$out/hang-$slot-$run.txt"
			for pid in $(games_of "$host"); do
				echo "slot $slot run $run: the host (pid $pid) logged nothing for 30 s" > "$hang"
				grep -E "State|Threads" "/proc/$pid/status" >> "$hang"
				for task in /proc/$pid/task/*; do
					tid=${task##*/}
					echo "thread $tid $(cut -d' ' -f3,14,15 "$task/stat") $(cat "$task/comm")" >> "$hang"
					# (tgkill, system call 234 on x86-64: SIGUSR2, 12, to that thread)
					python3 -c "import ctypes, sys; ctypes.CDLL(None).syscall(234, $pid, $tid, 12)"
					sleep 1
				done
				sleep 2
				grep -h "^stall-backtrace:" "$host/run.log" | while read -r _ word rest; do
					if [ "$word" = thread ]; then echo "== thread $rest"
					else echo "$word $(addr2line -f -i -C -p -e "$game" "$word" 2>/dev/null | head -n 3 | tr '\n' ' ')"
					fi
				done >> "$hang"
			done
			same=-1000000
		fi
	done
	wait "$test_pid"
	status=$?
	[ -n "$burners" ] && kill $burners 2>/dev/null
	for pid in $(games_of "$base"); do kill -9 "$pid" 2>/dev/null; done
	if [ "$status" -eq 0 ]; then
		echo "pass $slot $run" >> "$out/results"
		grep -h "skate test:" "$host/run.log" | grep -v "obstacles near" > "$out/shots-$slot-$run.txt" 2>/dev/null
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
