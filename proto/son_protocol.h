#ifndef SON_PROTOCOL_H
#define SON_PROTOCOL_H
/*
 * sigrok-over-net wire protocol — shared by sond (Pi server) and sonview (client).
 * Header-only, C-compatible, little-endian. Both ends are LE (aarch64 + x86-64).
 * Layout rule: fixed-width fields, largest first, explicit padding, NOT #pragma pack
 * (packed structs cause unaligned-access UB on aarch64). Sizes are asserted below.
 */
#include <stdint.h>
#ifdef __cplusplus
#include <cstddef>
#endif

#define SON_MAGIC          0x4b524753u  /* 'SGRK' little-endian */
#define SON_PROTO_VERSION  1

/* Control plane = stream_id 0, JSON payload. Data plane = binary structs below. */
enum son_msgtype {
    SON_MSG_HELLO        = 1,   /* JSON: client/server hello + caps            */
    SON_MSG_SERVER_INFO  = 2,   /* JSON: lib versions, capabilities            */
    SON_MSG_SCAN_REQ     = 3,   /* JSON: {driver?}                             */
    SON_MSG_SCAN_RESULT  = 4,   /* JSON: [{driver,vendor,model,conn,channels}] */
    SON_MSG_CONFIG       = 5,   /* JSON: samplerate,channels,mode,limit,trigger*/
    SON_MSG_START        = 6,   /* JSON: {}                                    */
    SON_MSG_STOP         = 7,   /* JSON: {}                                    */
    SON_MSG_SESSION_META = 8,   /* JSON: actual samplerate,unitsize,layout,t0  */
    SON_MSG_DATA_LOGIC   = 9,   /* bin:  son_logic_hdr + bit-packed samples    */
    SON_MSG_DATA_ANALOG  = 10,  /* bin:  son_analog_hdr + float32[]            */
    SON_MSG_DECODER_INFO = 11,  /* JSON: decode stack rows/classes/colors      */
    SON_MSG_ANN_BATCH    = 12,  /* bin:  son_ann_batch_hdr + entries           */
    SON_MSG_CAPTURE_END  = 13,  /* JSON: {reason}                              */
    SON_MSG_FLOW         = 14,  /* JSON: {credit_bytes}  (client -> server)    */
    SON_MSG_ERROR        = 15,  /* JSON: {code,message}                        */
    SON_MSG_DECODERS_REQ = 16,  /* JSON: {}                                    */
    SON_MSG_DECODERS_LIST= 17,  /* JSON: decoders + channels + options + rows  */
    SON_MSG_CLIENT_STATE = 18,  /* JSON: markers/macros/renames (in .son files)*/
    SON_MSG_TRIGGER      = 19,  /* JSON: {sample} — the trigger fired here      */
    SON_MSG_DECODE_REQ   = 20,  /* JSON: {samplerate,unitsize,decoders} then    */
                                /*  client streams DATA_LOGIC, ends with:       */
    SON_MSG_DECODE_END   = 21   /* JSON: {} both directions (end of feed / done)*/
};

enum son_flags {
    SON_FLAG_ZSTD          = 1u << 0,  /* payload is zstd-compressed           */
    SON_FLAG_XOR_DELTA     = 1u << 1,  /* logic pre-filtered with XOR-delta     */
    SON_FLAG_DISCONTINUITY = 1u << 2,  /* gap before this chunk (see dropped)   */
    SON_FLAG_LAST          = 1u << 3   /* final chunk of a capture              */
};

/* 16-byte fixed header prefixing every message. */
struct son_hdr {
    uint32_t magic;      /* SON_MAGIC                                          */
    uint8_t  version;    /* SON_PROTO_VERSION                                  */
    uint8_t  type;       /* enum son_msgtype                                   */
    uint16_t flags;      /* enum son_flags                                     */
    uint32_t stream_id;  /* 0 = control plane; else per-capture stream         */
    uint32_t length;     /* payload byte count following this header           */
};

struct son_logic_hdr {   /* precedes SON_MSG_DATA_LOGIC sample bytes           */
    uint64_t start_sample;
    uint32_t sample_count;
    uint32_t samples_dropped; /* gap size before this chunk (0 normally)       */
    uint8_t  unitsize;        /* bytes per sample word (bit-packed logic)      */
    uint8_t  _pad[7];
};

struct son_analog_hdr {  /* precedes SON_MSG_DATA_ANALOG float32[] (one ch)    */
    uint64_t start_sample;
    uint32_t sample_count;
    uint32_t channel_id;
};

struct son_ann_batch_hdr { /* precedes a run of son_ann for one decoder row   */
    uint32_t decode_stack_id;
    uint16_t row_id;
    uint16_t count;
};

struct son_ann {         /* followed by n_texts x {uint16 len; utf8[len]}     */
    uint64_t start_sample;
    uint64_t end_sample;
    uint16_t ann_class;
    uint16_t n_texts;    /* annotation strings, longest first (LOD)           */
    uint8_t  _pad[4];
};

/* Compile-time contract: struct sizes must match on both architectures. */
#ifdef __cplusplus
  #define SON_STATIC_ASSERT(c, m) static_assert(c, m)
#else
  #define SON_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
SON_STATIC_ASSERT(sizeof(struct son_hdr)          == 16, "son_hdr must be 16 bytes");
SON_STATIC_ASSERT(sizeof(struct son_logic_hdr)    == 24, "son_logic_hdr size drift");
SON_STATIC_ASSERT(sizeof(struct son_analog_hdr)   == 16, "son_analog_hdr size drift");
SON_STATIC_ASSERT(sizeof(struct son_ann_batch_hdr)==  8, "son_ann_batch_hdr size drift");
SON_STATIC_ASSERT(sizeof(struct son_ann)          == 24, "son_ann size drift");

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
  #error "sigrok-over-net assumes a little-endian target (aarch64/x86-64)"
#endif

#define SON_DEFAULT_PORT 5620

#endif /* SON_PROTOCOL_H */
