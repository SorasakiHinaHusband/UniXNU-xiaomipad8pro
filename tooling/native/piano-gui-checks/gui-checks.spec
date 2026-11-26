# Main Mac, current installed boot. Source this block with run-installed.sh.
steps() {
    local failed=0
    note "Checking installed Wi-Fi menu and Control Center Display on boot $GUI_BOOT"
    python3 "$GUI_TOOLS/run.py" ax-trust || return 2
    if ! fexpect ax-trust.txt '^GUI \{"trusted":true\}$'; then
        verdict 'VOID console-user Accessibility receipt missing'
        return 2
    fi
    verdict 'PASS console-user Accessibility verified'
    python3 "$GUI_TOOLS/run.py" wifi || failed=1
    python3 "$GUI_TOOLS/run.py" brightness-ax || failed=1
    python3 "$GUI_TOOLS/run.py" brightness-hid || failed=1
    return "$failed"
}
