# avdecc/ — build glue between AOEther's C code and la_avdecc.
#
# Produces `build/libaoether_avdecc.a`, a static library containing the
# C++ glue in src/entity.cpp linked against la_avdecc. The receiver and
# talker Makefiles pick this up when AOETHER_HAVE_AVDECC=1 is in the
# environment (or detected automatically if this archive exists).
#
# Build steps:
#   1. `make la_avdecc` — runs CMake on ../third_party/la_avdecc with the
#      options AOEther needs (Entity + ProtocolInterface only; no
#      Controller). Produces a static libavdecc-static.a in
#      build/la_avdecc/.
#   2. `make` — compiles src/entity.cpp, archives it and la_avdecc's
#      static lib into build/libaoether_avdecc.a.
#
# Run `git submodule update --init --recursive` first to populate
# ../third_party/la_avdecc.
#
# Build dependencies: cmake, g++ (C++17), libpcap-dev.

BUILD      := build
LA_AVDECC  := ../third_party/la_avdecc
LA_BUILD   := $(BUILD)/la_avdecc
LA_STATIC  := $(LA_BUILD)/src/libla_avdecc_static.a

CXX        ?= g++
CXXFLAGS   ?= -O2 -Wall -Wextra -std=c++17 -fPIC
INCLUDES   := -I$(LA_AVDECC)/include -I../common

OBJS       := $(BUILD)/entity.o
TARGET     := $(BUILD)/libaoether_avdecc.a

.PHONY: all la_avdecc clean check-submodule

all: check-submodule $(TARGET)

check-submodule:
	@test -f $(LA_AVDECC)/CMakeLists.txt || { \
	    echo "error: la_avdecc submodule not populated."; \
	    echo "       Run: git submodule update --init --recursive"; \
	    exit 1; \
	}

$(TARGET): $(OBJS) $(LA_STATIC) | $(BUILD)
	# Combine our entity.o with la_avdecc's static lib so the receiver
	# and talker only need to link one archive. GNU ar's MRI script is
	# the portable way to splice archives without extracting to disk.
	@echo "create $@" > $(BUILD)/merge.mri
	@echo "addlib $(LA_STATIC)" >> $(BUILD)/merge.mri
	@echo "addmod $(BUILD)/entity.o" >> $(BUILD)/merge.mri
	@echo "save" >> $(BUILD)/merge.mri
	@echo "end" >> $(BUILD)/merge.mri
	ar -M < $(BUILD)/merge.mri

$(BUILD)/entity.o: src/entity.cpp ../common/avdecc.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(LA_STATIC): | $(LA_BUILD)
	cd $(LA_BUILD) && cmake $(abspath $(LA_AVDECC)) \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DBUILD_AVDECC_CONTROLLER=OFF \
	    -DBUILD_AVDECC_EXAMPLES=OFF \
	    -DBUILD_AVDECC_TESTS=OFF \
	    -DBUILD_AVDECC_DOC=OFF \
	    -DBUILD_AVDECC_BINDINGS_C=ON \
	    -DBUILD_AVDECC_LIB_STATIC=ON \
	    -DBUILD_AVDECC_LIB_SHARED=OFF
	cmake --build $(LA_BUILD) --config Release -j

$(LA_BUILD) $(BUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILD)
