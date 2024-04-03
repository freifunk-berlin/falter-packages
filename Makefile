
LUAPATH = $(HOME)/.luarocks/bin
PIPPATH = $(HOME)/.local/bin

help:
	@echo
	@echo 'Available commands:'
	@echo '  make deps - Install the prerequisite code analysis tools.'
	@echo '  make fmt - Apply automatic code formatting. (shell)'
	@echo '  make lint - Check for frequent issues in code. (shell, lua)'
	@echo
.PHONY: help

deps:
	luarocks --local install luacheck
	pip install black isort pylint flake8
	@echo
	@echo 'Done. Please also install manually with apt or dnf: shellcheck shfmt gitleaks'
	@echo
.PHONY: deps

fmt: shfmt # TODO: black isort
.PHONY: fmt

lint: shellcheck luacheck # TODO: gitleaks flake8 pylint
.PHONY: lint

shfmt:
	shfmt -ln=mksh -l ./luci/ ./packages/ | xargs shfmt -w -l -ln=mksh -i 4 -ci -bn
.PHONY: shfmt

shellcheck:
	# The excluded cases are supported by Busybox but not POSIX.
	# TODO update to shellcheck 0.10 for busybox sh support
	grep -rIzl '^#![[:blank:]]*/bin/sh' ./packages ./luci | xargs shellcheck -S warning
.PHONY: shellcheck

luacheck:
	$(LUAPATH)/luacheck ./packages ./luci -q -o 'nowarningsplease'
.PHONY: luacheck
