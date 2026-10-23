#!/usr/bin/env bash
# Uso: ./flash.sh [psram|display] [puerto] [build|flash|monitor]
#   psram    (default) PSRAM octal 8 MB, display apagado salvo que SCL/SDA esten
#            recableados fuera de 33-37 (build, sdkconfig)
#   display  sin PSRAM, OLED en pines originales (build_display, sdkconfig.display)
#   puerto   default /dev/ttyACM0. Con dos placas conectadas conviene usar
#            /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00
#   accion   default flash. "monitor" flashea y abre el monitor serie.
set -euo pipefail

usage() {
    sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
    echo
    echo "Placas detectadas:"
    ls -1 /dev/serial/by-id/ 2>/dev/null | sed 's|^|  /dev/serial/by-id/|' || echo "  (ninguna)"
    exit 1
}

profile=psram
case ${1:-} in
    psram|display) profile=$1; shift ;;
    -h|--help)     usage ;;
esac
port=${1:-/dev/ttyACM0}
action=${2:-flash}

cd "$(dirname "$0")"

case $profile in
    psram)   args=(-B build) ;;
    display) args=(-B build_display -D SDKCONFIG=sdkconfig.display
                   -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.display") ;;
esac

case $action in
    build)   cmd=(build) ;;
    flash)   cmd=(-p "$port" flash) ;;
    monitor) cmd=(-p "$port" flash monitor) ;;
    *)       usage ;;
esac

if ! command -v idf.py >/dev/null; then
    # export.sh no tolera `set -u`
    set +u
    . "${IDF_PATH:-$HOME/Hackathon/esp-idf}/export.sh" >/dev/null
    set -u
fi

exec idf.py "${args[@]}" "${cmd[@]}"
