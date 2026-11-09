# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 eroobeeroo

# camera-inspect: what the head unit's camera inputs are, asked read only (README.md).
#
#   make test     built for this machine and run on a made-up /dev and /proc
#   make target   the head unit's binary in build/target, built in Docker
#                 (Ubuntu 18.04 amd64: gcc 7.5, glibc 2.27, as the unit)

CC      ?= cc
BUILD   ?= build
CFLAGS   = -std=c99 -O2 -g -Wall -Wextra -Werror -Wno-format-truncation
IMAGE   := camera-inspect-build:$(shell cksum < docker/Dockerfile | cut -d' ' -f1)
DOCKER_IMAGE = docker image inspect $(IMAGE) >/dev/null 2>&1 || docker build -q --platform linux/amd64 -t $(IMAGE) docker

.PHONY: test target target-in-docker clean

$(BUILD)/host/camera-inspect: src/camera_inspect.c
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $<

test: $(BUILD)/host/camera-inspect
	sh test/camera_inspect_test.sh $<

target:
	$(DOCKER_IMAGE)
	docker run --rm --platform linux/amd64 --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/w/camera-inspect" -w /w/camera-inspect $(IMAGE) \
		make -s BUILD=$(BUILD) target-in-docker

target-in-docker: $(BUILD)/target/camera-inspect

$(BUILD)/target/camera-inspect: src/camera_inspect.c
	mkdir -p $(@D)
	gcc $(CFLAGS) -o $@ $<
	@! objdump -T $@ | grep -E 'GLIBC_2\.(2[89]|3[0-9])' || { echo "needs a newer glibc than the unit's 2.27"; exit 1; }

clean:
	rm -rf $(BUILD)
