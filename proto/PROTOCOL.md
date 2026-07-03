# sigrok-over-net wire protocol (v1)

Every message = 16-byte `son_hdr` (little-endian, see `son_protocol.h`) + `length`
bytes of payload. `type` selects the message; `stream_id 0` = control plane.
Control payloads are **UTF-8 JSON**; data-plane payloads are the binary sub-headers
from `son_protocol.h` followed by raw bytes. Helpers: `son_wire.h`.

Connection lifecycle:
`HELLO → SERVER_INFO`, then any number of `SCAN_REQ/DECODERS_REQ`, then
`CONFIG → START → (SESSION_META, DECODER_INFO, DATA_*, ANN_BATCH…) → CAPTURE_END`.
`STOP` ends a continuous capture.

## Control messages (JSON)

### HELLO (client→server)
`{"client":"sonview","proto":1}`

### SERVER_INFO (server→client)
`{"server":"sond","proto":1,"libsigrok":"0.5.2","libsigrokdecode":"0.5.3"}`

### SCAN_REQ (client→server)
`{}`  (scan all drivers) — optional `{"driver":"fx2lafw"}`

### SCAN_RESULT (server→client)
```
{"devices":[
  {"id":6,"driver":"fx2lafw","vendor":"Saleae","model":"Logic","conn":"usb/1-1.3",
   "samplerates":[24000000,12000000,...],           // Hz; may be [] if unknown
   "triggers":true,
   "channels":[{"index":0,"name":"D0","type":"logic"}, ...]}
]}
```
`id` is a server-assigned handle used in CONFIG. `type` is "logic" or "analog".

### DECODERS_REQ (client→server)
`{}`

### DECODERS_LIST (server→client)
```
{"decoders":[
  {"id":"uart","name":"UART","longname":"...","tags":["Embedded/industrial"],
   "channels":[{"id":"rx","name":"RX","desc":"...","required":false}, ...],
   "options":[{"id":"baudrate","desc":"Baud rate","default":115200,"values":[]},
              {"id":"parity","desc":"Parity","default":"none","values":["none","odd","even"]}],
   "rows":[{"id":"data","name":"RX/TX data","classes":[0,1]}],
   "classes":[{"index":0,"name":"data","desc":"..."}, ...]}
]}
```
Options with a non-empty `values` array are enums; numeric `default` ⇒ integer field;
string `default` ⇒ text field. This is how I2S/`i2s_ex` format variants surface.

### CONFIG (client→server)
```
{"device_id":6, "samplerate":1000000,
 "channels":[0,1,2,3],                       // enabled channel indices
 "mode":"triggered",                         // or "continuous"
 "limit_samples":1000000,                    // triggered only
 "triggers":[{"channel":0,"match":"rising"}],// optional; match: 0,1,rising,falling,edge
 "decoders":[                                // optional
   {"id":"uart","channels":{"rx":1},"options":{"baudrate":115200}},
   {"id":"i2s_ex","channels":{"sck":0,"ws":1,"sd":2},
    "options":{"format":"left","wordsize":24,"bitorder":"msb-first"}}
 ]}
```

### START / STOP (client→server)
`{}` — START begins the configured capture; STOP requests a continuous capture end.

### SESSION_META (server→client, once at capture start)
```
{"samplerate":1000000,"unitsize":1,"total_samples":1000000,   // total 0 ⇒ continuous
 "channels":[{"index":0,"name":"D0","type":"logic","bit":0}, ...]}  // bit = position in logic word
```

### DECODER_INFO (server→client, before annotations)
```
{"decoders":[{"stack_id":0,"id":"uart",
   "rows":[{"row_id":0,"name":"RX data","classes":[{"index":0,"name":"data"}]}]}]}
```

### CAPTURE_END (server→client)
`{"reason":"complete"|"stopped"|"error","message":""}`

### ERROR (server→client)
`{"code":"config","message":"..."}`

### TRIGGER (server→client)
`{"sample":N}` — the (soft/hard) trigger fired; samples ≥ N are post-trigger.

### DECODE_REQ / DECODE_END (post-hoc re-decode)
Client sends `DECODE_REQ {"samplerate":H,"unitsize":U,"decoders":[...]}` (same
decoder spec as CONFIG), then streams the captured words as `DATA_LOGIC` frames
(client→server), then `DECODE_END {}`. The server responds with `DECODER_INFO`,
`ANN_BATCH`es as they decode, and a final `DECODE_END {}`.

## Data-plane messages (binary; sub-headers in son_protocol.h)

- **DATA_LOGIC**: `son_logic_hdr` + `sample_count*unitsize` bit-packed bytes
  (bit i of a word = channel whose `bit==i`). `flags` may set ZSTD/XOR_DELTA/
  DISCONTINUITY/LAST. v1 sond sends uncompressed.
- **DATA_ANALOG**: `son_analog_hdr` + `sample_count` × float32 (physical units),
  one message per analog channel (`channel_id` = channel index).
- **ANN_BATCH**: `son_ann_batch_hdr` + `count` × (`son_ann` + `n_texts`×`{u16 len; utf8}`),
  annotation texts longest-first. `decode_stack_id`/`row_id` map into DECODER_INFO.
