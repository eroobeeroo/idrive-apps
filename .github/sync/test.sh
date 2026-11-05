#!/bin/bash
# The fork's tests (as test.yml, without the head-unit builds), run on an upstream merge before it is offered.
set -e
make -C native/cluster-render test
make -C native/waze-on-cluster test
node apps/robertn_waze_cluster/tools/test_control.js
node apps/robertn_waze_cluster/tools/test_page.js
node apps/robertn_waze_cluster/tools/test_profiles.js
node apps/robertn_waze_cluster/tools/test_i18n.js
node release/test/release_test.js
node release/release.js check
