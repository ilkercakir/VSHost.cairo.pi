CC=gcc
CFLAGS=-Wall -c -DUSE_OPENGL -DUSE_EGL -DIS_RPI -DSTANDALONE -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DTARGET_POSIX -D_LINUX -fPIC -DPIC -D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -U_FORTIFY_SOURCE -g -ftree-vectorize -pipe -DHAVE_LIBBCM_HOST -DUSE_EXTERNAL_LIBBCM_HOST -DUSE_VCHIQ_ARM -Wno-psabi -mcpu=cortex-a53 -mfloat-abi=hard -mfpu=neon-fp-armv8 $(shell pkg-config --cflags gtk+-3.0) -I/opt/vc/include/ -I/opt/vc/include/interface/vcos/pthreads -I/opt/vc/include/interface/vmcs_host/linux -Wno-deprecated-declarations
LDFLAGS=-D_POSIX_C_SOURCE=199309L $(shell pkg-config --cflags gtk+-3.0) -Wl,--whole-archive -I/opt/vc/include -L/opt/vc/lib/ -lX11 -lGLESv2 -lEGL -lbcm_host -lvchiq_arm -lpthread -lrt -ldl -lm -Wl,--no-whole-archive -rdynamic $(shell pkg-config --libs gtk+-3.0) $(shell pkg-config --libs libavcodec libavformat libavutil libswscale libswresample) -lasound $(shell pkg-config --libs gtk+-3.0) $(shell pkg-config --libs sqlite3) -lpulse
SOURCES=VSHost.c VStudio.c VSJam.c VSEffect.c VSTMediaPlayer.c VSTVideoPlayer.c AudioDev.c AudioMic.c AudioSpk.c AudioMixer.c AudioPipeNB.c AudioEncoder.c PulseAudio.c VideoQueue.c YUV420RGBgl.c
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=VSHost

all: $(SOURCES) $(EXECUTABLE)
    
$(EXECUTABLE): $(OBJECTS) 
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

.c.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	-rm -f *.o
	-rm -f $(EXECUTABLE)
