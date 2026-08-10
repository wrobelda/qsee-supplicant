CC ?= cc
CPPFLAGS += -D_GNU_SOURCE -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror

COMMON = src/path.c src/services.c src/fs.c src/gpfs.c
NOTIFY = src/notify.c

PREFIX ?= /usr
SBINDIR ?= $(PREFIX)/sbin
UNITDIR ?= $(PREFIX)/lib/systemd/system
SYSCONFDIR ?= /etc
DOCDIR ?= $(PREFIX)/share/doc/qsee-supplicant
DESTDIR ?=

.PHONY: all check clean install
all: qsee-supplicant qsee-app-loader

qsee-supplicant: src/main.c src/transport_qseecom.c $(COMMON) $(NOTIFY)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

qsee-app-loader: src/app_loader.c $(NOTIFY)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test-protocol: tests/test_protocol.c $(COMMON)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test-notify: tests/test_notify.c $(NOTIFY)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

check: test-protocol test-notify
	./test-protocol
	./test-notify

install: qsee-supplicant qsee-app-loader
	install -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(UNITDIR) $(DESTDIR)$(SYSCONFDIR)/init.d $(DESTDIR)$(DOCDIR)
	install -m 0755 qsee-supplicant $(DESTDIR)$(SBINDIR)/qsee-supplicant
	install -m 0755 qsee-app-loader $(DESTDIR)$(SBINDIR)/qsee-app-loader
	install -m 0644 packaging/qsee-supplicant.service $(DESTDIR)$(UNITDIR)/
	install -m 0644 packaging/qsee-app-loader@.service $(DESTDIR)$(UNITDIR)/
	install -m 0755 packaging/qsee-supplicant.openrc $(DESTDIR)$(SYSCONFDIR)/init.d/qsee-supplicant
	install -m 0644 README.md $(DESTDIR)$(DOCDIR)/README.md

clean:
	rm -f qsee-supplicant qsee-app-loader test-protocol test-notify
