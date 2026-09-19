/*****************************************************************************
 * hdaverbs.h
 *****************************************************************************
 * Standard HDA codec verb IDs, parameter IDs, and widget-type constants
 * (from the public Intel HD Audio spec - protocol-level, not IDT-specific).
 * Codec-specific init verb sequences and pin defaults for the 92HD89E2 live
 * in codec.cpp, derived from sigmatel.c's STAC92HD73XX model path.
 */

#ifndef _HDAVERBS_H_
#define _HDAVERBS_H_

// Common Set verbs (12-bit verb, 8-bit payload unless noted)
#define HDA_VERB_GET_PARAMETER          0xF00
#define HDA_VERB_SET_CONN_SELECT        0x701
#define HDA_VERB_GET_CONN_SELECT        0xF01
#define HDA_VERB_GET_CONN_LIST_ENTRY    0xF02
#define HDA_VERB_SET_PROCESSING_STATE   0x703
#define HDA_VERB_SET_POWER_STATE        0x705
#define HDA_VERB_GET_POWER_STATE        0xF05
#define HDA_VERB_SET_CHAN_STREAMID      0x706  // upper nibble of payload = stream tag, lower = channel
#define HDA_VERB_GET_CHAN_STREAMID      0xF06
#define HDA_VERB_SET_PIN_WIDGET_CTRL    0x707
#define HDA_VERB_GET_PIN_WIDGET_CTRL    0xF07
#define HDA_VERB_SET_UNSOLICITED_ENABLE 0x708  // matches the "0x706xx-masked" special case found in stwrt64.sys's TransferCodecVerb
#define HDA_VERB_GET_UNSOLICITED_ENABLE 0xF08
#define HDA_VERB_SET_PIN_SENSE          0x709
#define HDA_VERB_GET_PIN_SENSE          0xF09
#define HDA_VERB_SET_EAPD_BTL_ENABLE    0x70C
#define HDA_VERB_GET_EAPD_BTL_ENABLE    0xF0C
#define HDA_VERB_SET_STREAM_FORMAT      0x2    // 16-bit payload form, encode directly (not 8-bit)
#define HDA_VERB_GET_STREAM_FORMAT      0xA
#define HDA_VERB_SET_AMP_GAIN_MUTE      0x3    // 16-bit payload form
#define HDA_VERB_GET_AMP_GAIN_MUTE      0xB
#define HDA_VERB_SET_CONFIG_DEFAULT_B0  0x71C  // config default is written/read a byte at a time (4 verbs)
#define HDA_VERB_SET_CONFIG_DEFAULT_B1  0x71D
#define HDA_VERB_SET_CONFIG_DEFAULT_B2  0x71E
#define HDA_VERB_SET_CONFIG_DEFAULT_B3  0x71F
#define HDA_VERB_GET_CONFIG_DEFAULT     0xF1C
#define HDA_VERB_SET_VOLUME_KNOB        0x7F0  // matches sigmatel.c's stac92hd73xx_core_init (NID found by widget Type, not hardcoded)
#define HDA_VERB_FUNCTION_RESET         0x7FF  // "codec reset verb" - the double-reset handshake found in stwrt64.sys targets this

// GET_PARAMETER parameter IDs
#define HDA_PARAM_VENDOR_ID              0x00
#define HDA_PARAM_REVISION_ID            0x02
#define HDA_PARAM_SUBORDINATE_NODE_COUNT 0x04
#define HDA_PARAM_FUNCTION_GROUP_TYPE    0x05
#define HDA_PARAM_AUDIO_WIDGET_CAP       0x09
#define HDA_PARAM_PIN_CAP                0x0C
#define HDA_PARAM_CONN_LIST_LENGTH       0x0E
#define HDA_PARAM_SUPP_PCM_RATES         0x0A

// PARAMETER 0x0A (SUPPORTED_PCM_SIZE_RATES) rate bits. Bit n set means the
// converter can actually produce g_HdaPcmRateHz[n]. A widget only answers
// this for itself when its AUDIO_WIDGET_CAP has the Format Override bit
// (bit 4) set; otherwise the Audio Function Group's answer is the one that
// applies to it, which is why QueryPcmCaps() reads both.
#define HDA_PCM_RATE_COUNT               12
#define HDA_RATE_BIT_8000                (1UL <<  0)
#define HDA_RATE_BIT_11025               (1UL <<  1)
#define HDA_RATE_BIT_16000               (1UL <<  2)
#define HDA_RATE_BIT_22050               (1UL <<  3)
#define HDA_RATE_BIT_32000               (1UL <<  4)
#define HDA_RATE_BIT_44100               (1UL <<  5)
#define HDA_RATE_BIT_48000               (1UL <<  6)
#define HDA_RATE_BIT_88200               (1UL <<  7)
#define HDA_RATE_BIT_96000               (1UL <<  8)
#define HDA_RATE_BIT_176400              (1UL <<  9)
#define HDA_RATE_BIT_192000              (1UL << 10)
#define HDA_RATE_BIT_384000              (1UL << 11)

// The rate each of those bits names, in bit order. Defined in common.cpp.
extern const ULONG g_HdaPcmRateHz[HDA_PCM_RATE_COUNT];
#define HDA_PARAM_SUPP_STREAM_FORMATS    0x0B
#define HDA_PARAM_GPIO_COUNT             0x0F
#define HDA_PARAM_INPUT_AMP_CAP          0x0D
#define HDA_PARAM_OUTPUT_AMP_CAP         0x12

// AMP_CAP dword decode (the GET_PARAMETER 0x12 / 0x0D response). Gain is
// expressed as a step index, not a level: step 0 is the most attenuated
// setting the amp has and step "Offset" is 0 dB, so a step's value in
// decibels is (step - Offset) * (StepSize + 1) * 0.25. A widget only
// reports its own amp capabilities if its AUDIO_WIDGET_CAP carries the
// "amp parameter override" bit; otherwise the AFG's defaults apply.
#define HDA_AMPCAP_OFFSET(c)      ((UCHAR)((c) & 0x7F))
#define HDA_AMPCAP_NUMSTEPS(c)    ((UCHAR)(((c) >> 8) & 0x7F))
#define HDA_AMPCAP_STEPSIZE(c)    ((UCHAR)(((c) >> 16) & 0x7F))
#define HDA_AMPCAP_MUTE(c)        (((c) & 0x80000000) != 0)

// SET_AMP_GAIN_MUTE (verb 0x3) 16-bit payload bits. The existing 0xB07F
// used in InitOutputPin is OUT | LEFT | RIGHT | gain 0x7F.
#define HDA_AMP_SET_OUTPUT        0x8000
#define HDA_AMP_SET_INPUT         0x4000
#define HDA_AMP_SET_LEFT          0x2000
#define HDA_AMP_SET_RIGHT         0x1000
#define HDA_AMP_SET_MUTE          0x0080
#define HDA_AMP_GAIN_MASK         0x007F

// AUDIO_WIDGET_CAP type field (bits 23:20 of the widget-cap dword)
#define HDA_WIDGET_TYPE_OUTPUT    0x0   // Audio Output (DAC)
#define HDA_WIDGET_TYPE_INPUT     0x1   // Audio Input (ADC)
#define HDA_WIDGET_TYPE_MIXER     0x2   // Audio Mixer
#define HDA_WIDGET_TYPE_SELECTOR  0x3   // Audio Selector
#define HDA_WIDGET_TYPE_PIN       0x4   // Pin Complex
#define HDA_WIDGET_TYPE_POWER     0x5   // Power Widget
#define HDA_WIDGET_TYPE_VOLKNOB   0x6   // Volume Knob Widget
#define HDA_WIDGET_TYPE_BEEP      0x7   // Beep Generator Widget
#define HDA_WIDGET_TYPE_VENDOR    0xF   // Vendor Defined Widget

// AUDIO_WIDGET_CAP flag bits used by the amplifier code
#define HDA_WIDGET_CAP_OUT_AMP    0x00000004  // bit 2: output amp present
#define HDA_WIDGET_CAP_AMP_OVRD   0x00000008  // bit 3: widget overrides the AFG's AMP_CAP

// Pin widget control bits (SET_PIN_WIDGET_CTRL payload)
#define HDA_PINCTL_OUT_ENABLE     0x40
#define HDA_PINCTL_IN_ENABLE      0x20
#define HDA_PINCTL_HPHN_ENABLE    0x80  // headphone amp enable, on pins that support it
#define HDA_PINCTL_VREF_MASK      0x07

// EAPD/BTL enable payload bit
#define HDA_EAPD_BTL_ENABLE       0x02

// PIN_CAP dword bit fields (GET_PARAMETER response)
#define HDA_PINCAP_EAPD           0x00010000  // bit 16: pin supports EAPD

// Power states (SET_POWER_STATE payload / GET_POWER_STATE response)
#define HDA_PWR_D0                0x00
#define HDA_PWR_D3                0x03

// GET_CONFIG_DEFAULT dword decode (pin complex default configuration).
// Mirrors the generic HDA auto-parser's pin-config decode (Linux
// include/sound/hda_verbs.h AC_DEFCFG_* macros) - used at runtime instead
// of any per-board hardcoded pin table, per this project's design (see
// shared.h's HDA_WIDGET comment / HANDOFF.md sigmatel.c findings).
#define HDA_PINCFG_PORT_CONN(cfg)   (((cfg) >> 30) & 0x3)
#define HDA_PINCFG_PORTCONN_NONE    0x1   // "no physical connection" - skip this pin

#define HDA_PINCFG_DEVICE(cfg)      (((cfg) >> 20) & 0xF)
#define HDA_PINCFG_DEVICE_LINE_OUT  0x0
#define HDA_PINCFG_DEVICE_SPEAKER   0x1
#define HDA_PINCFG_DEVICE_HP_OUT    0x2
#define HDA_PINCFG_DEVICE_CD        0x4
#define HDA_PINCFG_DEVICE_LINE_IN   0x8
#define HDA_PINCFG_DEVICE_MIC_IN    0x9
#define HDA_PINCFG_DEVICE_NONE      0xF

// Root/AFG node is always NID 0 for the codec-address-relative "root" query;
// the actual Audio Function Group NID is discovered via SUBORDINATE_NODE_COUNT
// on NID 0, matching the generic HDA auto-parser's approach (same as
// hda_generic.c / hda_auto_parser.c in the Linux reference) - no hardcoded
// NID numbers here, since (per sigmatel.c investigation) this board has no
// special-cased pin/NID table and just uses BIOS-programmed pin configs read
// at runtime via GET_CONFIG_DEFAULT.
#define HDA_NID_ROOT              0x00

#endif // _HDAVERBS_H_
