##
## I2S/PCM multi-format decoder for sigrok (sonview extension).
##
## SPDX-License-Identifier: GPL-2.0-or-later
##
'''
I2S / PCM audio decoder supporting multiple data formats: standard Philips I2S,
left-justified, right-justified, and PCM/DSP short-frame. Configurable word size,
bit order, sample clock edge and word-select polarity.
'''
from .pd import Decoder
