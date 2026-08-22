CXX ?= g++
CXXFLAGS ?= -O3 -march=native
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Werror -Wno-deprecated-declarations -Wno-reorder -Wno-unused-parameter
CPPFLAGS += -Isrc -I/home/andrew/programs/vm/vgk/tools/tu104-bar1-overlay -isystem build/deps/memflow/memflow-ffi
LDLIBS += -lm -ldl -lpthread

BUILD_DIR := build
MEMFLOW_DIR := $(BUILD_DIR)/deps/memflow
MEMFLOW_TAG := 0.2.4
MEMFLOW_HEADER := $(MEMFLOW_DIR)/memflow-ffi/memflow.hpp
MEMFLOW_LIBRARY := $(MEMFLOW_DIR)/target/release/libmemflow_ffi.a
TARGET := $(BUILD_DIR)/winshipping
SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
DEPENDENCIES := $(OBJECTS:.o=.d)

.PHONY: all clean distclean run

all: $(TARGET)

$(TARGET): $(OBJECTS) $(MEMFLOW_LIBRARY)
	$(CXX) $(CXXFLAGS) $(OBJECTS) $(MEMFLOW_LIBRARY) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: src/%.cpp $(MEMFLOW_HEADER) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(MEMFLOW_DIR)/.git:
	mkdir -p $(dir $(MEMFLOW_DIR))
	git clone --depth 1 --branch $(MEMFLOW_TAG) https://github.com/memflow/memflow.git $(MEMFLOW_DIR)

$(MEMFLOW_HEADER): $(MEMFLOW_DIR)/.git

$(MEMFLOW_LIBRARY): $(MEMFLOW_HEADER)
	cargo build --manifest-path $(MEMFLOW_DIR)/Cargo.toml --release -p memflow-ffi

$(BUILD_DIR):
	mkdir -p $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJECTS) $(DEPENDENCIES) $(TARGET)

distclean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
