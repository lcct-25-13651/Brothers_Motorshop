# RE Parts Tracker - build with:  make            (g++ -std=c++17)
#                    self-test:   make test       (no httplib needed)
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
SRC       = src/main.cpp src/web.cpp src/auth.cpp src/inventory.cpp src/reports.cpp src/util.cpp
CORE      = src/auth.cpp src/inventory.cpp src/reports.cpp src/util.cpp

ifeq ($(OS),Windows_NT)
  EXE      = .exe
  CXXFLAGS += -D_WIN32_WINNT=0x0A00 -DNOMINMAX
  LDLIBS   = -lws2_32
else
  EXE      =
  LDLIBS   = -pthread
endif

TARGET = re_parts_tracker$(EXE)

all: $(TARGET)

third_party/httplib.h:
	@echo "Missing third_party/httplib.h - download the single header from"
	@echo "  https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h"
	@echo "and save it as third_party/httplib.h, then run make again."
	@exit 1

$(TARGET): $(SRC) $(wildcard src/*.h) third_party/httplib.h
	$(CXX) $(CXXFLAGS) -Ithird_party $(SRC) -o $@ $(LDLIBS)

test: tests/smoke_test.cpp $(CORE) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) tests/smoke_test.cpp $(CORE) -o smoke_test$(EXE)
	./smoke_test$(EXE)

# Web layer end-to-end test: runs web.cpp against an in-process stand-in for httplib (tests/stub), no sockets.
e2e: tests/web_e2e.cpp src/web.cpp $(CORE) $(wildcard src/*.h) tests/stub/httplib.h
	$(CXX) $(CXXFLAGS) -Isrc -Itests/stub tests/web_e2e.cpp src/web.cpp $(CORE) -o web_e2e$(EXE)
	./web_e2e$(EXE)

# Imports the client's supplier-receipt parts into ./parts.csv (safe to re-run, see tools/seed_data.cpp).
seed: tools/seed_data.cpp
	$(CXX) $(CXXFLAGS) tools/seed_data.cpp -o seed_data$(EXE)
	./seed_data$(EXE)

clean:
	rm -f $(TARGET) smoke_test$(EXE) web_e2e$(EXE) seed_data$(EXE)

.PHONY: all test e2e seed clean
