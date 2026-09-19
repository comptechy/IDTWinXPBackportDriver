/*
 * szprobe - compile-time ABI probe for the Stage 5bo pin-create question.
 *
 * Built for BOTH x86 and x64 and run on the DEV machine, not on the XP box:
 * every number it prints is a property of the compiler's ABI for the target
 * architecture, so a Win10 x64 host reports exactly what the XP x64 machine
 * would. This answers "is KSPIN_CONNECT a different size in a 32-bit
 * process?" without spending a hardware test cycle.
 *
 * The specific question: kstest32's KsCreatePin fails with
 * STATUS_INVALID_BUFFER_SIZE (0xC00000F2) while the identical 64-bit build
 * succeeds. KsCreatePin sizes the create buffer as
 *     sizeof(KSPIN_CONNECT) + ((PKSDATAFORMAT)(Connect + 1))->FormatSize
 * so both that sizeof AND the offset of the format that follows it matter.
 */

#include <windows.h>
#include <stdio.h>
#include <stddef.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>

/* Exactly the declaration kstest.c uses. */
typedef struct _PIN_CONNECT_FORMAT
{
    KSPIN_CONNECT              Connect;
    KSDATAFORMAT_WAVEFORMATEX  Format;
} PIN_CONNECT_FORMAT;

int __cdecl main(void)
{
#if defined(_WIN64)
    printf("=== 64-bit build ===\n");
#else
    printf("=== 32-bit build ===\n");
#endif
    printf("sizeof(void*)                      = %u\n", (unsigned)sizeof(void*));
    printf("\n-- KSPIN_CONNECT --\n");
    printf("sizeof(KSPIN_CONNECT)              = %u\n", (unsigned)sizeof(KSPIN_CONNECT));
    printf("  offsetof Interface               = %u\n", (unsigned)offsetof(KSPIN_CONNECT, Interface));
    printf("  offsetof Medium                  = %u\n", (unsigned)offsetof(KSPIN_CONNECT, Medium));
    printf("  offsetof PinId                   = %u\n", (unsigned)offsetof(KSPIN_CONNECT, PinId));
    printf("  offsetof PinToHandle             = %u\n", (unsigned)offsetof(KSPIN_CONNECT, PinToHandle));
    printf("  offsetof Priority                = %u\n", (unsigned)offsetof(KSPIN_CONNECT, Priority));
    printf("  sizeof  KSPIN_INTERFACE          = %u\n", (unsigned)sizeof(KSPIN_INTERFACE));
    printf("  sizeof  KSPIN_MEDIUM             = %u\n", (unsigned)sizeof(KSPIN_MEDIUM));
    printf("  sizeof  KSPRIORITY               = %u\n", (unsigned)sizeof(KSPRIORITY));

    printf("\n-- data format --\n");
    printf("sizeof(KSDATAFORMAT)               = %u\n", (unsigned)sizeof(KSDATAFORMAT));
    printf("sizeof(WAVEFORMATEX)               = %u\n", (unsigned)sizeof(WAVEFORMATEX));
    printf("sizeof(KSDATAFORMAT_WAVEFORMATEX)  = %u\n", (unsigned)sizeof(KSDATAFORMAT_WAVEFORMATEX));
    printf("sizeof(KSDATARANGE)                = %u\n", (unsigned)sizeof(KSDATARANGE));
    printf("sizeof(KSDATARANGE_AUDIO)          = %u\n", (unsigned)sizeof(KSDATARANGE_AUDIO));

    printf("\n-- kstest's PIN_CONNECT_FORMAT --\n");
    printf("sizeof(PIN_CONNECT_FORMAT)         = %u\n", (unsigned)sizeof(PIN_CONNECT_FORMAT));
    printf("offsetof(.Format)                  = %u\n", (unsigned)offsetof(PIN_CONNECT_FORMAT, Format));
    printf("sizeof(KSPIN_CONNECT)              = %u   <- must equal offsetof(.Format)\n",
           (unsigned)sizeof(KSPIN_CONNECT));
    printf("format follows Connect contiguously? %s\n",
           (offsetof(PIN_CONNECT_FORMAT, Format) == sizeof(KSPIN_CONNECT)) ? "YES" : "NO - BUG");

    printf("\n-- what KsCreatePin will compute --\n");
    printf("FormatSize kstest writes           = %u\n", (unsigned)sizeof(KSDATAFORMAT_WAVEFORMATEX));
    printf("buffer size = sizeof(CONNECT)+Fmt  = %u\n",
           (unsigned)(sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX)));
    printf("bytes actually available in struct = %u\n", (unsigned)sizeof(PIN_CONNECT_FORMAT));

    return 0;
}
