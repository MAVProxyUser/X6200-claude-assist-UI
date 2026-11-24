# Host test of the JS8 app's crash of 2026-11-23 (docs/security/js8-crash-2026-11-23.md): a popup deleted later
# (lv_obj_del_async) and its window closed before LVGL's timers ran, the delete then on freed memory. Built with
# AddressSanitizer and UndefinedBehaviorSanitizer, the GUI's own LVGL (lvgl/, compiled here) and lv_conf.h.
#   1. the windows that defer deletes go through obj_later_del() (src/obj_later.c), never lv_obj_del_async()
#   2. the old way, the bench's "js8 stop" + "js8 close": AddressSanitizer must stop it (heap-use-after-free)
#   3. the same with obj_later_del(), and the helper's other cases: no sanitizer report
# `make` builds and runs it (LVGL compiled with the sanitizers in parallel: seconds on a Mac, under a minute in CI).
TOP     := ../..
SRC     := $(TOP)/src
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer
DEFS    := -DLV_CONF_INCLUDE_SIMPLE -DBOARD_DISP_W=800 -DBOARD_DISP_H=480 -DLV_DRAW_NEON=0
CFLAGS  := -std=gnu11 -O1 -g -fno-inline -fno-optimize-sibling-calls $(SAN) $(DEFS) -I $(TOP) -I $(SRC)
JOBS    := $(shell nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 4)
# No leak report: LVGL keeps its display and styles to the end of the program
RUN_ENV := ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1
# The windows that delete their popups or maps later
WINDOWS := $(SRC)/js8_window.c $(SRC)/ft8_window.c $(SRC)/dialog_dmr.c

all: test

# LVGL with the sanitizers, its files in parallel
obj/liblvgl_san.a: $(TOP)/lv_conf.h
	mkdir -p obj/lvgl
	find $(TOP)/lvgl/src -name '*.c' | while read -r f; do \
	    echo "$$f obj/lvgl/$$(echo "$$f" | sed 's|.*/lvgl/src/||; s|/|_|g; s|\.c$$|.o|')"; \
	done | xargs -n 2 -P $(JOBS) sh -c '$(CC) $(CFLAGS) -w -c "$$0" -o "$$1"'
	rm -f $@
	ar rcs $@ obj/lvgl/*.o 2>&1 | grep -v "has no symbols" || true

js8_close_uaf_test: js8_close_uaf_test.c $(SRC)/obj_later.c $(SRC)/obj_later.h obj/liblvgl_san.a
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ js8_close_uaf_test.c $(SRC)/obj_later.c obj/liblvgl_san.a -lm

test: js8_close_uaf_test
	@bad=$$(grep -n 'lv_obj_del_async(' $(WINDOWS) | grep -v '^[^:]*:[0-9]*: *\(/\*\|\*\)'); \
	if [ -n "$$bad" ]; then echo "  FAIL: lv_obj_del_async in a window, obj_later_del instead:"; echo "$$bad"; exit 1; fi; \
	echo "ok: no lv_obj_del_async left in $(notdir $(WINDOWS))"
	@$(RUN_ENV) ./js8_close_uaf_test old > old.log 2>&1; \
	if grep -q 'heap-use-after-free' old.log; then \
	    echo "ok: the old way caught by AddressSanitizer:"; \
	    grep -E 'heap-use-after-free|#[0-9]+ .* in (lv_obj_del|lv_obj_del_async_cb|lv_async_timer_cb|lv_timer_handler|loop_once|bench_sequence_old|close_window|p3_popup_new|lv_list_create) ' old.log | head -n 12 | sed 's/^/    /'; \
	else echo "  FAIL: the old way was not caught (the harness no longer reproduces the crash)"; cat old.log; exit 1; fi
	@$(RUN_ENV) ./js8_close_uaf_test

clean:
	rm -rf obj js8_close_uaf_test js8_close_uaf_test.dSYM old.log

.PHONY: all test clean
