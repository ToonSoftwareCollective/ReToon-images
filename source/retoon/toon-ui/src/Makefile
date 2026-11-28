# toon-ui: built by Buildroot (package/toon-ui/toon-ui.mk)
OBJS = main.o util.o i18n.o ha.o local.o dash.o wifi.o mqtt.o rescue.o apps.o touchcal.o notify.o graph.o bus.o energy.o gif.o
CFLAGS += -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE

# STATIC=toon-splash-static: also the static splash (the Toon 1 boot manager)
all: toon-ui toon-ha toon-splash toon-api $(STATIC)

toon-api: toon-api.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -ltoonbus -lmosquitto -lpthread

toon-splash: toon-splash.c splash-logo.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lm

# for a boot manager without glibc (the Toon 1 initrd, in the kernel)
toon-splash-static: toon-splash.c splash-logo.h
	$(CC) $(CFLAGS) $(LDFLAGS) -Os -static -s -o $@ $< -lm

toon-ha: toon-ha.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lcjson -lssl -lcrypto

toon-ui: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-z,noexecstack -o $@ $(OBJS) -llvgl -lcjson -ltoonbus -lmosquitto -lcurl -ljpeg $(QJS_LIB) -lssl -lcrypto -lpthread -ldl -lm $(EXTRA_LIBS)

$(OBJS): ui.h
apps.o gif.o: gif.h
