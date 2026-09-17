# Makefile -- C++ host with the Lua scripts baked into the executable.
#
#   src/      C++ translation units (every .cpp here is compiled and linked)
#   include/  headers shared between them
#   lua/      Lua sources, each embedded as a byte array
#   build/    everything generated; `make clean` removes it wholesale
#
# Sources are discovered by wildcard, so adding src/foo.cpp or lua/foo.lua
# needs no edit here.

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2

LUA_PKG    := lua5.4
LUA_CFLAGS := $(shell pkg-config --cflags $(LUA_PKG))
LUA_LIBS   := $(shell pkg-config --libs   $(LUA_PKG))

LUAC := $(shell command -v luac5.4 || command -v luac)
XXD  := xxd

SRC_DIR   := src
LUA_DIR   := lua
TEST_DIR  := test
BUILD_DIR := build

# Self-contained header-only Lua-binding layer. $(LUAKIT_DIR) is its include
# root, so it is reached as <luakit/...> and can be dropped into another
# project as a unit. It depends on nothing else here.
LUAKIT_DIR := luakit

TARGET := $(BUILD_DIR)/main

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# lua/foo.lua becomes build/foo_lua.h, defining foo_lua[] and foo_lua_len.
# That is why $(BUILD_DIR) is on the include path below.
LUA_SRCS := $(wildcard $(LUA_DIR)/*.lua)
LUA_HDRS := $(patsubst $(LUA_DIR)/%.lua,$(BUILD_DIR)/%_lua.h,$(LUA_SRCS))
LUA_BC   := $(patsubst $(LUA_DIR)/%.lua,$(BUILD_DIR)/%.luac,$(LUA_SRCS))

INCLUDES := -I$(LUAKIT_DIR) -I$(SRC_DIR) -I$(BUILD_DIR) $(LUA_CFLAGS)

# One binary per test file, so a crash in one suite cannot hide the others.
TEST_SRCS := $(wildcard $(TEST_DIR)/*.cpp)
TEST_BINS := $(patsubst $(TEST_DIR)/%.cpp,$(BUILD_DIR)/test/%,$(TEST_SRCS))

# The tests assert things only a sanitizer can see -- no leak from a failed
# argument check, no double free from a throwing constructor -- so they are
# always built instrumented, regardless of the main build's flags.
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

# -MMD -MP emits a .d file per object listing the headers it pulled in, so
# touching luakit/luakit/guard.hpp rebuilds exactly what depends on it.
DEPFLAGS := -MMD -MP

# EMBED=bytecode  precompile with luac (default)
# EMBED=source    embed the .lua text, let liblua parse it at startup;
#                 slower to start but immune to bytecode version skew.
# Run `make clean` when switching, the header's prerequisite changes.
EMBED ?= bytecode

# Strip line numbers and local names from the bytecode. Set to 0 while
# iterating, so script errors still carry useful tracebacks.
STRIP_DEBUG ?= 1
ifeq ($(STRIP_DEBUG),1)
  LUACFLAGS := -s
endif

CLANG_FORMAT := $(shell command -v clang-format)
STYLUA       := $(shell command -v stylua)
# api_gen.hpp is generated; excluded so `make api` output never fights
# `make format`.
GENERATED    := $(LUAKIT_DIR)/luakit/api_gen.hpp
FORMAT_SRCS  := $(SRCS) $(wildcard $(SRC_DIR)/*.hpp) $(TEST_SRCS) \
                $(wildcard $(TEST_DIR)/*.hpp) \
                $(filter-out $(GENERATED),$(wildcard $(LUAKIT_DIR)/luakit/*.hpp))

.PHONY: all run clean format format-check api test
.SECONDARY: $(LUA_BC)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LUA_LIBS)

# The generated headers have no .d entry until an object exists, so the first
# build needs this to know they must come first.
$(OBJS): $(LUA_HDRS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD_DIR)/%.luac: $(LUA_DIR)/%.lua | $(BUILD_DIR)
	$(LUAC) $(LUACFLAGS) -o $@ $<

ifeq ($(EMBED),bytecode)
$(BUILD_DIR)/%_lua.h: $(BUILD_DIR)/%.luac | $(BUILD_DIR)
	$(XXD) -i -n $*_lua $< > $@
else
$(BUILD_DIR)/%_lua.h: $(LUA_DIR)/%.lua | $(BUILD_DIR)
	$(XXD) -i -n $*_lua $< > $@
endif

# Order-only prerequisite: the directory's mtime changes on every write, so
# depending on it normally would rebuild everything each time.
$(BUILD_DIR):
	mkdir -p $@

run: $(TARGET)
	./$(TARGET)

test: $(TEST_BINS)
	@rc=0; for t in $(TEST_BINS); do \
	  echo "== $$t"; ASAN_OPTIONS=detect_leaks=1 $$t || rc=1; \
	done; exit $$rc

$(BUILD_DIR)/test/%: $(TEST_DIR)/%.cpp $(wildcard $(TEST_DIR)/*.hpp) \
                     $(wildcard $(LUAKIT_DIR)/luakit/*.hpp)
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -Wall -Wextra -pedantic $(SAN_FLAGS) $(INCLUDES) -I$(TEST_DIR) \
	  -o $@ $< $(LUA_LIBS)

clean:
	$(RM) -r $(BUILD_DIR)

# Regenerate the mechanical half of the lua:: layer from the Lua headers.
# Only needed when the Lua version changes.
api:
	python3 tools/gen_api.py --lua-include $(patsubst -I%,%,$(firstword $(LUA_CFLAGS))) -o $(GENERATED)

# C++ style comes from .clang-format (LLVM), Lua style from stylua.toml.
# Both file lists are globbed so a new file is never silently left out.
format:
	@test -n "$(CLANG_FORMAT)" || { echo "clang-format not installed"; exit 1; }
	@test -n "$(STYLUA)" || { echo "stylua not installed"; exit 1; }
	$(CLANG_FORMAT) -i $(FORMAT_SRCS)
	$(STYLUA) $(LUA_SRCS)

# Non-mutating: exits non-zero if anything is unformatted, for CI or a hook.
# Both checks run even if the first fails, so you see every offender at once.
format-check:
	@test -n "$(CLANG_FORMAT)" || { echo "clang-format not installed"; exit 1; }
	@test -n "$(STYLUA)" || { echo "stylua not installed"; exit 1; }
	@rc=0; \
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SRCS) || rc=1; \
	$(STYLUA) --check $(LUA_SRCS) || rc=1; \
	exit $$rc

-include $(DEPS)
