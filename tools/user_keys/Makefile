# Host check of the USER page's key names (src/button_bar.cpp user_label_of) against every action Settings > Keys
# offers (src/key_settings.cpp): `make` runs it. Upstream's apps (200 on) showed "?" there up to v0.3.12.48.
all: test

test:
	python3 check_labels.py

clean:

.PHONY: all test clean
