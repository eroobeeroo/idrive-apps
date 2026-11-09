// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 eroobeeroo
/* The layout's map window: Cluster Layouts (its Waze centre) writes
 * /dev/shm/cluster-info/map_window.txt every second while its layout shows,
 * one line "panel X Y W H", the window it leaves for the map on the whole
 * panel. While that file is fresh (written in the last kFreshS seconds) the
 * cluster's map goes to the panel target at that rect, above the layout
 * (kWindowZ), so it fills the window: in the map slot under the layout the
 * KOMBI draws its own map screen over the map (its speed readout, edge
 * shading and vignette). Gone or stale, the map goes back to its own place. */
#ifndef WOC_LAYOUT_WINDOW_H
#define WOC_LAYOUT_WINDOW_H

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include <string>

#include "map_rect.h"

namespace woc {

const int kWindowZ = 900;     /* over every layer of a layout (theirs are -210 to 20) */
const long kFreshS = 3;

/* The window in `path` when it is fresh and usable, else false. */
inline bool read_layout_window(const std::string &path, MapRect *out)
{
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) != 0)
        return false;
    const long age = (long)(time(nullptr) - st.st_mtime);
    if (age > kFreshS || age < -kFreshS)
        return false;
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    char target[16] = {0};
    unsigned x, y, w, h;
    const int n = fscanf(f, "%15s %u %u %u %u", target, &x, &y, &w, &h);
    fclose(f);
    if (n != 5 || std::string(target) != "panel")
        return false;
    MapRect r;
    char text[64];
    snprintf(text, sizeof text, "%u,%u,%u,%u", x, y, w, h);
    if (parse_map_rect(text, &r) || !r.fits(1920, 720))
        return false;
    *out = r;
    return true;
}

inline bool same_rect(const MapRect &a, const MapRect &b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

}  // namespace woc

#endif
