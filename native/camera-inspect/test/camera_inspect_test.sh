#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# camera-inspect on a made-up /dev and /proc: numeric node order, who holds each node, a node that isn't a
# V4L2 device reported (never fatal), the JSON valid, and nothing written to the nodes.
set -eu
BIN=${1:-build/host/camera-inspect}
R=$(mktemp -d)
trap 'rm -rf "$R"' EXIT
mkdir -p "$R/dev" "$R/proc/812/fd" "$R/proc/900/fd" "$R/proc/self-x"
for n in video0 video2 video10 v4l-subdev1 media0; do printf 'unchanged' > "$R/dev/$n"; done
ln -s /dev/video2 "$R/proc/812/fd/7"; echo icam2proxy > "$R/proc/812/comm"
ln -s /dev/null "$R/proc/900/fd/1"; echo bash > "$R/proc/900/comm"
"$BIN" --root "$R" > "$R/out.json"
python3 - "$R" <<'PY'
import json, sys
r = sys.argv[1]
j = json.load(open(r + '/out.json'))
fail = []
def ok(c, m):
    print(('ok   ' if c else 'FAIL ') + m)
    if not c: fail.append(m)
ok(j['tool'].startswith('camera-inspect '), 'reports its version')
ok([v['node'] for v in j['video']] == ['/dev/video0', '/dev/video2', '/dev/video10'], 'video nodes in numeric order')
v2 = j['video'][1]
ok(v2['open_by'] == [{'pid': 812, 'comm': 'icam2proxy'}], 'who holds video2: ' + json.dumps(v2['open_by']))
ok(j['video'][0]['open_by'] == [], 'video0 held by nobody')
ok('querycap_error' in v2 and 'driver' not in v2, 'a file that is no V4L2 node: reported, not fatal')
ok([s['node'] for s in j['subdevs']] == ['/dev/v4l-subdev1'] and [m['node'] for m in j['media']] == ['/dev/media0'], 'subdevices and media nodes listed')
ok(all(open(r + '/dev/' + n).read() == 'unchanged' for n in ('video0', 'video2', 'video10', 'v4l-subdev1', 'media0')), 'nothing written to any node')
sys.exit(1 if fail else 0)
PY
"$BIN" --nope >/dev/null 2>&1 && { echo "FAIL a bad argument accepted"; exit 1; } || echo "ok   a bad argument refused"
echo "camera-inspect: all passed"
