# Inlet, before it is removed ($1 is 0; 1 is an upgrade, which leaves them):
# the service stopped and off, and no longer started with PipeWire.
if [ "$1" = 0 ]; then
    if [ -d /run/systemd/system ]; then
        systemctl disable --now inletd.service >/dev/null 2>&1 || :
    fi
    systemctl --global disable inlet-pipewire.service >/dev/null 2>&1 || :
fi
