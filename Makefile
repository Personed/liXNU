# SPDX-License-Identifier: GPL-2.0-only
PREFIX ?= $(if $(filter 0,$(shell id -u)),/usr/local,$(HOME)/.local)

.PHONY: help install uninstall doctor test

help:
	@echo "Targets: install, uninstall, doctor, test"
	@echo "Default PREFIX: /usr/local as root; $(HOME)/.local otherwise"
	@echo "Use the same PREFIX=... for install and uninstall; set PREFIX=... to override the default."

install:
	@bash scripts/install.sh --prefix "$(PREFIX)"

uninstall:
	@bash scripts/install.sh --prefix "$(PREFIX)" --uninstall

doctor:
	@bash scripts/xnulinux doctor

test:
	@bash scripts/xnulinux test
