OPTIM=-O
WARNS=-Wall -Wstrict-prototypes -Wmissing-prototypes

PORTAUDIO_CFLAGS=`pkg-config portaudio-2.0 --cflags`
PORTAUDIO_LIBS=`pkg-config portaudio-2.0 --libs`

CFLAGS=$(OPTIM) $(WARNS) $(PORTAUDIO_CFLAGS)

LDFLAGS=$(PORTAUDIO_LIBS)

morseplayer: morseplayer.c
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)
