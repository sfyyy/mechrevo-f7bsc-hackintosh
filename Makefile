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
	bash -n macos/nullarc/nullarc-0.4.0-alpha/install/vcn/install-vcn.sh
	bash -n macos/nullarc/nullarc-0.4.0-alpha/install/vcn/rollback-vcn.sh
	bash -n macos/nullarc/nullarc-0.4.0-alpha/install/vcn/vcn-test.sh
	bash -n macos/nullarc/nullarc-0.4.0-alpha/tools/va-load-probe/build.sh
	python3 -c 'compile(open("macos/nullarc/nullarc-0.4.0-alpha/tools/vcn/perf-capture.py", encoding="utf-8").read(), "perf-capture.py", "exec")'
