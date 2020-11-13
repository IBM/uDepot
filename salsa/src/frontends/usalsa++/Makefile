.PHONY: all clean

BUILD_TYPE   ?= NORMAL
BUILD_DIR    ?= ./build

CC        = gcc
CPP       = g++

SALSA_SRC_DIR = ../..
SALSA_INCLUDE = $(SALSA_SRC_DIR)/include


WARNINGS       =  -Wall -Wno-unused-parameter -Wshadow -Werror
WARNINGS_EXTRA = $(WARNINGS) -Wextra
DEFS           = -D_GNU_SOURCE -D__STDC_FORMAT_MACROS

# common for C/C++
INCLUDES  = -I. -I$(SALSA_INCLUDE) -I$(SALSA_INCLUDE)/data-structures
CFLAGS_  += -fPIC $(INCLUDES) $(WARNINGS_EXTRA) $(DEFS)
#CFLAGS_  += -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -ltcmalloc_minimal
#CFLAGS_ += #-fprofile-arcs -ftest-coverage
ifeq (DEBUG,$(BUILD_TYPE))
CFLAGS_   += -O0 -ggdb
else ifeq (NORMAL,$(BUILD_TYPE))
CFLAGS_   += -O2 -g
else ifeq (PERFORMANCE,$(BUILD_TYPE))
CFLAGS_   += -O3 -DNDEBUG -march=native
else
$(error "Unknown BUILD_TYPE: --->$(BUILD_TYPE)<---")
endif

CFLAGS    = $(CFLAGS_) -std=c11
CPPFLAGS  = $(CFLAGS_) -std=c++11
LIBS      =  -lpthread -lrt

TEST_DEPS  = $(patsubst %.c,$(BUILD_DIR)/%.d,$(wildcard tests/*.c))
TEST_DEPS += $(patsubst %.cc,$(BUILD_DIR)/%.d,$(wildcard tests/*.cc))
BASE_DEPS  = $(patsubst %.c,$(BUILD_DIR)/%.d,$(wildcard *.c))
BASE_DEPS += $(patsubst %.cc,$(BUILD_DIR)/%.d,$(wildcard *.cc))

#JEMALLOC_PATH=jemalloc/install
#LIBS     += -L${JEMALLOC_PATH}/lib -Wl,-rpath,${JEMALLOC_PATH}/lib -ljemalloc
#LIBS     += -ljemalloc

all: $(BUILD_DIR)/libusalsa++.o $(BUILD_DIR)/tests/test

SALSA_FILES_SRC = util/parse-storage-args.c     \
                  util/scm_usr_helpers.c        \
                  sto-ctlr/sto-capacity-mgr.c   \
                  sto-ctlr/scm-dev-properties.c \
                  sto-ctlr/scm-seg-queues.c     \
                  sto-ctlr/scm-seg-alloc.c      \
                  sto-ctlr/scm-throttler.c      \
                  gc/gc-common.c                \
                  gc/gc-base.c                  \
                  gc/gc-nbin.c                   \
                  libos/os-thread.c             \
		  frontends/usalsa++/allocator.c\

SALSA_FILES_TARGETS = $(patsubst %.c,$(BUILD_DIR)/salsa.objs/%.o,$(SALSA_FILES_SRC))
SALSA_FILES_DEPS    = $(patsubst %.c,$(BUILD_DIR)/salsa.objs/%.d,$(SALSA_FILES_SRC))

$(BUILD_DIR)/libusalsa++.o: $(BUILD_DIR)/Scm.o $(BUILD_DIR)/SalsaCtlr.o $(SALSA_FILES_TARGETS)
	$(LD) -i $^ -o $@

-include $(TEST_DEPS)
-include $(BASE_DEPS)
-include $(SALSA_FILES_DEPS)

# flags for compiling salsa files
SALSA_CFLAGS = -fPIC $(INCLUDES) -I$(SALSA_INCLUDE)/generic $(WARNINGS) $(DEFS)
ifeq (DEBUG,$(BUILD_TYPE))
SALSA_CFLAGS   += -O0 -ggdb
else ifeq (NORMAL,$(BUILD_TYPE))
SALSA_CFLAGS   += -O2 -g
else ifeq (PERFORMANCE,$(BUILD_TYPE))
SALSA_CFLAGS   += -O3 -DNDEBUG -march=native
else
$(error "Unknown BUILD_TYPE: --->$(BUILD_TYPE)<---")
endif

$(BUILD_DIR)/salsa.objs/%.o: $(SALSA_SRC_DIR)/%.c
	@# create directry if it does not exist
	[ -d $(shell dirname $@) ] || mkdir -p $(shell dirname $@)
	$(CC) $(SALSA_CFLAGS) -c  $< -o $@
	$(CC) $(SALSA_CFLAGS) -MM $< -MT $@ -MF $(BUILD_DIR)/salsa.objs/$*.d

$(BUILD_DIR)/%.o: %.c
	@# create directry if it does not exist
	[ -d $(shell dirname $@) ] || mkdir -p $(shell dirname $@)
	$(CC) $(CFLAGS) -c  $< -o $@
	$(CC) $(CFLAGS) -MM $< -MT $@ -MF $(BUILD_DIR)/$*.d

$(BUILD_DIR)/%.o: %.cc
	@# create directry if it does not exist
	[ -d $(shell dirname $@) ] || mkdir -p $(shell dirname $@)
	$(CPP) $(CPPFLAGS) -c  $< -o $@
	$(CPP) $(CPPFLAGS) -MM $< -MT $@ -MF $(BUILD_DIR)/$*.d

$(BUILD_DIR)/tests/%: $(JCTX_OBJS) $(BUILD_DIR)/libusalsa++.o $(BUILD_DIR)/tests/%.o
	$(CPP) $^ $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -rf $(BUILD_DIR)
