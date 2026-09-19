#!/bin/sh
# ==============================================================================
# MSM8916 OpenWrt LTE Mode Switcher for MikroTik RouterOS & OpenWrt Router
# https://github.com/hungngit2/home-network
# ==============================================================================

set -u

MODEM_PORT=""
CURRENT_MODE=""

# ------------------------------------------------------------
# Find Modem AT port
# ------------------------------------------------------------
find_modem_at_port() {
    for port in /dev/wwan0at1 /dev/wwan0at0 /dev/smd11 /dev/smd7 /dev/ttyUSB1 /dev/ttyUSB2; do
        if [ -c "$port" ]; then
            echo "$port"
            return 0
        fi
    done
    return 1
}

# ------------------------------------------------------------
# Send AT command to modem directly
# ------------------------------------------------------------
send_at_cmd() {
    local port="$1"
    local command="$2"
    local wait_sec="${3:-2}"
    local out_tmp="/tmp/at_out.$$"

    if [ ! -c "$port" ]; then
        echo "ERROR: Port $port not accessible"
        return 1
    fi

    exec 3<>"$port" 2>/dev/null || {
        echo "ERROR: Failed to open $port"
        return 1
    }

    cat <&3 >"$out_tmp" 2>/dev/null &
    local cat_pid=$!
    usleep 100000 2>/dev/null || sleep 1

    printf '%s\r\n' "$command" >&3

    sleep "$wait_sec"
    kill "$cat_pid" 2>/dev/null || true
    exec 3<&- 2>/dev/null || true
    exec 3>&- 2>/dev/null || true

    if [ -f "$out_tmp" ]; then
        cat "$out_tmp" | tr -d '\r'
        rm -f "$out_tmp"
    fi
}

# ------------------------------------------------------------
# Detect current USB Gadget configuration
# ------------------------------------------------------------
detect_current_mode() {
    local vid=""
    local pid=""
    local acm_mode=""
    local acm_shell=""

    if [ -f /etc/config/usbgadget ]; then
        vid="$(uci -q get usbgadget.usb.vendor_id 2>/dev/null || echo '')"
        pid="$(uci -q get usbgadget.usb.product_id 2>/dev/null || echo '')"
        acm_mode="$(uci -q get usbgadget.acm.mode 2>/dev/null || echo '')"
        acm_shell="$(uci -q get usbgadget.acm.shell 2>/dev/null || echo '1')"
    fi

    if [ -d /sys/kernel/config/usb_gadget/g1 ]; then
        vid="$(cat /sys/kernel/config/usb_gadget/g1/idVendor 2>/dev/null || echo "$vid")"
        pid="$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null || echo "$pid")"
    fi

    case "${vid}:${pid}" in
        0x1bc7:0x1901|1bc7:1901)
            CURRENT_MODE="RouterOS Passthrough (Telit LN940 [1bc7:1901])"
            ;;
        0x2c7c:0x0125|2c7c:0125)
            CURRENT_MODE="RouterOS Passthrough (Quectel EC25 [2c7c:0125])"
            ;;
        0x05c6:0x9025|05c6:9025)
            CURRENT_MODE="RouterOS Passthrough (Qualcomm LTE [05c6:9025])"
            ;;
        0x1d6b:0x0104|1d6b:0104)
            if [ "$acm_mode" = "modem_bridge" ] || [ "$acm_shell" = "0" ]; then
                CURRENT_MODE="RouterOS Passthrough (Generic CDC [1d6b:0104])"
            else
                CURRENT_MODE="OpenWrt Router Mode (Linux CDC [1d6b:0104])"
            fi
            ;;
        *)
            if [ -n "$vid" ] && [ -n "$pid" ]; then
                CURRENT_MODE="Custom ($vid:$pid)"
            else
                CURRENT_MODE="Not configured / Inactive"
            fi
            ;;
    esac
}

# ------------------------------------------------------------
# Show status & modem diagnostics
# ------------------------------------------------------------
show_status() {
    detect_current_mode
    local modem_port
    modem_port="$(find_modem_at_port || true)"

    echo
    echo "==================== System Status ===================="
    echo "Current Mode   : $CURRENT_MODE"

    if [ -f /etc/config/usbgadget ]; then
        local vid pid mfg prod
        vid="$(uci -q get usbgadget.usb.vendor_id)"
        pid="$(uci -q get usbgadget.usb.product_id)"
        mfg="$(uci -q get usbgadget.usb.manufacturer)"
        prod="$(uci -q get usbgadget.usb.product)"
        echo "USB VID:PID    : ${vid}:${pid} ($mfg - $prod)"
    fi

    # AT bridge daemon status
    if pidof modem_at_bridge >/dev/null 2>&1; then
        echo "AT Bridge      : RUNNING (PID: $(pidof modem_at_bridge))"
    else
        echo "AT Bridge      : STOPPED"
    fi

    # ModemManager status
    if pidof ModemManager >/dev/null 2>&1; then
        echo "ModemManager   : RUNNING (OpenWrt managing LTE)"
    else
        echo "ModemManager   : STOPPED (Host/RouterOS has direct control)"
    fi

    echo "Modem AT Port  : ${modem_port:-NOT FOUND}"
    echo "Gadget Port    : /dev/ttyGS0"

    # Live AT Query if port is available
    if [ -n "$modem_port" ] && [ -c "$modem_port" ]; then
        echo
        echo "--- Modem Information (AT Query) ---"
        local at_resp
        at_resp="$(send_at_cmd "$modem_port" "ATI" 1 || true)"
        if [ -n "$at_resp" ]; then
            echo "$at_resp" | sed 's/^/  /'
        fi

        echo
        echo "--- Signal Strength (AT+CSQ) ---"
        send_at_cmd "$modem_port" "AT+CSQ" 1 | sed 's/^/  /'

        echo "--- SIM Status (AT+CPIN?) ---"
        send_at_cmd "$modem_port" "AT+CPIN?" 1 | sed 's/^/  /'

        echo "--- Network Registration (AT+CEREG?) ---"
        send_at_cmd "$modem_port" "AT+CEREG?" 1 | sed 's/^/  /'

        echo "--- Operator Info (AT+COPS?) ---"
        send_at_cmd "$modem_port" "AT+COPS?" 1 | sed 's/^/  /'
    fi
    echo "======================================================="
    echo
}

# ------------------------------------------------------------
# Set Passthrough Mode
# ------------------------------------------------------------
apply_mode() {
    local target="$1"

    echo
    case "$target" in
        telit)
            echo ">>> Switching to RouterOS Passthrough Mode (Telit LN940 [1bc7:1901])..."
            uci set usbgadget.usb.enabled='1'
            uci set usbgadget.usb.vendor_id='0x1bc7'
            uci set usbgadget.usb.product_id='0x1901'
            uci set usbgadget.usb.manufacturer='Telit'
            uci set usbgadget.usb.product='LN940 Mobile Broadband'
            uci set usbgadget.ncm.enabled='1'
            uci set usbgadget.ecm.enabled='0'
            uci set usbgadget.rndis.enabled='0'
            uci set usbgadget.acm.enabled='1'
            uci set usbgadget.acm.shell='0'
            uci set usbgadget.acm.mode='modem_bridge'
            uci commit usbgadget

            # Stop OpenWrt ModemManager so RouterOS has exclusive AT access
            /etc/init.d/modemmanager stop 2>/dev/null || true
            /etc/init.d/modemmanager disable 2>/dev/null || true

            # Restart USB Gadget
            /etc/init.d/usb-gadget restart

            # Enable and start AT bridge
            /etc/init.d/modem-at-bridge enable 2>/dev/null || true
            /etc/init.d/modem-at-bridge restart 2>/dev/null || true
            ;;
        quectel)
            echo ">>> Switching to RouterOS Passthrough Mode (Quectel EC25 [2c7c:0125])..."
            uci set usbgadget.usb.enabled='1'
            uci set usbgadget.usb.vendor_id='0x2c7c'
            uci set usbgadget.usb.product_id='0x0125'
            uci set usbgadget.usb.manufacturer='Quectel'
            uci set usbgadget.usb.product='EC25 LTE Modem'
            uci set usbgadget.ncm.enabled='1'
            uci set usbgadget.ecm.enabled='0'
            uci set usbgadget.rndis.enabled='0'
            uci set usbgadget.acm.enabled='1'
            uci set usbgadget.acm.shell='0'
            uci set usbgadget.acm.mode='modem_bridge'
            uci commit usbgadget

            /etc/init.d/modemmanager stop 2>/dev/null || true
            /etc/init.d/modemmanager disable 2>/dev/null || true
            /etc/init.d/usb-gadget restart
            /etc/init.d/modem-at-bridge enable 2>/dev/null || true
            /etc/init.d/modem-at-bridge restart 2>/dev/null || true
            ;;
        qualcomm)
            echo ">>> Switching to RouterOS Passthrough Mode (Qualcomm LTE [05c6:9025])..."
            uci set usbgadget.usb.enabled='1'
            uci set usbgadget.usb.vendor_id='0x05c6'
            uci set usbgadget.usb.product_id='0x9025'
            uci set usbgadget.usb.manufacturer='Qualcomm'
            uci set usbgadget.usb.product='Qualcomm LTE Modem'
            uci set usbgadget.ncm.enabled='1'
            uci set usbgadget.ecm.enabled='0'
            uci set usbgadget.rndis.enabled='0'
            uci set usbgadget.acm.enabled='1'
            uci set usbgadget.acm.shell='0'
            uci set usbgadget.acm.mode='modem_bridge'
            uci commit usbgadget

            /etc/init.d/modemmanager stop 2>/dev/null || true
            /etc/init.d/modemmanager disable 2>/dev/null || true
            /etc/init.d/usb-gadget restart
            /etc/init.d/modem-at-bridge enable 2>/dev/null || true
            /etc/init.d/modem-at-bridge restart 2>/dev/null || true
            ;;
        openwrt)
            echo ">>> Switching to Standard OpenWrt Router Mode..."
            uci set usbgadget.usb.enabled='1'
            uci set usbgadget.usb.vendor_id='0x1d6b'
            uci set usbgadget.usb.product_id='0x0104'
            uci set usbgadget.usb.manufacturer='OpenWrt'
            uci set usbgadget.usb.product='USB Gadget'
            uci set usbgadget.ncm.enabled='1'
            uci set usbgadget.acm.enabled='1'
            uci set usbgadget.acm.shell='1'
            uci set usbgadget.acm.mode='shell'
            uci commit usbgadget

            /etc/init.d/modem-at-bridge stop 2>/dev/null || true
            /etc/init.d/modem-at-bridge disable 2>/dev/null || true
            /etc/init.d/usb-gadget restart

            /etc/init.d/modemmanager enable 2>/dev/null || true
            /etc/init.d/modemmanager start 2>/dev/null || true
            ;;
        *)
            echo "Invalid mode specified: $target"
            return 1
            ;;
    esac

    echo
    echo "Mode applied successfully. The USB device is re-enumerating on host..."
    sleep 3
    show_status
}

read_input() {
    local prompt="$1"
    local var_name="$2"

    if [ -t 0 ]; then
        read -r -p "$prompt" "$var_name"
    elif [ -r /dev/tty ]; then
        read -r -p "$prompt" "$var_name" < /dev/tty
    else
        echo "$prompt"
        return 1
    fi
}

# ------------------------------------------------------------
# Command Line Argument Handling
# ------------------------------------------------------------
if [ $# -gt 0 ]; then
    case "$1" in
        --routeros|--telit|-1)
            apply_mode telit
            exit 0
            ;;
        --quectel|-2)
            apply_mode quectel
            exit 0
            ;;
        --qualcomm|-3)
            apply_mode qualcomm
            exit 0
            ;;
        --openwrt|--router|-4)
            apply_mode openwrt
            exit 0
            ;;
        --status|-s)
            show_status
            exit 0
            ;;
        --at)
            shift
            port="$(find_modem_at_port || true)"
            if [ -z "$port" ]; then
                echo "Modem AT port not found"
                exit 1
            fi
            send_at_cmd "$port" "$*" 2
            exit 0
            ;;
        --help|-h)
            echo "Usage: $0 [OPTION]"
            echo "Options:"
            echo "  -1, --telit, --routeros  Switch to RouterOS Passthrough (Telit LN940 [1bc7:1901])"
            echo "  -2, --quectel            Switch to RouterOS Passthrough (Quectel EC25 [2c7c:0125])"
            echo "  -3, --qualcomm           Switch to RouterOS Passthrough (Qualcomm LTE [05c6:9025])"
            echo "  -4, --openwrt, --router  Switch to OpenWrt Router Mode  (Linux CDC [1d6b:0104])"
            echo "  -s, --status             Show current status and modem diagnostics"
            echo "  --at <command>           Send direct AT command to modem"
            echo "  -h, --help               Show this help message"
            exit 0
            ;;
    esac
fi

# ------------------------------------------------------------
# Interactive Menu Loop
# ------------------------------------------------------------
while true; do
    detect_current_mode

    echo "========================================================"
    echo "       MSM8916 OpenWrt LTE Mode Switcher (RouterOS)    "
    echo "========================================================"
    echo " Active Mode: $CURRENT_MODE"
    echo
    echo " 1) RouterOS Passthrough Mode (Telit LN940: 1bc7:1901)"
    echo " 2) RouterOS Passthrough Mode (Quectel EC25: 2c7c:0125)"
    echo " 3) RouterOS Passthrough Mode (Qualcomm LTE: 05c6:9025)"
    echo " 4) Standard OpenWrt Router Mode (Linux Default: 1d6b:0104)"
    echo " 5) Show Detailed Status & Modem Diagnostics"
    echo " 6) Send Custom AT Command"
    echo " 0) Exit"
    echo "========================================================"

    read_input "Select option [0-6]: " choice || {
        echo "Non-interactive shell. Showing status and exiting:"
        show_status
        exit 0
    }

    case "$choice" in
        1)
            apply_mode telit
            read_input "Press Enter to continue..." dummy || true
            ;;
        2)
            apply_mode quectel
            read_input "Press Enter to continue..." dummy || true
            ;;
        3)
            apply_mode qualcomm
            read_input "Press Enter to continue..." dummy || true
            ;;
        4)
            apply_mode openwrt
            read_input "Press Enter to continue..." dummy || true
            ;;
        5)
            show_status
            read_input "Press Enter to continue..." dummy || true
            ;;
        6)
            read_input "Enter AT command (e.g. AT+CSQ): " at_input || true
            if [ -n "$at_input" ]; then
                port="$(find_modem_at_port || true)"
                if [ -n "$port" ]; then
                    echo "Sending: $at_input"
                    send_at_cmd "$port" "$at_input" 2
                else
                    echo "Modem AT port not found."
                fi
            fi
            read_input "Press Enter to continue..." dummy || true
            ;;
        0)
            echo "Exiting."
            exit 0
            ;;
        *)
            echo "Invalid option."
            sleep 1
            ;;
    esac
done
