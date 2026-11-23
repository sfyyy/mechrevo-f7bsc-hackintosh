.PHONY: check check-docs check-shell

check: check-docs check-shell

check-docs:
	python3 tools/check_docs.py

check-shell:
	bash -n linux/deploy-efi.sh
	bash -n macos/check.sh
	bash -n macos/display/install.sh
	bash -n macos/thermal/install.sh
	bash -n macos/thermal/uninstall.sh
	python3 -c 'compile(open("macos/nullarc/vcn/perf-capture.py", encoding="utf-8").read(), "perf-capture.py", "exec")'
