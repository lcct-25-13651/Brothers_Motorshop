# RE Parts Tracker - build with:  make            (g++ -std=c++17)
#                    self-test:   make test       (no httplib needed)
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
SRC       = src/main.cpp src/web.cpp src/api.cpp src/jobs.cpp src/auth.cpp src/inventory.cpp src/reports.cpp src/util.cpp
CORE      = src/auth.cpp src/inventory.cpp src/reports.cpp src/jobs.cpp src/util.cpp

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

# Phase 4 core: job orders, stock deduction/return, state machine, concurrency, crash recovery.
jobstest: tests/jobs_test.cpp $(CORE) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) -Isrc tests/jobs_test.cpp $(CORE) -o jobs_test$(EXE) $(LDLIBS)
	./jobs_test$(EXE)

# Phase 4 API: /api/* endpoints driven through the in-process httplib stand-in.
apie2e: tests/api_e2e.cpp src/api.cpp $(CORE) $(wildcard src/*.h) tests/stub/httplib.h
	$(CXX) $(CXXFLAGS) -Isrc -Itests/stub tests/api_e2e.cpp src/api.cpp $(CORE) -o api_e2e$(EXE) $(LDLIBS)
	./api_e2e$(EXE)

# Web layer end-to-end test: runs web.cpp against an in-process stand-in for httplib (tests/stub), no sockets.
e2e: tests/web_e2e.cpp src/web.cpp src/api.cpp $(CORE) $(wildcard src/*.h) tests/stub/httplib.h
	$(CXX) $(CXXFLAGS) -Isrc -Itests/stub tests/web_e2e.cpp src/web.cpp src/api.cpp $(CORE) -o web_e2e$(EXE) $(LDLIBS)
	./web_e2e$(EXE)

# Everything, the way CI would run it.
check: test jobstest e2e apie2e

# Imports the client's supplier-receipt parts into ./parts.csv (safe to re-run, see tools/seed_data.cpp).
seed: tools/seed_data.cpp
	$(CXX) $(CXXFLAGS) tools/seed_data.cpp -o seed_data$(EXE)
	./seed_data$(EXE)

clean:
	rm -f $(TARGET) smoke_test$(EXE) web_e2e$(EXE) jobs_test$(EXE) api_e2e$(EXE) seed_data$(EXE)

.PHONY: all test jobstest e2e apie2e check seed clean
