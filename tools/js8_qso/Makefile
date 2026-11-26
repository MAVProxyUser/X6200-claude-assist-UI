# Host test of the JS8 QSO book (src/js8/qso_book.cpp): a two-way QSO offered for the log once at its end, a later
# exchange with the same station a QSO of its own. `make` builds and runs it.
TOP      := ../..
SRC      := $(TOP)/src/js8
JS8CORE  := $(TOP)/third-party/js8core
CXXFLAGS := -std=gnu++20 -Wall -Wextra -O1 -g -I $(SRC) -I $(JS8CORE)/include -I $(JS8CORE)/src -I $(JS8CORE)
# msg_kind.cpp's needs: frame_text.cpp's call test, js8core's varicode and the JSC its words go through
CORE     := $(SRC)/frame_text.cpp $(JS8CORE)/src/protocol/varicode.cpp $(JS8CORE)/src/protocol/jsc.cpp \
            $(JS8CORE)/src/protocol/jsc_list.cpp $(JS8CORE)/src/protocol/jsc_map.cpp

all: test

qso_test: qso_test.cpp $(SRC)/qso_book.cpp $(SRC)/qso_book.hpp $(SRC)/msg_kind.cpp $(SRC)/msg_kind.hpp
	$(CXX) $(CXXFLAGS) -o $@ qso_test.cpp $(SRC)/qso_book.cpp $(SRC)/msg_kind.cpp $(CORE)

test: qso_test
	./qso_test

clean:
	rm -f qso_test

.PHONY: all test clean
