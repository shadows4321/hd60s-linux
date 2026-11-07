PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
UDEVDIR ?= /etc/udev/rules.d
UNITDIR ?= $(PREFIX)/lib/systemd/user

CFLAGS ?= -O2 -Wall -Wextra -g
CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS  = $(shell pkg-config --libs libusb-1.0) -lpthread

all: hd60s hd60s.service

hd60s: hd60s.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

hd60s.service: hd60s.service.in
	sed 's|@BINDIR@|$(BINDIR)|g' $< > $@

test: hd60s
	python3 -I ../tools/check_synth.py ./hd60s

install: all
	install -Dm755 hd60s          $(DESTDIR)$(BINDIR)/hd60s
	install -Dm755 hd60s-obs      $(DESTDIR)$(BINDIR)/hd60s-obs
	install -Dm644 60-hd60s.rules $(DESTDIR)$(UDEVDIR)/60-hd60s.rules
	install -Dm644 hd60s.service  $(DESTDIR)$(UNITDIR)/hd60s.service
	@echo
	@echo "Installed. Now run:  sudo udevadm control --reload"
	@echo "and as your user:    systemctl --user daemon-reload && systemctl --user enable hd60s.service"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/hd60s $(DESTDIR)$(BINDIR)/hd60s-obs \
	      $(DESTDIR)$(UDEVDIR)/60-hd60s.rules $(DESTDIR)$(UNITDIR)/hd60s.service

clean:
	rm -f hd60s hd60s.service

.PHONY: all test install uninstall clean
