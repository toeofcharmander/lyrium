#!/usr/bin/env bash
#
# Drive Ghidra's headless analyzer from WSL against the Windows install.
#
#   run.sh import                          one-time; 20-60 minutes
#   run.sh script DaoSanity.java [args]    read-only reporting run
#   run.sh write  DaoNames.java  [args]    mutating run, changes are saved
#
# Every run after the import uses -process -noanalysis, so it starts in seconds.
# -import must never be used twice: with -overwrite it silently re-analyses from
# scratch and throws away the hour.
#
# The command is written to a .bat and cmd.exe is pointed at that, rather than being
# passed on the command line. Passing it inline does not work: WSL's interop re-quotes
# the argument, and even `cmd.exe /c 'dir "C:\Program Files\..."'` comes back with
# "The filename, directory name, or volume label syntax is incorrect". Every path here
# has a space in it, so this is not avoidable -- inside a .bat the quoting is ordinary
# batch syntax that nothing else gets to touch.

set -euo pipefail

readonly GHIDRA_HOME='C:\Program Files\ghidra\ghidra_12.1.2_PUBLIC'
readonly GAME_EXE='E:\Dragon Age Origins\bin_ship\DAOrigins.exe'
readonly PROJECT_NAME='dao'
readonly PROGRAM_NAME='DAOrigins.exe'
readonly MAXMEM='16G'
readonly MAX_CPU='8'

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo_root"

readonly ANALYSIS="$repo_root/analysis/ghidra"
mkdir -p "$ANALYSIS/proj" "$ANALYSIS/out" "$ANALYSIS/decomp" "$ANALYSIS/logs"

win() { wslpath -w "$1"; }

readonly PROJ_W=$(win "$ANALYSIS/proj")
readonly OUT_W=$(win "$ANALYSIS/out")
readonly DECOMP_W=$(win "$ANALYSIS/decomp")
readonly LOGS_W=$(win "$ANALYSIS/logs")
readonly SCRIPTS_W=$(win "$repo_root/tools/ghidra")

# Run a batch body. Written to a file because of the interop quoting above.
run_bat() {
    local stem=$1 body=$2
    local bat="$ANALYSIS/logs/_$stem.bat"
    {
        echo '@echo off'
        echo "set \"GHIDRA_HEADLESS_MAXMEM=$MAXMEM\""
        echo "$body"
        echo 'exit /b %ERRORLEVEL%'
    } >"$bat"
    (cd /mnt/c && cmd.exe /c "$(wslpath -w "$bat")")
}

case "${1:-}" in
import)
    if [[ -e "$ANALYSIS/proj/$PROJECT_NAME.gpr" ]]; then
        echo "refusing: $ANALYSIS/proj/$PROJECT_NAME.gpr already exists." >&2
        echo "Re-importing would discard the analysed database. Delete it deliberately first." >&2
        exit 1
    fi
    echo "Importing $GAME_EXE -- expect 20-60 minutes."
    run_bat import "\"$GHIDRA_HOME\\support\\analyzeHeadless.bat\" \"$PROJ_W\" $PROJECT_NAME \
-import \"$GAME_EXE\" \
-loader PeLoader -loader-loadLibraries false \
-processor \"x86:LE:32:default\" -cspec windows \
-max-cpu $MAX_CPU \
-log \"$LOGS_W\\import.log\" -scriptlog \"$LOGS_W\\import.script.log\"" \
        2>&1 | tee "$ANALYSIS/logs/import.console.log"
    ;;

script | write)
    mode=$1
    shift
    [[ $# -ge 1 ]] || { echo "usage: run.sh $mode <Script.java> [args...]" >&2; exit 2; }
    script_name=$1
    shift
    readonly_flag='-readOnly'
    [[ $mode == write ]] && readonly_flag=''
    stem=${script_name%.java}
    script_args=''
    for a in "$@"; do
        # Convenience: OUT / DECOMP expand to the Windows paths the scripts want.
        case $a in
        OUT) a=$OUT_W ;;
        DECOMP) a=$DECOMP_W ;;
        esac
        script_args+=" \"$a\""
    done
    run_bat "$stem" "\"$GHIDRA_HOME\\support\\analyzeHeadless.bat\" \"$PROJ_W\" $PROJECT_NAME \
-process $PROGRAM_NAME -noanalysis $readonly_flag \
-scriptPath \"$SCRIPTS_W\" \
-postScript $script_name$script_args \
-log \"$LOGS_W\\$stem.log\" -scriptlog \"$LOGS_W\\$stem.script.log\"" \
        2>&1 | tee "$ANALYSIS/logs/$stem.console.log"
    # A script that fails to compile writes nothing useful to stdout; the reason is only
    # ever in the script log, so surface it rather than letting the run look successful.
    if grep -qiE "error|exception|unable to compile" "$ANALYSIS/logs/$stem.script.log" 2>/dev/null; then
        echo "--- $stem.script.log ---" >&2
        cat "$ANALYSIS/logs/$stem.script.log" >&2
    fi
    ;;

*)
    awk 'NR > 2 && !/^#/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"
    exit 2
    ;;
esac
