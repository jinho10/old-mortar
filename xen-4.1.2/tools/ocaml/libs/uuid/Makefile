TOPLEVEL=$(CURDIR)/../..
XEN_ROOT=$(TOPLEVEL)/../..
include $(TOPLEVEL)/common.make

OBJS = uuid
INTF = $(foreach obj, $(OBJS),$(obj).cmi)
LIBS = uuid.cma uuid.cmxa

all: $(INTF) $(LIBS) $(PROGRAMS)

bins: $(PROGRAMS)

libs: $(LIBS)

uuid_OBJS = $(OBJS)
OCAML_NOC_LIBRARY = uuid

.PHONY: install
install: $(LIBS) META
	mkdir -p $(OCAMLDESTDIR)
	#ocamlfind remove -destdir $(OCAMLDESTDIR) uuid
	sudo rm -rf $(OCAMLDESTDIR)/uuid
	ocamlfind install -destdir $(OCAMLDESTDIR) -ldconf ignore uuid META $(INTF) $(LIBS) *.a *.cmx

.PHONY: uninstall
uninstall:
	sudo rm -rf $(OCAMLDESTDIR)/uuid
	#ocamlfind remove -destdir $(OCAMLDESTDIR) uuid

include $(TOPLEVEL)/Makefile.rules

