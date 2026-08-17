#!/bin/sh
#
# /usr/sbin/poe_ctl.sh <port> <on|off|cycle>
#
# Точка адаптации демона poe_watchdog под реальный драйвер PoE.
# Реализуйте управление портом через sysfs, /switch или CLI вашего устройства.

set -eu

PORT="${1:?usage: poe_ctl.sh <port> <on|off|cycle>}"
ACTION="${2:?usage: poe_ctl.sh <port> <on|off|cycle>}"

log() {
    logger -t poe_ctl "port=$PORT action=$ACTION: $*"
}

write_value() {
    local path="$1"
    local value="$2"
    if [ -w "$path" ]; then
        echo "$value" > "$path"
        return $?
    fi
    return 1
}

do_off() {
    # Prefer poed socket helper if available
    if command -v /usr/sbin/poe_sock_ctl >/dev/null 2>&1 && [ -e /var/run/poed.sock ]; then
        /usr/sbin/poe_sock_ctl "$PORT" off && return 0
    fi
    # Try /sys/poe* control (switch app exposed interface).
    # The platform exposes port_info with lines like: "0 G24 off ..." — some interfaces
    # expect the port name (G24) rather than numeric id. Try several candidates.
    for d in /sys/poe*; do
        if [ -d "$d" ]; then
            # direct per-dir files
            if [ -w "$d/port_power_off" ]; then
                echo "$PORT" > "$d/port_power_off" 2>/dev/null && return 0 || true
            fi
            if [ -w "$d/port${PORT}_power_off" ]; then
                echo 1 > "$d/port${PORT}_power_off" 2>/dev/null && return 0 || true
            fi

            # try mapping numeric -> name using port_info
            if [ -r "$d/port_info" ]; then
                while read -r idx name rest; do
                    # try exact index match (0-based)
                    if [ "$idx" = "$PORT" ]; then
                        echo "$name" > "$d/port_power_off" 2>/dev/null && return 0 || true
                    fi
                    # try 1-based index match
                    one_based=$((idx + 1))
                    if [ "$one_based" = "$PORT" ]; then
                        echo "$name" > "$d/port_power_off" 2>/dev/null && return 0 || true
                    fi
                    # try name match
                    if [ "$name" = "$PORT" ]; then
                        echo "$name" > "$d/port_power_off" 2>/dev/null && return 0 || true
                    fi
                done < "$d/port_info"
            fi
        fi
    done
    if write_value "/sys/class/poe/poe${PORT}/enable" 0; then
        return 0
    fi

    if write_value "/sys/class/poe/poe${PORT}/power" 0; then
        return 0
    fi

    if write_value "/switch/poe/port${PORT}/power" 0; then
        return 0
    fi

    if command -v cli >/dev/null 2>&1; then
        cli -c "interface poe ${PORT}" -c "poe disable" -c "commit"
        return $?
    fi

    if command -v swcfg >/dev/null 2>&1; then
        swcfg poe set port "${PORT}" state off
        return $?
    fi

    if command -v poectl >/dev/null 2>&1; then
        poectl port "${PORT}" off
        return $?
    fi

    log "Не найден поддерживаемый интерфейс управления PoE-портом для отключения"
    return 1
}

do_on() {
    # Prefer poed socket helper if available
    if command -v /usr/sbin/poe_sock_ctl >/dev/null 2>&1 && [ -e /var/run/poed.sock ]; then
        /usr/sbin/poe_sock_ctl "$PORT" on && return 0
    fi
    # Try /sys/poe* control (switch app exposed interface).
    for d in /sys/poe*; do
        if [ -d "$d" ]; then
            if [ -w "$d/port_power_on" ]; then
                echo "$PORT" > "$d/port_power_on" 2>/dev/null && return 0 || true
            fi
            if [ -w "$d/port${PORT}_power_on" ]; then
                echo 1 > "$d/port${PORT}_power_on" 2>/dev/null && return 0 || true
            fi

            if [ -r "$d/port_info" ]; then
                while read -r idx name rest; do
                    if [ "$idx" = "$PORT" ]; then
                        echo "$name" > "$d/port_power_on" 2>/dev/null && return 0 || true
                    fi
                    one_based=$((idx + 1))
                    if [ "$one_based" = "$PORT" ]; then
                        echo "$name" > "$d/port_power_on" 2>/dev/null && return 0 || true
                    fi
                    if [ "$name" = "$PORT" ]; then
                        echo "$name" > "$d/port_power_on" 2>/dev/null && return 0 || true
                    fi
                done < "$d/port_info"
            fi
        fi
    done
    if write_value "/sys/class/poe/poe${PORT}/enable" 1; then
        return 0
    fi

    if write_value "/sys/class/poe/poe${PORT}/power" 1; then
        return 0
    fi

    if write_value "/switch/poe/port${PORT}/power" 1; then
        return 0
    fi

    if command -v cli >/dev/null 2>&1; then
        cli -c "interface poe ${PORT}" -c "poe enable" -c "commit"
        return $?
    fi

    if command -v swcfg >/dev/null 2>&1; then
        swcfg poe set port "${PORT}" state on
        return $?
    fi

    if command -v poectl >/dev/null 2>&1; then
        poectl port "${PORT}" on
        return $?
    fi

    log "Не найден поддерживаемый интерфейс управления PoE-портом для включения"
    return 1
}

case "$ACTION" in
    off)
        do_off && log "питание выключено" && exit 0
        ;;
    on)
        do_on && log "питание включено" && exit 0
        ;;
    cycle)
        do_off && log "питание выключено (cycle)"
        sleep 3
        do_on && log "питание включено (cycle)" && exit 0
        ;;
    *)
        echo "usage: $0 <port> <on|off|cycle>" >&2
        exit 2
        ;;
esac

exit 1
