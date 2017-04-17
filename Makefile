PROG=morseplayer
SRCS=morseplayer.c
LDADD=-lm
CFLAGS+=-Wall -Wstrict-prototypes -Wmissing-prototypes
BINDIR=/usr/local/bin
MANDIR=/usr/local/man/man

.include <bsd.prog.mk>
