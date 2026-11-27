TARGET = LBPPSPAlpha
OBJS = src/main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LDFLAGS =
LIBS = -lpspmpegbase -lpspmpeg -lpspaudio -lpspgu -lm

BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = LBPPSP2008
PSP_EBOOT_ICON = ../LBPPSP2008_ICON0.png
PSP_EBOOT_PIC1 = ../build/LBPPSP2008_PIC1.png

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
