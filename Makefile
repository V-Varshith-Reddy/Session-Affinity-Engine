# Compiler and flags
CXX        = g++
CXXFLAGS   = -std=c++17 -Wall -Wextra -Wpedantic -O2 -pthread
DEBUG_FLAGS = -g -DDEBUG

# Directories
SRC_DIR    = src
TEST_DIR   = tests
BUILD_DIR  = build
LOG_DIR    = logs

# Source files
SRCS       = $(SRC_DIR)/logger.cpp \
             $(SRC_DIR)/consistent_hash.cpp \
             $(SRC_DIR)/rate_limiter.cpp \
             $(SRC_DIR)/security.cpp \
             $(SRC_DIR)/session_manager.cpp \
             $(SRC_DIR)/request_handler.cpp \
             $(SRC_DIR)/load_balancer.cpp

MAIN_SRC   = $(SRC_DIR)/main.cpp
TEST_SRC   = $(TEST_DIR)/test_all.cpp

# Object files
OBJS       = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
MAIN_OBJ   = $(BUILD_DIR)/main.o
TEST_OBJ   = $(BUILD_DIR)/test_all.o

# Binaries
TARGET     = session_engine
TEST_BIN   = run_tests
DOCKER_BIN = session_engine_docker

# ─────────────────────────────────────────────────────────────
# Build targets
# ─────────────────────────────────────────────────────────────

.PHONY: all clean test run run-test dirs debug docker

## Build the main simulation binary
all: dirs $(TARGET)
	@echo "Build complete: ./$(TARGET)"

## Build the docker standalone binary
docker: dirs $(DOCKER_BIN)
	@echo "Build complete: ./$(DOCKER_BIN)"

## Build and run tests
test: dirs $(TEST_BIN)
	@echo ""
	@echo "Running tests..."
	@echo ""
	@./$(TEST_BIN)

## Build with debug symbols
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: clean all

## Run the simulation
run: all
	./$(TARGET) --logging on --mode sim

## Run tests
run-test: test

## Clean build artifacts
clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TEST_BIN) $(DOCKER_BIN)
	@echo "Cleaned."

# ─────────────────────────────────────────────────────────────
# Internal rules
# ─────────────────────────────────────────────────────────────

## Create necessary directories
dirs:
	@mkdir -p $(BUILD_DIR) $(LOG_DIR)

## Link the main binary
$(TARGET): $(OBJS) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "Linked: $@"

## Link the test binary
$(TEST_BIN): $(OBJS) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "Linked: $@"

## Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

## Link the docker binary
$(DOCKER_BIN): $(OBJS) $(BUILD_DIR)/main1.o
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "Linked: $@"

## Compile main
$(BUILD_DIR)/main.o: $(SRC_DIR)/main.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

## Compile docker main
$(BUILD_DIR)/main1.o: $(SRC_DIR)/main1.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

## Compile tests
$(BUILD_DIR)/test_all.o: $(TEST_DIR)/test_all.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ─────────────────────────────────────────────────────────────
# Dependency tracking (header changes trigger recompilation)
# ─────────────────────────────────────────────────────────────
$(BUILD_DIR)/logger.o:           $(SRC_DIR)/logger.h $(SRC_DIR)/common.h config.h
$(BUILD_DIR)/consistent_hash.o:  $(SRC_DIR)/consistent_hash.h $(SRC_DIR)/logger.h
$(BUILD_DIR)/rate_limiter.o:     $(SRC_DIR)/rate_limiter.h $(SRC_DIR)/logger.h $(SRC_DIR)/common.h
$(BUILD_DIR)/security.o:         $(SRC_DIR)/security.h $(SRC_DIR)/common.h $(SRC_DIR)/logger.h config.h
$(BUILD_DIR)/session_manager.o:  $(SRC_DIR)/session_manager.h $(SRC_DIR)/consistent_hash.h $(SRC_DIR)/common.h $(SRC_DIR)/logger.h
$(BUILD_DIR)/request_handler.o:  $(SRC_DIR)/request_handler.h $(SRC_DIR)/common.h $(SRC_DIR)/logger.h config.h
$(BUILD_DIR)/load_balancer.o:    $(SRC_DIR)/load_balancer.h $(SRC_DIR)/session_manager.h $(SRC_DIR)/consistent_hash.h \
                                 $(SRC_DIR)/rate_limiter.h $(SRC_DIR)/security.h $(SRC_DIR)/request_handler.h \
                                 $(SRC_DIR)/common.h $(SRC_DIR)/logger.h config.h
$(BUILD_DIR)/main.o:             $(SRC_DIR)/common.h $(SRC_DIR)/logger.h $(SRC_DIR)/consistent_hash.h \
                                 $(SRC_DIR)/rate_limiter.h $(SRC_DIR)/security.h $(SRC_DIR)/session_manager.h \
                                 $(SRC_DIR)/request_handler.h $(SRC_DIR)/load_balancer.h config.h
$(BUILD_DIR)/test_all.o:         $(SRC_DIR)/common.h $(SRC_DIR)/logger.h $(SRC_DIR)/consistent_hash.h \
                                 $(SRC_DIR)/rate_limiter.h $(SRC_DIR)/security.h $(SRC_DIR)/session_manager.h \
                                 $(SRC_DIR)/request_handler.h $(SRC_DIR)/load_balancer.h config.h
