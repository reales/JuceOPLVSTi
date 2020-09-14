#include "JuceHeader.h"

#include <stdio.h>
#include <string.h>

#include "drov1.h"

// http://sourceforge.net/p/dosbox/code-0/HEAD/tree/dosbox/tags/RELEASE_0_72/src/hardware/adlib.cpp#l196

// from mem.h
typedef Bit8u * HostPt;
#if defined(WORDS_BIGENDIAN) || !defined(C_UNALIGNED_MEMORY)

INLINE Bit8u host_readb(HostPt off) {
	return off[0];
};
INLINE Bit16u host_readw(HostPt off) {
	return off[0] | (off[1] << 8);
};
INLINE Bit32u host_readd(HostPt off) {
	return off[0] | (off[1] << 8) | (off[2] << 16) | (off[3] << 24);
};
INLINE void host_writeb(HostPt off,Bit8u val) {
	off[0]=val;
};
INLINE void host_writew(HostPt off,Bit16u val) {
	off[0]=(Bit8u)(val);
	off[1]=(Bit8u)(val >> 8);
};
INLINE void host_writed(HostPt off,Bit32u val) {
	off[0]=(Bit8u)(val);
	off[1]=(Bit8u)(val >> 8);
	off[2]=(Bit8u)(val >> 16);
	off[3]=(Bit8u)(val >> 24);
};

#else

INLINE Bit8u host_readb(HostPt off) {
	return *(Bit8u *)off;
};
INLINE Bit16u host_readw(HostPt off) {
	return *(Bit16u *)off;
};
INLINE Bit32u host_readd(HostPt off) {
	return *(Bit32u *)off;
};
INLINE void host_writeb(HostPt off,Bit8u val) {
	*(Bit8u *)(off)=val;
};
INLINE void host_writew(HostPt off,Bit16u val) {
	*(Bit16u *)(off)=val;
};
INLINE void host_writed(HostPt off,Bit32u val) {
	*(Bit32u *)(off)=val;
};

#endif

#define RAW_SIZE 1024
static struct {
	struct {
		FILE * handle;
		bool capturing;
		Bit64s start;
		Bit64s last;
		Bit8u index;
		Bit8u buffer[RAW_SIZE+8];
		Bit8u regs[2][256];
		Bit32u used;
		Bit32u done;
		Bit8u cmd[2];
		bool opl3;
		bool dualopl2;
	} raw;
} opl;

static void OPL_RawAdd(Bitu index,Bitu val);

static Bit8u dro_header[]={
	'D','B','R','A',		/* 0x00, Bit32u ID */
	'W','O','P','L',		/* 0x04, Bit32u ID */
	0x0,0x00,				/* 0x08, Bit16u version low */
	0x1,0x00,				/* 0x09, Bit16u version high */
	0x0,0x0,0x0,0x0,		/* 0x0c, Bit32u total milliseconds */
	0x0,0x0,0x0,0x0,		/* 0x10, Bit32u total data */
	0x0,0x0,0x0,0x0			/* 0x14, Bit32u Type 0=opl2,1=opl3,2=dual-opl2 */
};
/* Commands 
	0x00 Bit8u, millisecond delay+1
	0x02 none, Use the low index/data pair
	0x03 none, Use the high index/data pair
	0x10 Bit16u, millisecond delay+1
	0xxx Bit8u, send command and data to current index/data pair
*/ 

static void OPL_RawEmptyBuffer(void) {
	fwrite(opl.raw.buffer,1,opl.raw.used,opl.raw.handle);
	opl.raw.done+=opl.raw.used;
	opl.raw.used=0;
}

#define ADDBUF(_VAL_) opl.raw.buffer[opl.raw.used++]=_VAL_;
void OPL_CaptureReg(Bitu reg, Bitu val) { if (opl.raw.capturing) {
	/* Check if we have yet to start */
	if (!opl.raw.handle) {
		if (reg<0xb0 || reg>0xb8) return;
		if (!(val&0x20))  return;
		Bitu i;
		Bit64s t = Time::currentTimeMillis();
		opl.raw.last=t;
		opl.raw.start=t;
		opl.raw.handle=fopen("c:\\temp\\cap.dro", "wb");
		if (!opl.raw.handle) {
			opl.raw.capturing=false;		
			return;
		}
		memset(opl.raw.buffer,0,sizeof(opl.raw.buffer));
		fwrite(dro_header,1,sizeof(dro_header),opl.raw.handle);
		/* Check the registers to add */
		for (i=0;i<256;i++) {
			if (!opl.raw.regs[0][i]) continue;
			if (i>=0xb0 && i<=0xb8) continue;
			ADDBUF((Bit8u)i);
			ADDBUF(opl.raw.regs[0][i]);
		}
		bool donesecond=false;
		/* Check if we already have an opl3 enable bit logged */
		if (opl.raw.regs[1][5] & 1)
			opl.raw.opl3 = true;
		for (i=0;i<256;i++) {
			if (!opl.raw.regs[1][i]) continue;
			if (i>=0xb0 && i<=0xb8) continue;
			if (!donesecond) {
				/* Or already have dual opl2 */
				opl.raw.dualopl2 = true;
				donesecond=true;
				ADDBUF(0x3);
			}
			ADDBUF((Bit8u)i);
			ADDBUF(opl.raw.regs[1][i]);
		}
		if (donesecond) ADDBUF(0x2);
	}
	/* Check if we enable opl3 or access dual opl2 mode */
	if (cmd == 5 && index && (val & 1)) {
		opl.raw.opl3 = true;
	}
	if (index && val && cmd>=0xb0 && cmd<=0xb8) {
		opl.raw.dualopl2 = true;
	}
	/* Check how much time has passed, Allow an extra 5 milliseconds? */
	Bit64s t = Time::currentTimeMillis();
	if (t > (opl.raw.last+5)) {
		Bitu passed = (Bitu)(t - opl.raw.last);
		opl.raw.last = t;
		while (passed) {
			passed-=1;
			if (passed<256) {
				ADDBUF(0x00);					//8bit delay
				ADDBUF((Bit8u)passed);			//8bit delay
				passed=0;
			} else if (passed<0x10000) {
				ADDBUF(0x01);					//16bit delay
				ADDBUF((Bit8u)(passed & 0xff));	//lo-byte
				ADDBUF((Bit8u)(passed >> 8));	//hi-byte
				passed=0;
			} else {
				ADDBUF(0x01);					//16bit delay
				ADDBUF(0xff);					//lo-byte
				ADDBUF(0xff);					//hi-byte
				passed-=0xffff;
			}
		}
	}
	/* Check if we're still sending to the correct index */
	if (opl.raw.index != index) {
		opl.raw.index=(Bit8u)index;
		ADDBUF(opl.raw.index ? 0x3 : 0x2);
	}
	ADDBUF(cmd);
	ADDBUF((Bit8u)val);
	if (opl.raw.used>=RAW_SIZE) OPL_RawEmptyBuffer();
}}

void OPL_ToggleCapture() {
	/* Check for previously opened file */
	if (opl.raw.handle) {
		OPL_RawEmptyBuffer();
		/* Fill in the header with useful information */
		host_writed(&dro_header[0x0c],(Bit32u)(opl.raw.last-opl.raw.start));
		host_writed(&dro_header[0x10],opl.raw.done);
		if (opl.raw.opl3 && opl.raw.dualopl2) host_writed(&dro_header[0x14],0x1);
		else if (opl.raw.dualopl2) host_writed(&dro_header[0x14],0x2);
		else host_writed(&dro_header[0x14],0x0);
		fseek(opl.raw.handle,0,0);
		fwrite(dro_header,1,sizeof(dro_header),opl.raw.handle);
		fclose(opl.raw.handle);
		opl.raw.handle=0;
	}
	if (opl.raw.capturing) {
		opl.raw.capturing=false;
		//LOG_MSG("Stopping Raw OPL capturing.");
	} else {
		//LOG_MSG("Preparing to capture Raw OPL, will start with first note played.");
		opl.raw.capturing=true;
		opl.raw.index=0;
		opl.raw.used=0;
		opl.raw.done=0;
		opl.raw.start=0;
		opl.raw.last=0;
	}
}