# The bench harness's own check (tools/host_checks.sh runs it): radios-ft8 against two simulated radios, the
# watchdog's stops on a radio that stays keyed, a bad SWR, too much power and a receiver hearing too much. Python 3
# and sockets on this computer only: nothing on a radio, nothing on the air.

test:
	python3 -B sim_test.py

.PHONY: test
