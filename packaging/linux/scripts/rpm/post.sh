# Inlet, after installing or upgrading: the service on, started now and at
# every boot; Inlet in PipeWire for every user, from their next PipeWire start.
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || :
    systemctl enable inletd.service >/dev/null 2>&1 || :
    systemctl restart inletd.service >/dev/null 2>&1 || :
fi
systemctl --global enable inlet-pipewire.service >/dev/null 2>&1 || :
