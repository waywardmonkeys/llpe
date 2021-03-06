
include Makefile.config

TARGETS = arrayloop deepnesting dependent_loops elim_if inline inline-nested inline-siblings \
	  nested_loops nonconstarrayloop partial_nested_loops pass-pointer-in nonconstglobal \
          invariants def-function def-skip-function defloop defskiploop loadundef memops \
	  small-write-large-load large-write-small-load nonconstmemcpy memcpy-cases \
	  memcpy-cases-nonconst struct-const struct-global reinterpret fp fpreinterpret \
	  fpalign read read-indirect-fd varargs varargs-param varargs-copy pointerbase pointerarith \
	  pointerarithfail pointerarithnested multidef invarcall stdiowrite realstdio optimistloop \
	  ptrornull unboundloop varargs-dyn varargs-fp varargs-mix vfs-dyn invar-exit-edge deadalloc \
	  beforearray realloc punload xmlpush multibreak frames heapmerge heapstress

LLVM_TARGETS = load-struct load-array switch-loop

LLVM_TARGETS_SOURCE = $(patsubst %,%.lls,$(LLVM_TARGETS))
LLVM_TARGETS_ASM = $(patsubst %,%.s,$(LLVM_TARGETS))
LLVM_TARGETS_OBJ = $(patsubst %,%.o,$(LLVM_TARGETS))

BC_TARGETS = $(patsubst %,%.bc,$(TARGETS))
LL_TARGETS = $(patsubst %,%.ll,$(TARGETS))

ALL_TARGETS = $(TARGETS) $(LLVM_TARGETS)

OPT_BC_TARGETS = $(patsubst %,%-opt.bc,$(ALL_TARGETS))
OPT_TARGETS = $(patsubst %,%-opt,$(ALL_TARGETS))

SPECIAL_TARGETS = fakestdio.bc fakestdio-opt.bc

.PHONY: all clean

all: $(TARGETS) $(BC_TARGETS) $(LL_TARGETS) $(LLVM_TARGETS) $(OPT_BC_TARGETS) $(OPT_TARGETS) $(SPECIAL_TARGETS)

fakestdiolib.ltemp: fakestdiolib.c
	$(LLVM_GCC) -std=c99 -O3 $< -o $@

fakestdio.bc: fakestdioclient.bc fakestdiolib.bc
	llvm-link fakestdioclient.bc fakestdiolib.bc -o fakestdio.bc

% : %.c
	gcc -std=c99 $< -o $@

%.ltemp : %.c
	$(LLVM_GCC) -std=c99 $< -o $@

%.bc : %.ltemp
	opt -mem2reg -loop-simplify -lcssa $< -o $@

%.bc : %.lls
	llvm-as $< -o $@

%.ll : %.bc
	llvm-dis $<

% : %.o
	gcc $< -o $@

%.s : %.lls
	llc $< -o $@

%.s : %.bc
	llc $< -o $@

%.o : %.s
	as $< -o $@

%-opt.bc: %.bc
	../../scripts/opt-with-mods.sh -loop-rotate -instcombine -jump-threading -loop-simplify -lcssa -integrator -integrator-accept-all -jump-threading $< -o $@

clean:
	-rm -f $(TARGETS)
	-rm -f $(LLVM_TARGETS)
	-rm -f $(BC_TARGETS)
	-rm -f $(LL_TARGETS)
	-rm -f $(OPT_BC_TARGETS)
	-rm -f $(OPT_TARGETS)
