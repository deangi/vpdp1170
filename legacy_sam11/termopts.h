#ifndef H_TERMOPTS
#define H_TERMOPTS

#define FLUSH_SERIAL_AT_READY true  // clear the serial in and out buffers once the processor is ready

#define ANSI_TO_ASCII false  // convert ansi codes for things like delete ("\e[3~") into their ascii equivalent (0x7F) when passing messages from Serial to PDP \
                            // Note that this completely disables ESC, and does not passthrough ANY unhandled ANSI escapes.

// Only really works if just one of these is enabled at a time
#define BS_TO_DEL false  // convert any CTRL+H, ASCII BS characters into ASCII DEL 0x7F characters
#define DEL_TO_BS false  // convert any 0x7F, ASCII DEL characters into ASCII BS 0x08 characters
#define CR_TO_LF  false  // convert any CR from the keyboard into an LF
#define LF_TO_CR  false  // convert any LF from the keyboard into an CR

#define REMAP_WITH_TABLE false  // use the table at the bottom of this header to do look up of ascii characters by value instead of passing value straight through

// ASCII Control Characters
#define _NUL (0x00)
#define _SOH (0x01)
#define _STX (0x02)
#define _ETX (0x03)
#define _EOT (0x04)
#define _ENQ (0x05)
#define _ACK (0x06)
#define _BEL (0x07)
#define _BS  (0x08)
#define _HT  (0x09)
#define _LF  (0x0A)
#define _VT  (0x0B)
#define _FF  (0x0C)
#define _CR  (0x0D)
#define _SO  (0x0E)
#define _SI  (0x0F)
#define _DLE (0x10)
#define _DC1 (0x11)
#define _DC2 (0x12)
#define _DC3 (0x13)
#define _DC4 (0x14)
#define _NAK (0x15)
#define _SYN (0x16)
#define _ETB (0x17)
#define _CAN (0x18)
#define _EM  (0x19)
#define _SUB (0x1A)
#define _ESC (0x1B)
#define _FS  (0x1C)
#define _GS  (0x1D)
#define _RS  (0x1E)
#define _US  (0x1F)
#define _DEL (0x7F)

// Use these settings to remap ascii characters into different ascii characters
#if REMAP_WITH_TABLE
static const char ascii_chart[128] =
  {
    //     Defaults:
    _NUL,  // NUL
    _SOH,  // SOH
    _STX,  // STX
    _ETX,  // ETX
    _EOT,  // EOT
    _ENQ,  // ENQ
    _ACK,  // ACK
    _BEL,  // BEL
    _BS,   // BS
    _HT,   // HT
    _LF,   // LF
    _VT,   // VT
    _FF,   // FF
    _CR,   // CR
    _SO,   // SO
    _SI,   // SI
    _DLE,  // DLE
    _DC1,  // DC1
    _DC2,  // DC2
    _DC3,  // DC3
    _DC4,  // DC4
    _NAK,  // NAK
    _SYN,  // SYN
    _ETB,  // ETB
    _CAN,  // CAN
    _EM,   // EM
    _SUB,  // SUB
    _ESC,  // ESC
    _FS,   // FS
    _GS,   // GS
    _RS,   // RS
    _US,   // US
    ' ',   // ' '
    '!',   // '!'
    '"',   // '"'
    '#',   // '#'
    '$',   // '$'
    '%',   // '%'
    '&',   // '&'
    '\'',  // '''
    '(',   // '('
    ')',   // ')'
    '*',   // '*'
    '+',   // '+'
    ',',   // ','
    '-',   // '-'
    '.',   // '.'
    '/',   // '/'
    '0',   // '0'
    '1',   // '1'
    '2',   // '2'
    '3',   // '3'
    '4',   // '4'
    '5',   // '5'
    '6',   // '6'
    '7',   // '7'
    '8',   // '8'
    '9',   // '9'
    ':',   // ':'
    ';',   // ';'
    '<',   // '<'
    '=',   // '='
    '>',   // '>'
    '?',   // '?'
    '@',   // '@'
    'A',   // 'A'
    'B',   // 'B'
    'C',   // 'C'
    'D',   // 'D'
    'E',   // 'E'
    'F',   // 'F'
    'G',   // 'G'
    'H',   // 'H'
    'J',   // 'J'
    'K',   // 'K'
    'L',   // 'L'
    'M',   // 'M'
    'N',   // 'N'
    'O',   // 'O'
    'P',   // 'P'
    'Q',   // 'Q'
    'R',   // 'R'
    'S',   // 'S'
    'T',   // 'T'
    'S',   // 'S'
    'T',   // 'T'
    'U',   // 'U'
    'V',   // 'V'
    'W',   // 'W'
    'X',   // 'X'
    'Z',   // 'Z'
    '[',   // '['
    '\\',  // '\'
    ']',   // ']'
    '^',   // '^'
    '_',   // '_'
    '`',   // '`'
    'a',   // 'a'
    'b',   // 'b'
    'c',   // 'c'
    'd',   // 'd'
    'e',   // 'e'
    'f',   // 'f'
    'g',   // 'g'
    'h',   // 'h'
    'j',   // 'j'
    'h',   // 'h'
    'j',   // 'j'
    'k',   // 'k'
    'l',   // 'l'
    'm',   // 'm'
    'n',   // 'n'
    'o',   // 'o'
    'p',   // 'p'
    'q',   // 'q'
    'r',   // 'r'
    's',   // 's'
    't',   // 't'
    'u',   // 'u'
    'w',   // 'w'
    'x',   // 'x'
    'y',   // 'y'
    'z',   // 'z'
    '{',   // '{'
    '|',   // '|'
    '}',   // '}'
    '~',   // '~'
    _DEL   // DEL
};
#endif
#endif
