# NRADE: Network Resource Allocation and Deadlock Detection Engine

Multithreaded C11 + POSIX simulator. Network nodes (threads) compete for link
bandwidth, buffers, ports and channels. The engine avoids, detects and
recovers from deadlocks.

## Get the code

```sh
git clone https://github.com/kartikmarwari700/NRADE.git
cd NRADE
```

Without Git: on the GitHub page click **Code > Download ZIP** and extract it
(the folder is then called `NRADE-main`).

## Run on Linux / WSL 2 (the platform in the synopsis)

```sh
sudo apt install build-essential valgrind graphviz
make            # build
make test       # TC1 - TC8 and extra checks (takes up to 30 s or so)
make run        # interactive menu
make demo       # step by step demo for the teacher
```

## Run on Windows (MSYS2)

One time setup:

1. Install MSYS2 from <https://www.msys2.org> and keep the default folder
   `C:\msys64`.
2. Open **MSYS2 MSYS** from the Start menu (not UCRT64 or MINGW64) and run
   `pacman -S gcc make`, then press Enter / `Y` to confirm.

Do not use the old MinGW compiler or the VS Code Run button: this project needs
POSIX threads. Then, in PowerShell inside the project folder:

```powershell
.\run.bat          # menu
.\run.bat demo     # teacher demo
.\run.bat test     # tests
```

## Command line

```sh
./nrade -c config/textbook.txt --check
./nrade -c config/single_cycle.txt --detect
./nrade -c config/multi_deadlock.txt --recover
./nrade -c config/single_cycle.txt --simulate --mode detect --script config/ring_script.txt --verbose
./nrade -c config/stress.txt --compare --ops 15 --interval 2
./nrade --help
```

## Folder structure

| Path | What it contains | Module |
|---|---|---|
| `include/nrade.h` | common header: data structures and prototypes | all |
| `src/topology.c` | config parser, menu input, validation, invariant | M1 |
| `src/simulator.c` | one pthread per node, random / scripted requests | M2 |
| `src/banker.c` | request, release, Safety and Resource-Request algorithms | M3 |
| `src/sync.c` | monitor: mutex, condition variable, semaphores, FIFO queue | M4 |
| `src/detector.c` | Wait-For Graph, DFS, Work/Finish, detector thread | M5 |
| `src/recovery.c` | victim cost, pre-emption / termination | M6 |
| `src/logger.c` | logs, tables, DOT graphs, comparison report | M6 |
| `src/main.c` | menu and command line | front end |
| `config/` | input files for the test cases and demos | |
| `tests/` | automatic tests and demo script | |
| `docs/VIVA_GUIDE.md` | explanation for presentation and viva (Hinglish) | |

## Output files (folder `output/`, created when the program runs)

| File | Content |
|---|---|
| `events.log` | every request, grant, block, release, deadlock, recovery (new on each start) |
| `snapshots.txt` | allocation tables (appended) |
| `rag.dot`, `wfg.dot` | graphs from a manual detection |
| `rag_deadlock.dot`, `wfg_deadlock.dot` | graphs of a detected deadlock |
| `report.txt` | avoidance vs detection comparison |

Draw the graphs with `make graphs` (needs Graphviz), or open the `.dot` text
in any Graphviz viewer.

## Requirements coverage

| Req | Where |
|---|---|
| FR1 | `topology_load`, `topology_from_menu` |
| FR2 | `try_allocate`, `release_resources` |
| FR3 | `safety_check`, `try_allocate`; menu options 4 and 5 |
| FR4 | `build_wfg`, `find_cycle_dfs`, `write_rag_dot` |
| FR5 | `detect_deadlock` |
| FR6 | `victim_cost`, `select_victim`, `apply_recovery` |
| FR7 | `simulate`, `random_node`, `scripted_node` |
| FR8 | `logger.c` |

## Design decisions and limits

* **FIFO queue:** requests are examined oldest first. A request that cannot be
  granted does not block the ones behind it, because that can stall the system.
  After 25 overtakes, nodes that hold nothing must wait behind the starving one.
* **Checkpoint:** a pre-empted node rolls back to the start of its job, where it
  held nothing.
* **Prevention:** there is no separate prevention mode (FR1 - FR8 do not ask for
  one). Pre-emption during recovery attacks the "no pre-emption" condition.
* **TC6 order:** the program prints `<N0,N2,N3,N4,N1>`, the synopsis lists
  `<N0,N2,N3,N1,N4>`. A finish order is not unique; the test checks that both
  are valid.
* **Valgrind:** `make helgrind` and `make memcheck` need Linux. They have not
  been run yet.
* **Timing numbers** differ between runs and machines.
