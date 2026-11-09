# camera-inspect

*This fork's own tool (eroobeeroo).* The first step towards a camera app for the X5 (a performance recorder with
the camera picture and the car's data on it): what the head unit's camera inputs are, asked **read only**.

The cameras reach the head unit over its Intel IPU4 CSI-2 inputs (`/dev/video*`, six inputs with four channels
each on an X5 xDrive45e), which only the `video` group may open, and which the car's own camera services
(`dasvideo`, `icam2proxy`) use. So this tool never takes a picture, never changes a setting, and never starts
or stops a stream. For each node it reports:

- the driver, card and bus (which CSI-2 port and channel), and its capabilities;
- the current input, format (size, pixel format, line length) and frame interval, and the formats and sizes it
  offers;
- **which process has it open** (`/proc/<pid>/fd`), so a capture helper can tell a free input from one the car is
  using;
- for subdevices, each pad's active format (a camera's own size on a deserializer), and the media graph's enabled
  links (which camera feeds which node), where the unit has them.

It writes its report as it goes (the start at once, then node by node), so an empty answer means it never ran,
and a crash or its 12 s limit ends the report with a line naming the node it was reading. Every node is opened
read only. It runs per connection: an app (Camera Probe 0.3.0) connects to `/dev/shm/camera-inspect/report.sock` and
reads one JSON report. The service is a dynamic user in the `video` group with `CAP_SYS_PTRACE` only (to read
other processes' fd links), no network, a read-only system.

```
make test     # built here and run on a made-up /dev and /proc
make target   # the unit's binary, built in Docker (Ubuntu 18.04: gcc 7.5, glibc 2.27)
```

GPL-3.0-or-later.
