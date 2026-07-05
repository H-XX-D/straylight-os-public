# StrayLight Network Notes

The public network path documents XDP/eBPF as a datapath surface, not as a
kernel module.

- `xdp_stats` is pass-through and suitable for initial stand-up.
- `xdp_filter` and `xdp_redirect` require explicit packet policy before use.
- Attach XDP through an explicit systemd template such as
  `straylight-xdp@<iface>.service`.
- Public docs should use `<switch-facing-interface>` placeholders instead of
  private interface names.
