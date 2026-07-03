##
## I2S/PCM multi-format protocol decoder (sonview extension).
##
## Supports the common serial-audio framings that the stock sigrok 'i2s' decoder
## does not: Philips I2S (1-bit delay), left-justified, right-justified, and
## PCM/DSP short-frame, with configurable word size, bit order, sample edge and
## WS polarity.
##
## This program is free software; you can redistribute it and/or modify it under
## the terms of the GNU General Public License v2+.

import sigrokdecode as srd


class Decoder(srd.Decoder):
    api_version = 3
    id = 'i2s_ex'
    name = 'I2S/PCM'
    longname = 'I2S / PCM / justified audio (multi-format)'
    desc = 'Serial audio (I2S, left/right-justified, PCM/DSP) with format options.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    tags = ['Audio', 'PC']
    channels = (
        {'id': 'sck', 'name': 'SCK', 'desc': 'Bit (serial) clock'},
        {'id': 'ws',  'name': 'WS',  'desc': 'Word select / frame sync'},
        {'id': 'sd',  'name': 'SD',  'desc': 'Serial data'},
    )
    options = (
        {'id': 'format', 'desc': 'Data format', 'default': 'i2s',
         'values': ('i2s', 'left', 'right', 'pcm')},
        {'id': 'wordsize', 'desc': 'Word size in bits (0 = auto)', 'default': 0},
        {'id': 'bitorder', 'desc': 'Bit order', 'default': 'msb-first',
         'values': ('msb-first', 'lsb-first')},
        {'id': 'sample_edge', 'desc': 'Sample SD on clock edge', 'default': 'rising',
         'values': ('rising', 'falling')},
        {'id': 'wspol', 'desc': 'WS level for the left/first channel',
         'default': 'left-low', 'values': ('left-low', 'left-high')},
    )
    annotations = (
        ('left',  'Left channel'),
        ('right', 'Right channel'),
        ('slot',  'PCM slot'),
        ('warn',  'Warning'),
    )
    annotation_rows = (
        ('audio', 'Audio', (0, 1, 2)),
        ('warnings', 'Warnings', (3,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def _assemble(self, bits, msb_first):
        seq = bits if msb_first else list(reversed(bits))
        val = 0
        for b in seq:
            val = (val << 1) | (b & 1)
        return val

    def _emit_word(self, ss, es, ch, bits, msb_first):
        if not bits:
            return
        val = self._assemble(bits, msb_first)
        n = len(bits)
        if ch == 0:
            cls, tag = 0, 'L'
        elif ch == 1:
            cls, tag = 1, 'R'
        else:
            cls, tag = 2, 'S%d' % ch
        self.put(ss, es, self.out_ann,
                 [cls, ['%s: 0x%X (%d bits)' % (tag, val, n),
                        '%s: 0x%X' % (tag, val), tag]])

    def decode(self):
        fmt = self.options['format']
        wordsize = int(self.options['wordsize'])
        msb_first = self.options['bitorder'] == 'msb-first'
        clk_edge = 'r' if self.options['sample_edge'] == 'rising' else 'f'
        ws_left = 0 if self.options['wspol'] == 'left-low' else 1

        if fmt == 'pcm':
            self._decode_pcm(wordsize or 16, msb_first, clk_edge)
            return

        # WS-framed formats: each interval between WS edges is one channel slot.
        pins = self.wait({1: 'e'})     # sync to a WS edge
        slot_ss = self.samplenum
        ws_level = pins[1]             # WS level at the edge = the new slot's level
        bits = []
        first_bit_ss = None

        while True:
            pins = self.wait([{0: clk_edge}, {1: 'e'}])
            if self.matched[1]:
                # Slot boundary reached: interpret the collected bits.
                self._emit_slot(fmt, wordsize, msb_first, ws_left,
                                slot_ss, self.samplenum, ws_level, bits, first_bit_ss)
                slot_ss = self.samplenum
                ws_level = pins[1]
                bits = []
                first_bit_ss = None
            else:
                if first_bit_ss is None:
                    first_bit_ss = self.samplenum
                bits.append(pins[2])

    def _emit_slot(self, fmt, wordsize, msb_first, ws_left,
                   ss, es, ws_level, bits, first_bit_ss):
        ch = 0 if ws_level == ws_left else 1
        data = bits
        if fmt == 'i2s':
            data = bits[1:]                 # 1-bit delay after WS edge
        elif fmt == 'right':
            data = bits[-wordsize:] if wordsize > 0 else bits
        # 'left' uses bits as-is (data aligned to the WS edge).
        if wordsize > 0 and fmt != 'right':
            data = data[:wordsize]
        self._emit_word(first_bit_ss if first_bit_ss is not None else ss, es,
                        ch, data, msb_first)

    def _decode_pcm(self, wordsize, msb_first, clk_edge):
        # DSP/PCM short frame: frame-sync rising marks the frame; words follow.
        self.wait({1: 'r'})
        slot = 0
        bits = []
        ss = self.samplenum
        while True:
            pins = self.wait([{0: clk_edge}, {1: 'r'}])
            if self.matched[1]:
                if bits:
                    self._emit_word(ss, self.samplenum, 2 + slot, bits, msb_first)
                slot = 0
                bits = []
                ss = self.samplenum
                continue
            bits.append(pins[2])
            if len(bits) >= wordsize:
                self._emit_word(ss, self.samplenum, 2 + slot, bits, msb_first)
                slot += 1
                bits = []
                ss = self.samplenum
