#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path


APPLICATIONS_DIR = Path("/usr/share/applications")


WAYLAND_APPS = [
    ("straylight-sl-hub", "SL Hub", "StrayLight central control hub", "straylight-hub", "straylight-sl-hub", "System;"),
    ("straylight-sl-health", "SL Health", "Hardware and service health dashboard", "straylight-health-gui", "straylight-sl-health", "System;Monitor;"),
    ("straylight-sl-mesh", "SL Mesh", "Cluster mesh topology", "straylight-mesh-gui", "straylight-sl-mesh", "Network;System;"),
    ("straylight-sl-probe", "SL Probe", "Deep system and hardware probe", "straylight-probe-gui", "straylight-sl-probe", "System;Monitor;"),
    ("straylight-sl-snapshot", "SL Snapshot", "System snapshot and restore", "straylight-snapshot-gui", "straylight-sl-snapshot", "Utility;System;"),
    ("straylight-sl-vault", "SL Vault", "Encrypted secrets and key management", "straylight-vault-gui", "straylight-sl-vault", "Security;System;"),
    ("straylight-sl-bench", "SL Bench", "Hardware and AI benchmark suite", "straylight-bench-gui", "applications-science", "Science;System;"),
    ("straylight-sl-cron", "SL Cron", "Scheduled task manager", "straylight-cron-gui", "x-office-calendar", "Utility;System;"),
    ("straylight-sl-flux", "SL Flux", "Mesh and fabric traffic visualizer", "straylight-flux-gui", "network-wireless", "Network;System;"),
    ("straylight-sl-migrate", "SL Migrate", "Data and workload migration", "straylight-migrate-gui", "drive-harddisk", "System;Utility;"),
    ("straylight-sl-replay", "SL Replay", "Session and event replay", "straylight-replay-gui", "media-playback-start", "Utility;System;"),
    ("straylight-sl-sandbox", "SL Sandbox", "Isolated execution environment", "straylight-sandbox-gui", "security-high-symbolic", "Security;System;"),
    ("straylight-sl-shield", "SL Shield", "Security policy and firewall", "straylight-shield-gui", "security-medium", "Security;System;"),
]


WIDGETS = [
    ("scheduler_view", "Scheduler View", "System", "utilities-system-monitor"),
    ("memory_pressure", "Memory Pressure", "System", "utilities-system-monitor"),
    ("io_latency", "I/O Latency", "System", "utilities-system-monitor"),
    ("entropy_pool", "Entropy Pool", "System", "utilities-system-monitor"),
    ("cpu_topology", "CPU Topology", "System", "utilities-system-monitor"),
    ("snn_visualizer", "SNN Visualizer", "Research", "applications-science"),
    ("quantum_circuit", "Quantum Circuit", "Research", "applications-science"),
    ("paper_notes", "Paper Notes", "Research", "accessories-text-editor"),
    ("experiment_tracker", "Experiment Tracker", "Research", "applications-science"),
    ("dataset_browser", "Dataset Browser", "Research", "system-file-manager"),
    ("training_dashboard", "Training Dashboard", "Machine Learning", "applications-science"),
    ("tensor_inspector", "Tensor Inspector", "Machine Learning", "applications-science"),
    ("model_browser", "Model Browser", "Machine Learning", "system-search"),
    ("inference_monitor", "Inference Monitor", "Machine Learning", "utilities-system-monitor"),
    ("gpu_hud", "GPU HUD", "Machine Learning", "video-display"),
    ("resource_allocator", "Resource Allocator", "HPC", "utilities-system-monitor"),
    ("power_monitor", "Power Monitor", "HPC", "battery"),
    ("network_topology", "Network Topology", "HPC", "network-wired"),
    ("job_queue", "Job Queue", "HPC", "view-list-symbolic"),
    ("cluster_map", "Cluster Map", "HPC", "network-workgroup"),
]


def desktop_entry(
    *,
    name: str,
    comment: str,
    exec_line: str,
    icon: str,
    categories: str,
    startup_wm_class: str | None = None,
) -> str:
    lines = [
        "[Desktop Entry]",
        "Version=1.0",
        "Type=Application",
        f"Name={name}",
        f"Comment={comment}",
        f"Exec={exec_line}",
        f"Icon={icon}",
        f"Categories={categories}",
        "Terminal=false",
        "StartupNotify=true",
        "Keywords=StrayLight;",
    ]
    if startup_wm_class:
        lines.append(f"StartupWMClass={startup_wm_class}")
    lines.append("")
    return "\n".join(lines)


def write(path: Path, content: str) -> None:
    path.write_text(content)
    path.chmod(0o644)


def main() -> None:
    APPLICATIONS_DIR.mkdir(parents=True, exist_ok=True)

    for app_id, name, comment, binary, icon, categories in WAYLAND_APPS:
        write(
            APPLICATIONS_DIR / f"{app_id}.desktop",
            desktop_entry(
                name=name,
                comment=f"{comment} (Debian GUI compatible)",
                exec_line=f"/usr/bin/straylight-debian-launch --wayland /usr/bin/{binary}",
                icon=icon,
                categories=categories,
                startup_wm_class=app_id,
            ),
        )

    for widget_id, display_name, category, icon in WIDGETS:
        app_id = f"straylight-widget-{widget_id.replace('_', '-')}"
        write(
            APPLICATIONS_DIR / f"{app_id}.desktop",
            desktop_entry(
                name=f"SL Widget: {display_name}",
                comment=f"StrayLight {category} observer probe",
                exec_line=f"/usr/bin/straylight-debian-launch --native /usr/bin/straylight-widget-launcher {widget_id}",
                icon=icon,
                categories="System;Monitor;",
                startup_wm_class=f"StrayLight: {display_name}",
            ),
        )

    # Keep firstboot/session helpers out of the GNOME app menu.
    for stale in [
        "straylight-greeter.desktop",
        "straylight-sl-oobe.desktop",
        "straylight-sl-wizard.desktop",
    ]:
        path = APPLICATIONS_DIR / stale
        if path.exists():
            text = path.read_text()
            if "NoDisplay=true" not in text:
                path.write_text(text.rstrip() + "\nNoDisplay=true\n")

    print(f"Installed {len(WAYLAND_APPS)} StrayLight app entries")
    print(f"Installed {len(WIDGETS)} StrayLight widget entries")


if __name__ == "__main__":
    main()
