#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdint.h>

#define STR_TITLE "PCulator"
#define STR_VERSION "0.5.0"

//#define DEBUG_INTS // interrupt control-flow trace
//#define DEBUG_DMA
//#define DEBUG_VGA
//#define DEBUG_PIT
//#define DEBUG_PIC
//#define DEBUG_PPI
//#define DEBUG_KBC
//#define DEBUG_UART
//#define DEBUG_PCSPEAKER
//#define DEBUG_MEMORY
//#define DEBUG_PORTS
//#define DEBUG_TIMING
//#define DEBUG_OPL2
//#define DEBUG_BLASTER
//#define DEBUG_FDC
//#define DEBUG_NE2000
//#define DEBUG_PCAP

#define VIDEO_CARD_STDVGA	0
#define VIDEO_CARD_GD5440	1

#define SAMPLE_RATE		48000
#define SAMPLE_BUFFER	9600 //Stereo stream, so this is really 4800 two-channel samples

#ifdef _MSC_VER
#define FUNC_FORCE_INLINE __forceinline
#define FUNC_INLINE inline
#elif __GNUC__
#define FUNC_FORCE_INLINE inline __attribute__((always_inline))
#define FUNC_INLINE inline
#else
#define FUNC_FORCE_INLINE inline
#define FUNC_INLINE inline
#endif

#ifndef _MSC_VER
#define _stricmp strcasecmp
#endif

extern volatile uint8_t running;
extern uint8_t videocard, showMIPS;
extern volatile double speed;
extern volatile double currentMIPS;
extern uint32_t ramsize;
extern char* usemachine;

#endif
