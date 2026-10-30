# Inlet, removed or upgraded: systemd rereads its units.
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || :
fi
