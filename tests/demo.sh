#!/bin/sh
# Teacher demo: press Enter after every step.   Run with:  make demo
step() {
    printf '\n\n########## %s ##########\n' "$1"
    shift
    printf '$ %s\n' "$*"
    "$@"
    printf '\n[press Enter]'
    read -r _
}

step "1. Banker's algorithm: safe state"          ./nrade -c config/textbook.txt --check
step "2. Single-instance deadlock (WFG + DFS)"    ./nrade -c config/single_cycle.txt --detect
step "3. Multi-instance: no deadlock"             ./nrade -c config/multi_nodeadlock.txt --detect
step "4. Multi-instance: deadlock and recovery"   ./nrade -c config/multi_deadlock.txt --recover
step "5. Live threads: deadlock happens, recovery" ./nrade -c config/single_cycle.txt --simulate --mode detect --script config/ring_script.txt --verbose
step "6. Same threads with Banker: no deadlock"   ./nrade -c config/single_cycle.txt --simulate --mode avoid --script config/ring_script.txt --verbose
step "7. 20 threads: compare both modes"          ./nrade -c config/stress.txt --compare --ops 15 --interval 2
