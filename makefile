CXX = g++
PYTHON ?= python3

LOG_DIR := /mnt/c/Users/user/Desktop/PROJECT/log/
OUT_DIR := /mnt/c/Users/user/Desktop/PROJECT/test_output

INCLUDE_DIRS := $(wildcard src/*/)
CXXFLAGS = -Wall -Wextra 
CXXFLAGS += $(patsubst %, -I%, $(INCLUDE_DIRS))		# pattern substitude
SRC := $(wildcard src/*.cpp) $(wildcard src/*/*.cpp)



TEST = service
TEST_SRC = $(SRC)

TARGET = $(TEST)



.PHONY: all clean bench-full bench-speedup

all: $(TARGET) 

$(LOG_DIR) $(OUT_DIR):
	mkdir -p $@


$(TEST): $(TEST_SRC) | $(LOG_DIR) $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^


clean:
	rm -rf $(TARGET)
	rm -rf $(LOG_DIR)
	rm -rf $(OUT_DIR)


bench-full: all
	@test -x ./service || (echo "service binary not found. run make first"; exit 1)
	sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
	$(PYTHON) benchmark/benchmark.py --project-root . --bench-config benchmark/bench_config.json --runs-dir /mnt/c/Users/user/Desktop/git-PROJECT-mini/benchmark/runs --store true
