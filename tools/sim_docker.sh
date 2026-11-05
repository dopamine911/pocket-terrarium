#!/usr/bin/env bash
# Native Linux desktop launcher. The image contains the model; only saves and display/audio sockets are mounted.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
world=terrarium board=rect headless=0 build_only=0
usage() {
  cat <<'EOF'
Usage: tools/sim_docker.sh [--world terrarium|aquarium] [--board rect|round|watch|square]
                          [--build-only] [--headless] [-- simulator arguments...]
Builds a cached image, then opens the SDL window on the local Linux desktop.
--headless uses SDL's dummy video/audio drivers for selftests and frame exports.
Saves: ${PT_SIM_DATA:-~/.cache/pocket-terrarium/docker/<world>/<board>}
Docker context: Docker's normal selection; set DOCKER_CONTEXT=default for the system daemon.
EOF
}
while (($#)); do
  case "$1" in
    --world|--board)
      (($# >= 2)) || { usage >&2; exit 2; }
      case "$1" in --world) world=$2 ;; --board) board=$2 ;; esac
      shift 2 ;;
    --headless) headless=1; shift ;;
    --build-only) build_only=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    *) echo "Use -- before simulator arguments: $1" >&2; usage >&2; exit 2 ;;
  esac
done
case "$world" in terrarium|aquarium) ;; *) echo "Unknown world: $world" >&2; exit 2 ;; esac
case "$board" in rect|round|watch|square) ;; *) echo "Unknown board: $board" >&2; exit 2 ;; esac
if ! docker info >/dev/null 2>&1; then
  echo 'Docker is unavailable in the selected context. On this Linux host try DOCKER_CONTEXT=default.' >&2
  exit 1
fi
image="pocket-terrarium-sim:$world-$board"
docker build -f "$ROOT/sim/Dockerfile" --build-arg WORLD="$world" --build-arg BOARD="$board" \
  --build-arg JOBS="${PT_SIM_JOBS:-4}" -t "$image" "$ROOT"
((build_only)) && exit 0
save=${PT_SIM_DATA:-"$HOME/.cache/pocket-terrarium/docker/$world/$board"}
mkdir -p "$save"
save=$(cd "$save" && pwd)
uid=$(id -u) gid=$(id -g)
args=(--rm --init --user "$uid:$gid" --cap-drop ALL --security-opt no-new-privileges
      --mount "type=bind,src=$save,dst=/home/sim/.cache/pocket-terrarium"
      --tmpfs "/run/sim:mode=0700,uid=$uid,gid=$gid")
# X11 (also XWayland on a Wayland desktop), authorized by an existing cookie, not xhost +.
if ((headless)); then
  args+=(-e SDL_VIDEODRIVER=dummy -e SDL_AUDIODRIVER=dummy)
elif [[ ${DISPLAY:-} =~ ^:[0-9]+(\.[0-9]+)?$ ]]; then
  auth=${XAUTHORITY:-$HOME/.Xauthority}
  [[ -r $auth ]] || { echo 'X11 requires a readable XAUTHORITY cookie file.' >&2; exit 1; }
  args+=(-e SDL_VIDEODRIVER=x11 -e DISPLAY -e XAUTHORITY=/run/sim/Xauthority -e XAUTHLOCALHOSTNAME="$(hostname)"
         --mount type=bind,src=/tmp/.X11-unix,dst=/tmp/.X11-unix,readonly
         --mount "type=bind,src=$auth,dst=/run/sim/Xauthority,readonly")
else
  echo 'No local X11/XWayland display found. Run on your desktop with DISPLAY set, or use --headless.' >&2
  exit 1
fi
if ((!headless)); then
  if [[ -n ${XDG_RUNTIME_DIR:-} && -S $XDG_RUNTIME_DIR/pulse/native ]]; then
    args+=(-e SDL_AUDIODRIVER=pulseaudio -e PULSE_SERVER=unix:/run/sim/pulse-native
           --mount "type=bind,src=$XDG_RUNTIME_DIR/pulse/native,dst=/run/sim/pulse-native")
  else
    args+=(-e SDL_AUDIODRIVER=dummy)
    echo 'No local PulseAudio/PipeWire-Pulse socket; running without sound.' >&2
  fi
fi
exec docker run "${args[@]}" "$image" "$@"
