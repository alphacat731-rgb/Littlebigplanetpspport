TARGET = LBPPSPAlpha
OBJS = src/main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LDFLAGS =
LIBS = -lpspdebug -lpspctrl -lpspmpeg -lpsputility -lpspaudio -lpspgu -lpspdisplay -lpspge -lm

BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = LittleBigPlanet PSP Alpha
PSP_EBOOT_ICON = ../LBPPSP_Alpha_ICON0.png
PSP_EBOOT_PIC0 = ../LBPPSP_Alpha_LoadingScreen.png
PSP_EBOOT_PIC1 = ../LBPPSP_Alpha_PIC1.png

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
