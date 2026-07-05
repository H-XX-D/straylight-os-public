#!/usr/bin/env bash
# install.sh — Deploy StrayLight Eww widgets to ~/.config/eww/straylight/
# Works on both Hyprland and Plasma 6 Wayland sessions.
# Run as the target user (not root).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EWW_DIR="$HOME/.config/eww/straylight"

echo "=== StrayLight Eww Widget Installer ==="
echo "Source : $SCRIPT_DIR"
echo "Target : $EWW_DIR"
echo ""

# ── 1. Check eww is installed ─────────────────────────────────
if ! command -v eww &>/dev/null; then
    echo "eww not found. Install from: https://github.com/elkowar/eww"
    echo ""
    echo "Quick install (Debian/Ubuntu):"
    echo "  apt install cargo"
    echo "  cargo install --locked eww"
    echo "  # or grab a prebuilt from the releases page"
    echo ""
    read -rp "Continue anyway? [y/N] " yn
    [[ "$yn" =~ ^[Yy]$ ]] || exit 1
fi

# ── 2. Check Python 3 is available ───────────────────────────
if ! command -v python3 &>/dev/null; then
    echo "ERROR: python3 is required for probe scripts"
    exit 1
fi

# ── 3. Create target directory ────────────────────────────────
mkdir -p "$EWW_DIR/scripts"

# ── 4. Copy config files ──────────────────────────────────────
cp -v "$SCRIPT_DIR/eww.yuck"  "$EWW_DIR/eww.yuck"
cp -v "$SCRIPT_DIR/eww.scss"  "$EWW_DIR/eww.scss"

# ── 5. Install probe scripts (chmod +x) ──────────────────────
for probe in probe-system probe-ml probe-hpc probe-research probe-notify probe-network; do
    if [[ -f "$SCRIPT_DIR/scripts/$probe" ]]; then
        cp -v "$SCRIPT_DIR/scripts/$probe" "$EWW_DIR/scripts/$probe"
        chmod +x "$EWW_DIR/scripts/$probe"
    fi
done

# ── 6. Validate probes run without crashing ──────────────────
echo ""
echo "=== Validating probe scripts ==="
for probe in probe-system probe-ml probe-hpc probe-research probe-notify probe-network; do
    printf "  %-20s " "$probe"
    if output=$("$EWW_DIR/scripts/$probe" 2>/dev/null); then
        # Must produce valid JSON
        if echo "$output" | python3 -c "import sys,json; json.load(sys.stdin)" &>/dev/null; then
            echo "OK"
        else
            echo "WARNING (invalid JSON output)"
        fi
    else
        echo "WARNING (non-zero exit; service likely offline, will degrade gracefully)"
    fi
done

# ── 7. Write systemd user unit to autostart eww bar ──────────
SYSTEMD_DIR="$HOME/.config/systemd/user"
mkdir -p "$SYSTEMD_DIR"
cat > "$SYSTEMD_DIR/straylight-eww.service" <<'EOF'
[Unit]
Description=StrayLight Eww Desktop Widgets
After=graphical.target
PartOf=graphical.target

[Service]
Type=simple
ExecStart=/usr/bin/env eww --config %h/.config/eww/straylight daemon
ExecStartPost=/usr/bin/env eww --config %h/.config/eww/straylight open system-bar
Restart=on-failure
RestartSec=3

Environment=XDG_RUNTIME_DIR=/run/user/%U

[Install]
WantedBy=graphical.target
EOF

echo ""
echo "=== Systemd user service written ==="
echo "Enable with:"
echo "  systemctl --user daemon-reload"
echo "  systemctl --user enable --now straylight-eww"

# ── 8. Write Hyprland autostart snippet ──────────────────────
HYPR_CONF="$HOME/.config/hypr/straylight-eww.conf"
cat > "$HYPR_CONF" <<'EOF'
# StrayLight Eww bar — source this from hyprland.conf:
#   source = ~/.config/hypr/straylight-eww.conf

exec-once = eww --config ~/.config/eww/straylight daemon
exec-once = eww --config ~/.config/eww/straylight open system-bar

# Toggle keybinds
bind = SUPER, F2, exec, eww --config ~/.config/eww/straylight update show_gpu=$(eww --config ~/.config/eww/straylight get show_gpu | grep -q true && echo false || echo true) && eww --config ~/.config/eww/straylight $(eww --config ~/.config/eww/straylight get show_gpu | grep -q true && echo close || echo open) gpu-hud
bind = SUPER, F3, exec, eww --config ~/.config/eww/straylight update show_hpc=$(eww --config ~/.config/eww/straylight get show_hpc | grep -q true && echo false || echo true) && eww --config ~/.config/eww/straylight $(eww --config ~/.config/eww/straylight get show_hpc | grep -q true && echo close || echo open) hpc-panel
bind = SUPER, F4, exec, eww --config ~/.config/eww/straylight update show_research=$(eww --config ~/.config/eww/straylight get show_research | grep -q true && echo false || echo true) && eww --config ~/.config/eww/straylight $(eww --config ~/.config/eww/straylight get show_research | grep -q true && echo close || echo open) research-panel
EOF
echo ""
echo "=== Hyprland config snippet written to $HYPR_CONF ==="
echo "Add to ~/.config/hypr/hyprland.conf:"
echo "  source = ~/.config/hypr/straylight-eww.conf"

# ── 9. Done ───────────────────────────────────────────────────
echo ""
echo "=== Installation complete ==="
echo ""
echo "Start immediately:"
echo "  eww --config ~/.config/eww/straylight daemon &"
echo "  eww --config ~/.config/eww/straylight open system-bar"
echo ""
echo "Toggle panels:"
echo "  Super+F2  — GPU HUD"
echo "  Super+F3  — HPC Cluster"
echo "  Super+F4  — Research / SNN"
echo ""
echo "The heavy visualisers (SNN spike raster, quantum circuit) open"
echo "as separate ImGui windows via straylight-widget-launcher."
